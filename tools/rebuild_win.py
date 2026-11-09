"""Windows driver for the inFAMOUS 2 lift (replaces the Mac port's rebuild.py).

Same translation steps and arguments as
community/erikomis-infamous2-recomp/infamous2-lle/tools/rebuild.py, without its
macOS host check, NPUA80638 title-ID check and .rap requirement (our copy is the
EU disc BCES01143 + v1.04 update, whose code is identical: PPU-41a7d047...).

    python -I tools/rebuild_win.py [--only step,step] [--from step]

Steps: inputs port firmware ppu spu jobs compile
Logs:  work/logs/<step>.log    Output: work/port/
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
TK = os.environ.get("PORT_TK") or os.path.join(ROOT, "toolkit")   # PORT_TK: another toolkit tree (an agent worktree)
PORT_SRC = os.path.join(ROOT, "community", "erikomis-infamous2-recomp", "infamous2-lle")
WORK = os.path.join(ROOT, "work")
PORT = os.environ.get("PORT_DIR") or os.path.join(WORK, "port")   # PORT_DIR: a separate port tree (lifter A/B)
FW = os.path.join(WORK, "fw")
LOGS = os.path.join(WORK, "logs")
ELF_SRC = os.path.join(ROOT, "eboot", "EBOOT_upd104.elf")
ELF = os.path.join(WORK, "EBOOT.elf")
PY = os.path.join(WORK, "venv", "Scripts", "python.exe")
DEPS = os.path.join(ROOT, "deps")
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
            RPCS3 = port_paths.get("rpcs3")
            src = os.path.join(RPCS3, "dev_flash", "sys", "external", m + ".sprx")
            dst = os.path.join(FW, m + ".sprx")
            shutil.copy2(src, dst)
            # RPCS3 is single-instance: one file per invocation, nothing else running
            subprocess.run([os.path.join(RPCS3, "rpcs3.exe"), "--decrypt", dst], timeout=120)
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
PORT_PATCHES = os.path.join(ROOT, "patches", "port")   # our edits to community port files


def step_port():
    """Pristine community port files, then our files, then our patches, so
    work/port always equals community + port_src + patches/port."""
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
        shutil.copy2(s, d)
        n += 1
    print(f"  {n} port file(s) copied")
    # work/ sits inside our project repo, and inside a repo `git apply` resolves
    # paths from the repo root and silently SKIPS the rest. The ceiling makes it
    # a plain patcher in work/port.
    env = dict(os.environ, GIT_CEILING_DIRECTORIES=WORK)
    for p in sorted(os.listdir(PORT_PATCHES)):
        if p.endswith(".patch"):
            files = [ln[6:].strip() for ln in open(os.path.join(PORT_PATCHES, p), encoding="utf-8")
                     if ln.startswith("+++ b/")]
            r = subprocess.run(["git", "-c", "core.autocrlf=false", "apply", "-p1", os.path.join(PORT_PATCHES, p)], cwd=PORT,
                               capture_output=True, text=True, env=env)
            for f in files:   # never trust a silent apply
                src = os.path.join(PORT_SRC, f)
                if r.returncode == 0 and os.path.isfile(src) and \
                        open(src, "rb").read() == open(os.path.join(PORT, f), "rb").read():
                    raise StepError(f"patch {p} reported success but {f} is unchanged")
            if r.returncode != 0:
                raise StepError(f"patch {p} does not apply: {r.stderr.strip()}")
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


def step_compile():
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from vsenv import vs_env
    env = dict(vs_env(), PS3RECOMP=TK)
    build = os.environ.get("PORT_BUILD") or os.path.join(PORT, "build")   # PORT_BUILD: a separate build tree
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
        w = "/clang:-Wno-profile-instr-unprofiled /clang:-Wno-profile-instr-out-of-date /clang:-Wno-backend-plugin"
        lift_opt += ";" + u; spu_arch += ";" + u; c_rel += " " + u + " " + w; cxx_rel += " " + u + " " + w
    elif pgo:
        raise StepError("PORT_PGO must be gen or use:<profdata>, not %r" % pgo)
    for cmd in (["cmake", "-S", PORT, "-B", build, "-G", "Ninja",
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
                ["cmake", "--build", build, "-j", jobs, "--", "-k", "0"]):
        with open(os.path.join(LOGS, "compile%s.log" % (("-" + os.path.basename(os.environ["PORT_BUILD"])) if os.environ.get("PORT_BUILD") else "")), "a", encoding="utf-8", errors="replace") as lf:
            lf.write("\n$ " + " ".join(cmd) + "\n")
            lf.flush()
            t0 = time.time()
            p = subprocess.run(cmd, cwd=PORT, stdout=lf, stderr=subprocess.STDOUT, env=env)
            lf.write(f"[exit {p.returncode} after {time.time() - t0:.1f}s]\n")
        if p.returncode != 0:
            raise StepError(f"{cmd[1]} exited {p.returncode} (python -I tools/build_errors.py work/logs/compile.log)")
    sdl = os.path.join(DEPS, "SDL2-2.30.10", "lib", "x64", "SDL2.dll")   # runtime DLL
    dst = os.path.join(build, "SDL2.dll")
    try:
        shutil.copy2(sdl, build)
    except PermissionError:   # a running build (e.g. a check exe in this dir) holds it open
        if not (os.path.exists(dst) and os.path.getsize(dst) == os.path.getsize(sdl)):
            raise
    print("  built:", os.path.join(build, "Infamous2Recomp.exe"))


def step_firmware():
    fresh("lle", "lle_spu")
    bases = {"liblv2": "0x30000000", "libsysmodule": "0x30180000", "libsre": "0x30100000"}
    for m in FIRMWARE:
        run("firmware", [PY, T("lift_firmware_module.py"), os.path.join(FW, m + ".prx"),
                         "--base", bases[m], "--out", os.path.join("lle", m)])
    run("firmware", [PY, T("lift_firmware_spu.py"), "--module-dir", "lle/libsre",
                     "--base", "0x30100000", "--name", "libsre", "--first-image-id", "100",
                     "--body", "0x21480:0x2200:0xA00:0xA00:0x2110",
                     "--body", "0x23680:0x1E40:0xA00:0xA00:0x1C80",
                     "--body", "0x2A180:0x3DA0:0xA00:0xA00:0x3D00",
                     "--out", "lle_spu"])


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
        "PPU_VMX_SSE": "1", "PPU_FP_ONE_NAN": "1", "PPU_VMX_FAST": "1", "PPU_ST_LEAN": "1"}
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
    run("ppu", [PY, T("ppu_lifter.py"), ELF,
                "--functions", "out/EBOOT.functions.json", "--hle-stubs", "out/EBOOT.imports.json",
                "--code-end", "0x742e18", "--max-chunk-lines", "150000",
                "--jobs", str(os.cpu_count() or 8), "--output", "recompiled/"], env_defaults=fast)


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
    for s in steps:
        t0 = time.time()
        print(f"[{s}]", flush=True)
        try:
            globals()["step_" + s]()
        except StepError as e:
            print(f"  FAILED: {e}")
            sys.exit(1)
        print(f"  done in {time.time() - t0:.1f}s", flush=True)


if __name__ == "__main__":
    main()
