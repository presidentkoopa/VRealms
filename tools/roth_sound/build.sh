#!/bin/sh
# Build the Realms sound report tool. Needs only g++ (C++17).
# Writes into ./out (or $1), never beside the sources, and the tool itself
# writes NO game audio anywhere: it prints lengths and hashes.
#
#   tools/roth_sound/build.sh                 # also builds and runs sfx_test (the rules)
#   ROTH_DATA=/path/to/DATA tools/roth_sound/build.sh   # ... and over every retail node
#   tools/roth_sound/out/roth_sound_tool "/path/to/Realms of the Haunting/DATA"
#   ... --samples    every sample row      ... --nodes   every sound node
#   ... --commands   every sound command   ... --speech  every speech clip (decodes all; slow)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SRC="$HERE/../../src/roth"
OUT=${1:-"$HERE/out"}
mkdir -p "$OUT"
g++ -std=c++17 -Wall -Wextra -Werror -O2 -g -I"$SRC" -o "$OUT/roth_sound_tool" \
	"$HERE/roth_sound_tool.cpp" "$SRC/roth_sfx.cpp" "$SRC/roth_raw.cpp" \
	"$SRC/roth_dbase100.cpp" "$SRC/roth_install.cpp"
g++ -std=c++17 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
	-I"$SRC" -o "$OUT/sfx_test" "$HERE/sfx_test.cpp" "$SRC/roth_sfx.cpp" "$SRC/roth_raw.cpp" "$SRC/roth_install.cpp"
"$OUT/sfx_test" ${ROTH_DATA:+"$ROTH_DATA"}
echo "built $OUT/roth_sound_tool"
