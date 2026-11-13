"""Windows (and Linux) driver for the inFAMOUS 2 lift (replaces the Mac port's rebuild.py).

Same translation steps and arguments as base/infamous2-lle/tools/rebuild.py (the
port base, Erik Martins' infamous2-recomp), without its
macOS host check, NPUA80638 title-ID check and .rap requirement (our copy is the
EU disc BCES01143 + v1.04 update, whose code is identical: PPU-41a7d047...).

    python -I tools/rebuild_win.py [--only step,step] [--from step]
    python -I tools/rebuild.py ...        (the same driver, a host-neutral name)

Steps: inputs port firmware ppu spu jobs compile
Logs:  work/logs/<step>.log    Output: work/port/

Host: on Windows clang-cl + lld-link in the Visual Studio / portable MSVC environment.
On Linux (step_compile_linux) clang/clang++ + lld with the same flags in GNU spelling
(PORT_LIFT_OPT / PORT_SPU_ARCH then take GNU flags), the same PORT_PGO / PORT_LIFT_LTO;
no vsenv, no resource compiler, no SDL2.dll. Linux-only inputs: PORT_SDL2_DIR (an SDL2
CMake config folder; unset = the system's), PORT_GLSLANG_DIR (glslang's
lib/cmake/glslang; unset = deps/glslang-install-linux when present, else the system's).
"""
import argparse
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# The engine (engine/, our ps3recomp fork; "the toolkit" in older notes) and the port base (base/, Erik
# Martins' infamous2-recomp) are folders of this repository. PORT_TK: another engine tree (an agent
# worktree of the old toolkit repository, or another checkout's engine/); PORT_BASE: another port base.
TK = os.environ.get("PORT_TK") or os.path.join(ROOT, "engine")
PORT_SRC = os.path.join(os.environ.get("PORT_BASE") or os.path.join(ROOT, "base"), "infamous2-lle")
WORK = os.path.join(ROOT, "work")
PORT = os.environ.get("PORT_DIR") or os.path.join(WORK, "port")   # PORT_DIR: a separate port tree (lifter A/B)
FW = os.path.join(WORK, "fw")
LOGS = os.path.join(WORK, "logs")
ELF_SRC = os.path.join(ROOT, "eboot", "EBOOT_upd104.elf")
ELF = os.path.join(WORK, "EBOOT.elf")
IS_WIN = os.name == "nt"   # else Linux: step_compile_linux
PY = os.path.join(WORK, "venv", "Scripts", "python.exe") if IS_WIN else os.path.join(WORK, "venv", "bin", "python")
DEPS = os.path.join(ROOT, "deps")
# PORT_SPU_REWRITES: another native SPU rewrite directory (an agent's own copy; holds natives.txt + *.c),
# PORT_SPU_NATIVES_FILE: another natives.txt. Defaults: natives/spu of the repository (work/rewrites/spu when
# it holds a natives.txt) and its natives.txt.
# (natives, 2026-11-13) The published rewrites are natives/spu of this repository (Edge geometry, the BSP clipper,
# the render-list builder: MIT, see natives/README.md); a developer's work/rewrites/spu with a natives.txt wins.
_DEV_REWRITES = os.path.join(WORK, "rewrites", "spu")
SPU_REWRITES = os.path.abspath(os.environ.get("PORT_SPU_REWRITES") or
                               (_DEV_REWRITES if os.path.isfile(os.path.join(_DEV_REWRITES, "natives.txt"))
                                else os.path.join(ROOT, "natives", "spu")))
SPU_NATIVES = os.path.abspath(os.environ.get("PORT_SPU_NATIVES_FILE") or os.path.join(SPU_REWRITES, "natives.txt"))
# Run-time SPU instruction alternatives (trim, 2026-11-10): RPCS3-style SPU patches as switches the
# game flips while it runs (tools/spu_patches.txt; toolkit spu_lifter.py SPU_PATCH_FILE).
SPU_PATCHES = os.path.join(ROOT, "tools", "spu_patches.txt")
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import port_paths   # RPCS3 / Visual Studio / LLVM / Ninja on this PC (config.local.json, Setup.cmd)

PPU_HASH = "PPU-41a7d047c573c78c0307506c9d1b52d15a66e4f5"   # v1.04 == NPUA80638 1.03
FIRMWARE = ("liblv2", "libsysmodule", "libsre")
TESTED_FW_PRX_SHA256 = {   # from rebuild.py (firmware 4.93)
    "liblv2": "aac69540830b4a6673b93331bf52266f360551a2732adcbb3c7a808daddf09c5",
    "libsysmodule": "6e4c69756c84a0965206e64fee264c14d0e5ae09e35ec37f6713f519283c3be0",
    "libsre": "9e7ee6edc99b1d372686cf927b883b18f19ed4d32d1faf35e214cca4ae0737b6",
}
PORT_FILES = ("CMakeLists.txt", "main.cpp", "stubs.cpp", "gpu_light.c", "gpu_ssao.c",
              "ssao_gpu.c", "ssao_gpu.h", "edge_watch.c", "game_clock.c", "game_patches.cpp",
              "run.sh", "config.toml", "macos_host.m", "Info.plist.in", "window_icon.png")
TOOL_DATA = ("log_triage_known.txt",)
SPU_IMAGE_EXTRA = ("0x9C84", "0x9DB8")
JOBMOD_EXTRA = (("jobmod_ab654d30fd90_efe70", ("0x15280", "0x155EC")),
                ("jobmod_cbb4b5c85d75_e59f8", ("0x6C44", "0x6C64")))
STEPS = ("inputs", "port", "firmware", "ppu", "spu", "jobs", "compile")


def T(name):
    return os.path.join(TK, "tools", name)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def ppu_hash(path):
    b = open(path, "rb").read()
    phoff, = struct.unpack_from(">Q", b, 0x20)
    phentsize, phnum = struct.unpack_from(">HH", b, 0x36)
    sha = hashlib.sha1()
    for i in range(phnum):
        t, f, off, va, pa, fs, ms, al = struct.unpack_from(">IIQQQQQQ", b, phoff + i * phentsize)
        sha.update(struct.pack(">II", t, f))
        if t == 1 and ms:
            sha.update(struct.pack(">QQ", va, ms))
            sha.update(b[off:off + fs])
    return "PPU-" + sha.hexdigest()


class StepError(Exception):
    pass


def rpcs3_firmware(rpcs3):
    """(the installed firmware's dev_flash, the RPCS3 program). Windows: both in the RPCS3 folder.
    Linux: `rpcs3` is the AppImage or a folder holding rpcs3; dev_flash is beside it when there,
    else in RPCS3's config folder ($XDG_CONFIG_HOME/rpcs3, ~/.config/rpcs3)."""
    if IS_WIN:
        return os.path.join(rpcs3, "dev_flash"), os.path.join(rpcs3, "rpcs3.exe")
    exe = rpcs3 if os.path.isfile(rpcs3) else os.path.join(rpcs3, "rpcs3")
    flash = os.path.join(os.path.dirname(exe), "dev_flash")
    if not os.path.isdir(flash):
        cfg = os.environ.get("XDG_CONFIG_HOME") or os.path.join(os.path.expanduser("~"), ".config")
        flash = os.path.join(cfg, "rpcs3", "dev_flash")
    return flash, exe


