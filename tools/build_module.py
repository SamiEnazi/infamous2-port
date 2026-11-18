"""Build the game module -- the lifted PPU/SPU code and the lifted firmware of a lift (PORT_DIR:
recompiled/, spu/, lle/, lle_spu/), the native SPU rewrites, spu_dispatch_mt.c and the engine's
runtime/game_module/*.c -- into Infamous2Game.dll (Windows) / libInfamous2Game.so (Linux), the
file the prebuilt engine exe binds at start (engine/include/ps3emu/ps3_module.h).

    python -I tools/build_module.py [--port DIR] [--out DIR] [--jobs N] [--pgo gen|use:FILE]
                                    [--llvm DIR] [--host windows|linux] [--msvc-env] [--keep-going]

Only clang and lld, and Python's standard library: no CMake, no Ninja, and on Windows no MSVC and
no Windows SDK (the engine's runtime/game_module/shim headers stand in for the C library headers
the lifted code includes, and the C library itself is the engine's, handed over when it binds the
module). --msvc-env compiles with Visual Studio's headers instead (the old build's, for A/B).

Flags: those the single exe gave the same files (tools/rebuild_win.py step_compile + the port's
CMakeLists), PORT_LIFT_OPT / PORT_SPU_ARCH the same switches. Incremental: an object is rebuilt
when its command or any header it read changed (<obj>.dep). (slim split, 2026-11-14)
"""
import argparse
import concurrent.futures
import glob
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IS_WIN = os.name == "nt"
MODULE_FILE = {"windows": "Infamous2Game.dll", "linux": "libInfamous2Game.so"}


def die(msg):
    print("build_module: " + msg, file=sys.stderr)
    sys.exit(1)


def find_llvm(host, llvm):
    """(cc, linker, bin dir) of the LLVM folder: clang-cl/lld-link on Windows (clang --driver-mode=cl
    and lld -flavor link when only clang.exe and lld.exe are there: the slim toolchain),
    clang/ld.lld on Linux."""
    cands = []
    if llvm:
        cands.append(llvm)
    if os.environ.get("PORT_LLVM"):
        cands.append(os.environ["PORT_LLVM"])
    try:
        sys.path.insert(0, os.path.join(ROOT, "tools"))
        import port_paths
        p = port_paths.get("llvm", required=False)
        if p:
            cands.append(p)
    except Exception:
        pass
    exe = ".exe" if IS_WIN else ""
    for d in cands:
        b = os.path.join(d, "bin")
        if host == "windows":
            ld = next((os.path.join(b, n) for n in ("lld-link" + exe, "lld" + exe) if os.path.isfile(os.path.join(b, n))), None)
            for cc in ("clang-cl" + exe, "clang" + exe):
                if os.path.isfile(os.path.join(b, cc)) and ld:
                    return os.path.join(b, cc), ld, b
        else:
            cc, ld = os.path.join(b, "clang" + exe), os.path.join(b, "ld.lld" + exe)
            if os.path.isfile(cc) and (os.path.isfile(ld) or os.path.isfile(os.path.join(b, "lld" + exe))):
                return cc, ld, b
    for cc, ld in ((("clang-cl", "lld-link") if host == "windows" else ("clang", "ld.lld")),):
        c, l = shutil.which(cc), shutil.which(ld)
        if c and l:
            return c, l, os.path.dirname(c)
    die("no clang/lld found: give --llvm <LLVM folder> (bin/clang%s, bin/%s)" %
        (exe, "lld-link.exe" if host == "windows" else "ld.lld"))


