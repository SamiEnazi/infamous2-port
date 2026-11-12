"""The build tools, downloaded into tools-cache\\: portable, so no installer, no admin and no PATH
changes. Setup deletes tools-cache\\ after a successful build (tools/setup_cleanup.ps1).

  tools-cache\\cmake   CMake 3.31.3                    github.com/Kitware/CMake           47 MB
  tools-cache\\ninja   Ninja 1.11.1                    github.com/ninja-build/ninja       0.3 MB
  tools-cache\\llvm    LLVM 22.1.8 (clang-cl, lld)     github.com/llvm/llvm-project      862 MB
  tools-cache\\msvc    MSVC 14.38 C++ headers and libraries } Microsoft's own packages, fetched the
  tools-cache\\sdk     Windows SDK 10.0.22621              } portable-msvc way (~400 MB together)
  (tools-cache\\python, CPython 3.12, comes first: tools/fetch_tools.ps1 gets it, then runs this.)

The MSVC and SDK parts are Microsoft's, under the Visual Studio Build Tools license
(https://go.microsoft.com/fwlink/?LinkId=2179911): they are fetched only with --accept-msvc-license.
The approach follows Martins Mozeiko's portable-msvc.py (public domain,
https://gist.github.com/mmozeiko/7f3162ec2988e81e56d5c4e22cde9977).

Every file is checked against a SHA-256: pinned here, or the one in Microsoft's manifest. A tool
set on purpose for this PC (environment variable or config.local.json, see port_paths.py) is not
downloaded. Progress lines for Setup: "  progress: <percent>% <what>".

    python tools/fetch_tools.py [--accept-msvc-license]
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import port_paths   # noqa: E402

# The real path (a subst drive letter resolved): Windows Installer may not see a subst drive, and
# every move below then stays on one path. port_paths still finds the same folder through either.
CACHE = os.path.realpath(port_paths.CACHE)
DL = os.path.join(CACHE, "downloads")
MSVC_LICENSE = "https://go.microsoft.com/fwlink/?LinkId=2179911"
CHANNEL = "https://aka.ms/vs/17/release/channel"       # Visual Studio 2022
MSVC_VERSION, SDK_VERSION = "14.38", "22621"            # what this port is built and tested with
GH = "https://github.com/"

ARCHIVES = {
    "cmake": dict(label="CMake 3.31.3", size=46570817, top="cmake-3.31.3-windows-x86_64",
                  url=GH + "Kitware/CMake/releases/download/v3.31.3/cmake-3.31.3-windows-x86_64.zip",
                  sha256="d41e863589a3c152b1d14ecac2ffd1926f29aba64528984fcda8fd2b630f44ff"),
    "ninja": dict(label="Ninja 1.11.1", size=285922, top=None,
                  url=GH + "ninja-build/ninja/releases/download/v1.11.1/ninja-win.zip",
                  sha256="524b344a1a9a55005eaf868d991e090ab8ce07fa109f1820d40e74642e289abc"),
    "llvm": dict(label="LLVM 22.1.8", size=862053924, top="clang+llvm-22.1.8-x86_64-pc-windows-msvc",
                 url=GH + "llvm/llvm-project/releases/download/llvmorg-22.1.8/clang%2Bllvm-22.1.8-x86_64-pc-windows-msvc.tar.xz",
                 sha256="d96c2cc1736f4eb7fa43cb9bbdf56d93551a9ae0a9aadb9c99c3c3b2b712a234"),
}
# Only what builds the port is unpacked from LLVM's archive (it holds 5+ GB of developer files).
LLVM_KEEP = ("bin/clang-cl.exe bin/lld-link.exe bin/llvm-lib.exe bin/llvm-ar.exe bin/llvm-ranlib.exe bin/llvm-mt.exe "
             "bin/llvm-rc.exe bin/clang-scan-deps.exe lib/clang")      # ~300 MB unpacked
CABS_ESTIMATE = 260 * 1048576    # the SDK's .cab files, until the .msi files say which ones


class Progress:
    """One percentage over everything this run downloads."""

    def __init__(self):
        self.total, self.done, self.last = 1, 0, 0.0

    def say(self, what, force=False):
        now = time.time()
        if force or now - self.last >= 1.0:
            self.last = now
            pct = min(99, int(100 * self.done / max(1, self.total)))
            print(f"  progress: {pct}% {what}", flush=True)

    def add(self, n, what):
        self.done += n
        self.say(what)


def mb(n):
    return f"{n / 1048576:.0f} MB"


def fetch(url):
    req = urllib.request.Request(url, headers={"User-Agent": "inFAMOUS2-port-setup"})
    for attempt in (1, 2):
        try:
            with urllib.request.urlopen(req, timeout=120) as r:
                return r.read()
        except OSError as e:
            if attempt == 2:
                raise SystemExit(f"could not read {url}: {e}")
            print(f"  {url}: {e} -- trying once more", flush=True)
            time.sleep(3)


def download(url, path, sha256, size, what, prog):
    """url -> path with progress, checked against sha256; retried once."""
    if os.path.isfile(path) and _sha256(path) == sha256.lower():
        prog.add(size, what)
        return
    for attempt in (1, 2):
        got = 0
        try:
            req = urllib.request.Request(url.replace(" ", "%20"), headers={"User-Agent": "inFAMOUS2-port-setup"})
            h = hashlib.sha256()
            with urllib.request.urlopen(req, timeout=120) as r, open(path + ".part", "wb") as f:
                while True:
                    b = r.read(1 << 20)
                    if not b:
                        break
                    f.write(b)
                    h.update(b)
                    got += len(b)
                    prog.add(len(b), f"downloading {what} ({mb(got)} of {mb(size)})")
            if h.hexdigest() != sha256.lower():
                raise OSError(f"the download has the wrong SHA-256 ({h.hexdigest()})")
            os.replace(path + ".part", path)
            return
        except OSError as e:
            prog.done -= got
            if attempt == 2:
                raise SystemExit(f"downloading {what} failed: {e}. Check the internet connection and try again.")
            print(f"  {what}: {e} -- trying once more", flush=True)
            time.sleep(3)


def _sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def _fresh(path):
    if os.path.isdir(path):
        shutil.rmtree(path)
    os.makedirs(path)


def _ready(path, text):
    with open(os.path.join(path, ".ready"), "w") as f:
        f.write(text + "\n")


def _tar():
    t = os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32", "tar.exe")
    return t if os.path.isfile(t) else None


class _Counted:
    """A read-only file that counts the bytes read (progress while Python unpacks)."""

    def __init__(self, f):
        self.f, self.n = f, 0

    def read(self, size=-1):
        b = self.f.read(size)
        self.n += len(b)
        return b


def _untar_py(arc, tmp, wanted, label, prog):
    """Unpack the members under `wanted` (paths in the archive: a file, or a folder and all below it)
    with Python's own tarfile + lzma. Windows 10's tar.exe can't read .tar.xz (it looks for an xz.exe
    that Windows doesn't have: issue #8), Windows 11's can; this works on both, one pass, a few minutes.
    The "data" filter refuses links and paths that would land outside tmp."""
    import tarfile
    size = max(1, os.path.getsize(arc))
    got, last = [], 0.0
    with open(arc, "rb") as raw:
        f = _Counted(raw)
        with tarfile.open(fileobj=f, mode="r|xz") as t:
            for m in t:
                if any(m.name == w or m.name.startswith(w + "/") for w in wanted):
                    t.extract(m, tmp, filter="data")
                    got.append(m.name)
                now = time.time()
                if now - last >= 2.0:
                    last = now
                    prog.say(f"unpacking {label} ({100 * f.n // size}% of the archive)", force=True)
    return got


# ---------------------------------------------------------------- CMake, Ninja, LLVM

def get_archive(key, prog):
    a = ARCHIVES[key]
    dest = os.path.join(CACHE, key)
    arc = os.path.join(DL, a["url"].rsplit("/", 1)[1].replace("%2B", "+"))
    download(a["url"], arc, a["sha256"], a["size"], a["label"], prog)
    prog.say(f"unpacking {a['label']}" + (" (a few minutes)" if key == "llvm" else ""), force=True)
    tmp = dest + ".unpack"
    _fresh(tmp)
    if arc.endswith(".zip"):
        with zipfile.ZipFile(arc) as z:
            z.extractall(tmp)
    else:
        members = [a["top"] + "/" + m for m in LLVM_KEEP.split()]
        tar, why = _tar(), "Windows' tar.exe is missing"
        r = None
        if tar:
            r = subprocess.run([tar, "-xf", arc, "-C", tmp] + members, capture_output=True, text=True)
            why = r.stderr.strip()[-300:]
        if r is None or r.returncode:
            # Windows 10's tar.exe can't open .tar.xz ("unable to run program xz -d -qq"): Python can.
            print(f"  Windows' tar could not unpack {a['label']} ({why}); unpacking it with Python instead",
                  flush=True)
            _fresh(tmp)
            try:
                got = _untar_py(arc, tmp, members, a["label"], prog)
            except Exception as e:      # tarfile/lzma/OS errors: report them like tar's
                raise SystemExit(f"unpacking {a['label']} failed: {type(e).__name__}: {e}")
            missing = [m for m in members if not any(g == m or g.startswith(m + "/") for g in got)]
            if missing:
                raise SystemExit(f"unpacking {a['label']} failed: not in the archive: {', '.join(missing)}")
    src = os.path.join(tmp, a["top"]) if a["top"] else tmp
    if os.path.isdir(dest):
        shutil.rmtree(dest)
    os.replace(src, dest)
    if os.path.isdir(tmp):
        shutil.rmtree(tmp)
    os.remove(arc)
    _ready(dest, a["label"])
    print(f"  {a['label']}: tools-cache\\{key}", flush=True)


# ---------------------------------------------------------------- MSVC + Windows SDK (portable-msvc)

def _first(items, cond=lambda x: True):
    return next((i for i in items if cond(i)), None)


def _nearest(versions, want):
    """want if offered, else the closest version (by number) to it."""
    if want in versions:
        return want
    key = lambda v: abs(float(v) - float(want)) if v.replace(".", "").isdigit() else 1e9
    return min(versions, key=key)


def _msi_cabs(data):
    i = 0
    while True:
        i = data.find(b".cab", i + 4)
        if i < 0:
            return
        yield data[i - 32:i + 4].decode("ascii")


def plan_msvc():
    """Microsoft's manifest -> what to download: [(url, sha256, size, file name, kind)], versions."""
    print("  reading Microsoft's Visual Studio package list", flush=True)
    channel = json.loads(fetch(CHANNEL))
    vs = _first(channel["channelItems"], lambda x: x["id"] == "Microsoft.VisualStudio.Manifests.VisualStudio")
    man = json.loads(fetch(vs["payloads"][0]["url"]))
    packages = {}
    for p in man["packages"]:
        packages.setdefault(p["id"].lower(), []).append(p)
    msvc, sdk = {}, {}
    for pid in packages:
        if pid.startswith("microsoft.vc.") and pid.endswith(".tools.hostx64.targetx64.base") and "premium" not in pid:
            ver = ".".join(pid.split(".")[2:4])
            if ver[0].isdigit():
                msvc[ver] = pid
        elif pid.startswith(("microsoft.visualstudio.component.windows10sdk.", "microsoft.visualstudio.component.windows11sdk.")):
            ver = pid.split(".")[-1]
            if ver.isdigit():
                sdk[ver] = pid
    mv, sv = _nearest(list(msvc), MSVC_VERSION), _nearest(list(sdk), SDK_VERSION)
    full = msvc[mv][len("microsoft.vc."):-len(".tools.hostx64.targetx64.base")]
    files = []
    # Headers, libraries, and the x64 tools package (clang-cl and lld-link do the compiling).
    # crt.x64.desktop.base has only the static runtime (libcmt, libcpmt); crt.x64.store.base, despite
    # its name, has the DLL runtime's msvcrt.lib, vcruntime.lib, msvcprt.lib, oldnames.lib, delayimp.lib.
    for name in ("crt.headers.base", "crt.x64.desktop.base", "crt.x64.store.base", "tools.hostx64.targetx64.base"):
        p = _first(packages[f"microsoft.vc.{full}.{name}"], lambda p: p.get("language") in (None, "en-US"))
        for pl in p["payloads"]:
            files.append((pl["url"], pl["sha256"], pl.get("size", 0), pl["fileName"], "vsix"))
    sdk_pkg = packages[_first(packages[sdk[sv]][0]["dependencies"]).lower()][0]
    want = ["Windows SDK for Windows Store Apps Tools-x86_en-us.msi",
            "Windows SDK for Windows Store Apps Headers-x86_en-us.msi",
            "Windows SDK for Windows Store Apps Headers OnecoreUap-x86_en-us.msi",
            "Windows SDK for Windows Store Apps Libs-x86_en-us.msi",
            "Universal CRT Headers Libraries and Sources-x86_en-us.msi",
            "Windows SDK Desktop Libs x64-x86_en-us.msi"]
    for t in ("x86", "x64", "arm", "arm64"):
        want += [f"Windows SDK Desktop Headers {t}-x86_en-us.msi", f"Windows SDK OnecoreUap Headers {t}-x86_en-us.msi"]
    pay = {pl["fileName"].replace("\\", "/"): pl for pl in sdk_pkg["payloads"]}
    msis = [pay["Installers/" + w] for w in sorted(want) if "Installers/" + w in pay]
    for pl in msis:
        files.append((pl["url"], pl["sha256"], pl.get("size", 0), pl["fileName"].replace("\\", "/").split("/")[-1], "msi"))
    return files, pay, full, sv


