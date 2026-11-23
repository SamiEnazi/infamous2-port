"""Helpers for the Linux build environment (tools/buildenv.sh), run with setup's own Python (tools-cache/python):

    python -I tools/buildenv.py deb <folder> <package.deb>...
        unpacks Debian / Ubuntu packages into <folder> (a sysroot), without dpkg or ar: only the files a build
        reads (usr/include, lib*, usr/lib*; no documentation, no locale or gconv data); a link with an absolute
        target is made relative, so it points inside <folder> and never at this system's own files
    python -I tools/buildenv.py untar <archive.tar.xz> <folder> <pattern>...
        unpacks the members matching a pattern (fnmatch: "LLVM-.../bin/clang*"), for a system without xz
    python -I tools/buildenv.py elf <program or library> [--max-glibc 2.35] [--not-needed libstdc++.so.6,...]
        prints what an ELF file needs at run time ("needs: <libraries>" and "glibc: <the newest version it asks
        for>"); with the options, exit 1 and a "why:" line when it needs a newer glibc or one of those libraries

Only Python's standard library (3.14 reads the zstd packages Ubuntu makes; xz and gzip ones work everywhere).
"""
import fnmatch
import io
import os
import re
import struct
import sys
import tarfile

KEEP = ("usr/include/", "usr/lib/", "usr/lib64/", "lib/", "lib64/", "lib32/")
SKIP = ("usr/lib/x86_64-linux-gnu/gconv/", "usr/lib/locale/", "usr/lib/x86_64-linux-gnu/audit/")
# never used by the port's build: GCC's sanitizer / OpenMP / TM / quad-float libraries, and the fully static C library
# (left out, so nothing can link the C library statically by mistake: the game uses the system's own)
SKIP_RE = re.compile(r"(^|/)(lib(asan|tsan|lsan|ubsan|hwasan|gomp|itm|quadmath)[._]|lib(c|m-[0-9.]+|mvec)\.a$)")


def ar_members(data):
    """(name, bytes) of a Unix ar archive (a .deb is one)."""
    if data[:8] != b"!<arch>\n":
        raise ValueError("not a .deb (no ar header)")
    i = 8
    while i + 60 <= len(data):
        hdr = data[i:i + 60]
        name = hdr[:16].decode("ascii", "replace").strip().rstrip("/")
        size = int(hdr[48:58].decode("ascii").strip())
        yield name, data[i + 60:i + 60 + size]
        i += 60 + size + (size & 1)


def open_payload(name, blob):
    if name.endswith(".zst"):
        try:
            from compression import zstd
        except ImportError:
            raise SystemExit("why: this Python cannot read zstd (it needs 3.14): " + name)
        return io.BytesIO(zstd.decompress(blob))
    if name.endswith(".xz"):
        import lzma
        return io.BytesIO(lzma.decompress(blob))
    if name.endswith(".gz"):
        import gzip
        return io.BytesIO(gzip.decompress(blob))
    if name.endswith(".tar"):
        return io.BytesIO(blob)
    raise SystemExit("why: unknown package payload " + name)


def unpack_deb(dest, deb):
    data = open(deb, "rb").read()
    payload = None
    for name, blob in ar_members(data):
        if name.startswith("data.tar"):
            payload = open_payload(name, blob)
    if payload is None:
        raise SystemExit("why: no data.tar in " + deb)
    n = 0
    destr = os.path.realpath(dest)
    with tarfile.open(fileobj=payload, mode="r:") as t:
        for m in t:
            rel = m.name[2:] if m.name.startswith("./") else m.name.lstrip("/")
            if not rel or ".." in rel.split("/"):
                continue
            if not rel.startswith(KEEP) or rel.startswith(SKIP) or SKIP_RE.search(rel):
                continue
            out = os.path.join(destr, rel)
            if not (os.path.realpath(os.path.dirname(out)) + os.sep).startswith(destr + os.sep):
                continue   # (a folder that is a link pointing out of the sysroot: never written through)
            if m.isdir():
                os.makedirs(out, exist_ok=True)
            elif m.issym():
                target = m.linkname
                if target.startswith("/"):   # absolute: inside the sysroot, relative to the link's folder
                    target = os.path.relpath(os.path.join(destr, target.lstrip("/")), os.path.dirname(out))
                os.makedirs(os.path.dirname(out), exist_ok=True)
                if os.path.lexists(out):
                    os.remove(out)
                os.symlink(target, out)
                n += 1
            elif m.isfile():
                os.makedirs(os.path.dirname(out), exist_ok=True)
                if os.path.lexists(out):
                    os.remove(out)
                with t.extractfile(m) as src, open(out, "wb") as dst:
                    dst.write(src.read())
                os.chmod(out, 0o755 if m.mode & 0o111 else 0o644)
                n += 1
            elif m.islnk():
                src = os.path.join(destr, (m.linkname[2:] if m.linkname.startswith("./") else m.linkname.lstrip("/")))
                if os.path.isfile(src):
                    os.makedirs(os.path.dirname(out), exist_ok=True)
                    if os.path.lexists(out):
                        os.remove(out)
                    with open(src, "rb") as s, open(out, "wb") as d:
                        d.write(s.read())
                    n += 1
    return n


