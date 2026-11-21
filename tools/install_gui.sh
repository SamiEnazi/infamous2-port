#!/usr/bin/env bash
# The double-click installer (Linux, Steam Deck Desktop Mode): "Install inFAMOUS 2.desktop" in the port's folder runs
# this. The same build as ./setup.sh (tools/setup.sh), in dialogs: zenity, else kdialog; in a terminal without them.
#   1. what it does, with the checkbox "Optimise for this PC (PGO)" (on, as in Setup.exe; tools/setup.sh --pgo)
#   2. your game folder: the one an earlier setup used or RPCS3's game list has (asked to confirm), else a folder picker
#   3. a progress window with setup's steps (Cancel stops it; everything it says is in work/logs/install.log)
#   4. the end: "Play inFAMOUS 2.desktop" in this folder (it starts tools/play.sh), and Play now
# Started again once the game is built: Play / Rebuild / Optimise for this PC (or Train again).
# Nothing outside this folder is changed but the menu entry setup makes (~/.local/share/applications; kept as it is
# when it starts another folder's game).
set -u
here="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
cd "$here" || exit 1
title="inFAMOUS 2 Setup"
exe="$here/work/port/build/Infamous2Recomp"
log="$here/work/logs/install.log"
pgo_note="about 25 minutes more, a few % faster"

ui=tty
if [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]; then
    if command -v zenity >/dev/null 2>&1; then ui=zenity
    elif command -v kdialog >/dev/null 2>&1; then ui=kdialog; fi
fi
if [ $ui = tty ] && [ ! -t 0 ]; then   # double-clicked, no zenity or kdialog: the text way in a terminal
    for t in konsole gnome-terminal xfce4-terminal xterm; do
        command -v $t >/dev/null 2>&1 || continue
        if [ $t = gnome-terminal ]; then exec $t -- bash "$0"; else exec $t -e bash "$0"; fi
    done
    exit 1
fi

