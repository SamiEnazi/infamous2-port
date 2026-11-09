"""Where a window of frames goes: RSX semaphore waits, FIFO drains, GPU waits,
SPU busy, presents -- from a PS3_FTRACE file (kinds 4,5,7,9,10,12).

    python -I tools/frame_profile.py <ftrace.txt> [t0 t1]     (seconds; default: whole file)
"""
import collections
import sys

rows = [l.split() for l in open(sys.argv[1]) if l[:1].isdigit()]
t0 = float(sys.argv[2]) * 1e6 if len(sys.argv) > 2 else min(float(r[4]) for r in rows)
t1 = float(sys.argv[3]) * 1e6 if len(sys.argv) > 3 else max(float(r[5]) for r in rows)
rows = [r for r in rows if t0 <= float(r[4]) < t1]
W = (t1 - t0) / 1e6
names = {3: "spu_wait", 4: "spu_busy", 5: "rsx_sema", 6: "sema_gaveup", 7: "rsx_drain", 9: "flip_req",
         10: "present", 12: "gpu_wait", 16: "compile", 17: "texup"}
tot, cnt = collections.defaultdict(float), collections.Counter()
for r in rows:
    k = int(r[0]); tot[k] += (float(r[5]) - float(r[4])) / 1e6; cnt[k] += 1
frames = cnt[10] or 1
print(f"window {t0/1e6:.1f}-{t1/1e6:.1f}s  {W:.1f}s  presents {cnt[10]}  fps {cnt[10]/W:.2f}  frame {W/frames*1000:.0f} ms")
for k in sorted(cnt):
    print(f"  {names.get(k, str(k)):12s} n={cnt[k]:6d}  total {tot[k]:7.2f}s  {tot[k]/W*100:6.1f}% wall  per frame {tot[k]/frames*1000:7.1f} ms")
sema = collections.defaultdict(list)
for r in rows:
    if r[0] == "5": sema[r[2]].append((float(r[5]) - float(r[4])) / 1e3)
for a, v in sorted(sema.items(), key=lambda x: -sum(x[1])):
    print(f"  sema label 0x{a}: n={len(v)} mean {sum(v)/len(v):.1f} ms, per frame {sum(v)/frames:.1f} ms")
busy = collections.defaultdict(float)
for r in rows:
    if r[0] == "4": busy[r[1]] += (float(r[5]) - float(r[4])) / 1e6
if busy:
    print("  SPU busy %: " + " ".join(f"{k}:{v/W*100:.0f}" for k, v in sorted(busy.items())))
