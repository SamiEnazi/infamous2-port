"""Align one thread's syscall sequence between two native PS3_FTRACE runs.

    python -I tools/ftrace_seqdiff.py <a.txt> <b.txt> [--thread main] [--lr] [--ctx 8] [--blocks 6]

Same idea as syscall_seqdiff.py (oracle vs native) but native vs native: two
runs of the same build under different conditions (e.g. slow vs fast present)
-- the first non-equal block is where the thread's behaviour forks. With --lr
the call site (guest lr) is part of each token, so the same syscall from a
different place counts as different.
"""
import argparse
import difflib
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tracediff as td  # noqa: E402
from ftrace_threads import load  # noqa: E402

SKIP = {"sys_timer_usleep", "sys_ppu_thread_yield"}


def seq(path, want, with_lr):
    th, rows, names = load(path)
    tids = {k for k, v in th.items() if v == want or k == want}
    out = []
    for kind, tid, num, lr, t, _e in rows:
        if kind != 1 or tid not in tids:
            continue
        n = td.norm(names.get(num, f"syscall_{num}"))
        if n in SKIP:
            continue
        tok = f"{n}@{lr:08X}" if with_lr else n
        if out and out[-1][1] == tok:
            out[-1][2] += 1
        else:
            out.append([t, tok, 1])
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("a"); ap.add_argument("b")
    ap.add_argument("--thread", default="main")
    ap.add_argument("--lr", action="store_true")
    ap.add_argument("--ctx", type=int, default=8)
    ap.add_argument("--blocks", type=int, default=6)
    a = ap.parse_args()
    A, B = seq(a.a, a.thread, a.lr), seq(a.b, a.thread, a.lr)
    print(f"thread {a.thread}: A {len(A)} collapsed calls, B {len(B)}")
    sm = difflib.SequenceMatcher(a=[x[1] for x in A], b=[x[1] for x in B], autojunk=False)
    shown = 0
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag == "equal":
            continue
        print(f"\n== {tag}: A[{i1}:{i2}] vs B[{j1}:{j2}]")
        for k in range(max(0, i1 - a.ctx), i1):
            print(f"   = {A[k][0]:9.3f}  {A[k][1]} x{A[k][2]}")
        for k in range(i1, min(i2, i1 + 14)):
            print(f"   A {A[k][0]:9.3f}  {A[k][1]} x{A[k][2]}")
        for k in range(j1, min(j2, j1 + 14)):
            print(f"   B {B[k][0]:9.3f}  {B[k][1]} x{B[k][2]}")
        shown += 1
        if shown >= a.blocks:
            break


if __name__ == "__main__":
    main()
