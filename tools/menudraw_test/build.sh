#!/bin/sh
# Build the port side of the message box / dialogue text picture test (engine-free, g++ only).
#   tools/menudraw_test/build.sh [out-dir]
set -e
HERE=$(cd "$(dirname "$0")" && pwd); SRC="$HERE/../../src/roth"; OUT=${1:-${MENUDRAW_WORK:-/tmp/menudraw_test}}
mkdir -p "$OUT"
FLAGS="-std=c++17 -Wall -Wextra -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all"
# roth_das.cpp is built without -Werror: it carries a fall-through warning of its own (MeshFaceTexCoords).
g++ $FLAGS -w -c -o "$OUT/roth_das.o" "$SRC/roth_das.cpp"
g++ $FLAGS -Werror -o "$OUT/menudraw_diff" "$HERE/menudraw_diff.cpp" "$OUT/roth_das.o" \
	"$SRC/roth_menudraw.cpp" "$SRC/roth_dlgdraw.cpp" "$SRC/roth_uidraw.cpp" "$SRC/roth_gamecore.cpp" "$SRC/roth_dbase100_interp.cpp" \
	"$SRC/roth_inventory.cpp" "$SRC/roth_dbase100.cpp" "$SRC/roth_dialogue.cpp" "$SRC/roth_icons.cpp" "$SRC/roth_install.cpp" "$SRC/roth_palette.cpp"
echo "built $OUT/menudraw_diff"
