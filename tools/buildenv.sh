# The Linux build environment, the same on every x86-64 distribution (SteamOS, Bazzite, Ubuntu, Fedora, Arch, ...):
# everything the build needs is downloaded into tools-cache/ (pinned: an exact version, its address and SHA-256), and
# nothing comes from the system's packages: no root, no package manager. Sourced by tools/setup.sh (after
# tools/libc_headers.sh: lh_get) and by tools/install_gui.sh (be_python only); not a program of its own.
#
#   be_python <tools-cache> <download folder>   Python for every setup run (work/venv, the build's tools): a portable
#                                                CPython (python-build-standalone) -> tools-cache/python; sets PORT_PY
#   be_sysroot <tools-cache> <download folder>  the C/C++ headers and libraries the compiler builds against: a sysroot
#                                                of Ubuntu 22.04's own packages (glibc 2.35, Linux 5.15 headers, GCC 12's
#                                                libstdc++ and libgcc) -> tools-cache/sysroot; clang reads it through
#                                                --sysroot in tools-cache/llvm/bin/clang*.cfg, so nothing comes from
#                                                /usr/include. libstdc++ and libgcc are linked statically (the .cfg's
#                                                -static-libstdc++ -static-libgcc), so the game needs no C++ library
#                                                from the system: only a glibc of 2.35 or newer and the Vulkan driver.
#   be_sdl2 <tools-cache> <download folder>     the SDL2 library the game runs with: Valve's own build for the Steam
#                                                Runtime (it loads X11, PulseAudio / PipeWire / ALSA, udev by itself
#                                                when they are there, and needs nothing else) -> tools-cache/sdl2; the
#                                                build links against it and puts it beside the game program (RUNPATH
#                                                $ORIGIN); sets PORT_SDL2_RUNTIME
# The game's binaries are built for glibc 2.35 (Ubuntu 22.04, Debian 12, Fedora 36 and every later release; SteamOS 3.5+).

BE_GLIBC_MIN=2.35
BE_UBUNTU="https://archive.ubuntu.com/ubuntu/pool"
BE_LAUNCHPAD="https://launchpad.net/ubuntu/+archive/primary/+files"   # Launchpad keeps every package ever published

# Python: python-build-standalone (astral-sh), CPython 3.14 (3.14 reads the zstd packages Ubuntu makes), the
# install_only build without debug symbols (37 MB; 109 MB unpacked). Runs on glibc 2.17 and newer.
BE_PY_TAG=20261026
BE_PY_FILE="cpython-3.14.10+20261026-x86_64-unknown-linux-gnu-install_only_stripped.tar.gz"
BE_PY_SHA=4d3c16c59ed0802a3d1d34d8bce504b15ceeaff39d62dadecc6a9fa803412aed

# The sysroot: Ubuntu 22.04 (jammy) packages, amd64. One row a package:
#   name | version | SHA-256 (= the archive's Packages index) | its pool folder
# glibc and the kernel headers are the release's own versions (they never leave the archive); GCC 12 is 12.3.0, the
# jammy-updates build (Ubuntu's own libstdc++6 on 22.04 is this one). 11.6 MB of packages, 40 MB unpacked.
be_sysroot_table() {
    cat <<'EOF'
libc6 2.35-0ubuntu3 ea9a27e0ebdd0cfc9c750d94f8074f3a35d1f97dcc77ae04c370fb498a6b6db2 main/g/glibc
libc6-dev 2.35-0ubuntu3 cc37cab5c60bcfe4bbf289a8002f369949a41ed46e8b51a0503a001099370c56 main/g/glibc
linux-libc-dev 5.15.0-25.25 1b2a93020593c9e94a25f750ce442da5a6e8ff48a20f52cec92dfc3fa35336d8 main/l/linux
libgcc-12-dev 12.3.0-1ubuntu1~22.04.3 a9933d9949219f3a0598bf48390091f45c03c036fe736de38f33b7c60eddca0e main/g/gcc-12
libstdc++-12-dev 12.3.0-1ubuntu1~22.04.3 250f538cb3a5dfbbb767e16cb3d840a2b113de1582bec155ea97600740a6ed1e universe/g/gcc-12
libgcc-s1 12.3.0-1ubuntu1~22.04.3 d383e642d83263147f7b4b69fb92cf1036e5246c6be3accf10b77f986684589a main/g/gcc-12
EOF
}
BE_GCC_VER=12

