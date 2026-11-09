"""Build sha1_cli with clang-cl and compare against hashlib on several inputs."""
import hashlib, os, subprocess, sys, tempfile
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools"))
from vsenv import vs_env
with tempfile.TemporaryDirectory() as d:
    exe = os.path.join(d, "sha1_cli.exe")
    subprocess.run([r"C:\Program Files\LLVM\bin\clang-cl.exe", "-nologo", "-O2", "-Fe" + exe,
                    "-Fo" + os.path.join(d, "x.obj"), os.path.join(HERE, "sha1_cli.c")], check=True, env=vs_env())
    cases = {"empty": b"", "abc": b"abc", "55": b"a" * 55, "56": b"a" * 56, "64": b"a" * 64,
             "1M": os.urandom(1 << 20) + b"tail"}
    extra = sys.argv[1:]
    bad = 0
    for name, data in cases.items():
        p = os.path.join(d, name)
        open(p, "wb").write(data)
        got = subprocess.run([exe, p], capture_output=True, text=True).stdout.strip()
        ok = got == hashlib.sha1(data).hexdigest()
        bad += not ok
        print(f"{name:6} {'OK' if ok else 'MISMATCH'}")
    for p in extra:
        got = subprocess.run([exe, p], capture_output=True, text=True).stdout.strip()
        ok = got == hashlib.sha1(open(p, "rb").read()).hexdigest()
        bad += not ok
        print(f"{os.path.basename(p):6} {'OK' if ok else 'MISMATCH'}")
sys.exit(1 if bad else 0)
