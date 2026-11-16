#!/bin/sh
# tools/vrweapon_test/run.sh -- chapter 2's weapons in VR (src/roth/roth_vrweapon.h), engine-free: the blade's contact
# and swing, the two-hand hold against the engine's own block, the body slots after RSQ Tactical Rig's press rules.
# Exit 0 = every check passed. Nothing is written into the repo.
set -e
cd "$(dirname "$0")/../.."
OUT=${TMPDIR:-/tmp}/vrweapon_test
mkdir -p "$OUT"
g++ -std=c++17 -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -Isrc/roth \
	tools/vrweapon_test/test_vrweapon.cpp -o "$OUT/test_vrweapon"
"$OUT/test_vrweapon"