def get_msvc(files, pay, full, sv, prog):
    d = os.path.join(DL, "msvc")
    os.makedirs(d, exist_ok=True)
    msvc_tmp = os.path.join(CACHE, "msvc.unpack")
    _fresh(msvc_tmp)
    msis = []
    for url, sha, size, name, kind in files:
        path = os.path.join(d, name)
        download(url, path, sha, size, f"Microsoft C++ files ({name})", prog)
        if kind == "vsix":
            with zipfile.ZipFile(path) as z:
                for m in z.namelist():
                    if m.startswith("Contents/") and not m.endswith("/"):
                        out = os.path.join(msvc_tmp, *m[len("Contents/"):].split("/"))
                        os.makedirs(os.path.dirname(out), exist_ok=True)
                        with z.open(m) as src, open(out, "wb") as dst:
                            shutil.copyfileobj(src, dst)
            os.remove(path)
        else:
            msis.append(path)
    cabs = set()
    for m in msis:
        with open(m, "rb") as f:
            cabs.update(_msi_cabs(f.read()))
    cab_payloads = [pay["Installers/" + c] for c in sorted(cabs) if "Installers/" + c in pay]
    prog.total += sum(pl.get("size", 0) for pl in cab_payloads) - CABS_ESTIMATE   # the estimate becomes the real size
    for pl in cab_payloads:
        name = pl["fileName"].replace("\\", "/").split("/")[-1]
        download(pl["url"], os.path.join(d, name), pl["sha256"], pl.get("size", 0), "Windows SDK files", prog)
    # msiexec /a only unpacks (an "administrative install" into a folder): no install, no admin.
    sdk_tmp = os.path.join(CACHE, "sdk.unpack")
    _fresh(sdk_tmp)
    for i, m in enumerate(msis, 1):
        prog.say(f"unpacking the Windows SDK ({i} of {len(msis)})", force=True)
        for attempt in range(8):
            r = subprocess.run(f'msiexec.exe /a "{m}" /quiet /qn TARGETDIR="{sdk_tmp}"', timeout=900)
            if r.returncode != 1618:          # 1618: another installation is running: wait for it
                break
            time.sleep(15)
        if r.returncode:
            raise SystemExit(f"unpacking {os.path.basename(m)} failed (msiexec exit code {r.returncode})")
        copied = os.path.join(sdk_tmp, os.path.basename(m))
        if os.path.isfile(copied):
            os.remove(copied)
    kits = os.path.join(sdk_tmp, "Windows Kits", "10")
    vc_root = os.path.join(msvc_tmp, "VC", "Tools", "MSVC")
    if not os.path.isdir(vc_root) or not os.path.isdir(os.path.join(kits, "Include")):
        raise SystemExit("the MSVC / Windows SDK files are incomplete after unpacking")
    for dest, src in ((os.path.join(CACHE, "msvc"), msvc_tmp), (os.path.join(CACHE, "sdk"), kits)):
        if os.path.isdir(dest):
            shutil.rmtree(dest)
        os.replace(src, dest)
    shutil.rmtree(sdk_tmp, ignore_errors=True)
    shutil.rmtree(d, ignore_errors=True)
    _ready(os.path.join(CACHE, "msvc"), "MSVC " + full)
    _ready(os.path.join(CACHE, "sdk"), "Windows SDK 10.0." + sv)
    print(f"  MSVC {full} and Windows SDK 10.0.{sv}: tools-cache\\msvc, tools-cache\\sdk", flush=True)


