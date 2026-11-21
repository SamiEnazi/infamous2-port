"""Linux "Optimise for this PC" (tools/setup.sh --pgo; tools/pgo_train.ps1's counterpart, the same runs and rules):
profile-guided optimisation (PGO) trained on the player's own PC. A profile comes from running the game, so it never
leaves the PC (tools/make_release_zip_linux.py refuses *.profdata and *.profraw; work/ is never in git): every player
makes their own.
  tools/setup.sh --pgo   1. compiles an instrumented game module (rebuild.py PORT_PGO=gen) -> work/port/build-train
  this script            2. runs it by itself in a window, muted, a scripted controller playing:
                            a. a NEW GAME (always): the title, the opening montage and cutscenes, the tutorial and the
                               fight on the docks (the inputs of tools/fast_combat.steps), ~6 minutes
                            b. the player's newest story save, when there is one: ~2 minutes of looking around,
                               walking, jumping and fighting wherever it is
                         3. merges what the runs counted into one profile (llvm-profdata merge)
  tools/setup.sh --pgo   4. compiles the game with it (PORT_PGO=use:<profile>), the normal way when this script exits
                            non-zero: the player always ends with a working game
    python3 -I tools/pgo_train.py --exe <instrumented exe> --profdata <out .profdata> --llvm-profdata <llvm-profdata>
                                  [--runs opening,save] [--stop-after <s>]

As on Windows, nothing here looks at the screen, and nothing a run needs can be missed:
 - The game is driven from its own log (the menu, the montage, the city-falls cutscene) and its own world frame (the
   save run), never by a screen match. The title is passed by pressing START until the opening montage starts. The
   scripted presses start 17.9 s after the city-falls cutscene opens (its log line, the clock fast_combat.steps uses);
   when that line never comes they start anyway, and a second, looser round of the same inputs follows.
 - The counters are written every 40 s (PS3_PGO_DUMP_AT, patches/port/0007 + 0013: the engine asks the instrumented
   module to write them) and the newest complete write is set aside at once, so a run cut short (a crash, the
   player closing the window) still counts.
 - Every run that reached the game itself is merged, however far it got. One rule stays: a run that never left the
   title screen is NOT merged (all gameplay code would have zero counts, and clang compiles zero-count code as cold:
   that game would be slower than the normal build). "Reached the game" = past the title and 150 s on (the montage
   is 96 s), or the city-falls cutscene seen (new game); the game's own world frame and 30 s on (save).
The player's files are only read: each run plays on its own copy of the saves (work/pgo-setup/runs/<run>/gamedata)
and of the settings file; the sound is off, the player's keyboard, mouse and controllers do nothing in it (PS3_KBM=0,
PS3_PAD_SDL=0), and the window title says what is going on. The shader cache is the one the game uses for play.
The game is started the way tools/play.sh starts it (its environment, keep in step), plus the training's own.
Lines for setup: "  progress: <n>% training", "  player-note: <text>", and on failure "  why: <reason>".
--stop-after <s>: a test; the game is closed after <s> seconds of each run, as a player closing the window would.
"""
import argparse
import glob
import os
import queue
import shutil
import signal
import subprocess
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAME = os.path.join(ROOT, "game")


def say(text):
    print(text, flush=True)


def why(reason):
    say("  why: " + reason)
    sys.exit(1)


ap = argparse.ArgumentParser()
ap.add_argument("--exe", required=True)
ap.add_argument("--profdata", required=True)
ap.add_argument("--llvm-profdata", required=True)
ap.add_argument("--runs", default="opening,save")
ap.add_argument("--stop-after", type=int, default=0)
a = ap.parse_args()
exe, profdata, llvm_prof = os.path.abspath(a.exe), os.path.abspath(a.profdata), a.llvm_profdata
if not os.path.isfile(exe):
    why(f"no training build ({exe})")
if not os.path.isfile(os.path.join(os.path.dirname(exe), "libInfamous2Game.so")):
    why(f"no game module beside the training build ({os.path.dirname(exe)})")
