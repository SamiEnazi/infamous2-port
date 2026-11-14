#!/bin/bash
# Play the native Linux build of inFAMOUS 2 (tools/play.ps1's Linux counterpart).
#   tools/play.sh [--fullscreen] [--volume 0-100] [--fps N] [--spu-lighting] [--no-log] [--no-hud]
#                 [--no-kbm] [--trace] [--exe <path>]
# In the game: F1 FPS counter, F2 (or hold L3+R3) the PC settings menu, Alt+Enter / F11 fullscreen,
# F9 marks a slow moment in the session log. The menu saves to ~/.config/infamous2-port/settings.ini
# ($XDG_CONFIG_HOME); --fps / --volume / --fullscreen given here win over it for this session.
# Controls: any controller SDL knows (buttons by position, as on a PlayStation pad); keyboard + mouse
# (WASD, mouse = camera after a click in the window, right button = aim, left = R1, Space = Cross,
# Shift = Circle, F = Square, Q = Triangle, Enter = START, Esc = PC menu). Remapping: input.toml in
# the project folder (made on the first start from engine/docs/input.example.toml).
# Every session gets play/<date-time>/ (stdout.log, stderr.log, playlog.tsv, summary.txt); your
# saves (game/gamedata/dev_hdd0/home/00000001/savedata) are copied to saves-backup/<date-time>/
# before each session when they changed (the newest 20 copies are kept).
set -u
root="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
exe="$root/work/port/build/Infamous2Recomp"
fullscreen=0; volume=""; fps=""; spulight=0; nolog=0; nohud=0; nokbm=0; trace=0
while [ $# -gt 0 ]; do
    case "$1" in
        --fullscreen) fullscreen=1 ;;
        --volume) volume="$2"; shift ;;
        --fps) fps="$2"; shift ;;
        --spu-lighting) spulight=1 ;;
        --no-log) nolog=1 ;;
        --no-hud) nohud=1 ;;
        --no-kbm) nokbm=1 ;;
        --trace) trace=1 ;;
        --exe) exe="$2"; shift ;;
        -h|--help) sed -n '2,15p' "$0"; exit 0 ;;
        *) echo "unknown option $1 (--help)"; exit 2 ;;
    esac
    shift
done
[ -x "$exe" ] || { echo "not built yet: $exe (tools/setup.sh, or python3 -I tools/rebuild.py)"; exit 1; }
game="$root/game"

