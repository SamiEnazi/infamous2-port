# RPCS3-style PPU executable hash: sha1 over each phdr's (p_type,p_flags) BE,
# and for LOAD segments with memsz: p_vaddr, p_memsz (BE u64) + file bytes.
import sys, struct, hashlib
for path in sys.argv[1:]:
    b = open(path, 'rb').read()
    assert b[:4] == b'\x7fELF' and b[4] == 2 and b[5] == 2, "not ELF64 BE"
    e_phoff, = struct.unpack_from('>Q', b, 0x20)
    e_phentsize, e_phnum = struct.unpack_from('>HH', b, 0x36)
    sha = hashlib.sha1()
    segs = []
    for i in range(e_phnum):
        o = e_phoff + i * e_phentsize
        p_type, p_flags, p_off, p_vaddr, p_paddr, p_filesz, p_memsz, p_align = struct.unpack_from('>IIQQQQQQ', b, o)
        sha.update(struct.pack('>II', p_type, p_flags))
        if p_type == 1 and p_memsz:
            sha.update(struct.pack('>QQ', p_vaddr, p_memsz))
            sha.update(b[p_off:p_off + p_filesz])
        segs.append((hex(p_type), hex(p_flags), hex(p_vaddr), hex(p_filesz), hex(p_memsz)))
    print(path)
    print('  PPU-' + sha.hexdigest())
    for s in segs: print('   ', s)
