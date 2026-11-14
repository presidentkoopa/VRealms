#!/bin/sh
# cmp_pc.sh ENGINE_LOG [run|walk]: the [roth_move] tic lines of a move_flat.cfg run on the owner's PC
# against tools/move_test/expected/move_flat_<run|walk>.txt (cl_run 1 / 0). Health is not in the
# expected lines (the engine's, not the movement's). Prints the differing lines and the count.
HERE=$(cd "$(dirname "$0")" && pwd)
EXP="$HERE/expected/move_flat_${2:-run}.txt"
GOT=$(mktemp)
grep '^\[roth_move\] tic ' "$1" | tr -d '\r' | sed 's/^\[roth_move\] //; s/ health=[0-9-]*//' > "$GOT"
N=$(wc -l < "$GOT")
head -n "$N" "$EXP" | diff - "$GOT"
echo "$(head -n "$N" "$EXP" | diff - "$GOT" | grep -c '^>') of $N tic lines differ"
rm -f "$GOT"