def run(step, cmd, cwd=PORT, env_defaults=None):
    os.makedirs(LOGS, exist_ok=True)
    with open(os.path.join(LOGS, step + ".log"), "a", encoding="utf-8", errors="replace") as lf:
        lf.write("\n$ " + " ".join(cmd) + "\n")
        lf.flush()
        t0 = time.time()
        # the port's own tools (relift_jobmod, lift_datamods, lift_relocmods)
        # find the toolkit through PS3RECOMP
        env = dict(os.environ, PS3RECOMP=TK)
        for k, v in (env_defaults or {}).items():
            env.setdefault(k, v)
        # The game's SPU job lifts with merged chunks (spu3, 2026-11-08: exact on every
        # recording, part of batch 1: replay -5%, city +2-4% fps). SPU_LIFT_MERGE_CHUNKS=0
        # in the environment = the old lift.
        if step in ("spu", "jobs"):
            env.setdefault("SPU_LIFT_MERGE_CHUNKS", "1")
            # cpu agent (2026-11-09): a constant-target `bi` inside a merged group is a guarded
            # goto (toolkit spu_lifter.py --const-bi; exact by the replay; PS3_SPU_CONST_BI=0 at
            # run time = the indirect branch again). SPU_LIFT_CONST_BI=0 = the old lift.
            env.setdefault("SPU_LIFT_CONST_BI", "1")
            # Native rewrites of lifted SPU functions (game-derived C in work/rewrites/spu, gitignored):
            # the lift hooks the functions natives.txt names; step_compile builds the rewrites.
            # PORT_SPU_NATIVES=0 = no hooks (and no rewrites compiled).
            if os.environ.get("PORT_SPU_NATIVES", "1") != "0" and os.path.isfile(SPU_NATIVES):
                env.setdefault("SPU_NATIVE_FILE", SPU_NATIVES)
            # SPU instruction alternatives (tools/spu_patches.txt): each listed instruction is lifted
            # with its alternatives behind a run-time switch, e.g. Yahfz's "Disable Mesh Trimming"
            # (PS3RECOMP_MESH_TRIM, default 0 = the patch on). PORT_SPU_PATCHES=0 = a lift without
            # them (the game's code only); a toolkit without the feature ignores the variable.
            if os.environ.get("PORT_SPU_PATCHES", "1") != "0" and os.path.isfile(SPU_PATCHES):
                env.setdefault("SPU_PATCH_FILE", SPU_PATCHES)
        p = subprocess.run(cmd, cwd=cwd, stdout=lf, stderr=subprocess.STDOUT, env=env)
        lf.write(f"[exit {p.returncode} after {time.time() - t0:.1f}s]\n")
    if p.returncode != 0:
        raise StepError(f"{os.path.basename(cmd[1]) if len(cmd) > 1 else cmd[0]} exited {p.returncode} "
                        f"(see work/logs/{step}.log)")


def fresh(*dirs):
    for d in dirs:
        p = os.path.join(PORT, d)
        if os.path.isdir(p):
            shutil.rmtree(p)


# ---- updates: lift steps kept, byte-identical lifts keep their times (updater, 2026-11-12) ----------
# PORT_LIFT_REUSE=1 (tools/setup_build.ps1 sets it; Setup keeps work/port between updates,
# tools/setup_cleanup.ps1 -KeepBuildCache): a lift step whose inputs (the game's code or firmware, the
# toolkit's tools, this script, the lift switches in the environment, the cheats' hook lists, the SPU
# patch list, ...) are those of its last good run is skipped; a step that does run gives every file that
# comes out byte-identical its old time back, so ninja compiles only what really changed. The lifts are
# deterministic: two separate lifts of the same inputs are byte-identical, and three new cheat hooks
# changed 3 of the 37 PPU chunks (work/port-m23 vs work/port-bl). Unset = the old way: every step
# lifts again and every lifted file is new.
REUSE = os.environ.get("PORT_LIFT_REUSE") == "1"
LIFT = os.path.join(PORT, ".lift")        # <step>.fp: the inputs of the step's last good run
HELD = os.path.join(LIFT, "prev")         # this run's lift steps' previous outputs (for their times)
LIFT_DIRS = {"firmware": ("lle", "lle_spu"), "ppu": ("recompiled",), "spu": ("spu",)}
JOB_OUTPUTS = ("jobmods_register.c", "datamods_register.c", "relocmods_register.c", "jobmods_manifest.json")
# switches that change the compile, a tool's place or this logic, not what a lift writes
LIFT_ENV_SKIP = {"PORT_JOBS", "PORT_BUILD", "PORT_PGO", "PORT_PGO_NAMES", "PORT_LIFT_OPT", "PORT_SPU_ARCH",
                 "PORT_LIFT_LTO", "PORT_LIFT_REUSE", "PORT_BUILD_CACHE", "PORT_ONLY_CACHE", "PORT_RPCS3",
                 "PORT_VCVARS", "PORT_NINJA", "PORT_LLVM", "PORT_CMAKE", "PORT_PYTHON", "PORT_MSVC", "PORT_SDK",
                 "PORT_SETUP_API", "PORT_SETUP_CODELOAD"}   # (the last two: Setup.exe's test stand-in for GitHub)
_lifted = set()                           # the lift steps that ran in this invocation


def _hash_file(h, label, path):
    h.update(label.encode() + b"\0")
    h.update(sha256(path).encode() if path and os.path.isfile(path) else b"-")


def _hash_tree(h, top):
    if not os.path.isdir(top):
        h.update(b"no " + top.encode())
        return
    for dp, dn, fn in os.walk(top):
        dn[:] = sorted(d for d in dn if d not in ("__pycache__", ".git"))
        for f in sorted(fn):
            p = os.path.join(dp, f)
            _hash_file(h, os.path.relpath(p, top).replace("\\", "/"), p)


def fingerprint(step):
    """What a lift step reads, as one hash."""
    h = hashlib.sha256()
    h.update(("\0".join(["lift-v1", step, sys.version, str(os.cpu_count()), os.path.abspath(TK), os.path.abspath(PORT)]) + "\0").encode())
    _hash_file(h, "rebuild_win.py", os.path.abspath(__file__))
    _hash_tree(h, os.path.join(TK, "tools"))
    for k in sorted(os.environ, key=str.upper):
        ku = k.upper()
        if ku.startswith(("PPU_", "SPU_", "PORT_", "PS3_")) and ku not in LIFT_ENV_SKIP:
            h.update(f"{ku}={os.environ[k]}\0".encode())
    if step == "firmware":
        for m in FIRMWARE:
            _hash_file(h, m, os.path.join(FW, m + ".prx"))
    else:
        _hash_file(h, "elf", ELF)
    if step == "ppu":
        for f in ("hooks.txt", "patches.txt"):
            _hash_file(h, f, os.path.join(CHEATS, f))
    if step in ("spu", "jobs"):
        _hash_file(h, "natives", SPU_NATIVES)
        _hash_file(h, "spu_patches", SPU_PATCHES)
    if step == "jobs":
        _hash_tree(h, os.path.join(PORT, "tools"))   # the port's own lift tools (step_port puts them there)
    return h.hexdigest()


def _outputs(step):
    """A lift step's outputs, relative to PORT."""
    if step in LIFT_DIRS:
        return [d for d in LIFT_DIRS[step] if os.path.exists(os.path.join(PORT, d))]
    spu = os.path.join(PORT, "spu")
    names = sorted(os.listdir(spu)) if os.path.isdir(spu) else []
    return ["spu/" + n for n in names if n.startswith(("jobmod_", "datamod_", "relocmod_")) or n == "jobmods" or n in JOB_OUTPUTS]


