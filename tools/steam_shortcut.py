"""Add to Steam (the Linux installer, tools/install_gui.sh): a non-Steam game shortcut that starts this folder's game
(tools/play.sh), so it can be played in the Steam Deck's Game Mode, with artwork made from the player's own game files.

    python3 -I tools/steam_shortcut.py users [--steam <Steam folder>]
        one line per Steam user folder: "<id>\t<most recent 0|1>\t<account name>\t<shortcuts.vdf>"
    python3 -I tools/steam_shortcut.py check --user <id> --name <name> --exe <program> [--steam ...]
        "ok" | "name-taken\t<a free name>" | "exe-taken\t<the existing shortcut's name>"   (reads only)
    python3 -I tools/steam_shortcut.py add --user <id> --name <name> --exe <program> --start-dir <folder>
                                           [--art <PS3_GAME folder>] [--steam ...]

The rules (the player's Steam library is theirs):
 - It only ADDS an entry. An existing shortcut is never changed, replaced or removed: the file is read, written back
   byte for byte as it was (checked: a file this script cannot write back exactly is not touched) with the new entry
   appended. A name or program already in the file is refused ("check" says which; the installer offers another name).
 - shortcuts.vdf is copied to shortcuts.vdf.bak-<time> before it is written.
 - Steam keeps the file in memory and writes it when it exits, so the caller writes only while Steam is closed
   (tools/install_gui.sh checks, asks the player to close it, and says to start it again).
 - Artwork (optional, --art): from PS3_GAME/ICON0.PNG and PIC1.PNG with ffmpeg when the system has it, into
   userdata/<id>/config/grid/<app id>{p,,_hero,_icon}.png; never over an existing file.
--steam: the Steam folder (default: PORT_STEAM_ROOT, else ~/.local/share/Steam, else ~/.steam/steam). Tests point it at a
copy; nothing outside it is read or written.
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import time
import zlib


def steam_root(given):
    for d in (given, os.environ.get("PORT_STEAM_ROOT"), os.path.expanduser("~/.local/share/Steam"),
              os.path.expanduser("~/.steam/steam")):
        if d and os.path.isdir(os.path.join(d, "userdata")):
            return os.path.realpath(d)
    return None


# ---- binary VDF (shortcuts.vdf): 0x00 map, 0x01 string, 0x02 int32, 0x03 float32, 0x07 uint64, 0x08 end of map
def _cstr(b, i):
    j = b.index(b"\0", i)
    return b[i:j], j + 1


def parse_map(b, i):
    items = []
    while True:
        t = b[i]
        i += 1
        if t == 0x08:
            return items, i
        key, i = _cstr(b, i)
        if t == 0x00:
            val, i = parse_map(b, i)
        elif t == 0x01:
            val, i = _cstr(b, i)
        elif t in (0x02, 0x03):
            val, i = b[i:i + 4], i + 4
        elif t == 0x07:
            val, i = b[i:i + 8], i + 8
        else:
            raise ValueError(f"unknown field type {t:#x} at {i - 1}")
        items.append((t, key, val))


def ser_map(items):
    out = bytearray()
    for t, key, val in items:
        out += bytes([t]) + key + b"\0"
        if t == 0x00:
            out += ser_map(val)
        elif t == 0x01:
            out += val + b"\0"
        else:
            out += val
    return bytes(out + b"\x08")


EMPTY = b"\x00shortcuts\x00\x08\x08"


def read_shortcuts(path):
    """(top items, the shortcuts list) of a shortcuts.vdf; raises ValueError when it cannot be written back exactly"""
    b = open(path, "rb").read() if os.path.isfile(path) else EMPTY
    if not b:
        b = EMPTY
    top, end = parse_map(b, 0)
    if end != len(b) or ser_map(top) != b:
        raise ValueError("this shortcuts.vdf cannot be read back exactly")
    sc = next((v for t, k, v in top if t == 0x00 and k.lower() == b"shortcuts"), None)
    if sc is None:
        raise ValueError("no 'shortcuts' list in the file")
    return top, sc


def field(entry, name):
    for t, k, v in entry:
        if k.lower() == name.lower().encode():
            return v
    return None


def entries(sc):
    for t, k, v in sc:
        if t == 0x00:
            name = (field(v, "AppName") or b"").decode("utf-8", "replace")
            exe = (field(v, "Exe") or b"").decode("utf-8", "replace").strip().strip('"')
            yield name, exe


def users(root):
    """[(id, most recent, account name, shortcuts.vdf)]: every userdata/<id> folder; the most recent from loginusers.vdf"""
    recent, names, stamps = set(), {}, {}
    try:   # text VDF: "users" { "<steamid64>" { "AccountName" "x" ... "MostRecent" "1" / "timestamp" "<unix time>" } }
        cur = None
        for ln in open(os.path.join(root, "config", "loginusers.vdf"), encoding="utf-8", errors="replace"):
            parts = [p for p in ln.strip().split('"') if p.strip()]
            if len(parts) == 1 and parts[0].isdigit():
                cur = str(int(parts[0]) - 76561197960265728)   # SteamID64 -> account id (the userdata folder's name)
            elif len(parts) == 2 and cur is not None:
                k = parts[0].lower()
                if k == "mostrecent" and parts[1] == "1":
                    recent.add(cur)
                elif k == "accountname":
                    names[cur] = parts[1]
                elif k == "timestamp" and parts[1].isdigit():
                    stamps[cur] = int(parts[1])
    except OSError:
        pass
    if not recent and stamps:   # newer Steam: no MostRecent, the last login's time
        recent.add(max(stamps, key=stamps.get))
    out = []
    ud = os.path.join(root, "userdata")
    for d in sorted(os.listdir(ud)):
        if d.isdigit() and d != "0" and os.path.isdir(os.path.join(ud, d)):
            out.append((d, d in recent, names.get(d, ""), os.path.join(ud, d, "config", "shortcuts.vdf")))
    return out


def app_id(exe, name):
    """the shortcut's app id as Steam (and Steam ROM Manager) computes it: crc32(exe + name) with the top bit set"""
    return (zlib.crc32((exe + name).encode("utf-8")) & 0xFFFFFFFF) | 0x80000000


