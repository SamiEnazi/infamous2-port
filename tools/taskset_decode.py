"""Decode a CellSpursTaskset dump (guest bytes, big-endian), RPCS3 cellSpurs.h layout.

    python -I tools/taskset_decode.py <taskset.bin> [base_ea_hex]

Bit vectors are 128-bit, task 0 = MSB of the first byte (RPCS3 uses be_t<v128>
with bit (127 - id)). Prints which tasks exist and their scheduling state, then
each task's ELF / context-save / args.
"""
import struct
import sys

d = open(sys.argv[1], "rb").read()
base = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0
u32 = lambda o: struct.unpack_from(">I", d, o)[0]
u64 = lambda o: struct.unpack_from(">Q", d, o)[0]


def bits(o):
    v = int.from_bytes(d[o:o + 16], "big")
    return [i for i in range(128) if v >> (127 - i) & 1]


names = ["running", "ready", "pending_ready", "enabled", "signalled", "waiting"]
vec = {n: bits(0x10 * i) for i, n in enumerate(names)}
print(f"CellSpursTaskset @0x{base:08X}: spurs=0x{u64(0x60):X} args=0x{u64(0x68):X} "
      f"enable_clear_ls={d[0x70]} x71={d[0x71]} wkl_flag_wait_task={d[0x72]} last_scheduled={d[0x73]} "
      f"wid={u32(0x74)} x78=0x{u64(0x78):X}")
for n in names:
    print(f"  {n:14s} {vec[n]}")
print(f"  size=0x{u32(0x1890):X} exc_handler=0x{u64(0x1880):X} ef_id1=0x{u32(0x1898):X} ef_id2=0x{u32(0x189C):X}")
for t in vec["enabled"]:
    o = 0x80 + 0x30 * t
    args = d[o:o + 16].hex()
    elf, ctx = u64(o + 0x10), u64(o + 0x18)
    print(f"  task {t:3d}: elf=0x{elf:X} ctx_save=0x{ctx:X} args={args} ls_pattern={d[o+0x20:o+0x30].hex()}")
