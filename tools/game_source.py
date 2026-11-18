"""Setup's one pick (v0.6.0): the player's game as a folder, a disc image (.iso) or a PSN package (.pkg), made into
the port's own copy in game/disc. Python's standard library only; Windows and Linux.

    python -I tools/game_source.py info <folder|.iso|.pkg> [--json]
    python -I tools/game_source.py import <folder|.iso|.pkg> --game <port>/game [--ps3tool tools/ps3tool.py]
        [--python <python for ps3tool>] [--disc-key <file>] [--rap <file>]
    python -I tools/game_source.py modes --ps3tool tools/ps3tool.py [--python <python for ps3tool>]

info: what the pick is, as one JSON object: kind (folder | iso | pkg), layout (disc | psn), title (BCES01143 ...),
  name, app_ver, version, region, encrypted (an image whose files are still encrypted, or a package), needs
  (["disc-key"] for an encrypted image, ["rap"] for a PSN copy: its license), bytes, problem (why it can't be used).
modes: one line "modes: <word> <word> ..." with what this ps3tool offers, read from its own --help (pkg, pup,
  decrypt, iso, and an option as <mode>--<option>, e.g. iso--dkey), plus "psn-rap" when it can unlock a PSN copy
  with the player's license: the ps3tool that has the iso mode looks the .rap up itself (beside the file, in the
  folder PS3_RAP_DIR names, in RPCS3's exdata; it has no --rap option), an older one can't. Setup asks before it
  asks the player for a file and before it copies anything large: a file the tool can't use is never asked for.
import: the game's files copied (a folder) or extracted (an image, a package) INTO game/disc, so the port's folder can
  be moved or copied anywhere and still plays (no link to the player's folder). A disc keeps its layout
  (game/disc/PS3_GAME, PS3_DISC.SFB ...); a PSN copy's title folder becomes game/disc/PS3_GAME and is also the game's
  update folder game/dev_hdd0/game/<title> (the same files, hard links: no second copy on the disk).
  Checked before: free space. Done again only where it stopped (game/disc/.port-source.json is written last).
  Progress lines for Setup: "  progress: <percent>% <what>". Exit code 0 = done, 1 = failed (last line "error: <why>").
Keys and licenses: an encrypted image needs its disc key and a PSN package its license (.rap). Their files are only
  passed on, by path, to tools/ps3tool.py (Sami Enazi's, a black box here: python -I ps3tool.py <mode> <in> <out>);
  nothing here reads them. A ps3tool without such a mode says so, and the player is told what to do instead.
"""
import argparse
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time

KNOWN = {
    "BCES01143": ("Europe", "disc"), "BCES01144": ("Europe", "disc"), "BCES01229": ("Turkey", "disc"),
    "BCUS98125": ("USA", "disc"), "NPUA80638": ("USA", "psn"), "NPEA00318": ("Europe", "psn"),
}
SECTOR = 2048
# (said wherever an encrypted image is refused: tools/setup_common.ps1 and setup_build.ps1 have the same words)
ISO_CANT = ("this disc image is encrypted (a raw copy of the disc), and the ps3tool in this folder is an older one "
            "that can't decrypt disc images. Run Setup again to get the newest port, or choose your game's decrypted "
            "folder (the one that holds PS3_GAME) or a decrypted .iso instead")
MARK = ".port-source.json"


class Fail(Exception):
    pass


def say(s):
    print(s, flush=True)


def gb(n):
    return f"{n / (1 << 30):.1f} GB"


class Progress:
    def __init__(self, total, what):
        self.total, self.done, self.what, self.last = max(1, total), 0, what, 0.0

    def add(self, n):
        self.done += n
        now = time.time()
        if now - self.last >= 1.0:
            self.last = now
            say(f"  progress: {min(99, int(100 * self.done / self.total))}% {self.what} ({gb(self.done)} of {gb(self.total)})")


# ------------------------------------------------------------------------------------------------ PARAM.SFO

def sfo_parse(b):
    if b[:4] != b"\0PSF":
        return None
    keys, data, n = struct.unpack_from("<III", b, 8)
    out = {}
    for i in range(n):
        ko, fmt, ln, _mx, off = struct.unpack_from("<HHIII", b, 20 + 16 * i)
        k = b[keys + ko:b.index(b"\0", keys + ko)].decode("ascii", "replace")
        v = b[data + off:data + off + ln]
        out[k] = struct.unpack("<I", v[:4])[0] if fmt == 0x0404 else v.rstrip(b"\0").decode("utf-8", "replace")
    return out


