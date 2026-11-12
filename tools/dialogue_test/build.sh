#!/bin/sh
# Build the port side of the dialogue / inventory differential test (engine-free, g++ only).
#   tools/dialogue_test/build.sh [out-dir]
set -e
HERE=$(cd "$(dirname "$0")" && pwd); SRC="$HERE/../../src/roth"; OUT=${1:-${DLG_WORK:-/tmp/dialogue_test}}
mkdir -p "$OUT"
g++ -std=c++17 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -o "$OUT/dialogue_diff" \
	"$HERE/dialogue_diff.cpp" "$SRC/roth_gamecore.cpp" "$SRC/roth_dbase100_interp.cpp" "$SRC/roth_inventory.cpp" \
	"$SRC/roth_dbase100.cpp" "$SRC/roth_dialogue.cpp" "$SRC/roth_invscreen.cpp" "$SRC/roth_icons.cpp"
echo "built $OUT/dialogue_diff"
