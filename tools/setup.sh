#!/usr/bin/env bash
# Linux Setup (tools/setup_build.ps1's counterpart): from your own copy of inFAMOUS 2 to a native
# Linux build, in this folder only (nothing is installed system-wide, no root needed).
#   tools/setup.sh --disc <folder holding PS3_GAME> [--rpcs3 <RPCS3's folder>] [--rpcs3-bin <rpcs3 AppImage>]
#                  [--no-rpcs3] [--eboot <decrypted EBOOT.elf>] [--jobs N] [--no-desktop]
# RPCS3 is not needed: the game's update (1.01 + 1.03 + 1.04, 236 MB) and the PS3 system software (197 MB, of
# which three firmware modules are kept) come from Sony's own servers (tools/sony_files.py), unpacked and
# decrypted with tools/ps3tool.py (downloaded from the port's GitHub when a zip came without it).
#   --rpcs3      optional: RPCS3's data folder (holding dev_hdd0/ and dev_flash/). When it has the 1.04
#                update and the firmware, they are copied from there instead (nothing to download).
#                Default: ~/.config/rpcs3 (the AppImage, EmuDeck), else the Flatpak's
#                ~/.var/app/net.rpcs3.RPCS3/config/rpcs3, when it holds dev_hdd0/
#   --rpcs3-bin  optional: the RPCS3 AppImage (or rpcs3 program), used to decrypt only when ps3tool cannot.
#                Default: rpcs3 on PATH, an rpcs3*.AppImage in ~/Applications (EmuDeck), else the Flatpak.
#   --no-rpcs3   do not look for RPCS3 at all
#   --eboot      an already decrypted 1.04 EBOOT.elf instead
#   --jobs       compile jobs (default: half the CPU threads, at most 8: each lifted file is large)
# Steps:
#   1. check this PC and the game                 6. Vulkan headers, SDL2 headers, glslang -> deps/
#   2. the build tools -> tools-cache/            7. translate and compile the game (15-60 min)
#      (LLVM 22.1.8, and CMake / Ninja             -> work/port/build/Infamous2Recomp
#      when the system has none)                   8. a desktop launcher (~/.local/share/applications)
#      and the Python tools -> work/venv
#   3. your game linked into game/, its update and the firmware files into game/ (RPCS3's or Sony's)
#   4. the 1.04 EBOOT and the firmware modules decrypted -> eboot/EBOOT_upd104.elf, work/fw
#   5. (the Python tools: made in step 2)
# Needs (distribution packages): a C/C++ toolchain's headers and libraries (Debian/Ubuntu:
# build-essential; Fedora: gcc-c++; Arch: base-devel), python3 with venv, curl, tar, xz, unzip, the
# Vulkan loader (libvulkan1 / vulkan-loader) with your GPU's driver, and SDL2 (libsdl2-2.0-0 / SDL2).
# SteamOS (Steam Deck) has no C library headers and nothing can be installed there: step 2 downloads them into
# tools-cache/sysroot-fix (tools/libc_headers.sh; GitHub #81), so stock SteamOS 3.5 and later needs nothing extra.
# Play with tools/play.sh (or the desktop launcher).
set -eu   # (no pipefail: 'ldconfig -p | grep -q' would fail on grep's early exit)
root="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
disc=""; rpcs3=""; rpcs3bin=""; eboot=""; desktop=1; norpcs3=0
jobs=$(( $(nproc) / 2 )); [ $jobs -gt 8 ] && jobs=8; [ $jobs -lt 1 ] && jobs=1
while [ $# -gt 0 ]; do
    case "$1" in
        --disc) disc="$2"; shift ;;
        --rpcs3) rpcs3="$2"; shift ;;
        --rpcs3-bin) rpcs3bin="$2"; shift ;;
        --eboot) eboot="$2"; shift ;;
        --jobs) jobs="$2"; shift ;;
        --no-desktop) desktop=0 ;;
        --no-rpcs3) norpcs3=1 ;;
        -h|--help) sed -n '2,27p' "$0"; exit 0 ;;
        *) echo "unknown option $1 (--help)"; exit 2 ;;
    esac
    shift
done
if [ $norpcs3 = 1 ]; then rpcs3=""; rpcs3bin=""
elif [ -z "$rpcs3" ]; then
    rpcs3="${XDG_CONFIG_HOME:-$HOME/.config}/rpcs3"
    fp="$HOME/.var/app/net.rpcs3.RPCS3/config/rpcs3"
    [ ! -d "$rpcs3/dev_hdd0" ] && [ ! -f "$rpcs3/config/vfs.yml" ] && [ -d "$fp" ] && rpcs3="$fp"
    [ -d "$rpcs3/dev_hdd0" ] || [ -f "$rpcs3/config/vfs.yml" ] || rpcs3=""      # (RPCS3 is optional)
fi
# RPCS3's data folders to look in (GitHub #46, Bazzite: Flatpak RPCS3 and a dev_hdd0 moved by vfs.yml): the
# one given/chosen above, then the AppImage/package folder, then the Flatpak's.
rpcs3_dirs=()
[ -n "$rpcs3" ] && rpcs3_dirs=("$rpcs3")
if [ -n "$rpcs3" ]; then
    for d in "${XDG_CONFIG_HOME:-$HOME/.config}/rpcs3" "$HOME/.var/app/net.rpcs3.RPCS3/config/rpcs3" "$HOME/.local/share/rpcs3"; do
        if [ -d "$d" ] && [ "$d" != "$rpcs3" ]; then rpcs3_dirs+=("$d"); fi
    done
