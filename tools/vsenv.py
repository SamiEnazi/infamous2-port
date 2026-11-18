"""The x64 compile environment as a dict, with LLVM's bin first on PATH (clang-cl and lld-link
build the port; tools/port_paths.py finds every tool).

  - The portable MSVC + Windows SDK in tools-cache (tools/fetch_tools.ps1), when they are there:
    INCLUDE, LIB and PATH set here, the way portable-msvc's setup script does. No vcvars64.bat.
    With PORT_ONLY_CACHE=1, PATH holds only the tools-cache tools and Windows' own folders.
  - Else Visual Studio's vcvars64.bat (cached in build/vsenv.json).

    from vsenv import vs_env; subprocess.run([...], env=vs_env())
    python tools/vsenv.py run <program> [args...]     runs one program in that environment
"""
import json, os, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import port_paths

CACHE = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "build", "vsenv.json")


def _path_key(env):
    return next((k for k in env if k.upper() == "PATH"), "PATH")


def _windows_path():
    w = os.environ.get("SystemRoot", r"C:\Windows")
    return [os.path.join(w, "System32"), w, os.path.join(w, "System32", "Wbem"),
            os.path.join(w, "System32", "WindowsPowerShell", "v1.0")]


def _latest(folder, prefix=""):
    names = sorted(d for d in os.listdir(folder) if d.startswith(prefix) and os.path.isdir(os.path.join(folder, d)))
    if not names:
        raise SystemExit(f"vsenv: nothing in {folder}")
    return names[-1]


def portable_env(msvc, sdk):
    vcv = _latest(os.path.join(msvc, "VC", "Tools", "MSVC"))
    vc = os.path.join(msvc, "VC", "Tools", "MSVC", vcv)
    sdkv = _latest(os.path.join(sdk, "Include"), "10.")
    inc, lib, sbin = os.path.join(sdk, "Include", sdkv), os.path.join(sdk, "Lib", sdkv), os.path.join(sdk, "bin", sdkv, "x64")
    env = dict(os.environ)
    for k in ("VSINSTALLDIR", "VCINSTALLDIR", "DevEnvDir", "VS170COMNTOOLS", "EXTERNAL_INCLUDE", "LIBPATH", "INCLUDE", "LIB"):
        for kk in [x for x in env if x.upper() == k.upper()]:
            env.pop(kk)
    tools = [os.path.join(vc, "bin", "Hostx64", "x64"), sbin, os.path.join(sbin, "ucrt")]
    for key in ("cmake", "ninja", "python"):
        p = port_paths.cached(key)
        if p:
            tools.append(os.path.dirname(p))
    pk = _path_key(env)
    rest = _windows_path() if port_paths.only_cache() else env.get(pk, "").split(os.pathsep)
    env[pk] = os.pathsep.join(tools + rest)
    env["INCLUDE"] = os.pathsep.join([os.path.join(vc, "include")] +
                                     [os.path.join(inc, d) for d in ("ucrt", "shared", "um", "winrt", "cppwinrt")])
    env["LIB"] = os.pathsep.join([os.path.join(vc, "lib", "x64"), os.path.join(lib, "ucrt", "x64"), os.path.join(lib, "um", "x64")])
    env.update(VCToolsInstallDir=vc + "\\", VCToolsVersion=vcv, WindowsSdkDir=sdk + "\\", WindowsSDKVersion=sdkv + "\\",
               UniversalCRTSdkDir=sdk + "\\", UCRTVersion=sdkv, WindowsSdkBinPath=os.path.join(sdk, "bin") + "\\",
               VSCMD_ARG_HOST_ARCH="x64", VSCMD_ARG_TGT_ARCH="x64")
    return env


