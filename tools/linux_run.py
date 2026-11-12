"""Linux test run of the native build (tools/native_run.ps1 + fast.ps1 + fast_driver.ps1 in one):
run the game for up to N seconds on an X display, optionally driven by a steps file, keeping its
output as a recording in native/recordings/<tag>/.

    python3 tools/linux_run.py --tag T [--seconds 120] [--exe work/port/build/Infamous2Recomp]
                               [--steps tools/city_fast.steps] [--savefrom work/saves/first-save/gamedata]
                               [--env K=V ...] [--fastmovies 0|1] [--xshots 30,60]

Layout as native_run.ps1: /dev_bdvd = game/disc, /dev_hdd0 = game/dev_hdd0, /dev_flash =
game/dev_flash (else PORT_DEV_FLASH), saves in <rec>/gamedata (a fresh copy of --savefrom, never
the player's own). The game gets DISPLAY (default :0) and, when unset, the GDM session's
XAUTHORITY. Output: stdout.log, stderr.log, stderr_live.tsv ('<ms since start>\t<line>'),
driver.tsv, run.json, shot_<name>.png (the game's own image, PS3RECOMP_SHOT_REQ),
xshot_<name>.png (the X screen, ImageMagick import), exit code 0 = it ran to the end or closed
when told.

Steps files: fast_driver.ps1's language (wait, spam, press, sleep, anchor, anchorafter, anchorfile,
at, shot, waitimg, spamimg, close, say, fail, threads), plus
    xshot <name>        the whole X screen -> xshot_<name>.png
    xkey <keysym> [ms]  a key held for ms (tools/xtest.py; only into the focused game window)
    xmove <dx> <dy> [n] relative mouse motion in n steps;  xclick <button> [ms];  xfocus [name]
and their '#!' headers (seconds, pad, savefrom, fastmovies, worldmark, replay). waitimg/spamimg
need Pillow and work/fast_refs/<ref>.png. 'close' = SIGTERM (SDL turns it into the window's quit,
the game's atexit handlers run), SIGKILL after 15 s.
Fast movies (a run from a save or with a steps file, unless --fastmovies 0): work/fastmovies/,
the disc and dev_hdd0 seen through symlinks, every cutscene movie the disc's own 4 s placeholder
(tools/make_fastmovies.ps1's view; nothing in game/ is changed).
"""
import argparse
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAME = os.path.join(ROOT, "game")


def log(*a):
    print(*a, flush=True)


# ------------------------------------------------------------------ fast-movie views
def make_fastmovies(out):
    """work/fastmovies/{disc,dev_hdd0}: directories mirrored down to movies/cutscene, everything
    else a symlink into game/; cutscene .bik files -> the placeholder movie."""
    ph = os.path.join(GAME, "disc", "PS3_GAME", "USRDIR", "movies", "cutscene", "cut_scene_place_holder.bik")
    if not os.path.isfile(ph):
        sys.exit("no placeholder movie at " + ph)
    if os.path.islink(out) or os.path.isfile(out):
        os.remove(out)
    elif os.path.isdir(out):
        shutil.rmtree(out)   # symlinks inside are removed, never their targets

    def mirror(src, dst, descend):
        os.makedirs(dst, exist_ok=True)
        for e in sorted(os.listdir(src)):
            s, t = os.path.join(src, e), os.path.join(dst, e)
            if os.path.isdir(s):
                if descend and e == descend[0]:
                    mirror(s, t, descend[1:])
                else:
                    os.symlink(os.path.realpath(s), t)
            elif not descend and e.lower().endswith(".bik") and e != "cut_scene_place_holder.bik":
                os.symlink(os.path.realpath(ph), t)
            else:
                os.symlink(os.path.realpath(s), t)
    mirror(os.path.join(GAME, "disc"), os.path.join(out, "disc"), ["PS3_GAME", "USRDIR", "movies", "cutscene"])
    g = os.path.join(GAME, "dev_hdd0", "game")
    inst = sorted(d for d in os.listdir(g) if d.endswith("_INSTALL")) if os.path.isdir(g) else []
    mirror(os.path.join(GAME, "dev_hdd0"), os.path.join(out, "dev_hdd0"),
           ["game", inst[0] if inst else "BCES01143_INSTALL", "USRDIR", "movies", "cutscene"])
    with open(os.path.join(out, ".ready"), "w") as f:
        f.write("ok\n")


