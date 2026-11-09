"""Summarize a clang-cl/ninja build log: failed units and distinct errors.

    python -I tools/build_errors.py <build.log> [--all]
"""
import re
import sys
from collections import Counter, OrderedDict

log = open(sys.argv[1], encoding="utf-8", errors="replace").read().splitlines()
failed = OrderedDict()
for ln in log:
    if ln.startswith("FAILED:"):
        m = re.search(r"([\w./\\-]+\.(?:c|cc|cpp|cxx|m|mm))\.obj", ln)
        failed[m.group(1) if m else ln[:160]] = 1
err = re.compile(r"^(?P<file>.*?)\((?P<line>\d+),(?P<col>\d+)\): (?:fatal )?error: (?P<msg>.*)$")
errors = Counter()
first = {}
for ln in log:
    m = err.match(ln)
    if m:
        f = re.sub(r"^.*?toolkit[\\/]", "", m["file"])
        key = (f, m["msg"])
        errors[key] += 1
        first.setdefault(key, m["line"])
print(f"FAILED units: {len(failed)}")
for f in failed:
    print("  ", re.sub(r"^.*?ps3recomp_runtime\.dir[\\/]", "", f))
print(f"\ndistinct errors: {len(errors)}  (total {sum(errors.values())})")
limit = None if "--all" in sys.argv else 60
for (f, msg), n in errors.most_common(limit):
    print(f"  {n:4}x {f}:{first[(f, msg)]}: {msg[:200]}")
