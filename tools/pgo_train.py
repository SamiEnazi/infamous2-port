"""Linux "Optimise for this PC" (tools/setup.sh --pgo; tools/pgo_train.ps1's counterpart, the same runs and rules):
profile-guided optimisation (PGO) trained on the player's own PC. A profile comes from running the game, so it never
leaves the PC (tools/make_release_zip_linux.py refuses *.profdata and *.profraw; work/ is never in git): every player
makes their own.
  tools/setup.sh --pgo   1. compiles an instrumented game module (rebuild.py PORT_PGO=gen) -> work/port/build-train
  this script            2. runs it by itself in a window, muted, a scripted controller playing:
                            a. a NEW GAME (setup's default, the only run: the same for every player and region): the
                               title, the opening montage and cutscenes, then the tutorial on the pier played the way it
                               asks (the Player and OPENING_SECTIONS below: look around, run to the gate, blast its lock,
                               on down the pier, shoot at the disturbance, the Beast), ~7 minutes
                            b. (--runs opening,save, as on Windows) the player's newest story save, when there is one:
                               ~2 minutes of looking around, walking, jumping and fighting wherever it is
                            c. (optional, --runs street,plaza) a training save of the busy street / the plaza, shipped
                               in --scene-dir (<dir>/<run>/gamedata), and a recorded walk there (<dir>/<run>.pad), ~3
                               minutes each: the benchmark scenes. Skipped (said why) when the save is not there or is
                               for another version of the game (another title ID: that game does not list it).
                         3. merges what the runs counted into one profile (llvm-profdata merge)
  tools/setup.sh --pgo   4. compiles the game with it (PORT_PGO=use:<profile>), the normal way when this script exits
                            non-zero: the player always ends with a working game
  On Linux the engine is instrumented too (--engine-profdata: setup.sh builds it with PORT_ENGINE_PGO=gen): the engine
  writes its own counters (PS3_PGO_ENGINE_FILE, patches/port/0014) in the same runs, merged by the same rule into a
  second profile. An engine profile that fails is only left out (the engine is then compiled the normal way).
    python3 -I tools/pgo_train.py --exe <instrumented exe> --profdata <out .profdata> --llvm-profdata <llvm-profdata>
                                  [--engine-profdata <out .profdata>] [--runs opening,save] [--scene-dir <dir>]
                                  [--stop-after <s>]

As on Windows, nothing here looks at the screen, and nothing a run needs can be missed:
 - The game is driven from its own log (the menu, the montage, the cutscenes' subtitle files) and its own world frame
   (the save run), never by a screen match. The title is passed by pressing START until the opening montage starts.
   The tutorial's player starts 17.9 s after the city-falls cutscene opens (its log line, the clock fast_combat.steps
   uses) and plays its sections on PAD_FILE (Windows' pgo_train.ps1 keeps the fixed PAD_SCRIPT of fast_combat.steps);
   when that line never comes it starts anyway.
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
ap.add_argument("--engine-profdata", default="")
ap.add_argument("--runs", default="opening,save")
ap.add_argument("--scene-dir", default=os.path.join(ROOT, "tools", "pgo_scenes"))
ap.add_argument("--stop-after", type=int, default=0)
a = ap.parse_args()
exe, profdata, llvm_prof = os.path.abspath(a.exe), os.path.abspath(a.profdata), a.llvm_profdata
eprofdata = os.path.abspath(a.engine_profdata) if a.engine_profdata else ""
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


def test_profraw(path, least=1 << 20):
    """a counters file llvm-profdata can read (not empty, not cut off by a kill mid-write)"""
    if not os.path.isfile(path) or os.path.getsize(path) < least:
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

# The benchmark scenes (--runs street,plaza): a training save (<scene dir>/<run>/gamedata, the game loads its newest slot
# at boot) and a recorded walk (<scene dir>/<run>.pad, PS3_PAD_REPLAY) from the first world frame <after> s or more after
# the boot, on the frame-stepped guest clock (PS3_TB_FRAMESTEP_US: the same path at any frame rate); closed <close> s
# after that frame. street: the busy street toward the cathedral (route 84 s), plaza: the umbrella plaza (walk, then stand).
SCENES = {"street": dict(after=12, close=110, est=175), "plaza": dict(after=20, close=125, est=190)}


def sfo_title(path):
    """TITLE_ID of a PARAM.SFO (None when it cannot be read)"""
    import struct
    try:
        b = open(path, "rb").read()
        keys, data, n = struct.unpack_from("<III", b, 8)
        for i in range(n):
            ko, fmt, ln, mx, off = struct.unpack_from("<HHIII", b, 20 + 16 * i)
            if b[keys + ko:b.index(b"\0", keys + ko)] == b"TITLE_ID":
                return b[data + off:data + off + ln].rstrip(b"\0").decode("ascii", "replace")
    except (OSError, ValueError, struct.error):
        return None
    return None


want = [r.strip() for r in a.runs.split(",") if r.strip()]
for r_ in want:
    if r_ not in ("opening", "save") and r_ not in SCENES:
        why(f"unknown training run {r_!r} (opening, save, {', '.join(SCENES)})")
my_title = sfo_title(os.path.join(GAME, "disc", "PS3_GAME", "PARAM.SFO"))
installed = bool(glob.glob(os.path.join(GAME, "dev_hdd0", "game", "*_INSTALL")))
# the player's saves (read only): a story save is a save folder with a STATE file
gamedata = os.path.join(GAME, "gamedata")
savedata = os.path.join(gamedata, "dev_hdd0", "home", "00000001", "savedata")
story = [d for d in glob.glob(os.path.join(savedata, "*"))
         if os.path.isfile(os.path.join(d, "STATE")) and os.path.getsize(os.path.join(d, "STATE")) > 0]
plan = []
if "opening" in want:   # (Steam Deck, 2026-11-21: 384 s on a first start too; the HDD data installs during the menus)
    plan.append(dict(tag="opening", est=470 if installed else 500, cap=1200))
if "save" in want:
    if story:
        plan.append(dict(tag="save", est=170, cap=480))
    else:
        say("  no story save yet: training on a new game only")
for tag in [r_ for r_ in want if r_ in SCENES]:
    sg = os.path.join(a.scene_dir, tag, "gamedata")
    route = os.path.join(a.scene_dir, tag + ".pad")
    slots = glob.glob(os.path.join(sg, "dev_hdd0", "home", "00000001", "savedata", "*", "STATE"))
    if not slots or not os.path.isfile(route):
        say(f"  run {tag}: skipped (no training save and route in {a.scene_dir}: {tag}/gamedata, {tag}.pad)")
        continue
    st = {os.path.basename(os.path.dirname(f)).split("_")[0] for f in slots}
    if my_title and my_title not in st:
        say(f"  run {tag}: skipped (the training save is for {', '.join(sorted(st))}; your game is {my_title}, which does not list it)")
        continue
    plan.append(dict(tag=tag, est=SCENES[tag]["est"], cap=480, gamedata=sg, route=route))
if not plan:
    why(f"nothing to train on (--runs {a.runs})")
est_all = sum(r["est"] for r in plan)
times = {1: "", 2: " (twice)", 3: " (three times)"}.get(len(plan), f" ({len(plan)} times)")
say(f"  player-note: The game now opens by itself for about {-(-est_all // 60)} minutes{times} "
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
    "PS3_PAD_THREAD_US": "4000", "PS3_NATIVE_JOBS_PRIO": "1", "PS3RECOMP_ENG_R8": "3",
    "PS3RECOMP_VK_THREADED": "1", "PS3_CPU_PLACE_VKW": "1", "PS3_CPU_PLACE": "rsxcore", "PS3_STATIC_TLS_RETRY": "1",
    "PS3RECOMP_VK_SHADER_CACHE": os.path.join(ROOT, "work", "shader_cache"),   # the cache play uses (warmed)
}
if valve:   # 4 job threads on a Steam Deck only (play.sh); elsewhere the engine's own rule (physical cores - 2, at most 8)
    PLAY_DEFAULTS["PS3_NATIVE_JOBS_THREADS"] = "4"
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


class Player:
    """The opening's player, after the anchor: sections of moves played on PAD_FILE (closed loop: the game reads the file
    on its own pad polls, "<mask> <polls>"), each section started by the game's own log (an event already seen by the
    run loop) or by a time after the anchor. A held move is the same short command written again every tick while it
    lasts, so it lasts that many seconds whatever the frame rate. Moves:
      ("hold", mask, s)      hold buttons / sticks for s seconds       ("tap", mask)   one short press
      ("pause", s)           nothing for s seconds                     ("aim", s, every)  hold L1, R1 every <every> s
      ("press", mask, polls) one press of exactly <polls> pad reads (a small camera nudge)
      ("keep", mask)         keep these buttons held through everything after it (L1 while aiming); 0 = release
      ("say", text)          a line in the log (what the player does next)"""
    L1, R1, CROSS, SQUARE, TRIANGLE, CIRCLE = 0x400, 0x800, 0x4000, 0x8000, 0x1000, 0x2000
    LS_UP, LS_DOWN, LS_LEFT, LS_RIGHT = 0x10000, 0x20000, 0x40000, 0x80000
    RS_UP, RS_DOWN, RS_LEFT, RS_RIGHT = 0x100000, 0x200000, 0x400000, 0x800000

    def __init__(self, pad, sections, log):
        self.pad, self.sections, self.log = pad, list(sections), log
        self.sec, self.moves, self.move, self.move_t0, self.last_tap = None, [], None, 0.0, -1e9
        self.keep = 0
        self.done = False

    def write(self, mask, polls):
        try:
            with open(self.pad + ".tmp", "w") as f:
                f.write("%#x %d" % (mask, polls))
            os.replace(self.pad + ".tmp", self.pad)
        except OSError:
            pass

    def step(self, now, anchor_ms, ev):
        """now, anchor_ms: ms since the run's start; ev: first time (ms) of each log event seen"""
        if self.done or anchor_ms < 0 or now < anchor_ms:
            return
        t = (now - anchor_ms) / 1000.0
        if self.move is None and not self.moves:
            if not self.sections:
                self.done, self.done_t = True, t
                return
            name, start, moves = self.sections[0]
            # start: a number = that many s after the previous section ended (the first: after the anchor);
            # (event, after, timeout) = <after> s after that log event, or <timeout> s after the previous section ended
            if isinstance(start, (int, float)):
                ok = t >= self.sec_end + start
            else:
                key, after, tmo = start
                ok = (key in ev and now >= ev[key] + after * 1000) or t >= self.sec_end + tmo
            if not ok:
                return
            self.sections.pop(0)
            self.sec, self.moves = name, list(moves)
            self.log(f"  player: {name} (anchor + {t:.0f} s{', after ' + start[0] if not isinstance(start, (int, float)) and start[0] in ev else ''})")
        if self.move is None:
            self.move, self.move_t0 = self.moves.pop(0), t
            if self.move[0] in ("say", "keep"):
                if self.move[0] == "say":
                    self.log("  player: " + self.move[1])
                else:
                    self.keep = self.move[1]
                self.move = None
                if not self.moves:
                    self.sec_end = t
                return
        kind, el = self.move[0], t - self.move_t0
        self._step_move(kind, el, t)
        if self.move is None and not self.moves:
            self.sec_end = t   # this section is over: the next one's start counts from here

    def _step_move(self, kind, el, t):
        if kind == "hold":
            if el < self.move[2]:
                self.write(self.move[1] | self.keep, 8)
            else:
                self.move = None
        elif kind == "tap":
            self.write(self.move[1] | self.keep, 4)
            self.move = ("pause", 0.35)
            self.move_t0 = t
        elif kind == "press":
            self.write(self.move[1] | self.keep, self.move[2])
            self.move = ("pause", 0.1 + self.move[2] / 30.0)
            self.move_t0 = t
        elif kind == "pause":
            if el >= self.move[1]:
                self.move = None
            elif self.keep:
                self.write(self.keep, 8)
        elif kind == "aim":   # L1 held; R1 (with L1) every <every> s, the first after 0.6 s
            dur, every = self.move[1], self.move[2]
            if el >= dur:
                self.move = None
            elif el >= 0.6 and el - self.last_tap >= every:
                self.write(self.L1 | self.R1, 4)
                self.last_tap = el
            else:
                self.write(self.L1, 8)
            if self.move is None:
                self.last_tap = -1e9

    sec_end = 0.0
    done_t = 0.0


