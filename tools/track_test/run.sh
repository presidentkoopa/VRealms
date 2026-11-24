#!/bin/sh
# tools/track_test/run.sh -- the tracks a hand moves things along (src/roth/roth_track.h: hinge, dial, slider, button,
# ported from VRExpansionPlugin), engine-free. Exit 0 = every check passed. Nothing is written into the repo.
set -e
cd "$(dirname "$0")/../.."
OUT=${TMPDIR:-/tmp}/track_test
mkdir -p "$OUT"
g++ -std=c++17 -O1 -Wall -Wextra -Werror -Isrc/roth tools/track_test/test_track.cpp -o "$OUT/test_track"
"$OUT/test_track"
rm -rf "$OUT"