def lift_skip(step):
    """True: the step's outputs are there and its inputs did not change since its last good run. Else the
    step will run: its fingerprint is dropped first (a stopped run never counts) and its old outputs are
    held aside, so that lift_restore_times can give byte-identical new files their old times."""
    if step not in ("firmware", "ppu", "spu", "jobs"):
        return False
    fp_file = os.path.join(LIFT, step + ".fp")
    if not REUSE:   # a lift no fingerprint describes: a later PORT_LIFT_REUSE run must not keep it
        if os.path.isfile(fp_file):
            os.remove(fp_file)
        return False
    want = fingerprint(step)
    have = open(fp_file).read().strip() if os.path.isfile(fp_file) else ""
    # jobs writes into spu/: when spu lifted again, so does jobs
    blocked = step == "jobs" and "spu" in _lifted
    if have == want and _outputs(step) and not blocked:
        print(f"  unchanged since the last build: kept ({len(_outputs(step))} output(s))", flush=True)
        return True
    if os.path.isfile(fp_file):
        os.remove(fp_file)
    for rel in _outputs(step):
        src, dst = os.path.join(PORT, rel), os.path.join(HELD, rel)
        try:
            if os.path.exists(dst):             # (held since a stopped run: that older one stays)
                shutil.rmtree(src) if os.path.isdir(src) else os.remove(src)
                continue
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            os.replace(src, dst)
        except OSError as e:                    # (a file in use: the step's own clean-up deals with it)
            print(f"  could not set {rel} aside ({e}): its files are compiled again", flush=True)
    _lifted.add(step)
    return False


def lift_done(step):
    if REUSE and step in _lifted:
        os.makedirs(LIFT, exist_ok=True)
        with open(os.path.join(LIFT, step + ".fp"), "w") as f:
            f.write(fingerprint(step) + "\n")


def clamp_future_times():
    """GitHub issues #21/#22: "ninja: error: manifest 'build.ninja' still dirty after 100 tries". A zip's file
    times have no time zone, so west of UTC the files of a commit made a few hours ago come out dated hours in
    the FUTURE; CMake's inputs then stay newer than build.ninja, and ninja re-runs CMake until it gives up.
    Any file dated after now (the engine, the port's sources) gets now; content and fingerprints are unchanged."""
    now = time.time()
    fixed = 0
    for top in (TK, PORT):
        for dirpath, dirnames, filenames in os.walk(top):
            dirnames[:] = [d for d in dirnames if not d.startswith(("build", ".git"))]
            for n in filenames:
                f = os.path.join(dirpath, n)
                try:
                    if os.lstat(f).st_mtime > now + 60:
                        os.utime(f, (now, now))
                        fixed += 1
                except OSError:
                    pass
    if fixed:
        print(f"  {fixed} file(s) were dated in the future (a time zone or clock difference): dated now", flush=True)


def lift_restore_times():
    """Every newly lifted file with the same bytes as the one it replaces gets that one's time back."""
    if not os.path.isdir(HELD):
        return
    kept = new = 0
    for dp, _, fn in os.walk(HELD):
        for f in fn:
            old = os.path.join(dp, f)
            cur = os.path.join(PORT, os.path.relpath(old, HELD))
            try:
                so = os.stat(old)
                if os.path.isfile(cur) and os.path.getsize(cur) == so.st_size and _same_bytes(old, cur):
                    os.utime(cur, ns=(so.st_atime_ns, so.st_mtime_ns))
                    kept += 1
                else:
                    new += 1
            except OSError:
                new += 1
    shutil.rmtree(HELD, ignore_errors=True)
    print(f"  lifted files: {kept} unchanged (kept their times: not compiled again), {new} new or changed", flush=True)


def _same_bytes(a, b):
    with open(a, "rb") as fa, open(b, "rb") as fb:
        while True:
            x, y = fa.read(1 << 20), fb.read(1 << 20)
            if x != y:
                return False
            if not x:
                return True


def toolset_check(build):
    """PORT_LIFT_REUSE: the compiler and the C++ libraries the kept objects were made with. When they
    changed (a newer port wants newer tools: tools/fetch_tools.py), every object is made again."""
    if not REUSE:
        return
    import port_paths as pp
    parts = []
    portable = pp.get("msvc", required=False) and pp.get("sdk", required=False)
    for k in ("llvm", "msvc", "sdk") + (() if portable else ("vcvars",)):
        v = pp.get(k, required=False)
        if not v:
            continue
        mark = os.path.join(v, ".ready") if os.path.isdir(v) else v
        stamp = ""
        if os.path.isfile(mark):
            st = os.stat(mark)
            stamp = f"{st.st_size}:{st.st_mtime_ns}"
        clang = os.path.join(v, "bin", "clang-cl.exe") if k == "llvm" else ""
        if clang and os.path.isfile(clang):
            st = os.stat(clang)
            stamp += f"|{st.st_size}:{st.st_mtime_ns}"
        parts.append(f"{k}={os.path.abspath(v)}|{stamp}")
    want = "\n".join(parts) + "\n"
    f = os.path.join(build, ".toolset")
    have = open(f).read() if os.path.isfile(f) else None
    if have is not None and have != want:
        print("  the build tools changed since the last build: every object is made again", flush=True)
        for n in ("CMakeCache.txt", ".ninja_log", ".ninja_deps", "build.ninja"):
            p = os.path.join(build, n)
            if os.path.isfile(p):
                os.remove(p)
        shutil.rmtree(os.path.join(build, "CMakeFiles"), ignore_errors=True)
    os.makedirs(build, exist_ok=True)
    with open(f, "w") as fh:
        fh.write(want)


def step_inputs():
    os.makedirs(WORK, exist_ok=True)
    if ppu_hash(ELF_SRC) != PPU_HASH:
        raise StepError(f"{ELF_SRC} is not the v1.04 code ({ppu_hash(ELF_SRC)})")
    shutil.copy2(ELF_SRC, ELF)
    print(f"  EBOOT.elf  {PPU_HASH}  ok")
    os.makedirs(FW, exist_ok=True)
    for m in FIRMWARE:
        prx = os.path.join(FW, m + ".prx")
        if not os.path.isfile(prx):
            flash, rpcs3_exe = rpcs3_firmware(port_paths.get("rpcs3"))
            src = os.path.join(flash, "sys", "external", m + ".sprx")
            dst = os.path.join(FW, m + ".sprx")
            shutil.copy2(src, dst)
            # RPCS3 is single-instance: one file per invocation, nothing else running
            subprocess.run([rpcs3_exe, "--decrypt", dst], timeout=120)
            for cand in (os.path.join(FW, m + ".prx"), os.path.join(FW, m + ".elf")):
                if os.path.isfile(cand) and cand != prx:
                    os.replace(cand, prx)
        if not os.path.isfile(prx):
            raise StepError(f"decrypting {m}.sprx produced no .prx in {FW}")
        h = sha256(prx)
        print(f"  {m}.prx  {'matches the tested 4.93 module' if h == TESTED_FW_PRX_SHA256[m] else 'DIFFERS from tested: ' + h}")


OUR_PORT_SRC = os.path.join(ROOT, "port_src")          # new files we add to the port
CHEATS = os.path.join(WORK, "cheats")                  # the cheats' title side: the working copy
if not os.path.isfile(os.path.join(CHEATS, "cheats.cpp")):   # ... or the published one (cheats/)
    CHEATS = os.path.join(ROOT, "cheats")
# PORT_CHEATS_DIR: another cheats folder (cheats.cpp + hooks.txt + patches.txt), e.g. a project
# worktree's cheats/ (an agent branch), so a build of it leaves work/cheats alone.
if os.environ.get("PORT_CHEATS_DIR"):
    CHEATS = os.path.abspath(os.environ["PORT_CHEATS_DIR"])
    if not os.path.isfile(os.path.join(CHEATS, "cheats.cpp")):
        sys.exit("PORT_CHEATS_DIR: no cheats.cpp in " + CHEATS)
PORT_PATCHES = os.path.join(ROOT, "patches", "port")   # our edits to the port base's files


