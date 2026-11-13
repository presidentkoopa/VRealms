#!/bin/sh
# Build the port side of the HUD picture test (engine-free, g++ only).
#   tools/huddraw_test/build.sh [out-dir]
set -e
HERE=$(cd "$(dirname "$0")" && pwd); SRC="$HERE/../../src/roth"; OUT=${1:-${HUDDRAW_WORK:-/tmp/huddraw_test}}
mkdir -p "$OUT"
FLAGS="-std=c++17 -Wall -Wextra -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all"
# roth_das.cpp is built without -Werror: it carries a fall-through warning of its own (MeshFaceTexCoords).
g++ $FLAGS -w -c -o "$OUT/roth_das.o" "$SRC/roth_das.cpp"
g++ $FLAGS -Werror -I"$SRC" -o "$OUT/huddraw_diff" "$HERE/huddraw_diff.cpp" "$OUT/roth_das.o" \
	"$SRC/roth_huddraw.cpp" "$SRC/roth_uidraw.cpp" "$SRC/roth_gamecore.cpp" "$SRC/roth_dbase100_interp.cpp" \
	"$SRC/roth_inventory.cpp" "$SRC/roth_dbase100.cpp" "$SRC/roth_dialogue.cpp" "$SRC/roth_icons.cpp" "$SRC/roth_install.cpp" \
	"$SRC/roth_palette.cpp" "$SRC/roth_actors.cpp" "$SRC"/roth_actor_*.cpp "$SRC/roth_raw.cpp"
echo "built $OUT/huddraw_diff"
