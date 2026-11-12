"""Report for one tools/suite.ps1 run: perf per scene, screenshots against the golden set.

    python tools/suite_report.py <recording dir> [--golden NAME | --make-golden NAME | --calibrate NAME]

Perf: present-to-present times from the run's ftrace (kind 10) per fixed scene window of the
scripted run. Pictures: each shot_<t>s.png against native/goldens/<NAME>/shot_<t>s.png as the mean
absolute difference of 160x90 RGB thumbnails (0..255). Two good runs of the same build never match
exactly (animation timing), so --calibrate records that run-to-run noise per shot, and a shot is
flagged when its difference exceeds max(3 x noise + 3, 6). A flagged shot gets a golden | run |
difference strip in compare_sheet.png. Every run appends one row to native/suite_log.tsv.
Needs Pillow (user site-packages), so it is not run with -I.
"""
import datetime
import json
import os
import re
import subprocess
import sys

from PIL import Image, ImageChops

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GOLDENS = os.path.join(ROOT, "native", "goldens")
# Golden key frames (seconds) and how far from them a matching run frame may lie.
KEYS = [30, 60, 110, 155, 160, 165, 170, 175, 180, 185, 190, 195, 199, 204, 236, 246, 256, 262]
WINDOW = 6
LOG = os.path.join(ROOT, "native", "suite_log.tsv")

# Scene windows of the scripted run (seconds from process start), see PLAN.md.
SCENES = [
    ("title", 22, 34),
    ("intro", 40, 150),
    ("boat", 160, 196),
    ("skyline", 196, 202),
    ("docks", 202, 212),
    ("gameplay", 236, 262),
]


def frame_times(ftrace):
    pres = []
    if not os.path.exists(ftrace):
        return pres
    with open(ftrace) as f:
        for line in f:
            if not line[:1].isdigit():
                continue
            r = line.split()
            if len(r) >= 6 and r[0] == "10":
                pres.append(float(r[5]))
    pres.sort()
    return pres


def scene_stats(pres, t0, t1):
    p = [x for x in pres if t0 * 1e6 <= x < t1 * 1e6]
    if len(p) < 3:
        return None
    d = sorted((p[i] - p[i - 1]) / 1000.0 for i in range(1, len(p)))
    pct = lambda q: d[min(len(d) - 1, int(q / 100.0 * len(d)))]
    return {
        "fps": len(p) / (t1 - t0), "p50": pct(50), "p99": pct(99), "max": d[-1],
        "over17": sum(1 for x in d if x > 17.5), "over33": sum(1 for x in d if x > 33.4),
        "over50": sum(1 for x in d if x > 50.0), "frames": len(d),
    }


def thumb(path):
    return Image.open(path).convert("RGB").resize((160, 90), Image.BILINEAR)


def tdiff(ta, tb):
    h = ImageChops.difference(ta, tb).histogram()
    total = 0
    for ch in range(3):
        total += sum(i * h[ch * 256 + i] for i in range(256))
    return total / (160 * 90 * 3)


def shots_of(d):
    out = {}
    for n in os.listdir(d):
        m = re.match(r"shot_(\d+)s\.png$", n)
        if m:
            out[int(m.group(1))] = os.path.join(d, n)
    return out


def vk_errors(stderr_path):
    if not os.path.exists(stderr_path):
        return []
    bad = []
    with open(stderr_path, errors="replace") as f:
        for line in f:
            if "[RSX vulkan]" in line and re.search(r"failed|VkResult|error|lost", line, re.I) \
                    and "shader cache" not in line:
                bad.append(line.rstrip())
    return bad


def build_id():
    def git(*a):
        try:
            return subprocess.run(["git", *a], capture_output=True, text=True, timeout=20).stdout.strip()
        except Exception:
            return "?"
    # the engine is engine/ in this repository (since v0.5.0); PORT_TK: another engine tree (its own git)
    tk = os.environ.get("PORT_TK") or os.path.join(ROOT, "engine")
    proj = git("-C", ROOT, "rev-parse", "--short", "HEAD")
    tkh = git("-C", tk, "rev-parse", "--short", "HEAD")
    dirty = "+dirty" if git("-C", tk, "status", "--porcelain", "--untracked-files=no", "--", ".") else ""
    return f"{proj}/tk {tkh}{dirty}"


