#!/bin/sh
# Build the port side of the GDV differential test (engine-free, g++ only).
#   tools/gdv_test/build.sh [out-dir]
set -e
HERE=$(cd "$(dirname "$0")" && pwd); SRC="$HERE/../../src/roth"; OUT=${1:-${GDV_WORK:-/tmp/gdvprobe}}
mkdir -p "$OUT"
g++ -std=c++17 -Wall -Wextra -Werror -O2 -o "$OUT/gdvtool" "$HERE/gdvtool.cpp" "$SRC/roth_gdv.cpp" \
	"$SRC/roth_dialogue.cpp" "$SRC/roth_dbase100_interp.cpp" "$SRC/roth_dbase100.cpp"
echo "built $OUT/gdvtool"
