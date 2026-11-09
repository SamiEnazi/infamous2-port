"""Decode a CellSpurs instance dump (guest bytes, big-endian) into its workload table.

    python -I tools/spurs_decode.py <spurs.bin> [base_ea_hex]

Offsets follow RPCS3's rpcs3/Emu/Cell/Modules/cellSpurs.h (struct CellSpurs).
"""
import struct
import sys

STATE = {0: "NON_EXISTENT", 1: "PREPARING", 2: "RUNNABLE", 3: "SHUTTING_DOWN", 4: "REMOVABLE", 5: "INVALID"}
d = open(sys.argv[1], "rb").read()
base = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0
u8 = lambda o: d[o]
u16 = lambda o: struct.unpack_from(">H", d, o)[0]
u32 = lambda o: struct.unpack_from(">I", d, o)[0]
u64 = lambda o: struct.unpack_from(">Q", d, o)[0]

print(f"CellSpurs @0x{base:08X}: nSpus={u8(0x76)} flags1=0x{u8(0x74):02X} spuIdling=0x{u8(0x73):02X} "
      f"sysSrvMessage=0x{u8(0x72):02X} wklSignal1=0x{u16(0x70):04X} wklSignal2=0x{u16(0x78):04X} "
      f"wklFlag=0x{u32(0x6C):08X} wklFlagReceiver=0x{u8(0x77):02X}")
print(f"  wklEnabled=0x{u32(0xB0):08X} wklMskB=0x{u32(0xB4):08X} sysSrvMsgUpdateWorkload=0x{u8(0xBE):02X} "
      f"sysSrvOnSpu=0x{u8(0xC9):02X} spuPort=0x{u8(0xCA):02X}")
print(f"  spuTG=0x{u32(0xD30):X} eventQueue=0x{u32(0xD5C):X} eventPort=0x{u32(0xD60):X} flags=0x{u32(0xD80):X} "
      f"revision={u32(0xDA0)} sdkVersion=0x{u32(0xDA4):X} prefix={d[0xD8C:0xD8C + u8(0xD9B)]!r}")
print(f"\n wid  state         status event ready1 ready2/idle cur pend min max  policy_module_ea   arg                uniqueId prio")
for w in range(16):
    st = u8(0x80 + w)
    addr, arg, uid = u64(0xB00 + 0x20 * w), u64(0xB08 + 0x20 * w), u32(0xB10 + 0x20 * w)
    prio = d[0xB18 + 0x20 * w:0xB20 + 0x20 * w].hex()
    if st == 0 and addr == 0 and u8(0x40 + w) == 0:
        continue
    print(f" {w:3d}  {STATE.get(st, st):13} {u8(0x90 + w):6d} {u8(0xA0 + w):5d} {u8(0x00 + w):6d} {u8(0x10 + w):11d} "
          f"{u8(0x20 + w):3d} {u8(0x30 + w):4d} {u8(0x40 + w):3d} {u8(0x50 + w):3d}  0x{addr:016X} 0x{arg:016X} {uid:8d} {prio}")
print(f"\n sys service: policy_module_ea=0x{u64(0xD00):016X} arg=0x{u64(0xD08):016X}")
