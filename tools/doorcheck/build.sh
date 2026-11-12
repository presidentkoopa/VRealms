#!/bin/sh
# Build the door census. Engine-free: plain g++ and the src/roth readers.
cd "$(dirname "$0")/../.." || exit 2
g++ -std=c++17 -O1 -Isrc/roth -o "${1:-/tmp/doorcheck}" tools/doorcheck/doorcheck.cpp \
	src/roth/roth_raw.cpp src/roth/roth_install.cpp src/roth/roth_das.cpp \
	src/roth/roth_palette.cpp
