#!/bin/bash
# Linux Setup (tools/setup_build.ps1's counterpart): from your own copy of inFAMOUS 2 to a native
# Linux build, in this folder only (nothing is installed system-wide, no root needed).
#   tools/setup.sh --disc <folder holding PS3_GAME> --rpcs3 <RPCS3's folder> [--rpcs3-bin <rpcs3 AppImage>]
#                  [--eboot <decrypted EBOOT.elf>] [--jobs N] [--no-desktop]
#   --rpcs3      RPCS3's data folder: the one holding dev_hdd0/ and dev_flash/, with the game's 1.04 update
#                installed (File > Install Packages) and the PS3 firmware (File > Install Firmware).
#                Default: ~/.config/rpcs3 (the AppImage, EmuDeck), else the Flatpak's
#                ~/.var/app/net.rpcs3.RPCS3/config/rpcs3
#   --rpcs3-bin  the RPCS3 AppImage (or rpcs3 program) that decrypts the update's EBOOT.BIN and three
#                firmware modules (once; RPCS3 is not needed after Setup). Default: rpcs3 on PATH, an
#                rpcs3*.AppImage in ~/Applications (EmuDeck), else the Flatpak net.rpcs3.RPCS3.
#   --eboot      an already decrypted 1.04 EBOOT.elf instead (then RPCS3 is needed only for the firmware
#                modules, unless work/fw holds liblv2.prx, libsysmodule.prx and libsre.prx)
#   --jobs       compile jobs (default: half the CPU threads, at most 8: each lifted file is large)
# Steps:
#   1. check this PC and the game                 6. Vulkan headers, SDL2 headers, glslang -> deps/
#   2. the build tools -> tools-cache/            7. translate and compile the game (15-60 min)
#      (LLVM 22.1.8, and CMake / Ninja             -> work/port/build/Infamous2Recomp
#      when the system has none)                   8. a desktop launcher (~/.local/share/applications)
#   3. your game linked/copied into game/
#   4. the 1.04 EBOOT decrypted -> eboot/EBOOT_upd104.elf
#   5. Python tools -> work/venv
# Needs (distribution packages): a C/C++ toolchain's headers and libraries (Debian/Ubuntu:
# build-essential; Fedora: gcc-c++; Arch: base-devel), python3 with venv, curl, tar, xz, unzip, the
# Vulkan loader (libvulkan1 / vulkan-loader) with your GPU's driver, and SDL2 (libsdl2-2.0-0 / SDL2).
# Play with tools/play.sh (or the desktop launcher).
set -eu   # (no pipefail: 'ldconfig -p | grep -q' would fail on grep's early exit)
root="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
disc=""; rpcs3=""; rpcs3bin=""; eboot=""; desktop=1
jobs=$(( $(nproc) / 2 )); [ $jobs -gt 8 ] && jobs=8; [ $jobs -lt 1 ] && jobs=1
while [ $# -gt 0 ]; do
    case "$1" in
        --disc) disc="$2"; shift ;;
        --rpcs3) rpcs3="$2"; shift ;;
        --rpcs3-bin) rpcs3bin="$2"; shift ;;
        --eboot) eboot="$2"; shift ;;
        --jobs) jobs="$2"; shift ;;
        --no-desktop) desktop=0 ;;
        -h|--help) sed -n '2,27p' "$0"; exit 0 ;;
        *) echo "unknown option $1 (--help)"; exit 2 ;;
    esac
    shift
done
if [ -z "$rpcs3" ]; then
    rpcs3="${XDG_CONFIG_HOME:-$HOME/.config}/rpcs3"
    fp="$HOME/.var/app/net.rpcs3.RPCS3/config/rpcs3"
    [ ! -d "$rpcs3/dev_hdd0" ] && [ -d "$fp/dev_hdd0" ] && rpcs3="$fp"
fi
t0=$(date +%s)
step() { echo "== [$1/8] $2   ($(( ($(date +%s) - t0) / 60 )) min so far)"; }
fail() { echo "== FAILED: $*"; exit 1; }
cache="$root/tools-cache"; deps="$root/deps"; dl="$cache/dl"
mkdir -p "$cache" "$deps" "$dl"

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
        [ -f "$inc/vector" ] && [ -f "$d/libstdc++.so" -o -f "$d/libstdc++.a" ] && { gccdir="$d"; break 2; }; done