def rc_compiler():
    """The resource compiler for the exe's icon: the portable SDK's rc.exe, else LLVM's llvm-rc."""
    pm = port_paths.portable_msvc()
    if not pm:
        return None                      # Visual Studio: CMake finds the SDK's rc.exe itself
    sdk = pm[1]
    rc = os.path.join(sdk, "bin", _latest(os.path.join(sdk, "Include"), "10."), "x64", "rc.exe")
    if not os.path.isfile(rc):
        rc = os.path.join(port_paths.get("llvm"), "bin", "llvm-rc.exe")
    return rc.replace("\\", "/")        # CMake writes it into a .cmake file as is: "\i" would be an escape


def _vcvars_env():
    vcvars = port_paths.get("vcvars")
    env = None
    if os.path.isfile(CACHE) and os.path.getmtime(CACHE) > os.path.getmtime(vcvars):
        env = json.load(open(CACHE))
        if env.pop("__vcvars", vcvars) != vcvars:   # cached from another Visual Studio
            env = None
    if env is None:
        out = subprocess.run(f'cmd /c ""{vcvars}" >nul && set"', capture_output=True, text=True, shell=True).stdout
        env = dict(line.split("=", 1) for line in out.splitlines() if "=" in line and not line.startswith("="))
        os.makedirs(os.path.dirname(CACHE), exist_ok=True)
        json.dump(dict(env, __vcvars=vcvars), open(CACHE, "w"))
    return env


def _plain(p):
    """A path every build tool takes as is (tools/safe_paths.ps1 Test-PlainPath)."""
    return bool(p) and len(p) <= 80 and all(c.isascii() and (c.isalnum() or c in " _.-\\:()") for c in p)


def vs_env():
    pm = port_paths.portable_msvc()
    env = portable_env(*pm) if pm else _vcvars_env()
    llvm_bin = os.path.join(port_paths.get("llvm"), "bin")
    pk = _path_key(env)
    env[pk] = llvm_bin + os.pathsep + env.get(pk, "")
    # The compilers' temporary files: a user name in another alphabet or with "&" in it ("C:\Users\J&J\AppData\Local
    # \Temp") is a path some of them (rc.exe, ninja's subprocesses through cmd) can't take: then the build's own
    # folder (GitHub #34; Setup builds through a plain path, tools/safe_paths.ps1 Get-BuildRoot).
    tmp = env.get("TEMP") or env.get("TMP") or ""
    if not _plain(tmp):
        own = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "work", "tmp")
        if _plain(own):
            os.makedirs(own, exist_ok=True)
            for k in [k for k in env if k.upper() in ("TEMP", "TMP")]:
                env.pop(k)
            env["TEMP"] = env["TMP"] = own
    return env


def cmake_cache_fits(build):
    """False when build/CMakeCache.txt was made for another folder (the port moved, or it is built through a
    plain-named link now: GitHub #34); CMake then stops with "does not match the directory where it was created".
    Such a cache and CMake's own files are deleted here, so the folder is configured again (everything compiles
    again once)."""
    cache = os.path.join(build, "CMakeCache.txt")
    if not os.path.isfile(cache):
        return True
    made = None
    with open(cache, encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.startswith("CMAKE_CACHEFILE_DIR:INTERNAL="):
                made = line.split("=", 1)[1].strip()
                break
    norm = lambda p: os.path.normcase(os.path.normpath(p.replace("/", os.sep)))
    if made is None or norm(made) == norm(os.path.abspath(build)):
        return True
    print(f"  {os.path.basename(build)}: its build files were made in {made}: configured again here", flush=True)
    for n in ("CMakeCache.txt", "build.ninja", ".ninja_log", ".ninja_deps", "cmake_install.cmake"):
        p = os.path.join(build, n)
        if os.path.isfile(p):
            os.remove(p)
    import shutil
    shutil.rmtree(os.path.join(build, "CMakeFiles"), ignore_errors=True)
    return False


if __name__ == "__main__":
    if len(sys.argv) < 3 or sys.argv[1] != "run":
        sys.exit("usage: vsenv.py run <program> [args...]")
    sys.exit(subprocess.run(sys.argv[2:], env=vs_env()).returncode)
