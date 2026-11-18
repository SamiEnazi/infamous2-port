"""A plain (decrypted) ISO 9660 disc image of a game folder, for testing Setup's "pick an .iso" (v0.6.0). Python's
standard library only. The image has the ISO 9660 names (upper case, as on a PS3 disc) and a Joliet tree with the
real names, both path tables, and every file once.

    python -I tools/make_iso.py <game folder: the one holding PS3_GAME> <out.iso> [--label INFAMOUS2]

It never changes the folder. The result is the player's own game: never share it.
"""
import argparse
import os
import struct
import sys
import time

S = 2048


def both16(v):
    return struct.pack("<H", v) + struct.pack(">H", v)


def both32(v):
    return struct.pack("<I", v) + struct.pack(">I", v)


def dt7(t):
    g = time.gmtime(t)
    return bytes([g.tm_year - 1900, g.tm_mon, g.tm_mday, g.tm_hour, g.tm_min, g.tm_sec, 0])


def dt17(t):
    return time.strftime("%Y%m%d%H%M%S", time.gmtime(t)).encode() + b"00" + b"\0"


def iso_name(name, is_dir):
    ok = "".join(c if (c.isascii() and (c.isalnum() or c in "_.")) else "_" for c in name.upper())
    if is_dir:
        return ok.replace(".", "_").encode("ascii")
    if "." not in ok:
        ok += "."
    return (ok + ";1").encode("ascii")


def joliet_name(name, is_dir):
    return (name if is_dir else name + ";1").encode("utf-16-be")


class Node:
    def __init__(self, path, name, parent):
        self.path, self.name, self.parent = path, name, parent
        self.dirs, self.files = [], []
        self.lba = {0: 0, 1: 0}
        self.size = {0: 0, 1: 0}
        self.num = 0


def scan(top):
    root = Node(top, "", None)
    stack = [root]
    while stack:
        n = stack.pop()
        with os.scandir(n.path) as it:
            ents = sorted(it, key=lambda e: e.name.upper())
        for e in ents:
            if e.is_symlink():
                continue
            if e.is_dir():
                c = Node(e.path, e.name, n)
                n.dirs.append(c)
                stack.append(c)
            elif e.is_file():
                st = e.stat()
                n.files.append({"path": e.path, "name": e.name, "size": st.st_size, "mtime": st.st_mtime, "lba": 0})
    return root


def record(name, lba, size, is_dir, t):
    nl = len(name)
    ln = 33 + nl + (1 if nl % 2 == 0 else 0)
    return (bytes([ln, 0]) + both32(lba) + both32(size) + dt7(t) + bytes([2 if is_dir else 0, 0, 0]) + both16(1) +
            bytes([nl]) + name + (b"\0" if nl % 2 == 0 else b""))


def dir_records(n, tree, t):
    """the directory's records, laid out in sectors (a record never crosses one)"""
    nm = joliet_name if tree else iso_name
    recs = [record(b"\0", n.lba[tree], n.size[tree], True, t),
            record(b"\1", (n.parent or n).lba[tree], (n.parent or n).size[tree], True, t)]
    items = [(c.name, True, c) for c in n.dirs] + [(f["name"], False, f) for f in n.files]
    items.sort(key=lambda x: nm(x[0], x[1]))
    for name, is_dir, x in items:
        if is_dir:
            recs.append(record(nm(name, True), x.lba[tree], x.size[tree], True, t))
        else:
            recs.append(record(nm(name, False), x["lba"], x["size"], False, x["mtime"]))
    out = bytearray()
    for r in recs:
        if len(out) % S + len(r) > S:
            out += b"\0" * (S - len(out) % S)
        out += r
    if len(out) % S:
        out += b"\0" * (S - len(out) % S)
    return bytes(out)


def all_dirs(root):
    """breadth first, numbered for the path tables (1 = root)"""
    order, q = [], [root]
    while q:
        n = q.pop(0)
        order.append(n)
        q.extend(sorted(n.dirs, key=lambda c: c.name.upper()))
    for i, n in enumerate(order):
        n.num = i + 1
    return order


def path_table(order, tree, big):
    nm = joliet_name if tree else iso_name
    out = bytearray()
    for n in order:
        name = b"\0" if n.parent is None else nm(n.name, True)
        out += bytes([len(name), 0])
        out += struct.pack(">I" if big else "<I", n.lba[tree])
        out += struct.pack(">H" if big else "<H", (n.parent or n).num)
        out += name + (b"\0" if len(name) % 2 else b"")
    return bytes(out)


