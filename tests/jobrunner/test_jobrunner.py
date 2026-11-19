"""The native job runner without the game: build jobrunner_harness.c with the runner as it is and run its
scenarios (see the harness's header).

    python -I tests/jobrunner/test_jobrunner.py [--runner <another wws_runner.c>] [--expect-broken]

--runner: the runner source to test (default: this tree's engine/libs/spurs/wws_runner.c). With an older
file (e.g. `git show v0.6.2:engine/libs/spurs/wws_runner.c`) and --expect-broken the test passes when that
runner shows the faults: the hang of a manager created again in place, and its skipped jobs.
Exit 0 = everything as expected.
"""
import argparse
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tools"))

ap = argparse.ArgumentParser()
ap.add_argument("--runner", default=os.path.join(ROOT, "engine", "libs", "spurs", "wws_runner.c"))
ap.add_argument("--expect-broken", action="store_true")
args = ap.parse_args()

# (scenario, extra environment, result of a fixed runner, result of the v0.6.2 runner, what it shows)
PASS, FAIL, HANG = 0, 1, 3
CASES = [
    ("stream", {}, PASS, PASS, "20000 jobs over many swaps"),
    ("reinit_same", {}, PASS, HANG, "same address, workload id and arrays after a swap (the reported hang)"),
    ("reinit_poll", {}, PASS, HANG, "the same, the game polling the token test"),
    ("reinit_noswap", {}, PASS, FAIL, "same sequence byte too: the first entries must not be skipped"),
    ("reinit_inplace", {}, PASS, HANG, "the init over the old object"),
    # the stall repair alone: the new checks off, as if this were a fault nobody has found yet
    ("reinit_same", {"PS3_NATIVE_JOBS_NODETECT": "1"}, PASS, None, "repair: a wait, checks off"),
    ("reinit_poll", {"PS3_NATIVE_JOBS_NODETECT": "1"}, PASS, None, "repair: a polled test, checks off"),
    ("reinit_same", {"PS3_NATIVE_JOBS_NODETECT": "1", "PS3_NATIVE_JOBS_HEAL": "0"}, HANG, None,
     "checks off and repair off = the old hang (the switches work)"),
]
NAMES = {PASS: "pass", FAIL: "FAIL", HANG: "HANG"}

with tempfile.TemporaryDirectory() as d:
    exe = os.path.join(d, "jobrunner_harness.exe" if os.name == "nt" else "jobrunner_harness")
    if os.name == "nt":
        from vsenv import vs_env
        cmd = [r"C:\Program Files\LLVM\bin\clang-cl.exe", "-nologo", "-O2", "-MT", "-W3", "-D_CRT_SECURE_NO_WARNINGS", "-Fe" + exe, "-Fo" + d + os.sep,
               os.path.join(HERE, "jobrunner_harness.c"), os.path.abspath(args.runner)]
        subprocess.run(cmd, check=True, env=vs_env())
    else:
        subprocess.run(["clang", "-O2", "-pthread", "-o", exe, os.path.join(HERE, "jobrunner_harness.c"),
                        os.path.abspath(args.runner),
                        os.path.join(ROOT, "engine", "runtime", "platform", "win32_compat.c")], check=True)
    bad = 0
    for sc, env, want_fixed, want_old, what in CASES:
        want = want_old if args.expect_broken else want_fixed
        if want is None:
            continue
        e = dict(os.environ)
        for k in [k for k in e if k.startswith("PS3_NATIVE_JOBS")]:
            del e[k]
        e.update(env)
        p = subprocess.run([exe, sc], capture_output=True, text=True, env=e, timeout=60)
        ok = p.returncode == want
        bad += not ok
        repaired = "stall repaired" in p.stderr
        print(f"{'ok ' if ok else 'BAD'} {sc:15} {' '.join(f'{k[16:]}={v}' for k, v in env.items()):22} "
              f"-> {NAMES.get(p.returncode, p.returncode)} (expected {NAMES[want]})"
              f"{', repaired' if repaired else ''}   {what}")
        if not ok or env.get("PS3_NATIVE_JOBS_NODETECT") and want == PASS and not repaired:
            if ok:
                bad += 1
                print("    BAD: passed without the repair line")
            print("    " + "\n    ".join((p.stdout + p.stderr).strip().splitlines()[-12:]))
    print("FAILED" if bad else "all as expected")
    sys.exit(1 if bad else 0)
