"""Read PS3 guest memory out of the RUNNING native build (read-only).

    python -I tools/guestmem.py find                      -> pid, vm_base
    add --rpcs3 (anywhere) to read the SAME guest addresses out of a running RPCS3 (the oracle)
    python -I tools/guestmem.py hex  <ea> <len>           -> hex dump (guest is big-endian)
    python -I tools/guestmem.py u32  <ea> [count]         -> big-endian words
    python -I tools/guestmem.py dump <ea> <len> <file>    -> raw bytes to a file

vm_base is found, not assumed: the first LOAD segment of the EBOOT sits at guest
0x10000 (file offset 0), so the allocation whose +0x10000 holds exactly the ELF's
first bytes is the guest arena.
"""
import ctypes as C
import ctypes.wintypes as W
import os
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ELF = os.path.join(ROOT, "work", "EBOOT.elf")
EXE = "rpcs3.exe" if "--rpcs3" in sys.argv else "Infamous2Recomp.exe"
if "--rpcs3" in sys.argv:
    sys.argv.remove("--rpcs3")
k32 = C.WinDLL("kernel32", use_last_error=True)
PROCESS_QUERY_INFORMATION, PROCESS_VM_READ = 0x0400, 0x0010
MEM_COMMIT = 0x1000


class MBI(C.Structure):
    _fields_ = [("BaseAddress", C.c_void_p), ("AllocationBase", C.c_void_p), ("AllocationProtect", W.DWORD),
                ("PartitionId", W.WORD), ("RegionSize", C.c_size_t), ("State", W.DWORD),
                ("Protect", W.DWORD), ("Type", W.DWORD)]


k32.OpenProcess.restype = W.HANDLE
k32.VirtualQueryEx.argtypes = [W.HANDLE, C.c_void_p, C.POINTER(MBI), C.c_size_t]
k32.VirtualQueryEx.restype = C.c_size_t
k32.ReadProcessMemory.argtypes = [W.HANDLE, C.c_void_p, C.c_void_p, C.c_size_t, C.POINTER(C.c_size_t)]


def find_pid():
    out = subprocess.run(["tasklist", "/fo", "csv", "/nh", "/fi", f"imagename eq {EXE}"],
                         capture_output=True, text=True).stdout
    for ln in out.splitlines():
        parts = [p.strip('"') for p in ln.split('","')]
        if parts and parts[0].lower() == EXE.lower():
            return int(parts[1])
    sys.exit(f"{EXE} is not running")


def rpm(h, addr, n):
    buf = C.create_string_buffer(n)
    got = C.c_size_t(0)
    if not k32.ReadProcessMemory(h, C.c_void_p(addr), buf, n, C.byref(got)) or got.value != n:
        return None
    return buf.raw


def find_vm_base(h):
    sig = open(ELF, "rb").read(64)
    if EXE == "rpcs3.exe":
        # RPCS3 maps guest memory at vm::g_base_addr (0x300000000, page-protected
        # like the guest) and again at a full-access "sudo" view. Probe the usual
        # spots for the EBOOT at guest 0x10000, then fall back to a region scan.
        for cand in (0x400000000, 0x300000000, 0x500000000, 0x600000000):
            if rpm(h, cand + 0x10000, 64) == sig and rpm(h, cand + 0x40000000, 16) is not None:
                return cand
        for cand in (0x300000000, 0x400000000):
            if rpm(h, cand + 0x10000, 64) == sig:
                return cand
    addr, seen = 0, set()
    mbi = MBI()
    while k32.VirtualQueryEx(h, C.c_void_p(addr), C.byref(mbi), C.sizeof(mbi)):
        base = mbi.AllocationBase or 0
        if base and base not in seen and mbi.State == MEM_COMMIT:
            seen.add(base)
            if rpm(h, base + 0x10000, 64) == sig:
                return base
        addr = (mbi.BaseAddress or 0) + mbi.RegionSize
        if addr >= 1 << 47:
            break
    sys.exit("guest arena not found (EBOOT signature at +0x10000 matched nothing)")


def main():
    pid = find_pid()
    h = k32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
    if not h:
        sys.exit(f"OpenProcess({pid}) failed: {C.get_last_error()}")
    vm = find_vm_base(h)
    cmd = sys.argv[1] if len(sys.argv) > 1 else "find"
    if cmd == "find":
        print(f"pid {pid}  vm_base 0x{vm:X}")
        return
    ea = int(sys.argv[2], 16)
    if cmd == "u32":
        n = int(sys.argv[3]) if len(sys.argv) > 3 else 1
        d = rpm(h, vm + ea, 4 * n)
        for i in range(n):
            print(f"{ea + 4 * i:08X}: {struct.unpack_from('>I', d, 4 * i)[0]:08X}" if d else f"{ea:08X}: <unreadable>")
    elif cmd == "hex":
        n = int(sys.argv[3], 16)
        d = rpm(h, vm + ea, n)
        if not d:
            sys.exit("unreadable (uncommitted guest page?)")
        for o in range(0, n, 16):
            row = d[o:o + 16]
            print(f"{ea + o:08X}: " + " ".join(row[i:i + 4].hex() for i in range(0, len(row), 4)))
    elif cmd == "dump":
        n = int(sys.argv[3], 16)
        d = rpm(h, vm + ea, n)
        if d is None:
            # a span with uncommitted pages: read 4 KB at a time, zero-fill the holes
            parts, bad = [], 0
            for o in range(0, n, 0x1000):
                k = min(0x1000, n - o)
                pg = rpm(h, vm + ea + o, k)
                if pg is None:
                    bad += 1
                parts.append(pg or bytes(k))
            d = b"".join(parts)
            print(f"{bad} of {(n + 0xFFF) // 0x1000} pages unreadable (zero-filled)")
        open(sys.argv[4], "wb").write(d)
        print(f"{n:#x} bytes -> {sys.argv[4]}")


if __name__ == "__main__":
    main()
