#!/bin/bash
# mkgame.sh MAP PACK -> ${ROTH_GAME:-game}_MAP with MAP first in ROTH.RES
M=$1; P=$2; G=${ROTH_GAME:-game}_$M
mkdir -p $G/mods/oracle; cd $G
for f in ${ROTH_GAME:-game}/*; do b=$(basename $f); [ "$b" = ROTH.RES ] || [ "$b" = mods ] || ln -sfn $f $b; done
cp /home/claude/work/oracle/plugin.so mods/oracle/
python3 - "$M" "$P" <<'PY'
import sys
m,p=sys.argv[1].lower(),sys.argv[2].lower()
s=open('${ROTH_GAME:-game}/ROTH.RES','rb').read().decode()
s=s.replace('maps {\r\n','maps {\r\nm\\%s m\\%s\r\n'%(m,p),1)
open('ROTH.RES','w',newline='').write(s)
PY
echo $G
