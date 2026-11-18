"""Build the third-party pieces the Vulkan backend needs (into deps/; tools/fetch_deps.ps1 downloads them).
  deps/Vulkan-Headers   KhronosGroup/Vulkan-Headers v1.3.296 (headers only)
  deps/SDL2-2.30.10     libsdl-org SDL2-devel-2.30.10-VC.zip (+ deps/sdl2-inc/SDL2/*.h)
  deps/glslang-src      KhronosGroup/glslang 15.1.0 -> deps/glslang-install-mt (static, HLSL on, no optimizer)
glslang is built with the static C/C++ runtime (/MT), as the engine is (tools/rebuild_win.py): the engine exe then
needs no Visual C++ runtime DLLs (MSVCP140, VCRUNTIME140, VCRUNTIME140_1) on the player's PC. A deps/glslang-install
of an older checkout (the DLL runtime, /MD) is left alone; the engine build takes the -mt one when it is there.
The compile environment: tools/vsenv.py (the portable MSVC in tools-cache, else Visual Studio).
CMake, Ninja, LLVM: tools/port_paths.py (tools-cache, config.local.json, or found on this PC).

Every program runs with its arguments as a list (no cmd.exe in between): the batch file this replaces read the
"&" of "E:\\Decomps & Recomps\\..." as a second command (GitHub #34), and the ")" of "(x86)" before that (#19).

    python -I tools/build_deps.py        (tools/build_deps.cmd runs this)
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import port_paths  # noqa: E402
from vsenv import vs_env, cmake_cache_fits  # noqa: E402

ROOT = os.path.dirname(HERE)
D = os.path.join(ROOT, "deps")


def main():
    cmake, ninja = port_paths.get("cmake"), port_paths.get("ninja")
    env = vs_env()
    src, build, inst = os.path.join(D, "glslang-src"), os.path.join(D, "glslang-build-mt"), os.path.join(D, "glslang-install-mt")
    cmake_cache_fits(build)
    steps = [
        [cmake, "-S", src, "-B", build, "-G", "Ninja", "-DCMAKE_MAKE_PROGRAM=" + ninja,
         "-DCMAKE_C_COMPILER=clang-cl", "-DCMAKE_CXX_COMPILER=clang-cl", "-DCMAKE_BUILD_TYPE=Release",
         "-DPython3_EXECUTABLE=" + sys.executable, "-DPYTHON_EXECUTABLE=" + sys.executable,
         "-DCMAKE_INSTALL_PREFIX=" + inst, "-DBUILD_SHARED_LIBS=OFF", "-DBUILD_EXTERNAL=OFF",
         "-DCMAKE_POLICY_DEFAULT_CMP0091=NEW", "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded",   # /MT: see the top
         "-DENABLE_OPT=OFF", "-DENABLE_GLSLANG_BINARIES=OFF", "-DGLSLANG_TESTS=OFF", "-DENABLE_HLSL=ON",
         "-DENABLE_SPIRV=ON", "-DGLSLANG_ENABLE_INSTALL=ON"],
        [cmake, "--build", build],
        [cmake, "--install", build],
    ]
    for cmd in steps:
        sys.stdout.flush()
        r = subprocess.run(cmd, env=env)
        if r.returncode:
            print(f"build_deps: {' '.join(cmd[:3])} ... exited {r.returncode}", flush=True)
            return r.returncode
    return 0


if __name__ == "__main__":
    sys.exit(main())
