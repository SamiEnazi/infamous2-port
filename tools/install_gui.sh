#!/usr/bin/env bash
# The double-click installer (Linux, Steam Deck Desktop Mode): the program "Install inFAMOUS 2" in the port's folder
# (tools/launcher/launcher.c: GNOME's file manager runs programs, not .desktop files from a folder) runs this, and so
# does "Play inFAMOUS 2" before the game is built; so does `bash tools/install_gui.sh`. The same build as ./setup.sh (tools/setup.sh), in dialogs: zenity, else kdialog; in a terminal without them.
#   1. what it does, with the checkbox "Optimise for this PC (PGO)" (on, as in Setup.exe; tools/setup.sh --pgo)
#   2. your game folder: the one an earlier setup used or RPCS3's game list has (asked to confirm), else a folder picker;
#      then how to use it: from where it is (the default: no copy, the folder has to stay there) or copied into this folder
#   3. a progress window with setup's steps (Cancel stops it; everything it says is in work/logs/install.log)
#   4. the end: Play now (later: the program "Play inFAMOUS 2" in this folder starts tools/play.sh)
# Started again once the game is built: Play / Rebuild / Optimise for this PC (or Train again) / Add to Steam.
# Add to Steam (tools/steam_shortcut.py): a non-Steam shortcut that starts tools/play.sh (Game Mode), with artwork from the
# game's own ICON0/PIC1; it only ever ADDS an entry, writes only while Steam is closed (it asks to close it), backs the
# file up first. Tests: PORT_STEAM_ROOT=<a copy of a Steam folder>, PORT_STEAM_CMD=<a stand-in for the steam program>,
# PORT_STEAM_TEST_RUNNING=<a file: Steam counts as running while it exists>.
# Nothing outside this folder is changed but the menu entry setup makes (~/.local/share/applications; kept as it is
# when it starts another folder's game).
set -u
here="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
cd "$here" || exit 1
# v0.6.3-deck2/deck3 kept .desktop files in this folder next to the programs of the same name: the programs replace
# them (only ours are removed: the name and the "sh %k" start line); the programs stay executable
for n in "Install inFAMOUS 2" "Play inFAMOUS 2"; do
    f="$here/$n.desktop"
    [ -f "$f" ] && grep -qxF "Name=$n" "$f" && grep -qF "sh %k" "$f" && rm -f "$f"
    [ -f "$here/$n" ] && [ ! -x "$here/$n" ] && chmod +x "$here/$n" 2>/dev/null
done
title="inFAMOUS 2 Setup"
exe="$here/work/port/build/Infamous2Recomp"
log="$here/work/logs/install.log"
pgo_note="about 15 minutes more, 7-9 % faster on a Steam Deck"
gb() { echo $(( ($1 + 536870912) / 1073741824 )); }   # bytes -> GB, rounded

ui=tty
if [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]; then
    if command -v zenity >/dev/null 2>&1; then ui=zenity
    elif command -v kdialog >/dev/null 2>&1; then ui=kdialog; fi