def step_port():
    """Pristine port base files (base/infamous2-lle), then our files, then our patches, so
    work/port always equals the port base + port_src + patches/port."""
    os.makedirs(os.path.join(PORT, "tools"), exist_ok=True)
    n = 0
    pairs = [(os.path.join(PORT_SRC, f), os.path.join(PORT, f)) for f in PORT_FILES]
    tsrc = os.path.join(PORT_SRC, "tools")
    pairs += [(os.path.join(tsrc, f), os.path.join(PORT, "tools", f)) for f in sorted(os.listdir(tsrc))
              if f.endswith((".py", ".sh")) or f in TOOL_DATA]
    pairs += [(os.path.join(OUR_PORT_SRC, f), os.path.join(PORT, f)) for f in sorted(os.listdir(OUR_PORT_SRC))]
    # The in-game Cheats page's title side (cheats agent, 2026-11-09): title knowledge, so it
    # lives in work/cheats (gitignored), not port_src; patches/port 0008 builds it when present.
    pairs += [(os.path.join(CHEATS, "cheats.cpp"), os.path.join(PORT, "cheats.cpp"))]
    patched = set()
    for p in sorted(os.listdir(PORT_PATCHES)):
        if p.endswith(".patch"):
            for ln in open(os.path.join(PORT_PATCHES, p), encoding="utf-8"):
                if ln.startswith("+++ b/"):
                    patched.add(ln[6:].strip())
    for s, d in pairs:
        if not os.path.isfile(s):
            continue
        rel = os.path.relpath(d, PORT).replace("\\", "/")
        if rel not in patched and os.path.isfile(d) and open(s, "rb").read() == open(d, "rb").read():
            continue   # unchanged files keep their mtime: no needless recompile
        shutil.copy(s, d)   # a changed file gets a fresh mtime: copy2 kept the source's, and a source older
                            # than the object ninja built from another version was never recompiled
        n += 1
    print(f"  {n} port file(s) copied")
    # tools/apply_patch.py, not git: the build needs no git (byte-identical to `git apply` on these patches)
    from apply_patch import apply_patch, PatchError
    for p in sorted(os.listdir(PORT_PATCHES)):
        if p.endswith(".patch"):
            files = [ln[6:].strip() for ln in open(os.path.join(PORT_PATCHES, p), encoding="utf-8")
                     if ln.startswith("+++ b/")]
            try:
                apply_patch(os.path.join(PORT_PATCHES, p), PORT, strip=1)
            except PatchError as e:
                raise StepError(f"patch {p} does not apply: {e}")
            for f in files:   # never trust a silent apply
                src = os.path.join(PORT_SRC, f)
                if os.path.isfile(src) and open(src, "rb").read() == open(os.path.join(PORT, f), "rb").read():
                    raise StepError(f"patch {p} reported success but {f} is unchanged")
            print(f"  applied {p}")
    # The exe's icon: tools/make_icon.ps1 makes work/icon/app.ico from the player's own
    # PS3_GAME/ICON0.PNG; the port's CMakeLists compiles app.rc when it exists.
    ico = os.path.join(WORK, "icon", "app.ico")
    rc = os.path.join(PORT, "app.rc")
    if os.path.isfile(ico):
        want = '1 ICON "%s"\n' % ico.replace("\\", "/")
        if not os.path.isfile(rc) or open(rc, encoding="utf-8").read() != want:
            with open(rc, "w", encoding="utf-8") as f:
                f.write(want)
            print("  app.rc: the exe gets work/icon/app.ico")


# Linux: the rewrites are written against the MSVC CRT (_aligned_malloc, ...); the toolkit's
# msvc_compat.h supplies those names off Windows (the SPU TUs have runtime/platform on their path).
LINUX_REWRITE_PRELUDE = '#include "msvc_compat.h"   /* off Windows: the MSVC CRT names a rewrite uses */\n'


def rewrite_wrapper(path, linux=None):
    """The text of spu/zz_native_<name>/spu_recomp.c for the rewrite at <path> (linux: this host's)."""
    linux = (not IS_WIN) if linux is None else linux
    return ("/* native SPU rewrite (natives/spu, or a developer's work/rewrites/spu), built like the lifted SPU TUs */\n"
            + (LINUX_REWRITE_PRELUDE if linux else "")
            + '#include "%s"\n' % path.replace("\\", "/"))


def spu_rewrites():
    """work/rewrites/spu/*.c into the port tree, one lifted-SPU-like TU each (spu/zz_native_<name>:
    the port's CMake globs spu/*/spu_recomp.c and builds them with the SPU flags; one TU per file,
    each rewrite has its own statics), only when the lift hooked them (a lifted spu_recomp.c calls
    spu_native_entry); else those directories are removed."""
    spu = os.path.join(PORT, "spu")
    src = SPU_REWRITES
    hooked = False
    for root_, _, files in os.walk(spu):
        if "spu_recomp.c" in files and not os.path.basename(root_).startswith("zz_native_"):
            with open(os.path.join(root_, "spu_recomp.c"), encoding="utf-8", errors="replace") as f:
                if "if (spu_native_entry(ctx," in f.read():
                    hooked = True
                    break
    cs = sorted(f for f in os.listdir(src) if f.endswith(".c")) if os.path.isdir(src) else []
    if not hooked or os.environ.get("PORT_SPU_NATIVES", "1") == "0":
        cs = []
    want = {"zz_native_" + os.path.splitext(f)[0]: f for f in cs}
    for n in (os.listdir(spu) if os.path.isdir(spu) else []):
        if n.startswith("zz_native_") and n not in want:
            shutil.rmtree(os.path.join(spu, n))
    for n, f in sorted(want.items()):
        os.makedirs(os.path.join(spu, n), exist_ok=True)
        text = rewrite_wrapper(os.path.join(src, f))
        p = os.path.join(spu, n, "spu_recomp.c")
        if not os.path.isfile(p) or open(p, encoding="utf-8").read() != text:
            with open(p, "w", encoding="utf-8") as fh:
                fh.write(text)
    if cs:
        print("  native SPU rewrites:", ", ".join(cs))


def pgo_profile_names(profdata, exe=None):
    """How a profile names static functions: 'path' ('<full source path>;<name>', LLVM's
    default, every profile before PORT_PGO_NAMES) or 'base' ('<file name>;<name>'). For a
    'path' profile trained from another tree than this build's (PORT_TK, PORT_DIR), say
    so: every static function of this build then compiles without its profile.
    exe: llvm-profdata (default: the Windows LLVM folder's)."""
    exe = exe or os.path.join(port_paths.get("llvm"), "bin", "llvm-profdata.exe")
    try:
        out = subprocess.run([exe, "show", "--all-functions", profdata], capture_output=True,
                             text=True, errors="replace").stdout
    except OSError:
        return "path"
    paths = []
    for ln in out.splitlines():
        s = ln.strip()
        if s.endswith(":") and ";" in s:
            f = s[:-1].split(";", 1)[0]
            if (len(f) > 2 and f[1] == ":" and f[2] in "\\/") or (not IS_WIN and f.startswith("/")):
                paths.append(os.path.normcase(os.path.normpath(f)))
    if not paths:
        return "base"
    mine = tuple(os.path.normcase(os.path.abspath(d)) + os.sep for d in (TK, PORT))
    other = [p for p in paths if not p.startswith(mine)]
    if len(other) > len(paths) // 2:
        trees = sorted({os.sep.join(p.split(os.sep)[:5]) for p in other})
        print(f"  PGO WARNING: {os.path.basename(profdata)} names static functions by path and was trained "
              f"from {', '.join(trees[:3])}; this build compiles from {TK} and {PORT}, so {len(other)} of "
              f"the profile's {len(paths)} static-function records match nothing: those functions get NO "
              f"profile. Retrain (new profiles use file names), or build from the trained trees.")
    return "path"


