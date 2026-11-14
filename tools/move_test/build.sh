#!/bin/sh
# Build the movement port's replay test. Engine-free: g++ and the src/roth sources.
#   tools/move_test/build.sh [outdir]
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SRC="$HERE/../../src/roth"
OUT=${1:-"$HERE/out"}
mkdir -p "$OUT"
g++ -std=c++17 -Wall -Wextra -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -I"$SRC" \
	-o "$OUT/move_test" "$HERE/move_test.cpp" "$SRC/roth_playermove.cpp" "$SRC"/roth_actors.cpp "$SRC"/roth_actor_*.cpp \
	"$SRC/roth_raw.cpp" "$SRC/roth_dbase100.cpp" "$SRC/roth_das.cpp" "$SRC/roth_palette.cpp"
g++ -std=c++17 -Wall -Wextra -O1 -g -I"$SRC" -o "$OUT/move_sched" "$HERE/move_sched.cpp" "$SRC/roth_playermove.cpp" "$SRC"/roth_actors.cpp "$SRC"/roth_actor_*.cpp \
	"$SRC/roth_raw.cpp" "$SRC/roth_dbase100.cpp" "$SRC/roth_das.cpp" "$SRC/roth_palette.cpp"
