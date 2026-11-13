"""(lowend) Build and run test_rsx_draw_engine on Windows (no game, no GPU).

    python -I test_rsx_draw_engine_win.py [--out <dir>]

Builds the test with clang-cl against this toolkit's draw engine (as
test_rsx_draw_delta_win.py does: its environment, compiler and engine source
list) and runs it once. Exit 0 = every check passed.
"""
import argparse, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import test_rsx_draw_delta_win as dw  # noqa: E402


def build(tk, out_dir, name):
    obj = os.path.join(out_dir, name + "_obj")
    os.makedirs(obj, exist_ok=True)
    flags = ["/nologo", "-DPS3RECOMP_RSX_VULKAN"] + ["-I" + os.path.join(tk, i) for i in dw.INC] + [
        "/DWIN32", "/D_WINDOWS", "/O2", "/DNDEBUG", "/std:c17", "-MD", "/Z7", "-w", "-fno-strict-aliasing",
        "/clang:-ffp-contract=off", "-msse4.1", "/FI" + os.path.join(tk, r"runtime\platform\win32_posix_min.h")]
    # what cellGcmSys.c gives the engine in a full build (the test defines the address resolvers itself)
    extra = os.path.join(out_dir, "engine_test_stubs.c")
    with open(extra, "w") as f:
        f.write("volatile unsigned g_gcm_xlate_gen = 1;\n")
    srcs = [os.path.join(HERE, "test_rsx_draw_engine.c"), os.path.join(HERE, "delta_test_stubs.c"), extra] + \
           [os.path.join(tk, s) for s in dw.ENGINE]
    objs = []
    for s in srcs:
        o = os.path.join(obj, os.path.basename(s) + ".obj")
        p = subprocess.run([dw.CLANG] + flags + ["/Fo" + o, "-c", "--", s], env=dw.ENV, capture_output=True, text=True)
        if p.returncode:
            sys.exit("compile failed: %s\n%s" % (s, (p.stdout + p.stderr)[-3000:]))
        objs.append(o)
    exe = os.path.join(out_dir, name + ".exe")
    p = subprocess.run([dw.LLD, "/nologo", "/DEBUG", "/OUT:" + exe] + objs +
                       ["kernel32.lib", "user32.lib", "synchronization.lib", "advapi32.lib"],
                       env=dw.ENV, capture_output=True, text=True)
    if p.returncode:
        sys.exit("link failed:\n" + (p.stdout + p.stderr)[-3000:])
    return exe


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(os.environ.get("TEMP", "."), "rsx_engine_test"))
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    exe = build(dw.TK, a.out, "test_rsx_draw_engine")
    env = dict(os.environ)
    for k in ("PS3RECOMP_ENG_TEXCACHE_MB", "PS3RECOMP_ENG_TEXCACHE", "PS3RECOMP_ENG_FBFETCH"):
        env.pop(k, None)
    env["PS3RECOMP_WTRACK"] = "0"   # the guest memory is a calloc'd buffer: no page protection
    p = subprocess.run([exe], env=env, capture_output=True, text=True, timeout=600)
    out = p.stdout + p.stderr
    fails = [l for l in out.splitlines() if l.startswith("[FAIL]")]
    passes = sum(1 for l in out.splitlines() if l.startswith("[PASS]"))
    print("test_rsx_draw_engine: exit %d, %d passed, %d failed" % (p.returncode, passes, len(fails)))
    for l in fails:
        print("  " + l)
    sys.exit(1 if p.returncode or fails else 0)


if __name__ == "__main__":
    main()
