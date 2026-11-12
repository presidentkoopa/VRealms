#!/bin/bash
# run.sh [mode] -- the whole differential test: original, port, compare. Exit 0 = no difference.
# mode (PROOF_MODE) picks the stub answers both sides give: bit 0 = dialogue windows refuse to
# open (return 0), bit 1 = a DBASE300 music chunk loads fresh (return 0), bit 2 = the music is
# faded out (g_audio_seq_state 2). Run at least 0 1 2 4 7.
# Outputs under $PROOF_WORK (default /tmp/gameproof); nothing is written into the repo.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
DATA="${ROTH_DATA:-/mnt/user-data/uploads/Realms of the Haunting/DATA}"
WORK=${PROOF_WORK:-/tmp/gameproof}; MODE=${1:-0}; mkdir -p "$WORK"
export PROOF_RAWRECS=${PROOF_RAWRECS:-$WORK/rawrecs.txt}
"$HERE/build_replay.sh" "$WORK/replay" || exit 1
g++ -std=c++17 -O1 -Wall -Wextra -Werror -o "$WORK/rawrecs" "$HERE/rawrecs.cpp" "$HERE/../../src/roth/roth_raw.cpp" || exit 1
[ -s "$PROOF_RAWRECS" ] || "$WORK/rawrecs" "$DATA/M" > "$PROOF_RAWRECS" || exit 1
"$HERE/run_oracle.sh" "$WORK/oracle_m$MODE.trace" "$MODE" || exit 1
"$WORK/replay" "$DATA" "$WORK/oracle_m$MODE.trace" "$WORK/port_m$MODE.trace" "$PROOF_RAWRECS" || exit 1
python3 "$HERE/compare.py" "$WORK/oracle_m$MODE.trace" "$WORK/port_m$MODE.trace" ${PROOF_SHOW:+--show $PROOF_SHOW}
