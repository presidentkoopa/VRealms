#!/bin/sh
# Build wicks_test. Engine-free: plain g++ and the src/roth readers plus roth_wicks (its own files with -Werror).
cd "$(dirname "$0")/../.." || exit 2
OUT=${1:-/tmp/wicks_test}
g++ -std=c++17 -O1 -Wall -Wextra -Werror -Isrc/roth -c -o "$OUT.wicks.o" src/roth/roth_wicks.cpp || exit 1
g++ -std=c++17 -O1 -Wall -Wextra -Werror -Isrc/roth -c -o "$OUT.test.o" tools/wicks_test/wicks_test.cpp || exit 1
g++ -std=c++17 -O1 -Isrc/roth -o "$OUT" "$OUT.test.o" "$OUT.wicks.o" \
	src/roth/roth_raw.cpp src/roth/roth_install.cpp src/roth/roth_das.cpp src/roth/roth_palette.cpp
