#!/usr/bin/env bash
# Play the native Linux build of inFAMOUS 2 (tools/play.ps1's Linux counterpart).
#   tools/play.sh [--fullscreen] [--volume 0-100] [--fps N] [--spu-lighting] [--no-log] [--no-hud]
#                 [--no-kbm] [--trace] [--exe <path>] [--wayland]
# In the game: F1 FPS counter, F2 (or hold L3+R3) the PC settings menu, Alt+Enter / F11 fullscreen,
# F9 marks a slow moment in the session log. The menu saves to settings.ini in the port's folder (v0.6.0: with
# mods.ini, spots.ini and the shader cache in work/shader_cache, so the folder can be moved or copied and plays the
# same; the files of ~/.config/infamous2-port are copied in once); --fps / --volume / --fullscreen given here win
# over it for this session.
# Controls: any controller SDL knows (buttons by position, as on a PlayStation pad); keyboard + mouse
# (WASD, mouse = camera after a click in the window, right button = aim, left = R1, Space = Cross,
# Shift = Circle, F = Square, Q = Triangle, Esc = START / pause; F2 or Alt+Tab frees the mouse). Remapping: input.toml in
# the project folder (made on the first start from engine/docs/input.example.toml).
# Every session gets play/<date-time>/ (stdout.log, stderr.log, playlog.tsv, summary.txt); your
# saves (game/gamedata/dev_hdd0/home/00000001/savedata) are copied to saves-backup/<date-time>/
# before each session when they changed (the newest 20 copies are kept).
set -u
root="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
exe="$root/work/port/build/Infamous2Recomp"
fullscreen=0; volume=""; fps=""; spulight=0; nolog=0; nohud=0; nokbm=0; trace=0; wayland=0
while [ $# -gt 0 ]; do
    case "$1" in
        --fullscreen) fullscreen=1 ;;
        --wayland) wayland=1 ;;   # a Wayland desktop: never start the game again through X11 (see x11_watch)
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

