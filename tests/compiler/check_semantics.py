"""Guard: the flags we compile translated PS3 code with must keep PS3 semantics.

Compiles semantics.c with clang-cl and the given flags (FMA-capable target)
and checks the assembly:
  f(): signed overflow wraps        -> needs -fwrapv (else folded to 'return 0')
  g(): a*b+c stays mul + add       -> needs /clang:-ffp-contract=off (else vfmadd)

    python -I tests/compiler/check_semantics.py [extra clang-cl flags...]
Default flags = what our lifted-code build uses.
"""
import os, re, subprocess, sys, tempfile
HERE = os.path.dirname(os.path.abspath(__file__))
CL = r"C:\Program Files\LLVM\bin\clang-cl.exe"
flags = sys.argv[1:] or ["-fwrapv", "/clang:-ffp-contract=off", "-mavx2", "-mfma", "-mbmi2"]
with tempfile.TemporaryDirectory() as d:
    asm = os.path.join(d, "s.asm")
    subprocess.run([CL, "-nologo", "-O2", *flags, "-FA", "-Fa" + asm, "-Fo" + os.path.join(d, "s.obj"),
                    "-c", os.path.join(HERE, "semantics.c")], check=True)
    a = open(asm).read()
f = re.search(r"^f:.*?ret", a, re.S | re.M).group(0)
g = re.search(r"^g:.*?ret", a, re.S | re.M).group(0)
ok_wrap = "imul" in f
ok_fma = "vfmadd" not in g and "fmadd" not in g
print(f"signed overflow wraps (-fwrapv):      {'OK' if ok_wrap else 'BROKEN'}")
print(f"no FMA contraction (fp-contract=off): {'OK' if ok_fma else 'BROKEN'}")
sys.exit(0 if ok_wrap and ok_fma else 1)
