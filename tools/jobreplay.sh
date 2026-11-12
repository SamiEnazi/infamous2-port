#!/bin/bash
# SPU job equivalence check on Linux (tools/jobreplay.ps1's counterpart): replay the job recordings in
# <dir> through an exe's own lifted SPU code and compare every output bit for bit (toolkit
# runtime/spu/spu_jobrec.c, PS3_JOBREPLAY). The game does not boot (no window, no display needed).
#   tools/jobreplay.sh <dir> [exe] [threads] [extra K=V ...]
#   e.g. tools/jobreplay.sh native/recordings/ck-rec2/jobrec
# Exit code 0 = every replay matched. stderr goes to <dir>/replay_stderr_<time>.log.
set -u
root="$(cd "$(dirname "$0")/.." && pwd)"
dir="$(cd "${1:?usage: jobreplay.sh <dir> [exe] [threads] [K=V ...]}" && pwd)"
exe="${2:-$root/work/port/build/Infamous2Recomp}"
threads="${3:-2}"
shift $(( $# < 3 ? $# : 3 ))
flash="${PORT_DEV_FLASH:-$root/game/dev_flash}"
errf="$dir/replay_stderr_$(date +%Y%m%d_%H%M%S).log"
t0=$(date +%s)
cd "$dir"
env PS3_VFS_ROOT="$root/game/disc" PS3_HDD0_ROOT="$root/game/dev_hdd0" PS3_DEV_FLASH="$flash" \
    PS3_GAMEDATA_ROOT="$root/game/gamedata" PS3_TITLE="inFAMOUS 2" PS3_LANG=en PS3_AUDIO_VOLUME=0 \
    PS3_PAD_SDL=0 PS3_VERBOSE=0 \
    PS3RECOMP_ENG_GPU_LIGHT=07000B80:07384B80 PS3RECOMP_ENG_GPU_SSAO=0F800000 \
    PS3_JOBREPLAY="$dir" PS3_JOBREPLAY_THREADS="$threads" PS3_JOBREPLAY_REPEAT="${PS3_JOBREPLAY_REPEAT:-1}" \
    PS3_JOBREPLAY_TIMEOUT_MS="${PS3_JOBREPLAY_TIMEOUT_MS:-20000}" "$@" \
    nice -n "${PORT_NICE:-10}" "$exe" "$root/work/EBOOT.elf" 2>"$errf" |
    grep -E '^\[jobreplay\]|^==|^ovl|^[0-9]+\s|^TOTAL|^first FAIL'
rc=${PIPESTATUS[0]}
echo "exit=$rc after $(( $(date +%s) - t0 )) s  (stderr: $errf)"
exit $rc
