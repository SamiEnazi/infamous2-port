"""Convert binary PPM (P6) frames to PNG (stdlib only).   python -I tools/ppm2png.py a.ppm [b.ppm ...]"""
import struct, sys, zlib

def read_ppm(p):
    d = open(p, "rb").read()
    parts, i = [], 0
    while len(parts) < 4:                       # magic, w, h, maxval (skip comments)
        while d[i:i + 1].isspace(): i += 1
        if d[i:i + 1] == b"#":
            while d[i:i + 1] not in (b"\n", b""): i += 1
            continue
        j = i
        while not d[j:j + 1].isspace(): j += 1
        parts.append(d[i:j]); i = j
    assert parts[0] == b"P6", parts[0]
    w, h = int(parts[1]), int(parts[2])
    return w, h, d[i + 1:i + 1 + w * h * 3]

def write_png(p, w, h, rgb):
    raw = b"".join(b"\x00" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    def chunk(t, b): return struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b) & 0xFFFFFFFF)
    open(p, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                        + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))

for p in sys.argv[1:]:
    w, h, rgb = read_ppm(p)
    nz = sum(1 for k in range(0, len(rgb), 3 * 97) if rgb[k] | rgb[k + 1] | rgb[k + 2])
    write_png(p[:-4] + ".png", w, h, rgb)
    print(f"{p} -> png  {w}x{h}  non-black samples {nz}/{len(rgb) // (3 * 97)}")