if not os.path.isfile(llvm_prof):
    why(f"no llvm-profdata ({llvm_prof})")
elf = os.path.join(ROOT, "work", "EBOOT.elf")
if not os.path.isfile(elf):
    why("no work/EBOOT.elf")
if not os.environ.get("DISPLAY") and not os.environ.get("WAYLAND_DISPLAY"):
    why("no desktop to open the game's window on (run setup from the desktop, not over SSH)")
work = os.path.dirname(profdata)
runs_dir = os.path.join(work, "runs")
os.makedirs(runs_dir, exist_ok=True)


def remove_tree(path):   # (rmtree removes symlinks inside, never what they point to)
    if os.path.islink(path) or os.path.isfile(path):
        os.remove(path)
    elif os.path.isdir(path):
        shutil.rmtree(path, ignore_errors=True)


def test_profraw(path):
    """a counters file llvm-profdata can read (not empty, not cut off by a kill mid-write)"""
    if not os.path.isfile(path) or os.path.getsize(path) < (1 << 20):
        return False
    return subprocess.run([llvm_prof, "show", path], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0


# ---------------------------------------------------------------- the runs
# Pad scripts: PAD_SCRIPT events "w<s>:<mask>[:<hold s>]", seconds after PAD_SCRIPT_WAIT (anchor.flag) exists.
# Masks: START 0x0008, CROSS 0x4000, L1 0x0400, R1 0x0800; left stick 0x10000 up, right stick 0x100000 up,
# 0x400000 left, 0x800000 right. The same as tools/pgo_train.ps1 (keep in step): the opening's first 21 events are
# tools/fast_combat.steps', then one more round of the same kinds of input, 30 s, for a prompt met too early.
OPENING_PAD = ("w2:0x100000:3,w8:0x4000,w14:0x100000:3,w20:0x10000:4,w24:0x400000:3,w29:0x800000:3,w34:0x100000:2,"
               "w40:0x400:42,w42:0x800,w45:0x800,w48:0x800,w51:0x800,w54:0x800,w57:0x800,w84:0x10000:30,"
               "w116:0x400:24,w119:0x800,w123:0x800,w127:0x800,w131:0x800,w135:0x800,"
               "w142:0x100000:2,w145:0x400000:2,w148:0x10000:8,w157:0x400:12,w159:0x800,w162:0x800,w165:0x800,w168:0x4000")
# The save: from the game's first world frame 40 s or more after the first present (the in-engine cutscene that opens
# a save is a world frame too; the X presses end it): look, walk, jump, aim + fire, walk.
SAVE_PAD = ("w3:0x800000:2,w6:0x10000:8,w15:0x400000:3,w19:0x10000:10,w30:0x4000,w31:0x10000:6,w38:0x800000:4,"
            "w43:0x10000:12,w56:0x400:22,w58:0x800,w61:0x800,w64:0x800,w67:0x800,w70:0x800,w73:0x800,w76:0x800,"
            "w80:0x400000:4,w85:0x10000:12,w98:0x4000")

want = [r.strip() for r in a.runs.split(",") if r.strip()]
installed = bool(glob.glob(os.path.join(GAME, "dev_hdd0", "game", "*_INSTALL")))
# the player's saves (read only): a story save is a save folder with a STATE file
gamedata = os.path.join(GAME, "gamedata")
savedata = os.path.join(gamedata, "dev_hdd0", "home", "00000001", "savedata")
story = [d for d in glob.glob(os.path.join(savedata, "*"))
         if os.path.isfile(os.path.join(d, "STATE")) and os.path.getsize(os.path.join(d, "STATE")) > 0]
plan = []
if "opening" in want:   # (Steam Deck, 2026-11-21: 384 s on a first start too; the HDD data installs during the menus)
    plan.append(dict(tag="opening", est=390 if installed else 420, cap=1200))
if "save" in want:
    if story:
        plan.append(dict(tag="save", est=170, cap=480))
    else:
        say("  no story save yet: training on a new game only")
if not plan:
    why(f"nothing to train on (--runs {a.runs})")
est_all = sum(r["est"] for r in plan)
say(f"  player-note: The game now opens by itself for about {-(-est_all // 60)} minutes{' (twice)' if len(plan) > 1 else ''} "
    "to learn how your PC runs it. The sound is off. Please don't touch it or close it.")
if not installed:
    say("  the game's HDD data is not installed yet: the first run installs it (a few minutes more)")

# The settings the player plays with (a copy per run; what a run writes stays in it): the caller's PS3_OVERLAY_INI,
# else the port folder's settings.ini, else the one tools/play.sh copies in at the first start (~/.config's).
player_ini = os.environ.get("PS3_OVERLAY_INI") or os.path.join(ROOT, "settings.ini")
if not os.environ.get("PS3_OVERLAY_INI") and not os.path.isfile(player_ini) \
        and not os.path.isfile(os.path.join(ROOT, "work", "settings-moved-in.txt")):
    player_ini = os.path.join(os.environ.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.config"), "infamous2-port", "settings.ini")

# The game's environment as tools/play.sh sets it (keep in step).
flash = os.path.join(GAME, "dev_flash")
if not os.path.isdir(os.path.join(flash, "sys")):
    flash = os.environ.get("PORT_DEV_FLASH") or os.path.join(os.environ.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.config"), "rpcs3", "dev_flash")
defaults = ("PS3RECOMP_FPS_CAP=60;PS3_AUDIO_VOLUME=100;PS3RECOMP_RENDER_SCALE=1.5;PS3RECOMP_SOFT_SHADOWS=2;PS3RECOMP_SSAO=2;"
            "PS3RECOMP_SSAO_STRENGTH=125;PS3RECOMP_LIGHT_DITHER=1;PS3RECOMP_FX_RES=full;PS3RECOMP_SHARPEN=0.1")
try:
    valve = open("/sys/devices/virtual/dmi/id/board_vendor").read().strip() == "Valve"
except OSError:
    valve = False
if valve:   # the Steam Deck (play.sh): the PS3's 720p, fullscreen
    defaults = defaults.replace("PS3RECOMP_RENDER_SCALE=1.5", "PS3RECOMP_RENDER_SCALE=1.0") + ";PS3RECOMP_VK_FULLSCREEN=1"
PLAY_ENV = {
    "PS3_VFS_ROOT": os.path.join(GAME, "disc"), "PS3_HDD0_ROOT": os.path.join(GAME, "dev_hdd0"), "PS3_DEV_FLASH": flash,
    "GCM_SEMA_ACQUIRE": "1", "GCM_SEMA_ACQUIRE_MS": "200", "GCM_PARK_GRACE_MS": "1000",
    "PS3RECOMP_VK_WINDOW": "1", "PS3RECOMP_VK_GUEST_PROGRAMS": "1", "PS3_VERBOSE": "0",
    "PS3_OVERLAY_DEFAULTS": defaults,
    "SDL_GAMECONTROLLER_USE_BUTTON_LABELS": "0", "SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS": "1",
    "PS3RECOMP_ENG_GPU_LIGHT": "07000B80:07384B80", "PS3RECOMP_ENG_GPU_SSAO": "0F800000",
}
# play.sh's tuning defaults (each can be set from outside, as there)
PLAY_DEFAULTS = {
    "PS3RECOMP_GCM_SPIN_US": "0", "PS3RECOMP_VK_TQ_SPIN_US": "0", "PS3RECOMP_VK_PIPE_THREADS": "1", "PS3_FRAMECLOCK_US": "250",
    "PS3_NATIVE_JOBS_THREADS": "4", "PS3_PAD_THREAD_US": "4000", "PS3_NATIVE_JOBS_PRIO": "1", "PS3RECOMP_ENG_R8": "3",
    "PS3RECOMP_VK_THREADED": "1", "PS3_CPU_PLACE_VKW": "1", "PS3_CPU_PLACE": "rsxcore", "PS3_STATIC_TLS_RETRY": "1",
    "PS3RECOMP_VK_SHADER_CACHE": os.path.join(ROOT, "work", "shader_cache"),   # the cache play uses (warmed)
}
base_env = dict(os.environ)
for k, v in PLAY_DEFAULTS.items():
    base_env.setdefault(k, v)
base_env.update(PLAY_ENV)
if "glibc.rtld.optional_static_tls=" not in base_env.get("GLIBC_TUNABLES", ""):
    base_env["GLIBC_TUNABLES"] = ":".join(x for x in (base_env.get("GLIBC_TUNABLES"), "glibc.rtld.optional_static_tls=1048576") if x)
if base_env.get("WAYLAND_DISPLAY") and not base_env.get("SDL_VIDEODRIVER"):
    base_env["SDL_VIDEODRIVER"] = "x11"   # (play.sh: KDE Plasma's Wayland surface; XWayland works)
for k in ("PS3RECOMP_PLAYLOG", "PS3_LAUNCHER_RESTART", "PS3_INPUT_MAP", "PS3_GAME_MODS_DIR"):
    base_env.pop(k, None)


def pump(stream, path, q, t0):
    """the game's stdout/stderr: to a file, each line with the ms since the start, and into the queue the loop reads"""
    with open(path, "w", encoding="utf-8") as w:
        for raw in iter(stream.readline, b""):
            ms = (time.monotonic() - t0) * 1000.0
            text = raw.decode("utf-8", "replace").rstrip("\r\n")
            w.write("%d\t%s\n" % (ms, text))
            w.flush()
            q.put((ms, text))


# Setup stopped (the installer's Cancel, a closed terminal): the game goes too (it runs in its own session)
current = None


def stop_game(signum, frame):
    if current is not None and current.poll() is None:
        try:
            os.killpg(current.pid, signal.SIGKILL)
        except OSError:
            pass
    why("setup was stopped")


for s_ in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
    signal.signal(s_, stop_game)

good, notes, done, last_pct = [], [], 0.0, -1
prog_t0 = time.monotonic()
for r in plan:
    tag, is_save = r["tag"], r["tag"] == "save"
    rd = os.path.join(runs_dir, tag)
    remove_tree(rd)
    gd = os.path.join(rd, "gamedata")
    sv = os.path.join(gd, "dev_hdd0", "home", "00000001", "savedata")
    if is_save:   # the player's saves, copied (the game loads the newest by itself at boot)
        try:
            shutil.copytree(gamedata, gd, symlinks=True)
        except OSError as e:
            notes.append("save: could not copy the saves")
            say(f"  run save: not used (could not copy the saves: {e})")
            done += r["est"]
            continue
    os.makedirs(sv, exist_ok=True)
    ini = os.path.join(rd, "settings.ini")
    if os.path.isfile(player_ini):
        shutil.copyfile(player_ini, ini)
    raw, last = os.path.join(rd, "counters.profraw"), os.path.join(rd, "counters-last.profraw")
    anchor, world, pad = os.path.join(rd, "anchor.flag"), os.path.join(rd, "world.flag"), os.path.join(rd, "pad.txt")
    env = dict(base_env)
    env.update({
        "PS3_GAMEDATA_ROOT": gd,
        "PS3_OVERLAY_INI": ini, "PS3_CHEATS_SPOTS": os.path.join(rd, "spots.ini"), "PS3_GAME_MODS_STATE": os.path.join(rd, "mods.ini"),
        # the training's own
        "PS3_TITLE": "inFAMOUS 2 - Setup is optimising the game for this PC: it plays by itself for a few minutes, sound off. Please don't close this window.",
        "PS3_LANG": "en", "PS3_VOICE_LANG": "same", "PS3_TEXT_LANG": "same",   # the same movies and timings on every copy
        "PS3_AUDIO_VOLUME": "0", "PS3RECOMP_VK_FULLSCREEN": "0", "PS3RECOMP_FPS_CAP": "0",   # muted, a window, uncapped
        "PS3_KBM": "0", "PS3_PAD_SDL": "0",   # the player's keyboard, mouse and controllers do nothing in it
        "LLVM_PROFILE_FILE": raw, "PS3_PGO_DUMP_AT": ",".join(str(t) for t in range(40, r["cap"], 40)),
        "PAD_FILE": pad, "PAD_SCRIPT": SAVE_PAD if is_save else OPENING_PAD,
        "PAD_SCRIPT_WAIT": world if is_save else anchor,
        "PS3RECOMP_ENG_WORLD_MARK": world,   # the game writes it on its first world frame
    })
    if is_save:
        env["PS3RECOMP_ENG_WORLD_MARK_AFTER"] = "40"
    say(f"  run {tag}: started {time.strftime('%H:%M:%S')}")
    t0 = time.monotonic()
    p = subprocess.Popen([exe, elf], cwd=rd, env=env, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE, start_new_session=True)
    current = p
    q = queue.Queue()
    pumps = [threading.Thread(target=pump, args=(p.stderr, os.path.join(rd, "stderr.log"), q, t0), daemon=True),
             threading.Thread(target=pump, args=(p.stdout, os.path.join(rd, "stdout.log"), q, t0), daemon=True)]
    for th in pumps:
        th.start()

    ev = {}   # first time (ms) of each log event
    writes, closed_by, end_at, anchor_at, start_at, next_start, start_until = 0, "", -1.0, -1.0, -1.0, 0.0, -1.0
    while p.poll() is None:
        time.sleep(0.25)
        now = (time.monotonic() - t0) * 1000.0
        while True:
            try:
                ms, text = q.get_nowait()
            except queue.Empty:
                break
            if "[pgo] profile written at " in text and "(rc=0)" in text:
                # a complete write: set it aside at once (the next one may be cut off by the end of the run)
                try:
                    os.replace(raw, last)
                    writes += 1
                except OSError:
                    pass
            elif "menu" not in ev and "load_menu_catalog" in text:
                ev["menu"] = ms
            elif "montage" not in ev and "intro_montage" in text:
                ev["montage"] = ms
            elif "story" not in ev and "h_jbi_cityfalls" in text:
                ev["story"] = ms
            elif "world" not in ev and "[rsx engine] world mark" in text:
                ev["world"] = ms
        if is_save:
            if "world" in ev and end_at < 0:
                end_at = ev["world"] + 110000   # 105 s of moves, then close
            if "world" not in ev and now > 300000 and end_at < 0:
                closed_by, end_at = "no world frame in 300 s", now
        else:
            # the title: START every 1.5 s from the menu (or from 600 s: a first start installing the HDD data may say
            # nothing for minutes) until the montage starts; never once it did (START would pause the game)
            passed = "montage" in ev or "story" in ev
            if start_at < 0 and ("menu" in ev or now > 600000):
                start_at, start_until = now, now + 300000
            if start_at >= 0 and not passed and now < start_until and now >= next_start:
                try:
                    with open(pad + ".tmp", "w") as f:
                        f.write("0x0008 40")
                    os.replace(pad + ".tmp", pad)
                except OSError:
                    pass
                next_start = now + 1500
            if start_at >= 0 and not passed and now >= start_until and end_at < 0:
                closed_by, end_at = "the title was not passed", now
            # the scripted presses: 17.9 s after the city-falls cutscene opened; without it, 300 s after the montage
            if anchor_at < 0:
                if "story" in ev:
                    anchor_at = ev["story"] + 17900
                elif "montage" in ev and now > ev["montage"] + 300000:
                    anchor_at = now
            if anchor_at >= 0 and now >= anchor_at and not os.path.exists(anchor):
                with open(anchor, "w") as f:
                    f.write(str(int(now)))
                end_at = anchor_at + 175000   # the last press is at 168 s
        if a.stop_after > 0 and now >= a.stop_after * 1000 and not closed_by:
            closed_by, end_at = f"test: closed after {a.stop_after} s", now
        pct = int(min(99, 100 * (done + min(time.monotonic() - t0, r["est"])) / est_all))
        if pct != last_pct:
            say(f"  progress: {pct}% training")
            last_pct = pct
        if (end_at >= 0 and now >= end_at) or now >= r["cap"] * 1000:
            if now >= r["cap"] * 1000 and not closed_by:
                closed_by = f"still going after {r['cap']} s"
            # a clean exit (SIGTERM: SDL turns it into the window's quit) runs the profile writer once more at exit;
            # kill only as the last resort
            try:
                p.send_signal(signal.SIGTERM)
                p.wait(20)
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(p.pid, signal.SIGKILL)
                except OSError:
                    pass
            if not closed_by:
                closed_by = "done"
            break
    try:
        p.wait(30)
    except subprocess.TimeoutExpired:
        os.killpg(p.pid, signal.SIGKILL)
        p.wait()
    end = (time.monotonic() - t0) * 1000.0
    for th in pumps:
        th.join(10)
    while not q.empty():   # lines read after the loop (a write at exit, an event in the last 250 ms)
        ms, text = q.get_nowait()
        for k, needle in (("menu", "load_menu_catalog"), ("montage", "intro_montage"), ("story", "h_jbi_cityfalls"),
                          ("world", "[rsx engine] world mark")):
            if k not in ev and needle in text:
                ev[k] = ms
    if not closed_by:
        closed_by = f"the game closed by itself after {int(end / 1000)} s (exit code {p.returncode})"
    done += r["est"]

    # did it reach the game itself? (see the top: a title-only profile would slow the game down)
    reason = ""
    if is_save:
        if "world" not in ev:
            reason = "the save did not load into the world"
        elif end - ev["world"] < 30000:
            reason = f"it played only {int((end - ev['world']) / 1000)} s in the world"
    else:
        if "montage" not in ev and "story" not in ev:
            reason = "it did not get past the title screen"
        elif "story" not in ev and end - ev["montage"] < 150000:
            reason = f"it stopped {int((end - ev['montage']) / 1000)} s after the title, before the game's story"
    pick = None
    if not reason:
        pick = next((f for f in (raw, last) if test_profraw(f)), None)
        if not pick:
            reason = f"no readable counters ({writes} complete write(s) seen)"
    seen = ", ".join(f"{k} {int(ev[k] / 1000)} s" for k in ("menu", "montage", "story", "world") if k in ev)
    say(f"  run {tag}: {closed_by} at {int(end / 1000)} s ({seen or 'no game events'})")
    if pick:
        good.append(pick)
        say(f"  run {tag}: used, counters {os.path.getsize(pick) / 1048576:.0f} MB{' (the last complete write)' if pick == last else ''}")
    else:
        if closed_by.startswith("the game closed by itself") and reason:
            reason += " (the game window was closed or the game stopped)"
        notes.append(reason)
        say(f"  run {tag}: not used ({reason})")
    remove_tree(gd)   # the save copy
say("  progress: 100% training")
say(f"  training took {int(time.monotonic() - prog_t0)} s")

if not good:
    why(notes[0] if notes else "no training run finished")
if os.path.exists(profdata):
    os.remove(profdata)
m = subprocess.run([llvm_prof, "merge", "--failure-mode=all", "-o", profdata] + good,
                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
for ln in m.stdout.splitlines():
    say("    " + ln)
for r in plan:
    for f in glob.glob(os.path.join(runs_dir, r["tag"], "*.profraw")):
        os.remove(f)
if m.returncode or not test_profraw(profdata):
    if os.path.exists(profdata):
        os.remove(profdata)
    why(f"merging the counters failed (llvm-profdata exit {m.returncode})")
say(f"  profile: {profdata} ({os.path.getsize(profdata) / 1048576:.0f} MB, from {len(good)} run(s))")
sys.exit(0)