fi
# vfs_dir <dev_hdd0|dev_flash> <a path inside it>: RPCS3's config/vfs.yml of each data folder may say where it
# is ("/dev_hdd0/: $(EmulatorDir)dev_hdd0/", or a path on another disk); else <data folder>/<name>. The first
# place that holds the probe path, else the first place.
vfs_dir() {
    local name="$1" probe="$2" d v first="" emu='$(EmulatorDir)'
    for d in "${rpcs3_dirs[@]}"; do
        v=""
        if [ -f "$d/config/vfs.yml" ]; then
            v=$(sed -nE "s#^[[:space:]]*[\"']?/?${name}/?[\"']?[[:space:]]*:[[:space:]]*(.*[^[:space:]])?[[:space:]]*\$#\1#p" "$d/config/vfs.yml" | head -1 | tr -d "\r\"'")
            case "$v" in
                "$emu"*) v="${d%/}/${v:${#emu}}" ;;
                ""|/*) ;;
                *) v="${d%/}/$v" ;;
            esac
        fi
        for c in ${v:+"$v"} "$d/$name"; do
            [ -z "$first" ] && first="$c"
            [ -e "$c/$probe" ] && { printf '%s' "${c%/}"; return 0; }
        done
    done
    printf '%s' "${first%/}"
}
t0=$(date +%s)
step() { echo "== [$1/8] $2   ($(( ($(date +%s) - t0) / 60 )) min so far)"; }
# Something that moves while a long, silent command runs: a line every 5 seconds until tick_stop
tick_pid=""
tick_start() { ( s=0; while sleep 5; do s=$((s+5)); echo "  progress: $1, $((s/60)):$(printf '%02d' $((s%60))) so far"; done ) & tick_pid=$!; }
tick_stop() { if [ -n "$tick_pid" ]; then kill "$tick_pid" 2>/dev/null || true; tick_pid=""; fi; return 0; }
fail() { tick_stop; echo "== FAILED: $*"; exit 1; }
# Folder names (GitHub #34): spaces, "&", "(", brackets, other alphabets build fine here (every path is quoted, and
# CMake / Ninja take UTF-8), but CMake reads ";" as a list separator, and the desktop launcher can't hold a few others
case "$root$disc" in
    *';'*|*'"'*|*'\'*|*'`'*|*'$'*|*$'\n'*)
        fail "a folder name with one of these characters: ; \" \\ \` \$ (or a line break) can't be built: rename the folder ($root) or your game folder, then run Setup again" ;;
esac
cache="$root/tools-cache"; deps="$root/deps"; dl="$cache/dl"
mkdir -p "$cache" "$deps" "$dl"
. "$root/tools/libc_headers.sh"   # lh_*: the C library's headers where the system has none (SteamOS), the compiler self-test

# download <url> <file> <sha256 or ""> <what>: retried once, checked when a SHA-256 is given
download() {
    local url="$1" out="$2" sha="$3" what="$4"
    if [ -f "$out" ] && { [ -z "$sha" ] || [ "$(sha256sum "$out" | cut -c1-64)" = "$sha" ]; }; then return 0; fi
    for try in 1 2; do
        echo "  downloading $what"
        if curl -fL $([ -t 1 ] && echo -# || echo -sS) --retry 2 --connect-timeout 30 -A "inFAMOUS2-port-setup" -o "$out.part" "$url"; then
            if [ -n "$sha" ] && [ "$(sha256sum "$out.part" | cut -c1-64)" != "$sha" ]; then
                rm -f "$out.part"; echo "  $what: wrong SHA-256"
            else
                mv -f "$out.part" "$out"; return 0
            fi
        fi
        [ $try = 1 ] && { echo "  $what: trying once more"; sleep 3; }
    done
    fail "downloading $what failed ($url)"
}
sfo() {   # sfo <PARAM.SFO> <KEY>
    python3 - "$1" "$2" <<'EOF'
import struct, sys
b = open(sys.argv[1], "rb").read()
assert b[:4] == b"\0PSF", "not a PARAM.SFO"
keys, data, n = struct.unpack_from("<III", b, 8)
for i in range(n):
    ko, fmt, ln, mx, off = struct.unpack_from("<HHIII", b, 20 + 16 * i)
    k = b[keys + ko:b.index(b"\0", keys + ko)].decode()
    if k == sys.argv[2]:
        v = b[data + off:data + off + ln]
        print(struct.unpack("<I", v[:4])[0] if fmt == 0x0404 else v.rstrip(b"\0").decode("utf-8", "replace"))
EOF
}

# ------------------------------------------------------------------------------------------------
step 1 "checking"
miss=0
ldlist() { ldconfig -p 2>/dev/null || /sbin/ldconfig -p 2>/dev/null || true; }
haslib() { ldlist | grep "$1 " >/dev/null; }
ok() { printf '  %-5s %s: %s\n' "$1" "$2" "$3"; [ "$1" = MISS ] && miss=1; return 0; }
[ "$(uname -m)" = x86_64 ] && ok ok "64-bit x86 Linux" "$(uname -sr)" || ok MISS "64-bit x86 Linux" "$(uname -m): the port is built for x86-64 CPUs"
fl=$(grep -m1 '^flags' /proc/cpuinfo || true)
v3=1; for f in avx2 fma bmi2 movbe f16c; do echo "$fl" | grep -qw $f || v3=0; done
[ $v3 = 1 ] && ok ok "CPU with AVX2 (x86-64-v3)" yes || ok MISS "CPU with AVX2 (x86-64-v3)" "the port is built for AVX2 CPUs (Intel 4th gen / AMD Ryzen or newer)"
ram=$(awk '/MemTotal/{printf "%d", $2/1048576 + 0.5}' /proc/meminfo)
[ "$ram" -ge 16 ] && ok ok Memory "$ram GB" || ok note Memory "$ram GB (16 GB or more recommended: the build is large; try --jobs 2)"
free=$(df -P -BG "$root" | awk 'NR==2{gsub("G","",$4); print $4}')
[ "$free" -ge 30 ] && ok ok "Free disk space" "$free GB" || ok note "Free disk space" "$free GB (the build needs about 20 GB while it runs, 6 GB after)"
haslib libvulkan.so.1 && ok ok "Vulkan loader" found || ok MISS "Vulkan loader" "install it (Debian/Ubuntu: libvulkan1 mesa-vulkan-drivers; Fedora: vulkan-loader mesa-vulkan-drivers)"
haslib libSDL2-2.0.so.0 && ok ok "SDL2" found || ok MISS "SDL2" "install it (Debian/Ubuntu: libsdl2-2.0-0; Fedora/Arch: SDL2 / sdl2)"
py=$(command -v python3 || true)
if [ -n "$py" ] && "$py" -c 'import sys, venv, ensurepip; sys.exit(sys.version_info < (3, 10))' 2>/dev/null; then ok ok Python "$("$py" -V)"
else ok MISS Python "python3 3.10+ with venv (Debian/Ubuntu: python3-venv)"; fi
gccdir=""
for d in $(ls -d /usr/lib/gcc/x86_64-*linux-gnu/* /usr/lib/gcc/x86_64-*linux/* 2>/dev/null | sort -V -r); do
    v=$(basename "$d"); for inc in /usr/include/c++/"$v" /usr/include/c++/"${v%%.*}"; do
        [ -f "$inc/vector" ] || continue
        # libstdc++ sits next to GCC's crt files on Debian/Ubuntu, in the distro's lib dir on others (Fedora's
        # /usr/lib64, NixOS FHS environments): clang's --gcc-install-dir finds it in either place
        for lib in "$d" /usr/lib64 /usr/lib /usr/lib/x86_64-linux-gnu; do
            [ -f "$lib/libstdc++.so" -o -f "$lib/libstdc++.a" ] && { gccdir="$d"; break 3; }
        done; done
done
[ -n "$gccdir" ] && ok ok "C++ headers and libraries" "$gccdir (tried for real with the compiler in step 2)" || ok MISS "C++ headers and libraries" "install them (Debian/Ubuntu: build-essential; Fedora: gcc-c++; Arch: base-devel)"
# The C library's own headers (GitHub #81): SteamOS has the C++ ones above but no <features.h>, <stdio.h>,
# <pthread.h>, <linux/...>, and nothing can be installed there. For the glibc releases SteamOS uses, step 2 gets
# them into tools-cache/sysroot-fix (tools/libc_headers.sh); any other system is told here, not at step 6.
hmiss=$(lh_missing); hrow=""; hfix=0; hadvice=0
if [ -n "$hmiss" ] && [ -n "$gccdir" ]; then
    hlist="<${hmiss// />, <}>"; hrow=$(lh_pick "$(lh_glibc)")
    if [ -n "$hrow" ]; then
        hfix=1; ok note "C library headers" "not on this system ($hlist; SteamOS comes without them): downloaded into tools-cache/sysroot-fix in step 2 (12 MB; nothing is installed on the system)"
    else
        hadvice=1; ok MISS "C library headers" "not on this system ($hlist), and there is no set to download for its C library (glibc $(lh_glibc | grep . || echo unknown)): see \"What to do\" below"
    fi
fi
for t in curl tar xz unzip; do command -v $t >/dev/null && ok ok $t found || ok MISS $t "install it"; done
title=""
if [ -z "$disc" ]; then ok MISS "Game disc folder" "--disc <the folder that holds PS3_GAME>"
elif [ ! -f "$disc/PS3_GAME/PARAM.SFO" ]; then ok MISS "Game disc folder" "no PS3_GAME/PARAM.SFO in $disc"
else
    disc="$(readlink -f "$disc")"; title=$(sfo "$disc/PS3_GAME/PARAM.SFO" TITLE_ID)
    case "$title" in
        BCES01143|BCES01229|BCUS98125|NPUA80638|NPEA00318) ok ok "Game" "$(sfo "$disc/PS3_GAME/PARAM.SFO" TITLE) [$title] (tested)" ;;
        BCES01144) ok note "Game" "$(sfo "$disc/PS3_GAME/PARAM.SFO" TITLE) [$title] (please report how it goes)" ;;
        *) ok MISS "Game" "[$title] is not inFAMOUS 2" ;;
    esac
fi
# The update and the firmware files: the port's own copies in game/ (an earlier setup), RPCS3's when it has them
# (copied), else Sony's servers (tools/sony_files.py). RPCS3 is optional; only the digital version needs it.
upd_ok() {   # upd_ok <folder>: the 1.04 update (the digital version: the game itself, 01.03)
    local v=""; [ -f "$1/PARAM.SFO" ] && [ -f "$1/USRDIR/EBOOT.BIN" ] && v=$(sfo "$1/PARAM.SFO" APP_VER)
    [ "$v" = "01.04" ] || { [ "${title:0:2}" = NP ] && [ "$v" = "01.03" ]; }
}
fw_ok() { [ -f "$1/sys/external/liblv2.sprx" ] && [ -f "$1/sys/external/libsysmodule.sprx" ] && [ -f "$1/sys/external/libsre.sprx" ]; }
hdd0=""; flash=""
if [ -n "$rpcs3" ]; then hdd0=$(vfs_dir dev_hdd0 "game/$title/PARAM.SFO"); flash=$(vfs_dir dev_flash sys/external/liblv2.sprx); fi
upd="${hdd0:+$hdd0/game/$title}"
[ -n "$rpcs3" ] && ok ok RPCS3 "$rpcs3 (optional: its update and firmware files are copied when it has them)" \
                || ok note RPCS3 "not used (not needed: the game's update and the PS3 firmware files come from Sony's servers)"
if [ -n "$title" ]; then
    if upd_ok "$root/game/dev_hdd0/game/$title"; then ok ok "Game update" "in game/dev_hdd0/game/$title"
    elif [ -n "$upd" ] && upd_ok "$upd"; then ok ok "Game update" "copied from RPCS3 ($upd)"
    elif [ "${title:0:2}" = NP ]; then ok MISS "Digital game in RPCS3" "the digital (PSN) version needs RPCS3 with the game and its license installed (File > Install Packages/Raps/Edats); give --rpcs3 <RPCS3's folder holding dev_hdd0>"
    else ok note "Game update" "downloaded from Sony's servers during the setup: the updates 1.01, 1.03 and 1.04 (236 MB)"; fi
fi
if fw_ok "$root/game/dev_flash"; then ok ok "PS3 firmware files" "in game/dev_flash"
elif [ -n "$flash" ] && fw_ok "$flash"; then ok ok "PS3 firmware files" "copied from RPCS3 ($flash)"
else ok note "PS3 firmware files" "downloaded from Sony's servers during the setup: the system software 4.93 (197 MB), of which 3 modules are kept"; fi
# DLC (GitHub #48, #56; optional): the game's unlock files, <dev_hdd0>/game/<ID>/USRDIR/dlc/ulf/unlock_*.edat. RPCS3's
# are copied into the port's folder in step 3 (on a first setup and on every later run), the dlc folder's too.
dlc_files() { [ -d "$1" ] && find "$1" -type f -iname '*.edat' 2>/dev/null | sort; return 0; }
dlc_count() { dlc_files "$1" | wc -l | tr -d ' '; }
dlc_copy() {   # dlc_copy <from> <to> [flat]: missing or different files only, lower-case names; says how many
    local f rel dst n=0
    while IFS= read -r f; do
        rel="${f#"$1"/}"; [ "${3:-}" = flat ] && rel="$(basename "$f")"
        dst="$2/${rel,,}"
        if ! cmp -s "$f" "$dst"; then
            mkdir -p "$(dirname "$dst")" && rm -f "$dst" "$dst.dec" && cp "$f" "$dst" && chmod u+rw "$dst" && n=$((n + 1))
        fi
    done < <(dlc_files "$1")
    printf '%s' "$n"
}
dlc_need_license() {   # how many of a folder's unlock files are bound to a license (.rap): header byte 11 is 1 or 2
    local f n=0
    while IFS= read -r f; do
        if [ "$(head -c 3 "$f" 2>/dev/null)" = NPD ]; then
            case "$(od -An -tu1 -j11 -N1 "$f" 2>/dev/null | tr -d ' ')" in 1|2) n=$((n + 1)) ;; esac
        fi
    done < <(dlc_files "$1")
    printf '%s' "$n"
}
nfiles() { [ "$1" = 1 ] && printf '1 file' || printf '%s files' "$1"; }
rpdlc=""
if [ -n "$rpcs3" ] && [ -n "$title" ]; then rpdlc="$(vfs_dir dev_hdd0 "game/$title/USRDIR/dlc")/game/$title/USRDIR/dlc"; fi
if [ -n "$title" ]; then
    d_rp=0; [ -n "$rpdlc" ] && d_rp=$(dlc_count "$rpdlc")
    d_drop=$(dlc_count "$root/dlc"); d_own=$(dlc_count "$root/game/dev_hdd0/game/$title/USRDIR/dlc")
    d=""
    [ "$d_rp" -gt 0 ] && d="$(nfiles "$d_rp") found in RPCS3 (copied into the port's folder during the setup)"
    [ "$d_drop" -gt 0 ] && d="${d:+$d; }$(nfiles "$d_drop") in the dlc folder"
    [ "$d_own" -gt 0 ] && d="${d:+$d; }$(nfiles "$d_own") in the port's folder already"
    if [ -n "$d" ]; then ok ok DLC "$d"
    else ok note DLC "none (optional: install your DLC in RPCS3 and run setup again, or put its unlock files, *.edat, in the port's dlc folder)"; fi
fi
# RPCS3's program decrypts only when ps3tool cannot (step 4)
if [ $norpcs3 = 0 ]; then
    [ -z "$rpcs3bin" ] && rpcs3bin=$(command -v rpcs3 || true)
    [ -z "$rpcs3bin" ] && rpcs3bin=$(ls -1 "$HOME"/Applications/rpcs3*.AppImage "$HOME"/Applications/RPCS3*.AppImage 2>/dev/null | head -1 || true)
    if [ -z "$rpcs3bin" ] && command -v flatpak >/dev/null && flatpak info net.rpcs3.RPCS3 >/dev/null 2>&1; then
        rpcs3bin="$cache/rpcs3-flatpak.sh"   # one program path for rpcs3_decrypt
        printf '#!/bin/sh\nexec flatpak run --filesystem=%s net.rpcs3.RPCS3 "$@"\n' "$root" > "$rpcs3bin"; chmod +x "$rpcs3bin"
    fi
fi
[ -f "$root/tools/ps3tool.py" ] && ok note ps3tool "tools/ps3tool.py (unpacks and decrypts your game's files)" \
                                || ok note ps3tool "downloaded from GitHub during the setup (tools/ps3tool.py: unpacks and decrypts your game's files)"
for sub in engine/CMakeLists.txt base/infamous2-lle/CMakeLists.txt; do   # (one repository since v0.5.0)
    [ -f "$root/$sub" ] && ok ok "${sub%%/*}" present || ok MISS "${sub%%/*}" "missing: download or git clone the port again"
done
if [ $miss != 0 ]; then
    [ $hadvice = 1 ] && lh_advice "$root"
    fail "fix the MISS items first"
fi

# ------------------------------------------------------------------------------------------------
step 2 "the build tools (into tools-cache/, nothing is installed) and the Python tools"
LLVM_TOP=LLVM-22.1.8-Linux-X64
if [ ! -f "$cache/llvm/.ready" ]; then
    download "https://github.com/llvm/llvm-project/releases/download/llvmorg-22.1.8/$LLVM_TOP.tar.xz" "$dl/$LLVM_TOP.tar.xz" \
        df0e1ecf16caf3489a272a5eea4eec9b0d82878f6477fa309504f918a0006384 "LLVM 22.1.8 (1.9 GB)"
    echo "  unpacking LLVM (the compiler, linker and profile tool only; a few minutes)"
    rm -rf "$cache/llvm" "$cache/$LLVM_TOP"
    tar -xJf "$dl/$LLVM_TOP.tar.xz" -C "$cache" --wildcards "$LLVM_TOP/bin/clang*" "$LLVM_TOP/bin/lld" "$LLVM_TOP/bin/ld.lld" \
        "$LLVM_TOP/bin/llvm-profdata" "$LLVM_TOP/bin/llvm-ar" "$LLVM_TOP/bin/llvm-ranlib" "$LLVM_TOP/bin/llvm-symbolizer" "$LLVM_TOP/lib/clang/*"
    mv "$cache/$LLVM_TOP" "$cache/llvm"
    touch "$cache/llvm/.ready"
fi
# clang picks the newest GCC folder; a system with a newer GCC runtime than its C++ headers (Ubuntu
# 22.04 + gcc-12-base) then finds no <vector>: name the folder that has them.
# Where the system has no C library headers (SteamOS, step 1): the downloaded ones, searched after every system
# folder (so the C++ headers' #include_next finds them, and a system that has its own keeps using its own).
# <CFGDIR> is the .cfg file's folder: the port's folder can be moved, and its name may hold spaces.
if [ $hfix = 1 ]; then
    lh_install "$cache" "$dl" "$hrow" || { lh_advice "$root"; fail "the C library headers this system lacks could not be downloaded (above). Check your internet connection and run setup again, or use the Distrobox way above"; }
fi
for d in clang clang++; do
    { echo "--gcc-install-dir=$gccdir"; if [ $hfix = 1 ]; then echo '-idirafter <CFGDIR>/../../sysroot-fix/include'; fi; } > "$cache/llvm/bin/$d.cfg"
done
# The LLVM release's linker was built against libxml2.so.2 (used only to merge Windows manifests, never here).
# Ubuntu 25.10 and later ship only libxml2.so.16, so the linker would not start (GitHub #24): it gets a small
# stand-in libxml2.so.2 in its own lib folder (found through its RUNPATH, $ORIGIN/../lib), made here once,
# linked by the system's GNU linker or, without one, by lld itself started against the system's newer libxml2.
if ! "$cache/llvm/bin/ld.lld" --version >/dev/null 2>&1 && "$cache/llvm/bin/ld.lld" --version 2>&1 | grep -q "libxml2\.so\.2"; then
    xs="$cache/xml2stub"; rm -rf "$xs"; mkdir -p "$xs"
    cat > "$xs/stub.c" <<'XEOF'
/* Stand-in for libxml2.so.2: lld names these for COFF manifest merging only. Each one fails softly. */
static void nofree(void* p) { (void)p; }
void (*xmlFree)(void*) = nofree;
void* xmlReadMemory(void) { return 0; }
void* xmlDocGetRootElement(void) { return 0; }
void* xmlNewDoc(void) { return 0; }
void* xmlDocSetRootElement(void) { return 0; }
void xmlDocDumpFormatMemoryEnc(void) {}
void xmlSetGenericErrorFunc(void) {}
void xmlUnlinkNode(void) {}
void xmlFreeNode(void) {}
void* xmlAddChild(void) { return 0; }
void* xmlNewProp(void) { return 0; }
void* xmlNewNs(void) { return 0; }
void* xmlStrdup(void) { return 0; }
void* xmlCopyNamespace(void) { return 0; }
void xmlFreeNs(void) {}
void xmlFreeDoc(void) {}
XEOF
    cat > "$xs/stub.map" <<'XEOF'