# ---- the translation units ------------------------------------------------------------------
def sources(port, tk, host):
    """[(group, path)]: ppu = lifted EBOOT + firmware PPU code (C++), spu = lifted SPU images,
    firmware SPU code, the rewrites, the registrations, spu_dispatch_mt.c (C), mod = the module's
    own side of the interface (C)."""
    out = []
    for f in sorted(glob.glob(os.path.join(port, "recompiled", "*.cpp")) + glob.glob(os.path.join(port, "recompiled", "*.c"))):
        out.append(("ppu", f))
    for f in sorted(glob.glob(os.path.join(port, "lle", "*", "*.cpp"))):
        out.append(("ppu", f))
    spu = os.path.join(port, "spu")
    for f in sorted(glob.glob(os.path.join(spu, "*", "spu_recomp.c"))):
        out.append(("spu", f))
    for g in ("spu_workloads.c", "spu_jobs.c", "jobmods_register.c", "datamods_register.c",
              "relocmods_register.c", "taskset_policy_bytes.c"):
        if os.path.isfile(os.path.join(spu, g)):
            out.append(("spu", os.path.join(spu, g)))
    for f in sorted(glob.glob(os.path.join(port, "lle_spu", "*", "spu_recomp.c")) +
                    glob.glob(os.path.join(port, "lle_spu", "*_spu_register.c"))):
        out.append(("spu", f))
    out.append(("spu", os.path.join(tk, "runtime", "spu", "spu_dispatch_mt.c")))
    gm = os.path.join(tk, "runtime", "game_module")
    out.append(("mod", os.path.join(gm, "ps3_module_data.c")))
    out.append(("mod", os.path.join(gm, "ps3_module_layout.c")))
    if host == "windows":
        out.append(("mod", os.path.join(gm, "ps3_module_crt_win.c")))
        out.append(("mod", os.path.join(gm, "ps3_module_imports.c")))
    if not any(g == "ppu" and "recompiled" in p for g, p in out):
        die("no lifted code in %s (recompiled/*.cpp): run the lift first (tools/rebuild_win.py)" % port)
    return out


def kit_version(tk):
    """The engine source the module is built from: the git commit when there is one."""
    for cmd in (["git", "-C", tk, "rev-parse", "--short=12", "HEAD"],):
        try:
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=10)
            if r.returncode == 0 and r.stdout.strip():
                dirty = subprocess.run(["git", "-C", tk, "status", "--porcelain", "--", "."], capture_output=True,
                                       text=True, timeout=20).stdout.strip()
                return r.stdout.strip() + ("+" if dirty else "")
        except (OSError, subprocess.TimeoutExpired):
            pass
    vf = os.path.join(tk, "VERSION")
    return open(vf, encoding="utf-8").read().strip() if os.path.isfile(vf) else "unknown"


# ---- flags -------------------------------------------------------------------------------------
def win_flags(group, port, tk, a):
    """clang-cl flags for a TU of the group: the single exe's (build.ninja of tools/rebuild_win.py's
    build), with the shim in place of the MSVC/SDK headers."""
    eng = lambda *p: os.path.join(tk, *p)
    inc = [os.path.join(port, "recompiled"), eng("include"), eng("runtime", "ppu"), eng("runtime", "spu"),
           eng("runtime", "memory"), eng("runtime", "syscalls"), eng("runtime", "prx"), eng("runtime", "platform"),
           eng("libs", "video"), port, eng("runtime", "platform", "win32_include")]
    if group in ("spu", "mod"):
        inc = [eng("runtime", "spu")] + inc
    if a.msvc_env:
        pre = ["/FI" + eng("runtime", "platform", "win32_posix_min.h")]
    else:
        shim = eng("runtime", "game_module", "shim", "include")
        pre = ["/X", "-I" + shim, "/FI" + os.path.join(shim, "slim_prelude.h")]
    common = ["-Wall", "-Wextra", "-Wno-unused-parameter", "-fno-strict-aliasing", "-fwrapv"]
    defs = ["-DPS3RECOMP_RSX_VULKAN", "-DSDL_MAIN_HANDLED", "-DPS3_GAME_MODULE=1"]
    if group == "ppu" and not a.no_direct:
        pre = pre + ["/FI" + eng("runtime", "game_module", "ps3_module_direct.h")]
    if group == "ppu":
        f = ["/DWIN32", "/D_WINDOWS", "/EHsc", "/O2", "/Ob2", "/DNDEBUG", "-std:c++20", "-MD"] + common
        f += ["/w", "/bigobj", "/clang:-ffp-contract=off"] + [x for x in a.lift_opt.split(";") if x]
    else:
        f = ["/DWIN32", "/D_WINDOWS", "/O2", "/Ob2", "/DNDEBUG", "/std:c17", "-MD"] + common
        f += ["-O2", "/clang:-ffp-contract=off"] + [x for x in a.spu_arch.split(";") if x] + ["/w"]
        defs += ["-DSPU_USE_DISPATCH_MT"]
        if group == "mod":
            defs += ['-DPS3_MODULE_KIT="%s"' % a.kit]
    f += pgo_flags(a, "windows")
    if a.xdata:
        pre = pre + ["/FI" + a.xdata]
        defs += ["-DPS3_MODULE_DATA_IMPORT=1"]
    return pre + f + defs + ["-I" + d for d in inc]


