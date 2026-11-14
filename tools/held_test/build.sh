#!/bin/sh
# Build held_test. Engine-free: plain g++, roth_heldlogic, roth_wicks and roth_lightramp.h (their own files with -Werror),
# and the map reader roth_raw.
cd "$(dirname "$0")/../.." || exit 2
OUT=${1:-/tmp/held_test}
g++ -std=c++17 -O1 -Wall -Wextra -Werror -Isrc/roth -c -o "$OUT.logic.o" src/roth/roth_heldlogic.cpp || exit 1
g++ -std=c++17 -O1 -Wall -Wextra -Werror -Isrc/roth -c -o "$OUT.wicks.o" src/roth/roth_wicks.cpp || exit 1
g++ -std=c++17 -O1 -Wall -Wextra -Werror -Isrc/roth -c -o "$OUT.test.o" tools/held_test/test_held.cpp || exit 1
g++ -std=c++17 -O1 -o "$OUT" "$OUT.test.o" "$OUT.logic.o" "$OUT.wicks.o" src/roth/roth_raw.cpp
