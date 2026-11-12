#!/bin/sh
# Build the host side of the RAW-command differential test. Needs only g++ (C++17).
#   tools/cmdcore_test/build.sh [outdir]
# The test itself needs the retail DATA folder and, for the original's half,
# the ROTH.C oracle build with the probe mod; see README.md.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SRC="$HERE/../../src/roth"
OUT=${1:-"$HERE/out"}
mkdir -p "$OUT"
g++ -std=c++17 -Wall -Wextra -Werror -O1 -g -I "$SRC" -o "$OUT/cmdcore_test" "$HERE/cmdcore_test.cpp" \
	"$SRC/roth_cmdcore.cpp" "$SRC/roth_commands.cpp" "$SRC/roth_raw.cpp" "$SRC/roth_install.cpp"
g++ -std=c++17 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -I "$SRC" \
	-o "$OUT/cmdcore_test_san" "$HERE/cmdcore_test.cpp" \
	"$SRC/roth_cmdcore.cpp" "$SRC/roth_commands.cpp" "$SRC/roth_raw.cpp" "$SRC/roth_install.cpp"
echo "built $OUT/cmdcore_test"