LIBXML2_2.4.30 { global: xmlFree; xmlDocGetRootElement; xmlNewDoc; xmlDocSetRootElement; xmlDocDumpFormatMemoryEnc;
    xmlSetGenericErrorFunc; xmlUnlinkNode; xmlFreeNode; xmlAddChild; xmlNewProp; xmlNewNs; xmlStrdup;
    xmlCopyNamespace; xmlFreeNs; xmlFreeDoc; local: *; };
LIBXML2_2.6.0 { global: xmlReadMemory; } LIBXML2_2.4.30;
XEOF
    "$cache/llvm/bin/clang" -c -fPIC -O1 "$xs/stub.c" -o "$xs/stub.o"
    lnk=(-shared -soname libxml2.so.2 --version-script "$xs/stub.map" "$xs/stub.o" -o "$xs/libxml2.so.2")
    if command -v ld >/dev/null 2>&1 && ld "${lnk[@]}" > "$xs/log" 2>&1; then :
    else
        sys=$(ls /usr/lib/x86_64-linux-gnu/libxml2.so.[1-9]* /usr/lib64/libxml2.so.[1-9]* /usr/lib/libxml2.so.[1-9]* 2>/dev/null | head -n 1 || true)
        if [ -n "$sys" ]; then
            ln -sf "$sys" "$cache/llvm/lib/libxml2.so.2"
            "$cache/llvm/bin/ld.lld" "${lnk[@]}" >> "$xs/log" 2>&1 || true
            rm -f "$cache/llvm/lib/libxml2.so.2"
        fi
    fi
    [ -f "$xs/libxml2.so.2" ] && cp -f "$xs/libxml2.so.2" "$cache/llvm/lib/libxml2.so.2"
    "$cache/llvm/bin/ld.lld" --version >/dev/null 2>&1 || fail "LLVM's linker cannot start: it needs libxml2.so.2. Install the GNU linker (sudo apt install binutils) or libxml2 (sudo apt install libxml2-16 on Ubuntu 25.10 and later), then run setup.sh again"
    echo "  LLVM's linker: made a stand-in libxml2.so.2 (this system has a newer libxml2)"
