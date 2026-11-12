#!/bin/sh
# Build the moving-geometry survey. Engine-free: plain g++ and src/roth/roth_raw.
cd "$(dirname "$0")/../.." || exit 2
g++ -std=c++17 -O1 -Isrc/roth -o "${1:-/tmp/platsurvey}" tools/platforms/platsurvey.cpp \
	src/roth/roth_raw.cpp
