# The C library's headers for systems that ship without them, and the compiler self-test. Sourced by
# tools/setup.sh (not a program of its own).
#
# GitHub #81: SteamOS (the Steam Deck) has the compiler's C++ headers and every library a link needs
# (crt1.o, libc.so, libstdc++.so, libgcc), but its image leaves out the headers of the glibc and
# linux-api-headers packages: no <features.h>, <stdio.h>, <pthread.h>, <linux/...>. Its system folders are
# read-only, so they cannot be installed either. Setup then gets the two packages SteamOS itself was made
# from (Valve's package mirror; the same files are in Arch Linux's archive), checks their SHA-256, and
# unpacks only their usr/include into tools-cache/sysroot-fix/include. The compiler searches that folder
# after every system folder (-idirafter in tools-cache/llvm/bin/clang*.cfg), so a system that has the
# headers keeps using its own. Nothing is installed on the system, and the game still links against and
# runs with the system's own C library: the headers are the ones of that same glibc release.
#
# Test switches: LH_INC (the system's include folder, default /usr/include), LH_GLIBC (its glibc version).

LH_MIRROR="https://steamdeck-packages.steamos.cloud/archlinux-mirror"
LH_ARCHIVE="https://archive.archlinux.org/packages"

# One row per SteamOS release, oldest first:
#   glibc release | SteamOS | glibc package | its SHA-256 | kernel headers package | its SHA-256
# (packages: <name>.pkg.tar.zst, 10-11 MB and 1.3-1.6 MB; the headers are 11-13 MB unpacked)
lh_table() {
    cat <<'EOF'
2.37 3.5 glibc-2.37-3-x86_64 5e0e7c6062383e66a695c61d323641e85d408b8e8ce12c13c37649f38d0ca527 linux-api-headers-6.3-1-any 01f78c15fca00618f3916304622678805098dfa20d8daa4d1c476fd3facd9477
2.39 3.6 glibc-2.39-1-x86_64 17ff75d9476c690f50911a6c23d844ffc760471ae5e8e69b6a1dcde86113332a linux-api-headers-6.7-1-any e794502033db416930499a53c21d09614e6518a31adc8cf06896d4fc66dd6e61
2.40 3.7 glibc-2.40+r16+gaa533d58ff-2-x86_64 c87c0e71fd03472918dc052b5833d5568ab62866e8259d28ab453ffc4bcf8291 linux-api-headers-6.10-1-x86_64 c1c44e637528f6ca17fbf1d896872c726ca014f35ee21879383990ea252d1539
2.41 3.8 glibc-2.41+r65+ge7c419a29575-1-x86_64 4eeb7ce640fcb5c38a8a89c9015f68979f7f91cc71d715d3bd90f74bd442a107 linux-api-headers-6.15-1-x86_64 b5cdf41804e8e9929e84dbe45ba212abafa70f36374cff88b931d79d373d3efd
2.43 3.9 glibc-2.43+r37+gfdf10644d6ee-1-x86_64 213332b0d5b712c20a12dd61d96ba4c21f192df63d454a94ad5cef2b7864e8d1 linux-api-headers-7.1-1-x86_64 a6edb1e19a58c43abb93b38e70eb327b2e8b0c15aa74abf31a187c0ee6a5d732
2.44 3.10 glibc-2.44+r50+g1848099f063e-1-x86_64 e8e4d50d45cb2bd21c9f7eabacc6a01bd5eb93dbd476f3f44d44038fc1eba417 linux-api-headers-7.3-1-x86_64 a5a6f5fc8922ef454c54e65e321906183e6606acaadaf416896dcbc44f2194ed
EOF
}

# The C headers a build needs that this system lacks (names, space separated); nothing = all there.
# (Debian/Ubuntu keep some of them in the multiarch folder.)
lh_missing() {
    local inc="${LH_INC:-/usr/include}" h d found out=""
    for h in features.h stdio.h pthread.h linux/limits.h asm/errno.h; do
        found=0
        for d in "$inc" "$inc/x86_64-linux-gnu"; do [ -f "$d/$h" ] && found=1; done
        [ $found = 1 ] || out="$out $h"
    done
    printf '%s' "${out# }"
}

# This system's glibc release ("2.43"); nothing when it cannot be told (another C library)
lh_glibc() {
    local v="${LH_GLIBC:-}"
    [ -z "$v" ] && v=$(getconf GNU_LIBC_VERSION 2>/dev/null | awk '{print $2}' || true)
    [ -z "$v" ] && v=$(ldd --version 2>/dev/null | head -n 1 | grep -iE "glibc|gnu libc" | grep -oE '[0-9]+\.[0-9]+$' || true)
    printf '%s' "$v"
}