fi
"$cache/llvm/bin/clang" --version | sed -n '1s/^/  /p'
# The compiler tried for real (GitHub #81: the check in step 1 only looks for folders): a small C and a small C++
# program with the engine's headers and libraries, the build's flags and linker, built and started.
if why=$(lh_selftest "$cache/llvm/bin/clang" "$cache/llvm/bin/clang++" "$cache/selftest"); then
    echo "  compiler check: a C and a C++ program compile, link and run"
else
    echo "  compiler check: $why"
    echo "  (the compiler's own words: $cache/selftest/log.txt)"
    lh_advice "$root"
    fail "this system cannot build C/C++ programs yet: $why"
fi
export PORT_LLVM="$cache/llvm"
cmv=$(cmake --version 2>/dev/null | head -1 | awk '{print $3}' || true)
if [ -n "$cmv" ] && printf '3.24\n%s\n' "$cmv" | sort -V -C; then export PORT_CMAKE="$(command -v cmake)"
else
    if [ ! -f "$cache/cmake/.ready" ]; then
        download https://github.com/Kitware/CMake/releases/download/v3.31.3/cmake-3.31.3-linux-x86_64.tar.gz "$dl/cmake.tar.gz" \
            442b93b3db0f6c6501a74add587acca5ea6b8c1526ccca7bc8d89fc592036971 "CMake 3.31.3"
        rm -rf "$cache/cmake" && mkdir -p "$cache/cmake" && tar -xzf "$dl/cmake.tar.gz" -C "$cache/cmake" --strip-components 1
        touch "$cache/cmake/.ready"
    fi
    export PORT_CMAKE="$cache/cmake/bin/cmake"
