"""Per-thread syscall histogram of a native PS3_FTRACE file, in time windows.

    python -I tools/ftrace_threads.py <ftrace.txt> [--thread main] [--win 10] [--from 0] [--to 999]

Rows: kind tid num(hex) lr(hex) start_us end_us; kind 1 = syscall. Names come
from the toolkit's lv2 syscall table (tracediff.syscall_names). Shows, per time
window, which syscalls a thread made and how often: a thread stuck in a loop
shows the same small set forever; one that progresses shows it change.
"""
import argparse
import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tracediff as td  # noqa: E402


def load(path):
    names = td.syscall_names()
    th, rows = {}, []
    for ln in open(path):
        p = ln.split()
        if not p or p[0] == "#":
            continue
        if p[0] == "T":
            th[p[1]] = " ".join(p[2:])
            continue
        rows.append((int(p[0]), p[1], int(p[2], 16), int(p[3], 16), float(p[4]) / 1e6, float(p[5]) / 1e6))
    return th, rows, names


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ftrace")
    ap.add_argument("--thread", default="main")
    ap.add_argument("--win", type=float, default=10)
    ap.add_argument("--from", dest="t0", type=float, default=0)
    ap.add_argument("--to", dest="t1", type=float, default=1e9)
    ap.add_argument("--top", type=int, default=8)
    a = ap.parse_args()
    th, rows, names = load(a.ftrace)
    tids = {k for k, v in th.items() if v == a.thread or k == a.thread}
    buckets = collections.defaultdict(collections.Counter)
    for kind, tid, num, _lr, t, _e in rows:
        if kind != 1 or tid not in tids or not (a.t0 <= t < a.t1):
            continue
        buckets[int(t // a.win)][td.norm(names.get(num, f"syscall_{num}"))] += 1
    for b in sorted(buckets):
        c = buckets[b]
        print(f"{b * a.win:6.0f}s n={sum(c.values()):5d}  " +
              ", ".join(f"{n}x{k}" for k, n in c.most_common(a.top)))


if __name__ == "__main__":
    main()
