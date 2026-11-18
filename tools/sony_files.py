"""The game's update and the PS3 firmware files without RPCS3: downloaded from Sony's own servers (the
same files a PS3 downloads) and unpacked with tools/ps3tool.py. Windows and Linux; Python's standard
library only (ps3tool runs much faster when the Python running it has pycryptodome: Setup's work/venv).

    python tools/sony_files.py ps3tool                  tools/ps3tool.py is there (downloaded if missing)
    python tools/sony_files.py update --disc <folder>   the 1.04 update -> game/dev_hdd0/game/<title id>
    python tools/sony_files.py firmware                 the 3 firmware modules -> game/dev_flash/sys/external
    python tools/sony_files.py all --disc <folder>      all three
Options: --root <port folder> (default: the folder above tools/), --downloads <folder> (where downloads go
and where already downloaded files are looked for; default work/sony-downloads, emptied at the end),
--keep-downloads.

What it gets, all checked before use:
  update    a disc's updates 1.01, 1.03 and 1.04 (247 MB), unpacked in that order like a PS3 installs them.
            1.02 holds only an EBOOT.BIN that 1.03 replaces (checked on all four discs), so it is skipped.
            Each package's SHA-1 is pinned below (Sony's own: <title>-ver.xml, over the file without its
            last 32 bytes, which hold that same digest). The result is checked: PARAM.SFO says 01.04, the
            EBOOT.BIN and patch1/3/4.psarc_s are there.
  firmware  the PS3 system software 4.93 (PS3UPDAT.PUP, 197 MB; pinned MD5 and SHA-256), of which only
            sys/external/liblv2.sprx, libsysmodule.sprx and libsre.sprx are kept (178 KB): the build
            translates those three, and the game itself opens nothing else in dev_flash while it runs.
            When Sony no longer serves 4.93, the newest one from Sony's update list is used (its MD5 is in
            its address); the build then says the modules differ from the tested ones.
  ps3tool   tools/ps3tool.py (Sami Enazi's, in the port's public repository, never in a release zip):
            from GitHub at the port's own commit (port-version.txt), else from main.
ps3tool is used as a program only: python tools/ps3tool.py pkg|pup <in> <out>.
The digital (PSN) versions are not covered: their update is part of the game, installed in RPCS3.
Progress lines for Setup: "  progress: <percent>% <what>". Exit code 0 = done, 1 = failed (last line: why).
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
UA = "inFAMOUS2-port-setup"
REPO = "SamiEnazi/infamous2-port"
FW_MODULES = ("liblv2", "libsysmodule", "libsre")

# Sony's update packages per disc (http://b0.ww.np.dl.playstation.net, as listed in
# https://a0.ww.np.dl.playstation.net/tpl/np/<title>/<title>-ver.xml): version, size, SHA-1, address.
UPDATES = {
    "BCES01143": [
        ("01.01", 111696752, "6e63fc86c19b9c459fb516e68014209355f77a87",
         "http://b0.ww.np.dl.playstation.net/tppkg/np/BCES01143/BCES01143_T21/fbed4ba0a414f8f7/EP9000-BCES01143_00-P001SHIP05252213-A0101-V0100-PE.pkg"),
        ("01.03", 23580256, "49d059bd7a5409a9ac0619bb50b104a7b6d1501f",
         "http://b0.ww.np.dl.playstation.net/tppkg/np/BCES01143/BCES01143_T21/fbed4ba0a414f8f7/EP9000-BCES01143_00-P003SHIP08231456-A0103-V0100-PE.pkg"),
        ("01.04", 112134048, "41f3e4c9de6d93b5164aef00684cf3e11993ca5b",
         "http://b0.ww.np.dl.playstation.net/tppkg/np/BCES01143/BCES01143_T21/fbed4ba0a414f8f7/EP9000-BCES01143_00-P004SHIP10171526-A0104-V0100-PE.pkg"),
    ],
    "BCES01144": [
        ("01.01", 111696752, "2391dfcc366396e08722b2ecf13776ec6d383c5c",
         "http://b0.ww.np.dl.playstation.net/tppkg/np/BCES01144/BCES01144_T19/70418cac340f3947/EP9000-BCES01144_00-P001SHIP05252213-A0101-V0100-PE.pkg"),
        ("01.03", 23580256, "48849006315ee05473f4e61c567a1b36e064730c",
         "http://b0.ww.np.dl.playstation.net/tppkg/np/BCES01144/BCES01144_T19/70418cac340f3947/EP9000-BCES01144_00-P003SHIP08231456-A0103-V0100-PE.pkg"),
        ("01.04", 112134048, "d9dd516454ce011a0233b62a06513b24aab706e3",
         "http://b0.ww.np.dl.playstation.net/tppkg/np/BCES01144/BCES01144_T19/70418cac340f3947/EP9000-BCES01144_00-P004SHIP10171526-A0104-V0100-PE.pkg"),
    ],
    "BCES01229": [
        ("01.01", 111696752, "0582aa6925c189b54dcf9beb70aef855d6594b91",
         "http://b0.ww.np.dl.playstation.net/tppkg/np/BCES01229/BCES01229_T17/4e156726e3aeb45b/EP9000-BCES01229_00-P001SHIP05252213-A0101-V0100-PE.pkg"),
        ("01.03", 23580256, "7464aa8935b6f78db61375ff9da68f5bbd47a3a5",
         "http://b0.ww.np.dl.playstation.net/tppkg/np/BCES01229/BCES01229_T17/4e156726e3aeb45b/EP9000-BCES01229_00-P003SHIP08231456-A0103-V0100-PE.pkg"),
        ("01.04", 112134048, "6cf696ad41ff8075995a04991322109fbf0f5209",
         "http://b0.ww.np.dl.playstation.net/tppkg/np/BCES01229/BCES01229_T17/4e156726e3aeb45b/EP9000-BCES01229_00-P004SHIP10171526-A0104-V0100-PE.pkg"),
    ],
    "BCUS98125": [
        ("01.01", 111695440, "917b373f1e442ac1a61648c320a6c28f0ed2cdee",
         "http://b0.ww.np.dl.playstation.net/tppkg/np/BCUS98125/BCUS98125_T24/53b9709ebcdaded3/UP9000-BCUS98125_00-P001SHIP05231325-A0101-V0100-PE.pkg"),
        ("01.03", 23580256, "89036aeb39bf6d62542e7d73dc474caa2cf4f99e",
         "http://b0.ww.np.dl.playstation.net/tppkg/np/BCUS98125/BCUS98125_T24/53b9709ebcdaded3/UP9000-BCUS98125_00-P003SHIP08231456-A0103-V0100-PE.pkg"),
        ("01.04", 112134048, "53279626bd9fb037ecb271420207a01cbb5b148e",
         "http://b0.ww.np.dl.playstation.net/tppkg/np/BCUS98125/BCUS98125_T24/53b9709ebcdaded3/UP9000-BCUS98125_00-P004SHIP10171526-A0104-V0100-PE.pkg"),
    ],
}
UPDATE_FILES = ("USRDIR/EBOOT.BIN", "USRDIR/patch/psarc/patch1.psarc_s", "USRDIR/patch/psarc/patch3.psarc_s",
                "USRDIR/patch/psarc/patch4.psarc_s")

# PS3 system software 4.93 (2026-03-18 image; the same file on Sony's EU and US servers).
FW_MD5 = "a2b60b6ac1d2e49e230144345616927c"
FW_SHA256 = "158471fd834f8ea8036136b6aab43cd86c7ba73d79ca30e0af3c0fe0001cf365"
FW_SIZE = 206197916
FW_URLS = ("http://deu01.ps3.update.playstation.net/update/ps3/image/eu/2026_0318_" + FW_MD5 + "/PS3UPDAT.PUP",
           "http://dus01.ps3.update.playstation.net/update/ps3/image/us/2026_0318_" + FW_MD5 + "/PS3UPDAT.PUP")
FW_LISTS = ("http://feu01.ps3.update.playstation.net/update/ps3/list/eu/ps3-updatelist.txt",
            "http://fus01.ps3.update.playstation.net/update/ps3/list/us/ps3-updatelist.txt")


class Fail(Exception):
    pass


def say(s):
    print(s, flush=True)


def mb(n):
    return f"{n / 1048576:.0f} MB"


def retry_os(fn, *args):
    """os.replace / shutil.move of what was just written or unpacked: a virus scanner (or Windows Search) reading
    a new file holds it for a moment, and Windows then says "Access is denied" (5) or "in use" (32). Tried again
    for up to 30 s."""
    for i in range(61):
        try:
            return fn(*args)
        except OSError as e:
            if i == 60 or getattr(e, "winerror", None) not in (5, 32):
                raise
            time.sleep(0.5)


class Progress:
    """One percentage over everything this run downloads (Setup.exe's bar)."""

    def __init__(self, total):
        self.total, self.done, self.last = max(1, total), 0, 0.0

    def add(self, n, what):
        self.done += n
        now = time.time()
        if now - self.last >= 1.0:
            self.last = now
            say(f"  progress: {min(99, int(100 * self.done / self.total))}% {what}")


def read_sfo(path):
    """PARAM.SFO -> {key: value}"""
    import struct
    b = open(path, "rb").read()
    if b[:4] != b"\0PSF":
        raise Fail(f"{path} is not a PARAM.SFO")
    keys, data, n = struct.unpack_from("<III", b, 8)
    out = {}
    for i in range(n):
        ko, fmt, ln, _mx, off = struct.unpack_from("<HHIII", b, 20 + 16 * i)
        k = b[keys + ko:b.index(b"\0", keys + ko)].decode()
        v = b[data + off:data + off + ln]
        out[k] = struct.unpack("<I", v[:4])[0] if fmt == 0x0404 else v.rstrip(b"\0").decode("utf-8", "replace")
    return out


def file_hash(path, algo, skip_tail=0):
    h = hashlib.new(algo)
    left = os.path.getsize(path) - skip_tail
    with open(path, "rb") as f:
        while left > 0:
            b = f.read(min(1 << 20, left))
            if not b:
                break
            h.update(b)
            left -= len(b)
    return h.hexdigest()


def fetch_text(url, timeout=30):
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read().decode("utf-8", "replace")


def download(url, path, size, what, prog, check):
    """url -> path, resuming a .part left by an earlier try (Sony's servers answer byte ranges), checked by
    check(path) -> None or the reason it is wrong; one more try from the start when it is wrong."""
    if os.path.isfile(path) and (size is None or os.path.getsize(path) == size) and check(path) is None:
        say(f"  {what}: already downloaded")
        prog.add(size or os.path.getsize(path), what)
        return
    part = path + ".part"
    counted = 0                                   # this file's bytes in prog (a retry counts them again)
    for attempt in (1, 2, 3):
        have = os.path.getsize(part) if os.path.isfile(part) else 0
        if size is not None and have > size:
            os.remove(part)
            have = 0
        prog.add(have - counted, what)
        counted = have
        try:
            headers = {"User-Agent": UA}
            if have:
                headers["Range"] = f"bytes={have}-"
            req = urllib.request.Request(url, headers=headers)
            with urllib.request.urlopen(req, timeout=60) as r:
                if have and r.status != 206:      # no range answer: from the start
                    prog.add(-have, what)
                    counted -= have
                    have = 0
                with open(part, "ab" if have else "wb") as f:
                    got = have
                    while True:
                        b = r.read(1 << 20)
                        if not b:
                            break
                        f.write(b)
                        got += len(b)
                        counted += len(b)
                        prog.add(len(b), f"downloading {what} from Sony ({mb(got)} of {mb(size or got)})")
            if size is not None and os.path.getsize(part) != size:
                raise OSError(f"got {os.path.getsize(part)} bytes, not {size}")
            why = check(part)
            if why:
                prog.add(-counted, what)
                counted = 0
                os.remove(part)
                raise OSError(why)
            retry_os(os.replace, part, path)
            return
        except (OSError, urllib.error.URLError) as e:
            if attempt == 3:
                raise Fail(f"downloading {what} from Sony failed: {e}. Check the internet connection and run Setup again "
                           f"(what was downloaded so far is kept)")
            say(f"  {what}: {e} -- trying again")
            time.sleep(3 * attempt)


def run_ps3tool(py, ps3tool, mode, src, dst, what):
    """ps3tool as a program: python tools/ps3tool.py <mode> <in> <out>"""
    t0 = time.time()
    p = subprocess.run([py, "-I", ps3tool, mode, src, dst], capture_output=True, text=True, errors="replace")
    if p.returncode != 0:
        tail = (p.stdout + p.stderr).strip().splitlines()[-6:]
        raise Fail(f"ps3tool could not unpack {what} (exit code {p.returncode}): " + " | ".join(tail))
    say(f"  {what}: unpacked ({time.time() - t0:.0f} s)")


def has_crypto():
    try:
        import Crypto.Cipher.AES   # noqa: F401 (pycryptodome: ps3tool is fast with it)
        return True
    except Exception:
        return False


# ---------------------------------------------------------------------------------------- ps3tool

def port_commit(root):
    """The port's commit from port-version.txt ("<repo> <commit>" on a later line), or None."""
    try:
        for ln in open(os.path.join(root, "port-version.txt"), encoding="utf-8", errors="replace"):
            p = ln.split()
            if len(p) == 2 and p[0].lower() == REPO.lower() and len(p[1]) >= 7:
                return p[1]
    except OSError:
        pass
    return None


def ensure_ps3tool(root):
    path = os.path.join(root, "tools", "ps3tool.py")
    if os.path.isfile(path) and os.path.getsize(path) > 1000:
        return path
    refs = [r for r in (port_commit(root), "main") if r]
    last = None
    for ref in refs:
        url = f"https://raw.githubusercontent.com/{REPO}/{ref}/tools/ps3tool.py"
        try:
            req = urllib.request.Request(url, headers={"User-Agent": UA})
            with urllib.request.urlopen(req, timeout=60) as r:
                data = r.read()
            if len(data) < 1000 or b"\0" in data:
                raise OSError("not a Python file")
            with open(path + ".part", "wb") as f:
                f.write(data)
            retry_os(os.replace, path + ".part", path)
            say(f"  ps3tool: tools/ps3tool.py from GitHub ({REPO} {ref[:12]})")
            return path
        except (OSError, urllib.error.URLError) as e:
            last = e
    raise Fail(f"tools/ps3tool.py is missing and could not be downloaded from GitHub ({last})")


# ---------------------------------------------------------------------------------------- update

def update_ok(dst):
    sfo = os.path.join(dst, "PARAM.SFO")
    if not os.path.isfile(sfo):
        return False
    try:
        if read_sfo(sfo).get("APP_VER") != "01.04":
            return False
    except Exception:
        return False
    return all(os.path.isfile(os.path.join(dst, *f.split("/"))) for f in UPDATE_FILES)


def merge_tree(src, dst):
    """src's files over dst (later packages win, as on a PS3); nothing in dst is deleted."""
    for dp, dn, fn in os.walk(src):
        rel = os.path.relpath(dp, src)
        out = os.path.join(dst, rel) if rel != "." else dst
        os.makedirs(out, exist_ok=True)
        for f in fn:
            t = os.path.join(out, f)
            if os.path.isfile(t):
                os.remove(t)
            retry_os(shutil.move, os.path.join(dp, f), t)


def get_update(root, title, py, ps3tool, dl, prog):
    dst = os.path.join(root, "game", "dev_hdd0", "game", title)
    if update_ok(dst):
        say(f"  the 1.04 update: already in game/dev_hdd0/game/{title}")
        return 0
    if title not in UPDATES:
        raise Fail(f"{title}: Setup knows Sony's update packages for the disc versions only (BCES01143, BCES01144, "
                   f"BCES01229, BCUS98125). For the digital version, install the game in RPCS3 and run Setup again")
    pkgs = UPDATES[title]
    stage = os.path.join(root, "work", "sony-unpack")
    for ver, size, sha1, url in pkgs:
        name = url.rsplit("/", 1)[1]
        path = os.path.join(dl, name)

        def check(p, sha1=sha1):
            # Sony's SHA-1 covers the file without its last 32 bytes (they hold the same digest)
            got = file_hash(p, "sha1", 32)
            return None if got == sha1 else f"the update {ver} has the wrong SHA-1 ({got})"
        download(url, path, size, f"the game's update {ver}", prog, check)
    os.makedirs(dst, exist_ok=True)
    for ver, size, sha1, url in pkgs:
        path = os.path.join(dl, url.rsplit("/", 1)[1])
        out = os.path.join(stage, ver)
        if os.path.isdir(out):
            shutil.rmtree(out)
        os.makedirs(stage, exist_ok=True)
        run_ps3tool(py, ps3tool, "pkg", path, out, f"the update {ver}")
        merge_tree(out, dst)
        shutil.rmtree(out, ignore_errors=True)
    shutil.rmtree(stage, ignore_errors=True)
    if not update_ok(dst):
        raise Fail(f"after unpacking, game/dev_hdd0/game/{title} is not the 1.04 update (PARAM.SFO 01.04, "
                   f"EBOOT.BIN, patch1/3/4.psarc_s)")
    say(f"  the 1.04 update: game/dev_hdd0/game/{title} ({', '.join(p[0] for p in pkgs)})")
    return sum(p[1] for p in pkgs)


# ---------------------------------------------------------------------------------------- firmware

def fw_dir(root):
    return os.path.join(root, "game", "dev_flash", "sys", "external")


def fw_ok(root):
    return all(os.path.isfile(os.path.join(fw_dir(root), m + ".sprx")) for m in FW_MODULES)


def newest_pup():
    """(url, md5) of the newest PS3UPDAT.PUP in Sony's update list, or None."""
    for lst in FW_LISTS:
        try:
            for ln in fetch_text(lst).splitlines():
                if "PS3UPDAT.PUP" in ln and "CDN=" in ln:
                    url = ln.split("CDN=", 1)[1].split(";", 1)[0].strip()
                    folder = url.rsplit("/", 2)[-2]               # 2026_0318_<md5>
                    md5 = folder.rsplit("_", 1)[-1].lower()
                    if len(md5) == 32:
                        return url, md5
        except (OSError, urllib.error.URLError, IndexError):
            continue
    return None


def get_firmware(root, py, ps3tool, dl, prog):
    if fw_ok(root):
        say("  the firmware modules: already in game/dev_flash/sys/external")
        return 0
    pup = os.path.join(dl, "PS3UPDAT.PUP")
    done = False
    for url in FW_URLS:
        try:
            download(url, pup, FW_SIZE, "the PS3 system software 4.93", prog,
                     lambda p: None if file_hash(p, "sha256") == FW_SHA256 else "PS3UPDAT.PUP has the wrong SHA-256")
            done = True
            break
        except Fail as e:
            say(f"  {e}")
    if not done:
        new = newest_pup()
        if not new:
            raise Fail("Sony's PS3 system software could not be downloaded (neither 4.93 nor the newest one)")
        url, md5 = new
        say(f"  Sony serves another system software now: {url} (the build will say if its modules differ)")
        if os.path.isfile(pup):
            os.remove(pup)
        download(url, pup, None, "the PS3 system software", prog,
                 lambda p: None if file_hash(p, "md5") == md5 else "PS3UPDAT.PUP has the wrong MD5")
    tmp = os.path.join(root, "work", "sony-unpack", "dev_flash")
    if os.path.isdir(tmp):
        shutil.rmtree(tmp)
    os.makedirs(os.path.dirname(tmp), exist_ok=True)
    run_ps3tool(py, ps3tool, "pup", pup, tmp, "the system software")
    os.makedirs(fw_dir(root), exist_ok=True)
    for m in FW_MODULES:
        src = os.path.join(tmp, "sys", "external", m + ".sprx")
        if not os.path.isfile(src):
            raise Fail(f"the system software has no sys/external/{m}.sprx")
        shutil.copy2(src, os.path.join(fw_dir(root), m + ".sprx"))
    shutil.rmtree(os.path.join(root, "work", "sony-unpack"), ignore_errors=True)
    say("  the firmware modules: game/dev_flash/sys/external (liblv2, libsysmodule, libsre)")
    return os.path.getsize(pup)


# ---------------------------------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("what", choices=("ps3tool", "update", "firmware", "all"))
    ap.add_argument("--root", default=os.path.dirname(HERE))
    ap.add_argument("--disc", help="the game folder (holds PS3_GAME): its title id picks the update")
    ap.add_argument("--title", help="the title id, instead of --disc")
    ap.add_argument("--downloads")
    ap.add_argument("--keep-downloads", action="store_true")
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    dl = os.path.abspath(a.downloads or os.path.join(root, "work", "sony-downloads"))
    t0 = time.time()
    try:
        ps3tool = ensure_ps3tool(root)
        if a.what == "ps3tool":
            say(f"  ps3tool: {ps3tool}")
            return 0
        title = a.title
        if a.what in ("update", "all") and not title:
            if not a.disc:
                raise Fail("--disc <game folder> or --title <id> is needed for the update")
            title = read_sfo(os.path.join(a.disc, "PS3_GAME", "PARAM.SFO"))["TITLE_ID"]
        need_upd = a.what in ("update", "all") and not update_ok(os.path.join(root, "game", "dev_hdd0", "game", title))
        need_fw = a.what in ("firmware", "all") and not fw_ok(root)
        total = (sum(p[1] for p in UPDATES.get(title, [])) if need_upd else 0) + (FW_SIZE if need_fw else 0)
        if total:
            say(f"  from Sony's servers: {'the update ' if need_upd else ''}{'and ' if need_upd and need_fw else ''}"
                f"{'the system software ' if need_fw else ''}({mb(total)})")
            if not has_crypto():
                say("  note: this Python has no pycryptodome, so unpacking takes several minutes (Setup's work/venv has it)")
        os.makedirs(dl, exist_ok=True)
        prog = Progress(total)
        got = 0
        if a.what in ("update", "all"):
            got += get_update(root, title, sys.executable, ps3tool, dl, prog)
        if a.what in ("firmware", "all"):
            got += get_firmware(root, sys.executable, ps3tool, dl, prog)
        if not a.keep_downloads and os.path.isdir(dl) and not a.downloads:
            shutil.rmtree(dl, ignore_errors=True)
        say("  progress: 100% the game's update and the firmware files are ready")
        say(f"  from Sony: {mb(got)} in {time.time() - t0:.0f} s")
        return 0
    except Fail as e:
        say(f"error: {e}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