# SDL2: the Steam Runtime 1 (scout) build of SDL 2.32.10 from Valve's package repository (585 KB; it needs only
# libc / libm / libdl / libpthread / librt of glibc 2.14+). Valve's pool holds only the newest build: when this one is
# gone, setup uses the system's libSDL2-2.0.so.0 instead (as before) and says so.
BE_SDL_FILE="libsdl2-2.0-0_2.32.10+dfsg-4+steamrt1.1+srt1_amd64.deb"
BE_SDL_SHA=102f431dddb13556099a915aadbb9060a3f9028795a04229abcab84958e91052
BE_SDL_URL="https://repo.steampowered.com/steamrt/pool/main/libs/libsdl2/libsdl2-2.0-0_2.32.10%2Bdfsg-4%2Bsteamrt1.1%2Bsrt1_amd64.deb"

be_urlenc() { local s="${1//+/%2B}"; printf '%s' "${s//\~/%7E}"; }

# Each be_* prints its progress; on failure it returns 1 with the reason in BE_WHY.
BE_WHY=""

# be_python <tools-cache> <download folder>: PORT_PY = tools-cache/python/bin/python3
be_python() {
    local cache="$1" dl="$2" py="$1/python/bin/python3" tmp="$1/python.tmp"
    if [ "$(cat "$cache/python/.ready" 2>/dev/null)" = "$BE_PY_FILE" ] && "$py" -I -c "import venv" 2>/dev/null; then
        PORT_PY="$py"; export PORT_PY; return 0
    fi
    mkdir -p "$dl"
    lh_get "Python 3.14.10 (37 MB)" "$BE_PY_SHA" "$dl/$BE_PY_FILE" \
        "https://github.com/astral-sh/python-build-standalone/releases/download/$BE_PY_TAG/$(be_urlenc "$BE_PY_FILE")" \
        || { BE_WHY="Python could not be downloaded (GitHub)"; return 1; }
    rm -rf "$tmp" && mkdir -p "$tmp" && tar -xzf "$dl/$BE_PY_FILE" -C "$tmp" \
        || { rm -rf "$tmp"; BE_WHY="unpacking Python failed"; return 1; }
    if ! "$tmp/python/bin/python3" -I -c "import venv, ensurepip, ssl, lzma, zlib" 2>/dev/null; then
        rm -rf "$tmp"; BE_WHY="the downloaded Python does not start on this system"; return 1
    fi
    rm -rf "$cache/python" && mv "$tmp/python" "$cache/python" && rmdir "$tmp"
    echo "$BE_PY_FILE" > "$cache/python/.ready"
    PORT_PY="$py"; export PORT_PY
    echo "  Python: tools-cache/python ($("$py" -V 2>&1), a portable build)"
    return 0
}

# be_sysroot <tools-cache> <download folder>: tools-cache/sysroot. Needs PORT_PY.
be_sysroot() {
    local cache="$1" dl="$2" sr="$1/sysroot" tmp="$1/sysroot.tmp" id name ver sha pool f debs=() h
    id="$(be_sysroot_table | cksum | cut -d' ' -f1)"
    if [ "$(cat "$sr/.ready" 2>/dev/null)" = "$id" ] && [ -f "$sr/usr/include/features.h" ]; then
        echo "  C/C++ headers and libraries: tools-cache/sysroot (Ubuntu 22.04's glibc $BE_GLIBC_MIN and GCC $BE_GCC_VER, already there)"
        return 0
    fi
    mkdir -p "$dl"
    while read -r name ver sha pool; do
        f="${name}_${ver}_amd64.deb"
        lh_get "$name $ver" "$sha" "$dl/$f" "$BE_UBUNTU/$pool/$(be_urlenc "$f")" "$BE_LAUNCHPAD/$(be_urlenc "$f")" \
            || { BE_WHY="downloading $f failed (Ubuntu's archive and Launchpad)"; return 1; }
        debs+=("$dl/$f")
    done < <(be_sysroot_table)
    rm -rf "$tmp"
    "$PORT_PY" -I "$BE_HERE/buildenv.py" deb "$tmp" "${debs[@]}" > "$cache/sysroot-unpack.log" 2>&1 \
        || { rm -rf "$tmp"; BE_WHY="unpacking the packages failed ($(tail -n 1 "$cache/sysroot-unpack.log"))"; return 1; }
    for h in usr/include/features.h usr/include/linux/limits.h "usr/include/c++/$BE_GCC_VER/vector" \
             "usr/lib/gcc/x86_64-linux-gnu/$BE_GCC_VER/crtbeginS.o" "usr/lib/gcc/x86_64-linux-gnu/$BE_GCC_VER/libstdc++.a" \
             usr/lib/x86_64-linux-gnu/Scrt1.o usr/lib/x86_64-linux-gnu/libc_nonshared.a lib/x86_64-linux-gnu/libc.so.6; do
        [ -e "$tmp/$h" ] || { rm -rf "$tmp"; BE_WHY="the unpacked packages have no $h"; return 1; }
    done
    rm -rf "$sr" && mv "$tmp" "$sr" && echo "$id" > "$sr/.ready"
    echo "  C/C++ headers and libraries: tools-cache/sysroot (Ubuntu 22.04's glibc $BE_GLIBC_MIN, Linux 5.15 headers, GCC $BE_GCC_VER's libstdc++; $(du -sm "$sr" | cut -f1) MB)"
    return 0
}

