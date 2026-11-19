"""The engine's C library binding for the game module (engine/runtime/module/ps3_module_libc.c),
built the way the release engine is (static C runtime, then the DLL runtime too): every name a
module may import is the engine's own or Windows', none comes from another C runtime, and the
calls the PGO profile writer makes work through them.   python -I tests/module_libc/test_libc_bind.py"""
import os, subprocess, sys, tempfile
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..", "..")
sys.path.insert(0, os.path.join(ROOT, "tools"))
from vsenv import vs_env
bad = 0
with tempfile.TemporaryDirectory() as d:
    for crt in ("-MT", "-MD"):
        exe = os.path.join(d, "libc_bind%s.exe" % crt)
        p = subprocess.run(["clang-cl", "-nologo", "-O2", "-w", "-D_CRT_SECURE_NO_WARNINGS", "-D_CRT_NONSTDC_NO_WARNINGS", crt, "-I" + os.path.join(ROOT, "engine", "include"),
                            "-Fe" + exe, "-Fo" + d + os.sep, os.path.join(HERE, "libc_bind_test.c"),
                            os.path.join(ROOT, "engine", "runtime", "module", "ps3_module_libc.c")],
                           env=vs_env(), capture_output=True, text=True, errors="replace")
        if p.returncode:
            print(crt, "BUILD FAILED\n" + (p.stdout + p.stderr)[-4000:])
            bad += 1
            continue
        r = subprocess.run([exe, os.path.join(d, "t.profraw")], capture_output=True, text=True, errors="replace")
        print(crt, "OK" if r.returncode == 0 else "FAILED (exit 0x%08X)" % (r.returncode & 0xFFFFFFFF))
        print("  " + (r.stdout + r.stderr).strip().replace("\n", "\n  "))
        bad += r.returncode != 0
sys.exit(1 if bad else 0)