fi
if [ $ui = tty ] && [ ! -t 0 ]; then   # double-clicked, no zenity or kdialog: the text way in a terminal
    for t in konsole ptyxis kgx gnome-terminal xfce4-terminal mate-terminal tilix x-terminal-emulator alacritty kitty foot xterm; do
        command -v $t >/dev/null 2>&1 || continue
        case $t in
            ptyxis|kgx|gnome-terminal|tilix) exec $t -- bash "$0" ;;
            xfce4-terminal|mate-terminal) exec $t -x bash "$0" ;;
            kitty|foot) exec $t bash "$0" ;;
            *) exec $t -e bash "$0" ;;
        esac
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
    local text="$1" on=TRUE k=on i=0 r h; shift
    h=$(( 180 + 20 * $(printf '%s\n' "$text" | wc -l) + 30 * ($# / 2) )); [ $h -lt 340 ] && h=340
    local z=() kd=() tags=()
    while [ $# -ge 2 ]; do
        z+=("$on" "$1" "$2"); kd+=("$1" "$2" "$k"); tags+=("$1"); i=$((i + 1))
        [ $ui = tty ] && printf '  %d. %s\n' "$i" "$2" >&2
        on=FALSE; k=off; shift 2
    done
    case $ui in
        zenity) zenity --list --radiolist --title="$title" --text="$(esc "$text")" --width=700 --height=$h \
                    --column="" --column=tag --column="" --hide-column=2 --print-column=2 --hide-header "${z[@]}" 2>/dev/null ;;
        kdialog) kdialog --title "$title" --radiolist "$text" "${kd[@]}" 2>/dev/null ;;
        *) printf '%s\n' "$text" >&2; read -r -p "Choose 1-$i: " r; [ "$r" -ge 1 ] 2>/dev/null && [ "$r" -le $i ] && printf '%s\n' "${tags[$((r - 1))]}" ;;
    esac
}
first_screen() {   # the intro with the checkboxes: prints the ticked ones ("pgo", "steam"), one a line; 1 = cancelled
    local text="This builds inFAMOUS 2 from your own copy of the game, in this folder:
$here

It takes 15-60 minutes (about 25 on a Steam Deck) and about 10 GB of free space (25 GB if you let
it copy your game into this folder: you choose after picking your game folder). It gets the game's
1.04 update and the PS3 firmware files (from RPCS3 when it has them, else from Sony's servers) and
its own build tools (about 2 GB, into this folder). Nothing is installed on the system, it works the
same on every Linux, and your game folder is never changed.

Optimise for this PC: the game and its engine are compiled once more with what they learn while a
new game plays by itself in a window for about 7 minutes (the opening and the tutorial on the pier;
sound off; please don't touch it). On a Steam Deck that made the game 7-9 % faster. If it fails,
you still get the normal build." opt="Optimise for this PC (PGO): $pgo_note" r
    local z=(TRUE pgo "$opt") kd=(pgo "$opt" on)
    if [ -n "$steam_users" ]; then z+=(TRUE steam "$steam_label"); kd+=(steam "$steam_label" on); fi
    case $ui in
        zenity) r="$(zenity --list --checklist --title="$title" --text="$(esc "$text")" --width=700 --height=400 --ok-label=Install \
                    --column="" --column=tag --column="" --hide-column=2 --print-column=2 --hide-header "${z[@]}" 2>/dev/null)" || return 1
                printf '%s\n' "$r" | tr '|' '\n' ;;
        kdialog) r="$(kdialog --title "$title" --checklist "$text" "${kd[@]}" 2>/dev/null)" || return 1
                 printf '%s\n' "$r" | tr -d '"' | tr ' ' '\n' ;;
        *) printf '\n%s\n' "$text" >&2; ask "$opt" yes no && echo pgo
           [ -n "$steam_users" ] && ask "$steam_label" yes no && echo steam; return 0 ;;
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

play() { cd "$here" && exec bash "$here/tools/play.sh"; }

# ------------------------------------------------------------------ Add to Steam
# Python: setup's own (tools-cache/python, tools/buildenv.sh), else the system's; on a system with none, setup's is
# downloaded first (37 MB), so the first screen can offer Add to Steam
pyexe="$here/tools-cache/python/bin/python3"
if [ ! -x "$pyexe" ]; then
    pyexe="$(command -v python3 || true)"
    if [ -z "$pyexe" ]; then
        mkdir -p "$here/tools-cache/dl" "$here/work/logs"
        if ( . "$here/tools/libc_headers.sh" && . "$here/tools/buildenv.sh" && be_python "$here/tools-cache" "$here/tools-cache/dl" ) >> "$here/work/logs/install.log" 2>&1; then
            pyexe="$here/tools-cache/python/bin/python3"
        fi
    fi
