"""Small stdlib-only image tool for frame A/B work (no PIL/numpy on this box).

    python -I tools/imgtool.py stats  <img>                 mean / max per channel, non-black share
    python -I tools/imgtool.py boost  <img> <out.png> [gain] contrast-stretch (auto: max -> 255) or x gain
    python -I tools/imgtool.py diff   <a> <b> <out.png>      per-pixel |a-b| x8, plus summary numbers
    python -I tools/imgtool.py crop   <img> <out.png> x y w h
    python -I tools/imgtool.py pixel  <img> x y              RGB at a point

Reads PNG (8-bit RGB/RGBA, non-interlaced, all five filters) and binary PPM (P6).
Images are handled as (w, h, bytearray RGB).
"""
import struct
import sys
import zlib


def read_png(p):
    d = open(p, "rb").read()
    assert d[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG"
    i, idat, w = 8, b"", 0
    while i < len(d):
        n = struct.unpack(">I", d[i:i + 4])[0]
        t, c = d[i + 4:i + 8], d[i + 8:i + 8 + n]
        if t == b"IHDR":
            w, h, bd, ct, _cm, _fm, il = struct.unpack(">IIBBBBB", c)
            assert bd == 8 and ct in (2, 6) and il == 0, (bd, ct, il)
        elif t == b"IDAT":
            idat += c
        i += 12 + n
    bpp = 3 if ct == 2 else 4
    raw = zlib.decompress(idat)
    stride = w * bpp
    out = bytearray(h * stride)
    prev = bytearray(stride)
    pos = 0
    for y in range(h):
        f = raw[pos]; pos += 1
        line = bytearray(raw[pos:pos + stride]); pos += stride
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if f == 1: line[x] = (line[x] + a) & 255
            elif f == 2: line[x] = (line[x] + b) & 255
            elif f == 3: line[x] = (line[x] + ((a + b) >> 1)) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[x] = (line[x] + pr) & 255
        out[y * stride:(y + 1) * stride] = line
        prev = line
    if bpp == 4:
        rgb = bytearray(w * h * 3)
        rgb[0::3], rgb[1::3], rgb[2::3] = out[0::4], out[1::4], out[2::4]
        return w, h, rgb
    return w, h, out


def read_ppm(p):
    d = open(p, "rb").read()
    parts, i = [], 0
    while len(parts) < 4:
        while d[i:i + 1].isspace(): i += 1
        if d[i:i + 1] == b"#":
            while d[i:i + 1] not in (b"\n", b""): i += 1
            continue
        j = i
        while not d[j:j + 1].isspace(): j += 1
        parts.append(d[i:j]); i = j
    w, h = int(parts[1]), int(parts[2])
    return w, h, bytearray(d[i + 1:i + 1 + w * h * 3])


def read(p):
    return read_ppm(p) if p.lower().endswith(".ppm") else read_png(p)


def write_png(p, w, h, rgb):
    raw = b"".join(b"\x00" + bytes(rgb[y * w * 3:(y + 1) * w * 3]) for y in range(h))
    def chunk(t, b): return struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b) & 0xFFFFFFFF)
    open(p, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                        + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def stats(rgb):
    n = len(rgb) // 3
    m = [sum(rgb[k::3]) / n for k in range(3)]
    mx = [max(rgb[k::3]) for k in range(3)]
    nz = sum(1 for k in range(0, len(rgb), 3) if rgb[k] > 8 or rgb[k + 1] > 8 or rgb[k + 2] > 8) / n
    return m, mx, nz


def main():
    cmd = sys.argv[1]
    if cmd == "stats":
        w, h, rgb = read(sys.argv[2])
        m, mx, nz = stats(rgb)
        print(f"{w}x{h} mean R{m[0]:.1f} G{m[1]:.1f} B{m[2]:.1f}  max R{mx[0]} G{mx[1]} B{mx[2]}  >8: {nz*100:.1f}%")
    elif cmd == "boost":
        w, h, rgb = read(sys.argv[2])
        if len(sys.argv) > 4:
            g = float(sys.argv[4])
        else:
            top = sorted(rgb)[int(len(rgb) * 0.999)] or 1
            g = 255.0 / top
        write_png(sys.argv[3], w, h, bytearray(min(255, int(v * g)) for v in rgb))
        print(f"gain x{g:.1f} -> {sys.argv[3]}")
    elif cmd == "diff":
        wa, ha, a = read(sys.argv[2]); wb, hb, b = read(sys.argv[3])
        assert (wa, ha) == (wb, hb), "size differs"
        d = bytearray(min(255, abs(x - y) * 8) for x, y in zip(a, b))
        write_png(sys.argv[4], wa, ha, d)
        raw = [abs(x - y) for x, y in zip(a, b)]
        big = sum(1 for k in range(0, len(raw), 3) if max(raw[k:k + 3]) > 24) / (len(raw) // 3)
        print(f"mean |diff| {sum(raw)/len(raw):.2f}  max {max(raw)}  pixels >24: {big*100:.2f}%")
    elif cmd == "crop":
        w, h, rgb = read(sys.argv[2])
        x, y, cw, ch = (int(v) for v in sys.argv[4:8])
        out = bytearray()
        for yy in range(y, y + ch):
            out += rgb[(yy * w + x) * 3:(yy * w + x + cw) * 3]
        write_png(sys.argv[3], cw, ch, out)
    elif cmd == "pixel":
        w, h, rgb = read(sys.argv[2])
        x, y = int(sys.argv[3]), int(sys.argv[4])
        print(tuple(rgb[(y * w + x) * 3:(y * w + x) * 3 + 3]))


if __name__ == "__main__":
    main()
