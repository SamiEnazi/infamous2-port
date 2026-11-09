"""Host-level sampling profiler for the running native build (Windows, stdlib only).

Suspends the busiest thread(s) of Infamous2Recomp.exe every few ms, reads RIP
and the top of the stack, and resolves them through DbgHelp + the build's PDB
(tools/rebuild_win.py links with /DEBUG). Unlike the in-process guest sampler,
it names the HOST function the thread is in (a runtime helper, a spin, a
syscall wrapper, ntdll) and, beside it, the nearest lifted guest function
(func_XXXXXXXX) found on the stack -- who called it.

    python -I tools/host_sample.py [--seconds 10] [--interval 2] [--threads 1]
                                   [--delay 0] [--tid <host tid>] [--out <file>]

--threads N samples the N threads with the most CPU over a 1 s probe (or --tid).
"""
import argparse
import collections
import ctypes
import ctypes.wintypes as W
import os
import sys
import time

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
dbg = ctypes.WinDLL("dbghelp", use_last_error=True)

TH32CS_SNAPPROCESS, TH32CS_SNAPTHREAD = 0x2, 0x4
PROCESS_ALL = 0x0400 | 0x0010 | 0x0008 | 0x0020   # query, vm read, vm op, vm write
THREAD_ACCESS = 0x0002 | 0x0008 | 0x0040           # suspend/resume, get context, query


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [("dwSize", W.DWORD), ("cntUsage", W.DWORD), ("th32ProcessID", W.DWORD),
                ("th32DefaultHeapID", ctypes.c_size_t), ("th32ModuleID", W.DWORD),
                ("cntThreads", W.DWORD), ("th32ParentProcessID", W.DWORD),
                ("pcPriClassBase", ctypes.c_long), ("dwFlags", W.DWORD), ("szExeFile", ctypes.c_wchar * 260)]


class THREADENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", W.DWORD), ("cntUsage", W.DWORD), ("th32ThreadID", W.DWORD),
                ("th32OwnerProcessID", W.DWORD), ("tpBasePri", ctypes.c_long),
                ("tpDeltaPri", ctypes.c_long), ("dwFlags", W.DWORD)]


class SYMBOL_INFO(ctypes.Structure):
    _fields_ = [("SizeOfStruct", W.ULONG), ("TypeIndex", W.ULONG), ("Reserved", ctypes.c_uint64 * 2),
                ("Index", W.ULONG), ("Size", W.ULONG), ("ModBase", ctypes.c_uint64), ("Flags", W.ULONG),
                ("Value", ctypes.c_uint64), ("Address", ctypes.c_uint64), ("Register", W.ULONG),
                ("Scope", W.ULONG), ("Tag", W.ULONG), ("NameLen", W.ULONG), ("MaxNameLen", W.ULONG),
                ("Name", ctypes.c_char * 512)]


k32.OpenProcess.restype = W.HANDLE
k32.OpenThread.restype = W.HANDLE
k32.CreateToolhelp32Snapshot.restype = W.HANDLE
dbg.SymFromAddr.argtypes = [W.HANDLE, ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(SYMBOL_INFO)]
dbg.SymFunctionTableAccess64.restype = ctypes.c_void_p
dbg.SymFunctionTableAccess64.argtypes = [W.HANDLE, ctypes.c_uint64]
dbg.SymGetModuleBase64.restype = ctypes.c_uint64
dbg.SymGetModuleBase64.argtypes = [W.HANDLE, ctypes.c_uint64]
FTA = ctypes.WINFUNCTYPE(ctypes.c_void_p, W.HANDLE, ctypes.c_uint64)(("SymFunctionTableAccess64", dbg))
GMB = ctypes.WINFUNCTYPE(ctypes.c_uint64, W.HANDLE, ctypes.c_uint64)(("SymGetModuleBase64", dbg))
dbg.StackWalk64.argtypes = [W.DWORD, W.HANDLE, W.HANDLE, ctypes.c_void_p, ctypes.c_void_p,
                            ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]