def pgo_mismatch_summary(logf, start):
    """After a PGO use build: the functions whose code changed since the profile was trained
    (IR PGO's '-Wbackend-plugin' "hash mismatch ... up to N count discarded": they compile
    without profile), hottest first. Only what this build compiled (the log from `start`;
    an object ninja did not recompile keeps what it had)."""
    import re
    hits = {}
    with open(logf, encoding="utf-8", errors="replace") as f:
        f.seek(start)
        for ln in f:
            m = re.search(r"hash mismatch\) (\S+) Hash = \d+ up to (\d+) count", ln)
            if m:
                hits[m.group(1)] = max(hits.get(m.group(1), 0), int(m.group(2)))
    if not hits:
        print("  PGO: every compiled function matched its profile")
        return
    top = sorted(hits.items(), key=lambda kv: -kv[1])
    print(f"  PGO: {len(hits)} function(s) changed since the profile and compiled without it; hottest: "
          + ", ".join(f"{n} ({c:.2g})" for n, c in top[:6])
          + (" -- retrain when these are hot (new profile file name)" if top[0][1] >= 1e8 else ""))


def step_compile():
    if not IS_WIN:
        return step_compile_linux()
    spu_rewrites()
    lift_restore_times()   # (PORT_LIFT_REUSE) after spu_rewrites: its spu/zz_native_* files count too
    clamp_future_times()   # (GitHub #21/#22) before CMake: no input dated after build.ninja
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from vsenv import vs_env, rc_compiler
    env = dict(vs_env(), PS3RECOMP=TK)
    cmake = port_paths.get("cmake")     # tools-cache, config.local.json, or this PC's
    rc = rc_compiler()                  # the portable SDK's rc.exe (None: Visual Studio's, found by CMake)
    build = os.environ.get("PORT_BUILD") or os.path.join(PORT, "build")   # PORT_BUILD: a separate build tree
    toolset_check(build)
    jobs = os.environ.get("PORT_JOBS", "8")   # each lifted TU is ~150k lines: bound the RAM
    # PORT_PGO=gen | use:<profdata>: clang PGO over every C/C++ TU (lifted PPU/SPU/firmware +
    # runtime). gen = instrumented build (the port's PS3_PGO_DUMP_AT writes the counters: the
    # scripted runs end in a kill); train with tools/combat.ps1, llvm-profdata merge, then build
    # with use:<file>. Unset = the normal build (every flag below then has its old value).
    pgo = os.environ.get("PORT_PGO", "")
    # /GS- (ppu2, 2026-11-08): no stack-cookie checks in the lifted code (147 -> 128 MB, part of city +14%).
    lift_opt = os.environ.get("PORT_LIFT_OPT", "/clang:-O2;/arch:AVX2;/GS-")
    # The lifted SPU modules' extra flags. PORT_SPU_ARCH="/arch:AVX2;-DSPU_FLOAT_RZ_CHECK"
    # = the SPU_FLOAT_RZ check build (every native float op checked in game; slow).
    # Default (edge agent, 2026-11-08): LS fast path (LS = ctx->ls_store, no probe
    # checks; a raw SPU aborts with a message -- none seen in this game) and RZ-only
    # floats (no exact-emulation fallback; SPU_FLOAT_RZ=0 is ignored). With the
    # toolkit's x86 integer ops: SPU jobs -30%, gameplay 86 -> 103 fps, bit-exact
    # on every recorded job. PORT_SPU_ARCH="/arch:AVX2" = the previous build.
    spu_arch = os.environ.get("PORT_SPU_ARCH", "/arch:AVX2;-DSPU_LS_FASTPATH=1;-DSPU_FLOAT_RZ_ONLY=1")
    c_rel, cxx_rel = "/O2 /Ob2 /DNDEBUG /Z7", "/O2 /Ob2 /DNDEBUG"
    link_rel = "/INCREMENTAL:NO /DEBUG /OPT:REF /OPT:ICF"
    # How a profile names a STATIC function (inf2-perf, 2026-11-11). LLVM's default is
    # '<full source path>;<name>', so a profile trained from one tree misses every static
    # function of a build from another, silently: m17 (wt\m17 + work\port-m17, profile
    # r6next trained from wt\next + work\port-cpuint) lost the SPU channel/DMA inlines and
    # the draw engine's helpers (dock -5%). PORT_PGO_NAMES=base (the default for new
    # profiles) = '<file name>;<name>', a profile fits a build from any tree; path = LLVM's
    # default (every profile up to inf2-r6next). A use build follows its profile's own
    # scheme unless PORT_PGO_NAMES says otherwise. Only in the flag strings: the
    # -mllvm option may occur once per command line.
    if pgo:
        names = os.environ.get("PORT_PGO_NAMES") or (pgo_profile_names(pgo[4:]) if pgo.startswith("use:") else "base")
        if names not in ("base", "path"):
            raise StepError("PORT_PGO_NAMES must be base or path, not %r" % names)
        if names == "base":
            c_rel += " -mllvm -static-func-full-module-prefix=false"; cxx_rel += " -mllvm -static-func-full-module-prefix=false"
        print(f"  PGO: static functions named by {'file name' if names == 'base' else 'full path'}")
    if pgo == "gen":
        g = "/clang:-fprofile-generate"
        lift_opt += ";" + g; spu_arch += ";" + g; c_rel += " " + g; cxx_rel += " " + g + " -DPORT_PGO_GEN"
        import glob
        rt = sorted(glob.glob(os.path.join(port_paths.get("llvm"), "lib", "clang", "*", "lib", "windows",
                                           "clang_rt.profile-x86_64.lib")))
        if not rt:
            raise StepError("PORT_PGO=gen: no clang_rt.profile-x86_64.lib in the LLVM folder")
        link_rel += ' "' + rt[-1].replace("\\", "/") + '"'
    elif pgo.startswith("use:"):
        u = "/clang:-fprofile-use=" + pgo[4:].replace("\\", "/")
        # -Wbackend-plugin stays on (inf2-perf, 2026-11-11): it is where IR PGO reports a function whose
        # code changed since the profile ("hash mismatch ... count discarded"); pgo_mismatch_summary
        # lists the hottest after the build (m17 lost spu_wrch's profile, 48.8e9 counts, unnoticed).
        w = "/clang:-Wno-profile-instr-unprofiled /clang:-Wno-profile-instr-out-of-date"
        # a player's folder may have spaces (Setup's "Optimise for this PC"): the flag strings below are
        # command-line text, so the flag is quoted there; the ;-lists are list items (CMake quotes them)
        uq = '"%s"' % u if " " in u else u
        lift_opt += ";" + u; spu_arch += ";" + u; c_rel += " " + uq + " " + w; cxx_rel += " " + uq + " " + w
    elif pgo:
        raise StepError("PORT_PGO must be gen or use:<profdata>, not %r" % pgo)
    # PORT_LIFT_LTO=thin (ppu-xfn, 2026-11-10): ThinLTO over the lifted PPU + firmware TUs, so a
    # hot call into another chunk can be inlined (with PGO: by its profile). lld-link runs the
    # back ends, PORT_JOBS at once, and caches them in the build tree (a runtime-only change
    # relinks from the cache). Unset or 0 = the normal build.
    lto = os.environ.get("PORT_LIFT_LTO", "0")
    if lto not in ("0", ""):
        if lto != "thin":
            raise StepError("PORT_LIFT_LTO must be thin or 0, not %r" % lto)
        lift_opt += ";/clang:-flto=thin"
        cache = os.path.join(build, "thinlto-cache").replace("\\", "/")
        cache = '"%s"' % cache if " " in cache else cache
        link_rel += " /opt:lldltojobs=%s /lldltocache:%s /lldltocachepolicy:cache_size_bytes=8g" % (jobs, cache)
    for cmd in ([cmake, "-S", PORT, "-B", build, "-G", "Ninja"] + (["-DCMAKE_RC_COMPILER=" + rc] if rc else []) + [
                 "-DCMAKE_MAKE_PROGRAM=" + port_paths.get("ninja"),
                 "-DCMAKE_C_COMPILER=clang-cl", "-DCMAKE_CXX_COMPILER=clang-cl",
                 "-DCMAKE_BUILD_TYPE=Release", "-DPS3RECOMP_DIR=" + TK, "-DPython3_EXECUTABLE=" + PY,
                 # M4: the Vulkan backend (shared draw engine) instead of D3D12
                 "-DPS3RECOMP_RSX_VULKAN=ON",
                 "-DPS3RECOMP_VULKAN_INCLUDE_DIR=" + os.path.join(DEPS, "Vulkan-Headers", "include"),
                 "-DPS3RECOMP_SDL2_INCLUDE_PARENT=" + os.path.join(DEPS, "sdl2-inc"),
                 "-DSDL2_DIR=" + os.path.join(DEPS, "SDL2-2.30.10", "cmake"),
                 "-Dglslang_DIR=" + os.path.join(DEPS, "glslang-install", "lib", "cmake", "glslang"),
                 # The lifted PPU + firmware TUs: -O2 and AVX2 (FMA3). At the old
                 # -O1/baseline every AltiVec vmaddfp lane was a software fmaf()
                 # call and the main thread was CPU-bound in vector math (2026-11-07).
                 # PORT_LIFT_OPT=<flags;...> overrides (e.g. "/clang:-O1" = the old build).
                 "-DPORT_LIFT_OPT=" + lift_opt,
                 "-DPORT_SPU_ARCH=" + spu_arch,   # see spu_arch above
                 # A PDB with every function's public symbol (no /Zi needed): host
                 # profiles (tools/host_sample.py) and crash addresses resolve to
                 # func_XXXXXXXX / runtime names. Same code: OPT:REF/ICF kept on.
                 "-DCMAKE_EXE_LINKER_FLAGS_RELEASE=" + link_rel,
                 # Full debug info (/Z7) for the C sources -- the lifted SPU modules and the C
                 # runtime/libs, not the huge lifted PPU .cpp chunks: static helpers (the SPU
                 # helpers kept out of line, the runtime's statics) get their own names in the
                 # PDB instead of being credited to the public symbol before them. Same code.
                 # The C++ flags are always passed (CMake's own Release default) so a PGO
                 # build's flags never linger in the cache of a later normal build.
                 "-DCMAKE_C_FLAGS_RELEASE=" + c_rel, "-DCMAKE_CXX_FLAGS_RELEASE=" + cxx_rel],
                [cmake, "--build", build, "-j", jobs, "--", "-k", "0"]):
        logf = os.path.join(LOGS, "compile%s.log" % (("-" + os.path.basename(os.environ["PORT_BUILD"])) if os.environ.get("PORT_BUILD") else ""))
        log0 = os.path.getsize(logf) if os.path.exists(logf) else 0
        with open(logf, "a", encoding="utf-8", errors="replace") as lf:
            lf.write("\n$ " + " ".join(cmd) + "\n")
            lf.flush()
            t0 = time.time()
            p = subprocess.run(cmd, cwd=PORT, stdout=lf, stderr=subprocess.STDOUT, env=env)
            lf.write(f"[exit {p.returncode} after {time.time() - t0:.1f}s]\n")
        if p.returncode != 0:
            raise StepError(f"{cmd[1]} exited {p.returncode} (python -I tools/build_errors.py work/logs/compile.log)")
        if pgo.startswith("use:") and cmd[1] == "--build":
            pgo_mismatch_summary(logf, log0)
    sdl = os.path.join(DEPS, "SDL2-2.30.10", "lib", "x64", "SDL2.dll")   # runtime DLL
    dst = os.path.join(build, "SDL2.dll")
    try:
        shutil.copy2(sdl, build)
    except PermissionError:   # a running build (e.g. a check exe in this dir) holds it open
        if not (os.path.exists(dst) and os.path.getsize(dst) == os.path.getsize(sdl)):
            raise
    print("  built:", os.path.join(build, "Infamous2Recomp.exe"))


