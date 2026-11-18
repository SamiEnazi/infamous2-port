"""[aa] Build and run test_alpha_aa on Windows (no game, no GPU): the alpha-test
rewrites of rsx_fp_decompiler.c through glslang, as the Vulkan backend compiles them.

    python -I test_alpha_aa_win.py [--out <dir>]
"""
import argparse, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import test_rsx_draw_delta_win as dw  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(os.environ.get("TEMP", "."), "rsx_alpha_aa_test"))
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    tk = dw.TK
    root = os.path.normpath(os.path.join(tk, ".."))
    gl = os.path.join(root, "deps", "glslang-install")
    inc = ["-I" + os.path.join(tk, i) for i in dw.INC] + ["-imsvc" + os.path.join(gl, "include")]
    base = ["/nologo", "/DWIN32", "/D_WINDOWS", "/O2", "/DNDEBUG", "-MD", "-w", "/Z7"]
    jobs = [(os.path.join(HERE, "test_alpha_aa.c"), ["/std:c17"]),
            (os.path.join(tk, r"libs\video\rsx_fp_decompiler.c"), ["/std:c17"]),
            (os.path.join(tk, r"libs\video\rsx_vp_decompiler.c"), ["/std:c17"]),
            (os.path.join(tk, r"libs\video\rsx_shader_spirv.cpp"), ["-std:c++20", "/EHsc", "-DPS3RECOMP_HAVE_SPIRV_TRANSLATION"])]
    objs = []
    for src, extra in jobs:
        o = os.path.join(a.out, os.path.basename(src) + ".obj")
        p = subprocess.run([dw.CLANG] + base + extra + inc + ["/Fo" + o, "-c", "--", src], env=dw.ENV,
                           capture_output=True, text=True)
        if p.returncode:
            sys.exit("compile failed: %s\n%s" % (src, (p.stdout + p.stderr)[-3000:]))
        objs.append(o)
    libs = [os.path.join(gl, "lib", n) for n in ("glslang.lib", "MachineIndependent.lib", "GenericCodeGen.lib",
                                                 "OSDependent.lib", "SPIRV.lib", "glslang-default-resource-limits.lib")]
    exe = os.path.join(a.out, "test_alpha_aa.exe")
    p = subprocess.run([dw.LLD, "/nologo", "/DEBUG", "/OUT:" + exe] + objs + libs + ["kernel32.lib"],
                       env=dw.ENV, capture_output=True, text=True)
    if p.returncode:
        sys.exit("link failed:\n" + (p.stdout + p.stderr)[-3000:])
    p = subprocess.run([exe], capture_output=True, text=True, timeout=300)
    out = p.stdout + p.stderr
    print(out[-20000:])
    sys.exit(p.returncode)


if __name__ == "__main__":
    main()
