"""A/B statistics for game runs (testspeed agent, 2026-11-08).

Per recording and window: fps, p50/p99 frame ms, RSX drain ms per frame, and the fps of the
slowest 25% of half-second buckets ("low" = what the heavy view is judged by). Then per group
mean, sd, CV, and for two groups the difference with a 95% confidence interval (Welch t), and a
verdict: "B faster / B slower" when the interval excludes 0, "same within +-X%" when it lies
inside the -Same band, else "undecided: N more runs per side" (estimated from the pooled sd).

    python -I tools/ab_stats.py A=tagA1,tagA2 B=tagB1,tagB2 [--win 262:270,322:352] [--same 2]
    python -I tools/ab_stats.py --groups      # find same exe+env groups among recent combat runs: run-to-run noise
Tags are folders under native/recordings (or full paths). Windows default to combat.ps1's.
A recording with a contaminated cpu_watch (run.json) is shown with '!' and left out of the stats
(tools/cpu_watch.ps1 data for the window itself, when present, decides: > 0.5 other cores).
"""
import json, math, os, sys, glob

REC = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "native", "recordings")
WINS = [(262, 270), (322, 352), (354, 378)]
NOISE = {}   # --noise <json>: {"<a>-<b>": {"fps": cv%, "low": cv%}} for the test being compared
T95 = {1: 12.71, 2: 4.30, 3: 3.18, 4: 2.78, 5: 2.57, 6: 2.45, 7: 2.36, 8: 2.31, 9: 2.26, 10: 2.23,
       12: 2.18, 15: 2.13, 20: 2.09, 30: 2.04}


def tq(df):
    df = max(1, int(df))
    for k in sorted(T95):
        if df <= k:
            return T95[k]
    return 1.96


def path_of(tag):
    return tag if os.path.isdir(tag) else os.path.join(REC, tag)


_cache = {}


def load(tag):
    if tag in _cache:
        return _cache[tag]
    rows = []
    f = os.path.join(path_of(tag), "ftrace.txt")
    if os.path.exists(f):
        for l in open(f):
            if l[:1].isdigit():
                p = l.split()
                if len(p) >= 6:
                    rows.append((int(p[0]), float(p[4]), float(p[5])))
    _cache[tag] = rows
    return rows


def offset(tag):
    """Seconds to add to the windows: fast runs (tools/fast.ps1) store their scene anchor in
    run.json 'anchor_s' (the measured windows count from it); combat runs have none."""
    try:
        j = json.load(open(os.path.join(path_of(tag), "run.json"), encoding="utf-8-sig"))
        return float(j.get("anchor_s") or 0)
    except Exception:
        return 0.0


def contam(tag, t0, t1):
    """Average other-process cores during [t0,t1) from cpuwatch.tsv, else None."""
    f = os.path.join(path_of(tag), "cpuwatch.tsv")
    if not os.path.exists(f):
        return None
    v = []
    for l in open(f):
        p = l.split("\t")
        try:
            t, o = float(p[0]), float(p[1])
        except (ValueError, IndexError):
            continue
        if t0 <= t < t1 + 2:
            v.append(o)
    return sum(v) / len(v) if v else None


def invalid(tag):
    """fast.ps1 marks a run invalid (driver timeout, no/false anchor, frozen scene): run.json valid=false."""
    try:
        j = json.load(open(os.path.join(path_of(tag), "run.json"), encoding="utf-8-sig"))
        return None if j.get("valid", True) else (j.get("invalid_reason") or "invalid")
    except Exception:
        return None