fi
steam_py() { [ -n "$pyexe" ] || return 1; "$pyexe" -I "$here/tools/steam_shortcut.py" "$@" ${PORT_STEAM_ROOT:+--steam "$PORT_STEAM_ROOT"}; }
steam_users="$(steam_py users 2>/dev/null)"   # "<id>\t<most recent>\t<account>\t<file>" lines; empty = no Steam here
steam_label="Add to Steam (a shortcut to play it in Game Mode; Steam has to be closed for a moment at the end)"
steam_exe="$here/tools/play.sh"
steam_name=""; steam_user=""
steam_running() {
    if [ -n "${PORT_STEAM_TEST_RUNNING:-}" ]; then [ -e "$PORT_STEAM_TEST_RUNNING" ]; return; fi
    pgrep -x steam >/dev/null 2>&1
}
entry_dialog() {   # entry_dialog <text> <default>: prints what the player typed; 1 = cancelled
    case $ui in
        zenity) zenity --entry --title="$title" --width=560 --text="$(esc "$1")" --entry-text="$2" 2>/dev/null ;;
        kdialog) kdialog --title "$title" --inputbox "$1" "$2" 2>/dev/null ;;
        *) local r; printf '\n%s\n' "$1" >&2; read -r -p "[$2]: " r; printf '%s\n' "${r:-$2}" ;;
    esac
}
steam_pick_user() {   # the Steam account to add it to: the only one, else ask (the most recently logged-in one first)
    local n m args=() id rec acc f
    n=$(printf '%s\n' "$steam_users" | grep -c .)
    if [ "$n" -le 1 ]; then steam_user="$(printf '%s\n' "$steam_users" | head -n 1 | cut -f1)"; return 0; fi
    while IFS=$'\t' read -r id rec acc f; do
        [ "$rec" = 1 ] && args=("$id" "${acc:-account $id} (last logged in)" "${args[@]}") || args+=("$id" "${acc:-account $id}")
    done <<< "$steam_users"
    steam_user="$(menu "This PC has $n Steam accounts. Add inFAMOUS 2 to which one?" "${args[@]}")" || return 1
    [ -n "$steam_user" ]
}
steam_ask_name() {   # the name in Steam, checked against the shortcuts already there (none is ever changed); 1 = skip
    local name="${1:-inFAMOUS 2 (PC port)}" note="" c
    steam_pick_user || return 1
    while :; do
        name="$(entry_dialog "${note}The name inFAMOUS 2 gets in your Steam library:" "$name")" || return 1
        name="$(printf '%s' "$name" | tr -d '\n\r\t' | sed 's/^ *//; s/ *$//')"
        [ -z "$name" ] && { name="inFAMOUS 2 (PC port)"; continue; }
        c="$(steam_py check --user "$steam_user" --name "$name" --exe "$steam_exe" 2>/dev/null)" \
            || c="$(steam_py check --user "$steam_user" --name "$name" --exe "$steam_exe" 2>&1 >/dev/null | tail -n 1)"
        case "$c" in
            ok) steam_name="$name"; return 0 ;;
            name-taken*) note="Your Steam library already has a game called \"$name\" (it stays as it is).
This one gets another name: \"${c#*$'\t'}\", or type your own.

