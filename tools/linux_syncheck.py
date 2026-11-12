"""Linux cross syntax check (about 2 minutes, no Linux machine): every engine and game-side
translation unit compiled with clang -fsyntax-only for x86_64-linux-gnu against the Ubuntu
22.04 sysroot (work/crossplat/sysroot: the home server's /usr/include + GCC 11 headers), with
the compile commands of the port's own CMake configured the way tools/rebuild.py configures it
on Linux (rebuild_win.linux_configure_cmd), plus a cross toolchain file.

    python -I tools/linux_syncheck.py [<engine>] [--port <dir>] [--patches <dir>] [--cheats <dir>]
                                      [--ppu N|all] [--jobs N] [--out <dir>] [--toolkit-only]

<engine>      the engine tree to check (default PORT_TK, else this checkout's engine/; any other
              engine tree works too: an old toolkit worktree, another checkout's engine/)
--port        a lifted port tree: recompiled/ppu_recomp.h, spu/, lle/, lle_spu/ (default PORT_DIR,
              else work/port). Only read: <out>/port-src links to its lifted folders, and the
              port's own sources are made there as rebuild_win.py's port step makes them
              (base/ + port_src + cheats + every patch in --patches, default patches/port).
--ppu         lifted PPU chunks to check: N spread over the lift (default 2), all, or 0
--toolkit-only  the engine's own CMake alone (no lifted tree needed; the 4 scaffold files that
              include the lift's ppu_recomp.h and the engine's tests are then skipped)
--sysroot     default PORT_LINUX_SYSROOT, else work/crossplat/sysroot

Checks: (1) every TU compiles for Linux (each TU's own warning flags: only errors count, clang's
default-error ones included); (2) flags: every lifted SPU TU has -ffp-contract=off (bit-exact SPU
math with FMA hardware) and the PORT_SPU_ARCH flags, every lifted PPU / firmware TU has
-ffp-contract=off and the PORT_LIFT_OPT flags; (3) the HLE NID table: every function
tools/gen_hle_nids.py registers (it reads libs/*/*.c as text, #ifdef or not) is defined in its
file as Linux preprocesses it, or the link fails. Exit 0 = all three hold.
The native SPU rewrites (spu/zz_native_*) are checked with the wrapper a Linux build writes.
Run from a project worktree without deps/, it takes deps/ and work/ (gitignored) from the main
checkout and the worktree's own engine/, base/, patches/ and port_src/. On a Linux host it runs natively (no
toolchain file, the system's headers).
Logs: <out>/configure.log, <out>/syncheck_errors.txt (default out: work/crossplat/syncheck/<tk>).
e.g.  python -I tools/linux_syncheck.py --port work/port-linuxcheck   (~10 s here)
"""
import argparse
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
ROOT = os.path.dirname(HERE)
import port_paths    # noqa: E402
import rebuild_win   # noqa: E402  (the Linux configure command and the port step)

IS_WIN = os.name == "nt"
LIFT_DIRS = ("recompiled", "spu", "lle", "lle_spu")


def main_checkout():
    r = subprocess.run(["git", "-C", ROOT, "rev-parse", "--path-format=absolute", "--git-common-dir"],
                       capture_output=True, text=True)
    common = r.stdout.strip() if r.returncode == 0 else ""
    return os.path.dirname(os.path.normpath(common)) if common else ROOT


# deps/ and work/ are not in git: run from a project worktree that has no deps/, this script takes
# them from the main checkout (engine/, base/, patches/ and port_src/ are the worktree's own).
BASE = ROOT if os.path.isdir(os.path.join(ROOT, "deps")) else main_checkout()
if BASE != ROOT:
    rebuild_win.WORK = os.path.join(BASE, "work")
    rebuild_win.DEPS = os.path.join(BASE, "deps")
    if not os.environ.get("PORT_CHEATS_DIR") and os.path.isfile(os.path.join(BASE, "work", "cheats", "cheats.cpp")):
        rebuild_win.CHEATS = os.path.join(BASE, "work", "cheats")
SCAFFOLD = ("ppu_loader.cpp", "ppu_hle.cpp", "ppu_fs.cpp", "ppu_sysprx.cpp")