def vd(kind, label, vol_sectors, pt_size, pt_l, pt_m, root_rec, t, joliet):
    d = bytearray(S)
    d[0] = kind
    d[1:6] = b"CD001"
    d[6] = 1
    if joliet:
        d[8:40] = "PS3".ljust(16).encode("utf-16-be")
        d[40:72] = label[:16].ljust(16).encode("utf-16-be")
        d[88:91] = b"%/E"
    else:
        d[8:40] = b"PS3".ljust(32)
        d[40:72] = label.upper()[:32].ljust(32).encode("ascii")
    d[80:88] = both32(vol_sectors)
    d[120:124] = both16(1)
    d[124:128] = both16(1)
    d[128:132] = both16(S)
    d[132:140] = both32(pt_size)
    d[140:144] = struct.pack("<I", pt_l)
    d[148:152] = struct.pack(">I", pt_m)
    d[156:190] = root_rec
    for off in (190, 318, 446, 574):          # volume set, publisher, preparer, application
        d[off:off + 128] = (b"\0 " * 64) if joliet else b" " * 128
    for off in (702, 739, 776):
        d[off:off + 37] = (b"\0 " * 18 + b"\0") if joliet else b" " * 37
    d[813:830] = dt17(t)
    d[830:847] = dt17(t)
    d[847:864] = b"0" * 16 + b"\0"
    d[864:881] = b"0" * 16 + b"\0"
    d[881] = 1
    return bytes(d)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("folder")
    ap.add_argument("out")
    ap.add_argument("--label", default="INFAMOUS2")
    a = ap.parse_args()
    if not os.path.isfile(os.path.join(a.folder, "PS3_GAME", "PARAM.SFO")):
        sys.exit("no PS3_GAME/PARAM.SFO in " + a.folder)
    t = time.time()
    root = scan(a.folder)
    order = all_dirs(root)
    # sizes first (a directory's size depends only on its own entries' names)
    for tree in (0, 1):
        for n in order:
            n.size[tree] = len(dir_records(n, tree, t))
    pts = [len(path_table(order, tr, False)) for tr in (0, 1)]
    ptsec = [(p + S - 1) // S for p in pts]
    lba = 19                                  # 16 system sectors, PVD, Joliet SVD, terminator
    pt_at = {}
    for tr in (0, 1):
        pt_at[tr] = (lba, lba + ptsec[tr])
        lba += 2 * ptsec[tr]
    for tree in (0, 1):
        for n in order:
            n.lba[tree] = lba
            lba += n.size[tree] // S
    for n in order:
        for f in n.files:
            f["lba"] = lba
            lba += (f["size"] + S - 1) // S
    total = lba
    tmp = a.out + ".part"
    done, t0, last = 0, time.time(), 0
    allbytes = sum(f["size"] for n in order for f in n.files)
    with open(tmp, "wb") as o:
        o.write(b"\0" * (16 * S))
        o.write(vd(1, a.label, total, pts[0], pt_at[0][0], pt_at[0][1], record(b"\0", root.lba[0], root.size[0], True, t), t, False))
        o.write(vd(2, a.label, total, pts[1], pt_at[1][0], pt_at[1][1], record(b"\0", root.lba[1], root.size[1], True, t), t, True))
        term = bytearray(S)
        term[0] = 255
        term[1:6] = b"CD001"
        term[6] = 1
        o.write(term)
        for tr in (0, 1):
            for big in (False, True):
                p = path_table(order, tr, big)
                o.write(p + b"\0" * (ptsec[tr] * S - len(p)))
        for tree in (0, 1):
            for n in order:
                assert o.tell() == n.lba[tree] * S
                o.write(dir_records(n, tree, t))
        for n in order:
            for f in n.files:
                assert o.tell() == f["lba"] * S
                with open(f["path"], "rb") as i:
                    while True:
                        b = i.read(8 << 20)
                        if not b:
                            break
                        o.write(b)
                        done += len(b)
                        if time.time() - last > 5:
                            last = time.time()
                            print(f"  {100 * done // max(1, allbytes)}% ({done >> 20} of {allbytes >> 20} MB)", flush=True)
                pad = (-f["size"]) % S
                if pad:
                    o.write(b"\0" * pad)
    os.replace(tmp, a.out)
    print(f"{a.out}: {total} sectors ({total * S / (1 << 30):.2f} GB), {sum(len(n.files) for n in order)} files, "
          f"{len(order)} folders, in {time.time() - t0:.0f} s")


if __name__ == "__main__":
    main()