def free_name(taken, name):
    n = 2
    while f"{name} ({n})".lower() in taken:
        n += 1
    return f"{name} ({n})"


def check(sc, name, exe):
    names = {n.lower() for n, e in entries(sc)}
    for n, e in entries(sc):
        if e and os.path.realpath(e) == os.path.realpath(exe):
            return "exe-taken\t" + n
    if name.lower() in names:
        return "name-taken\t" + free_name(names, name)
    return "ok"


def artwork(ps3_game, grid, aid):
    """grid/<aid>p.png (600x900), <aid>.png (920x430), <aid>_hero.png (1920x620), <aid>_icon.png (256x256) from the
    game's ICON0.PNG (320x176) and PIC1.PNG (1920x1080); returns the icon's path (or "")"""
    icon0, pic1 = os.path.join(ps3_game, "ICON0.PNG"), os.path.join(ps3_game, "PIC1.PNG")
    ff = shutil.which("ffmpeg")
    if not ff or not os.path.isfile(icon0):
        return ""
    os.makedirs(grid, exist_ok=True)
    jobs = []
    if os.path.isfile(pic1):
        jobs += [
            (f"{aid}p.png", [pic1, icon0],
             "[0]scale=-2:900,crop=600:900:(iw-600)*0.8:0[bg];[1]scale=560:-2[ic];[bg][ic]overlay=(W-w)/2:H-h-40"),
            (f"{aid}.png", [pic1, icon0],
             "[0]scale=920:-2,crop=920:430[bg];[1]scale=-2:300[ic];[bg][ic]overlay=(W-w)/2:(H-h)/2"),
            (f"{aid}_hero.png", [pic1], "[0]crop=1920:620:0:(ih-620)/2"),
        ]
    else:
        jobs.append((f"{aid}.png", [icon0], "[0]scale=-2:430,pad=920:430:(ow-iw)/2:0"))
    jobs.append((f"{aid}_icon.png", [icon0], "[0]scale=256:-2,format=rgba,pad=256:256:0:(oh-ih)/2:color=black@0"))
    made = {}
    for out, ins, fc in jobs:
        dst = os.path.join(grid, out)
        if os.path.exists(dst):   # never over an existing picture
            made[out] = dst
            continue
        cmd = [ff, "-loglevel", "error", "-y"]
        for f in ins:
            cmd += ["-i", f]
        cmd += ["-filter_complex", fc, "-frames:v", "1", dst]
        if subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0 and os.path.isfile(dst):
            made[out] = dst
    for out in made:
        print("  artwork: " + made[out])
    return made.get(f"{aid}_icon.png", "")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("what", choices=("users", "check", "add"))
    ap.add_argument("--steam")
    ap.add_argument("--user")
    ap.add_argument("--name")
    ap.add_argument("--exe")
    ap.add_argument("--start-dir")
    ap.add_argument("--art")
    a = ap.parse_args()
    root = steam_root(a.steam)
    if not root:
        sys.exit("no Steam folder (with userdata/) found")
    if a.what == "users":
        for u, recent, name, path in users(root):
            print(f"{u}\t{int(recent)}\t{name}\t{path}")
        return
    if not (a.user and a.name and a.exe):
        sys.exit("--user, --name and --exe are needed")
    path = os.path.join(root, "userdata", a.user, "config", "shortcuts.vdf")
    if not os.path.isdir(os.path.join(root, "userdata", a.user)):
        sys.exit(f"no Steam user folder {a.user}")
    try:
        top, sc = read_shortcuts(path)
    except (ValueError, IndexError, OSError) as e:
        sys.exit(f"Steam's shortcut file could not be read safely, so it was not changed ({e}): {path}")
    c = check(sc, a.name, a.exe)
    if a.what == "check":
        print(c)
        return
    if c != "ok":
        sys.exit("not added: " + c.replace("\t", ": "))
    exe, start = os.path.abspath(a.exe), os.path.abspath(a.start_dir or os.path.dirname(a.exe))
    qexe, qstart = f'"{exe}"', f'"{start}/"'
    aid = app_id(qexe, a.name)
    icon = artwork(a.art, os.path.join(root, "userdata", a.user, "config", "grid"), aid) if a.art else ""
    keys = [int(k) for t, k, v in sc if k.isdigit()]
    i32 = lambda v: struct.pack("<i", v)
    s = lambda v: v.encode("utf-8")
    entry = [(0x02, b"appid", struct.pack("<I", aid)), (0x01, b"AppName", s(a.name)), (0x01, b"Exe", s(qexe)),
             (0x01, b"StartDir", s(qstart)), (0x01, b"icon", s(icon)), (0x01, b"ShortcutPath", b""),
             (0x01, b"LaunchOptions", b""), (0x02, b"IsHidden", i32(0)), (0x02, b"AllowDesktopConfig", i32(1)),
             (0x02, b"AllowOverlay", i32(1)), (0x02, b"OpenVR", i32(0)), (0x02, b"Devkit", i32(0)),
             (0x01, b"DevkitGameID", b""), (0x02, b"DevkitOverrideAppID", i32(0)), (0x02, b"LastPlayTime", i32(0)),
             (0x01, b"FlatpakAppID", b""), (0x00, b"tags", [])]
    before = open(path, "rb").read() if os.path.isfile(path) else b""
    sc.append((0x00, str(max(keys) + 1 if keys else 0).encode(), entry))
    data = ser_map(top)
    # what was there stays byte for byte: the old file minus its last two end marks is the start of the new one
    if before and not data.startswith(before[:-2]):
        sys.exit("internal check failed: the existing shortcuts would not be kept exactly; nothing was written")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    if before:
        bak = path + ".bak-" + time.strftime("%Y%m%d-%H%M%S")
        shutil.copy2(path, bak)
        print("  backup: " + bak)
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, path)
    print(f"  added: \"{a.name}\" (app id {aid}) -> {qexe}, in {path}")


if __name__ == "__main__":
    main()