def norm(p):
    return os.path.normcase(os.path.normpath(os.path.abspath(p)))


def under(p, d):
    p, d = norm(p), norm(d)
    return p == d or p.startswith(d.rstrip(os.sep) + os.sep)


def fwd(p):
    return p.replace("\\", "/")


def toolchain_file(out, cc, cxx, sysroot):
    path = os.path.join(out, "linux-x64.cmake")
    text = ("set(CMAKE_SYSTEM_NAME Linux)\nset(CMAKE_SYSTEM_PROCESSOR x86_64)\n"
            f'set(CMAKE_C_COMPILER "{fwd(cc)}")\nset(CMAKE_CXX_COMPILER "{fwd(cxx)}")\n'
            "set(CMAKE_C_COMPILER_TARGET x86_64-linux-gnu)\nset(CMAKE_CXX_COMPILER_TARGET x86_64-linux-gnu)\n"
            f'set(CMAKE_SYSROOT "{fwd(sysroot)}")\n'
            "set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)\n"
            "set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)\n")
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    return path


def sdl2_config(out):
    """An SDL2 CMake config over the pinned headers (deps/sdl2-inc): the sysroot has none."""
    d = os.path.join(out, "sdl2cfg")
    os.makedirs(d, exist_ok=True)
    inc = fwd(os.path.join(BASE, "deps", "sdl2-inc"))
    with open(os.path.join(d, "sdl2-config.cmake"), "w", encoding="utf-8") as f:
        f.write(f'set(SDL2_FOUND TRUE)\nset(SDL2_INCLUDE_DIRS "{inc};{inc}/SDL2")\nset(SDL2_LIBRARIES SDL2)\n'
                "if(NOT TARGET SDL2::SDL2)\n  add_library(SDL2::SDL2 INTERFACE IMPORTED)\n"
                '  set_target_properties(SDL2::SDL2 PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${SDL2_INCLUDE_DIRS}")\n'
                "endif()\n")
    return d


def link_dir(link, target):
    """<link> -> <target> (a junction on Windows, needs no privilege; a symlink elsewhere)."""
    if os.path.lexists(link):
        if os.path.exists(link) and norm(os.path.realpath(link)) == norm(os.path.realpath(target)):
            return
        try:
            os.rmdir(link) if IS_WIN else os.unlink(link)   # a junction / link only, never a real folder's files
        except OSError as e:
            sys.exit(f"linux_syncheck: {link} is a real folder, not a link ({e}); remove it or use another --out")
    if IS_WIN:
        import _winapi
        _winapi.CreateJunction(os.path.abspath(target), os.path.abspath(link))
    else:
        os.symlink(os.path.abspath(target), link)


def port_overlay(out, port, patches, cheats):
    """<out>/port-src: the port's sources as rebuild_win.step_port makes them, the lift linked in."""
    src = os.path.join(out, "port-src")
    os.makedirs(src, exist_ok=True)
    rebuild_win.PORT, rebuild_win.PORT_PATCHES = src, patches
    if cheats:
        rebuild_win.CHEATS = cheats
    import contextlib
    import io
    buf = io.StringIO()
    try:
        with contextlib.redirect_stdout(buf):
            rebuild_win.step_port()
    except rebuild_win.StepError as e:
        sys.exit(f"linux_syncheck: port step: {e}")
    applied = [ln.split()[-1] for ln in buf.getvalue().splitlines() if ln.strip().startswith("applied ")]
    # The links live only while this runs: a recursive delete of <out> that follows a junction
    # (Windows PowerShell's Remove-Item -Recurse does) would empty the lifted tree behind it.
    import atexit
    atexit.register(unlink_lift, src)
    for d in LIFT_DIRS:
        if os.path.isdir(os.path.join(port, d)):
            link_dir(os.path.join(src, d), os.path.join(port, d))
    return src, applied


def unlink_lift(src):
    for d in LIFT_DIRS:
        p = os.path.join(src, d)
        if os.path.lexists(p):
            try:
                os.rmdir(p) if IS_WIN else os.unlink(p)   # the link itself; a real non-empty folder stays
            except OSError:
                pass