P = Player
# look up, a little back down, left, back right (left and right as long as each other, so the camera ends where the
# game put it: along the pier). The "look up" prompt wants the camera to come up a good way: 0.5 s up then 0.5 s down
# did not satisfy it (the game stayed stopped at the prompt), 0.7 s up then 0.5 s down did.
LOOK = [("hold", P.RS_UP, 1.2), ("pause", 0.8), ("hold", P.RS_DOWN, 0.7), ("pause", 0.8),
        ("hold", P.RS_LEFT, 1.0), ("pause", 0.8), ("hold", P.RS_RIGHT, 1.0), ("pause", 1.0)]


def aim_fire(shots, every=1.0):
    return [("aim", 0.6 + shots * every, every)]


def lock_search(steps=5, polls=3):
    """L1 held: a bolt at the middle, then at small camera turns outward, left and right in turn (-1, +1, -2, +2, ...
    steps of <polls> pad reads each), so the likeliest places come first; back to the middle at the end"""
    moves, at = [("tap", P.R1), ("pause", 0.3)], 0
    for k in range(1, steps + 1):
        for want in (-k, k):
            d = want - at
            moves += [("press", P.RS_LEFT if d < 0 else P.RS_RIGHT, polls * abs(d)), ("pause", 0.15), ("tap", P.R1), ("pause", 0.3)]
            at = want
    return moves + [("press", P.RS_LEFT, polls * at), ("pause", 0.2)]