# DLC (GitHub #48, #56): the "dlc" folder. The game's unlock files (*.edat) put there, in it or in subfolders, are
# copied to where the game opens them (game/dev_hdd0/game/<ID>/USRDIR/dlc/ulf, lower-case names: the game opens
# lower-case), only when missing or different. tools/setup.sh copies RPCS3's the same way. Never stops the game.
mkdir -p "$root/dlc" 2>/dev/null
[ -f "$root/dlc/README.txt" ] || cp "$root/tools/dlc_readme.txt" "$root/dlc/README.txt" 2>/dev/null
for td in "$game"/dev_hdd0/game/*/; do
    td="${td%/}"
    case "$td" in *_INSTALL) continue ;; esac
    [ -f "$td/USRDIR/EBOOT.BIN" ] || continue
    ulf="$td/USRDIR/dlc/ulf"; nnew=0
    while IFS= read -r f; do
        name="$(basename "$f")"; dst="$ulf/${name,,}"
        if ! cmp -s "$f" "$dst"; then
            mkdir -p "$ulf" && rm -f "$dst" "$dst.dec" && cp "$f" "$dst" && chmod u+rw "$dst" && nnew=$((nnew + 1))
        fi
    done < <(find "$root/dlc" -type f -iname '*.edat' 2>/dev/null | sort)
    have=0; lic=0
    while IFS= read -r f; do
        have=$((have + 1))
        # the license kind in the file's header: 3 = free; 1 or 2 = bound to a license (.rap)
        if [ "$(head -c 3 "$f" 2>/dev/null)" = NPD ]; then
            case "$(od -An -tu1 -j11 -N1 "$f" 2>/dev/null | tr -d ' ')" in 1|2) lic=$((lic + 1)) ;; esac
        fi
    done < <(find "$td/USRDIR/dlc" -type f -iname '*.edat' 2>/dev/null)
    line="DLC: none in game/dev_hdd0/game/$(basename "$td")/USRDIR/dlc (optional: put your DLC's *.edat files in the dlc folder)"
    [ $have -gt 0 ] && line="DLC: $have unlock file(s) in game/dev_hdd0/game/$(basename "$td")/USRDIR/dlc"
    [ $nnew -gt 0 ] && line="$line ($nnew new from the dlc folder)"
    [ $lic -gt 0 ] && line="$line; $lic need their license (.rap): not supported yet"
    echo "$line"
done

flash="$game/dev_flash"
[ -d "$flash/sys" ] || flash="${PORT_DEV_FLASH:-${XDG_CONFIG_HOME:-$HOME/.config}/rpcs3/dev_flash}"
defaults="PS3RECOMP_FPS_CAP=60;PS3_AUDIO_VOLUME=100;PS3RECOMP_RENDER_SCALE=1.5;PS3RECOMP_SOFT_SHADOWS=2;PS3RECOMP_SSAO=2;"
defaults+="PS3RECOMP_SSAO_STRENGTH=125;PS3RECOMP_LIGHT_DITHER=1;PS3RECOMP_FX_RES=full;PS3RECOMP_SHARPEN=0.1"
# Steam Deck (confirmed by a player, #24): its 1280x800 screen and small GPU -- the PS3's 720p, fullscreen (the menu's
# settings.ini still wins over these defaults)
if [ "$(cat /sys/devices/virtual/dmi/id/board_vendor 2>/dev/null)" = Valve ]; then
    defaults="${defaults/PS3RECOMP_RENDER_SCALE=1.5/PS3RECOMP_RENDER_SCALE=1.0};PS3RECOMP_VK_FULLSCREEN=1"
fi
# everything in the port's folder (v0.6.0); an older version's files copied in once (they stay where they were)
if [ ! -f "$root/work/settings-moved-in.txt" ]; then
    oldcfg="${XDG_CONFIG_HOME:-$HOME/.config}/infamous2-port"; took=""
    for f in settings.ini mods.ini spots.ini; do
        if [ -f "$oldcfg/$f" ] && [ ! -f "$root/$f" ]; then cp -p "$oldcfg/$f" "$root/$f" && took="$took $f"; fi
    done
    oldsc="${XDG_CACHE_HOME:-$HOME/.cache}/ps3recomp/shader_cache"
    if [ ! -d "$root/work/shader_cache" ]; then
        for d in "$oldsc"/inFAMOUS*; do [ -d "$d" ] && cp -a "$d" "$root/work/shader_cache" && took="$took shader-cache" && break; done
    fi
    [ -n "$took" ] && echo "Your settings are kept in this folder now: copied in$took (those files stay where they were)."
    mkdir -p "$root/work" && echo "$(date '+%F %T')$took" > "$root/work/settings-moved-in.txt"
fi
[ -n "${PS3_OVERLAY_INI+x}" ] || export PS3_OVERLAY_INI="$root/settings.ini" PS3_CHEATS_SPOTS="$root/spots.ini" PS3_GAME_MODS_STATE="$root/mods.ini"
[ -n "${PS3RECOMP_VK_SHADER_CACHE+x}" ] || export PS3RECOMP_VK_SHADER_CACHE="$root/work/shader_cache"
if ! grep -qw avx2 /proc/cpuinfo 2>/dev/null; then
    echo "This PC's CPU has no AVX2, and the game is built for CPUs with it (Intel 4th gen / AMD Ryzen or newer)."; exit 1
fi
export PS3_VFS_ROOT="$game/disc" PS3_HDD0_ROOT="$game/dev_hdd0" PS3_DEV_FLASH="$flash" \
       PS3_GAMEDATA_ROOT="$game/gamedata" PS3_TITLE="inFAMOUS 2" PS3_GAME_MODS_DIR="$mods" \
       PS3_LAUNCHER_RESTART=1 GCM_SEMA_ACQUIRE=1 GCM_SEMA_ACQUIRE_MS=200 GCM_PARK_GRACE_MS=1000 \
       PS3RECOMP_VK_WINDOW=1 PS3RECOMP_VK_GUEST_PROGRAMS=1 PS3_VERBOSE="${PS3_VERBOSE:-0}" \
       PS3_OVERLAY_DEFAULTS="$defaults" \
       SDL_GAMECONTROLLER_USE_BUTTON_LABELS=0 SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=1
if [ $spulight = 0 ]; then export PS3RECOMP_ENG_GPU_LIGHT=07000B80:07384B80 PS3RECOMP_ENG_GPU_SSAO=0F800000; fi
[ -f "$toml" ] && export PS3_INPUT_MAP="$toml"
[ -f "$root/supporters.json" ] && export PS3_SUPPORTERS_FILE="$root/supporters.json"   # F2 > About: the list that came with this copy (nothing is fetched here)
[ $fullscreen = 1 ] && export PS3RECOMP_VK_FULLSCREEN=1
[ -n "$fps" ] && export PS3RECOMP_FPS_CAP="$fps"
[ -n "$volume" ] && export PS3_AUDIO_VOLUME="$volume"
[ $nohud = 1 ] && export PS3_OVERLAY_HUD=0
[ $nokbm = 1 ] && export PS3_KBM=0
[ $trace = 1 ] && export PS3_PAD_TRACE=1
[ $nolog = 0 ] && export PS3RECOMP_PLAYLOG="$out/playlog.tsv"
# Local fix (Steam Deck, KDE Plasma 6 Wayland): the Wayland surface reports more than 64 formats, the
# engine asks for 64, gets VK_INCOMPLETE and runs without a window (sound only). XWayland works.
[ -z "${SDL_VIDEODRIVER:-}" ] && [ -n "${WAYLAND_DISPLAY:-}" ] && export SDL_VIDEODRIVER=x11
# Local tuning (Steam Deck, 4 cores). Benchmarked 2026-11-16 at one city street from the same save,
# PGO build: 28.5 -> 30.9 fps. No busy-waiting in the GCM wait and the Vulkan worker (they take CPU
# time from the busy threads on 4 cores), one pipeline worker, a 0.25 ms frame clock (on Linux it
# sleeps its whole period; Windows wakes it early). Each can still be set from outside.
: "${PS3RECOMP_GCM_SPIN_US:=0}" "${PS3RECOMP_VK_TQ_SPIN_US:=0}" "${PS3RECOMP_VK_PIPE_THREADS:=1}" "${PS3_FRAMECLOCK_US:=250}"
export PS3RECOMP_GCM_SPIN_US PS3RECOMP_VK_TQ_SPIN_US PS3RECOMP_VK_PIPE_THREADS PS3_FRAMECLOCK_US
# (deck-merge) v0.6's native job runner: 4 pool threads (its default, physical cores - 2, is 2 here). Det bench at the
# plaza, 2026-11-18: 2 threads 37.8 fps (the main thread runs jobs in its waits: 26 ms/frame), 3 40.8, 4 41.0.
: "${PS3_NATIVE_JOBS_THREADS:=4}"; export PS3_NATIVE_JOBS_THREADS
# (deck-v063) v0.6.3's controller thread reads the pads every 1 ms by default: here every 4 ms. On the Deck's controller
# (SDL HIDAPI) a pass costs 40-55 us, so 1 ms = 32 ms of CPU a second (755 passes), 4 ms = 13 ms (234 passes), on the
# CPUs the main thread and the job pool share; street det bench with SDL pads on, guest 4-20 s, 4 runs each: 46.46 vs
# 47.12 fps (main 19.41 vs 19.14 ms). A pad call then copies a snapshot at most ~4 ms old (1 ms with upstream's default;
# v0.6.2 read the pads inside the call, at most every 1 ms), against a 17-20 ms frame. Windows keeps upstream's 1 ms.
: "${PS3_PAD_THREAD_US:=4000}"; export PS3_PAD_THREAD_US
# (deck-perf, step 6 / hd5, 2026-11-20) Two engine switches on by default here (both default off in the engine; each can
# still be set from outside, =0 = the old path). Measured in the combination (no-PGO modules, the store lift on, 6 rotated
# rounds of 5 configurations; docs/STEAM_DECK_PERF.md step 6): taking either one out of it was slower, never faster.
#   PS3_NATIVE_JOBS_PRIO=1: the job runner puts a manager whose last flush came from another PPU thread and whose jobs are
#   long (mean >= 250 us: here Edge alone) behind the main thread's jobs, with an 8 ms age limit. Without it, quick street
#   -0.76 % (SE 0.48), plaza -0.56 % (SE 1.01); alone it was street +1.25 % (SE 0.88), plaza +2.06 % (SE 0.63).
#   PS3RECOMP_ENG_R8=3: descriptor sets kept across frames + the fragment program structural memo (the same picture,
#   checked byte for byte). Without it, quick street -1.69 % (SE 0.45, RSX thread +0.60 ms), plaza -1.42 % (SE 0.92).
# With both and the store lift (Linux builds: tools/rebuild_win.py) + the mg5r profiles = build hd5c, against hd4: street
# +1.65 % (SE 0.46, guest 4-20 s) / +2.63 % (SE 1.11, full route), plaza +1.70 % (SE 0.47), standing 60 fps equal.
: "${PS3_NATIVE_JOBS_PRIO:=1}" "${PS3RECOMP_ENG_R8:=3}"; export PS3_NATIVE_JOBS_PRIO PS3RECOMP_ENG_R8
# The RSX (draw) thread alone on a core, every other thread on the rest (engine host_place.c, Linux):
# 30.5 -> 33.0 fps on the Deck, 34.7 with the job 210 native. PS3_CPU_PLACE=0 = the OS decides.
: "${PS3_CPU_PLACE:=rsxcore}"; export PS3_CPU_PLACE
# (deck-merge) v0.6's game module (libInfamous2Game.so) has initial-exec thread-locals: glibc needs room for them
# in its static TLS, else the engine starts itself again through /proc/self/exe with this tunable
# (runtime/module/ps3_module_host.c) and the process is then called "exe" (pgrep -x Infamous2Recomp, the bench's
# budget.py and the game-running checks miss it). Set here, the first start already has it.
case ":${GLIBC_TUNABLES:-}:" in
    *glibc.rtld.optional_static_tls=*) ;;
    *) export GLIBC_TUNABLES="${GLIBC_TUNABLES:+$GLIBC_TUNABLES:}glibc.rtld.optional_static_tls=1048576" ;;
esac
: "${PS3_STATIC_TLS_RETRY:=1}"; export PS3_STATIC_TLS_RETRY
# An X screen without DRI3 (a headless machine's dummy screen, some remote desktops): Mesa's GPU drivers
# can present there only through their software path, else the game falls back to llvmpipe (the CPU).
if [ -z "${MESA_VK_WSI_DEBUG+x}" ] && [ -z "${WAYLAND_DISPLAY:-}" ] && command -v xdpyinfo >/dev/null &&
   xdpyinfo -queryExtensions 2>/dev/null | grep -q '^name of display' && ! xdpyinfo -queryExtensions 2>/dev/null | grep -q DRI3; then
    export MESA_VK_WSI_DEBUG=sw
fi

# Built inside a Distrobox container (tools/setup.sh notes it) and started outside: the program may not find the
# container's libraries (a player's Steam Deck, GitHub #24: sound but no window). Said, not refused.
if [ -f "$root/work/built-in-container" ] && [ ! -f /run/.containerenv ] && [ ! -f /.dockerenv ] && [ -z "${CONTAINER_ID:-}" ]; then
    box="$(head -n 1 "$root/work/built-in-container")"; [ -n "$box" ] || box="<the container's name>"
    echo "Note: the game was built inside a container ($box). If it does not start or shows no window here, start it from there:"
    echo "  distrobox enter $box -- \"$root/setup.sh\""
fi
echo "Starting inFAMOUS 2... F1 = FPS counter, F2 = settings."
[ $nolog = 0 ] && echo "Press F9 in the game window whenever something feels slow or stutters. Session folder: $out"
t0=$(date +%s)
run=0
# A Wayland desktop where the game's window cannot be made (GitHub #54, #44: the game went on without one: sound,
# no picture, and nothing to close). The cause seen there is fixed in the engine; should another one drop the
# window, the game is started once more through X11 (XWayland), which worked for those players. Only when the
# choice is ours: not with --wayland, not when SDL_VIDEODRIVER is set, not without an X display.
# x11_watch reads this run's part of stderr.log for up to 90 s: the window's swapchain line = all is well;
# "window disabled" after SDL chose 'wayland' = stop the game (retry-x11 tells the loop below why).
x11_retry=0
if [ $wayland = 0 ] && [ -z "${SDL_VIDEODRIVER+x}" ] && [ -n "${WAYLAND_DISPLAY:-}" ] && [ -n "${DISPLAY:-}" ]; then x11_retry=1; fi
x11_watch() {
    local n0="$1" pid="" new i
    for i in $(seq 1 90); do
        sleep 1
        if [ -z "$pid" ]; then pid="$(cat "$out/game.pid" 2>/dev/null)"; [ -n "$pid" ] || continue; fi
        kill -0 "$pid" 2>/dev/null || return 0
        new="$(tail -n +"$((n0 + 1))" "$out/stderr.log" 2>/dev/null)"
        case "$new" in
            *"window disabled, continuing headless"*)
                case "$new" in *"SDL video driver 'wayland'"*)
                    : > "$out/retry-x11"; kill "$pid" 2>/dev/null
                    for i in 1 2 3 4 5; do sleep 1; kill -0 "$pid" 2>/dev/null || return 0; done
                    kill -9 "$pid" 2>/dev/null ;;
                esac
                return 0 ;;
            *"window swapchain "*) return 0 ;;
        esac
    done
}
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
    rm -f "$out/game.pid" "$out/retry-x11"; watch=""
    if [ $x11_retry = 1 ]; then x11_watch "$n0" & watch=$!; fi
    (cd "$out" && echo "$BASHPID" > "$out/game.pid" && exec "$exe" "$root/work/EBOOT.elf") >> "$out/stdout.log" 2>> "$out/stderr.log"
    rc=$?
    if [ -n "$watch" ]; then kill "$watch" 2>/dev/null; wait "$watch" 2>/dev/null; fi
    if [ -f "$out/retry-x11" ]; then
        rm -f "$out/retry-x11"; x11_retry=0; run=$((run - 1))
        export SDL_VIDEODRIVER=x11
        echo "The game could not open its window on Wayland: starting it again through X11 (XWayland)."
        echo "Please report it with $out/stderr.log; SDL_VIDEODRIVER=x11 tools/play.sh starts it this way at once."
        echo "=== $(date +%H:%M:%S) no window on Wayland: started again with SDL_VIDEODRIVER=x11 ===" >> "$out/stderr.log"
        continue
    fi
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