# ------------------------------------------------------------------ dialogs
esc() { printf '%s' "$1" | sed -e 's/&/\&amp;/g' -e 's/</\&lt;/g' -e 's/>/\&gt;/g'; }   # zenity's text is Pango markup
msg() {   # msg info|error <text>
    case $ui in
        zenity) zenity --"$1" --title="$title" --width=560 --text="$(esc "$2")" 2>/dev/null ;;
        kdialog) if [ "$1" = error ]; then kdialog --title "$title" --error "$2"; else kdialog --title "$title" --msgbox "$2"; fi 2>/dev/null ;;
        *) printf '\n%s\n' "$2" >&2 ;;
    esac
    return 0
}
ask() {   # ask <text> <yes> <no>: 0 = yes
    case $ui in
        zenity) zenity --question --title="$title" --width=560 --text="$(esc "$1")" --ok-label="$2" --cancel-label="$3" 2>/dev/null ;;
        kdialog) kdialog --title "$title" --yesno "$1" --yes-label "$2" --no-label "$3" 2>/dev/null ;;
        *) local r; printf '\n%s\n' "$1" >&2; read -r -p "$2? [y/n] " r; case "$r" in [yY]*) return 0 ;; *) return 1 ;; esac ;;
    esac
}
menu() {   # menu <text> <tag> <label> [<tag> <label> ...]: prints the chosen tag (the first is preselected)
    local text="$1" on=TRUE k=on i=0 r; shift
    local z=() kd=() tags=()
    while [ $# -ge 2 ]; do
        z+=("$on" "$1" "$2"); kd+=("$1" "$2" "$k"); tags+=("$1"); i=$((i + 1))
        [ $ui = tty ] && printf '  %d. %s\n' "$i" "$2" >&2
        on=FALSE; k=off; shift 2
    done
    case $ui in
        zenity) zenity --list --radiolist --title="$title" --text="$(esc "$text")" --width=640 --height=340 \
                    --column="" --column=tag --column="" --hide-column=2 --print-column=2 --hide-header "${z[@]}" 2>/dev/null ;;
        kdialog) kdialog --title "$title" --radiolist "$text" "${kd[@]}" 2>/dev/null ;;
        *) printf '%s\n' "$text" >&2; read -r -p "Choose 1-$i: " r; [ "$r" -ge 1 ] 2>/dev/null && [ "$r" -le $i ] && printf '%s\n' "${tags[$((r - 1))]}" ;;
    esac
}
first_screen() {   # the intro with the PGO checkbox: prints "pgo" when ticked; 1 = cancelled
    local text="This builds inFAMOUS 2 from your own copy of the game, in this folder:
$here

It takes about 15-60 minutes and about 25 GB of free space while it builds. It copies your game into
this folder, gets its 1.04 update and the PS3 firmware files (from RPCS3 when it has them, else from
Sony's servers) and the build tools from GitHub (about 2 GB). Nothing is installed on the system.

Optimise for this PC: the game is compiled once more with what it learns while it plays by itself
in a window for about 9 minutes (sound off; please don't touch it). On a Steam Deck that made a busy
street 3 % faster (no change where the graphics thread sets the pace). If it fails, you still get
the normal build." opt="Optimise for this PC (PGO): $pgo_note"
    case $ui in
        zenity) zenity --list --checklist --title="$title" --text="$(esc "$text")" --width=700 --height=360 --ok-label=Install \
                    --column="" --column=tag --column="" --hide-column=2 --print-column=2 --hide-header TRUE pgo "$opt" 2>/dev/null ;;
        kdialog) kdialog --title "$title" --checklist "$text" pgo "$opt" on 2>/dev/null | tr -d '"' ;;
        *) printf '\n%s\n' "$text" >&2; ask "$opt" yes no && echo pgo; return 0 ;;
    esac
}
pick_folder() {   # prints the folder the player picked; 1 = cancelled
    local start="$HOME" d
    for d in "$HOME/Emulation/roms/ps3" /run/media/*/Emulation/roms/ps3 /run/media/*/*/Emulation/roms/ps3; do
        [ -d "$d" ] && { start="$d"; break; }
    done
    local text="Choose your inFAMOUS 2 game folder: the one that holds PS3_GAME"
    case $ui in
        zenity) zenity --file-selection --directory --title="$text" --filename="$start/" 2>/dev/null | tail -n 1 ;;
        kdialog) kdialog --title "$text" --getexistingdirectory "$start" 2>/dev/null | tail -n 1 ;;
        *) read -r -p "$text (type or drag it here): " d
           d="${d#"${d%%[![:space:]]*}"}"; d="${d%"${d##*[![:space:]]}"}"
           case "$d" in \'*\') d="${d#\'}"; d="${d%\'}" ;; \"*\") d="${d#\"}"; d="${d%\"}" ;; esac
           [ -n "$d" ] && printf '%s\n' "$d" ;;
    esac
}
game_folder() {   # game_folder <picked folder>: the folder holding PS3_GAME (it, its parent, or its one game subfolder)
    local d="${1%/}" c n=0 one=""
    [ -f "$d/PS3_GAME/PARAM.SFO" ] && { printf '%s\n' "$d"; return 0; }
    [ -f "$(dirname "$d")/PS3_GAME/PARAM.SFO" ] && [ "$(basename "$d")" = PS3_GAME ] && { dirname "$d"; return 0; }
    for c in "$d"/*/; do [ -f "$c/PS3_GAME/PARAM.SFO" ] && { n=$((n + 1)); one="${c%/}"; }; done
    [ $n = 1 ] && { printf '%s\n' "$one"; return 0; }
    return 1
}

# ------------------------------------------------------------------ the progress window
# Step weights (percent of the bar), from a clean install on a Steam Deck (2026-11-21: 50 min with PGO, the steps 0, 2,
# 4, 0, 0, 0.4, 25, 9, 10, 0 min); the "  progress: N%" lines move the bar inside the long steps (rebuild.py's parts in
# step 7, the training in step 8 of 10, the optimised compile in step 9 of 10).
weights() {   # weights <steps>: "w1 w2 ..."
    if [ "$1" = 10 ]; then echo "1 4 8 0 0 1 48 18 20 0"; else echo "1 7 14 0 0 1 77 0"; fi
}
# rebuild.py's parts inside step 7: where each starts and how much of the step it takes (percent; the translation of
# the game's code is about half of it, the compile the other half)
part_start() {
    case "$1" in inputs|port|firmware) echo 0 ;; ppu) echo 1 ;; spu) echo 46 ;; jobs) echo 47 ;; compile) echo 52 ;; *) echo -1 ;; esac
}
part_size() { case "$1" in firmware|spu) echo 1 ;; ppu) echo 45 ;; jobs) echo 5 ;; compile) echo 48 ;; *) echo 0 ;; esac; }

zpid=""; zfd=""; kref=""; qd=""
ui_open() {
    case $ui in
        zenity)
            coproc ZEN { zenity --progress --title="$title" --width=640 --percentage=0 --auto-close \
                             --text="Starting..." 2>/dev/null; }
            zpid=$ZEN_PID; zfd=${ZEN[1]} ;;
        kdialog)
            qd="$(command -v qdbus6 || command -v qdbus || true)"
            if [ -n "$qd" ]; then
                kref="$(kdialog --title "$title" --progressbar "Starting..." 100 2>/dev/null)"
                $qd $kref showCancelButton true >/dev/null 2>&1
            fi ;;
    esac
}
ui_set() {   # ui_set <percent> <text>
    case $ui in
        zenity) local t; t="$(esc "$2")"; t="${t//$'\n'/\\n}"   # (one line: zenity turns "\n" into a line break)
                [ -n "$zpid" ] && kill -0 "$zpid" 2>/dev/null && printf '%s\n# %s\n' "$1" "$t" >&"$zfd" 2>/dev/null ;;
        kdialog) if [ -n "$kref" ]; then $qd $kref Set "" value "$1" >/dev/null 2>&1; $qd $kref setLabelText "$2" >/dev/null 2>&1
                 fi ;;
    esac
    return 0
}
ui_cancelled() {   # 0 = the player pressed Cancel (or closed the window)
    case $ui in
        zenity) [ -n "$zpid" ] && ! kill -0 "$zpid" 2>/dev/null ;;
        kdialog) [ -n "$kref" ] && [ "$($qd $kref wasCancelled 2>/dev/null)" = true ] ;;
        *) return 1 ;;
    esac
}
ui_close() {
    case $ui in
        zenity) if [ -n "$zpid" ]; then exec {zfd}>&- 2>/dev/null; kill "$zpid" 2>/dev/null; wait "$zpid" 2>/dev/null; zpid=""; fi ;;
        kdialog) [ -n "$kref" ] && $qd $kref close >/dev/null 2>&1; kref="" ;;
    esac
    return 0
}

run_setup() {   # run_setup <setup.sh options>: setup in the background, its steps in the progress window; 0 = built
    mkdir -p "$here/work/logs"
    if [ $ui = tty ]; then PORT_SETUP_NO_BOX=1 bash "$here/setup.sh" --setup "$@" 2>&1 | tee "$log"; return "${PIPESTATUS[0]}"; fi
    : > "$log"
    PORT_SETUP_NO_BOX=1 setsid bash "$here/setup.sh" --setup "$@" > "$log" 2>&1 < /dev/null &
    local spid=$! n=0 m line steps=8 step=0 stext="Starting" base=0 w=() part="" detail="" note="" pct=0 t0 rc
    t0=$(date +%s)
    trap '' PIPE
    ui_open
    while kill -0 "$spid" 2>/dev/null; do
        sleep 1
        m=$(wc -l < "$log")
        while [ $n -lt $m ]; do
            n=$((n + 1)); line="$(sed -n "${n}p" "$log")"
            case "$line" in
                "== ["*/*"] "*)
                    step="${line#== [}"; steps="${step#*/}"; steps="${steps%%]*}"; step="${step%%/*}"
                    stext="${line#*] }"; stext="${stext%   (*min so far)}"; detail=""; note=""; part=""
                    read -r -a w <<< "$(weights "$steps")"
                    base=0; local i; for ((i = 1; i < step; i++)); do base=$((base + ${w[$((i - 1))]:-0})); done
                    pct=$base ;;
                "["*"]")   # rebuild.py: one of its parts begins
                    part="${line#[}"; part="${part%]}"
                    if [ "$(part_start "$part")" -ge 0 ]; then
                        pct=$((base + ${w[$((step - 1))]:-0} * $(part_start "$part") / 100))
                        [ "$steps" = 10 ] && [ "$step" = 9 ] && pct=$base   # (step 9 of 10 is only the compile)
                    fi ;;
                "  progress: "*)
                    detail="${line#  progress: }"
                    local p="${detail%%%*}"
                    if [ "$p" -ge 0 ] 2>/dev/null; then
                        local ws=${w[$((step - 1))]:-0} s0=0 sz=100
                        if [ -n "$part" ] && [ "$(part_start "$part")" -ge 0 ] && ! { [ "$steps" = 10 ] && [ "$step" = 9 ]; }; then
                            s0=$(part_start "$part"); sz=$(part_size "$part")
                        fi
                        pct=$((base + ws * (s0 + sz * p / 100) / 100))
                    fi ;;
                "  player-note: "*) note="${line#  player-note: }" ;;
            esac
        done
        ui_set "$pct" "Step $step of $steps: ${stext}${note:+
