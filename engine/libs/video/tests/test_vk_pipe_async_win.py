"""(stutter) Build and run test_vk_pipe_async on Windows (no game, no GPU, no Vulkan driver).

    python -I test_vk_pipe_async_win.py [--out <dir>] [--runs N]

Builds the test with clang-cl -- vk_pipe_async.inc against a stand-in for the
Vulkan backend (test_vk_pipe_async.c) -- and runs it N times (default 5: the
workers' timing differs run to run), each in an empty cache folder, plus once
with PS3RECOMP_VK_PIPE_THREADS=1 and once with 8. Exit 0 = every run passed.
"""
import argparse, os, shutil, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import test_rsx_draw_delta_win as dw  # noqa: E402  (its environment and compiler)


def build(out_dir):
    exe = os.path.join(out_dir, "test_vk_pipe_async.exe")
    obj = os.path.join(out_dir, "test_vk_pipe_async.obj")
    flags = ["/nologo", "-DPS3RECOMP_RSX_VULKAN"] + ["-I" + os.path.join(dw.TK, i) for i in dw.INC] + [
        "-I" + os.path.join(dw.ROOT, "deps", "Vulkan-Headers", "include"),
        "/DWIN32", "/D_WINDOWS", "/O2", "/DNDEBUG", "/std:c17", "-MD", "/Z7", "-Wall", "-Wno-unused-function",
        "-Wno-padded", "-Wno-declaration-after-statement", "-Wno-unsafe-buffer-usage", "-Wno-deprecated-declarations",
        "-Wno-reserved-identifier", "-Wno-missing-prototypes", "-Wno-cast-qual", "-Wno-sign-conversion",
        "-Wno-implicit-int-conversion", "-Wno-shorten-64-to-32", "-Wno-missing-variable-declarations",
        "-Wno-nonportable-system-include-path", "-Wno-c++98-compat", "-Wno-pre-c11-compat", "-Wno-switch-default",
        "-Wno-covered-switch-default", "-Wno-unused-macros", "-Wno-float-equal", "-Wno-double-promotion",
        "-Wno-format-nonliteral", "-Wno-bad-function-cast", "-Wno-cast-function-type-strict", "-Wno-pre-c23-compat",
        "-Wno-gnu-zero-variadic-macro-arguments", "-Wno-flexible-array-extensions", "-Wno-assign-enum",
        "-Wno-tautological-unsigned-zero-compare", "-Wno-comma", "-Wno-extra-semi-stmt", "-Wno-used-but-marked-unused",
        "-Wno-unused-parameter", "-Wno-language-extension-token", "-Wno-c11-extensions", "-Wno-int-to-pointer-cast",
        "-fno-strict-aliasing"]
    src = os.path.join(HERE, "test_vk_pipe_async.c")
    p = subprocess.run([dw.CLANG] + flags + ["/Fo" + obj, "-c", "--", src], env=dw.ENV, capture_output=True, text=True)
    out = p.stdout + p.stderr
    warn = [l for l in out.splitlines() if "warning:" in l and ("vk_pipe_async" in l)]
    for l in warn[:40]:
        print("   ", l)
    if p.returncode:
        sys.exit("compile failed:\n" + out[-4000:])
    p = subprocess.run([dw.LLD, "/nologo", "/DEBUG", "/OUT:" + exe, obj, "kernel32.lib"],
                       env=dw.ENV, capture_output=True, text=True)
    if p.returncode:
        sys.exit("link failed:\n" + (p.stdout + p.stderr)[-3000:])
    return exe, len(warn)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(os.environ.get("TEMP", "."), "vk_pipe_async_test"))
    ap.add_argument("--runs", type=int, default=5)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    exe, nwarn = build(a.out)
    print("built %s (%d warnings in vk_pipe_async.inc)" % (exe, nwarn))
    bad = 0
    runs = [{} for _ in range(a.runs)] + [{"PS3RECOMP_VK_PIPE_THREADS": "1"}, {"PS3RECOMP_VK_PIPE_THREADS": "8"}]
    for i, extra in enumerate(runs):
        d = os.path.join(a.out, "cache%d" % i)
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d)
        env = dict(os.environ)
        for k in [k for k in env if k.startswith("PS3RECOMP_VK_PIPE")]:
            env.pop(k)
        env.update(extra)
        env["TEST_DIR"] = d
        p = subprocess.run([exe], env=env, capture_output=True, text=True, timeout=900)
        out = p.stdout + p.stderr
        fails = [l for l in out.splitlines() if l.startswith("[FAIL]")]
        print("run %d %-36s exit %d, %d failed" % (i, extra, p.returncode, len(fails)))
        for l in out.splitlines():
            if l.startswith("[INFO]") or l.startswith("[PASS]") or l.startswith("[FAIL]"):
                print("   ", l[:220])
        bad |= p.returncode != 0 or bool(fails)
    print("ALL PASSED" if not bad else "FAILED")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