def main():
    a = sys.argv[1:]
    rec = os.path.abspath(a[0])
    mode, name = "golden", "current"
    for flag in ("--golden", "--make-golden", "--calibrate"):
        if flag in a:
            mode, name = flag[2:], a[a.index(flag) + 1]
    tag = os.path.basename(rec)
    lines = []
    say = lambda s="": (print(s), lines.append(s))

    run = {}
    rj = os.path.join(rec, "run.json")
    if os.path.exists(rj):
        try:
            run = json.load(open(rj, encoding="utf-8-sig"))
        except Exception:
            run = {}
    say(f"suite {tag}  build {build_id()}  {datetime.datetime.now():%Y-%m-%d %H:%M}")
    pres = frame_times(os.path.join(rec, "ftrace.txt"))
    say(f"{'scene':9} {'fps':>6} {'p50':>6} {'p99':>6} {'max':>6} {'>17ms':>6} {'>33ms':>6} {'>50ms':>6}")
    row = {}
    for sc, t0, t1 in SCENES:
        st = scene_stats(pres, t0, t1)
        if not st:
            say(f"{sc:9}   (no frames)")
            continue
        row[sc] = st
        say(f"{sc:9} {st['fps']:6.1f} {st['p50']:6.1f} {st['p99']:6.1f} {st['max']:6.1f} "
            f"{st['over17']:6d} {st['over33']:6d} {st['over50']:6d}")
    errs = vk_errors(os.path.join(rec, "stderr.log"))
    say(f"vulkan errors: {len(errs)}" + ("".join("\n  " + e for e in errs[:5])))

    shots = shots_of(rec)
    gdir = os.path.join(GOLDENS, name)
    verdict = "n/a"
    thumbs = {}
    tget = lambda path: thumbs.setdefault(path, thumb(path))
    def best(gpath, t):
        """the run frame within WINDOW s of t that looks most like the golden frame"""
        g = tget(gpath)
        cand = [u for u in shots if abs(u - t) <= WINDOW]
        if not cand:
            return None, None
        sc = {u: tdiff(g, tget(shots[u])) for u in cand}
        u = min(sc, key=sc.get)
        return sc[u], u
    if mode == "make-golden":
        os.makedirs(gdir, exist_ok=True)
        kept = [t for t in KEYS if t in shots]
        for t in kept:
            Image.open(shots[t]).save(os.path.join(gdir, os.path.basename(shots[t])))
        json.dump({"source": tag, "build": build_id(), "made": f"{datetime.datetime.now():%Y-%m-%d %H:%M}",
                   "keys": kept}, open(os.path.join(gdir, "meta.json"), "w"), indent=1)
        say(f"golden set '{name}': {len(kept)} key frames from {tag}")
        verdict = "golden"
    elif os.path.isdir(gdir):
        gold = shots_of(gdir)
        noise_path = os.path.join(gdir, "noise.json")
        noise = json.load(open(noise_path)) if os.path.exists(noise_path) else {}
        scores, flagged, at = {}, [], {}
        say(f"pictures vs golden '{name}' (best match within +-{WINDOW} s):")
        for t in sorted(gold):
            s_, u = best(gold[t], t)
            if s_ is None:
                say(f"  {t:4d}s  MISSING in the run")
                flagged.append(t)
                continue
            scores[t], at[t] = s_, u
            if mode == "calibrate":
                say(f"  {t:4d}s  noise {s_:5.1f}  (run frame {u}s)")
                continue
            lim = max(3.0 * noise.get(str(t), 2.0) + 3.0, 6.0)
            bad = s_ > lim
            if bad:
                flagged.append(t)
            say(f"  {t:4d}s  diff {s_:5.1f}  (limit {lim:4.1f}, run frame {u}s){'  <-- DIFFERENT' if bad else ''}")
        if mode == "calibrate":
            json.dump({str(k): round(v, 2) for k, v in scores.items()}, open(noise_path, "w"), indent=1)
            say(f"noise levels written to {noise_path}")
            verdict = "calibrated"
        else:
            verdict = "pictures: no outlier" if not flagged else f"PICTURES DIFFER at {flagged}"
            # Every key frame goes on the sheet: animation makes two good runs of one build differ
            # by 5-56 (0..255), so the score only flags outliers -- the sheet is read by eye.
            strips = sorted(at)
            sheet = Image.new("RGB", (3 * 427, 240 * max(1, len(strips))))
            for i, t in enumerate(strips):
                g = Image.open(gold[t]).convert("RGB").resize((427, 240))
                c = Image.open(shots[at[t]]).convert("RGB").resize((427, 240))
                dimg = ImageChops.difference(g, c).point(lambda v: min(255, v * 4))
                for j, im in enumerate((g, c, dimg)):
                    sheet.paste(im, (j * 427, i * 240))
            sheet.save(os.path.join(rec, "compare_sheet.png"))
            say(f"{verdict}  (sheet: {os.path.join(rec, 'compare_sheet.png')}; rows {strips})")
    else:
        say(f"no golden set '{name}' (make one with -MakeGolden {name})")

    open(os.path.join(rec, "suite_report.txt"), "w").write("\n".join(lines) + "\n")
    new = not os.path.exists(LOG)
    with open(LOG, "a") as f:
        if new:
            f.write("when\ttag\tbuild\t" + "\t".join(f"{s}_fps\t{s}_p99" for s, _, _ in SCENES) +
                    "\tvk_errors\tpictures\textra\n")
        cols = []
        for s, _, _ in SCENES:
            st = row.get(s)
            cols += [f"{st['fps']:.1f}", f"{st['p99']:.1f}"] if st else ["", ""]
        extra = a[a.index("--extra") + 1] if "--extra" in a else ""
        f.write(f"{datetime.datetime.now():%Y-%m-%d %H:%M}\t{tag}\t{build_id()}\t" + "\t".join(cols) +
                f"\t{len(errs)}\t{verdict}\t{extra}\n")


if __name__ == "__main__":
    main()
