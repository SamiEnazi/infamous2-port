"""Summary of one play session (tools/play.ps1 writes play/<date-time>/playlog.tsv + threads.tsv).

    python -I tools/playlog_report.py <session dir>

Prints and writes summary.txt: how much of the session held the cap, the worst seconds,
hitches, every F9 mark with the seconds around it, and the busiest threads.
"""
import os
import sys
from collections import defaultdict


def main():
    d = os.path.abspath(sys.argv[1])
    secs, hitches, marks = [], [], []
    with open(os.path.join(d, "playlog.tsv")) as f:
        for line in f:
            r = line.rstrip("\n").split("\t")
            if not r or r[0].startswith("#"):
                continue
            if r[0] == "HITCH":
                hitches.append((float(r[1]), float(r[2])))
            elif r[0] == "MARK":
                marks.append(float(r[1]))
            elif len(r) >= 10:
                t, fps, p50, p99, mx, n17, n25, n33, n50, dpf = r[:10]
                secs.append(dict(t=float(t), fps=float(fps), p50=float(p50), p99=float(p99), max=float(mx),
                                 n17=int(n17), n25=int(n25), n33=int(n33), n50=int(n50), draws=float(dpf)))
    out = []
    say = lambda s="": (print(s), out.append(s))
    if not secs:
        say("no frames recorded")
        return
    total = secs[-1]["t"]
    frames = sum(s["fps"] for s in secs)
    say(f"session {os.path.basename(d)}: {total / 60:.1f} min, {frames:.0f} frames, average {frames / len(secs):.1f} fps")
    # the cap the session ran at (play.ps1 passes --cap; movies and menus run at 30 by design)
    cap = float(sys.argv[sys.argv.index("--cap") + 1]) if "--cap" in sys.argv else 60.0
    if cap <= 0:
        cap = max(s["fps"] for s in secs)
    held = sum(1 for s in secs if s["fps"] >= cap - 1.5)
    say(f"seconds at the cap (~{cap:.0f} fps): {held}/{len(secs)} ({100.0 * held / len(secs):.1f}%)")
    for lim in (55, 45, 30):
        n = sum(1 for s in secs if s["fps"] < lim)
        say(f"  seconds under {lim} fps: {n}")
    n33 = sum(s["n33"] for s in secs)
    n50 = sum(s["n50"] for s in secs)
    say(f"frames over 33 ms: {n33}, over 50 ms: {n50}  (hitch lines: {len(hitches)})")
    say("")
    say("worst 10 seconds (time, fps, p99 ms, max ms, draws/frame):")
    for s in sorted(secs, key=lambda s: s["fps"])[:10]:
        say(f"  {s['t'] / 60:5.1f} min ({s['t']:7.1f} s)  {s['fps']:5.1f} fps  p99 {s['p99']:5.1f}  max {s['max']:6.1f}  {s['draws']:5.0f} draws")
    if hitches:
        say("")
        say("longest hitches (time s, ms):")
        for t, ms in sorted(hitches, key=lambda h: -h[1])[:10]:
            say(f"  {t:7.1f} s  {ms:6.1f} ms")
    if marks:
        say("")
        say("F9 marks (the 5 s before each):")
        for m in marks:
            win = [s for s in secs if m - 5 <= s["t"] <= m + 1]
            if win:
                say(f"  {m:7.1f} s ({m / 60:.1f} min): min fps {min(s['fps'] for s in win):.1f}, "
                    f"worst frame {max(s['max'] for s in win):.1f} ms, draws/frame {max(s['draws'] for s in win):.0f}")
            else:
                say(f"  {m:7.1f} s")
    tp = os.path.join(d, "threads.tsv")
    if os.path.exists(tp):
        acc = defaultdict(list)
        with open(tp) as f:
            next(f, None)
            for line in f:
                r = line.split("\t")
                if len(r) >= 3:
                    acc[r[1]].append(float(r[2]))
        if acc:
            say("")
            say("busiest threads (host tid: average / peak CPU %, over the samples they were busy in):")
            for tid, v in sorted(acc.items(), key=lambda kv: -sum(kv[1]))[:10]:
                say(f"  {tid:>6}: {sum(v) / len(v):5.1f} / {max(v):5.1f}  ({len(v)} samples)")
    open(os.path.join(d, "summary.txt"), "w").write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
