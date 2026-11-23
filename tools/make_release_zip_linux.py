"""The Linux zip of a release, inFAMOUS2-port-v<version>-Linux-x64.zip, for launchers such as Quiver that pick a
release's file by "linux" in its name: the port at one commit (git archive: committed files only, LF line ends),
setup.sh at the top (it does everything: finds the game, builds it once, plays), and port-version.txt. Scripts keep
their executable bit (Unix modes in the zip). It never holds a game file, firmware or a PGO profile (refused if one is found).
Also at the top: the double-click programs "Install inFAMOUS 2" and "Play inFAMOUS 2" (tools/launcher/launcher.c, compiled
here twice: static, no C library, the same files for every x86-64 Linux; their SHA-256 is printed) and "How to install on
Linux.txt". Left out: the Windows starters at the top (Setup.cmd, Play inFAMOUS 2.cmd), so the folder has one of each.
    python -I tools/make_release_zip_linux.py --version 0.5.4 --repo <clone of SamiEnazi/infamous2-port>
                                              [--commit HEAD] [--out <folder>] [--llvm <LLVM folder>]
--llvm: the clang to compile the launcher with (default: PORT_LLVM, else tools-cache/llvm of the repo or of this
script's folder, else clang on PATH); linked with its lld, else with the GNU linker (ld.bfd).
"""
import argparse
import io
import os
import re
import struct
import subprocess
import sys
import tarfile
import time
import zipfile

ap = argparse.ArgumentParser()
ap.add_argument("--version", required=True)
ap.add_argument("--repo", required=True)
ap.add_argument("--commit", default="HEAD")
ap.add_argument("--out", default=".")
ap.add_argument("--llvm", default="")
a = ap.parse_args()

sha = subprocess.run(["git", "-C", a.repo, "rev-parse", a.commit], capture_output=True, text=True, check=True).stdout.strip()
tar_bytes = subprocess.run(["git", "-C", a.repo, "-c", "core.autocrlf=false", "archive", "--format=tar", sha],
                           capture_output=True, check=True).stdout
name = f"inFAMOUS2-port-v{a.version}-Linux-x64"
out = os.path.join(a.out, name + ".zip")

GAME = re.compile(r"\.(elf|self|sprx|prx|pkg|rap|edat|sdat|pup|iso)$|\.psarc|(^|/)(EBOOT\.BIN|PARAM\.SFO|TROPHY\.TRP|TROPUSR\.DAT"
                  r"|TROPCONF\.SFM)$|(^|/)TROP_?\d*\.SFM$|(^|/)recomp_achievements[^/]*\.json$|(^|/)keys/|disc_?keys|\.keys$", re.I)
# A PGO profile ("Optimise for this PC", tools/pgo_train.py) comes from running the game: it never leaves the player's PC
PROFILE = re.compile(r"\.(profdata|profraw)$|^work/", re.I)
# The decryptor (committed to the public repository on its own): left out of the zip; setup fetches it
OWN = re.compile(r"^tools/(ps3tool[^/]*|keys/disc_keys\.example\.json)$", re.I)
# Windows' starters at the top ("Play inFAMOUS 2.cmd" beside the program "Play inFAMOUS 2" was one more thing to choose)
WINDOWS = re.compile(r"^[^/]+\.(cmd|bat|exe)$", re.I)
# every entry dated 26 hours ago (GitHub #21/#22: a ZIP's times have no time zone; a future time stalls the build)
stamp = time.localtime(time.time() - 26 * 3600)[:6]

# the double-click programs: name -> the compiler's extra flags (tools/launcher/launcher.c)
LAUNCHERS = {"Install inFAMOUS 2": [], "Play inFAMOUS 2": ["-DPLAY"]}
HOWTO = """inFAMOUS 2 (PC port) for Linux and the Steam Deck: how to install

The same on every 64-bit Linux (SteamOS, Bazzite, Ubuntu, Fedora, Arch, ...): nothing to install, no password.
Everything the build needs is downloaded into this folder (about 2 GB).

1. Unpack this zip where you want the game (about 10 GB of free space; the Deck: Desktop Mode).
2. Double-click "Install inFAMOUS 2" in this folder. If the desktop asks whether to run it, choose Execute / Run.
   GNOME (Ubuntu, Fedora Workstation): if the file manager opens it as a file or says it cannot open it, right-click
   it and choose "Run as a Program", or open a terminal in this folder and type:  bash tools/install_gui.sh
3. Follow the windows: pick your game folder (the one holding PS3_GAME). The build takes 15-60 minutes.

Then play: double-click "Play inFAMOUS 2" in this folder (or start it from the application menu, or from Steam with
Add to Steam). Before the game is built, "Play inFAMOUS 2" starts the installer.
In a terminal, ./setup.sh does the same (./setup.sh --help).
"""