def linux_flags(group, port, tk, a, src=""):
    """clang flags for the Linux module: the Linux single exe's (rebuild_win.py step_compile_linux +
    the port's CMakeLists GNU branch), position-independent, hidden by default. Thread-locals are
    initial-exec (the lifted code's g_trampoline_fn, read after every lifted call, stays one load);
    the module's TLS block (~155 KB with the natives' per-thread scratch) then has to fit glibc's
    static TLS: the engine restarts itself with glibc.rtld.optional_static_tls when it does not
    (runtime/module/ps3_module_host.c)."""
    eng = lambda *p: os.path.join(tk, *p)
    inc = [os.path.join(port, "recompiled"), eng("include"), eng("runtime", "ppu"), eng("runtime", "spu"),
           eng("runtime", "memory"), eng("runtime", "syscalls"), eng("runtime", "prx"), eng("runtime", "platform"),
           eng("libs", "video"), port]
    if group in ("spu", "mod"):
        inc = [eng("runtime", "spu")] + inc
    tls = ["-ftls-model=initial-exec"]
    common = ["-Wall", "-Wextra", "-Wno-unused-parameter", "-fno-strict-aliasing", "-fwrapv",
              "-fPIC", "-fvisibility=hidden", "-fno-plt"] + tls
    defs = ["-DPS3RECOMP_RSX_VULKAN", "-DSDL_MAIN_HANDLED", "-DPS3_GAME_MODULE=1"]
    if group == "ppu":
        f = ["-x", "c++", "-std=gnu++20", "-O2", "-DNDEBUG"] + common + ["-w", "-ffp-contract=off"]
        f += [x for x in a.lift_opt.split(";") if x]
    else:
        f = ["-x", "c", "-std=gnu17", "-O2", "-DNDEBUG"] + common + ["-O2", "-ffp-contract=off"]
        f += [x for x in a.spu_arch.split(";") if x] + ["-w"]
        defs += ["-DSPU_USE_DISPATCH_MT"]
        if group == "mod":
            defs += ['-DPS3_MODULE_KIT="%s"' % a.kit]
    f += pgo_flags(a, "linux")
    if getattr(a, "xdata", None):
        f = ["-include", a.xdata] + f
        defs += ["-DPS3_MODULE_DATA_IMPORT=1"]
    return f + defs + ["-I" + d for d in inc]


def xdata_header(tk, out):
    """--data-import (a measurement build, not for players): ps3_xdata.h, force-included into every
    module TU, makes each shared variable of ps3_module.h (*ps3_x_<name>), a pointer to the ENGINE's
    storage that the engine fills -- the code a dllimport of the engine's variables would be."""
    import re
    hdr = open(os.path.join(tk, "include", "ps3emu", "ps3_module.h"), encoding="utf-8").read()
    rows = []
    for start in ("#define PS3_MODULE_VARS(R, C)", "#define PS3_MODULE_HVARS(H)"):
        body = hdr[hdr.index(start) + len(start):]
        body = body[:body.index("\n\n")] if "\n\n" in body else body
        body = body[:body.index("\n/*")] if "\n/*" in body else body
        rows += re.findall(r"\b([RCH])\(([^,]+), (\w+), ([^)]*)\)", body)
    lines = ["/* GENERATED by tools/build_module.py --data-import (a measurement build) */", "#pragma once",
             '#include <stdint.h>', '#include "ps3emu/ps3_module.h"',
             "#ifdef __cplusplus", 'extern "C" {', "#endif"]
    for kind, typ, name, dims in rows:
        if name in ("g_spu_coh_armed", "g_spu_coh_bitmap"):
            typ += " const"   # the lifted header's own declaration
        if name == "g_ppu_toc_ok":
            dims = "[]"       # ditto (an array of unknown bound there)
        lines.append("extern %s (*ps3_x_%s)%s;" % (typ, name, dims.strip()))
    lines += ["#ifdef __cplusplus", "}", "#endif"]
    lines += ["#define %s (*ps3_x_%s)" % (name, name) for _, _, name, _ in rows]
    os.makedirs(out, exist_ok=True)
    path = os.path.join(out, "ps3_xdata.h")
    text = "\n".join(lines) + "\n"
    if not os.path.isfile(path) or open(path, encoding="utf-8").read() != text:
        with open(path, "w", encoding="utf-8") as f:
            f.write(text)
    return path


