"""The game's trophies as a Recomp Achievements list (RecompLauncher), from your own copy.

RecompLauncher hosts one list per recomp (its SDK's docs/SPEC.md, format 1). For a PS3 game
that is a "native" list: the port reports each trophy unlock with its trophy id (the
engine's libs/misc/ps3_recomp_achievements.c), and the list says what each id is called
and is worth. The names and descriptions are the game's, so this repository does not carry
them: this tool reads them from your copy's TROPHY.TRP and writes the list.

    python -I tools/make_rl_achievements.py <game folder with PS3_GAME, or TROPHY.TRP> [-o list.json]
        [--game <RecompLauncher id>] [--versions ">=0.5.0"] [--lang TROP_01.SFM]

What is decided here (ours, not the game's): the points by grade (bronze 5, silver 10,
gold 25; the platinum is left out, as the format asks: finishing the list is Mastered),
the trophies left out because they need the game's online service (gone: the
user-generated missions), the "win" ones (Beaten: either ending), and the "missable" ones
(one side of a karma choice per playthrough).
"""
import argparse
import json
import os
import re
import struct
import sys

GAME_ID = "infamous2-port"           # RecompLauncher's id for this recomp: check with them
COPIES = ["BCES01143", "BCUS98125", "NPUA80638", "NPEA00318"]
POINTS = {"B": 5, "S": 10, "G": 25}  # their 5/10/25/50 scale by grade
ONLINE_ONLY = {48, 49, 50, 51}       # the UGC trophies: the mission service is gone (README, offline UGC)
WIN = {5, 6}                         # the good and the evil ending: either one is Beaten
MISSABLE = {15, 16, 17, 18, 19, 20}  # one side of a karma choice per playthrough (Kuo / Nix)
COMM_ID = "NPWR01730_00"


def trp_entries(path):
    with open(path, "rb") as f:
        d = f.read()
    magic, _ver, _size, count, esz = struct.unpack_from(">IIQII", d, 0)
    if magic != 0xDCA24D00:
        raise SystemExit("%s is not a TROPHY.TRP" % path)
    out = {}
    for i in range(count):
        e = d[0x40 + i * esz:0x40 + i * esz + 48]
        name = e[:32].rstrip(b"\0").decode("ascii", "replace")
        off, size = struct.unpack_from(">QQ", e, 32)
        out[name] = d[off:off + size]
    return out


def unescape(s):
    s = re.sub(r"&#x([0-9a-fA-F]+);", lambda m: chr(int(m.group(1), 16)), s)
    s = re.sub(r"&#([0-9]+);", lambda m: chr(int(m.group(1))), s)
    return (s.replace("&lt;", "<").replace("&gt;", ">").replace("&quot;", '"')
             .replace("&apos;", "'").replace("&amp;", "&")).strip()


def trophies(sfm):
    x = sfm.decode("utf-8", "replace")
    title = re.search(r"<title-name>(.*?)</title-name>", x, re.S)
    out = []
    for m in re.finditer(r'<trophy\s([^>]*?)(/?)>(.*?)(?:</trophy>|(?=<trophy\s)|$)', x, re.S):
        attrs = dict(re.findall(r'(\w[\w-]*)="([^"]*)"', m.group(1)))
        body = m.group(3) if not m.group(2) else ""
        name = re.search(r"<name>(.*?)</name>", body, re.S)
        detail = re.search(r"<detail>(.*?)</detail>", body, re.S)
        out.append({"id": int(attrs.get("id", "-1")), "grade": attrs.get("ttype", "B")[:1],
                    "name": unescape(name.group(1)) if name else "",
                    "detail": unescape(detail.group(1)) if detail else ""})
    return (unescape(title.group(1)) if title else ""), out


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("source", help="the game folder (the one with PS3_GAME) or its TROPHY.TRP")
    ap.add_argument("-o", "--out", help="the list to write (default: print it)")
    ap.add_argument("--game", default=GAME_ID)
    ap.add_argument("--versions", default=">=0.5.0")
    ap.add_argument("--lang", default="TROP_01.SFM", help="the text file to take names from (English)")
    a = ap.parse_args()
    trp = a.source
    if os.path.isdir(trp):
        trp = os.path.join(trp, "PS3_GAME", "TROPDIR", COMM_ID, "TROPHY.TRP")
    files = trp_entries(trp)
    text = files.get(a.lang) or files.get("TROP.SFM")
    if not text:
        raise SystemExit("no %s or TROP.SFM in %s" % (a.lang, trp))
    title, tl = trophies(text)
    achievements, left_out = [], []
    for t in sorted(tl, key=lambda t: t["id"]):
        if t["grade"] == "P":
            continue
        if t["id"] in ONLINE_ONLY:
            left_out.append(t)
            continue
        e = {"id": t["id"], "title": t["name"], "description": t["detail"],
             "points": POINTS.get(t["grade"], 5)}
        if t["id"] in WIN:
            e["type"] = "win"
        elif t["id"] in MISSABLE:
            e["type"] = "missable"
        achievements.append(e)
    doc = {"format": 1, "game": a.game, "source": "native", "console": "ps3", "copies": COPIES,
           "versions": a.versions, "achievements": achievements}
    s = json.dumps(doc, indent=2, ensure_ascii=False) + "\n"
    if a.out:
        with open(a.out, "w", encoding="utf-8", newline="\n") as f:
            f.write(s)
    else:
        sys.stdout.write(s)
    total = sum(e["points"] for e in achievements)
    print("%s: %d achievements, %d points (left out: the platinum, and online only: %s)"
          % (title, len(achievements), total, ", ".join("%03d %s" % (t["id"], t["name"]) for t in left_out)),
          file=sys.stderr)


if __name__ == "__main__":
    main()