def win_stats(tag, a, b):
    if invalid(tag) and "--all" not in sys.argv:
        return None
    off = offset(tag)
    rows = load(tag)
    t0, t1 = (a + off) * 1e6, (b + off) * 1e6
    pres = sorted(e for k, s, e in rows if k == 10 and t0 <= e < t1)
    if len(pres) < 3:
        return None
    dts = sorted((pres[i] - pres[i - 1]) / 1000 for i in range(1, len(pres)))
    pct = lambda q: dts[min(len(dts) - 1, int(q / 100 * len(dts)))]
    drains = [(e - s) / 1000 for k, s, e in rows if k == 7 and t0 <= s < t1]
    bk = {}
    for x in pres:
        bk[int((x - t0) / 5e5)] = bk.get(int((x - t0) / 5e5), 0) + 1
    nb = int((t1 - t0) / 5e5)
    cnt = sorted(bk.get(i, 0) for i in range(nb))
    q = max(1, nb // 4)
    low = sum(cnt[:q]) / (q * 0.5)
    return dict(fps=len(pres) / ((t1 - t0) / 1e6), p50=pct(50), p99=pct(99),
                drain=sum(drains) / max(1, len(pres)), low=low, contam=contam(tag, a + off, b + off))


def mean_sd(v):
    n = len(v)
    m = sum(v) / n
    sd = math.sqrt(sum((x - m) ** 2 for x in v) / (n - 1)) if n > 1 else float("nan")
    return m, sd


def compare(groups, wins, same_pct, metric_list=("fps", "low", "drain")):
    verdicts = []
    for a, b in wins:
        print(f"\n== window {a}-{b} s")
        per = {}
        for g, tags in groups.items():
            for t in tags:
                s = win_stats(t, a, b)
                bad = s and s["contam"] is not None and s["contam"] > 0.5
                flag_only = bad and "--keep-contaminated" in sys.argv   # measure a disturbance on purpose
                if flag_only:
                    bad = False
                if s:
                    flag = "!" if (bad or flag_only) else " "
                    c = "" if s["contam"] is None else f"  other {s['contam']:.2f} cores"
                    print(f"  {g} {flag}{t:16s} fps {s['fps']:6.1f}  low {s['low']:6.1f}  p50 {s['p50']:5.1f}  p99 {s['p99']:5.1f}  drain/f {s['drain']:5.2f}{c}")
                    if not bad:
                        per.setdefault(g, []).append(s)
                else:
                    print(f"  {g}  {t:16s} " + (f"INVALID: {invalid(t)}" if invalid(t) else "(no frames in window)"))
        for m in metric_list:
            line = []
            for g in groups:
                v = [s[m] for s in per.get(g, [])]
                if v:
                    mu, sd = mean_sd(v)
                    line.append(f"{g}: {mu:7.2f} sd {sd:5.2f} cv {100 * sd / mu if mu else 0:4.1f}% n={len(v)}")
            print(f"  {m:5s} " + " | ".join(line))
            gs = list(groups)
            if len(gs) == 2 and all(len(per.get(g, [])) >= 2 for g in gs):
                va, vb = [s[m] for s in per[gs[0]]], [s[m] for s in per[gs[1]]]
                (ma, sa), (mb, sb) = mean_sd(va), mean_sd(vb)
                na, nb_ = len(va), len(vb)
                se = math.sqrt(sa * sa / na + sb * sb / nb_)
                df = (se ** 4) / ((sa * sa / na) ** 2 / (na - 1) + (sb * sb / nb_) ** 2 / (nb_ - 1)) if se > 0 else 1
                d = mb - ma
                h = tq(df) * se
                cal = NOISE.get(f"{a:g}-{b:g}", {}).get(m)
                if cal and max(sa, sb) <= 2.5 * cal * ma / 100:
                    # the test's calibrated run-to-run noise (--noise, >= 6 same-build runs) instead of
                    # an sd from 2-3 runs; a run noisier than 2.5x that falls back to the Welch t above
                    sig = max(math.sqrt((sa * sa + sb * sb) / 2), cal * ma / 100)
                    se = sig * math.sqrt(1 / na + 1 / nb_)
                    h = 1.96 * se
                lo, hi = 100 * (d - h) / ma, 100 * (d + h) / ma
                better = "faster" if m in ("fps", "low") else "slower"
                worse = "slower" if m in ("fps", "low") else "faster"
                if lo > 0:
                    v = f"{gs[1]} {better if m != 'drain' else 'more RSX ms'}"
                elif hi < 0:
                    v = f"{gs[1]} {worse if m != 'drain' else 'less RSX ms'}"
                elif -same_pct <= lo and hi <= same_pct:
                    v = f"same within +-{same_pct}%"
                else:
                    # runs per side so the 95% half-width falls under half the observed |d| or the same band
                    sp = max(math.sqrt((sa * sa + sb * sb) / 2), (cal or 0) * ma / 100)
                    target = max(abs(d) / 2, same_pct * ma / 100)
                    need = math.ceil(2 * (1.96 * sp / target) ** 2) if target > 0 else 99
                    v = f"undecided (need ~{max(need, na + 1)} runs per side)"
                print(f"        {gs[1]}-{gs[0]}: {100 * d / ma:+5.1f}%  95% CI [{lo:+.1f}%, {hi:+.1f}%]  -> {v}")
                verdicts.append((f"{a:g}-{b:g}", m, v))
    return verdicts


def find_groups():
    """Same exe + same env (paths with the tag removed) among recordings with an ftrace."""
    runs = []
    for f in glob.glob(os.path.join(REC, "*", "run.json")):
        try:
            j = json.load(open(f, encoding="utf-8-sig"))
        except Exception:
            continue
        tag = j.get("tag", "")
        if not os.path.exists(os.path.join(os.path.dirname(f), "ftrace.txt")):
            continue
        env = {k: v for k, v in (j.get("env") or {}).items()
               if tag not in str(v) and k not in ("PS3_GAMEDATA_ROOT", "PS3_OVERLAY_INI")}
        key = (j.get("exe", ""), json.dumps(env, sort_keys=True))
        runs.append((j.get("started", ""), tag, key))
    runs.sort()
    groups = {}
    for st, tag, key in runs:
        groups.setdefault(key, []).append((st, tag))
    out = []
    for key, v in groups.items():
        # split by session: a gap > 45 min between runs = a new session (the exe may be rebuilt)
        cur = [v[0]]
        for prev, nxt in zip(v, v[1:]):
            import datetime as dt
            g = (dt.datetime.fromisoformat(nxt[0]) - dt.datetime.fromisoformat(prev[0])).total_seconds()
            if g > 2700:
                out.append(cur); cur = []
            cur.append(nxt)
        out.append(cur)
    return [[t for _, t in g] for g in out if len(g) >= 2]


if __name__ == "__main__":
    args = sys.argv[1:]
    wins = WINS
    same = 2.0
    if "--win" in args:
        w = args[args.index("--win") + 1]
        wins = [tuple(float(x) for x in p.split(":")) for p in w.split(",")]
    if "--noise" in args:
        NOISE.update(json.load(open(args[args.index("--noise") + 1])))
    if "--same" in args:
        same = float(args[args.index("--same") + 1])
    if "--groups" in args:
        for g in find_groups():
            print("same exe+env:", ",".join(g))
            compare({"G": g}, wins, same, ("fps", "low"))
        sys.exit(0)
    groups = {}
    skip = set()
    for i, a in enumerate(args):
        if a in ("--win", "--same", "--metrics", "--noise"):
            skip.add(i + 1)
        if i in skip or a.startswith("--"):
            continue
        g, _, tags = a.partition("=")
        groups[g] = [t for t in tags.split(",") if t]
    mets = tuple(args[args.index("--metrics") + 1].split(",")) if "--metrics" in args else ("fps", "low", "drain")
    vs = compare(groups, wins, same, mets)
    if vs:
        print("\nVERDICTS: " + "; ".join(f"{w} {m}: {v}" for w, m, v in vs))
        print("DECIDED" if all("undecided" not in v for _, _, v in vs) else "UNDECIDED")