# ---------------------------------------------------------------------------------------------
# Linux: the same build in GNU spelling (work/crossplat/REPORT.md). clang/clang++ and lld from
# the LLVM folder (PORT_LLVM, config.local.json "llvm", tools-cache/llvm; else PATH); the
# Windows step above is untouched by it.
# ---------------------------------------------------------------------------------------------
LINUX_LIFT_OPT = "-O2;-march=x86-64-v3;-fno-stack-protector"   # = /clang:-O2;/arch:AVX2;/GS-
LINUX_SPU_ARCH = "-march=x86-64-v3;-fno-stack-protector;-DSPU_LS_FASTPATH=1;-DSPU_FLOAT_RZ_ONLY=1"


def linux_tools():
    """{cc, cxx, lld, profdata, cmake, ninja}: absolute paths of the Linux build's programs."""
    llvm = port_paths.get("llvm", required=False)
    out = {}
    for key, name in (("cc", "clang"), ("cxx", "clang++"), ("lld", "ld.lld"), ("profdata", "llvm-profdata")):
        p = os.path.join(llvm, "bin", name) if llvm else shutil.which(name)
        if not p or not os.path.isfile(p):
            raise StepError(f"no {name} ({'in ' + os.path.join(llvm, 'bin') if llvm else 'on PATH'}): "
                            "set PORT_LLVM to the LLVM folder (bin/clang, bin/ld.lld)")
        out[key] = p
    out["cmake"], out["ninja"] = port_paths.get("cmake"), port_paths.get("ninja")
    return out


def linux_compile_flags(build, jobs, pgo="", lto="0", profdata=None, lift_opt=None, spu_arch=None):
    """(lift_opt, spu_arch, c_rel, cxx_rel, link_rel) for clang + lld: step_compile's flags in
    GNU spelling. pgo: "" | gen | use:<profdata>; lto: 0 | thin; lift_opt / spu_arch: else
    PORT_LIFT_OPT / PORT_SPU_ARCH, else the defaults above. Raises StepError on a bad value."""
    lift_opt = lift_opt or os.environ.get("PORT_LIFT_OPT", LINUX_LIFT_OPT)
    spu_arch = spu_arch or os.environ.get("PORT_SPU_ARCH", LINUX_SPU_ARCH)
    c_rel, cxx_rel = "-O2 -DNDEBUG -g", "-O2 -DNDEBUG"   # = /O2 /Ob2 /DNDEBUG /Z7, /O2 /Ob2 /DNDEBUG
    link_rel = "-fuse-ld=lld"   # the symbol table stays in the ELF (= /DEBUG's public symbols)
    if pgo:
        names = os.environ.get("PORT_PGO_NAMES") or (pgo_profile_names(pgo[4:], profdata) if pgo.startswith("use:") else "base")
        if names not in ("base", "path"):
            raise StepError("PORT_PGO_NAMES must be base or path, not %r" % names)
        if names == "base":
            c_rel += " -mllvm -static-func-full-module-prefix=false"; cxx_rel += " -mllvm -static-func-full-module-prefix=false"
        print(f"  PGO: static functions named by {'file name' if names == 'base' else 'full path'}")
    if pgo == "gen":
        g = "-fprofile-generate"   # the clang driver links the profile runtime itself
        lift_opt += ";" + g; spu_arch += ";" + g; c_rel += " " + g; cxx_rel += " " + g + " -DPORT_PGO_GEN"
        link_rel += " " + g
    elif pgo.startswith("use:"):
        u = "-fprofile-use=" + pgo[4:]
        w = "-Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date"
        uq = "'%s'" % u if " " in u else u   # the flag strings are shell text (ninja runs /bin/sh -c)
        lift_opt += ";" + u; spu_arch += ";" + u; c_rel += " " + uq + " " + w; cxx_rel += " " + uq + " " + w
    elif pgo:
        raise StepError("PORT_PGO must be gen or use:<profdata>, not %r" % pgo)
    if lto not in ("0", ""):
        if lto != "thin":
            raise StepError("PORT_LIFT_LTO must be thin or 0, not %r" % lto)
        lift_opt += ";-flto=thin"
        cache = os.path.join(build, "thinlto-cache")
        cache = "'%s'" % cache if " " in cache else cache
        link_rel += (" -flto=thin -Wl,--thinlto-jobs=%s -Wl,--thinlto-cache-dir=%s"
                     " -Wl,--thinlto-cache-policy=cache_size_bytes=8g" % (jobs, cache))
    return lift_opt, spu_arch, c_rel, cxx_rel, link_rel