done
[ -n "$gccdir" ] && ok ok "C++ headers and libraries" "$gccdir" || ok MISS "C++ headers and libraries" "install them (Debian/Ubuntu: build-essential; Fedora: gcc-c++; Arch: base-devel)"
for t in curl tar xz unzip; do command -v $t >/dev/null && ok ok $t found || ok MISS $t "install it"; done
title=""
if [ -z "$disc" ]; then ok MISS "Game disc folder" "--disc <the folder that holds PS3_GAME>"
elif [ ! -f "$disc/PS3_GAME/PARAM.SFO" ]; then ok MISS "Game disc folder" "no PS3_GAME/PARAM.SFO in $disc"
else
    disc="$(readlink -f "$disc")"; title=$(sfo "$disc/PS3_GAME/PARAM.SFO" TITLE_ID)
    case "$title" in
        BCES01143|BCUS98125|NPUA80638) ok ok "Game" "$(sfo "$disc/PS3_GAME/PARAM.SFO" TITLE) [$title] (tested)" ;;
        BCES01144|BCES01229|NPEA00318) ok note "Game" "$(sfo "$disc/PS3_GAME/PARAM.SFO" TITLE) [$title] (untested: please report how it goes)" ;;
        *) ok MISS "Game" "[$title] is not inFAMOUS 2" ;;
    esac
fi
upd="$rpcs3/dev_hdd0/game/$title"
if [ -n "$title" ]; then
    ver=""; [ -f "$upd/PARAM.SFO" ] && ver=$(sfo "$upd/PARAM.SFO" APP_VER)
    if [ "$ver" = "01.04" ] || { [ "${title:0:2}" = NP ] && [ "$ver" = "01.03" ]; }; then ok ok "Game update in RPCS3" "$upd ($ver)"
    else ok MISS "Game update in RPCS3" "${ver:+found $ver; }install the 1.04 update in RPCS3 (File > Install Packages), or give --rpcs3 <RPCS3's folder holding dev_hdd0>"; fi
fi
[ -f "$rpcs3/dev_flash/sys/external/liblv2.sprx" ] && ok ok "PS3 firmware in RPCS3" "$rpcs3/dev_flash" \
    || ok MISS "PS3 firmware in RPCS3" "install it in RPCS3: File > Install Firmware (PS3UPDAT.PUP from playstation.com)"
[ -z "$rpcs3bin" ] && rpcs3bin=$(command -v rpcs3 || true)
[ -z "$rpcs3bin" ] && rpcs3bin=$(ls -1 "$HOME"/Applications/rpcs3*.AppImage "$HOME"/Applications/RPCS3*.AppImage 2>/dev/null | head -1 || true)
if [ -z "$rpcs3bin" ] && command -v flatpak >/dev/null && flatpak info net.rpcs3.RPCS3 >/dev/null 2>&1; then
    rpcs3bin="$cache/rpcs3-flatpak.sh"   # one program path for rpcs3_decrypt
    printf '#!/bin/sh\nexec flatpak run --filesystem=%s net.rpcs3.RPCS3 "$@"\n' "$root" > "$rpcs3bin"; chmod +x "$rpcs3bin"
fi
needfw=0; for m in liblv2 libsysmodule libsre; do [ -f "$root/work/fw/$m.prx" ] || needfw=1; done
if [ -z "$eboot" ] && [ ! -f "$root/eboot/EBOOT_upd104.elf" ] || [ $needfw = 1 ]; then
    [ -n "$rpcs3bin" ] && [ -x "$rpcs3bin" ] && ok ok "RPCS3 program" "$rpcs3bin" || ok MISS "RPCS3 program" "--rpcs3-bin <the rpcs3 AppImage> (it decrypts the game's code once)"
fi
for sub in engine/CMakeLists.txt base/infamous2-lle/CMakeLists.txt; do   # (one repository since v0.5.0)
    [ -f "$root/$sub" ] && ok ok "${sub%%/*}" present || ok MISS "${sub%%/*}" "missing: download or git clone the port again"
done
[ $miss = 0 ] || fail "fix the MISS items first"

# ------------------------------------------------------------------------------------------------
step 2 "the build tools (into tools-cache/, nothing is installed)"
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
for d in clang clang++; do echo "--gcc-install-dir=$gccdir" > "$cache/llvm/bin/$d.cfg"; done
"$cache/llvm/bin/clang" --version | sed -n '1s/^/  /p'
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

