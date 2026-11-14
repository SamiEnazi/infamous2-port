#!/usr/bin/env bash
# tools/detect.sh - the bash side of tools/detect.ps1: what kind of game copy a path is, its
# PARAM.SFO title.
#
# Source it (it only defines functions):
#     . tools/detect.sh
#     get_game_info "/path/to/game" || exit 1
#     echo "$GAME_FORMAT $GAME_TITLE_ID $GAME_EBOOT"
#
# Or run it to see what it finds (JSON):
#     tools/detect.sh "/path/to/game"
#
# Needs python3 for the binary parsers (the Linux Setup needs it anyway).

# One parser for both sides: mode "sfo" | "info" | "key" | "json".
_ps3_detect() {
    if ! command -v python3 >/dev/null 2>&1; then
        echo "detect.sh: python3 is needed to read PARAM.SFO and disc images" >&2
        return 127
    fi
    python3 - "$@" <<'PY'
import json
import os
import re
import shlex
import struct
import sys

SECTOR = 2048


class DetectError(Exception):
    pass


def decode_string(raw):
    """PARAM.SFO strings are UTF-8 or UTF-16-LE (a BOM, or zero high bytes)."""
    if not raw:
        return ""
    if raw.startswith(b"\xff\xfe"):
        return raw[2:].decode("utf-16-le", "replace").rstrip("\0")
    if raw.startswith(b"\xfe\xff"):
        return raw[2:].decode("utf-16-be", "replace").rstrip("\0")
    if len(raw) >= 4 and len(raw) % 2 == 0:
        odd = raw[1::2]
        if odd.count(0) >= (len(odd) * 3 + 4) // 5:   # 60 % or more of the high bytes are zero
            return raw.decode("utf-16-le", "replace").rstrip("\0")
    return raw.rstrip(b"\0").decode("utf-8", "replace").rstrip("\0")


def read_sfo_bytes(b):
    """PARAM.SFO -> dict (0x0404 = uint32, 0x0204/0x0004 = string)."""
    if len(b) < 20 or b[:4] != b"\0PSF":
        raise DetectError("not a PARAM.SFO")
    keys, data, count = struct.unpack_from("<III", b, 8)
    out = {}
    for i in range(count):
        e = 20 + 16 * i
        if e + 16 > len(b):
            break
        ko, fmt, ln, mx, off = struct.unpack_from("<HHIII", b, e)
        end = b.find(b"\0", keys + ko)
        if end < 0:
            continue
        name = b[keys + ko:end].decode("ascii", "replace")
        v = data + off
        if v > len(b):
            continue
        if fmt == 0x0404:
            if v + 4 <= len(b):
                out[name] = struct.unpack_from("<I", b, v)[0]
        else:
            ln = min(ln, len(b) - v)
            out[name] = decode_string(b[v:v + ln])
    return out


def read_sfo(path):
    with open(path, "rb") as f:
        return read_sfo_bytes(f.read())


def read_at(f, off, n):
    f.seek(off)
    return f.read(n)


def iso_find(f, directory_record, name):
    lba = struct.unpack_from("<I", directory_record, 2)[0]
    size = struct.unpack_from("<I", directory_record, 10)[0]
    if size <= 0 or size > (16 << 20):
        return None
    data = read_at(f, lba * SECTOR, size)
    pos = 0
    while pos + 34 <= len(data):
        ln = data[pos]
        if ln == 0:
            pos = (pos // SECTOR + 1) * SECTOR     # records never cross a sector
            continue
        if pos + ln > len(data):
            break
        nl = data[pos + 32]
        if nl > 0 and data[pos + 33] not in (0, 1) and pos + 33 + nl <= len(data):
            nm = data[pos + 33:pos + 33 + nl].decode("ascii", "replace").split(";")[0]
            if nm.upper() == name.upper():
                return data[pos:pos + ln]
        pos += ln
    return None


def iso_param_sfo(path):
    """PARAM.SFO from a PS3 .iso (ISO 9660). UDF-only images give None."""
    try:
        with open(path, "rb") as f:
            pvd = read_at(f, 16 * SECTOR, SECTOR)
            if len(pvd) < 190 or pvd[0] != 1 or pvd[1:6] != b"CD001":
                return None
            root = pvd[156:190]
            ps3 = iso_find(f, root, "PS3_GAME")
            if not ps3:
                return None
            sfo = iso_find(f, ps3, "PARAM.SFO")
            if not sfo:
                return None
            lba = struct.unpack_from("<I", sfo, 2)[0]
            size = struct.unpack_from("<I", sfo, 10)[0]
            if size <= 0 or size > (1 << 20):
                return None
            return read_sfo_bytes(read_at(f, lba * SECTOR, size))
    except (OSError, struct.error, DetectError):
        return None


def pkg_title_id(path):
    """A PS3 PKG carries its content id (UP0001-BCUS98125_00-...) at 0x30."""
    try:
        with open(path, "rb") as f:
            h = f.read(0xA0)
    except OSError:
        return None
    if len(h) < 4 or h[:4] != b"\x7fPKG":
        return None
    if len(h) >= 0x60:
        m = re.search(rb"[A-Z]{4}[0-9]{5}", h[0x30:0x60])
        if m:
            return m.group(0).decode()
    m = re.search(rb"[A-Z]{4}[0-9]{5}", h)
    return m.group(0).decode() if m else None


def text(v):
    if v is None:
        return None
    s = str(v)
    return s if s else None


def detect(path):
    path = os.path.abspath(path)
    if not os.path.exists(path):
        raise DetectError("no such file or folder: %s" % path)

    if os.path.isdir(path):
        if os.path.isfile(os.path.join(path, "PS3_GAME", "PARAM.SFO")):
            game = os.path.join(path, "PS3_GAME")
        elif os.path.basename(path) == "PS3_GAME" and os.path.isfile(os.path.join(path, "PARAM.SFO")):
            game = path
        else:
            raise DetectError("not a PS3 game folder: %s has no PS3_GAME/PARAM.SFO (choose the "
                              "folder that holds PS3_GAME, or PS3_GAME itself)" % path)
        title_id = text(read_sfo(os.path.join(game, "PARAM.SFO")).get("TITLE_ID"))
        return {"format": "folder", "title_id": title_id, "serial": title_id,
                "eboot": os.path.join(game, "USRDIR", "EBOOT.BIN")}

    ext = os.path.splitext(path)[1].lower()
    if ext == ".iso":
        sfo = iso_param_sfo(path)
        title_id = text(sfo.get("TITLE_ID")) if sfo else None
        # One file: this is the EBOOT's path inside the image. Extract the entry first
        # (7z x game.iso PS3_GAME/USRDIR/EBOOT.BIN) and decrypt that.
        return {"format": "iso", "title_id": title_id, "serial": title_id,
                "eboot": os.path.join(path, "PS3_GAME", "USRDIR", "EBOOT.BIN")}

    if ext == ".pkg":
        title_id = pkg_title_id(path)
        return {"format": "pkg", "title_id": title_id, "serial": title_id, "eboot": None}

    raise DetectError("not a game: expected an .iso, a .pkg, or a folder that holds PS3_GAME "
                      "(got %s)" % path)


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else ""
    if mode == "sfo":
        if len(sys.argv) != 4:
            raise DetectError("usage: sfo <PARAM.SFO> <KEY>")
        value = read_sfo(sys.argv[2]).get(sys.argv[3])
        if value is None:
            sys.exit(1)
        print(value)
    elif mode == "info":
        if len(sys.argv) != 3:
            raise DetectError("usage: info <path>")
        g = detect(sys.argv[2])
        print("GAME_FORMAT=" + shlex.quote(g["format"]))
        print("GAME_TITLE_ID=" + shlex.quote(g["title_id"] or ""))
        print("GAME_SERIAL=" + shlex.quote(g["serial"] or ""))
        print("GAME_EBOOT=" + shlex.quote(g["eboot"] or ""))
    elif mode == "json":
        if len(sys.argv) < 3:
            raise DetectError("usage: json <path>")
        g = detect(sys.argv[2])
        print(json.dumps({
            "Format": g["format"],
            "TitleId": g["title_id"],
            "Serial": g["serial"],
            "EbootPath": g["eboot"],
        }, indent=2))
    else:
        raise DetectError("unknown mode %r" % mode)


try:
    main()
except DetectError as e:
    sys.stderr.write("detect.sh: %s\n" % e)
    sys.exit(1)
PY
}

# param_sfo_value <PARAM.SFO> <KEY>: prints the value (an integer or a string).
param_sfo_value() {
    _ps3_detect sfo "${1:-}" "${2:-}"
}

# get_game_info <path>: sets GAME_FORMAT, GAME_TITLE_ID, GAME_SERIAL and GAME_EBOOT.
get_game_info() {
    if [ -z "${1:-}" ]; then
        echo "get_game_info: no path given" >&2
        return 2
    fi
    local out
    out=$(_ps3_detect info "$1") || return 1
    eval "$out"
}

# Direct run: JSON on stdout, errors on stderr and a non-zero exit.
if [ "${BASH_SOURCE[0]:-}" = "$0" ]; then
    if [ $# -lt 1 ] || [ -z "$1" ]; then
        echo "usage: $0 <game folder | game.iso | game.pkg>" >&2
        exit 2
    fi
    _ps3_detect json "$1"
fi
