"""Game-lock accounting (testspeed agent, 2026-11-08): where the machine's game time goes.

    python -I tools/lock_stats.py [--since HH:MM]

Reads work/game.lock.history.tsv (who, queued, got, released; written by with_game_lock.ps1) and
native/recordings/*/run.json (game process start + length) and prints, per worker: turns, time
waiting in the queue, time holding the lock, and how much of the holding time a game was really
running (the rest is builds, analysis or idle under the lock).
"""
import datetime as dt, glob, json, os, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
since = None
if "--since" in sys.argv:
    h, m = sys.argv[sys.argv.index("--since") + 1].split(":")
    since = dt.datetime.now().replace(hour=int(h), minute=int(m), second=0, microsecond=0)
rows = []
hist = os.path.join(ROOT, "work", "game.lock.history.tsv")
if os.path.exists(hist):
    for l in open(hist, encoding="utf-8-sig"):
        p = l.rstrip("\n").split("\t")
        if len(p) == 4:
            try:
                rows.append((p[0], *(dt.datetime.fromisoformat(x) for x in p[1:])))
            except ValueError:
                pass
runs = []
for f in glob.glob(os.path.join(ROOT, "native", "recordings", "*", "run.json")):
    try:
        j = json.load(open(f, encoding="utf-8-sig"))
        st = dt.datetime.fromisoformat(j["started"])
        runs.append((st, dt.datetime.fromtimestamp(os.path.getmtime(f))))
    except Exception:
        pass
if since:
    rows = [r for r in rows if r[2] >= since]
    runs = [r for r in runs if r[0] >= since]
if not rows:
    sys.exit("no lock history yet (work/game.lock.history.tsv)")
per = {}
for who, q, g, r in rows:
    key = who.split()[0]
    game = sum(max(0.0, (min(r, e) - max(g, s)).total_seconds()) for s, e in runs)
    d = per.setdefault(key, [0, 0.0, 0.0, 0.0])
    d[0] += 1; d[1] += (g - q).total_seconds(); d[2] += (r - g).total_seconds(); d[3] += game
span = (max(r[3] for r in rows) - min(r[1] for r in rows)).total_seconds()
held = sum(v[2] for v in per.values())
print(f"{len(rows)} turns over {span / 3600:.2f} h; lock held {held / span * 100:.0f}% of the time")
print(f"{'who':14s} turns  wait_min  hold_min  game_min  game%ofhold")
for k, (n, w, h, g) in sorted(per.items(), key=lambda x: -x[1][2]):
    print(f"{k:14s} {n:5d}  {w / 60:8.1f}  {h / 60:8.1f}  {g / 60:8.1f}  {100 * g / h if h else 0:6.0f}%")
