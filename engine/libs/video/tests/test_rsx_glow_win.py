"""(fx12) Build and run test_rsx_glow on Windows (no game, no GPU).

    python -I test_rsx_glow_win.py [--out <dir>]

Builds the test with clang-cl against this toolkit's draw engine (as
test_rsx_draw_delta_win.py does) and runs it under PS3RECOMP_ENG_DELTA=0, =1,
=1 with PS3RECOMP_ENG_DELTA_VERIFY=1, and =1 with PS3RECOMP_ENG_FASTDRAW=0.
Exit 0 = every run passed.
"""
import argparse, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import test_rsx_draw_delta_win as dw  # noqa: E402  (its environment, compiler and engine source list)


def build(tk, out_dir, name):
    obj = os.path.join(out_dir, name + "_obj")
    os.makedirs(obj, exist_ok=True)
    flags = ["/nologo", "-DPS3RECOMP_RSX_VULKAN"] + ["-I" + os.path.join(tk, i) for i in dw.INC] + [
        "/DWIN32", "/D_WINDOWS", "/O2", "/DNDEBUG", "/std:c17", "-MD", "/Z7", "-w", "-fno-strict-aliasing",
        "/clang:-ffp-contract=off", "-msse4.1", "/FI" + os.path.join(tk, r"runtime\platform\win32_posix_min.h")]
    srcs = [os.path.join(HERE, "test_rsx_glow.c"), os.path.join(HERE, "delta_test_stubs.c")] + \
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
    ap.add_argument("--out", default=os.path.join(os.environ.get("TEMP", "."), "rsx_glow_test"))
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    exe = build(dw.TK, a.out, "test_rsx_glow")
    bad = 0
    for extra in ({"PS3RECOMP_ENG_DELTA": "0"}, {"PS3RECOMP_ENG_DELTA": "1"},
                  {"PS3RECOMP_ENG_DELTA": "1", "PS3RECOMP_ENG_DELTA_VERIFY": "1"},
                  {"PS3RECOMP_ENG_DELTA": "1", "PS3RECOMP_ENG_FASTDRAW": "0"}):
        env = dict(os.environ)
        for k in ("PS3RECOMP_ENG_DELTA", "PS3RECOMP_ENG_DELTA_VERIFY", "PS3RECOMP_ENG_FASTDRAW",
                  "PS3RECOMP_FX_GLOW", "PS3RECOMP_FX_GLOW_TARGETS", "PS3RECOMP_FX_RES"):
            env.pop(k, None)
        env.update(extra)
        p = subprocess.run([exe], env=env, capture_output=True, text=True, timeout=600)
        out = p.stdout + p.stderr
        fails = [l for l in out.splitlines() if l.startswith("[FAIL]") or l.startswith("[delta-verify]")]
        passes = sum(1 for l in out.splitlines() if l.startswith("[PASS]"))
        print("%-70s exit %d  %d passed  %d failed" % (extra, p.returncode, passes, len(fails)))
        for l in fails[:20]:
            print("   ", l)
        bad |= p.returncode != 0 or bool(fails)
    print("ALL PASSED" if not bad else "FAILED")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