def find_pid(name="Infamous2Recomp.exe"):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    e = PROCESSENTRY32W(); e.dwSize = ctypes.sizeof(e)
    ok = k32.Process32FirstW(snap, ctypes.byref(e))
    pid = None
    while ok:
        if e.szExeFile.lower() == name.lower():
            pid = e.th32ProcessID
        ok = k32.Process32NextW(snap, ctypes.byref(e))
    k32.CloseHandle(snap)
    return pid


def threads_of(pid):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    e = THREADENTRY32(); e.dwSize = ctypes.sizeof(e)
    ok = k32.Thread32First(snap, ctypes.byref(e))
    out = []
    while ok:
        if e.th32OwnerProcessID == pid:
            out.append(e.th32ThreadID)
        ok = k32.Thread32Next(snap, ctypes.byref(e))
    k32.CloseHandle(snap)
    return out


def thread_cpu(h):
    c, x, kt, ut = W.FILETIME(), W.FILETIME(), W.FILETIME(), W.FILETIME()
    if not k32.GetThreadTimes(h, ctypes.byref(c), ctypes.byref(x), ctypes.byref(kt), ctypes.byref(ut)):
        return 0
    f = lambda t: (t.dwHighDateTime << 32) | t.dwLowDateTime
    return f(kt) + f(ut)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=10)
    ap.add_argument("--interval", type=float, default=2.0, help="ms between samples")
    ap.add_argument("--threads", type=int, default=1)
    ap.add_argument("--tid", type=int, default=0)
    ap.add_argument("--delay", type=float, default=0)
    ap.add_argument("--out", default="")
    ap.add_argument("--walk", type=int, default=0,
                    help="unwind N frames with StackWalk64 (the exe's unwind tables) instead of "
                         "scanning the stack for return addresses: exact callers, slower")
    a = ap.parse_args()
    if a.delay:
        time.sleep(a.delay)
    pid = find_pid()
    if not pid:
        sys.exit("Infamous2Recomp.exe is not running")
    hp = k32.OpenProcess(PROCESS_ALL, False, pid)
    exe_dir = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "work", "port", "build")
    dbg.SymSetOptions(0x2 | 0x4 | 0x200)   # UNDNAME | DEFERRED_LOADS | FAIL_CRITICAL_ERRORS
    if not dbg.SymInitializeW(hp, ctypes.c_wchar_p(exe_dir), True):
        sys.exit("SymInitialize failed %d" % ctypes.get_last_error())

    # the busiest threads over a 1 s probe
    hs = {}
    for t in threads_of(pid):
        h = k32.OpenThread(THREAD_ACCESS, False, t)
        if h:
            hs[t] = h
    if a.tid:
        pick = [a.tid]
    else:
        c0 = {t: thread_cpu(h) for t, h in hs.items()}
        time.sleep(1.0)
        c1 = {t: thread_cpu(h) for t, h in hs.items()}
        busy = sorted(hs, key=lambda t: -(c1[t] - c0[t]))
        pick = busy[:a.threads]
        for t in pick:
            print(f"thread {t}: {(c1[t] - c0[t]) / 1e5:.0f}% CPU in the probe second")

    ctx_buf = ctypes.create_string_buffer(1232 + 16)
    base = (ctypes.addressof(ctx_buf) + 15) & ~15
    stack = ctypes.create_string_buffer(8192)
    sf = ctypes.create_string_buffer(1024)   # STACKFRAME64 (+ KDHELP64), zeroed per walk
    raw = collections.defaultdict(list)
    t_end = time.time() + a.seconds
    n = 0
    while time.time() < t_end:
        for t in pick:
            h = hs.get(t) or k32.OpenThread(THREAD_ACCESS, False, t)
            if k32.SuspendThread(h) == 0xFFFFFFFF:
                continue
            ctypes.c_uint32.from_address(base + 0x30).value = 0x10000B if a.walk else 0x100001
            ok = k32.GetThreadContext(h, ctypes.c_void_p(base))
            rip = ctypes.c_uint64.from_address(base + 0xF8).value
            rsp = ctypes.c_uint64.from_address(base + 0x98).value
            got = ctypes.c_size_t(0)
            words = []
            if ok and a.walk:
                ctypes.memset(sf, 0, ctypes.sizeof(sf))
                for off, val in ((0, rip), (32, ctypes.c_uint64.from_address(base + 0xA0).value), (48, rsp)):
                    ctypes.c_uint64.from_address(ctypes.addressof(sf) + off).value = val
                    ctypes.c_uint32.from_address(ctypes.addressof(sf) + off + 12).value = 3   # AddrModeFlat
                for _ in range(a.walk):
                    if not dbg.StackWalk64(0x8664, hp, h, ctypes.addressof(sf), base, None,
                                           ctypes.cast(FTA, ctypes.c_void_p), ctypes.cast(GMB, ctypes.c_void_p), None):
                        break
                    pc = ctypes.c_uint64.from_address(ctypes.addressof(sf)).value
                    if not pc:
                        break
                    words.append(pc)
            elif ok:
                k32.ReadProcessMemory(hp, ctypes.c_void_p(rsp), stack, 8192, ctypes.byref(got))
            k32.ResumeThread(h)
            if ok:
                if not a.walk:
                    words = [int.from_bytes(stack.raw[i:i + 8], "little") for i in range(0, got.value, 8)]
                raw[t].append((rip, words))
                n += 1
        time.sleep(a.interval / 1000.0)

    cache = {}
    sym = SYMBOL_INFO(); sym.SizeOfStruct = 88; sym.MaxNameLen = 500
    disp = ctypes.c_uint64(0)

    def name(addr):
        if addr in cache:
            return cache[addr]
        r = None
        if 0x10000 < addr < 0x7FFFFFFFFFFF and dbg.SymFromAddr(hp, addr, ctypes.byref(disp), ctypes.byref(sym)):
            r = sym.Name.decode(errors="replace")
        cache[addr] = r
        return r

    lines = [f"{n} samples over {a.seconds:.0f} s"]
    for t, samples in raw.items():
        leaf = collections.Counter(); pair = collections.Counter(); guest = collections.Counter()
        chain = collections.Counter(); incl = collections.Counter()
        for rip, words in samples:
            lf = name(rip) or "?"
            g = None
            if a.walk:
                fr = [name(w) or "?" for w in words]
                g = next((f for f in fr if f.startswith("func_")), None)
                # the host chain under the leaf (first 5 frames) -- who called the wait
                chain[" < ".join(fr[:6])] += 1
                for f in set(fr):
                    incl[f] += 1
            elif lf.startswith("func_"):
                g = lf
            else:
                for w in words[:512]:
                    if 0x7FF000000000 <= w <= 0x7FFFFFFFFFFF:
                        nm = name(w)
                        if nm and nm.startswith("func_"):
                            g = nm
                            break
            leaf[lf] += 1; guest[g or "-"] += 1; pair[(lf, g or "-")] += 1
        tot = len(samples) or 1
        lines.append(f"\n== thread {t}: {len(samples)} samples")
        lines.append("  host function (leaf):")
        lines += [f"   {c/tot*100:5.1f}%  {k}" for k, c in leaf.most_common(25)]
        lines.append("  nearest guest function on the stack (inclusive):")
        lines += [f"   {c/tot*100:5.1f}%  {k}" for k, c in guest.most_common(20)]
        lines.append("  leaf <- guest:")
        lines += [f"   {c/tot*100:5.1f}%  {k[0]}  <-  {k[1]}" for k, c in pair.most_common(25)]
        if a.walk:
            lines.append("  inclusive (any frame of the walk):")
            lines += [f"   {c/tot*100:5.1f}%  {k}" for k, c in incl.most_common(30)]
            lines.append("  leaf < callers (unwound):")
            lines += [f"   {c/tot*100:5.1f}%  {k}" for k, c in chain.most_common(30)]
    txt = "\n".join(lines)
    print(txt)
    if a.out:
        open(a.out, "w", encoding="utf-8").write(txt)


if __name__ == "__main__":
    main()
