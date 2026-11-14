#!/bin/sh
# tools/gesture_test/run.sh -- the chapter-1 gesture detectors (src/roth/roth_gesture.h), engine-free: the punch
# reader against a transcription of RSQ Melee's own (random hand paths), the contact, the knock, the attempts
# (push, press, tug, wave) and the ward tingle. Exit 0 = every check passed. Nothing is written into the repo.
set -e
cd "$(dirname "$0")/../.."
OUT=${TMPDIR:-/tmp}/gesture_test
mkdir -p "$OUT"
g++ -std=c++17 -O1 -Wall -Wextra -Werror -Isrc/roth tools/gesture_test/test_gesture.cpp -o "$OUT/test_gesture"
"$OUT/test_gesture"