# The new game after the city-falls cutscene, as a player would follow the tutorial (watched on a Steam Deck,
# 2026-11-21: the prompts and objectives named below are the game's own words). The game's log gives two marks: the
# anchor (the city-falls cutscene + 17.9 s) and "sub2" (the next cutscene's subtitle: Zeke on the pier); after that
# there is nothing to see in the log, so the rest is timed from the end of the previous section, with moves that are
# right whatever the frame rate (Cole runs until something stops him; the camera ends where it started).
# Each section: (what it is, its start, its moves); a start ("sub2", after, timeout) = <after> s after that cutscene
# began, else <timeout> s after the anchor; a number = that many s after the previous section ended.
OPENING_SECTIONS = [
    # "Use the right stick to look up" / "... look left" / "... right": the game stops at each until it is done
    ("the tutorial: look around with the camera, as it asks (up, left, right)", ("sub2", 3, 25), LOOK * 3),
    # "Head towards the disturbance" (Kuo calls): down the pier to the EMPIRE CITY arch; a step to the left off the
    # railing the run may end against, then on to the gate, until Cole stands at it (the camera turns in behind him)
    ("head towards the disturbance: run down the pier to the gate", 0.5,
     [("hold", P.LS_UP, 9), ("pause", 0.3), ("hold", P.LS_LEFT, 0.6), ("hold", P.LS_UP, 8), ("pause", 0.6)]),
    # "Blast open the gate" ("Hold L1 to aim", "Press R1 to fire lightning bolts"): the gate opens when a bolt hits its
    # chain lock, in the middle of the two halves. Cole stops at the gate a little to one side or the other, so: a step
    # back, aim, and look along the gate for the lock, a bolt at each small turn of the camera, outward from the middle
    # (iteration 4: a sweep from the middle to the left found it at the 3rd bolt, and the gate blew open)
    ("blast open the gate: aim (L1) and look along it for the lock, a bolt (R1) at each step", 0,
     [("hold", P.LS_DOWN, 0.4), ("pause", 0.3), ("keep", P.L1), ("pause", 0.8)] + lock_search(5, 3)
     + [("keep", 0), ("pause", 0.8)]),
    # through the gate, past the people running away, on to the end of the pier (the pier collapses on the way: a
    # cutscene; at the end Cole climbs the post)
    ("through the gate and down the pier to its end", 0,
     [("hold", P.LS_UP, 14), ("pause", 0.5), ("hold", P.LS_UP, 12), ("pause", 0.5), ("hold", P.LS_UP, 10), ("pause", 1)]),
    # "Shoot at the source of disturbance" (from the post: "Hold L1 to aim")
    ("shoot at the source of the disturbance", 0, aim_fire(5, 1.1) + [("pause", 1)]),
    # the Beast rises (a cutscene, ~50 s), then "Take down the Beast": fire, move, jump, fire again
    ("the Beast: fire at it, move, jump, fire again", 50,
     aim_fire(5, 1.2) + [("hold", P.LS_LEFT, 1.2), ("tap", P.CROSS), ("pause", 0.8)] + aim_fire(5, 1.2)
     + [("hold", P.LS_RIGHT, 1.2), ("tap", P.CROSS), ("pause", 0.8)] + aim_fire(5, 1.2) + [("pause", 1)]),
]


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

