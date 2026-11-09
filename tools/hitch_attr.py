r"""What slow frames were spent on (perf agent, 2026-11-09): reads playlog.tsv HITCH lines written by a
build with toolkit agent/perf (fields drain/sema/flush@line/fif/tex/hash/mod/texc/pipe/spv/alloc, see
vk_playlog_attr in libs/video/rsx_vulkan_backend.c) and prints hitch counts, the average attribution of the
frames over 50 ms and the six worst. Works on play\<session> and native\recordings\<tag> folders.
usage: python -I tools/hitch_attr.py <dir> [<rec_dir> ...] [--from T] [--to T]"""
import os, sys
from collections import Counter

args = [a for a in sys.argv[1:] if not a.startswith("--")]
t_from = float(sys.argv[sys.argv.index("--from") + 1]) if "--from" in sys.argv else 0.0
t_to = float(sys.argv[sys.argv.index("--to") + 1]) if "--to" in sys.argv else 1e9
args = [a for a in args if not a.replace(".", "").isdigit()]

def num(v):
    try:
        return float(v)
    except ValueError:
        return 0.0

for d in args:
    secs, hitch = [], []
    for line in open(os.path.join(d, "playlog.tsv"), encoding="utf-8", errors="replace"):
        r = line.rstrip("\n").split("\t")
        if not r or r[0].startswith("#"):
            continue
        if r[0] == "HITCH":
            t, ms = float(r[1]), float(r[2])
            if not (t_from <= t <= t_to):
                continue
            f = dict(kv.split("=", 1) for kv in r[3:] if "=" in kv)
            hitch.append((t, ms, f))
        elif r[0] != "MARK" and len(r) >= 10:
            t = float(r[0])
            if t_from <= t <= t_to:
                secs.append((t, float(r[1]), float(r[4]), float(r[9])))
    if not secs:
        print(d, "no seconds"); continue
    fps = sum(s[1] for s in secs) / len(secs)
    low = sorted(s[1] for s in secs)[: max(1, len(secs) // 10)]
    h50 = [h for h in hitch if h[1] > 50]
    h100 = [h for h in hitch if h[1] > 100]
    worst = max((h[1] for h in hitch), default=0)
    print(f"== {os.path.basename(d)}  [{t_from:.0f}-{min(t_to, secs[-1][0]):.0f} s]  avg {fps:.1f} fps, worst-10% seconds {sum(low)/len(low):.1f} fps, "
          f"frames>25ms {len(hitch)}, >50ms {len(h50)}, >100ms {len(h100)}, worst {worst:.0f} ms, max draws {max(s[3] for s in secs):.0f}")
    # attribution over the >50 ms frames
    agg = Counter()
    for t, ms, f in h50:
        agg["ms"] += ms
        agg["drain"] += num(f.get("drain", "0"))
        s = f.get("sema", "0/0").split("/"); agg["sema"] += num(s[0])
        fl = f.get("flush", "0/0@0").split("@")[0].split("/"); agg["flush_n"] += num(fl[0]); agg["flush"] += num(fl[1]) if len(fl) > 1 else 0
        agg["fif"] += num(f.get("fif", "0"))
        tx = f.get("tex", "0+0/0,0MB,0").split(",")
        nn = tx[0].split("/"); a = nn[0].split("+")
        agg["tex_new"] += num(a[0]); agg["tex_reup"] += num(a[1]) if len(a) > 1 else 0; agg["tex_evict"] += num(nn[1]) if len(nn) > 1 else 0
        agg["tex_mb"] += num(tx[1].replace("MB", "")) if len(tx) > 1 else 0; agg["tex"] += num(tx[2]) if len(tx) > 2 else 0
        agg["hash"] += num(f.get("hash", "0"))
        p = f.get("pipe", "0/0").split("/"); agg["pipe_n"] += num(p[0]); agg["pipe"] += num(p[1])
        sp = f.get("spv", "0/0").split("/"); agg["spv_n"] += num(sp[0]); agg["spv"] += num(sp[1])
        agg["alloc"] += num(f.get("alloc", "0"))
    if h50:
        n = len(h50)
        print("   per >50ms frame (avg): " + ", ".join(f"{k} {agg[k]/n:.1f}" for k in
              ("ms", "drain", "sema", "flush_n", "flush", "fif", "tex_new", "tex_reup", "tex_evict", "tex_mb", "tex", "hash",
               "pipe_n", "pipe", "spv_n", "spv", "alloc")))
        lines = Counter(f.get("flush", "").split("@")[-1] for _, _, f in h50 if not f.get("flush", "0/").startswith("0/"))
        if lines: print("   longest-flush lines:", dict(lines.most_common(5)))
        texc = [num(f.get("texc", "0")) for _, _, f in h50]
        print(f"   textures cached at those frames: {min(texc):.0f}..{max(texc):.0f}")
        for t, ms, f in sorted(h50, key=lambda h: -h[1])[:6]:
            print(f"   {t:7.2f} {ms:6.1f} ms  " + " ".join(f"{k}={v}" for k, v in f.items()))
