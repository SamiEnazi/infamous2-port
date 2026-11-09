"""Where this PC keeps what the build and the game need.

Each value comes from, in order:
  1. an environment variable (PORT_RPCS3, PORT_VCVARS, PORT_NINJA, PORT_LLVM)
  2. config.local.json in the project folder (Setup.cmd writes it; it is not in git)
  3. auto-detection (vswhere, PATH, the default install folders)

    from port_paths import get; get("rpcs3")
    python tools/port_paths.py <key>        prints one value (for .cmd files)

Keys: rpcs3   the RPCS3 folder (rpcs3.exe, firmware installed: dev_flash\\sys\\external)
      vcvars  Visual Studio 2022's vcvars64.bat (MSVC headers/libs + Windows SDK)
      ninja   ninja.exe
      llvm    the LLVM folder whose bin\\clang-cl.exe and lld-link.exe build the port
"""
import json
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CONFIG = os.path.join(ROOT, "config.local.json")
ENV = {"rpcs3": "PORT_RPCS3", "vcvars": "PORT_VCVARS", "ninja": "PORT_NINJA", "llvm": "PORT_LLVM"}
HINT = {
    "rpcs3": "the RPCS3 folder (rpcs3.exe, with the PS3 firmware installed)",
    "vcvars": "Visual Studio 2022 or its Build Tools with the C++ workload (vcvars64.bat)",
    "ninja": "ninja.exe (winget install Ninja-build.Ninja, or Visual Studio's CMake tools)",
    "llvm": "LLVM with clang-cl (winget install LLVM.LLVM)",
}


def _config():
    try:
        with open(CONFIG, encoding="utf-8-sig") as f:
            return json.load(f)
    except FileNotFoundError:
        return {}


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


def _detect_llvm():
    p = shutil.which("clang-cl")
    if p:
        return os.path.dirname(os.path.dirname(p))
    c = os.path.join(os.environ.get("ProgramFiles", r"C:\Program Files"), "LLVM")
    return c if os.path.isfile(os.path.join(c, "bin", "clang-cl.exe")) else None


DETECT = {"rpcs3": lambda: None, "vcvars": _detect_vcvars, "ninja": _detect_ninja, "llvm": _detect_llvm}


def get(key, required=True):
    v = os.environ.get(ENV[key]) or _config().get(key) or DETECT[key]()
    if not v and required:
        raise SystemExit(f"port_paths: no {key} found -- {HINT[key]}. Run Setup.cmd, or set "
                         f"{ENV[key]}, or \"{key}\" in config.local.json")
    return v


if __name__ == "__main__":
    if len(sys.argv) != 2 or sys.argv[1] not in ENV:
        sys.exit("usage: port_paths.py " + "|".join(ENV))
    print(get(sys.argv[1]))