# be_cfg <tools-cache>: clang's .cfg files for the sysroot (<CFGDIR> = the .cfg's folder: the port's folder can be moved,
# and its name may hold spaces)
be_cfg() {
    printf '%s\n' '--sysroot=<CFGDIR>/../../sysroot' -static-libgcc > "$1/llvm/bin/clang.cfg"
    printf '%s\n' '--sysroot=<CFGDIR>/../../sysroot' -static-libgcc -static-libstdc++ > "$1/llvm/bin/clang++.cfg"
}

# be_sdl2 <tools-cache> <download folder>: PORT_SDL2_RUNTIME = tools-cache/sdl2/libSDL2-2.0.so.0
be_sdl2() {
    local cache="$1" dl="$2" d="$1/sdl2" tmp="$1/sdl2.tmp" so
    if [ "$(cat "$d/.ready" 2>/dev/null)" = "$BE_SDL_FILE" ] && [ -f "$d/libSDL2-2.0.so.0" ]; then
        PORT_SDL2_RUNTIME="$d/libSDL2-2.0.so.0"; export PORT_SDL2_RUNTIME; return 0
    fi
    mkdir -p "$dl"
    lh_get "SDL2 2.32.10 (Valve's Steam Runtime build)" "$BE_SDL_SHA" "$dl/$BE_SDL_FILE" "$BE_SDL_URL" \
        || { BE_WHY="Valve's SDL2 package could not be downloaded"; return 1; }
    rm -rf "$tmp"
    "$PORT_PY" -I "$BE_HERE/buildenv.py" deb "$tmp" "$dl/$BE_SDL_FILE" >/dev/null 2>&1 \
        || { rm -rf "$tmp"; BE_WHY="unpacking Valve's SDL2 package failed"; return 1; }
    so="$(ls "$tmp"/usr/lib/x86_64-linux-gnu/libSDL2-2.0.so.0.* 2>/dev/null | head -n 1)"
    [ -n "$so" ] || { rm -rf "$tmp"; BE_WHY="Valve's SDL2 package has no libSDL2-2.0.so.0"; return 1; }
    rm -rf "$d" && mkdir -p "$d" && cp -f "$so" "$d/libSDL2-2.0.so.0" && rm -rf "$tmp"
    echo "$BE_SDL_FILE" > "$d/.ready"
    PORT_SDL2_RUNTIME="$d/libSDL2-2.0.so.0"; export PORT_SDL2_RUNTIME
    echo "  SDL2: tools-cache/sdl2 (2.32.10, Valve's Steam Runtime build)"
    return 0
}

# be_vulkan: 0 = a Vulkan driver is installed (the loader libvulkan.so.1 and a driver's ICD file)
be_vulkan() {
    local d ok=0 dirs="${XDG_DATA_DIRS:-/usr/local/share:/usr/share}:/usr/share:/usr/local/share:/etc:/run/opengl-driver/share"
    while IFS= read -r d; do
        [ -n "$d" ] && ls "$d"/vulkan/icd.d/*.json >/dev/null 2>&1 && { ok=1; break; }
    done < <(printf '%s\n' "${dirs//:/$'\n'}")
    [ $ok = 1 ] || return 1
    { ldconfig -p 2>/dev/null || /sbin/ldconfig -p 2>/dev/null; } | grep -q 'libvulkan\.so\.1 ' && return 0
    for d in /usr/lib/x86_64-linux-gnu /usr/lib64 /usr/lib /lib/x86_64-linux-gnu /run/opengl-driver/lib; do
        [ -e "$d/libvulkan.so.1" ] && return 0
    done
    return 1
}

BE_HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