def sfo_file(path):
    try:
        with open(path, "rb") as f:
            return sfo_parse(f.read(1 << 16))
    except (OSError, struct.error, ValueError):
        return None


# ------------------------------------------------------------------------------------------------ ISO 9660

class Iso:
    """A disc image: ISO 9660, its Joliet names when there are (the game's real, lower-case names), files over
    several extents (ISO 9660 level 3) joined. Read only."""

    def __init__(self, path):
        self.path = path
        self.f = open(path, "rb")
        self.size = os.path.getsize(path)
        root = joliet = None
        for lba in range(16, 64):
            d = self.read(lba * SECTOR, SECTOR)
            if len(d) < SECTOR or d[1:6] != b"CD001":
                break
            t = d[0]
            if t == 255:
                break
            if t == 1 and root is None:
                root = d[156:156 + 34]
            elif t == 2 and d[88:91] in (b"%/@", b"%/C", b"%/E"):
                joliet = d[156:156 + 34]
        if root is None:
            raise Fail(f"{os.path.basename(path)} is not a disc image this Setup can read (no ISO 9660 volume in it)")
        self.joliet = joliet is not None
        self.root = self.record(joliet or root)

    def close(self):
        self.f.close()

    def read(self, off, n):
        self.f.seek(off)
        return self.f.read(n)

    def record(self, r):
        ln = r[0]
        lba = struct.unpack_from("<I", r, 2)[0]
        size = struct.unpack_from("<I", r, 10)[0]
        flags = r[25]
        nl = r[32]
        raw = r[33:33 + nl]
        if raw in (b"\0", b"\1"):
            name = raw.decode("latin-1")
        elif self.__dict__.get("joliet"):
            name = raw.decode("utf-16-be", "replace")
        else:
            name = raw.decode("latin-1")
        name = name.split(";", 1)[0]
        if not (flags & 2) and name.endswith("."):
            name = name[:-1]
        return {"name": name, "lba": lba, "size": size, "dir": bool(flags & 2), "more": bool(flags & 0x80), "len": ln}

    def listdir(self, d):
        """[(name, is_dir, [(lba, size), ...])] of a directory record"""
        data = self.read(d["lba"] * SECTOR, d["size"])
        out, pos, pending = [], 0, None
        while pos < len(data):
            ln = data[pos]
            if ln == 0:
                pos = (pos // SECTOR + 1) * SECTOR      # records never cross a sector: the rest is padding
                continue
            e = self.record(data[pos:pos + ln])
            pos += ln
            if e["name"] in ("\0", "\1"):
                continue
            if pending and pending[0] == e["name"]:
                pending[2].append((e["lba"], e["size"]))
            else:
                if pending:
                    out.append(pending)
                pending = (e["name"], e["dir"], [(e["lba"], e["size"])], e)
            if not e["more"]:
                out.append(pending)
                pending = None
        if pending:
            out.append(pending)
        return out

    def walk(self):
        """(relative path with /, is_dir, extents) of everything, folders first"""
        stack = [("", self.root)]
        while stack:
            rel, d = stack.pop()
            for name, is_dir, ext, rec in self.listdir(d):
                p = f"{rel}/{name}" if rel else name
                yield p, is_dir, ext
                if is_dir:
                    stack.append((p, rec))

    def find(self, rel):
        d = self.root
        parts = [x for x in rel.split("/") if x]
        for i, part in enumerate(parts):
            hit = None
            for name, is_dir, ext, rec in self.listdir(d):
                if name.lower() == part.lower():
                    hit = (is_dir, ext, rec)
                    break
            if not hit:
                return None
            if i == len(parts) - 1:
                return hit
            if not hit[0]:
                return None
            d = hit[2]
        return None

    def head(self, rel, n=4096):
        hit = self.find(rel)
        if not hit or hit[0]:
            return None
        lba, size = hit[1][0]
        return self.read(lba * SECTOR, min(n, size))


# ------------------------------------------------------------------------------------------------ PKG

def pkg_header(path):
    """A PS3 package's content id (plain text in its header: "UP9000-NPUA80638_00-..."), or None."""
    try:
        with open(path, "rb") as f:
            h = f.read(0x60)
    except OSError:
        return None
    if h[:4] != b"\x7fPKG":
        return None
    return h[0x30:0x30 + 0x24].split(b"\0", 1)[0].decode("ascii", "replace")


# ------------------------------------------------------------------------------------------------ info

def tree_bytes(top):
    n = 0
    for dp, _dn, fn in os.walk(top):
        for f in fn:
            if f in (MARK, MARK + ".part") or f.endswith(".part"):
                continue
            try:
                n += os.path.getsize(os.path.join(dp, f))
            except OSError:
                pass
    return n


def game_root(folder):
    """(the folder playing PS3_GAME's part, layout) of a game folder: <folder>/PS3_GAME (a disc), the folder itself (a
    PSN title folder: PARAM.SFO + USRDIR/EBOOT.BIN), PS3_GAME picked itself, or one folder down; else (None, None)"""
    f = os.path.abspath(folder)
    if os.path.basename(f).upper() == "PS3_GAME" and os.path.isfile(os.path.join(f, "PARAM.SFO")):
        f = os.path.dirname(f)
    if os.path.isfile(os.path.join(f, "PS3_GAME", "PARAM.SFO")):
        return os.path.join(f, "PS3_GAME"), "disc"
    s = sfo_file(os.path.join(f, "PARAM.SFO"))
    if s and str(s.get("TITLE_ID", "")).startswith("NP") and os.path.isfile(os.path.join(f, "USRDIR", "EBOOT.BIN")):
        return f, "psn"
    try:
        for d in sorted(os.listdir(f)):
            g = os.path.join(f, d)
            if os.path.isdir(g) and (os.path.isfile(os.path.join(g, "PS3_GAME", "PARAM.SFO")) or
                                     os.path.isfile(os.path.join(g, "USRDIR", "EBOOT.BIN"))):
                r = game_root(g)
                if r[0]:
                    return r
    except OSError:
        pass
    return None, None


def describe(sfo, out):
    t = str(sfo.get("TITLE_ID", ""))
    out["title"] = t
    out["name"] = sfo.get("TITLE", "")
    out["app_ver"] = sfo.get("APP_VER", "")
    out["version"] = sfo.get("VERSION", "")
    reg, lay = KNOWN.get(t, ("", ""))
    out["region"] = reg
    if lay:
        out["layout"] = lay
    if t not in KNOWN:
        out["problem"] = f"this is {out['name'] or 'another game'} [{t}], not inFAMOUS 2"


def info(path):
    out = {"path": os.path.abspath(path), "kind": "", "layout": "", "title": "", "name": "", "app_ver": "",
           "version": "", "region": "", "encrypted": False, "needs": [], "bytes": 0, "problem": ""}
    if os.path.isdir(path):
        out["kind"] = "folder"
        root, lay = game_root(path)
        if not root:
            out["problem"] = ("no game in this folder: choose the folder that holds PS3_GAME (a disc), the PSN version's "
                              "NPUA80638 / NPEA00318 folder, an .iso file or a .pkg file")
            return out
        out["game_root"] = root
        out["source"] = os.path.dirname(root) if lay == "disc" else root
        out["layout"] = lay
        s = sfo_file(os.path.join(root, "PARAM.SFO"))
        if not s:
            out["problem"] = "its PARAM.SFO can't be read"
            return out
        describe(s, out)
        out["bytes"] = tree_bytes(out["source"])
        if lay == "disc":
            hdr = b""
            try:
                with open(os.path.join(root, "USRDIR", "cache", "psarc", "core.psarc"), "rb") as f:
                    hdr = f.read(4)
            except OSError:
                pass
            if hdr and hdr != b"PSAR":
                out["encrypted"] = True
                out["problem"] = ("the game's files in this folder are still encrypted (core.psarc doesn't start like a game "
                                  "archive): the port needs a decrypted copy, the kind RPCS3 uses for a disc game")
        else:
            out["needs"] = ["rap"]
        return out
    if not os.path.isfile(path):
        out["problem"] = "there is no such file or folder"
        return out
    low = path.lower()
    with open(path, "rb") as f:
        magic = f.read(4)
    if magic == b"\x7fPKG" or low.endswith(".pkg"):
        out["kind"] = "pkg"
        cid = pkg_header(path)
        if not cid:
            out["problem"] = "this is not a PS3 package (.pkg)"
            return out
        out["content_id"] = cid
        t = cid[7:16] if len(cid) >= 16 else ""
        out["title"] = t
        out["bytes"] = os.path.getsize(path)
        out["encrypted"] = True
        reg, lay = KNOWN.get(t, ("", ""))
        out["region"], out["layout"] = reg, lay or "psn"
        if t not in KNOWN:
            out["problem"] = f"this package is {cid}, not inFAMOUS 2"
        elif lay == "disc":
            # (an update and a DLC package of a disc title carry the same content-id title: not told apart here)
            out["problem"] = ("this is an update or DLC package of the disc version, not the game: choose your game (the "
                              "disc's folder or .iso) instead. Setup gets the updates by itself. For DLC: install the package "
                              "in RPCS3 and run Setup again, it copies the DLC from there; or put the DLC's unlock files "
                              "(*.edat) in the port's dlc folder")
        else:
            out["needs"] = ["rap"]
        return out
    out["kind"] = "iso"
    out["layout"] = "disc"
    out["bytes"] = os.path.getsize(path)
    try:
        iso = Iso(path)
    except Fail as e:
        out["problem"] = str(e)
        return out
    try:
        sfo_raw = iso.head("PS3_GAME/PARAM.SFO", 1 << 16)
        if sfo_raw is None:
            out["problem"] = "this disc image has no PS3_GAME/PARAM.SFO: it is not a PS3 game disc"
            return out
        s = sfo_parse(sfo_raw)
        core = iso.head("PS3_GAME/USRDIR/cache/psarc/core.psarc", 4)
        if not s or (core is not None and core != b"PSAR"):
            out["encrypted"] = True
            out["needs"] = ["disc-key"]
        if s:
            describe(s, out)
        out["joliet"] = iso.joliet
    finally:
        iso.close()
    return out


# ------------------------------------------------------------------------------------------------ import

def free_bytes(path):
    p = path
    while p and not os.path.exists(p):
        p = os.path.dirname(p)
    return shutil.disk_usage(p or ".").free


def check_space(game, need):
    # the game's files, then about 3 GB for the update, the game's own HDD install and the build files
    want = need + (3 << 30)
    have = free_bytes(game)
    if have < want:
        raise Fail(f"not enough free space for your game's files: they need {gb(need)} (and about 3 GB more for the build), "
                   f"the drive has {gb(have)} free. Free some space, or put the port's folder on another drive")


def mark_ok(disc, src):
    try:
        with open(os.path.join(disc, MARK), encoding="utf-8") as f:
            m = json.load(f)
        return m.get("complete") and m.get("bytes") == src.get("bytes") and m.get("title") == src.get("title")
    except (OSError, ValueError):
        return False


def write_mark(disc, src):
    m = {k: src.get(k) for k in ("kind", "layout", "title", "app_ver", "version", "region", "bytes")}
    m["from"] = os.path.basename(src.get("path", ""))   # only the name: no path of the player's PC is kept
    m["complete"] = True
    m["at"] = time.strftime("%Y-%m-%d %H:%M:%S")
    with open(os.path.join(disc, MARK + ".part"), "w", encoding="utf-8") as f:
        json.dump(m, f, indent=1)
    os.replace(os.path.join(disc, MARK + ".part"), os.path.join(disc, MARK))


def same_file(a, b):
    try:
        return os.path.getsize(a) == os.path.getsize(b)
    except OSError:
        return False


def copy_file(src, dst, prog):
    if os.path.isfile(dst) and same_file(src, dst):
        prog.add(os.path.getsize(src))
        return
    tmp = dst + ".part"
    with open(src, "rb") as fi, open(tmp, "wb") as fo:
        while True:
            b = fi.read(8 << 20)
            if not b:
                break
            fo.write(b)
            prog.add(len(b))
    shutil.copystat(src, tmp)
    os.replace(tmp, dst)


def copy_tree(src, dst, prog):
    for dp, dn, fn in os.walk(src):
        dn[:] = [d for d in dn if not os.path.islink(os.path.join(dp, d))]
        rel = os.path.relpath(dp, src)
        out = dst if rel == "." else os.path.join(dst, rel)
        os.makedirs(out, exist_ok=True)
        for f in fn:
            if f in (MARK, MARK + ".part"):
                continue
            copy_file(os.path.join(dp, f), os.path.join(out, f), prog)


def link_tree(src, dst):
    """dst gets src's files as hard links (the same file twice, no second copy on the disk); a copy where the drive
    can't link"""
    n = 0
    for dp, _dn, fn in os.walk(src):
        rel = os.path.relpath(dp, src)
        out = dst if rel == "." else os.path.join(dst, rel)
        os.makedirs(out, exist_ok=True)
        for f in fn:
            if f in (MARK, MARK + ".part"):
                continue
            s, d = os.path.join(dp, f), os.path.join(out, f)
            if os.path.isfile(d):
                if same_file(s, d):
                    continue
                os.remove(d)
            try:
                os.link(s, d)
            except OSError:
                shutil.copy2(s, d)
            n += 1
    return n


def extract_iso(iso_path, dst, prog):
    iso = Iso(iso_path)
    try:
        for rel, is_dir, ext in iso.walk():
            out = os.path.join(dst, *rel.split("/"))
            if is_dir:
                os.makedirs(out, exist_ok=True)
                continue
            os.makedirs(os.path.dirname(out), exist_ok=True)
            size = sum(s for _l, s in ext)
            if os.path.isfile(out) and os.path.getsize(out) == size:
                prog.add(size)
                continue
            tmp = out + ".part"
            with open(tmp, "wb") as fo:
                for lba, s in ext:
                    iso.f.seek(lba * SECTOR)
                    left = s
                    while left > 0:
                        b = iso.f.read(min(8 << 20, left))
                        if not b:
                            raise Fail(f"the disc image ends inside {rel}: it is incomplete")
                        fo.write(b)
                        left -= len(b)
                        prog.add(len(b))
            os.replace(tmp, out)
    finally:
        iso.close()


def ps3tool_modes(py, tool):
    """the modes and options ps3tool offers (its --help), e.g. {'pkg', 'pup', 'decrypt', '--rap'}"""
    have = set()
    try:
        p = subprocess.run([py, "-I", tool, "--help"], capture_output=True, text=True, errors="replace", timeout=60)
        txt = p.stdout + p.stderr
        if "{" in txt:
            have |= set(txt.split("{", 1)[1].split("}", 1)[0].split(","))
        for m in ("decrypt", "pkg", "iso"):
            q = subprocess.run([py, "-I", tool, m, "--help"], capture_output=True, text=True, errors="replace", timeout=60)
            if q.returncode == 0:
                for w in (q.stdout + q.stderr).replace(",", " ").split():
                    if w.startswith("--"):
                        have.add(m + w)
    except (OSError, subprocess.SubprocessError):
        pass
    have = {h.strip() for h in have}
    if "decrypt--rap" in have or "iso" in have:
        have.add("psn-rap")   # (see "modes" at the top)
    return have


def head_ci(top, parts, n):
    """the first n bytes of top/<parts...>, the names matched without regard to case (an image's ISO 9660 names
    are upper case); None when there is no such file"""
    d = top
    for i, part in enumerate(parts):
        try:
            hit = [x for x in os.listdir(d) if x.lower() == part.lower()]
        except OSError:
            return None
        if not hit:
            return None
        d = os.path.join(d, hit[0])
    try:
        with open(d, "rb") as f:
            return f.read(n)
    except OSError:
        return None


def run_ps3tool(py, tool, args, what, env=None, watch=None):
    """ps3tool as a program. env: variables for it (PS3_RAP_DIR). watch = (folder, bytes expected, words): a
    "progress:" line every 2 seconds from how much it has written there (it prints nothing while it works)."""
    t0 = time.time()
    e = dict(os.environ)
    e.update(env or {})
    with tempfile.TemporaryFile(mode="w+", encoding="utf-8", errors="replace") as out:
        p = subprocess.Popen([py, "-I", tool] + args, stdout=out, stderr=subprocess.STDOUT, env=e)
        try:
            beat = time.time()
            while True:
                try:
                    p.wait(timeout=0.5)
                    break
                except subprocess.TimeoutExpired:
                    pass
                if watch and time.time() - beat >= 2.0:
                    beat = time.time()
                    have = tree_bytes(watch[0]) if os.path.isdir(watch[0]) else 0
                    pct = min(99, int(100 * have / max(1, watch[1])))
                    say(f"  progress: {pct}% {watch[2]} ({gb(have)} of {gb(watch[1])})")
        except BaseException:
            p.kill()
            raise
        out.seek(0)
        text = out.read()
    if p.returncode != 0:
        tail = text.strip().splitlines()[-6:]
        raise Fail(f"ps3tool could not unpack {what} (exit code {p.returncode}): " + " | ".join(tail))
    say(f"  {what}: unpacked ({time.time() - t0:.0f} s)")


def do_import(a):
    src = info(a.source)
    if src["problem"]:
        raise Fail(src["problem"])
    game = os.path.abspath(a.game)
    disc = os.path.join(game, "disc")
    if os.path.islink(disc) or (os.name == "nt" and os.path.isdir(disc) and is_junction(disc)):
        raise Fail(f"{disc} is a link to another folder: Setup replaces it with a copy itself (setup_build), not here")
    title = src["title"]
    os.makedirs(game, exist_ok=True)
    if src["kind"] == "folder" and os.path.normcase(os.path.abspath(src["source"])) in (os.path.normcase(disc),
                                                                                      os.path.normcase(os.path.join(disc, "PS3_GAME"))):
        # the port's own copy (an update, a moved folder): nothing to copy
        if not os.path.isfile(os.path.join(disc, MARK)):
            write_mark(disc, src)
        if src["layout"] == "psn":
            link_tree(os.path.join(disc, "PS3_GAME"), os.path.join(game, "dev_hdd0", "game", title))
        say(f"  your game: in game/disc ({title}, {gb(src['bytes'])})")
        return 0
    try:
        with open(os.path.join(disc, MARK), encoding="utf-8") as f:
            had = json.load(f).get("title")
        if had and had != title:
            say(f"  game/disc held another copy ({had}): replaced by this one ({title})")
            shutil.rmtree(disc)
    except (OSError, ValueError):
        pass
    if mark_ok(disc, src):
        say(f"  your game: already in game/disc ({src['kind']}, {title}, {gb(src['bytes'])})")
        if src["layout"] == "psn":
            link_tree(os.path.join(disc, "PS3_GAME"), os.path.join(game, "dev_hdd0", "game", title))
        return 0
    os.makedirs(disc, exist_ok=True)
    have = tree_bytes(disc)
    check_space(game, max(0, src["bytes"] - have))
    t0 = time.time()
    if src["kind"] == "folder":
        say(f"  copying your game into the port's folder ({gb(src['bytes'])}): from then on the port needs nothing outside its folder")
        prog = Progress(src["bytes"], "copying your game")
        if src["layout"] == "disc":
            copy_tree(src["source"], disc, prog)
        else:
            copy_tree(src["source"], os.path.join(disc, "PS3_GAME"), prog)
    elif src["kind"] == "iso":
        if src["encrypted"]:
            tool = a.ps3tool
            modes = ps3tool_modes(a.python, tool) if tool and os.path.isfile(tool) else set()
            # (first whether it can be done at all: a key the tool can't use is never asked for)
            if "iso" not in modes:
                raise Fail(ISO_CANT)
            # The disc key: the file (or 32 hex digits) the player gave, else ps3tool finds it itself (<name>.dkey /
            # .key / .ird beside the image, or inside a 3K3Y image). Only when it finds none is the player asked.
            key = []
            if a.disc_key:
                opt = "--dkey" if "iso--dkey" in modes else "--key" if "iso--key" in modes else ""
                if not opt:
                    raise Fail("this ps3tool's iso mode takes no disc key option (its --help names none)")
                key = [opt, a.disc_key]
            out = os.path.join(game, "disc.unpack")
            shutil.rmtree(out, ignore_errors=True)
            say(f"  decrypting your disc image into the port's folder with ps3tool ({gb(src['bytes'])})")
            try:
                run_ps3tool(a.python, tool, ["iso"] + key + [a.source, out], "your disc image",
                            watch=(out, src["bytes"], "decrypting your disc image"))
            except Fail as e:
                shutil.rmtree(out, ignore_errors=True)
                if not a.disc_key:
                    raise Fail("this disc image is encrypted and needs its disc key: a .dkey or .ird file (none was "
                               f"beside the image or in it). {e}")
                raise
            # what came out must be the game's readable files: a wrong key gives files that are still scrambled
            magic = head_ci(out, ("PS3_GAME", "USRDIR", "cache", "psarc", "core.psarc"), 4)
            if magic is not None and magic != b"PSAR":
                shutil.rmtree(out, ignore_errors=True)
                raise Fail("ps3tool unpacked this disc image, but the game's files in it are still unreadable: "
                           + ("this is not the disc key of this disc" if a.disc_key else
                              "it needs its disc key (a .dkey or .ird file)"))
            # into game/disc: a rename on the same drive (no second copy of 9 GB), else copied
            try:
                if os.path.isdir(disc) and not os.listdir(disc):
                    os.rmdir(disc)
                os.replace(out, disc)
            except OSError:
                os.makedirs(disc, exist_ok=True)
                copy_tree(out, disc, Progress(tree_bytes(out), "copying your game"))
                shutil.rmtree(out, ignore_errors=True)
        else:
            say(f"  extracting your disc image into the port's folder ({gb(src['bytes'])})")
            extract_iso(a.source, disc, Progress(src["bytes"], "extracting your disc image"))
    elif src["kind"] == "pkg":
        tool = a.ps3tool
        if not tool or not os.path.isfile(tool):
            raise Fail("unpacking a .pkg needs tools/ps3tool.py, and it is missing")
        out = os.path.join(game, "pkg.unpack")
        shutil.rmtree(out, ignore_errors=True)
        say(f"  unpacking your PSN package with ps3tool ({gb(src['bytes'])}; a few minutes)")
        # the player's license: ps3tool looks it up itself, in the folder PS3_RAP_DIR names (the folder of the .rap
        # the player chose, else the package's own folder) and where RPCS3 keeps licenses
        rap_dir = os.path.dirname(os.path.abspath(a.rap)) if a.rap else os.path.dirname(os.path.abspath(a.source))
        run_ps3tool(a.python, tool, ["pkg", a.source, out], "your PSN package", env={"PS3_RAP_DIR": rap_dir},
                    watch=(out, src["bytes"], "unpacking your PSN package"))
        root, lay = game_root(out)
        if not root or lay != "psn":
            raise Fail("the package unpacked, but it holds no PSN game folder (PARAM.SFO + USRDIR/EBOOT.BIN)")
        s = sfo_file(os.path.join(root, "PARAM.SFO")) or {}
        describe(s, src)
        if src.get("problem"):
            raise Fail(src["problem"])
        dst = os.path.join(disc, "PS3_GAME")
        if os.path.isdir(dst):
            shutil.rmtree(dst)
        os.makedirs(disc, exist_ok=True)
        os.replace(root, dst)
        shutil.rmtree(out, ignore_errors=True)
        src["bytes"] = tree_bytes(dst)
    else:
        raise Fail("Setup can't use this: " + (src["problem"] or src["kind"]))
    if src["layout"] == "psn":
        n = link_tree(os.path.join(disc, "PS3_GAME"), os.path.join(game, "dev_hdd0", "game", title))
        say(f"  the PSN copy is also its own update folder: game/dev_hdd0/game/{title} ({n} files linked, no second copy)")
    if src["kind"] == "pkg":
        src["bytes_source"] = src["bytes"]
    write_mark(disc, src)
    say(f"  progress: 100% your game is in the port's folder")
    say(f"  your game: game/disc ({src['kind']}, {title} {src['region']}, {gb(tree_bytes(disc))}) in {time.time() - t0:.0f} s")
    return 0


def is_junction(path):
    try:
        import stat
        st = os.lstat(path)
        return bool(getattr(st, "st_file_attributes", 0) & 0x400)   # FILE_ATTRIBUTE_REPARSE_POINT
    except OSError:
        return False


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    i = sub.add_parser("info")
    i.add_argument("source")
    i.add_argument("--json", action="store_true")
    m = sub.add_parser("import")
    m.add_argument("source")
    m.add_argument("--game", required=True)
    m.add_argument("--ps3tool")
    m.add_argument("--python", default=sys.executable)
    m.add_argument("--disc-key")
    m.add_argument("--rap")
    t = sub.add_parser("modes")
    t.add_argument("--ps3tool", required=True)
    t.add_argument("--python", default=sys.executable)
    a = ap.parse_args()
    try:
        if a.cmd == "modes":
            print("modes: " + " ".join(sorted(ps3tool_modes(a.python, a.ps3tool) if os.path.isfile(a.ps3tool) else [])))
            return 0
        if a.cmd == "info":
            r = info(a.source)
            print(json.dumps(r, indent=None if a.json else 1))
            return 0
        return do_import(a)
    except Fail as e:
        say(f"error: {e}")
        return 1
    except OSError as e:
        say(f"error: {e}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