def run_logged(cmd, log, cwd=None):
    with open(log, "w", encoding="utf-8", errors="replace") as lf:
        lf.write("$ " + subprocess.list2cmdline(cmd) + "\n")
        lf.flush()
        return subprocess.run(cmd, cwd=cwd, stdout=lf, stderr=subprocess.STDOUT).returncode


def syntax_cmd(command):
    """The compile command as -fsyntax-only: no object, no dependency file. Its own warning flags
    stay: -Wno-everything would also silence clang's default-error diagnostics -- a C call to an
    undeclared function (_fseeki64, _aligned_malloc) is an error in a real build and a link
    failure after it, and with -Wno-everything it passed."""
    c = re.sub(r"\s-o\s+\S+", " ", command)
    c = re.sub(r"\s-M[DT]\s+\S+\.o\b", " ", c)
    c = re.sub(r"\s-MF\s+\S+", " ", c)
    c = re.sub(r"\s-MD\b", " ", c)
    c = re.sub(r"\s-c\s+", " ", c)
    return c + " -fsyntax-only -ferror-limit=50"


def hle_table_check(tk, out, cc_json, jobs_n):
    """(3) The HLE NID table: tools/gen_hle_nids.py --all (the port's CMake runs it at build time)
    registers every non-static cell*/sys*/sce* function it READS in libs/*/*.c -- #ifdef or not --
    and the exe links the table, so each one must be defined in its file as Linux compiles it.
    -> (registered, [names missing for Linux]) or (0, [why the check could not run])."""
    gen = os.path.join(out, "ppu_hle_nids.cpp")
    p = subprocess.run([sys.executable, "-I", os.path.join(tk, "tools", "gen_hle_nids.py"), "--out", gen, "--all"],
                       capture_output=True, text=True, errors="replace")
    if p.returncode:
        return 0, ["gen_hle_nids.py failed: " + (p.stderr or p.stdout)[-300:]]
    names = set(re.findall(r'ps3_hle_register(?:_ctx)?\(0x[0-9A-Fa-f]+u?,\s*"([^"]+)"',
                           open(gen, encoding="utf-8", errors="replace").read()))
    libs = [e for e in cc_json if under(e["file"], os.path.join(tk, "libs")) and e["file"].endswith(".c")
            and re.match(r"^(cell|sce|sys)", os.path.basename(e["file"]))]

    def defined(e):
        c = syntax_cmd(e["command"]).replace(" -fsyntax-only", " -E -P -w")
        r = subprocess.run(c, cwd=e["directory"], shell=not IS_WIN, capture_output=True, text=True, errors="replace")
        return set(re.findall(r"\b([A-Za-z_]\w*)\s*\((?:[^;{}()]|\([^()]*\))*\)\s*\{", r.stdout))

    with ThreadPoolExecutor(jobs_n) as ex:
        have = set().union(*ex.map(defined, libs)) if libs else set()
    return len(names), sorted(names - have)