good, egood, enotes, notes, done, last_pct = [], [], [], [], 0.0, -1
prog_t0 = time.monotonic()
for r in plan:
    tag = r["tag"]
    is_scene = tag in SCENES
    is_save = tag == "save" or is_scene   # both start from a save: the world frame is the anchor
    rd = os.path.join(runs_dir, tag)
    remove_tree(rd)
    gd = os.path.join(rd, "gamedata")
    sv = os.path.join(gd, "dev_hdd0", "home", "00000001", "savedata")
    if is_save:   # the player's saves (or the scene's training save), copied (the game loads the newest by itself at boot)
        try:
            shutil.copytree(r.get("gamedata", gamedata), gd, symlinks=True)
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
    eraw, elast = os.path.join(rd, "engine.profraw"), os.path.join(rd, "engine-last.profraw")
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
    if eprofdata:
        env["PS3_PGO_ENGINE_FILE"] = eraw   # the engine's own counters (the module keeps LLVM_PROFILE_FILE)
    if is_save:
        env["PS3RECOMP_ENG_WORLD_MARK_AFTER"] = "40"
    if not is_save:   # the new game: the Player above on PAD_FILE, no fixed script
        del env["PAD_SCRIPT"]
    if is_scene:   # the recorded walk on the frame-stepped clock (the benchmark's way), no scripted presses
        del env["PAD_SCRIPT"]
        env.update({"PS3_PAD_REPLAY": r["route"], "PS3RECOMP_ENG_WORLD_MARK_AFTER": str(SCENES[tag]["after"]),
                    "PS3_TB_FRAMESTEP_US": "33333", "PS3_TB_FRAMESTEP_IDLE_US": "2000"})
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
    subs = []
    player = None if is_save else Player(pad, OPENING_SECTIONS, say)
    writes, closed_by, end_at, anchor_at, start_at, next_start, start_until = 0, "", -1.0, -1.0, -1.0, 0.0, -1.0
    while p.poll() is None:
        time.sleep(0.1)
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
            elif "[pgo] engine profile written at " in text and "(rc=0)" in text:
                try:
                    os.replace(eraw, elast)
                except OSError:
                    pass
            elif "menu" not in ev and "load_menu_catalog" in text:
                ev["menu"] = ms
            elif "montage" not in ev and "intro_montage" in text:
                ev["montage"] = ms
            elif "story" not in ev and "h_jbi_cityfalls" in text:
                ev["story"] = ms
            if "[fs] open" in text and "/subtitle/" in text:   # a cutscene: sub1, sub2, ... in order (each name once)
                nm = text.rsplit("/", 1)[-1].split("'")[0]
                if nm not in subs:
                    subs.append(nm)
                    ev["sub%d" % len(subs)] = ms
            elif "world" not in ev and "[rsx engine] world mark" in text:
                ev["world"] = ms
        if is_save:
            if "world" in ev and end_at < 0:
                end_at = ev["world"] + (SCENES[tag]["close"] * 1000 if is_scene else 110000)   # the moves, then close
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
                end_at = anchor_at + 330000   # (the player's last section ends well before)
            if anchor_at >= 0:
                player.step(now, anchor_at, ev)
                if player.done and end_at > anchor_at + player.done_t * 1000 + 3000:
                    end_at = anchor_at + player.done_t * 1000 + 3000
        if a.stop_after > 0 and now >= a.stop_after * 1000 and not closed_by:
            closed_by, end_at = f"test: closed after {a.stop_after} s", now
        pct = int(min(99, 100 * (done + min(time.monotonic() - t0, r["est"])) / est_all))
        if pct != last_pct:
            say(f"  progress: {pct}% training")
            last_pct = pct
        # never close within ~2 s of a counters write (PS3_PGO_DUMP_AT, every 40 s): the dump thread and the exit's writer
        # would write the same file at once (a plaza run closed 1.7 s after its 160 s write left both in one file)
        phase = (now / 1000.0) % 40.0
        near_dump = phase >= 38.5 or phase < 4.0
        if ((end_at >= 0 and now >= end_at) or now >= r["cap"] * 1000) and not near_dump:
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
        if eprofdata:   # the engine's counters of the same run (the same rule: only runs that reached the game)
            epick = next((f for f in (eraw, elast) if test_profraw(f, 1 << 16)), None)
            if epick:
                egood.append(epick)
                say(f"  run {tag}: engine counters {os.path.getsize(epick) / 1048576:.1f} MB{' (the last complete write)' if epick == elast else ''}")
            else:
                enotes.append(f"no readable engine counters in run {tag}")
                say(f"  run {tag}: no readable engine counters")
    else:
        if closed_by.startswith("the game closed by itself") and reason:
            reason += " (the game window was closed or the game stopped)"
        notes.append(reason)
        say(f"  run {tag}: not used ({reason})")
    if os.environ.get("PORT_PGO_KEEP_RUN") != "1":   # (a test: the run's saves stay)
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
keep_raw = os.environ.get("PORT_PGO_KEEP_RAW") == "1"   # (a test: the runs' counters stay for merging by hand)
for r in plan:
    for f in glob.glob(os.path.join(runs_dir, r["tag"], "counters*.profraw")):
        if not keep_raw:
            os.remove(f)
