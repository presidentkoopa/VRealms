#!/bin/sh
# Build the wall U/V comparison. Engine-free: plain g++ and the src/roth readers.
cd "$(dirname "$0")/../.." || exit 2
g++ -std=c++17 -O1 -Isrc/roth -o "${1:-/tmp/wallseams}" tools/wallseams/wallseams.cpp \
	src/roth/roth_raw.cpp src/roth/roth_install.cpp src/roth/roth_das.cpp \
	src/roth/roth_palette.cpp