fi
if command -v ninja >/dev/null; then export PORT_NINJA="$(command -v ninja)"
else
    if [ ! -f "$cache/ninja/.ready" ]; then
        download https://github.com/ninja-build/ninja/releases/download/v1.11.1/ninja-linux.zip "$dl/ninja-linux.zip" \
            b901ba96e486dce377f9a070ed4ef3f79deb45f4ffe2938f8e7ddc69cfb3df77 "Ninja 1.11.1"
        rm -rf "$cache/ninja" && mkdir -p "$cache/ninja" && unzip -q "$dl/ninja-linux.zip" -d "$cache/ninja" && touch "$cache/ninja/.ready"
    fi
    export PORT_NINJA="$cache/ninja/ninja"
fi
echo "  cmake $PORT_CMAKE, ninja $PORT_NINJA"
# work/venv: the build's Python packages, and pycryptodome for tools/ps3tool.py (steps 3 and 4: much faster with it)
venv="$root/work/venv"
[ -x "$venv/bin/python" ] && "$venv/bin/python" -c "import sys" 2>/dev/null || { rm -rf "$venv"; python3 -m venv "$venv"; }
"$venv/bin/python" -m pip install --disable-pip-version-check -q -r "$root/engine/requirements.txt" || fail "pip install failed"
echo "  Python tools: work/venv"

# ------------------------------------------------------------------------------------------------
step 3 "your game: the disc folder linked; the game's update and the PS3 firmware files (from RPCS3 when it has them, else from Sony's servers)"
game="$root/game"
mkdir -p "$game/dev_hdd0/game" "$game/gamedata"
# (v0.6.0) the game's files are copied INTO the port's folder, so the folder can be moved or copied anywhere and
# still plays; an older setup's link (game/disc -> your folder) is replaced by the copy. Your folder is not changed.
if [ "$(readlink -f "$disc")" = "$(readlink -f "$game/disc" 2>/dev/null)" ] && [ ! -L "$game/disc" ]; then
    echo "  your game: in game/disc"
else
    need=$(du -sb "$disc" 2>/dev/null | cut -f1); need=${need:-0}
    have=$(df -P -B1 "$root" | awk 'NR==2{print $4}')
    [ "$have" -gt $(( need + 3221225472 )) ] || fail "not enough free space to copy your game into the port's folder: it needs $(( need / 1073741824 + 1 )) GB and about 3 GB more"
    [ -L "$game/disc" ] && rm -f "$game/disc"
    echo "  copying your game into the port's folder ($(( need / 1073741824 )) GB): from then on the port needs nothing outside its folder"
    mkdir -p "$game/disc"
    cp -a --no-preserve=ownership "$disc/." "$game/disc/" || fail "copying your game into game/disc failed"
    echo "  your game: game/disc (a copy of $disc)"