def elf_needs(path):
    """(needed libraries, versions asked for: {library: [version names]})"""
    b = open(path, "rb").read()
    if b[:4] != b"\x7fELF" or b[4] != 2 or b[5] != 1:
        raise SystemExit("why: not a 64-bit little-endian ELF file: " + path)
    shoff, = struct.unpack_from("<Q", b, 0x28)
    shentsize, shnum = struct.unpack_from("<HH", b, 0x3A)
    secs = [struct.unpack_from("<IIQQQQIIQQ", b, shoff + i * shentsize) for i in range(shnum)]
    # (name, type, flags, addr, offset, size, link, info, align, entsize)

    def cstr(off):
        return b[off:b.index(b"\0", off)].decode("utf-8", "replace")

    needed, vers = [], {}
    for s in secs:
        if s[1] == 6:   # SHT_DYNAMIC
            strtab = secs[s[6]][4]
            for j in range(0, s[5], 16):
                tag, val = struct.unpack_from("<qQ", b, s[4] + j)
                if tag == 0:
                    break
                if tag == 1:   # DT_NEEDED
                    needed.append(cstr(strtab + val))
        elif s[1] == 0x6FFFFFFE:   # SHT_GNU_verneed
            strtab = secs[s[6]][4]
            off, count = s[4], s[7]
            for _ in range(count):
                _, cnt, vfile, aux, nxt = struct.unpack_from("<HHIII", b, off)
                lib = cstr(strtab + vfile)
                a = off + aux
                for _ in range(cnt):
                    _, _, _, vname, anext = struct.unpack_from("<IHHII", b, a)
                    vers.setdefault(lib, []).append(cstr(strtab + vname))
                    if not anext:
                        break
                    a += anext
                if not nxt:
                    break
                off += nxt
    return needed, vers


def vkey(v):
    return tuple(int(x) for x in v.split(".") if x.isdigit())


def main():
    if len(sys.argv) >= 4 and sys.argv[1] == "deb":
        dest = sys.argv[2]
        os.makedirs(dest, exist_ok=True)
        for deb in sys.argv[3:]:
            n = unpack_deb(dest, deb)
            print(f"  {os.path.basename(deb)}: {n} files")
        return 0
    if len(sys.argv) >= 5 and sys.argv[1] == "untar":
        arc, dest, pats = sys.argv[2], sys.argv[3], sys.argv[4:]
        kw = {"filter": "data"} if hasattr(tarfile, "data_filter") else {}
        n = 0
        with tarfile.open(arc, mode="r|*") as t:
            for m in t:
                if any(fnmatch.fnmatchcase(m.name, p) for p in pats):
                    t.extract(m, dest, **kw)
                    n += 1
        print(f"  {os.path.basename(arc)}: {n} files")
        return 0 if n else 1
    if len(sys.argv) >= 3 and sys.argv[1] == "elf":
        path, args = sys.argv[2], sys.argv[3:]
        if not os.path.isfile(path):
            print("why: no such file: " + path)
            return 1
        maxg = args[args.index("--max-glibc") + 1] if "--max-glibc" in args else ""
        notn = args[args.index("--not-needed") + 1].split(",") if "--not-needed" in args else []
        needed, vers = elf_needs(path)
        g = sorted({v[6:] for vl in vers.values() for v in vl if v.startswith("GLIBC_") and v[6:7].isdigit()}, key=vkey)
        newest = g[-1] if g else ""
        print("needs: " + (" ".join(needed) or "nothing"))
        print("glibc: " + (newest or "-"))
        bad = [n for n in needed if n in notn]
        if bad:
            print("why: it needs " + ", ".join(bad))
            return 1
        if maxg and newest and vkey(newest) > vkey(maxg):
            print(f"why: it asks for glibc {newest} (newer than {maxg})")
            return 1
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
