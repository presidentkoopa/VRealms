#!/bin/sh
# tools/invhand_test/run.sh -- the VR hands on the inventory panel, engine-free:
# the panel point <-> the original's screen pixel (src/roth/roth_panelmap.h) and the
# hand's laser / tap / touch (src/roth/roth_invhand.h). See test_invhand.cpp.
# The click those gestures make is proved against the original by tools/invdraw_test
# (scripts/pointer.txt). Exit 0 = every check passed.
set -e
cd "$(dirname "$0")/../.."
OUT=${TMPDIR:-/tmp}/invhand_test
mkdir -p "$OUT"
g++ -std=c++17 -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
	-Isrc/roth tools/invhand_test/test_invhand.cpp -o "$OUT/test_invhand"
"$OUT/test_invhand"
