#!/usr/bin/env bash
# inFAMOUS 2 native port -- Linux: one script for everything (launchers such as Quiver run this).
#   ./setup.sh                    first time: find your game, build it (15-60 min), then play;
#                                 after that: just play
#   ./setup.sh --setup            build (or rebuild after an update) without playing
#   ./setup.sh --play [...]       play only (tools/play.sh and its options)
#   ./setup.sh --disc <folder> [tools/setup.sh options]   say where your game folder is (the one holding PS3_GAME)
#   ./setup.sh --prepare --disc <folder> [--dry-run]   for launchers (recomplauncher.json, as Setup.exe --prepare):
#                                 build only, no message boxes, no desktop entry, Setup.exe's exit codes
#                                 (71 no PS3_GAME, 79 the build failed: play/setup.log); --game = --disc
# The game is built from YOUR copy, here, once: nothing of the game is in this download. RPCS3 is not needed: the
# game's update and the PS3 firmware files come from Sony's own servers (or from RPCS3, when it has them).
# Your game folder is found in RPCS3's game list (games.yml) when you added it there; otherwise you are asked.
set -u -o pipefail
here="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
cd "$here" || exit 1
exe="$here/work/port/build/Infamous2Recomp"
stamp="$here/work/setup-built-from"
current="$(cat "$here/port-version.txt" 2>/dev/null | tail -n 1)"

prepare=0; dry=0   # --prepare: a launcher's setup step (see the top)

say() { printf '%s\n' "$*"; }
gui_msg() {   # a message box when there is no terminal (a launcher started us), else the terminal
    if [ -t 1 ] || [ $prepare = 1 ]; then say "$1"; return; fi   # --prepare: the launcher shows the exit code's message
    # (zenity's text is Pango markup: a "&" or "<" in a folder name would hide the whole message)
    local m; m="$(printf '%s' "$1" | sed -e 's/&/\&amp;/g' -e 's/</\&lt;/g' -e 's/>/\&gt;/g')"
    if command -v zenity >/dev/null 2>&1; then zenity --info --title="inFAMOUS 2" --width=520 --text="$m" 2>/dev/null
    elif command -v kdialog >/dev/null 2>&1; then kdialog --title "inFAMOUS 2" --msgbox "$1" 2>/dev/null
    else say "$1"; fi
}

# The game folder: RPCS3's games.yml (AppImage/EmuDeck and Flatpak), the disc versions this port knows.
find_disc() {
    local y line id path
    for y in "$HOME/.config/rpcs3/games.yml" "$HOME/.var/app/net.rpcs3.RPCS3/config/rpcs3/games.yml"; do
        [ -f "$y" ] || continue
        while IFS= read -r line; do
            case "$line" in
                BCES01143:*|BCES01144:*|BCES01229:*|BCUS98125:*)
                    path="${line#*:}"; path="${path#"${path%%[![:space:]]*}"}"
                    path="${path%\"}"; path="${path#\"}"; path="${path%\'}"; path="${path#\'}"; path="${path%/}"
                    if [ -f "$path/PS3_GAME/PARAM.SFO" ]; then printf '%s\n' "$path"; return 0; fi ;;
            esac
        done < "$y"
    done
    return 1
}
# Prints the chosen folder, and nothing else, on stdout: the caller reads it with $(ask_disc). The question and
# every message go to stderr (GitHub #54: the question was printed on stdout, so it became the "folder").
ask_disc() {
    local d=""
    local text="Choose your inFAMOUS 2 game folder: the one that holds PS3_GAME (your Europe or US disc's files)."
    if [ -t 0 ]; then
        say "$text" >&2; read -r -p "Game folder: " d
        # typed or dragged into the terminal: quotes around it, a trailing space, ~ for the home folder
        d="${d#"${d%%[![:space:]]*}"}"; d="${d%"${d##*[![:space:]]}"}"
        case "$d" in \'*\') d="${d#\'}"; d="${d%\'}" ;; \"*\") d="${d#\"}"; d="${d%\"}" ;; esac
        case "$d" in "~") d="$HOME" ;; "~/"*) d="$HOME/${d#"~/"}" ;; esac
    elif command -v zenity >/dev/null 2>&1; then
        d="$(zenity --file-selection --directory --title="$text" 2>/dev/null | tail -n 1)"
    elif command -v kdialog >/dev/null 2>&1; then
        d="$(kdialog --getexistingdirectory "$HOME" --title "$text" 2>/dev/null | tail -n 1)"
    fi
    [ "$d" = "/" ] || d="${d%/}"
    [ -z "$d" ] && return 1
    [ -f "$d/PS3_GAME/PARAM.SFO" ] && { printf '%s\n' "$d"; return 0; }
    [ -f "$(dirname "$d")/PS3_GAME/PARAM.SFO" ] && { dirname "$d"; return 0; }   # PS3_GAME itself was picked
    say "There is no PS3_GAME/PARAM.SFO in: $d" >&2
    return 1
}