"; name="${c#*$'\t'}" ;;
            exe-taken*) msg info "Steam already has a shortcut that starts the game in this folder: \"${c#*$'\t'}\". Nothing is added."; return 1 ;;
            *) msg error "Steam's shortcut list could not be read, so nothing is added to it: $c"; return 1 ;;
        esac
    done
}
steam_add() {   # adds the shortcut (Steam closed first); says what happened
    local r out i
    [ -n "$steam_name" ] && [ -n "$steam_user" ] || return 0
    while steam_running; do
        r="$(menu "Steam is running. It keeps its list of games in memory and writes it when it closes, so the new shortcut
can only be added while Steam is closed. Nothing else in your Steam library is changed.

Close Steam now? (Downloads pause and Steam's windows close; start Steam again afterwards.)" \
                  close "Close Steam now, then add the shortcut" retry "I closed Steam myself: add it now" skip "Not now (add it later: run Install inFAMOUS 2 again)")" || r=skip
        case "$r" in
            close) "${PORT_STEAM_CMD:-steam}" -shutdown >/dev/null 2>&1 &
                   for i in $(seq 1 60); do sleep 1; steam_running || break; done
                   steam_running && msg info "Steam is still closing. Wait until it is gone, then choose \"I closed Steam myself\"." ;;
            retry) ;;
            *) return 0 ;;
        esac
    done
    if out="$(steam_py add --user "$steam_user" --name "$steam_name" --exe "$steam_exe" --start-dir "$here" --art "$here/game/disc/PS3_GAME" 2>&1)"; then
        printf '%s\n' "$out" >> "$log"
        msg info "Added to Steam: \"$steam_name\". Start Steam again: it is in your library under Non-Steam, and in Game Mode.

It starts $steam_exe (if you move this folder, add it again). A copy of Steam's shortcut list from before is next to it (shortcuts.vdf.bak-...)."
    else
        printf '%s\n' "$out" >> "$log"
        msg error "Adding it to Steam did not work, and Steam's list was not changed: ${out##*$'\n'}"
    fi
}

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
    st=(); [ -n "$steam_users" ] && st=(steam "Add to Steam (a shortcut to play it in Game Mode)")
    choice="$(menu "inFAMOUS 2 is installed in this folder.
$now
${upd}What now?" \
              play "Play" rebuild "Rebuild the normal way (after an update, or to repair it)" "${opt[@]}" ${st[@]+"${st[@]}"})" || exit 0
    case "$choice" in
        play) play ;;
        steam) mkdir -p "$here/work/logs"; steam_ask_name && steam_add; exit 0 ;;
        rebuild) args=(--no-pgo) ;;
        pgo) args=(--pgo) ;;
        retrain) rm -f "$here"/work/pgo-setup/setup-*.profdata "$here"/work/pgo-setup/engine-*.profdata; args=(--pgo) ;;
        *) exit 0 ;;
    esac
else
    sel="$(first_screen)" || exit 0
    if printf '%s\n' "$sel" | grep -qx pgo; then args=(--pgo); else args=(--no-pgo); fi
    if printf '%s\n' "$sel" | grep -qx steam; then steam_ask_name || true; fi
fi

# setup's menu entry (~/.local/share/applications/infamous2-port.desktop) is kept when it starts another folder's game
entry="${XDG_DATA_HOME:-$HOME/.local/share}/applications/infamous2-port.desktop"
menu_too=", or find inFAMOUS 2 in the application menu"
play_how="double-click \"Play inFAMOUS 2\" in this folder ($here)"
[ -f "$here/Play inFAMOUS 2" ] || play_how="./setup.sh --play in this folder ($here)"
if [ -f "$entry" ] && ! grep -qxF "Path=$here" "$entry"; then args+=(--no-desktop); menu_too=""; fi

# The game folder an earlier setup linked (game/disc -> your folder) and it is not there: an SD card or drive that is not
# in (or not mounted yet), or a folder that was moved or deleted
missing_disc() {
    local where="$1" r
    while [ -L "$here/game/disc" ] && [ ! -f "$here/game/disc/PS3_GAME/PARAM.SFO" ]; do
        r="$(menu "Your game folder is not there:
$where

The game is used from that folder (it was not copied). If it is on an SD card or another drive,
put it in and wait until the desktop shows it, then choose Try again. If you moved the folder,
choose it at its new place." \
                  retry "Try again" pick "Choose the game folder" quit "Cancel")" || return 1
        case "$r" in retry) ;; pick) return 1 ;; *) exit 0 ;; esac
    done
    return 0
}
disc=""
if [ -L "$here/game/disc" ] && [ ! -f "$here/game/disc/PS3_GAME/PARAM.SFO" ]; then
    missing_disc "$(readlink "$here/game/disc")" && disc="$(readlink -f "$here/game/disc")"