# lh_pick <glibc release>: the table's row for it; for a release between two rows the older one (headers may
# be older than the C library, never newer); nothing for a release older than the first row, or none given.
lh_pick() {
    lh_table | awk -v v="$1" '
        function n(s,   a) { split(s, a, "."); return a[1] * 1000 + a[2] }
        v ~ /^[0-9]+\.[0-9]+/ && n($1) <= n(v) { row = $0 }
        END { if (row != "") print row }'
}

# lh_get <name> <sha256> <out> <url>...: the first address that gives the file with that SHA-256
lh_get() {
    local name="$1" sha="$2" out="$3" u host
    shift 3
    if [ -f "$out" ] && [ "$(sha256sum "$out" | cut -c1-64)" = "$sha" ]; then return 0; fi
    for u in "$@"; do
        host="${u#https://}"; host="${host%%/*}"
        echo "  downloading $name (from $host)"
        if curl -fL -sS --retry 2 --connect-timeout 30 -A "inFAMOUS2-port-setup" -o "$out.part" "$u"; then
            if [ "$(sha256sum "$out.part" | cut -c1-64)" = "$sha" ]; then mv -f "$out.part" "$out"; return 0; fi
            echo "  $name: wrong SHA-256 (not used)"
        fi
        rm -f "$out.part"
    done
    return 1
}

# lh_unpack <package.pkg.tar.zst> <folder>: its usr/include -> <folder>/include (bsdtar, GNU tar with zstd,
# else Python 3.14's own zstd)
lh_unpack() {
    local f="$1" d="$2"
    if command -v bsdtar >/dev/null 2>&1 && bsdtar -xf "$f" -C "$d" --strip-components 1 usr/include 2>/dev/null; then return 0; fi
    if tar --force-local --zstd -xf "$f" -C "$d" --strip-components=1 usr/include 2>/dev/null; then return 0; fi
    python3 -I - "$f" "$d" 2>/dev/null <<'EOF'
import sys, tarfile
f, d = sys.argv[1:3]
try:
    from compression import zstd as z      # Python 3.14 and later
except ImportError:
    import zstandard as z                  # the package, where it is installed
kw = {"filter": "data"} if hasattr(tarfile, "data_filter") else {}
with z.open(f, "rb") as fh, tarfile.open(fileobj=fh, mode="r|") as t:
    for m in t:
        if m.name.startswith("usr/include/") and (m.isfile() or m.isdir()):
            m.name = m.name[4:]
            t.extract(m, d, **kw)
EOF
}

# lh_install <tools-cache> <download folder> <a row of lh_table>: <tools-cache>/sysroot-fix/include
lh_install() {
    local cache="$1" dl="$2" gv rel gp gs kp ks fix tmp name sha sub enc
    read -r gv rel gp gs kp ks <<< "$3"
    fix="$cache/sysroot-fix"; tmp="$cache/sysroot-fix.tmp"
    if [ "$(cat "$fix/.ready" 2>/dev/null)" = "$gp $kp" ] && [ -f "$fix/include/features.h" ]; then
        echo "  C library headers: tools-cache/sysroot-fix (glibc $gv, already there)"; return 0
    fi
    rm -rf "$tmp"; mkdir -p "$tmp" "$dl"
    while read -r name sha sub; do
        enc="${name//+/%2B}.pkg.tar.zst"
        lh_get "$name" "$sha" "$dl/$name.pkg.tar.zst" "$LH_MIRROR/core-$rel/os/x86_64/$enc" "$LH_ARCHIVE/$sub/$enc" \
            || { echo "  downloading $name failed (Valve's package mirror and Arch Linux's archive)"; rm -rf "$tmp"; return 1; }
        lh_unpack "$dl/$name.pkg.tar.zst" "$tmp" \
            || { echo "  unpacking $name failed (it needs bsdtar, or tar with zstd, or Python 3.14)"; rm -rf "$tmp"; return 1; }
    done <<EOF
$gp $gs g/glibc
$kp $ks l/linux-api-headers
EOF
    if [ ! -f "$tmp/include/features.h" ] || [ ! -f "$tmp/include/linux/limits.h" ]; then
        echo "  the unpacked packages do not hold the headers"; rm -rf "$tmp"; return 1
    fi
    rm -rf "$fix"; mv "$tmp" "$fix"; echo "$gp $kp" > "$fix/.ready"
    echo "  C library headers: tools-cache/sysroot-fix (glibc $gv and Linux headers, as SteamOS $rel's packages)"
    return 0
}

