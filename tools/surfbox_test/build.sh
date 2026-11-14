#!/bin/sh
# Build the port side of the surface-box test. Engine-free: plain g++ and the src/roth readers. Writes $1 (default /tmp/surfbox_test/surfbox_test).
cd "$(dirname "$0")/../.." || exit 2
OUT=${1:-/tmp/surfbox_test/surfbox_test}; mkdir -p "$(dirname "$OUT")"
g++ -std=c++17 -O1 -Wall -Wextra -Isrc/roth -o "$OUT" tools/surfbox_test/surfbox_test.cpp \
	src/roth/roth_raw.cpp src/roth/roth_install.cpp src/roth/roth_das.cpp src/roth/roth_palette.cpp \
	src/roth/roth_surface.cpp src/roth/roth_commands.cpp src/roth/roth_log.cpp