def linux_configure_cmd(build, tools, flags, port=None, tk=None, python=None):
    """The Linux CMake configure command (tools/linux_syncheck.py runs it with a cross toolchain)."""
    lift_opt, spu_arch, c_rel, cxx_rel, link_rel = flags
    cmd = [tools["cmake"], "-S", port or PORT, "-B", build, "-G", "Ninja",
           "-DCMAKE_MAKE_PROGRAM=" + tools["ninja"],
           "-DCMAKE_C_COMPILER=" + tools["cc"], "-DCMAKE_CXX_COMPILER=" + tools["cxx"],
           "-DCMAKE_BUILD_TYPE=Release", "-DPS3RECOMP_DIR=" + (tk or TK), "-DPython3_EXECUTABLE=" + (python or PY),
           "-DPS3RECOMP_RSX_VULKAN=ON",
           "-DPS3RECOMP_VULKAN_INCLUDE_DIR=" + os.path.join(DEPS, "Vulkan-Headers", "include")]
    if os.environ.get("PORT_SDL2_DIR"):
        cmd.append("-DSDL2_DIR=" + os.environ["PORT_SDL2_DIR"])
    gl = os.environ.get("PORT_GLSLANG_DIR") or os.path.join(DEPS, "glslang-install-linux", "lib", "cmake", "glslang")
    if os.path.isdir(gl):
        cmd.append("-Dglslang_DIR=" + gl)
    return cmd + ["-DPORT_LIFT_OPT=" + lift_opt, "-DPORT_SPU_ARCH=" + spu_arch,
                  "-DCMAKE_EXE_LINKER_FLAGS_RELEASE=" + link_rel,
                  "-DCMAKE_C_FLAGS_RELEASE=" + c_rel, "-DCMAKE_CXX_FLAGS_RELEASE=" + cxx_rel]


def step_compile_linux():
    spu_rewrites()
    clamp_future_times()
    tools = linux_tools()
    env = dict(os.environ, PS3RECOMP=TK)
    build = os.environ.get("PORT_BUILD") or os.path.join(PORT, "build")
    jobs = os.environ.get("PORT_JOBS", "8")
    pgo = os.environ.get("PORT_PGO", "")
    flags = linux_compile_flags(build, jobs, pgo, os.environ.get("PORT_LIFT_LTO", "0"), tools["profdata"])
    for cmd in (linux_configure_cmd(build, tools, flags),
                [tools["cmake"], "--build", build, "-j", jobs, "--", "-k", "0"]):
        logf = os.path.join(LOGS, "compile%s.log" % (("-" + os.path.basename(os.environ["PORT_BUILD"])) if os.environ.get("PORT_BUILD") else ""))
        log0 = os.path.getsize(logf) if os.path.exists(logf) else 0
        os.makedirs(LOGS, exist_ok=True)
        with open(logf, "a", encoding="utf-8", errors="replace") as lf:
            lf.write("\n$ " + " ".join(cmd) + "\n")
            lf.flush()
            t0 = time.time()
            p = subprocess.run(cmd, cwd=PORT, stdout=lf, stderr=subprocess.STDOUT, env=env)
            lf.write(f"[exit {p.returncode} after {time.time() - t0:.1f}s]\n")
        if p.returncode != 0:
            raise StepError(f"{cmd[1]} exited {p.returncode} (python -I tools/build_errors.py {logf})")
        if pgo.startswith("use:") and cmd[1] == "--build":
            pgo_mismatch_summary(logf, log0)
    print("  built:", os.path.join(build, "Infamous2Recomp"))


# The three SPU code bodies libsre's SPURS kernel DMAs into local store (system service,
# taskset policy module, ...): offset in firmware 4.93's module image, size, the rest of the
# --body spec (LS address, entry, ...), and the SHA-256 of their bytes there. Other firmware
# versions put them elsewhere (an older 4.9x libsre has them 0x100 earlier, GitHub issue #1),
# and a lift registered at the wrong address never matches what the kernel loads, so they
# ran on the SPU interpreter. Found by content instead; not found = interpreter, as before.
LIBSRE_BODIES = (
    (0x21480, 0x2200, "0xA00:0xA00:0x2110", "fbe18b5448c86bda5545b387b55de37022d6992317d785a526f38a5b4c7cf81a"),
    (0x23680, 0x1E40, "0xA00:0xA00:0x1C80", "c514efddd5d2a9a5f4e5c206deda3698c7726fd564762da33f49738ca3592cf2"),
    (0x2A180, 0x3DA0, "0xA00:0xA00:0x3D00", "b4bc8e292ee05064e3edd7111d2aeb20bff4c76bde82a1e148d2efb7d0795539"),
)


