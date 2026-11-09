"""Automatic picture check (testspeed agent, 2026-11-08): a run's screenshots against reference
shots of the same names, with per-shot noise learned from good runs -- instead of eyeballing.
Needs PIL + numpy: run with plain `python` (not -I).

    python tools/shotcheck.py <rec> --ref <refdir>              compare; exit 1 if any shot is flagged
    python tools/shotcheck.py --make-ref <refdir> <rec>          a good run's shots become the reference
    python tools/shotcheck.py --calibrate <refdir> <rec> [...]   more good runs: per-shot noise -> <refdir>/noise.json

Per shot (both 1280x720 PNGs; recordings dir or tag under native/recordings):
  mean   mean |difference| of 64x36 block colours (0-255): overall change (lighting, tone, missing pass)
  local  98th percentile of 32x18 block colour differences: a local fault (missing object, black box)
  sharp  % change of mean edge strength: blur / lost detail / AA or upscale regressions
  dark   % change of near-black pixels: missing geometry, failed draws
A shot is flagged when a metric exceeds max(2 x its noise + margin, floor). Flagged shots get a
ref | run | diff x4 strip in <rec>/shotcheck_<name>.png; shotcheck.json has every number.
"""
import json, os, sys, glob
import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REC = os.path.join(ROOT, "native", "recordings")
FLOOR = {"mean": 4.0, "local": 14.0, "sharp": 6.0, "dark": 3.0}
MARGIN = {"mean": 1.5, "local": 4.0, "sharp": 2.0, "dark": 1.0}


def rdir(x):
    return x if os.path.isdir(x) else os.path.join(REC, x)


def load(p):
    im = Image.open(p).convert("RGB")
    if im.size != (1280, 720):
        im = im.resize((1280, 720), Image.BILINEAR)
    return np.asarray(im, dtype=np.float32)


def blocks(a, bw, bh):
    h, w, c = a.shape
    return a.reshape(bh, h // bh, bw, w // bw, c).mean(axis=(1, 3))


def edge(a):
    y = a[::2, ::2] @ np.array([0.299, 0.587, 0.114], np.float32)
    gx = np.abs(np.diff(y, axis=1)).mean()
    gy = np.abs(np.diff(y, axis=0)).mean()
    return gx + gy


def metrics(ref, run):
    a, b = load(ref), load(run)
    m = float(np.abs(blocks(a, 64, 36) - blocks(b, 64, 36)).mean())
    d32 = np.abs(blocks(a, 32, 18) - blocks(b, 32, 18)).max(axis=2)
    loc = float(np.percentile(d32, 98))
    ea, eb = edge(a), edge(b)
    sharp = float(100 * abs(eb - ea) / max(ea, 1e-3))
    da = float((a.max(axis=2) < 12).mean() * 100)
    db = float((b.max(axis=2) < 12).mean() * 100)
    return {"mean": round(m, 2), "local": round(loc, 2), "sharp": round(sharp, 2), "dark": round(abs(db - da), 2)}, a, b


def shots(d):
    return {os.path.basename(p)[5:-4]: p for p in glob.glob(os.path.join(d, "shot_*.png"))}


def strip(a, b, out):
    d = np.clip(np.abs(a - b) * 4, 0, 255)
    im = np.concatenate([a, b, d], axis=1).astype(np.uint8)
    Image.fromarray(im).resize((1920, 360), Image.BILINEAR).save(out)


def main():
    args = sys.argv[1:]
    if args and args[0] == "--make-ref":
        ref, rec = args[1], rdir(args[2])
        os.makedirs(ref, exist_ok=True)
        for n, p in shots(rec).items():
            Image.open(p).save(os.path.join(ref, f"shot_{n}.png"))
        json.dump({"from": rec, "noise": {}}, open(os.path.join(ref, "noise.json"), "w"), indent=1)
        print(f"reference {ref}: {len(shots(rec))} shots from {rec}")
        return 0
    if args and args[0] == "--calibrate":
        ref = args[1]
        nz = json.load(open(os.path.join(ref, "noise.json")))
        noise = nz.get("noise", {})
        for rec in args[2:]:
            rs = shots(rdir(rec))
            for n, p in shots(ref).items():
                if n in rs:
                    m, _, _ = metrics(p, rs[n])
                    cur = noise.setdefault(n, {})
                    for k, v in m.items():
                        cur[k] = max(cur.get(k, 0.0), v)
        nz["noise"] = noise
        nz.setdefault("calibrated_with", []).extend(args[2:])
        json.dump(nz, open(os.path.join(ref, "noise.json"), "w"), indent=1)
        for n in sorted(noise):
            print(f"  {n:10s} noise " + "  ".join(f"{k} {v:5.2f}" for k, v in noise[n].items()))
        return 0
    rec = rdir(args[0])
    ref = args[args.index("--ref") + 1]
    nzf = os.path.join(ref, "noise.json")
    noise = json.load(open(nzf)).get("noise", {}) if os.path.exists(nzf) else {}
    rs = shots(rec)
    res, bad = {}, []
    for n, p in sorted(shots(ref).items()):
        if n not in rs:
            print(f"  {n:10s} MISSING in run")
            bad.append(n)
            continue
        m, a, b = metrics(p, rs[n])
        lim = {k: max(2 * noise.get(n, {}).get(k, 0.0) + MARGIN[k], FLOOR[k]) for k in m}
        over = [k for k in m if m[k] > lim[k]]
        res[n] = {"metrics": m, "limits": {k: round(v, 2) for k, v in lim.items()}, "flag": over}
        print(f"  {n:10s} " + "  ".join(f"{k} {m[k]:5.2f}/{lim[k]:5.1f}" for k in m) + ("   FLAG " + ",".join(over) if over else "   ok"))
        if over:
            bad.append(n)
            strip(a, b, os.path.join(rec, f"shotcheck_{n}.png"))
    json.dump({"ref": ref, "shots": res, "flagged": bad}, open(os.path.join(rec, "shotcheck.json"), "w"), indent=1)
    print(f"picture check: {len(res)} shots, {len(bad)} flagged" + (f": {', '.join(bad)} (strips in {rec})" if bad else ""))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