build() {
    local args=("$@") have_disc=0 a
    for a in "${args[@]}"; do [ "$a" = "--disc" ] && have_disc=1; done
    if [ $have_disc = 0 ]; then
        local disc=""
        # the folder an earlier setup linked (game/disc) is used again
        if [ -f "$here/game/disc/PS3_GAME/PARAM.SFO" ]; then disc="$(readlink -f "$here/game/disc")"; fi
        [ -z "$disc" ] && disc="$(find_disc)"
        [ -z "$disc" ] && disc="$(ask_disc)"
        if [ -z "$disc" ]; then
            gui_msg "Setup needs your inFAMOUS 2 game folder (the one holding PS3_GAME). Run: ./setup.sh --disc <your game folder>"
            exit 2
        fi
        say "Your game folder: $disc"
        args=(--disc "$disc" "${args[@]}")
    fi
    mkdir -p "$here/work/logs" "$here/play"
    local failcode=1; [ $prepare = 1 ] && { failcode=79; args+=(--no-desktop); }
    say "Building inFAMOUS 2 from your copy (once; 15-60 minutes). The log: work/logs/setup.log"
    if ! bash "$here/tools/setup.sh" "${args[@]}" 2>&1 | tee "$here/work/logs/setup.log"; then
        cp -f "$here/work/logs/setup.log" "$here/play/setup.log" 2>/dev/null   # a launcher's Logs button opens play/
        gui_msg "Setup stopped. The reason is at the end of $here/work/logs/setup.log"
        exit $failcode
    fi
    cp -f "$here/work/logs/setup.log" "$here/play/setup.log" 2>/dev/null
    [ -x "$exe" ] || { gui_msg "Setup finished without the game program. See $here/work/logs/setup.log"; exit $failcode; }
    printf '%s\n' "$current" > "$stamp"
}

# Launchers start us the way they start Setup.exe on Windows (GitHub #44: "unknown option --prepare"):
# --prepare anywhere on the line, --game for --disc, --dry-run; --pgo is Windows-only and ignored.
for a in "$@"; do [ "$a" = "--prepare" ] && prepare=1; done
if [ $prepare = 1 ]; then
    rest=()
    while [ $# -gt 0 ]; do
        case "$1" in
            --prepare|--pgo) ;;
            --dry-run) dry=1 ;;
            --game) rest+=(--disc) ;;
            *) rest+=("$1") ;;
        esac
        shift
    done
    set -- ${rest[@]+"${rest[@]}"}
    disc=""; prev=""
    for a in "$@"; do [ "$prev" = "--disc" ] && disc="${a%/}"; prev="$a"; done
    if [ -z "$disc" ] && [ -f "$here/game/disc/PS3_GAME/PARAM.SFO" ]; then disc="$(readlink -f "$here/game/disc")"; set -- --disc "$disc" "$@"; fi
    if [ -z "$disc" ] || [ ! -f "$disc/PS3_GAME/PARAM.SFO" ]; then
        say "No PS3_GAME/PARAM.SFO in the game folder: ${disc:-(none given)}. Pick the folder that holds PS3_GAME."
        exit 71
    fi
    if [ $dry = 1 ]; then say "Dry run: the game folder is $disc. Nothing was built."; exit 0; fi
    build "$@"
    say "Done."
    exit 0
fi

mode="auto"
case "${1:-}" in
    --setup) mode="setup"; shift ;;
    --play) mode="play"; shift ;;
    -h|--help) sed -n '2,13p' "$0"; exit 0 ;;
    "") ;;
    *) mode="setup-then-play" ;;   # tools/setup.sh options, e.g. --disc <folder>
esac

case "$mode" in
    play)
        [ -x "$exe" ] || { say "The game isn't built yet: run ./setup.sh first."; exit 80; }   # Setup.exe --play's code
        exec bash "$here/tools/play.sh" "$@" ;;
    setup) build "$@"; say "Done. Play with ./setup.sh (or ./setup.sh --play)."; exit 0 ;;
    setup-then-play) build "$@"; exec bash "$here/tools/play.sh" ;;
    auto)
        # built, and from this version of the port: play; else build first (a new download, or an update)
        if [ -x "$exe" ] && [ "$(cat "$stamp" 2>/dev/null)" = "$current" ]; then exec bash "$here/tools/play.sh"; fi
        build; exec bash "$here/tools/play.sh" ;;
esac