def build_launcher(src_text, work, defs):
    """a launcher's static binary (bytes) and how it was linked; compiled from the commit's own source"""
    import shutil
    cands = [a.llvm, os.environ.get("PORT_LLVM", ""), os.path.join(a.repo, "tools-cache", "llvm"),
             os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools-cache", "llvm")]
    cc = next((os.path.join(c, "bin", "clang") for c in cands if c and os.path.isfile(os.path.join(c, "bin", "clang"))), None)
    cc = cc or shutil.which("clang")
    if not cc:
        sys.exit("no clang to compile the launcher with: give --llvm <LLVM folder> (setup's tools-cache/llvm)")
    src = os.path.join(work, "launcher.c")
    with open(src, "w", encoding="utf-8") as f:
        f.write(src_text)
    exe = os.path.join(work, "launcher")
    flags = ["--no-default-config", "--target=x86_64-linux-gnu", "-Os", "-static", "-nostdlib", "-ffreestanding", "-fno-pic",
             "-fno-stack-protector", "-fno-asynchronous-unwind-tables", "-fno-unwind-tables", "-fcf-protection=none",
             "-fno-ident", "-ffunction-sections", "-fdata-sections", "-Wall", "-Wextra",
             "-Wl,--gc-sections", "-Wl,--build-id=none", "-Wl,-z,noexecstack", "-Wl,-z,norelro", "-Wl,-s"]
    errs = []
    for ld in ("lld", "bfd"):   # (an LLVM whose lld cannot start here: libxml2.so.2, see tools/setup.sh)
        p = subprocess.run([cc] + flags + defs + ["-fuse-ld=" + ld, "-o", exe, src], capture_output=True, text=True)
        if p.returncode == 0 and os.path.isfile(exe):
            data = open(exe, "rb").read()
            # static: an ELF executable with no program interpreter and no dynamic section
            phoff, = struct.unpack_from("<Q", data, 0x20)
            phentsize, phnum = struct.unpack_from("<HH", data, 0x36)
            kinds = {struct.unpack_from("<I", data, phoff + i * phentsize)[0] for i in range(phnum)}
            if data[:4] != b"\x7fELF" or struct.unpack_from("<H", data, 0x10)[0] != 2 or 2 in kinds or 3 in kinds:
                sys.exit("the launcher came out dynamic or not an executable: " + exe)
            return data, f"{os.path.basename(cc)} + {ld}"
        errs.append(f"{ld}: {(p.stdout + p.stderr).strip()[-600:]}")
    sys.exit("compiling the launcher failed:\n" + "\n".join(errs))


files, bad = 0, []
os.makedirs(a.out, exist_ok=True)
tmp = out + ".part"
with tarfile.open(fileobj=io.BytesIO(tar_bytes)) as t, zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    names = []
    for m in t.getmembers():
        if not m.isfile():
            continue
        if OWN.search(m.name):
            print("  left out (setup fetches it):", m.name)
            continue
        if WINDOWS.search(m.name):
            print("  left out (Windows):", m.name)
            continue
        if GAME.search(m.name) or PROFILE.search(m.name):
            bad.append(m.name)
            continue
        zi = zipfile.ZipInfo(m.name, date_time=stamp)
        zi.compress_type = zipfile.ZIP_DEFLATED
        zi.create_system = 3                                  # Unix: the modes below are used on extract
        zi.external_attr = ((0o100755 if m.mode & 0o111 else 0o100644) & 0xFFFF) << 16
        z.writestr(zi, t.extractfile(m).read())
        names.append(m.name)
        files += 1
    import tempfile
    src_text = t.extractfile("tools/launcher/launcher.c").read().decode("utf-8")
    import hashlib as _h
    for lname, defs in LAUNCHERS.items():
        with tempfile.TemporaryDirectory() as work:
            launcher, how = build_launcher(src_text, work, defs)
        li = zipfile.ZipInfo(lname, date_time=stamp)
        li.compress_type = zipfile.ZIP_DEFLATED
        li.create_system = 3
        li.external_attr = 0o100755 << 16
        z.writestr(li, launcher)
        files += 1
        print(f"  {lname}: {len(launcher)} bytes, static ({how}), sha256 {_h.sha256(launcher).hexdigest()}")
    hi = zipfile.ZipInfo("How to install on Linux.txt", date_time=stamp)
    hi.compress_type = zipfile.ZIP_DEFLATED
    hi.create_system = 3
    hi.external_attr = 0o100644 << 16
    z.writestr(hi, HOWTO)
    files += 1
    pv = zipfile.ZipInfo("port-version.txt", date_time=stamp)
    pv.create_system = 3
    pv.external_attr = 0o100644 << 16
    z.writestr(pv, f"Release v{a.version} ({name}.zip)\nSamiEnazi/infamous2-port {sha}\n")
    files += 1
if bad:
    os.remove(tmp)
    for b in bad:
        print("  GAME FILE OR PROFILE:", b)
    sys.exit("game files or PGO profiles in the commit: nothing made")
with zipfile.ZipFile(tmp) as z:
    top = {n.split("/")[0] for n in z.namelist()}
    for need in ("setup.sh", "port-version.txt", "tools/setup.sh", "tools/play.sh", "tools/install_gui.sh", *LAUNCHERS):
        if need not in z.namelist():
            os.remove(tmp)
            sys.exit(f"{need} is not in the zip")
    mode = (z.getinfo("setup.sh").external_attr >> 16) & 0o777
    if not mode & 0o100:
        os.remove(tmp)
        sys.exit("setup.sh is not executable in the zip (git update-index --chmod=+x setup.sh)")
    for lname in LAUNCHERS:
        if not (z.getinfo(lname).external_attr >> 16) & 0o100:
            os.remove(tmp)
            sys.exit(lname + " is not executable in the zip")
    extra = sorted(n for n in z.namelist() if "/" not in n and n.lower().endswith((".desktop", ".cmd", ".bat", ".exe")))
    if extra:
        os.remove(tmp)
        sys.exit("starters other than the two programs at the top: " + ", ".join(extra))
os.replace(tmp, out)
import hashlib
h = hashlib.sha256(open(out, "rb").read()).hexdigest()
print(f"{out}\n  the port: {sha}\n  {files} files, {os.path.getsize(out) / 1e6:.1f} MB, setup.sh mode {oct(mode)}\n  sha256 {h}")
