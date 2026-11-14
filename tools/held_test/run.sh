#!/bin/sh
# The held objects' rules, engine-free (src/roth/roth_heldlogic.h): the strike detector, the match's timer and flame
# point, the lamp's hook / grab / release, the light follower, the forced turn's view, the Book's view, the hands.
# The differential test against the running original is run_diff.sh.
cd "$(dirname "$0")" || exit 2
sh ./build.sh /tmp/held_test || exit 1
/tmp/held_test unit