def tokens(command):
    return [t.strip('"') for t in shlex.split(command, posix=not IS_WIN)]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("toolkit", nargs="?", default=os.environ.get("PORT_TK") or os.path.join(ROOT, "engine"))
    ap.add_argument("--port", default=os.environ.get("PORT_DIR") or os.path.join(BASE, "work", "port"))
    ap.add_argument("--patches", default=os.path.join(ROOT, "patches", "port"))
    ap.add_argument("--cheats", help="a cheats folder (cheats.cpp); default as rebuild_win.py")
    ap.add_argument("--ppu", default="2")
    ap.add_argument("--jobs", type=int, default=min(8, os.cpu_count() or 4))
    ap.add_argument("--out")
    ap.add_argument("--sysroot", default=os.environ.get("PORT_LINUX_SYSROOT") or
                    os.path.join(BASE, "work", "crossplat", "sysroot"))
    ap.add_argument("--toolkit-only", action="store_true")
    a = ap.parse_args()
    t_start = time.time()
    tk = os.path.abspath(a.toolkit)
    port = os.path.abspath(a.port)
    if not os.path.isfile(os.path.join(tk, "runtime", "ppu", "ppu_loader.cpp")):
        sys.exit(f"linux_syncheck: {tk} is not an engine tree")
    game = not a.toolkit_only and os.path.isfile(os.path.join(port, "recompiled", "ppu_recomp.h"))
    if not a.toolkit_only and not game:
        print(f"no lifted tree at {port} (recompiled/ppu_recomp.h): toolkit only")
    out = os.path.abspath(a.out or os.path.join(BASE, "work", "crossplat", "syncheck", os.path.basename(tk.rstrip("\\/"))))
    os.makedirs(out, exist_ok=True)

    # the compilers: Windows -> the LLVM folder's clang for x86_64-linux-gnu + the sysroot
    if IS_WIN:
        llvm = port_paths.get("llvm")
        cc, cxx = os.path.join(llvm, "bin", "clang.exe"), os.path.join(llvm, "bin", "clang++.exe")
        if not os.path.isdir(os.path.join(a.sysroot, "usr", "include")):
            sys.exit(f"linux_syncheck: no sysroot at {a.sysroot} (usr/include)")
        tc = toolchain_file(out, cc, cxx, a.sysroot)
    else:
        t = rebuild_win.linux_tools()
        cc, cxx, tc = t["cc"], t["cxx"], None
    tools = {"cc": cc, "cxx": cxx, "cmake": port_paths.get("cmake"), "ninja": port_paths.get("ninja")}
    build = os.path.join(out, "build-game" if game else "build-tk")
    flags = rebuild_win.linux_compile_flags(build, str(a.jobs), "", "0",
                                            lift_opt=rebuild_win.LINUX_LIFT_OPT, spu_arch=rebuild_win.LINUX_SPU_ARCH)
    extra = ["-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"]
    if tc:
        extra += ["-DCMAKE_TOOLCHAIN_FILE=" + fwd(tc), "-DSDL2_DIR=" + fwd(sdl2_config(out)),
                  "-Dglslang_DIR=" + fwd(os.environ.get("PORT_GLSLANG_DIR") or
                                         os.path.join(BASE, "deps", "glslang-install", "lib", "cmake", "glslang"))]
    applied = []
    if game:
        src, applied = port_overlay(out, port, os.path.abspath(a.patches), a.cheats and os.path.abspath(a.cheats))
        cmd = rebuild_win.linux_configure_cmd(build, tools, flags, port=src, tk=tk, python=sys.executable)
        cmd = [c for c in cmd if not (tc and c.startswith(("-DSDL2_DIR=", "-Dglslang_DIR=")))] + extra
    else:
        src = None
        cmd = [tools["cmake"], "-S", tk, "-B", build, "-G", "Ninja", "-DCMAKE_MAKE_PROGRAM=" + tools["ninja"],
               "-DCMAKE_BUILD_TYPE=Release", "-DPS3RECOMP_RSX_VULKAN=ON", "-DPS3RECOMP_BUILD_TESTS=OFF",
               "-DPS3RECOMP_VULKAN_INCLUDE_DIR=" + os.path.join(BASE, "deps", "Vulkan-Headers", "include"),
               "-DPython3_EXECUTABLE=" + sys.executable] + extra
        if not tc:
            cmd += ["-DCMAKE_C_COMPILER=" + cc, "-DCMAKE_CXX_COMPILER=" + cxx]
    # a fresh cache: the cached compiler / flags of an older configure never linger
    shutil.rmtree(os.path.join(build, "CMakeFiles"), ignore_errors=True)
    if os.path.exists(os.path.join(build, "CMakeCache.txt")):
        os.remove(os.path.join(build, "CMakeCache.txt"))
    clog = os.path.join(out, "configure.log")
    rc = run_logged(cmd, clog)
    t_conf = time.time() - t_start
    if rc != 0:
        print(f"CONFIGURE FAILED (exit {rc}); the end of {clog}:")
        print("".join(open(clog, encoding="utf-8", errors="replace").readlines()[-25:]))
        sys.exit(1)
    cc_json = json.load(open(os.path.join(build, "compile_commands.json"), encoding="utf-8"))

    # what to check
    jobs, skipped, ppu = [], [], []
    for e in cc_json:
        f = os.path.join(e["directory"], e["file"]) if not os.path.isabs(e["file"]) else e["file"]
        rel = None
        if under(f, build):
            skipped.append((f, "generated at build time"))
            continue
        if under(f, os.path.join(tk, "runtime", "ppu", "tests")) or under(f, os.path.join(tk, "tests")):
            skipped.append((f, "toolkit test (needs a generated header)"))
            continue
        if src and under(f, os.path.join(src, "recompiled")):
            ppu.append((f, e))
            continue
        if src and (under(f, os.path.join(src, "spu")) or under(f, os.path.join(src, "lle_spu"))
                    or f.endswith("spu_dispatch_mt.c") and not under(f, tk + os.sep + "libs")):
            kind = "spu"
        elif src and under(f, os.path.join(src, "lle")):
            kind = "lle"
        elif src and under(f, src):
            kind = "port"
        elif under(f, tk):
            kind = "scaffold" if os.path.basename(f) in SCAFFOLD and under(f, os.path.join(tk, "runtime", "ppu")) else "toolkit"
            if kind == "scaffold" and not (src and fwd(os.path.join(src, "recompiled")).lower() in fwd(e["command"]).lower()):
                # toolkit-only, or the toolkit's own boot smoke title (a ppu_recomp.h made at build time)
                skipped.append((f, "includes a lift's ppu_recomp.h"))
                continue
        else:
            kind = "other"
        jobs.append((kind, f, e))
    ppu.sort(key=lambda x: x[0])
    if a.ppu == "all":
        pick = ppu
    else:
        n = max(0, int(a.ppu))
        pick = [ppu[round(i * (len(ppu) - 1) / (n - 1))] for i in range(n)] if n > 1 and ppu else ppu[:n]
        pick = list({p[0]: p for p in pick}.values())
    jobs = [("ppu", f, e) for f, e in pick] + jobs   # the big ones first

    # The native SPU rewrites' wrappers (spu/zz_native_<name>/spu_recomp.c) in the lifted tree were
    # written by the host that lifted it; a Linux build writes its own (rebuild_win.rewrite_wrapper):
    # compile that text instead, with the same command.
    if src:
        wdir = os.path.join(out, "linux-rewrites")
        for i, (kind, f, e) in enumerate(jobs):
            d = os.path.basename(os.path.dirname(f))
            if kind != "spu" or not d.startswith("zz_native_"):
                continue
            inc = re.findall(r'^\s*#\s*include\s+"([^"]+)"', open(f, encoding="utf-8", errors="replace").read(), re.M)
            if not inc:
                continue
            nf = os.path.join(wdir, d, "spu_recomp.c")
            os.makedirs(os.path.dirname(nf), exist_ok=True)
            with open(nf, "w", encoding="utf-8") as fh:
                fh.write(rebuild_win.rewrite_wrapper(inc[-1], linux=True))
            cmd2 = e["command"]
            for v in {e["file"], fwd(e["file"]), e["file"].replace("/", "\\")}:
                cmd2 = cmd2.replace(v, fwd(nf))
            if cmd2 != e["command"]:
                jobs[i] = (kind, f, dict(e, command=cmd2))

    # (2) the flags
    flag_bad = []
    want_spu = ["-ffp-contract=off"] + [x for x in rebuild_win.LINUX_SPU_ARCH.split(";") if x]
    want_lift = ["-ffp-contract=off"] + [x for x in rebuild_win.LINUX_LIFT_OPT.split(";") if x]
    n_flag = 0
    if src:
        for kind, f, e in [("ppu", f, e) for f, e in ppu] + [j for j in jobs if j[0] in ("spu", "lle")]:
            toks = tokens(e["command"])
            want = want_spu if kind == "spu" else want_lift
            miss = [w for w in want if w not in toks]
            n_flag += 1
            if miss:
                flag_bad.append((kind, f, miss))

    def run(job):
        kind, f, e = job
        kw = {}
        if IS_WIN:
            kw["creationflags"] = 0x00004000   # BELOW_NORMAL_PRIORITY_CLASS: never slow a game
        else:
            kw["preexec_fn"] = lambda: os.nice(10)
        t0 = time.time()
        try:
            p = subprocess.run(syntax_cmd(e["command"]), cwd=e["directory"], shell=not IS_WIN,
                               capture_output=True, text=True, errors="replace", timeout=900, **kw)
            return kind, f, p.returncode, p.stderr, time.time() - t0
        except subprocess.TimeoutExpired:
            return kind, f, -1, "error: timed out after 900 s", time.time() - t0

    t1 = time.time()
    with ThreadPoolExecutor(a.jobs) as ex:
        res = list(ex.map(run, jobs))
    t_tu = time.time() - t1
    n_hle, hle_missing = hle_table_check(tk, out, cc_json, a.jobs)

    counts = {}
    for kind, *_ in res:
        counts[kind] = counts.get(kind, 0) + 1
    bad = [r for r in res if r[2] != 0]
    n_err = 0

    def short(line):
        for base, tag in ((tk, ""), (src or "\0", "port/"), (port, "port/")):
            for b in {base, fwd(base), base.replace("/", "\\")}:
                line = line.replace(b + "\\", tag).replace(b + "/", tag)
        return line

    with open(os.path.join(out, "syncheck_errors.txt"), "w", encoding="utf-8") as fh:
        fh.write(f"toolkit {tk}\nport {port}\npatches {', '.join(applied)}\n")
        for kind, f, rc_, err, _ in bad:
            fh.write(f"\n### {f}\n{err}\n")
    print(f"linux_syncheck: toolkit {tk}" + (f", lifted port {port}" if src else ""))
    if applied:
        print(f"  port patches: {applied[0].split('-')[0]}..{applied[-1].split('-')[0]} ({len(applied)}), last {applied[-1]}")
    print(f"  {len(res)} TUs (" + ", ".join(f"{k} {v}" for k, v in sorted(counts.items())) +
          f"{', lifted PPU %d of %d' % (len(pick), len(ppu)) if src else ''}), {len(skipped)} skipped; "
          f"configure {t_conf:.0f} s, compile {t_tu:.0f} s")
    for kind, f, rc_, err, _ in sorted(bad, key=lambda r: r[1]):
        errs = [ln for ln in err.splitlines() if "error:" in ln]
        n_err += max(1, len(errs))
        print(f"\n  ### {short(f)}  ({len(errs)} error(s))")
        for ln in errs[:6]:
            print("     ", short(ln)[:230])
    if src:
        if flag_bad:
            nfc = sum(1 for _, _, miss in flag_bad if "-ffp-contract=off" in miss)
            print(f"\n  FLAGS: {len(flag_bad)} of {n_flag} lifted TUs miss a flag"
                  + (f"; {nfc} have no -ffp-contract=off (SPU / PPU float math not bit exact)" if nfc else "") + ", e.g.:")
            shown = {}
            for kind, f, miss in flag_bad:
                if shown.get(kind, 0) < 2:
                    shown[kind] = shown.get(kind, 0) + 1
                    print(f"     {kind} {short(f)}: no {' '.join(miss)}")
        else:
            print(f"  flags: all {n_flag} lifted TUs have -ffp-contract=off and their PORT_LIFT_OPT / PORT_SPU_ARCH flags")
    if hle_missing:
        print(f"\n  HLE TABLE: {len(hle_missing)} of the {n_hle} functions gen_hle_nids.py registers are not defined for "
              f"Linux (an #ifdef'd cell*/sys*/sce* function: the link fails): {', '.join(hle_missing[:8])}")
    else:
        print(f"  HLE table: all {n_hle} functions gen_hle_nids.py registers are defined for Linux")
    print(f"\n  RESULT: {n_err} error(s) in {len(bad)} TU(s)" + (f", {len(flag_bad)} flag problem(s)" if src else "") +
          f", {len(hle_missing)} HLE table problem(s)  [{time.time() - t_start:.0f} s; {os.path.join(out, 'syncheck_errors.txt')}]")
    sys.exit(0 if not bad and not flag_bad and not hle_missing else 1)


if __name__ == "__main__":
    main()
