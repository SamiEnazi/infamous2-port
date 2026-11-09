"""List POSIX-only #includes and calls that no platform #if protects.

Tracks the #if/#ifdef/#elif/#else/#endif stack per file and reports any POSIX
header include (and a few POSIX calls) whose enclosing conditions never mention
a platform macro. Heuristic, but deterministic: a starting list for the Windows
build, not a proof.

    python -I tools/posix_guard_scan.py <toolkit_dir> [--calls]
"""
import os
import re
import sys

POSIX_HEADERS = {
    "pthread.h", "unistd.h", "sched.h", "sys/mman.h", "signal.h", "dlfcn.h",
    "sys/time.h", "ucontext.h", "dirent.h", "sys/syscall.h", "sys/resource.h",
    "sys/wait.h", "termios.h", "poll.h", "sys/socket.h", "netinet/in.h",
    "arpa/inet.h", "netdb.h", "sys/ioctl.h", "execinfo.h", "linux/futex.h",
    "mach/mach.h", "libkern/OSCacheControl.h", "os/lock.h", "sys/sysctl.h",
    "semaphore.h", "fcntl.h", "sys/select.h", "spawn.h", "pwd.h",
}
POSIX_CALLS = re.compile(
    r"\b(pthread_\w+|mprotect|mmap|munmap|sigaction|sched_setaffinity|"
    r"dlopen|dlsym|usleep|nanosleep|clock_gettime|gettid|syscall|backtrace|"
    r"posix_memalign|madvise|sysconf)\s*\(")
PLATFORM = re.compile(
    r"_WIN32|_WIN64|_MSC_VER|__MINGW|__APPLE__|__MACH__|__linux__|__unix__|"
    r"__unix|_POSIX|__FreeBSD__|__ANDROID__|PS3RECOMP_POSIX|TARGET_OS|WIN32")
SKIP_DIRS = {"third_party", "thirdparty", "external", "imgui", ".git", "tests", "build"}
EXTS = (".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".m", ".mm")


def scan(path, want_calls):
    hits = []
    stack = []  # list of condition strings currently open
    with open(path, encoding="utf-8", errors="ignore") as f:
        lines = f.readlines()
    i = 0
    while i < len(lines):
        line = lines[i]
        while line.rstrip().endswith("\\") and i + 1 < len(lines):
            i += 1
            line = line.rstrip()[:-1] + lines[i]
        s = line.strip()
        m = re.match(r"#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)", s)
        if m:
            kw, cond = m.group(1), m.group(2)
            if kw in ("if", "ifdef", "ifndef"):
                stack.append(cond)
            elif kw in ("elif", "else"):
                if stack:
                    # an #else/#elif branch is still platform-guarded if the
                    # chain it belongs to was
                    stack[-1] = stack[-1] + " " + cond
            elif kw == "endif" and stack:
                stack.pop()
        else:
            guarded = any(PLATFORM.search(c) for c in stack)
            inc = re.match(r'#\s*include\s*[<"]([^>"]+)[>"]', s)
            if inc and inc.group(1) in POSIX_HEADERS and not guarded:
                hits.append((i + 1, "include", inc.group(1)))
            elif want_calls and not guarded and not s.startswith(("//", "*", "/*")):
                c = POSIX_CALLS.search(line)
                if c:
                    hits.append((i + 1, "call", c.group(1)))
        i += 1
    return hits


def main():
    root = sys.argv[1]
    want_calls = "--calls" in sys.argv
    total = 0
    for d, dirs, files in os.walk(root):
        dirs[:] = [x for x in dirs if x.lower() not in SKIP_DIRS]
        for fn in sorted(files):
            if not fn.endswith(EXTS) or fn.endswith((".m", ".mm")):
                continue  # Objective-C files are Apple-only by construction
            p = os.path.join(d, fn)
            hits = scan(p, want_calls)
            if hits:
                rel = os.path.relpath(p, root)
                kinds = {}
                for ln, k, name in hits:
                    kinds.setdefault(name, []).append(ln)
                desc = ", ".join(f"{n}@{','.join(map(str, l[:4]))}{'…' if len(l) > 4 else ''}"
                                 for n, l in kinds.items())
                print(f"{rel}: {desc}")
                total += len(hits)
    print(f"-- {total} unguarded POSIX uses")


if __name__ == "__main__":
    main()