# ------------------------------------------------------------------------------------------------
step 3 "your game: the disc folder linked, the update, firmware files and HDD install copied into game/"
game="$root/game"
mkdir -p "$game/dev_hdd0/game" "$game/gamedata"
if [ -L "$game/disc" ] && [ "$(readlink -f "$game/disc")" != "$disc" ]; then rm -f "$game/disc"; fi
[ -e "$game/disc" ] || ln -s "$disc" "$game/disc"
echo "  $game/disc -> $disc (keep your disc folder where it is)"
copy_tree() {   # copy_tree <src> <dst> <what>
    echo "  copying $3"; mkdir -p "$2"; cp -a --no-preserve=ownership "$1/." "$2/"
}
[ -f "$game/dev_hdd0/game/$title/USRDIR/EBOOT.BIN" ] || copy_tree "$upd" "$game/dev_hdd0/game/$title" "the 1.04 update (0.2 GB)"
[ -d "$game/dev_flash/sys" ] || copy_tree "$rpcs3/dev_flash" "$game/dev_flash" "the PS3 firmware files the game reads (0.2 GB)"
inst="$rpcs3/dev_hdd0/game/${title}_INSTALL"
if [ -d "$inst" ] && [ ! -d "$game/dev_hdd0/game/${title}_INSTALL" ]; then copy_tree "$inst" "$game/dev_hdd0/game/${title}_INSTALL" "the game's HDD install (2.4 GB)"
elif [ ! -d "$game/dev_hdd0/game/${title}_INSTALL" ]; then echo "  note: no ${title}_INSTALL in RPCS3 -- the port's first boot installs it"; fi

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
step 4 "the game's 1.04 EBOOT decrypted"
mkdir -p "$root/eboot"
elf="$root/eboot/EBOOT_upd104.elf"
if [ -n "$eboot" ]; then cp -f "$eboot" "$elf"
elif [ ! -f "$elf" ]; then
    bin="$root/eboot/EBOOT_upd104.BIN"
    cp -f "$upd/USRDIR/EBOOT.BIN" "$bin"
    rpcs3_decrypt "$bin"
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
    cp -f "$rpcs3/dev_flash/sys/external/$m.sprx" "$root/work/fw/$m.sprx"
    rpcs3_decrypt "$root/work/fw/$m.sprx"
    [ -f "$root/work/fw/$m.elf" ] && mv -f "$root/work/fw/$m.elf" "$root/work/fw/$m.prx"
    [ -f "$root/work/fw/$m.prx" ] || { tail -n 15 "$root/work/logs/rpcs3_decrypt.log" 2>/dev/null | sed 's/^/    /'; fail "decrypting $m.sprx gave no work/fw/$m.prx (RPCS3's output: work/logs/rpcs3_decrypt.log)"; }
    echo "  work/fw/$m.prx"
done

# ------------------------------------------------------------------------------------------------
step 5 "Python tools (work/venv)"
venv="$root/work/venv"
[ -x "$venv/bin/python" ] && "$venv/bin/python" -c "import sys" 2>/dev/null || { rm -rf "$venv"; python3 -m venv "$venv"; }
"$venv/bin/python" -m pip install --disable-pip-version-check -q -r "$root/engine/requirements.txt" || fail "pip install failed"

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
    "$PORT_CMAKE" -S "$deps/glslang-src" -B "$deps/glslang-build-linux" -G Ninja -DCMAKE_MAKE_PROGRAM="$PORT_NINJA" \
        -DCMAKE_C_COMPILER="$PORT_LLVM/bin/clang" -DCMAKE_CXX_COMPILER="$PORT_LLVM/bin/clang++" -DCMAKE_BUILD_TYPE=Release \
        -DPython3_EXECUTABLE="$venv/bin/python" -DCMAKE_INSTALL_PREFIX="$deps/glslang-install-linux" -DBUILD_SHARED_LIBS=OFF \
        -DBUILD_EXTERNAL=OFF -DENABLE_OPT=OFF -DENABLE_GLSLANG_BINARIES=OFF -DGLSLANG_TESTS=OFF -DENABLE_HLSL=ON \
        -DENABLE_SPIRV=ON -DGLSLANG_ENABLE_INSTALL=ON -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld > "$deps/glslang-build.log" 2>&1 \
        && "$PORT_CMAKE" --build "$deps/glslang-build-linux" -j "$jobs" >> "$deps/glslang-build.log" 2>&1 \
        && "$PORT_CMAKE" --install "$deps/glslang-build-linux" >> "$deps/glslang-build.log" 2>&1 \
        || fail "building glslang failed (see $deps/glslang-build.log)"
fi
echo "  deps ready"

# ------------------------------------------------------------------------------------------------
step 7 "translating and compiling the game (the long part: 15-60 minutes)"
export PORT_JOBS="$jobs" PORT_LIFT_JOBS="$jobs"
"$venv/bin/python" -I "$root/tools/rebuild.py" || fail "the build failed: see work/logs/ (python3 -I tools/build_errors.py work/logs/compile.log)"
exe="$root/work/port/build/Infamous2Recomp"
[ -x "$exe" ] || fail "no $exe after the build"

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
Exec="$root/tools/play.sh"
Path=$root
${icon:+Icon=$icon}
Terminal=false
Categories=Game;
EOF
    echo "  $apps/infamous2-port.desktop"
else echo "  skipped (--no-desktop)"; fi
echo "== done in $(( ($(date +%s) - t0) / 60 )) min: play with tools/play.sh (or the desktop launcher). RPCS3 is no longer needed."