# ------------------------------------------------------------------ the run
class Run:
    def __init__(self, a):
        self.a = a
        self.rec = os.path.join(ROOT, "native", "recordings", a.tag)
        os.makedirs(self.rec, exist_ok=True)
        self.lines = []          # (ms, text)
        self.cursor = 0
        self.lock = threading.Lock()
        self.anchor_ms = -1
        self.last_line_ms = -1
        self.t0 = None
        self.p = None
        self.dlog = None
        self.stopped = None

    def now(self):
        return int((time.monotonic() - self.t0) * 1000)

    def note(self, step, info=""):
        self.dlog.write("%d\t%s\t%s\n" % (self.now(), step, info))
        self.dlog.flush()

    def alive(self):
        return self.p.poll() is None

    # -- stderr reader: every line with its time, live into stderr_live.tsv
    def pump(self, stream, live):
        for raw in iter(stream.readline, b""):
            ms = self.now()
            txt = raw.decode("utf-8", "replace").rstrip("\r\n")
            with self.lock:
                self.lines.append((ms, txt))
            live.write("%d\t%s\n" % (ms, txt))
            live.flush()

    def wait_line(self, rx, timeout):
        r = re.compile(rx)
        dl = time.monotonic() + timeout
        while time.monotonic() < dl and self.alive():
            with self.lock:
                while self.cursor < len(self.lines):
                    ms, l = self.lines[self.cursor]
                    self.cursor += 1
                    if r.search(l):
                        self.last_line_ms = ms
                        return ms, l
            time.sleep(0.04)
        return None

    def press(self, mask, polls=40):
        tmp = self.pad + ".tmp"
        with open(tmp, "w") as f:
            f.write("%s %d" % (mask, polls))
        os.replace(tmp, self.pad)

    def stop(self, why):
        self.note("stop", why)
        self.stopped = why
        with open(os.path.join(self.rec, "stop.flag"), "w") as f:
            f.write(why + "\n")

    # -- pictures
    def shot(self, out, timeout=4.0):
        """The game's own last image (PS3RECOMP_SHOT_REQ); False when the build does not serve it."""
        req = self.env.get("PS3RECOMP_SHOT_REQ")
        if not req or not os.path.exists(req + ".ready"):
            return False
        out = os.path.abspath(out)
        if os.path.exists(out):
            os.remove(out)
        with open(req + ".tmp", "w") as f:
            f.write(out)
        os.replace(req + ".tmp", req)
        dl = time.monotonic() + timeout
        while time.monotonic() < dl and not os.path.exists(out):
            time.sleep(0.02)
        return os.path.exists(out)

    def xshot(self, out):
        env = dict(os.environ, DISPLAY=self.env["DISPLAY"], XAUTHORITY=self.env.get("XAUTHORITY", ""))
        r = subprocess.run(["import", "-window", "root", out], env=env, capture_output=True, timeout=30)
        return r.returncode == 0 and os.path.exists(out)

    def screen_diff(self, ref, region):
        try:
            from PIL import Image
        except ImportError:
            return 999.0
        def thumb(im, r):
            sx, sy = im.width / 1280.0, im.height / 720.0
            c = im.crop((int(r[0] * sx), int(r[1] * sy), int((r[0] + r[2]) * sx), int((r[1] + r[3]) * sy)))
            return list(c.convert("L").resize((32, 18), Image.BILINEAR).getdata())
        key = (ref, tuple(region))
        if key not in self.refcache:
            rb = Image.open(os.path.join(ROOT, "work", "fast_refs", ref + ".png")).convert("RGB")
            if (rb.width, rb.height) != (1280, 720):
                full = Image.new("RGB", (1280, 720))
                full.paste(rb.resize((region[2], region[3])), (region[0], region[1]))
                rb = full
            self.refcache[key] = thumb(rb, region)
        tmp = os.path.join(self.rec, "driver_look.png")
        if not self.shot(tmp):
            return 999.0
        try:
            v = thumb(Image.open(tmp).convert("RGB"), region)
        except OSError:
            return 999.0
        a = self.refcache[key]
        return sum(abs(x - y) for x, y in zip(v, a)) / len(a)

    def close_game(self):
        self.p.send_signal(signal.SIGTERM)
        self.note("close", "SIGTERM sent")
        try:
            self.p.wait(15)
            self.note("close", "exited by itself")
        except subprocess.TimeoutExpired:
            self.stop("close: no clean exit in 15 s")

    def threads(self, name, sec):
        tck = os.sysconf("SC_CLK_TCK")
        def sample():
            out = {}
            d = "/proc/%d/task" % self.p.pid
            for t in os.listdir(d):
                try:
                    with open(os.path.join(d, t, "stat")) as f:
                        s = f.read()
                    nm = s[s.index("(") + 1:s.rindex(")")]
                    fl = s[s.rindex(")") + 2:].split()
                    out[int(t)] = (nm, (int(fl[11]) + int(fl[12])) * 1000.0 / tck)
                except (OSError, ValueError):
                    pass
            return out
        a, w0 = sample(), time.monotonic()
        time.sleep(sec)
        b, wall = sample(), (time.monotonic() - w0) * 1000.0
        rows = sorted(((tid, 100.0 * (ms - a.get(tid, (nm, 0.0))[1]) / wall, nm) for tid, (nm, ms) in b.items()),
                      key=lambda r: -r[1])
        with open(os.path.join(self.rec, "threads_%s.tsv" % name), "w") as f:
            f.write("hosttid\tcpu_pct\tname\n")
            for tid, pct, nm in rows:
                if pct > 2:
                    f.write("%d\t%.1f\t%s\n" % (tid, pct, nm))

    # -- the steps
    def drive(self, steps):
        self.refcache = {}
        for raw in steps:
            if not self.alive():
                self.note("gone", "the game exited")
                return
            s = raw.strip()
            op, _, rest = s.partition(" ")
            rest = rest.strip()
            if op == "wait":
                m = re.match(r"^(.*?)(?:\s+(\d+(?:\.\d+)?))?$", rest)
                hit = self.wait_line(m.group(1), float(m.group(2) or 300))
                if not hit:
                    self.note("TIMEOUT", "wait " + m.group(1)); self.stop("timeout: wait " + m.group(1)); return
                self.note("wait", "matched %s at game ms %d" % (m.group(1), hit[0]))
            elif op == "spam":
                mask, every, tail = rest.split(None, 2)
                m = re.match(r"^(.*?)(?:\s+(\d+(?:\.\d+)?))?$", tail)
                dl = time.monotonic() + float(m.group(2) or 300)
                n, hit = 0, None
                while time.monotonic() < dl and self.alive() and not hit:
                    self.press(mask); n += 1
                    hit = self.wait_line(m.group(1), float(every))
                if not hit:
                    self.note("TIMEOUT", "spam %s until %s (%d presses)" % (mask, m.group(1), n))
                    self.stop("timeout: spam " + m.group(1)); return
                self.note("spam", "%s x%d until %s (game ms %d)" % (mask, n, m.group(1), hit[0]))
            elif op == "press":
                a = rest.split()
                self.press(a[0], int(a[1]) if len(a) > 1 else 40)
                self.note("press", rest)
            elif op == "sleep":
                time.sleep(float(rest)); self.note("sleep", rest)
            elif op in ("anchor", "anchorafter"):
                if op == "anchorafter":
                    if self.last_line_ms < 0:
                        self.note("error", "anchorafter without a matched wait"); self.stop("anchorafter: no wait line"); return
                    t = self.last_line_ms + int(float(rest) * 1000)
                    while self.now() < t and self.alive():
                        time.sleep(0.02)
                self.anchor_ms = self.now()
                with open(self.anchor_flag, "w") as f:
                    f.write("%d\n" % self.anchor_ms)
                self.note("anchor", str(self.anchor_ms))
            elif op == "anchorfile":
                dl = time.monotonic() + float(rest or 300)
                while not os.path.exists(self.anchor_flag) and time.monotonic() < dl and self.alive():
                    time.sleep(0.04)
                if not os.path.exists(self.anchor_flag):
                    self.note("TIMEOUT", "anchorfile"); self.stop("timeout: anchorfile"); return
                self.anchor_ms = int((os.path.getmtime(self.anchor_flag) - self.wall0) * 1000)
                self.note("anchor", str(self.anchor_ms))
            elif op == "at":
                if self.anchor_ms < 0:
                    self.note("error", "at before anchor"); continue
                t = self.anchor_ms + int(float(rest) * 1000)
                while self.now() < t and self.alive():
                    time.sleep(min(0.25, max(0.001, (t - self.now()) / 1000.0)))
            elif op == "shot":
                ok = self.shot(os.path.join(self.rec, "shot_%s.png" % rest))
                if not ok:
                    ok = self.xshot(os.path.join(self.rec, "shot_%s.png" % rest))
                self.note("shot", rest if ok else "failed " + rest)
            elif op == "xshot":
                ok = self.xshot(os.path.join(self.rec, "xshot_%s.png" % rest))
                self.note("xshot", rest if ok else "failed " + rest)
            elif op in ("xkey", "xmove", "xclick", "xfocus"):
                # keyboard / mouse through X (tools/xtest.py): only into the game's focused window
                a = rest.split()
                args = {"xkey": ["key"] + a, "xmove": ["move"] + a, "xclick": ["click"] + a,
                        "xfocus": ["activate", a[0] if a else "inFAMOUS"]}[op]
                env = dict(os.environ, DISPLAY=self.env["DISPLAY"], XAUTHORITY=self.env.get("XAUTHORITY", ""))
                r = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "xtest.py")] + args, env=env,
                                   capture_output=True, text=True, timeout=60)
                self.note(op, "%s -> %s" % (rest, (r.stdout + r.stderr).strip().replace("\n", " | ") or "ok"))
            elif op in ("waitimg", "spamimg"):
                a = rest.split()
                if op == "spamimg":
                    mask, every, a = a[0], float(a[1]), a[2:]
                ref, region, mx = a[0], [int(v) for v in a[1].split(",")], float(a[2])
                dl = time.monotonic() + (float(a[3]) if len(a) > 3 else 120)
                d, n, nextp = 999.0, 0, 0.0
                while time.monotonic() < dl and self.alive():
                    d = self.screen_diff(ref, region)
                    if d <= mx:
                        break
                    if op == "spamimg" and time.monotonic() >= nextp:
                        self.press(mask); n += 1; nextp = time.monotonic() + every
                    time.sleep(0.2)
                if d > mx:
                    self.note("TIMEOUT", "%s %s (last diff %.1f)" % (op, ref, d)); self.stop("timeout: %s %s" % (op, ref)); return
                self.note(op, "%s diff %.1f%s" % (ref, d, " x%d" % n if op == "spamimg" else ""))
            elif op == "close":
                self.close_game(); return
            elif op == "threads":
                a = rest.split()
                self.threads(a[0], float(a[1])); self.note("threads", rest)
            elif op == "say":
                log(rest); self.note("say", rest)
            elif op == "fail":
                self.stop("fail: " + rest); return
            else:
                self.note("error", "unknown step: " + s)
        self.note("end")

    def main(self):
        a = self.a
        steps, hdr = [], {}
        if a.steps:
            for l in open(a.steps, encoding="utf-8"):
                m = re.match(r"^#!\s*(\w+)\s+(.*)$", l)
                if m:
                    hdr[m.group(1)] = m.group(2).strip()
                elif l.strip() and not l.strip().startswith("#"):
                    steps.append(l)
        secs = a.seconds or int(hdr.get("seconds", 120))
        savefrom = a.savefrom or (os.path.join(ROOT, hdr["savefrom"].replace("\\", "/")) if "savefrom" in hdr else "")
        rec = self.rec
        for n in os.listdir(rec):
            if n.startswith(("shot_", "xshot_")) or n in ("anchor.flag", "pad.txt", "stop.flag", "shot.req",
                                                         "shot.req.ready", "driver.tsv", "ftrace.txt"):
                os.remove(os.path.join(rec, n))
        gd = os.path.join(rec, "gamedata")
        if os.path.isdir(gd):
            shutil.rmtree(gd)
        sd = os.path.join(gd, "dev_hdd0", "home", "00000001", "savedata")
        os.makedirs(sd)
        if savefrom:
            if os.path.isdir(os.path.join(savefrom, "dev_hdd0")):
                shutil.copytree(savefrom, gd, dirs_exist_ok=True)
            else:
                shutil.copytree(savefrom, sd, dirs_exist_ok=True)
            log("saves from %s: %s" % (savefrom, ", ".join(sorted(os.listdir(sd)))))
        if os.path.exists(os.path.join(rec, "settings.ini")):
            os.remove(os.path.join(rec, "settings.ini"))
        flash = os.environ.get("PORT_DEV_FLASH") or os.path.join(GAME, "dev_flash")
        env = {
            "PS3_VFS_ROOT": os.path.join(GAME, "disc"), "PS3_HDD0_ROOT": os.path.join(GAME, "dev_hdd0"),
            "PS3_DEV_FLASH": flash, "PS3_GAMEDATA_ROOT": gd, "PS3_TITLE": "inFAMOUS 2", "PS3_LANG": "en",
            "PS3_AUDIO_VOLUME": "15", "PS3_PAD_SDL": "0", "PS3_VERBOSE": "0",
            "GCM_SEMA_ACQUIRE": "1", "GCM_SEMA_ACQUIRE_MS": "200", "GCM_PARK_GRACE_MS": "1000",
            "PS3RECOMP_ENG_GPU_LIGHT": "07000B80:07384B80", "PS3RECOMP_ENG_GPU_SSAO": "0F800000",
            "PS3RECOMP_VK_WINDOW": "1", "PS3RECOMP_VK_GUEST_PROGRAMS": "1", "PS3RECOMP_KEEP_AWAKE": "0",
            "PS3_OVERLAY_INI": os.path.join(rec, "settings.ini"), "PS3_OVERLAY_HUD": "0",
            "PS3RECOMP_SHOT_REQ": os.path.join(rec, "shot.req"),
            "DISPLAY": os.environ.get("DISPLAY") or ":0",
        }
        xa = os.environ.get("XAUTHORITY") or "/run/user/%d/gdm/Xauthority" % os.getuid()
        if os.path.exists(xa):
            env["XAUTHORITY"] = xa
        # An X screen without DRI3 (a headless box's dummy driver: the GPU renders, the screen is a plain
        # framebuffer) lets Mesa's hardware drivers present only through their software path; without
        # it the backend falls back to llvmpipe (CPU rendering). --env MESA_VK_WSI_DEBUG= turns it off.
        try:
            xd = subprocess.run(["xdpyinfo", "-queryExtensions"], env=dict(os.environ, **{k: env[k] for k in ("DISPLAY", "XAUTHORITY") if k in env}),
                                capture_output=True, text=True, timeout=10).stdout
            if xd and "DRI3" not in xd:
                env["MESA_VK_WSI_DEBUG"] = "sw"
        except (OSError, subprocess.TimeoutExpired):
            pass
        self.pad = os.path.join(rec, "pad.txt")
        self.anchor_flag = os.path.join(rec, "anchor.flag")
        if steps:
            env.update({"PAD_FILE": self.pad, "PAD_SCRIPT_WAIT": self.anchor_flag,
                        "PS3_FTRACE_KINDS": "5,7,10,12,16,17",
                        "PS3_FTRACE": "5:%d:%s" % (secs + 60, os.path.join(rec, "ftrace.txt"))})
            if "pad" in hdr:
                env["PAD_SCRIPT"] = hdr["pad"]
            if "replay" in hdr:
                env["PS3_PAD_REPLAY"] = os.path.join(ROOT, hdr["replay"].replace("\\", "/"))
            if "worldmark" in hdr:
                env["PS3RECOMP_ENG_WORLD_MARK"] = self.anchor_flag
                if int(hdr["worldmark"]) > 0:
                    env["PS3RECOMP_ENG_WORLD_MARK_AFTER"] = hdr["worldmark"]
        fast = a.fastmovies if a.fastmovies is not None else (1 if (savefrom or steps) and hdr.get("fastmovies") != "0" else 0)
        if fast:
            fm = os.path.join(ROOT, "work", "fastmovies")
            if not os.path.exists(os.path.join(fm, ".ready")):
                make_fastmovies(fm)
            env["PS3_VFS_ROOT"], env["PS3_HDD0_ROOT"] = os.path.join(fm, "disc"), os.path.join(fm, "dev_hdd0")
        for kv in a.env or []:
            k, _, v = kv.partition("=")
            env[k] = v
        self.env = env
        exe = os.path.abspath(a.exe)
        elf = os.path.join(ROOT, "work", "EBOOT.elf")
        so = open(os.path.join(rec, "stdout.log"), "wb")
        live = open(os.path.join(rec, "stderr_live.tsv"), "w", encoding="utf-8")
        self.dlog = open(os.path.join(rec, "driver.tsv"), "w")
        self.dlog.write("ms\tstep\tinfo\n")
        log("run %s: %s, up to %d s, movies %s, display %s" % (a.tag, exe, secs, "fast" if fast else "full", env["DISPLAY"]))
        self.wall0 = time.time()
        self.t0 = time.monotonic()
        self.p = subprocess.Popen([exe, elf], cwd=rec, env=dict(os.environ, **env), stdout=so,
                                  stderr=subprocess.PIPE, stdin=subprocess.DEVNULL,
                                  preexec_fn=lambda: os.nice(a.nice) if a.nice else None)
        rd = threading.Thread(target=self.pump, args=(self.p.stderr, live), daemon=True)
        rd.start()
        self.note("start", "steps=%s" % (a.steps or ""))
        xs = sorted(int(x) for x in a.xshots.split(",") if x) if a.xshots else []
        if steps:
            drv = threading.Thread(target=self.drive, args=(steps,), daemon=True)
            drv.start()
        deadline = self.t0 + secs
        while self.alive() and time.monotonic() < deadline and not self.stopped:
            while xs and time.monotonic() - self.t0 >= xs[0]:
                t = xs.pop(0)
                self.xshot(os.path.join(rec, "xshot_%ds.png" % t))
                self.shot(os.path.join(rec, "shot_%ds.png" % t))
            if steps and not drv.is_alive() and self.alive():
                self.note("end", "steps done, waiting for the exit")
                try:
                    self.p.wait(20)
                except subprocess.TimeoutExpired:
                    pass
                break
            time.sleep(0.1)
        exited = not self.alive()
        if not exited:
            self.p.send_signal(signal.SIGTERM)
            try:
                self.p.wait(15)
                exited_term = True
            except subprocess.TimeoutExpired:
                self.p.kill(); self.p.wait(5); exited_term = False
        else:
            exited_term = None
        rd.join(5)
        so.close(); live.close()
        with self.lock:
            with open(os.path.join(rec, "stderr.log"), "w", encoding="utf-8") as f:
                for _, l in self.lines:
                    f.write(l + "\n")
        info = {"tag": a.tag, "exe": exe, "seconds": secs, "steps": a.steps, "movies": "fast" if fast else "full",
                "exited_by_itself": exited, "exit_code": self.p.returncode, "term_clean": exited_term,
                "anchor_ms": self.anchor_ms if self.anchor_ms >= 0 else None, "stopped": self.stopped,
                "wall_s": round(time.monotonic() - self.t0, 1), "env": env}
        with open(os.path.join(rec, "run.json"), "w") as f:
            json.dump(info, f, indent=1)
        self.dlog.close()
        log("recording %s: exited_by_itself=%s exit=%s stopped=%s stderr=%d lines" %
            (rec, exited, self.p.returncode, self.stopped, len(self.lines)))
        return 0 if not (self.stopped and not self.stopped.startswith("close")) else 2


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tag", required=True)
    ap.add_argument("--seconds", type=int, default=0)
    ap.add_argument("--exe", default=os.path.join(ROOT, "work", "port", "build", "Infamous2Recomp"))
    ap.add_argument("--steps", default="")
    ap.add_argument("--savefrom", default="")
    ap.add_argument("--env", action="append")
    ap.add_argument("--fastmovies", type=int, choices=(0, 1), default=None)
    ap.add_argument("--xshots", default="", help="seconds after start for X screen + game shots, e.g. 30,60")
    ap.add_argument("--nice", type=int, default=0)
    a = ap.parse_args()
    if a.steps:
        a.steps = os.path.abspath(a.steps)
    if a.savefrom:
        a.savefrom = os.path.abspath(a.savefrom)
    sys.exit(Run(a).main())


if __name__ == "__main__":
    main()