# lh_why <compiler log>: the first thing it could not find, in plain words
lh_why() {
    local h l
    h=$(sed -n "s/.*fatal error: '\([^']*\)' file not found.*/\1/p" "$1" | head -n 1)
    l=$(sed -n -e 's/.*unable to find library \(-l[^ ]*\).*/\1/p' -e 's/.*cannot find \(-l[^ :]*\).*/\1/p' \
               -e 's/.*cannot open \([^ :]*crt[^ :]*\.o\).*/\1/p' "$1" | head -n 1)
    if [ -n "$h" ]; then printf 'the header <%s> is missing' "$h"
    elif [ -n "$l" ]; then printf 'the library or start file %s is missing' "$l"
    else printf '%s' "$(grep -m 1 -iE 'error|not found|cannot' "$1" || echo 'no reason given')"; fi
}

# lh_selftest <clang> <clang++> <work folder>: a small C and a small C++ program, with the headers and
# libraries the engine uses and the build's own compiler, flags and linker, are compiled, linked and started.
# 0 = fine; else 1 and the reason on stdout (the compiler's own words stay in <work folder>/log.txt).
lh_selftest() {
    local cc="$1" cxx="$2" w="$3"
    rm -rf "$w"; mkdir -p "$w"
    cat > "$w/t.c" <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <errno.h>
#include <signal.h>
#include <setjmp.h>
#include <pthread.h>
#include <sched.h>
#include <dlfcn.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <execinfo.h>
#include <ucontext.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <netinet/in.h>
#include <linux/membarrier.h>
#include <immintrin.h>
static void* run(void* p) { return p; }
int main(int argc, char** argv) {
    pthread_t t; void* h = dlopen(NULL, RTLD_LAZY);
    (void)argv;
    if (pthread_create(&t, NULL, run, NULL) != 0) return 2;
    pthread_join(t, NULL);
    return (h != NULL && sqrt((double)(argc * 4)) >= 2.0) ? 0 : 3;
}
EOF
    cat > "$w/t.cpp" <<'EOF'
#include <algorithm>
#include <array>
#include <atomic>
#include <cfenv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
int main() {
    std::vector<std::string> v{"a", "b"}; std::atomic<int> n{0}; std::mutex m;
    std::thread t([&] { std::lock_guard<std::mutex> g(m); n += (int)v.size(); });
    t.join();
    try { if (n != 2) throw std::runtime_error("x"); } catch (const std::exception&) { return 2; }
    return 0;
}
EOF
    : > "$w/log.txt"
    if ! "$cc" -O2 -march=x86-64-v3 "$w/t.c" -fuse-ld=lld -lpthread -ldl -lm -o "$w/t_c" >> "$w/log.txt" 2>&1; then
        printf 'a C program cannot be built: %s' "$(lh_why "$w/log.txt")"; return 1
    fi
    if ! "$cxx" -std=c++17 -O2 -march=x86-64-v3 "$w/t.cpp" -fuse-ld=lld -lpthread -o "$w/t_cpp" >> "$w/log.txt" 2>&1; then
        printf 'a C++ program cannot be built: %s' "$(lh_why "$w/log.txt")"; return 1
    fi
    if ! "$w/t_c" >> "$w/log.txt" 2>&1; then printf 'the C test program was built but does not run: %s' "$(lh_why "$w/log.txt")"; return 1; fi
    if ! "$w/t_cpp" >> "$w/log.txt" 2>&1; then printf 'the C++ test program was built but does not run: %s' "$(lh_why "$w/log.txt")"; return 1; fi
    return 0
}

# lh_advice <the port's folder>: what a player can do when this system cannot build C/C++ programs
lh_advice() {
    cat <<EOF
  What to do:
  - Ubuntu/Debian: sudo apt install build-essential      Fedora: sudo dnf install gcc-c++ glibc-devel
    Arch: sudo pacman -S base-devel                      then run ./setup.sh again.
  - SteamOS and other systems whose system folders are read-only: build and play inside a Distrobox
    container (Distrobox comes with SteamOS 3.5 and later). In a terminal (on the Steam Deck: Desktop Mode, Konsole):
        distrobox create --name inf2 --image ubuntu:24.04
        distrobox enter inf2
        sudo apt update && sudo apt install -y build-essential python3-venv curl xz-utils unzip libvulkan1 mesa-vulkan-drivers libsdl2-2.0-0
        cd "$1" && ./setup.sh
    The game is then built with the container's libraries: start it from the container as well:
        distrobox enter inf2 -- "$1/setup.sh"
    (as a Steam shortcut: target distrobox, launch options: enter inf2 -- "$1/setup.sh")
EOF
}
