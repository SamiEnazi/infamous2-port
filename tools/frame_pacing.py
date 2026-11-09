"""Frame pacing from a PS3_FTRACE file: present-to-present times, their
percentiles, and every hitch with what overlapped it (shader compiles, texture
uploads, GPU waits, RSX semaphore waits) -- where the stutter comes from.

    python -I tools/frame_pacing.py <ftrace.txt> [t0 t1] [--hitch-ms 50] [--by-who]

Rows are "<kind> <who> <a> <b> <start_us> <end_us>" (kinds as frame_profile.py:
10 present, 16 compile, 17 texture upload, 12 GPU wait, 5 RSX semaphore wait).
"""
import sys

args = [a for a in sys.argv[1:] if not a.startswith("--")]
hitch_ms = 50.0
if "--hitch-ms" in sys.argv:
    hitch_ms = float(sys.argv[sys.argv.index("--hitch-ms") + 1])
    args = [a for a in args if a != sys.argv[sys.argv.index("--hitch-ms") + 1]]
rows = [l.split() for l in open(args[0]) if l[:1].isdigit()]
raw = [r for r in rows if len(r) >= 6]
rows = [(int(r[0]), r[1], r[2], float(r[4]), float(r[5])) for r in raw]
# --by-who: the compile column split into the engine pipeline (decompile + backend; it
# contains the vs/fs translations), vs/fs SPIR-V (_new = glslang, _disk = from the shader
# cache), vkpipe (vkCreateGraphicsPipelines) and pc_save (pipeline-cache save)
by_who = "--by-who" in sys.argv
b_of = {id(t): r[3] for t, r in zip(rows, raw)}
t0 = float(args[1]) * 1e6 if len(args) > 1 else min(r[3] for r in rows)
t1 = float(args[2]) * 1e6 if len(args) > 2 else max(r[4] for r in rows)
rows = [r for r in rows if t0 <= r[3] < t1]

pres = sorted(r[4] for r in rows if r[0] == 10)
dts = [(pres[i] - pres[i - 1]) / 1000.0 for i in range(1, len(pres))]
if not dts:
    sys.exit("no presents in the window")
s = sorted(dts)
pct = lambda p: s[min(len(s) - 1, int(p / 100.0 * len(s)))]
W = (t1 - t0) / 1e6
print(f"window {t0/1e6:.1f}-{t1/1e6:.1f}s: {len(pres)} presents, {len(pres)/W:.1f} fps avg")
print(f"frame ms: p50 {pct(50):.1f}  p90 {pct(90):.1f}  p99 {pct(99):.1f}  max {s[-1]:.1f}")
for lim in (33.4, 50, 100, 250):
    n = sum(1 for d in dts if d > lim)
    print(f"  frames > {lim:5.1f} ms: {n:5d}  ({n/len(dts)*100:.1f}%)")

names = {16: "compile", 17: "texup", 12: "gpu_wait", 5: "rsx_sema", 7: "rsx_drain"}
print(f"\nhitches > {hitch_ms:.0f} ms (time, length, overlapping work):")
shown = 0
for i in range(1, len(pres)):
    d = (pres[i] - pres[i - 1]) / 1000.0
    if d <= hitch_ms:
        continue
    a, b = pres[i - 1], pres[i]
    over = {}
    for r in rows:
        k, who, lab, st, en = r
        if k in names and en > a and st < b:
            ov = (min(en, b) - max(st, a)) / 1000.0
            key = names[k] + ("" if k != 5 else f"@{lab}")
            if k == 16 and by_who:   # compile split by stage (rsx_vulkan_backend.c PS3_FT_COMPILE)
                key = {"0": "eng_pipe", "1": "vs", "2": "fs", "3": "vkpipe", "5": "pc_save", "6": "glslang", "7": "spv_write"}.get(who, "compile" + who)
                if who in ("1", "2"): key += "_disk" if b_of.get(id(r), "0") == "1" else "_new"
            over[key] = over.get(key, 0.0) + ov
    desc = ", ".join(f"{k} {v:.0f}ms" for k, v in sorted(over.items(), key=lambda x: -x[1])[:4])
    print(f"  {b/1e6:8.2f}s  {d:6.1f} ms  {desc}")
    shown += 1
    if shown >= 60:
        print("  ...")
        break