fi
[ -z "$disc" ] && disc="$(bash "$here/setup.sh" --find-disc 2>/dev/null)"
if [ -n "$disc" ] && [ $built = 0 ] && ! ask "Your game: $disc

Use this folder?" "Use it" "Choose another"; then disc=""; fi
while [ -z "$disc" ]; do
    picked="$(pick_folder)" || exit 0
    [ -z "$picked" ] && exit 0
    disc="$(game_folder "$picked")" || { msg error "There is no PS3_GAME folder in $picked. Choose the folder that holds PS3_GAME (your disc's files)."; disc=""; }
done

# How to use it (a first install, or a game folder that was never set up here): from where it is (no copy: the default)
# or copied into this folder. Later runs keep the choice (work/setup-game).
if [ ! -f "$here/work/setup-game" ] || [ $built = 0 ]; then
    size=$(du -sb "$disc" 2>/dev/null | cut -f1); size=${size:-0}
    free=$(df -P -B1 "$here" | awk 'NR==2{print $4}')
    keep="keep the folder where it is"; keep2="it has to stay where it is"
    case "$disc" in /run/media/*|/media/*) keep="keep the SD card in"; keep2="keep the SD card (or drive) in whenever you play" ;; esac
    if [ "$(stat -c %d "$disc" 2>/dev/null)" = "$(stat -c %d "$here" 2>/dev/null)" ]; then same=" (it is on the same drive as this folder, so a copy only takes room)"; else same=""; fi
    copy_label="Copy the game into this folder ($(gb "$size") GB more; then the port needs nothing outside it)"
    [ "$free" -gt $(( size + 3221225472 + 10737418240 )) ] || copy_label="Copy the game into this folder ($(gb "$size") GB more: there is not enough free space, $(gb "$free") GB)"
    gmode="$(menu "Your game: $disc ($(gb "$size") GB)

Use the game from its folder: nothing is copied$same. The game only reads that folder (it is never
changed): $keep2.
Copy the game into this folder: $(gb "$size") GB more and a few minutes longer; afterwards the SD card
can go and this folder can be moved anywhere.

Your saves, settings and the game's update are kept in this folder either way." \
               link "Use the game from its folder (saves $(gb "$size") GB; $keep)" copy "$copy_label")" || exit 0
    case "$gmode" in link) args+=(--link-game) ;; copy) args+=(--copy-game) ;; *) exit 0 ;; esac
fi

if run_setup --disc "$disc" "${args[@]}"; then
    steam_add   # (when Add to Steam was ticked: before the last window, so Steam can be started again after it)
    pgo_line="$(sed -n 's/^  pgo: \(used (.*\|not used: .*\)$/\1/p' "$log" | tail -n 1)"
    mins="$(sed -n 's/^== done in \([0-9]*\) min.*/\1/p' "$log" | tail -n 1)"
    case "$pgo_line" in
        "used ("*) how="Ready to play, optimised for this PC." ;;
        "not used: "*) how="Ready to play. Optimise for this PC didn't work this time (${pgo_line#not used: }), so the game was built the normal way." ;;
        *) how="Ready to play." ;;
    esac
    linked=""
    [ -L "$here/game/disc" ] && linked="
Your game is used from $(readlink "$here/game/disc"): it has to be there when you play."
    if ask "$how${mins:+ (setup took $mins min)}

To play later: ${play_how}${menu_too}.
In the game: F1 = FPS counter, F2 = settings.$linked" "Play now" "Close"; then play; fi
else
    why="$(sed -n 's/^== FAILED: //p' "$log" | tail -n 1)"
    miss="$(sed -n 's/^  MISS  //p' "$log")"   # step 1's checks that failed
    msg error "Setup stopped: ${why:-see the log}.${miss:+

$miss}

What is done so far stays: run the installer again to go on. Everything setup said is in $log"
    exit 1
fi