fi
copy_tree() {   # copy_tree <src> <dst> <what>
    echo "  copying $3"; mkdir -p "$2"; cp -a --no-preserve=ownership "$1/." "$2/"
}
sony=""
if upd_ok "$game/dev_hdd0/game/$title"; then echo "  the 1.04 update: already in game/dev_hdd0/game/$title"
elif [ -n "$upd" ] && upd_ok "$upd"; then copy_tree "$upd" "$game/dev_hdd0/game/$title" "the 1.04 update from RPCS3 (0.2 GB)"
else sony="update"; fi
if fw_ok "$game/dev_flash"; then echo "  the PS3 firmware files: already in game/dev_flash"
elif [ -n "$flash" ] && fw_ok "$flash"; then copy_tree "$flash" "$game/dev_flash" "the PS3 firmware files from RPCS3 (0.2 GB)"
else if [ -n "$sony" ]; then sony=all; else sony=firmware; fi; fi
# tools/sony_files.py: Sony's files when needed, and tools/ps3tool.py in any case (a zip comes without it: from
# GitHub); ps3tool also decrypts in step 4
if [ -n "$sony" ]; then [ -n "$rpcs3" ] && echo "  from Sony's servers: $sony (RPCS3 does not have it)" || echo "  from Sony's servers: $sony (RPCS3 is not used)"; fi
if ! "$venv/bin/python" -I "$root/tools/sony_files.py" "${sony:-ps3tool}" --root "$root" --disc "$disc"; then
    [ -n "$sony" ] && fail "getting the game's files from Sony's servers failed (the reason is above). Check your internet connection and run setup again: it goes on where it stopped. (Only if Sony's servers can't be reached from your network at all: an RPCS3 that has the 1.04 update and firmware 4.93 installed works too, give --rpcs3 <its folder>.)"
    echo "  note: no ps3tool: step 4 decrypts with RPCS3 instead"
fi
# DLC: RPCS3's unlock files and the dlc folder's, copied in (wherever the update came from; nothing is ever deleted)
dlcdst="$game/dev_hdd0/game/$title/USRDIR/dlc"; n_rp=0
if [ -n "$rpdlc" ] && [ -d "$rpdlc" ]; then n_rp=$(dlc_copy "$rpdlc" "$dlcdst"); fi
mkdir -p "$root/dlc"
[ -f "$root/dlc/README.txt" ] || cp "$root/tools/dlc_readme.txt" "$root/dlc/README.txt" 2>/dev/null || true
n_drop=$(dlc_copy "$root/dlc" "$dlcdst/ulf" flat)
n_dlc=$(dlc_count "$dlcdst")
if [ "$n_dlc" -gt 0 ]; then
    from=""
    [ "$n_rp" -gt 0 ] && from="$n_rp copied from RPCS3 ($rpdlc)"
    [ "$n_drop" -gt 0 ] && from="${from:+$from, }$n_drop copied from the dlc folder"
    echo "  DLC: $n_dlc unlock file(s) in game/dev_hdd0/game/$title/USRDIR/dlc${from:+ ($from)}"
    n_lic=$(dlc_need_license "$dlcdst")
    [ "$n_lic" -gt 0 ] && echo "  note: $n_lic of the DLC files need their license (.rap): not supported yet (the game will not show that DLC)"
else
    echo "  DLC: none found (optional: install it in RPCS3 and run setup again, or put its unlock files, *.edat, in the port's dlc folder)"
fi
inst="${hdd0:+$hdd0/game/${title}_INSTALL}"
if [ -n "$inst" ] && [ -d "$inst" ] && [ ! -d "$game/dev_hdd0/game/${title}_INSTALL" ]; then copy_tree "$inst" "$game/dev_hdd0/game/${title}_INSTALL" "the game's HDD install from RPCS3 (2.4 GB)"
elif [ ! -d "$game/dev_hdd0/game/${title}_INSTALL" ]; then echo "  the game's HDD install (2.4 GB): the game makes it itself at its first start"; fi

