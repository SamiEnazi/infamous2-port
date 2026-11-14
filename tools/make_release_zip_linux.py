"""The Linux zip of a release, inFAMOUS2-port-v<version>-Linux-x64.zip, for launchers such as Quiver that pick a
release's file by "linux" in its name: the port at one commit (git archive: committed files only, LF line ends),
setup.sh at the top (it does everything: finds the game, builds it once, plays), and port-version.txt. Scripts keep
their executable bit (Unix modes in the zip). It never holds a game file or firmware (refused if one is found).
    python -I tools/make_release_zip_linux.py --version 0.5.4 --repo <clone of SamiEnazi/infamous2-port>
                                              [--commit HEAD] [--out <folder>]
"""
import argparse
import io
import os
import re
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
a = ap.parse_args()

sha = subprocess.run(["git", "-C", a.repo, "rev-parse", a.commit], capture_output=True, text=True, check=True).stdout.strip()
tar_bytes = subprocess.run(["git", "-C", a.repo, "-c", "core.autocrlf=false", "archive", "--format=tar", sha],
                           capture_output=True, check=True).stdout
name = f"inFAMOUS2-port-v{a.version}-Linux-x64"
out = os.path.join(a.out, name + ".zip")

GAME = re.compile(r"\.(elf|self|sprx|prx|pkg|rap|edat|sdat|pup|iso)$|\.psarc|(^|/)(EBOOT\.BIN|PARAM\.SFO|TROPHY\.TRP|TROPUSR\.DAT"
                  r"|TROPCONF\.SFM)$|(^|/)TROP_?\d*\.SFM$|(^|/)recomp_achievements[^/]*\.json$|(^|/)keys/|disc_?keys|\.keys$", re.I)
# Sami's own decryptor (he commits it to the public repository himself): left out of the zip; setup fetches it
OWN = re.compile(r"^tools/(ps3tool[^/]*|keys/disc_keys\.example\.json)$", re.I)
# every entry dated 26 hours ago (GitHub #21/#22: a ZIP's times have no time zone; a future time stalls the build)
stamp = time.localtime(time.time() - 26 * 3600)[:6]

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
        if GAME.search(m.name):
            bad.append(m.name)
            continue
        zi = zipfile.ZipInfo(m.name, date_time=stamp)
        zi.compress_type = zipfile.ZIP_DEFLATED
        zi.create_system = 3                                  # Unix: the modes below are used on extract
        zi.external_attr = ((0o100755 if m.mode & 0o111 else 0o100644) & 0xFFFF) << 16
        z.writestr(zi, t.extractfile(m).read())
        names.append(m.name)
        files += 1
    pv = zipfile.ZipInfo("port-version.txt", date_time=stamp)
    pv.create_system = 3
    pv.external_attr = 0o100644 << 16
    z.writestr(pv, f"Release v{a.version} ({name}.zip)\nSamiEnazi/infamous2-port {sha}\n")
    files += 1
if bad:
    os.remove(tmp)
    for b in bad:
        print("  GAME FILE:", b)
    sys.exit("game files in the commit: nothing made")
with zipfile.ZipFile(tmp) as z:
    top = {n.split("/")[0] for n in z.namelist()}
    for need in ("setup.sh", "port-version.txt", "tools/setup.sh", "tools/play.sh"):
        if need not in z.namelist():
            os.remove(tmp)
            sys.exit(f"{need} is not in the zip")
    mode = (z.getinfo("setup.sh").external_attr >> 16) & 0o777
    if not mode & 0o100:
        os.remove(tmp)
        sys.exit("setup.sh is not executable in the zip (git update-index --chmod=+x setup.sh)")
os.replace(tmp, out)
import hashlib
h = hashlib.sha256(open(out, "rb").read()).hexdigest()
print(f"{out}\n  the port: {sha}\n  {files} files, {os.path.getsize(out) / 1e6:.1f} MB, setup.sh mode {oct(mode)}\n  sha256 {h}")
