"""Where this PC keeps what the build and the game need.

Each value comes from, in order:
  1. an environment variable (PORT_RPCS3, PORT_VCVARS, PORT_NINJA, PORT_LLVM, PORT_CMAKE, PORT_PYTHON,
     PORT_MSVC, PORT_SDK)
  2. config.local.json in the project folder (Setup writes "rpcs3"; the rest only if you set them)
  3. tools-cache\\ in the project folder: the portable tools Setup downloads (tools/fetch_tools.ps1)
     and deletes after a successful build
  4. auto-detection on this PC (vswhere, PATH, the default install folders)

PORT_ONLY_CACHE=1 skips 2 and 4 for the build tools (only the environment variable and tools-cache
count): a test that the portable tools alone build the port.

    from port_paths import get; get("rpcs3")
    python tools/port_paths.py <key>        prints one value (for .cmd files)

Keys: rpcs3   the RPCS3 folder (rpcs3.exe, firmware installed: dev_flash\\sys\\external)
      vcvars  Visual Studio 2022's vcvars64.bat (MSVC headers/libs + Windows SDK), when there is no
              portable MSVC (msvc + sdk below)
      msvc    the portable MSVC folder (holds VC\\Tools\\MSVC\\<version>)
      sdk     the portable Windows SDK folder (holds Include, Lib, bin)
      ninja   ninja.exe
      cmake   cmake.exe
      llvm    the LLVM folder whose bin\\clang-cl.exe and lld-link.exe build the port
      python  a Python 3.10+ python.exe (the build's own venv is made from it)
"""
import json
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CONFIG = os.path.join(ROOT, "config.local.json")
CACHE = os.path.join(ROOT, "tools-cache")
ENV = {"rpcs3": "PORT_RPCS3", "vcvars": "PORT_VCVARS", "ninja": "PORT_NINJA", "llvm": "PORT_LLVM",
       "cmake": "PORT_CMAKE", "python": "PORT_PYTHON", "msvc": "PORT_MSVC", "sdk": "PORT_SDK"}
TOOLS = ("vcvars", "ninja", "llvm", "cmake", "python", "msvc", "sdk")
HINT = {
    "rpcs3": "the RPCS3 folder (rpcs3.exe, with the PS3 firmware installed)",
    "vcvars": "Visual Studio 2022 or its Build Tools with the C++ workload (vcvars64.bat), or the portable MSVC "
              "(tools\\fetch_tools.ps1)",
    "ninja": "ninja.exe (tools\\fetch_tools.ps1 downloads it)",
    "llvm": "LLVM with clang-cl (tools\\fetch_tools.ps1 downloads it)",
    "cmake": "cmake.exe (tools\\fetch_tools.ps1 downloads it)",
    "python": "Python 3.10+ (tools\\fetch_tools.ps1 downloads it)",
    "msvc": "the portable MSVC (tools\\fetch_tools.ps1 -AcceptMsvcLicense)",
    "sdk": "the portable Windows SDK (tools\\fetch_tools.ps1 -AcceptMsvcLicense)",
}
# tools-cache\<dir>\.ready is written last, after a complete download and unpack
CACHED = {"ninja": ("ninja", "ninja.exe"), "llvm": ("llvm", ""), "cmake": ("cmake", os.path.join("bin", "cmake.exe")),
          "python": ("python", "python.exe"), "msvc": ("msvc", ""), "sdk": ("sdk", "")}


def only_cache():
    return os.environ.get("PORT_ONLY_CACHE") == "1"


def _config():
    try:
        with open(CONFIG, encoding="utf-8-sig") as f:
            return json.load(f)
    except FileNotFoundError:
        return {}


def cached(key):
    """The tools-cache copy of a tool, if it is there and complete."""
    if key not in CACHED:
        return None
    d, exe = CACHED[key]
    base = os.path.join(CACHE, d)
    if not os.path.isfile(os.path.join(base, ".ready")):
        return None
    return os.path.join(base, exe) if exe else base


def configured(key):
    """Set on purpose for this PC (environment or config.local.json): Setup then downloads nothing for it."""
    if os.environ.get(ENV[key]):
        return True
    return not only_cache() and bool(_config().get(key))


def _vs_install():
    vw = os.path.join(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"),
                      "Microsoft Visual Studio", "Installer", "vswhere.exe")
    if not os.path.isfile(vw):
        return None
    out = subprocess.run([vw, "-latest", "-products", "*", "-requires",
                          "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                          "-property", "installationPath"], capture_output=True, text=True).stdout
    lines = [ln.strip() for ln in out.splitlines() if ln.strip()]
    return lines[0] if lines else None


def _detect_vcvars():
    vs = _vs_install()
    p = os.path.join(vs, "VC", "Auxiliary", "Build", "vcvars64.bat") if vs else None
    return p if p and os.path.isfile(p) else None


def _detect_ninja():
    p = shutil.which("ninja")
    if p:
        return p
    vs = _vs_install()
    if vs:
        p = os.path.join(vs, "Common7", "IDE", "CommonExtensions", "Microsoft", "CMake", "Ninja", "ninja.exe")
        if os.path.isfile(p):
            return p
    return None


def _detect_cmake():
    p = shutil.which("cmake")
    if p:
        return p
    vs = _vs_install()
    if vs:
        p = os.path.join(vs, "Common7", "IDE", "CommonExtensions", "Microsoft", "CMake", "CMake", "bin", "cmake.exe")
        if os.path.isfile(p):
            return p
    return None


def _detect_llvm():
    p = shutil.which("clang-cl")
    if p:
        return os.path.dirname(os.path.dirname(p))
    c = os.path.join(os.environ.get("ProgramFiles", r"C:\Program Files"), "LLVM")
    return c if os.path.isfile(os.path.join(c, "bin", "clang-cl.exe")) else None


def _detect_python():
    p = shutil.which("python")
    # not the Microsoft Store's "python" stand-in, which only offers to install Python
    return p if p and "WindowsApps" not in p else None


DETECT = {"rpcs3": lambda: None, "vcvars": _detect_vcvars, "ninja": _detect_ninja, "llvm": _detect_llvm,
          "cmake": _detect_cmake, "python": _detect_python, "msvc": lambda: None, "sdk": lambda: None}


def get(key, required=True):
    v = os.environ.get(ENV[key])
    if not v and not (only_cache() and key in TOOLS):
        v = _config().get(key)
    if not v:
        v = cached(key)
    if not v and not (only_cache() and key in TOOLS):
        v = DETECT[key]()
    if not v and required:
        raise SystemExit(f"port_paths: no {key} found -- {HINT[key]}. Run Setup, or set "
                         f"{ENV[key]}, or \"{key}\" in config.local.json")
    return v


def portable_msvc():
    """(msvc, sdk) when the portable MSVC + Windows SDK are to be used, else None (Visual Studio's vcvars)."""
    msvc, sdk = get("msvc", required=False), get("sdk", required=False)
    if msvc and sdk:
        return msvc, sdk
    if only_cache():
        raise SystemExit("port_paths: PORT_ONLY_CACHE=1 but tools-cache has no MSVC + Windows SDK "
                         "(tools\\fetch_tools.ps1 -AcceptMsvcLicense)")
    return None


if __name__ == "__main__":
    if len(sys.argv) != 2 or sys.argv[1] not in ENV:
        sys.exit("usage: port_paths.py " + "|".join(ENV))
    print(get(sys.argv[1]))