# ------------------------------------------------------------------------------------------------
# RPCS3 decrypts one file per start, without a window (Qt offscreen). An AppImage is unpacked once and its
# AppRun started (no FUSE needed: libfuse2 is missing on many current systems; and APPIMAGE_EXTRACT_AND_RUN
# races with the previous start's clean-up when started again at once).
# First "--headless" (no Qt screen plugin at all). Newer RPCS3 AppImages ship no "offscreen" Qt plugin (only
# wayland, wayland-egl, xcb), so when RPCS3 says it has no such plugin the next way is tried: the desktop's
# Wayland, its X11, then a hidden X server (xvfb-run).
# A missing system library is named with the Ubuntu/Debian package that has it (retrying cannot help there).
rpcs3_libpkg() {
    case "$1" in
        libOpenGL.so.0) echo libopengl0 ;; libEGL.so.1) echo libegl1 ;; libGLX.so.0) echo libglx0 ;;
        libGL.so.1) echo libgl1 ;; libxcb-cursor.so.0) echo libxcb-cursor0 ;; libxkbcommon.so.0) echo libxkbcommon0 ;;
        libxkbcommon-x11.so.0) echo libxkbcommon-x11-0 ;; libfuse.so.2) echo libfuse2 ;; libSDL2-2.0.so.0) echo libsdl2-2.0-0 ;;
        libasound.so.2) echo libasound2 ;; libpulse.so.0) echo libpulse0 ;; libfontconfig.so.1) echo libfontconfig1 ;;
        libwayland-client.so.0) echo libwayland-client0 ;; libwayland-egl.so.1) echo libwayland-egl1 ;;
        *) echo "" ;;
    esac
}
rpcs3_decrypt() {
    local f="$1" out="${1%.*}" log="$root/work/logs/rpcs3_decrypt.log" one="$root/work/logs/rpcs3_decrypt.last"
    local plats="headless offscreen" p try lib pkg
    case "$rpcs3bin" in
        *.AppImage|*.appimage)
            if [ ! -x "$cache/rpcs3-appimage/squashfs-root/AppRun" ]; then
                rm -rf "$cache/rpcs3-appimage" && mkdir -p "$cache/rpcs3-appimage"
                (cd "$cache/rpcs3-appimage" && "$rpcs3bin" --appimage-extract >/dev/null 2>&1) || true
            fi
            [ -x "$cache/rpcs3-appimage/squashfs-root/AppRun" ] && rpcs3bin="$cache/rpcs3-appimage/squashfs-root/AppRun" ;;
    esac
    mkdir -p "$root/work/logs"
    [ -n "${WAYLAND_DISPLAY:-}" ] && plats="$plats wayland"
    [ -n "${DISPLAY:-}" ] && plats="$plats xcb"
    command -v xvfb-run >/dev/null 2>&1 && plats="$plats xvfb"
    for p in $plats; do
        for try in 1 2 3; do
            echo "== $(date) $p try $try: $rpcs3bin --decrypt $f" >> "$log"
            case "$p" in
                headless) timeout 300 "$rpcs3bin" --headless --decrypt "$f" > "$one" 2>&1 || true ;;
                xvfb) QT_QPA_PLATFORM=xcb timeout 300 xvfb-run -a "$rpcs3bin" --decrypt "$f" > "$one" 2>&1 || true ;;
                *) QT_QPA_PLATFORM=$p timeout 300 "$rpcs3bin" --decrypt "$f" > "$one" 2>&1 || true ;;
            esac
            cat "$one" >> "$log"
            if [ -f "$out.elf" ] || [ -f "$out.prx" ]; then return 0; fi
            lib=$(sed -n 's/.*error while loading shared libraries: \([^:]*\):.*/\1/p' "$one" | head -1)
            if [ -n "$lib" ]; then
                pkg=$(rpcs3_libpkg "$lib")
                echo "  RPCS3 needs the system library $lib, and this system does not have it."
                [ -n "$pkg" ] && echo "  On Ubuntu/Debian (also inside Distrobox): sudo apt install $pkg   then run setup.sh again."
                return 0
            fi
            [ "$p" = headless ] && break   # (one try: RPCS3 builds without --headless, or it failed: go on)
            grep -qiE "platform plugin|connect to display|open display" "$one" && break   # not this way here: try the next
            sleep 2
        done
    done
    if grep -qi "xcb-cursor" "$one" 2>/dev/null; then
        echo "  RPCS3's X11 mode needs libxcb-cursor0. On Ubuntu/Debian: sudo apt install libxcb-cursor0   then run setup.sh again."
    elif grep -qiE "platform plugin|connect to display|open display" "$one" 2>/dev/null; then
        echo "  RPCS3 could not start without a screen. Run setup.sh from the desktop (not over SSH), or install xvfb (sudo apt install xvfb) and run it again."
    fi
    return 0
}
# ps3tool_decrypt <in> <out>: tools/ps3tool.py as a program (Setup's venv Python: pycryptodome), an ELF out
ps3tool_decrypt() {
    [ -f "$root/tools/ps3tool.py" ] || return 1
    rm -f "$2"
    "$venv/bin/python" -I "$root/tools/ps3tool.py" decrypt "$1" "$2" > "$root/work/logs/ps3tool_decrypt.last" 2>&1 || { cat "$root/work/logs/ps3tool_decrypt.last" | tail -n 5 | sed 's/^/    ps3tool: /'; return 1; }
    [ "$(head -c 4 "$2" 2>/dev/null | od -An -tx1 | tr -d ' \n')" = "7f454c46" ]
}
step 4 "the game's 1.04 EBOOT and the firmware modules decrypted (tools/ps3tool.py, else RPCS3)"
mkdir -p "$root/eboot" "$root/work/logs"
elf="$root/eboot/EBOOT_upd104.elf"
if [ -n "$eboot" ]; then cp -f "$eboot" "$elf"
elif [ ! -f "$elf" ]; then
    bin="$root/eboot/EBOOT_upd104.BIN"
    cp -f "$game/dev_hdd0/game/$title/USRDIR/EBOOT.BIN" "$bin"
    if ! ps3tool_decrypt "$bin" "$elf"; then
        rm -f "$elf"
        [ -n "$rpcs3bin" ] || fail "decrypting the game's EBOOT failed (ps3tool, above) and there is no RPCS3 to try instead (--rpcs3-bin <the rpcs3 AppImage>)"
        echo "  ps3tool could not decrypt it: trying RPCS3"
        rpcs3_decrypt "$bin"
    fi
    if [ ! -f "$elf" ]; then
        echo "  RPCS3's last words (work/logs/rpcs3_decrypt.log):"; tail -n 15 "$root/work/logs/rpcs3_decrypt.log" 2>/dev/null | sed 's/^/    /'
        fail "RPCS3 did not write $elf. Please post work/logs/rpcs3_decrypt.log in a GitHub issue. (Or decrypt it yourself: RPCS3 > Utilities > Decrypt PS3 Binaries on $bin, then run setup.sh again with --eboot <the .elf it wrote>)"
    fi
fi
echo "  $elf"
# the three firmware modules the port translates (rebuild.py's inputs step then finds them in work/fw)
mkdir -p "$root/work/fw"
for m in liblv2 libsysmodule libsre; do
    [ -f "$root/work/fw/$m.prx" ] && continue
    cp -f "$game/dev_flash/sys/external/$m.sprx" "$root/work/fw/$m.sprx"
    if ! ps3tool_decrypt "$root/work/fw/$m.sprx" "$root/work/fw/$m.prx"; then
        rm -f "$root/work/fw/$m.prx"
        [ -n "$rpcs3bin" ] || fail "decrypting $m.sprx failed (ps3tool, above) and there is no RPCS3 to try instead (--rpcs3-bin <the rpcs3 AppImage>)"
        rpcs3_decrypt "$root/work/fw/$m.sprx"
    fi
    [ -f "$root/work/fw/$m.elf" ] && mv -f "$root/work/fw/$m.elf" "$root/work/fw/$m.prx"
    [ -f "$root/work/fw/$m.prx" ] || { tail -n 15 "$root/work/logs/rpcs3_decrypt.log" 2>/dev/null | sed 's/^/    /'; fail "decrypting $m.sprx gave no work/fw/$m.prx (RPCS3's output: work/logs/rpcs3_decrypt.log)"; }
    echo "  work/fw/$m.prx"
done

# ------------------------------------------------------------------------------------------------
step 5 "Python tools (work/venv)"
echo "  ready (made in step 2)"

