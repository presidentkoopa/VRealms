#!/bin/sh
# Build the wall texel-rule comparison. Engine-free: plain g++ and the src/roth readers.
cd "$(dirname "$0")/../.." || exit 2
g++ -std=c++17 -O1 -Isrc/roth -o "${1:-/tmp/wallrule}" tools/wallrule/wallrule.cpp \
	src/roth/roth_raw.cpp src/roth/roth_install.cpp src/roth/roth_das.cpp \
	src/roth/roth_palette.cpp src/roth/roth_surface.cpp
