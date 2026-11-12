#!/bin/bash
# diffcmds.sh MAP -- run every 0x16/0x3c/0x3a record of MAP through the original and the port; diff the dumps.
# The port is synced to the original's state after the "initial" dump, so a creature the original's first
# frame woke (first-draw rule) shows up only in the LOAD line, not in the command results.
P=/home/claude/actp; D="/mnt/user-data/uploads/Realms of the Haunting/DATA"; M=$1
mkdir -p $P/cmds
ST=$($P/out/actors_test "$D" $M --census | grep start= | sed 's/.*start=//'); X=${ST%,*}; Y=${ST#*,}
$P/out/actors_test "$D" $M --gen > $P/cmds/$M.script
$P/run.sh $M $X $Y - $P/cmds/$M.script $P/cmds/$M.orig 300 > /dev/null
$P/out/actors_test "$D" $M $P/cmds/$M.script $P/cmds/$M.orig | grep -v "^SND\|^CHAIN" > $P/cmds/$M.port
grep -v "^ENV" $P/cmds/$M.orig > $P/cmds/$M.orig2
split_at() { awk -v want=$2 '/^== /{inb=($2=="initial")} (want=="load"&&inb)||(want=="cmd"&&!inb)' $1; }
N=$(grep -c "^CMD" $P/cmds/$M.orig2); L=$(wc -l < $P/cmds/$M.orig2)
DL=$(diff <(split_at $P/cmds/$M.orig2 load) <(split_at $P/cmds/$M.port load) | grep -c '^[<>]')
DC=$(diff <(split_at $P/cmds/$M.orig2 cmd) <(split_at $P/cmds/$M.port cmd) | grep -c '^[<>]')
echo "$M: commands=$N lines=$L load-diff=$DL command-diff=$DC"
