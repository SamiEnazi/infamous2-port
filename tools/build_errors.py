"""Summarize a clang-cl or clang (Linux) / ninja build log: failed units and distinct errors.

    python -I tools/build_errors.py <build.log> [--all] [--every-try]

The compile log is appended to by every build, so only the last build in it is read (from its last
"$ ... --build" line); --every-try reads the whole file.
"""
import re
import sys
from collections import Counter, OrderedDict

log = open(sys.argv[1], encoding="utf-8", errors="replace").read().splitlines()
if "--every-try" not in sys.argv:
    starts = [i for i, ln in enumerate(log) if ln.startswith("$ ") and " --build " in ln]
    if starts:
        log = log[starts[-1]:]
failed = OrderedDict()
for ln in log:
    if ln.startswith("FAILED:"):
        m = re.search(r"([\w./\\-]+\.(?:c|cc|cpp|cxx|m|mm))\.(?:obj|o)\b", ln)
        failed[m.group(1) if m else ln[:160]] = 1
# clang-cl:  file(12,5): error: ...      clang / gcc:  file:12:5: error: ...      and clang++: error: ...
err_cl = re.compile(r"^(?P<file>.*?)\((?P<line>\d+),(?P<col>\d+)\): (?:fatal )?error: (?P<msg>.*)$")
err_gnu = re.compile(r"^(?P<file>[^:\s][^:]*?):(?P<line>\d+):(?P<col>\d+): (?:fatal )?error: (?P<msg>.*)$")
err_drv = re.compile(r"^(?P<file>clang\+\+|clang|clang-cl|lld-link|ld\.lld|ld): error: (?P<msg>.*)$")
errors = Counter()
first = {}
for ln in log:
    m = err_cl.match(ln) or err_gnu.match(ln)
    line = m["line"] if m else "-"
    if not m:
        m = err_drv.match(ln)
    if m:
        f = re.sub(r"^.*?(?:toolkit|engine)[\\/]", "", m["file"])
        key = (f, m["msg"])
        errors[key] += 1
        first.setdefault(key, line)
print(f"FAILED units: {len(failed)}")
for f in failed:
    print("  ", re.sub(r"^.*?(?:ps3recomp_runtime|Infamous2Recomp)\.dir[\\/]", "", f))
print(f"\ndistinct errors: {len(errors)}  (total {sum(errors.values())})")
limit = None if "--all" in sys.argv else 60
for (f, msg), n in errors.most_common(limit):
    print(f"  {n:4}x {f}:{first[(f, msg)]}: {msg[:200]}")
