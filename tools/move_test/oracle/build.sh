#!/bin/bash
# build.sh: the probe plugin into $MOVP_DIR (default /home/claude/movp)
OB=/home/claude/oracle_build; P=${MOVP_DIR:-/home/claude/movp}; mkdir -p $P
$OB/bin/i386-musl-gcc -shared -fPIC -std=gnu11 -O1 -I$OB/rothc/sdk/include -o $P/plugin.so "$(dirname "$0")/moveprobe.c"
