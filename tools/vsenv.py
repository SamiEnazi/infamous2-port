"""The VS2022 x64 developer environment (vcvars64) as a dict, cached, with LLVM's bin
first on PATH (clang-cl and lld-link build the port; tools/port_paths.py finds both).

    from vsenv import vs_env; subprocess.run([...], env=vs_env())
"""
import json, os, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import port_paths

CACHE = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "build", "vsenv.json")


def vs_env():
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
    llvm_bin = os.path.join(port_paths.get("llvm"), "bin")
    path_key = next((k for k in env if k.upper() == "PATH"), "PATH")
    env[path_key] = llvm_bin + os.pathsep + env.get(path_key, "")
    return env
