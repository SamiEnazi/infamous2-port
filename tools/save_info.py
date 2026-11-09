"""List inFAMOUS 2 saves: each save folder's PARAM.SFO fields, its files and their sizes.

    python -I tools/save_info.py [<savedata dir> | <gamedata dir> | <recording dir>]

Default: Sami's saves (game\\gamedata\\dev_hdd0\\home\\00000001\\savedata). PARAM.SFO is read
in the console's PSF format (what the toolkit writes since 2026-11-08) or the older
"KEY=value" text form. Exit 1 when a save looks broken (no PARAM.SFO, empty file, bad PSF).
"""
import os
import struct
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SUB = os.path.join("dev_hdd0", "home", "00000001", "savedata")


def read_sfo(path):
    b = open(path, "rb").read()
    if b[:4] != b"\0PSF":
        out = {}
        for line in b.decode("utf-8", "replace").splitlines():
            if "=" in line:
                k, v = line.split("=", 1)
                out[k] = v
        return "text", out
    ks, ds, n = struct.unpack_from("<III", b, 8)
    out = {}
    for i in range(n):
        ko, fmt, ln, mx, do = struct.unpack_from("<HHIII", b, 20 + 16 * i)
        key = b[ks + ko:b.index(b"\0", ks + ko)].decode()
        raw = b[ds + do:ds + do + ln]
        if fmt == 0x0404:
            out[key] = struct.unpack("<I", raw[:4])[0]
        elif fmt == 0x0204:
            out[key] = raw.split(b"\0", 1)[0].decode("utf-8", "replace")
        else:
            out[key] = "<%d bytes>" % ln
    return "psf", out


def main():
    arg = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "game", "gamedata")
    for cand in (os.path.join(arg, "gamedata", SUB), os.path.join(arg, SUB), arg):
        if os.path.isdir(cand) and (cand != arg or any(os.path.isdir(os.path.join(cand, d)) for d in os.listdir(cand))):
            root = cand
            break
    else:
        print("no saves under", arg)
        return 0
    bad = 0
    print("saves in", root)
    for d in sorted(os.listdir(root)):
        p = os.path.join(root, d)
        if not os.path.isdir(p):
            continue
        sfo = os.path.join(p, "PARAM.SFO")
        files = sorted(f for f in os.listdir(p) if os.path.isfile(os.path.join(p, f)))
        print("\n%s" % d)
        if not os.path.isfile(sfo):
            print("  BROKEN: no PARAM.SFO (an unfinished save; the game does not list it)")
            bad += 1
        else:
            try:
                kind, f = read_sfo(sfo)
                t = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(os.path.getmtime(sfo)))
                print("  PARAM.SFO (%s), saved %s" % (kind, t))
                for k in ("TITLE", "SUB_TITLE", "DETAIL", "SAVEDATA_DIRECTORY", "SAVEDATA_LIST_PARAM",
                          "LIST_PARAM", "ATTRIBUTE", "PARENTAL_LEVEL", "CATEGORY"):
                    if k in f:
                        v = str(f[k]).replace("\n", " | ")
                        print("    %-20s %s" % (k, v))
            except Exception as e:  # noqa: BLE001 -- report and go on
                print("  BROKEN PARAM.SFO:", e)
                bad += 1
        for fn in files:
            if fn == "PARAM.SFO":
                continue
            sz = os.path.getsize(os.path.join(p, fn))
            note = "  <- EMPTY" if sz == 0 and fn != "PARAM.PFD" else ""
            bad += bool(note)
            print("  %-14s %10d bytes%s" % (fn, sz, note))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
