"""Static call-graph helpers over the decrypted EBOOT (PPC64 BE).

    python tools/ppc_xref.py func <addr>            function start containing addr (stdu prologue scan)
    python tools/ppc_xref.py callers <addr> [depth] callers of the function containing addr, recursively
    python tools/ppc_xref.py dis <addr> [len]       disassemble from addr

Direct `bl` only (indirect calls through function pointers / vtables are not seen).
Run with the work venv python (capstone): work\\venv\\Scripts\\python.exe -I tools\\ppc_xref.py ...
"""
import os
import struct
import sys
from functools import lru_cache

from capstone import CS_ARCH_PPC, CS_MODE_64, CS_MODE_BIG_ENDIAN, Cs

ELF = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "work", "EBOOT.elf")
B = open(ELF, "rb").read()
TEXT_LO, TEXT_HI = 0x10000, 0x742E18          # lifter's --code-end for this EBOOT
md = Cs(CS_ARCH_PPC, CS_MODE_64 | CS_MODE_BIG_ENDIAN)


def w32(a):
    return struct.unpack_from(">I", B, a - 0x10000)[0]


def func_start(a):
    p = a & ~3
    while p > TEXT_LO:
        if (w32(p) & 0xFFFF0000) == 0xF8210000:   # stdu r1,-X(r1)
            # the prologue may start a few instructions before the stdu (mflr r0; std ...)
            q = p
            for _ in range(4):
                if w32(q - 4) == 0x4E800020 or (w32(q - 4) & 0xFC000003) == 0x48000000:
                    break
                q -= 4
            return q if w32(q) != 0x4E800020 else p
        p -= 4
    return None


@lru_cache(maxsize=1)
def bl_index():
    idx = {}
    for a in range(TEXT_LO, TEXT_HI, 4):
        w = w32(a)
        if (w >> 26) == 18 and (w & 3) == 1:
            li = w & 0x03FFFFFC
            if li & 0x02000000:
                li -= 0x04000000
            idx.setdefault(a + li, []).append(a)
    return idx


def callers(fa):
    idx = bl_index()
    out = []
    for t in (fa, fa + 4, fa + 8, fa + 12):       # tolerate a slightly-off start
        out += idx.get(t, [])
    return sorted(set(out))


def tree(a, depth, ind=0, seen=None):
    seen = seen if seen is not None else set()
    f = func_start(a)
    print(f"{'  ' * ind}0x{a:08X} in func 0x{f:08X}" if f else f"{'  ' * ind}0x{a:08X} (no func)")
    if not f or f in seen or depth == 0:
        return
    seen.add(f)
    cs = callers(f)
    if not cs:
        print(f"{'  ' * ind}  (no direct callers: called through a pointer/vtable)")
    for c in cs[:12]:
        tree(c, depth - 1, ind + 1, seen)


def dis(a, n):
    for ins in md.disasm(B[a - 0x10000:a - 0x10000 + n], a):
        print(f"  {ins.address:08X}: {ins.mnemonic:8} {ins.op_str}")


if __name__ == "__main__":
    cmd, a = sys.argv[1], int(sys.argv[2], 16)
    if cmd == "func":
        print(hex(func_start(a)))
    elif cmd == "callers":
        tree(a, int(sys.argv[3]) if len(sys.argv) > 3 else 3)
    elif cmd == "dis":
        dis(a, int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x80)
