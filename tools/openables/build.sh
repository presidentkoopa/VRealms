#!/bin/sh
# Build the openables census. Engine-free: plain g++ and the src/roth readers.
cd "$(dirname "$0")/../.." || exit 2
g++ -std=c++17 -O1 -Isrc/roth -o "${1:-/tmp/openables}" tools/openables/openables.cpp \
	src/roth/roth_raw.cpp src/roth/roth_install.cpp src/roth/roth_das.cpp src/roth/roth_palette.cpp \
	src/roth/roth_dbase100.cpp src/roth/roth_dbase100_install.cpp src/roth/roth_log.cpp