def pgo_flags(a, host):
    if not a.pgo:
        return []
    cl = (lambda x: "/clang:" + x) if host == "windows" else (lambda x: x)
    names = ["-mllvm", "-static-func-full-module-prefix=false"]   # file-name scheme (rebuild_win.py PORT_PGO_NAMES=base)
    if a.pgo == "gen":
        return [cl("-fprofile-generate"), "-DPS3_MODULE_PGO_GEN=1"] + names
    if a.pgo.startswith("use:"):
        return [cl("-fprofile-use=" + a.pgo[4:].replace("\\", "/")), cl("-Wno-profile-instr-unprofiled"),
                cl("-Wno-profile-instr-out-of-date"), "-DPS3_MODULE_PGO_USE=1"] + names
    die("--pgo must be gen or use:<profdata>, not %r" % a.pgo)


# ---- incremental compile ---------------------------------------------------------------------
def obj_path(out, port, tk, group, src):
    try:
        rel = os.path.relpath(src, port)
        if rel.startswith(".."):
            raise ValueError
    except ValueError:
        rel = "engine/" + os.path.relpath(src, tk)
    rel = rel.replace("\\", "/").replace("/", "~")
    return os.path.join(out, "obj", rel + (".obj" if IS_WIN else ".o"))


def read_deps(depfile):
    """the files of a make-style .d file (clang -MD)"""
    try:
        text = open(depfile, encoding="utf-8", errors="replace").read()
    except OSError:
        return None
    text = text.replace("\\\r\n", " ").replace("\\\n", " ")
    _, _, rest = text.partition(": ")
    files, cur, i = [], "", 0
    while i < len(rest):
        c = rest[i]
        if c == "\\" and i + 1 < len(rest) and rest[i + 1] == " ":
            cur += " "; i += 2; continue
        if c.isspace():
            if cur:
                files.append(cur); cur = ""
        else:
            cur += c
        i += 1
    if cur:
        files.append(cur)
    return files


def stamp(path):
    try:
        st = os.stat(path)
        return [st.st_mtime_ns, st.st_size]
    except OSError:
        return None


# (portable install, v0.6.0) the port's folder may be moved or copied to another drive or PC: the build files keep
# no path of it. Inside ROOT a path is kept as "<root>/..." (the keys, the header lists), and found again under the
# folder's new place; a moved folder then compiles only what changed, as before the move.
def _roots():
    r = [os.path.normcase(os.path.abspath(ROOT))]
    real = os.environ.get("PORT_REAL_ROOT")   # (setup_build.ps1 builds an odd-named folder through a plain-named link)
    if real:
        r.append(os.path.normcase(os.path.abspath(real)))
    return r


def rel_root(path):
    try:
        a = os.path.normcase(os.path.abspath(path))
    except (TypeError, ValueError):
        return path
    for r in _roots():
        if a == r or a.startswith(r + os.sep):
            return "<root>/" + os.path.relpath(a, r).replace("\\", "/")
    return path


def from_root(path):
    if isinstance(path, str) and path.startswith("<root>/"):
        return os.path.join(ROOT, *path[7:].split("/"))
    return path


def key_text(s):
    """a compile flag with the port's folder written as <root> (in any of its spellings: \\ or /, any case)"""
    out = s
    for r in [ROOT] + ([os.environ["PORT_REAL_ROOT"]] if os.environ.get("PORT_REAL_ROOT") else []):
        for v in {r, r.replace("\\", "/"), r.replace("/", "\\")}:
            i = out.lower().find(v.lower())
            while i >= 0:
                out = out[:i] + "<root>" + out[i + len(v):]
                i = out.lower().find(v.lower(), i + 6)
    return out


