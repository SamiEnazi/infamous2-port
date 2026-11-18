# NixOS: an FHS environment for building and playing the port (nothing is installed system-wide).
# NixOS has no /usr/lib or /usr/include, so setup.sh's checks (ldconfig, the C++ headers) and the
# downloaded build tools need this Debian-like layout. Build it once, then run everything inside it:
#
#   nix-build tools/nixos/fhs-env.nix -o work/nixos-fhs
#   work/nixos-fhs/bin/infamous2-fhs -c './setup.sh'                 # build (first time / updates), then play
#   work/nixos-fhs/bin/infamous2-fhs -c 'exec bash tools/play.sh'    # play
#
# The desktop entry that setup.sh writes starts tools/play.sh directly; on NixOS change its Exec line to the
# second command (with absolute paths). Requires nix-ld or this environment for the downloaded LLVM/CMake.
{ pkgs ? import <nixpkgs> {} }:
pkgs.buildFHSEnv {
  name = "infamous2-fhs";
  targetPkgs = p: with p; [
    # build: C/C++ headers and libraries, python3 with venv, the tools setup.sh uses
    gcc gcc.cc gcc.cc.lib glibc glibc.dev binutils
    python3 curl gnutar xz unzip gzip which file
    libxml2 zlib ncurses
    # run: Vulkan, SDL2 (input, window, audio) and the desktop libraries an RPCS3 AppImage needs. RPCS3 is
    # optional: setup uses it only to decrypt when ps3tool can't (the digital version)
    vulkan-loader vulkan-headers SDL2
    libGL libglvnd libxkbcommon wayland libx11 libxext libxrandr libxi libxcursor
    libxcb xcb-util-cursor fontconfig freetype alsa-lib libpulseaudio pipewire udev dbus
  ];
  runScript = "bash";
}
