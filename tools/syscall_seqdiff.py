"""Per-thread lv2 syscall sequence: RPCS3 oracle vs native build.

    python -I tools/syscall_seqdiff.py <oracle_dir> <native_ftrace.txt> [--thread main] [--from NAME] [--ctx 6]

Oracle : <dir>/parsed/events.jsonl (rpcs3_events.py), RPCS3 run with the sys_* channels at Trace.
Native : PS3_FTRACE file recorded with PS3_FTRACE_KINDS=1 PS3_FTRACE_MIN_US=0 (kind 1 = syscall).
Both sequences are reduced to syscall NAMES with consecutive repeats collapsed
("sys_lwmutex_lock x40" -> one entry), optionally cut to start at the first
occurrence of --from, then aligned with difflib. The first non-equal block is
the first point where the native main thread does something the oracle's did not.
"""
import argparse
import difflib
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import tracediff as td  # noqa: E402

SKIP = {"sys_timer_usleep", "sys_time_get_current_time", "sys_time_get_timebase_frequency",
        "sys_ppu_thread_yield", "sys_time_get_system_time"}   # pacing noise, not logic


ALIAS = {"sys_ppu_thread_create_raw": "sys_ppu_thread_create"}


def norm(n):
    n = td.norm(n)
    for suf in ("_raw",):
        if n.endswith(suf):
            n = n[: -len(suf)]
    return ALIAS.get(n, n)


def oracle_seq(d, want):
    seq = []
    for ln in open(os.path.join(d, "parsed", "events.jsonl"), encoding="utf-8"):
        e = json.loads(ln)
        th = e.get("thread") or ""
        if not th.startswith("PPU["):
            continue
        m = re.search(r"Thread \((.*)\)$", th)
        name = m.group(1) if m else th
        if name != want:
            continue
        fn = e.get("fn") or ""
        msg = e["msg"]
        if not fn.lstrip("_").startswith("sys_") or "): " in msg or "failed with" in msg or "aborted" in msg:
            continue
        if not msg.startswith(fn + "(") and not msg.startswith("'" + fn):
            continue
        n = norm(fn)
        if n in {norm(s) for s in SKIP}:
            continue
        seq.append((e["t"], n))
    return seq


def native_seq(path, want):
    names = td.syscall_names()
    th, seq = {}, []
    for ln in open(path):
        p = ln.split()
        if not p or p[0] == "#":
            continue
        if p[0] == "T":
            th[p[1]] = " ".join(p[2:])
            continue
        if p[0] != "1":
            continue
        tid, num, t0 = p[1], int(p[2], 16), float(p[4]) / 1e6
        tname = th.get(tid, tid)
        if (want == "main_thread" and tname not in ("main", "main_thread")) or (want != "main_thread" and tname != want):
            continue
        n = norm(names.get(num, f"syscall_{num}"))
        if n in {norm(s) for s in SKIP}:
            continue
        seq.append((t0, n))
    return seq


def collapse(seq):
    out = []
    for t, n in seq:
        if out and out[-1][1] == n:
            out[-1][2] += 1
        else:
            out.append([t, n, 1])
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("oracle"); ap.add_argument("native")
    ap.add_argument("--thread", default="main_thread")
    ap.add_argument("--from", dest="start")
    ap.add_argument("--ctx", type=int, default=6)
    ap.add_argument("--structural", action="store_true",
                    help="drop timing/contention and layer-dependent calls (locks, signals, fs I/O, rsx, config)")
    a = ap.parse_args()
    o, n = oracle_seq(a.oracle, a.thread), native_seq(a.native, a.thread)
    if a.structural:
        noisy = re.compile(r"^sys_(lw)?(mutex|cond)_|^sys_semaphore_|^sys_rwlock_|^sys_fs_|^sys_rsx_|^sys_config_|"
                           r"^sys_event_flag_(wait|set|clear|trywait)|^sys_lwcond_|^sys_ss_|^sys_process_get")
        o = [x for x in o if not noisy.search(x[1])]
        n = [x for x in n if not noisy.search(x[1])]
    o, n = collapse(o), collapse(n)
    if a.start:
        s = norm(a.start)
        o = o[next(i for i, x in enumerate(o) if x[1] == s):] if any(x[1] == s for x in o) else o
        n = n[next(i for i, x in enumerate(n) if x[1] == s):] if any(x[1] == s for x in n) else n
    print(f"thread {a.thread}: oracle {len(o)} collapsed calls, native {len(n)}")
    sm = difflib.SequenceMatcher(a=[x[1] for x in o], b=[x[1] for x in n], autojunk=False)
    shown = 0
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag == "equal":
            continue
        print(f"\n== {tag}: oracle[{i1}:{i2}] vs native[{j1}:{j2}]")
        for k in range(max(0, i1 - a.ctx), i1):
            print(f"   = {o[k][0]:9.3f}  {o[k][1]} x{o[k][2]}")
        for k in range(i1, min(i2, i1 + 12)):
            print(f"   O {o[k][0]:9.3f}  {o[k][1]} x{o[k][2]}")
        for k in range(j1, min(j2, j1 + 12)):
            print(f"   N {n[k][0]:9.3f}  {n[k][1]} x{n[k][2]}")
        shown += 1
        if shown >= 6:
            break


if __name__ == "__main__":
    main()