def up_to_date(obj, key):
    meta = obj + ".dep.json"
    if not os.path.isfile(obj) or not os.path.isfile(meta):
        return False
    try:
        m = json.load(open(meta, encoding="utf-8"))
    except (OSError, ValueError):
        return False
    if m.get("key") != key:
        return False
    for p, s in m.get("deps", []):
        if stamp(from_root(p)) != s:
            return False
    return True


def compile_one(cc, flags, src, obj, host, key, verbose):
    os.makedirs(os.path.dirname(obj), exist_ok=True)
    dep = obj + ".d"
    if host == "windows":
        cmd = [cc] + (["--driver-mode=cl"] if os.path.basename(cc).lower().startswith("clang.") else [])
        cmd += flags + ["/clang:-MD", "/clang:-MF" + dep, "/c", "/Fo" + obj, src]
    else:
        cmd = [cc] + flags + ["-MD", "-MF", dep, "-c", "-o", obj, src]
    t0 = time.time()
    p = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
    dt = time.time() - t0
    if p.returncode != 0:
        try:
            os.remove(obj)
        except OSError:
            pass
        return src, dt, p.returncode, (p.stdout + p.stderr)[-6000:], cmd
    deps = read_deps(dep) or [src]
    with open(obj + ".dep.json", "w", encoding="utf-8") as f:
        json.dump({"key": key, "deps": [[rel_root(d), stamp(d)] for d in deps if stamp(d)]}, f)
    return src, dt, 0, (p.stdout + p.stderr)[-200000:], cmd


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--port", default=os.environ.get("PORT_DIR") or os.path.join(ROOT, "work", "port"))
    ap.add_argument("--engine", default=os.environ.get("PORT_TK") or os.path.join(ROOT, "engine"))
    ap.add_argument("--out", help="objects and logs (default: <port>/module)")
    ap.add_argument("--dll-dir", help="where the module file goes (default: --out), next to the engine exe")
    ap.add_argument("--jobs", type=int, default=int(os.environ.get("PORT_JOBS") or 0) or (os.cpu_count() or 4))
    ap.add_argument("--pgo", default=os.environ.get("PORT_PGO", ""))
    ap.add_argument("--llvm")
    ap.add_argument("--host", choices=("windows", "linux"), default="windows" if IS_WIN else "linux")
    ap.add_argument("--msvc-env", action="store_true", help="Visual Studio's headers instead of the shim (A/B)")
    ap.add_argument("--keep-going", action="store_true")
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--no-pdb", action="store_true", help="Windows: no PDB (crash addresses then lack names)")
    ap.add_argument("--compile-only", action="store_true")
    ap.add_argument("--no-direct", action="store_true", help="every engine call through the import stubs (A/B)")
    ap.add_argument("--data-import", action="store_true", help="measurement build: the module reaches the engine's variables through pointers")
    ap.add_argument("--only", help="compile only the units whose path holds one of these (comma list); implies --compile-only")
    a = ap.parse_args()
    a.port, a.engine = os.path.abspath(a.port), os.path.abspath(a.engine)
    out = os.path.abspath(a.out or os.path.join(a.port, "module"))
    if a.host == "windows":
        a.lift_opt = os.environ.get("PORT_LIFT_OPT", "/clang:-O2;/arch:AVX2;/GS-")
        a.spu_arch = os.environ.get("PORT_SPU_ARCH", "/arch:AVX2;-DSPU_LS_FASTPATH=1;-DSPU_FLOAT_RZ_ONLY=1")
    else:
        a.lift_opt = os.environ.get("PORT_LIFT_OPT", "-O2;-march=x86-64-v3;-fno-stack-protector")
        a.spu_arch = os.environ.get("PORT_SPU_ARCH", "-march=x86-64-v3;-fno-stack-protector;-DSPU_LS_FASTPATH=1;-DSPU_FLOAT_RZ_ONLY=1")
    a.kit = kit_version(a.engine)
    a.xdata = xdata_header(a.engine, out) if a.data_import else None
    cc, ld, bindir = find_llvm(a.host, a.llvm)
    env_msvc = None
    if a.msvc_env:
        sys.path.insert(0, os.path.join(ROOT, "tools"))
        from vsenv import vs_env
        env_msvc = vs_env()
    elif a.host == "windows":
        # the proof that no MSVC / Windows SDK is used: their header and library paths are not
        # in the environment the compiler sees (and /X ignores INCLUDE anyway)
        for k in ("INCLUDE", "LIB", "LIBPATH", "EXTERNAL_INCLUDE"):
            os.environ.pop(k, None)
    if env_msvc:
        os.environ.clear(); os.environ.update(env_msvc)
    tus = sources(a.port, a.engine, a.host)
    if a.only:
        keys = [k for k in a.only.split(",") if k]
        tus = [t for t in tus if any(k in t[1].replace("\\", "/") for k in keys)]
        a.compile_only = True
    ccstamp = stamp(cc)
    work = []
    for group, src in tus:
        flags = win_flags(group, a.port, a.engine, a) if a.host == "windows" else \
            linux_flags(group, a.port, a.engine, a, src)
        obj = obj_path(out, a.port, a.engine, group, src)
        key = hashlib.sha1(json.dumps([key_text(cc), ccstamp, [key_text(f) for f in flags]]).encode()).hexdigest()
        work.append((group, src, obj, flags, key))
    todo = [w for w in work if not up_to_date(w[2], w[4])]
    todo.sort(key=lambda w: -os.path.getsize(w[1]))     # the big lifted chunks first
    print(f"[module] {len(work)} translation units, {len(todo)} to compile, {a.jobs} at once "
          f"({os.path.basename(cc)}, {a.host}{', MSVC headers' if a.msvc_env else (', shim headers' if a.host == 'windows' else '')}"
          f"{', PGO ' + a.pgo.split(':')[0] if a.pgo else ''}; kit {a.kit})", flush=True)
    t0 = time.time()
    failed = []
    os.makedirs(out, exist_ok=True)
    clog = open(os.path.join(out, "compile.log"), "w", encoding="utf-8", errors="replace")   # every unit's output (PGO notes)
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, a.jobs)) as ex:
        futs = [ex.submit(compile_one, cc, w[3], w[1], w[2], a.host, w[4], a.verbose) for w in todo]
        done = 0
        for fu in concurrent.futures.as_completed(futs):
            src, dt, rc, msg, cmd = fu.result()
            done += 1
            clog.write(f"$ {src} ({dt:.1f}s, exit {rc})\n{msg}\n")
            name = os.path.relpath(src, a.port) if src.startswith(a.port) else os.path.basename(src)
            if rc:
                failed.append(src)
                print(f"  FAILED {name} (exit {rc}):\n{msg}\n  command: {' '.join(cmd)}", flush=True)
                if not a.keep_going:
                    for f in futs:
                        f.cancel()
            elif dt > 20 or a.verbose:
                print(f"  [{done}/{len(todo)}] {name} {dt:.0f}s" + (("\n" + msg[-2000:]) if a.verbose and msg else ""), flush=True)
    tc = time.time() - t0
    clog.close()
    if failed:
        die(f"{len(failed)} translation unit(s) failed")
    print(f"[module] compiled in {tc:.1f}s", flush=True)
    if a.compile_only:
        return
    # ---- link ----
    objs = [w[2] for w in work]
    dst = os.path.join(os.path.abspath(a.dll_dir or out), MODULE_FILE[a.host])
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    rsp = os.path.join(out, "link.rsp")
    t1 = time.time()
    if a.host == "windows":
        args = ["/DLL", "/NODEFAULTLIB", "/ENTRY:ps3_module_dll_entry", "/MACHINE:X64", "/INCREMENTAL:NO",
                "/OPT:REF", "/OPT:ICF", "/NOIMPLIB", "/OUT:" + dst]
        args += ["/NOPDB" if a.no_pdb else "/DEBUG:FULL", ] + ([] if a.no_pdb else ["/PDB:" + os.path.splitext(dst)[0] + ".pdb"])
        if a.pgo == "gen":
            args += pgo_runtime_win(bindir, out)
        flavor = ["-flavor", "link"] if os.path.basename(ld).lower().startswith("lld.") else []
        args.append(kernel32_implib(ld, flavor, out))
        with open(rsp, "w", encoding="utf-8") as f:
            f.write("\n".join('"%s"' % x for x in args + objs))
        cmd = [ld] + flavor + ["@" + rsp]
    else:
        args = ["-shared", "-o", dst, "--hash-style=gnu", "-z", "now", "-z", "noexecstack", "--gc-sections",
                "--icf=all", "-soname", MODULE_FILE["linux"]]
        if a.pgo == "gen":
            args += pgo_runtime_linux(bindir)
        with open(rsp, "w", encoding="utf-8") as f:
            f.write("\n".join(args + objs))
        cmd = [ld] + (["-flavor", "gnu"] if os.path.basename(ld).lower().startswith("lld") and not os.path.basename(ld).lower().startswith("ld.lld") else []) + ["@" + rsp]
    p = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
    tl = time.time() - t1
    if p.returncode != 0:
        print((p.stdout + p.stderr)[-8000:])
        die("link failed (exit %d): %s" % (p.returncode, " ".join(cmd)))
    if p.stdout.strip() or p.stderr.strip():
        print((p.stdout + p.stderr)[-3000:])
    mb = os.path.getsize(dst) / 1048576
    print(f"[module] linked {dst} ({mb:.1f} MB) in {tl:.1f}s; total {time.time() - t0:.1f}s", flush=True)
    with open(os.path.join(out, "module.json"), "w", encoding="utf-8") as f:
        json.dump({"file": dst, "kit": a.kit, "host": a.host, "pgo": a.pgo, "msvc_env": a.msvc_env,
                   "compiler": cc, "units": len(work), "compiled": len(todo), "compile_s": round(tc, 1),
                   "link_s": round(tl, 1), "MB": round(mb, 2), "built": time.strftime("%Y-%m-%d %H:%M:%S")},
                  f, indent=1)