$note}${detail:+
$detail}
($(( ($(date +%s) - t0) / 60 )) min so far)"
        if ui_cancelled; then
            if ask "Stop the installation? What is done so far stays: run the installer again to go on." "Stop it" "Go on"; then
                kill -TERM -- "-$spid" 2>/dev/null; sleep 2; kill -KILL -- "-$spid" 2>/dev/null
                wait "$spid" 2>/dev/null; ui_close
                echo "== FAILED: stopped by the player" >> "$log"
                return 1
            fi
            ui_open; ui_set "$pct" "Step $step of $steps: $stext"
        fi
    done
    wait "$spid"; rc=$?
    ui_close
    return $rc
}

play_entry() {   # "Play inFAMOUS 2.desktop" in this folder (%k: its own place, so the folder can be moved)
    local f="$here/Play inFAMOUS 2.desktop" icon="$here/work/icon/icon0.png"
    [ -f "$icon" ] || icon=applications-games
    cat > "$f" <<EOF
[Desktop Entry]
Type=Application
Name=Play inFAMOUS 2
Comment=inFAMOUS 2, natively on Linux (your own copy, recompiled)
Exec=sh -c 'exec bash "\$(dirname "\$1")/tools/play.sh"' sh %k
Icon=$icon
Terminal=false
Categories=Game;
EOF
    chmod +x "$f"
    command -v gio >/dev/null 2>&1 && gio set "$f" metadata::trusted true 2>/dev/null
    return 0
}
play() { play_entry; cd "$here" && exec bash "$here/tools/play.sh"; }