# Your game folder (GitHub issue #13): game/disc is a link to it with its full path. A game folder kept
# inside the port's folder (or beside it) keeps the OLD path in the link when the port's folder is renamed
# or moved: the game then shows its intro logos and a black screen. Find it at the new place and link it
# again; else say so.
if [ -L "$game/disc" ] && [ ! -f "$game/disc/PS3_GAME/PARAM.SFO" ]; then
    old="$(readlink "$game/disc")"; old="${old%/}"; found=""
    for base in "$root" "$(dirname "$root")"; do
        tail="${old#/}"
        while [ -z "$found" ] && [[ "$tail" == */* ]]; do
            tail="${tail#*/}"
            if [ "$base/$tail" != "$old" ] && [ -f "$base/$tail/PS3_GAME/PARAM.SFO" ]; then found="$base/$tail"; fi
        done
        [ -n "$found" ] && break
    done
    if [ -n "$found" ]; then
        rm -f "$game/disc" && ln -s "$found" "$game/disc"      # the link only, never your game
        echo "Your game folder moved (it was $old): game/disc links to $found now."
    else
        echo "Your game folder is not where it was: game/disc still points to $old, and that folder is gone."
        echo "Move it back there, or run tools/setup.sh again with --disc <your game folder>."
        exit 1
    fi
fi
# The game's files (GitHub #9/#13): the main archives must be there and start with "PSAR" (a copy still
# encrypted, or one missing core/music.psarc, ended in a black screen after the intro).
psarc="$game/disc/PS3_GAME/USRDIR/cache/psarc"
if [ -d "$game/disc/PS3_GAME" ]; then
    for f in core.psarc music.psarc; do
        if [ ! -f "$psarc/$f" ]; then
            echo "Your game folder is missing some of the game's files: $f (in PS3_GAME/USRDIR/cache/psarc)."
            echo "Copy the complete game folder again (the one with PS3_GAME), then start the game again."
            exit 1
        fi
    done
    for f in core.psarc music.psarc install1/infamous1.psarc_s install2/infamous2.psarc_s; do
        if [ -f "$psarc/$f" ] && [ "$(head -c 4 "$psarc/$f")" != "PSAR" ]; then
            echo "Your game's files can't be read: $f doesn't start like a game archive."
            echo "The copy is still encrypted or damaged. The port needs a decrypted copy of the game folder,"
            echo "the same kind RPCS3 uses for a disc game."
            exit 1
        fi
    done
fi
out="$root/play/$(date +%Y%m%d-%H%M%S)"
mkdir -p "$out" "$game/gamedata"

# --- saves: a copy before every session in which they changed ---------------------------------
saves="$game/gamedata/dev_hdd0/home/00000001/savedata"
bk="$root/saves-backup"
if [ -d "$saves" ] && [ -n "$(ls -A "$saves" 2>/dev/null)" ]; then
    sig="$(cd "$saves" && find . -type f -printf '%p|%s|%T@\n' | sort)"
    last="$(ls -1d "$bk"/*/ 2>/dev/null | sort | tail -1)"
    if [ -z "$last" ] || [ "$sig" != "$(cat "${last}files.txt" 2>/dev/null)" ]; then
        dst="$bk/$(date +%Y%m%d-%H%M%S)"
        mkdir -p "$dst" && cp -a "$saves/." "$dst/" && printf '%s' "$sig" > "$dst/files.txt" && echo "Saves backed up to $dst"
        ls -1d "$bk"/*/ | sort -r | tail -n +21 | while read -r d; do rm -rf -- "$d"; done
    fi
fi

# --- controls file and mods folder --------------------------------------------------------------
toml="$root/input.toml"
if [ ! -f "$toml" ] && [ -f "$root/engine/docs/input.example.toml" ]; then
    cp "$root/engine/docs/input.example.toml" "$toml" && echo "Made input.toml (your controls) from the example."
fi
mods="$root/mods"
mkdir -p "$mods"
[ -f "$mods/README.txt" ] || cat > "$mods/README.txt" <<'EOF'
inFAMOUS 2 mods go here: one folder per mod, the folder with the mod's mod.json in it.

Mods made for the inFAMOUS Mod Manager work as they are: unzip the mod into this folder
(e.g. mods/Red Jacket/mod.json) and start the game. Your game files are never changed: the port
hands the game the mod's files in their place.

In the game, F2 > Mods lists them: turn each one on or off and pick its options there. Changes apply
the next time the game starts ("Restart to apply" does that at once).
EOF

flash="$game/dev_flash"
[ -d "$flash/sys" ] || flash="${PORT_DEV_FLASH:-${XDG_CONFIG_HOME:-$HOME/.config}/rpcs3/dev_flash}"
defaults="PS3RECOMP_FPS_CAP=60;PS3_AUDIO_VOLUME=100;PS3RECOMP_RENDER_SCALE=1.5;PS3RECOMP_SOFT_SHADOWS=2;PS3RECOMP_SSAO=2;"
defaults+="PS3RECOMP_SSAO_STRENGTH=125;PS3RECOMP_LIGHT_DITHER=1;PS3RECOMP_FX_RES=full;PS3RECOMP_SHARPEN=0.1"
# Steam Deck (confirmed by a player, #24): its 1280x800 screen and small GPU -- the PS3's 720p, fullscreen (the menu's
# settings.ini still wins over these defaults)
if [ "$(cat /sys/devices/virtual/dmi/id/board_vendor 2>/dev/null)" = Valve ]; then
    defaults="${defaults/PS3RECOMP_RENDER_SCALE=1.5/PS3RECOMP_RENDER_SCALE=1.0};PS3RECOMP_VK_FULLSCREEN=1"
fi
export PS3_VFS_ROOT="$game/disc" PS3_HDD0_ROOT="$game/dev_hdd0" PS3_DEV_FLASH="$flash" \
       PS3_GAMEDATA_ROOT="$game/gamedata" PS3_TITLE="inFAMOUS 2" PS3_GAME_MODS_DIR="$mods" \
       PS3_LAUNCHER_RESTART=1 GCM_SEMA_ACQUIRE=1 GCM_SEMA_ACQUIRE_MS=200 GCM_PARK_GRACE_MS=1000 \
       PS3RECOMP_VK_WINDOW=1 PS3RECOMP_VK_GUEST_PROGRAMS=1 PS3_VERBOSE="${PS3_VERBOSE:-0}" \
       PS3_OVERLAY_DEFAULTS="$defaults" \
       SDL_GAMECONTROLLER_USE_BUTTON_LABELS=0 SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=1
if [ $spulight = 0 ]; then export PS3RECOMP_ENG_GPU_LIGHT=07000B80:07384B80 PS3RECOMP_ENG_GPU_SSAO=0F800000; fi
[ -f "$toml" ] && export PS3_INPUT_MAP="$toml"
[ $fullscreen = 1 ] && export PS3RECOMP_VK_FULLSCREEN=1
[ -n "$fps" ] && export PS3RECOMP_FPS_CAP="$fps"
[ -n "$volume" ] && export PS3_AUDIO_VOLUME="$volume"
[ $nohud = 1 ] && export PS3_OVERLAY_HUD=0
[ $nokbm = 1 ] && export PS3_KBM=0
[ $trace = 1 ] && export PS3_PAD_TRACE=1
[ $nolog = 0 ] && export PS3RECOMP_PLAYLOG="$out/playlog.tsv"
# An X screen without DRI3 (a headless machine's dummy screen, some remote desktops): Mesa's GPU drivers
# can present there only through their software path, else the game falls back to llvmpipe (the CPU).
if [ -z "${MESA_VK_WSI_DEBUG+x}" ] && [ -z "${WAYLAND_DISPLAY:-}" ] && command -v xdpyinfo >/dev/null &&
   xdpyinfo -queryExtensions 2>/dev/null | grep -q '^name of display' && ! xdpyinfo -queryExtensions 2>/dev/null | grep -q DRI3; then
    export MESA_VK_WSI_DEBUG=sw
fi

echo "Starting inFAMOUS 2... F1 = FPS counter, F2 = settings."
[ $nolog = 0 ] && echo "Press F9 in the game window whenever something feels slow or stutters. Session folder: $out"
t0=$(date +%s)
run=0
# F2 > Mods > "Restart to apply" ends the game with status 0x52455354 ("REST"; a Linux exit status
# keeps its low byte, 0x54) and the line "[restart] exit 0x52455354": started again here, same
# settings, same session folder; the game comes back at its title screen (Continue goes on).
while :; do
    run=$((run + 1))
    if [ $run -gt 1 ]; then
        echo "Restarting inFAMOUS 2 for the new mods (restart $((run - 1))): choose Continue at the title screen."
        mark="=== $(date +%H:%M:%S) restart $((run - 1)) (F2 > Mods: Restart to apply) ==="
        echo "$mark" >> "$out/stdout.log"; echo "$mark" >> "$out/stderr.log"
        [ -f "$out/playlog.tsv" ] && mv -f "$out/playlog.tsv" "$out/playlog.$((run - 1)).tsv"
        [ -f "$out/hitches.tsv" ] && mv -f "$out/hitches.tsv" "$out/hitches.$((run - 1)).tsv"
    fi
    n0=0; [ -f "$out/stderr.log" ] && n0=$(wc -l < "$out/stderr.log")
    (cd "$out" && exec "$exe" "$root/work/EBOOT.elf") >> "$out/stdout.log" 2>> "$out/stderr.log"
    rc=$?
    if [ $rc = 84 ] && tail -n +"$((n0 + 1))" "$out/stderr.log" | grep -q '^\[restart\] exit 0x52455354' && [ $run -lt 20 ]; then
        continue
    fi
    break
done
echo "Exited (code $rc) after $(( ($(date +%s) - t0) / 60 )) min. Logs: $out"
if [ $nolog = 0 ] && [ -f "$out/playlog.tsv" ] && command -v python3 >/dev/null; then
    cap=$(grep -oE 'frame cap (off|[0-9.]+)' "$out/stderr.log" | tail -1 | awk '{print $3}')
    [ "$cap" = off ] && cap=0
    python3 -I "$root/tools/playlog_report.py" "$out" --cap "${cap:-60}" 2>&1 || true
fi
exit $rc