def kernel32_implib(ld, flavor, out):
    """A one-entry import library for kernel32 (GetCurrentThreadId), made by lld's own librarian
    from a .def -- no SDK. The Windows loader sets up a DLL's thread-locals only when the DLL
    imports something (runtime/game_module/ps3_module_crt_win.c)."""
    d = os.path.join(out, "k32.def")
    lib = os.path.join(out, "k32.lib")
    text = "LIBRARY KERNEL32.dll\nEXPORTS\n  GetCurrentThreadId\n"
    if not (os.path.isfile(lib) and os.path.isfile(d) and open(d, encoding="ascii").read() == text):
        with open(d, "w", encoding="ascii") as f:
            f.write(text)
        p = subprocess.run([ld] + flavor + ["/lib", "/def:" + d, "/machine:x64", "/out:" + lib],
                           capture_output=True, text=True, errors="replace")
        if p.returncode != 0 or not os.path.isfile(lib):
            die("could not make the kernel32 import library: " + (p.stdout + p.stderr)[-1000:])
    return lib


def pgo_runtime_win(bindir, out):
    """the compiler-rt profile runtime for the instrumented module (ps3_module_pgo.c answers the C
    library names it needs from the engine)"""
    rt = sorted(glob.glob(os.path.join(os.path.dirname(bindir), "lib", "clang", "*", "lib", "windows",
                                       "clang_rt.profile-x86_64.lib")))
    if not rt:
        die("--pgo gen: no clang_rt.profile-x86_64.lib in the LLVM folder")
    return [rt[-1]]


def pgo_runtime_linux(bindir):
    rt = sorted(glob.glob(os.path.join(os.path.dirname(bindir), "lib", "clang", "*", "lib", "*linux*", "libclang_rt.profile*.a")) +
                glob.glob(os.path.join(os.path.dirname(bindir), "lib", "clang", "*", "lib", "linux", "libclang_rt.profile-x86_64.a")))
    if not rt:
        die("--pgo gen: no libclang_rt.profile*.a in the LLVM folder")
    return [rt[-1], "-u", "__llvm_profile_runtime"]


if __name__ == "__main__":
    main()
