#!/bin/sh
# Build the port side of the examine popup picture test (engine-free, g++ only).
#   tools/popdraw_test/build.sh [out-dir]
set -e
HERE=$(cd "$(dirname "$0")" && pwd); SRC="$HERE/../../src/roth"; OUT=${1:-${POPDRAW_WORK:-/tmp/popdraw_test}}
mkdir -p "$OUT"
FLAGS="-std=c++17 -Wall -Wextra -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all"
# roth_das.cpp is built without -Werror: it carries a fall-through warning of its own (MeshFaceTexCoords).
g++ $FLAGS -w -c -o "$OUT/roth_das.o" "$SRC/roth_das.cpp"
g++ $FLAGS -Werror -o "$OUT/popdraw_diff" "$HERE/popdraw_diff.cpp" "$OUT/roth_das.o" \
	"$SRC/roth_popupdraw.cpp" "$SRC/roth_gdv.cpp" "$SRC/roth_invdraw.cpp" "$SRC/roth_uidraw.cpp" "$SRC/roth_invscreen.cpp" "$SRC/roth_gamecore.cpp" \
	"$SRC/roth_dbase100_interp.cpp" "$SRC/roth_inventory.cpp" "$SRC/roth_dbase100.cpp" "$SRC/roth_dialogue.cpp" "$SRC/roth_icons.cpp" "$SRC/roth_install.cpp" "$SRC/roth_palette.cpp"
echo "built $OUT/popdraw_diff"
