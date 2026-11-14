#!/bin/bash
# run_all.sh [scenario...]: every scenario through the original (run.sh) and the port (move_test), counts.
# MOVE_TEST: the port binary (default: build into /tmp/move_test_out). OUTDIR: logs (default /tmp/move_test_logs).
HERE=$(cd "$(dirname "$0")" && pwd)
OUTDIR=${OUTDIR:-/tmp/move_test_logs}; mkdir -p $OUTDIR
if [ -z "$MOVE_TEST" ]; then $HERE/../build.sh /tmp/move_test_out >/dev/null || exit 1; MOVE_TEST=/tmp/move_test_out/move_test; fi
$HERE/build.sh || exit 1
DATA="${ROTH_DATA:-/mnt/user-data/uploads/Realms of the Haunting/DATA}"
RAW=$(find "$DATA/M" -maxdepth 1 -iname STUDY1.RAW | head -1)
LIST=${@:-$(cd $HERE/scenarios && ls *.txt | sed 's/\.txt$//')}
for s in $LIST; do
  $HERE/run.sh STUDY1 $HERE/scenarios/$s.txt $OUTDIR/$s.log 6000 >/dev/null
  echo "== $s"; $MOVE_TEST "$RAW" $OUTDIR/$s.log | grep -E "^(tables|image, |run|exercised|host|NOTE|DIFF|  )"
done