# ------------------------------------------------------------------------------------------------
step 6 "Vulkan headers, SDL2 headers and glslang (download, then build glslang: a few minutes)"
repo_tgz() {   # repo_tgz <owner/repo> <tag> <dir>: a GitHub tag's source, without the top folder
    if [ -f "$3/.source" ] && [ "$(cat "$3/.source")" = "$1 $2" ]; then echo "  $(basename "$3"): already there"; return; fi
    download "https://codeload.github.com/$1/tar.gz/refs/tags/$2" "$dl/$(basename "$3")-$2.tar.gz" "" "$1 $2"
    rm -rf "$3" && mkdir -p "$3" && tar -xzf "$dl/$(basename "$3")-$2.tar.gz" -C "$3" --strip-components 1
    echo "$1 $2" > "$3/.source"
}
repo_tgz KhronosGroup/Vulkan-Headers v1.3.296 "$deps/Vulkan-Headers"
repo_tgz KhronosGroup/glslang 15.1.0 "$deps/glslang-src"
if [ ! -f "$deps/sdl2-inc/SDL2/SDL.h" ]; then
    download https://github.com/libsdl-org/SDL/releases/download/release-2.30.10/SDL2-2.30.10.tar.gz "$dl/SDL2-2.30.10.tar.gz" \
        f59adf36a0fcf4c94198e7d3d776c1b3824211ab7aeebeb31fe19836661196aa "SDL2 2.30.10 (headers)"
    rm -rf "$deps/sdl2-inc" && mkdir -p "$deps/sdl2-inc/SDL2"
    tar -xzf "$dl/SDL2-2.30.10.tar.gz" -C "$deps/sdl2-inc/SDL2" --strip-components 2 SDL2-2.30.10/include
fi
# SDL2 for CMake: the 2.30.10 headers (as the Windows build) over the system's libSDL2-2.0.so.0; the
# few entry points newer than an older system library are looked up at run time.
sdllib=$(ldlist | awk '/libSDL2-2.0.so.0 .*x86-64/{print $NF}' | head -1)
mkdir -p "$deps/sdl2-cmake"
cat > "$deps/sdl2-cmake/sdl2-config.cmake" <<EOF
set(SDL2_FOUND TRUE)
set(SDL2_INCLUDE_DIRS "$deps/sdl2-inc;$deps/sdl2-inc/SDL2")
set(SDL2_LIBRARIES "$sdllib")
if(NOT TARGET SDL2::SDL2)
  add_library(SDL2::SDL2 SHARED IMPORTED)
  set_target_properties(SDL2::SDL2 PROPERTIES IMPORTED_LOCATION "$sdllib"
    INTERFACE_INCLUDE_DIRECTORIES "$deps/sdl2-inc;$deps/sdl2-inc/SDL2")
endif()
EOF
printf 'set(PACKAGE_VERSION "2.30.10")\nset(PACKAGE_VERSION_COMPATIBLE TRUE)\n' > "$deps/sdl2-cmake/sdl2-config-version.cmake"
export PORT_SDL2_DIR="$deps/sdl2-cmake"
if [ ! -f "$deps/glslang-install-linux/lib/cmake/glslang/glslang-config.cmake" ]; then
    rm -rf "$deps/glslang-build-linux"   # (a failed try leaves CMake's compiler checks cached)
    tick_start "building glslang (a few minutes)"
    "$PORT_CMAKE" -S "$deps/glslang-src" -B "$deps/glslang-build-linux" -G Ninja -DCMAKE_MAKE_PROGRAM="$PORT_NINJA" \
        -DCMAKE_C_COMPILER="$PORT_LLVM/bin/clang" -DCMAKE_CXX_COMPILER="$PORT_LLVM/bin/clang++" -DCMAKE_BUILD_TYPE=Release \
        -DPython3_EXECUTABLE="$venv/bin/python" -DCMAKE_INSTALL_PREFIX="$deps/glslang-install-linux" -DCMAKE_INSTALL_LIBDIR=lib -DBUILD_SHARED_LIBS=OFF \
        -DBUILD_EXTERNAL=OFF -DENABLE_OPT=OFF -DENABLE_GLSLANG_BINARIES=OFF -DGLSLANG_TESTS=OFF -DENABLE_HLSL=ON \
        -DENABLE_SPIRV=ON -DGLSLANG_ENABLE_INSTALL=ON -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld > "$deps/glslang-build.log" 2>&1 \
        && "$PORT_CMAKE" --build "$deps/glslang-build-linux" -j "$jobs" >> "$deps/glslang-build.log" 2>&1 \
        && "$PORT_CMAKE" --install "$deps/glslang-build-linux" >> "$deps/glslang-build.log" 2>&1 \
        || fail "building glslang failed (see $deps/glslang-build.log)"
    tick_stop
fi
# without glslang the build still succeeds, but the game draws nothing (no guest programs): stop here instead
[ -f "$deps/glslang-install-linux/lib/cmake/glslang/glslang-config.cmake" ] \
    || fail "glslang is not where the build looks for it ($deps/glslang-install-linux/lib/cmake/glslang); see $deps/glslang-build.log"
echo "  deps ready"

# ------------------------------------------------------------------------------------------------
step 7 "translating and compiling the game (the long part: 15-60 minutes; each part says what it is and how far it is)"
export PORT_JOBS="$jobs" PORT_LIFT_JOBS="$jobs"
"$venv/bin/python" -I "$root/tools/rebuild.py" || fail "the build failed: see work/logs/ (python3 -I tools/build_errors.py work/logs/compile.log)"
exe="$root/work/port/build/Infamous2Recomp"
[ -x "$exe" ] || fail "no $exe after the build"
# Built inside a container (Distrobox: the way for systems without build tools): the program uses the
# container's libraries, so tools/play.sh says where to start it when it is started outside
if [ -f /run/.containerenv ] || [ -f /.dockerenv ] || [ -n "${CONTAINER_ID:-}" ]; then echo "${CONTAINER_ID:-}" > "$root/work/built-in-container"
else rm -f "$root/work/built-in-container"; fi

# ------------------------------------------------------------------------------------------------
step 8 "desktop launcher"
if [ $desktop = 1 ]; then
    apps="${XDG_DATA_HOME:-$HOME/.local/share}/applications"; mkdir -p "$apps"
    icon="$root/work/icon/icon0.png"; mkdir -p "$root/work/icon"; cp -f "$disc/PS3_GAME/ICON0.PNG" "$icon" 2>/dev/null || icon=""
    cat > "$apps/infamous2-port.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=inFAMOUS 2 (PC port)
Comment=inFAMOUS 2, natively on Linux (your own copy, recompiled)
Exec="${root//%/%%}/tools/play.sh"
Path=$root
${icon:+Icon=$icon}
Terminal=false
Categories=Game;
EOF
    echo "  $apps/infamous2-port.desktop"
else echo "  skipped (--no-desktop)"; fi
echo "== done in $(( ($(date +%s) - t0) / 60 )) min: play with tools/play.sh (or the desktop launcher). RPCS3 is not needed (the game's update and firmware files are in game/)."