# ------------------------------------------------------------------ what to do
built=0; [ -x "$exe" ] && [ -f "$here/work/port/build/libInfamous2Game.so" ] && built=1
current="$(tail -n 1 "$here/port-version.txt" 2>/dev/null)"
pgo_state="$(cat "$here/work/setup-pgo" 2>/dev/null)"
args=()
if [ $built = 1 ]; then
    now="Built the normal way."
    case "$pgo_state" in "used ("*) now="Optimised for this PC (${pgo_state#used (}"; now="${now%)})." ;;
                         "not used: "*) now="Built the normal way: Optimise for this PC didn't work last time
(${pgo_state#not used: })." ;; esac
    opt=(pgo "Optimise for this PC (PGO): $pgo_note")
    case "$pgo_state" in "used ("*) opt=(retrain "Train again: optimise anew for this PC (about 45 minutes)") ;; esac
    upd=""
    [ "$(cat "$here/work/setup-built-from" 2>/dev/null)" != "$current" ] && upd="A new version of the port is in this folder: Rebuild or Optimise builds it (your saves and settings stay).
"
    choice="$(menu "inFAMOUS 2 is installed in this folder.
$now
${upd}What now?" \
              play "Play" rebuild "Rebuild the normal way (after an update, or to repair it)" "${opt[@]}")" || exit 0
    case "$choice" in
        play) play ;;
        rebuild) args=(--no-pgo) ;;
        pgo) args=(--pgo) ;;
        retrain) rm -f "$here"/work/pgo-setup/setup-*.profdata; args=(--pgo) ;;
        *) exit 0 ;;
    esac
else
    sel="$(first_screen)" || exit 0
    if [ "$sel" = pgo ]; then args=(--pgo); else args=(--no-pgo); fi
fi

# setup's menu entry (~/.local/share/applications/infamous2-port.desktop) is kept when it starts another folder's game
entry="${XDG_DATA_HOME:-$HOME/.local/share}/applications/infamous2-port.desktop"
menu_too=", or find inFAMOUS 2 in the application menu"
if [ -f "$entry" ] && ! grep -qxF "Path=$here" "$entry"; then args+=(--no-desktop); menu_too=""; fi

disc="$(bash "$here/setup.sh" --find-disc 2>/dev/null)"
if [ -n "$disc" ] && [ $built = 0 ] && ! ask "Your game: $disc

Use this folder?" "Use it" "Choose another"; then disc=""; fi
while [ -z "$disc" ]; do
    picked="$(pick_folder)" || exit 0
    [ -z "$picked" ] && exit 0
    disc="$(game_folder "$picked")" || { msg error "There is no PS3_GAME folder in $picked. Choose the folder that holds PS3_GAME (your disc's files)."; disc=""; }
done

if run_setup --disc "$disc" "${args[@]}"; then
    play_entry
    pgo_line="$(sed -n 's/^  pgo: //p' "$log" | tail -n 1)"
    mins="$(sed -n 's/^== done in \([0-9]*\) min.*/\1/p' "$log" | tail -n 1)"
    case "$pgo_line" in
        "used ("*) how="Ready to play, optimised for this PC." ;;
        "not used: "*) how="Ready to play. Optimise for this PC didn't work this time (${pgo_line#not used: }), so the game was built the normal way." ;;
        *) how="Ready to play." ;;
    esac
    if ask "$how${mins:+ (setup took $mins min)}

To play later: double-click \"Play inFAMOUS 2\" in this folder ($here)${menu_too}.
In the game: F1 = FPS counter, F2 = settings." "Play now" "Close"; then play; fi
else
    why="$(sed -n 's/^== FAILED: //p' "$log" | tail -n 1)"
    miss="$(sed -n 's/^  MISS  //p' "$log")"   # step 1's checks that failed
    msg error "Setup stopped: ${why:-see the log}.${miss:+

$miss}

What is done so far stays: run the installer again to go on. Everything setup said is in $log"
    exit 1
fi