if m.returncode or not test_profraw(profdata):
    if os.path.exists(profdata):
        os.remove(profdata)
    why(f"merging the counters failed (llvm-profdata exit {m.returncode})")
say(f"  profile: {profdata} ({os.path.getsize(profdata) / 1048576:.0f} MB, from {len(good)} run(s))")
if eprofdata:   # the engine's profile: left out (said why) when it does not work; the game module's stands
    if os.path.exists(eprofdata):
        os.remove(eprofdata)
    if not egood:
        say("  engine: not used: " + (enotes[0] if enotes else "no engine counters"))
    else:
        m = subprocess.run([llvm_prof, "merge", "--failure-mode=all", "-o", eprofdata] + egood,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
        for ln in m.stdout.splitlines():
            say("    " + ln)
        if m.returncode or not test_profraw(eprofdata, 1 << 12):
            if os.path.exists(eprofdata):
                os.remove(eprofdata)
            say(f"  engine: not used: merging its counters failed (llvm-profdata exit {m.returncode})")
        else:
            say(f"  engine profile: {eprofdata} ({os.path.getsize(eprofdata) / 1048576:.1f} MB, from {len(egood)} run(s))")
    for r in plan:
        for f in glob.glob(os.path.join(runs_dir, r["tag"], "engine*.profraw")):
            if not keep_raw:
                os.remove(f)
sys.exit(0)