def libsre_body_args():
    with open(os.path.join(PORT, "lle", "libsre", "meta", "libsre_image.bin"), "rb") as f:
        img = f.read()
    args = []
    for off, size, rest, sha in LIBSRE_BODIES:
        found = None
        # nearest first: SPU code is 16-byte aligned; a shifted module finds it in a few steps
        for k in range(0, len(img) // 0x10 + 1):
            for o in ((off,) if k == 0 else (off - k * 0x10, off + k * 0x10)):
                if 0 <= o <= len(img) - size and hashlib.sha256(img[o:o + size]).hexdigest() == sha:
                    found = o
                    break
            if found is not None or (off - k * 0x10 < 0 and off + k * 0x10 > len(img) - size):
                break
        if found is None:
            print(f"  libsre SPU body {off:#x}: not in this firmware's libsre, it runs on the SPU "
                  "interpreter (slower; firmware 4.93 is the tested one)")
            continue
        if found != off:
            print(f"  libsre SPU body {off:#x}: at {found:#x} in this firmware's libsre")
        args += ["--body", f"{found:#x}:{size:#x}:{rest}"]
    return args


def step_firmware():
    fresh("lle", "lle_spu")
    bases = {"liblv2": "0x30000000", "libsysmodule": "0x30180000", "libsre": "0x30100000"}
    for m in FIRMWARE:
        run("firmware", [PY, T("lift_firmware_module.py"), os.path.join(FW, m + ".prx"),
                         "--base", bases[m], "--out", os.path.join("lle", m)])
    run("firmware", [PY, T("lift_firmware_spu.py"), "--module-dir", "lle/libsre",
                     "--base", "0x30100000", "--name", "libsre", "--first-image-id", "100"]
        + libsre_body_args() + ["--out", "lle_spu"])


def step_ppu():
    fresh("recompiled")
    os.makedirs(os.path.join(PORT, "out"), exist_ok=True)
    run("ppu", [PY, T("ppu_loader.py"), ELF, "-o", "out/"])
    # The EBOOT lift's codegen (main agent, 2026-11-08; toolkit ppu_lifter.py, a lifter
    # without them ignores the variables): guest registers as C locals with liveness-based
    # write-backs, the memory gates in a per-function local, no trampoline drain after calls
    # that cannot leave one, CR SO bits left 0 (this EBOOT never sets XER[SO]). Main-thread
    # lifted code -26% ms/frame, +11% fps over the run (same-session A/B, twice; a
    # PPU_NV_VERIFY lift ran clean). Any of them =0 in the environment restores the old
    # form; PORT_LIFT_FAST=0 turns all four off.
    fast = {} if os.environ.get("PORT_LIFT_FAST", "1") == "0" else {
        "PPU_NV_LOCALS": "1", "PPU_MM": "1", "PPU_DRAIN_ELIDE": "1", "PPU_SO_ZERO": "1",
        # ppu2 (2026-11-08): AltiVec on SSE/AVX2 + one NaN test per FP op (city walk +14%);
        # ppu3 (2026-11-09): plain SSE when no lane is denormal/NaN + lean store check (opening +4.5..8%).
        "PPU_VMX_SSE": "1", "PPU_FP_ONE_NAN": "1", "PPU_VMX_FAST": "1", "PPU_ST_LEAN": "1",
        # ppu-xfn (2026-11-10): direct tail jumps as real tail calls (musttail: no trampoline round
        # trip, and calls to them lose their drain) + small frameless leaves lifted inline at their
        # call sites (PPU_XINLINE_MAX=<n> statements, default 48); the trampoline pointer declared
        # constinit, so clang-cl drops its TLS init guard from every drain test.
        "PPU_XTAIL": "1", "PPU_XINLINE": "1", "PPU_TLS_CONSTINIT": "1"}
    # The main thread's SPU-job poll loops (wake agent, 2026-11-08; toolkit ppu_lifter.py
    # PPU_WAKE_HOOKS + runtime/syscalls/ppu_wake.c, run-time switch PS3_PPU_WAKE; a lifter
    # without the feature ignores the variable). Checks func_0054C730 / func_0054CE30 read
    # the 16-byte job-completion row at r4 / r9; func_0029A8EC's list scan never sleeps:
    # 0x29A9B4 = it handles a finished entry, 0x29AB44 = the end of one pass.
    # Off by default (2026-11-08: no city gain, light views -8..-10%; agent/wake not merged).
    # PORT_WAKE_HOOKS=1 = the hooks (needs a toolkit with agent/wake).
    if os.environ.get("PORT_WAKE_HOOKS", "0") != "0":
        fast["PPU_WAKE_HOOKS"] = ("note:0054C744:r4:16,note:0054CEB8:r9:16,"
                                  "progress:0029A9B4,pass:0029AB44")
    # The cheats' hook slots and instruction alternatives (toolkit agent/cheats; a lifter
    # without the feature ignores the variables): empty slots cost a load and a never-taken
    # branch. PORT_CHEATS=0 = a lift without them (the Cheats page then says so).
    if os.environ.get("PORT_CHEATS", "1") != "0":
        for var, f in (("PPU_HOOK_FILE", "hooks.txt"), ("PPU_PATCH_FILE", "patches.txt")):
            if os.path.isfile(os.path.join(CHEATS, f)):
                fast[var] = os.path.join(CHEATS, f)
    # ppu-vmx (2026-11-10; toolkit tools/ppu_vmx_flow.py, a lifter without it ignores them): VMX
    # float ops test only the operands that may hold a denormal, integer AltiVec ops on SSE,
    # single FP ops of single values in single precision, fctiw/fctid as one conversion. Each
    # =0 in the environment = the old form.
    if fast:
        fast.update({"PPU_VMX_CLEAN": "1", "PPU_VMX_INT": "1", "PPU_FP_SINGLE": "1", "PPU_FP_CVT": "1"})
    # ppu-mem (2026-11-10; toolkit tools/ppu_fastpath.py, a lifter without it ignores them): a
    # store tests one byte of a page map instead of the range + its line's reservation bit twice
    # (PPU_ST_PMAP), the guest accesses are single MOVBE accesses without the alignment test
    # (PPU_MOVBE), loads of TOC entries are their ELF values under a per-entry run-time guard
    # (PPU_TOC_FOLD). Each =0 in the environment = the old form; at run time PS3_ST_PMAP=0 /
    # PS3_TOC_FOLD=0 turn the page map / the folding off in the same exe.
    if fast:
        fast.update({"PPU_ST_PMAP": "1", "PPU_MOVBE": "1", "PPU_TOC_FOLD": "1"})
    run("ppu", [PY, T("ppu_lifter.py"), ELF,
                "--functions", "out/EBOOT.functions.json", "--hle-stubs", "out/EBOOT.imports.json",
                "--code-end", "0x742e18", "--max-chunk-lines", "150000",
                # PORT_LIFT_JOBS: the lifter's worker processes (default: every CPU; a shared box,
                # e.g. the Linux test server, keeps it to a few)
                "--jobs", os.environ.get("PORT_LIFT_JOBS") or str(os.cpu_count() or 8),
                "--output", "recompiled/"], env_defaults=fast)


def step_spu():
    fresh("spu")
    os.makedirs(os.path.join(PORT, "out"), exist_ok=True)
    run("spu", [PY, T("extract_spu_images.py"), ELF, "-o", "spu/images"])
    # the Wws_Job policy module (guest 0x8E5B00, 11904 bytes, file offset 0x8D5B00)
    with open(ELF, "rb") as f:
        f.seek(0x8D5B00)
        blob = f.read(11904)
    with open(os.path.join(PORT, "out", "wwsjob_pm.bin"), "wb") as f:
        f.write(blob)
    run("spu", [PY, T("wrap_spu_elf.py"), "out/wwsjob_pm.bin", "--base", "0xA00",
                "--entry", "0xA00", "--out", "spu/images/wwsjob_pm_at_008E5B00.elf"])
    run("spu", [PY, T("build_spu_workloads.py"), "--images", "spu/images", "--lifted", "spu",
                "--out", "spu/spu_workloads.c", "--register-fn", "infamous2_spu_register_all",
                "--constructor", "--title", "infamous2", "--eboot", ELF,
                "--extra-funcs", "wwsjob_pm_at_008E5B00=0xA2C",
                "--extra-funcs", "spu_0000_at_007FA100=" + ",".join(SPU_IMAGE_EXTRA)])


def step_jobs():
    spu = os.path.join(PORT, "spu")
    for n in os.listdir(spu) if os.path.isdir(spu) else []:
        p = os.path.join(spu, n)
        if n.startswith(("jobmod_", "datamod_", "relocmod_")) or n == "jobmods":
            shutil.rmtree(p) if os.path.isdir(p) else os.remove(p)
        elif n in ("jobmods_register.c", "datamods_register.c", "relocmods_register.c", "jobmods_manifest.json"):
            os.remove(p)
    tools = os.path.join(PORT, "tools")
    run("jobs", [PY, os.path.join(tools, "extract_eboot_jobmods.py"), ELF, "--out", "spu/jobmods"])
    run("jobs", [PY, T("lift_jobmods.py"), "--jobmods", "spu/jobmods", "--out", "spu",
                 "--register", "spu/jobmods_register.c", "--symbol-prefix", "infamous2"])
    for mod, extra in JOBMOD_EXTRA:
        run("jobs", [PY, os.path.join(tools, "relift_jobmod.py"), mod, "--extra", ",".join(extra),
                     "--jobmods", "spu/jobmods", "--out", "spu"])
    run("jobs", [PY, os.path.join(tools, "lift_datamods.py"), ELF, "--out", "spu", "--extra", "0x6C44,0x6C64"])
    run("jobs", [PY, os.path.join(tools, "lift_relocmods.py"), ELF, "--out", "spu"])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only")
    ap.add_argument("--from", dest="start")
    a = ap.parse_args()
    steps = list(STEPS)
    if a.start:
        steps = steps[steps.index(a.start):]
    if a.only:
        steps = [s for s in steps if s in a.only.split(",")]
    if not REUSE and os.path.isdir(HELD):   # held by a PORT_LIFT_REUSE run that stopped: not this build's
        shutil.rmtree(HELD, ignore_errors=True)
    for s in steps:
        t0 = time.time()
        print(f"[{s}]", flush=True)
        try:
            if not lift_skip(s):
                globals()["step_" + s]()
                lift_done(s)
        except StepError as e:
            print(f"  FAILED: {e}")
            sys.exit(1)
        print(f"  done in {time.time() - t0:.1f}s", flush=True)
    lift_restore_times()   # (lift steps run without the compile step)


if __name__ == "__main__":
    main()