# ---------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--accept-msvc-license", action="store_true")
    a = ap.parse_args()
    t0 = time.time()
    get = [k for k in ("cmake", "ninja", "llvm") if not port_paths.cached(k) and not port_paths.configured(k)]
    msvc = not (port_paths.cached("msvc") and port_paths.cached("sdk")) and \
        not (port_paths.configured("msvc") and port_paths.configured("sdk")) and not port_paths.configured("vcvars")
    if msvc and not a.accept_msvc_license:
        raise SystemExit("The C++ compiler parts (MSVC and the Windows SDK) are Microsoft's, under the Visual Studio "
                         f"Build Tools license: {MSVC_LICENSE}\nAccept it in Setup, or run "
                         "tools\\fetch_tools.ps1 -AcceptMsvcLicense")
    if not get and not msvc:
        print("  build tools: all there already", flush=True)
        return
    os.makedirs(DL, exist_ok=True)
    prog = Progress()
    plan = plan_msvc() if msvc else None
    prog.total = sum(ARCHIVES[k]["size"] for k in get) + (sum(f[2] for f in plan[0]) + CABS_ESTIMATE if plan else 0)
    print(f"  downloading about {mb(prog.total)} of build tools (temporary)", flush=True)
    if plan:
        get_msvc(*plan, prog)
    for k in get:
        get_archive(k, prog)
    shutil.rmtree(DL, ignore_errors=True)
    print(f"  progress: 100% build tools ready", flush=True)
    print(f"  build tools ready: downloaded {mb(prog.done)} in {time.time() - t0:.0f} s", flush=True)


if __name__ == "__main__":
    main()
