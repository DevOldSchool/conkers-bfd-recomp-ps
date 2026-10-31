#!/bin/zsh
# krun.sh NAME SECS PORT: the keyboard's Start (runsim with k1.pad and k2.pad, keyboard pictures):
# the guest joins with Enter, its Enter and Esc mid-match pause nothing, the host's Esc does not
# pause and its Enter does. Both windows are captured at every "# shot" step, as
# logs/NAME.<role>.vi<VI>.png; then the input logs say which ports held Start.
source ${0:A:h}/env.sh
K=${0:A:h}; NAME=$1; SECS=$2; PORT=$3; L=$W0/logs/$NAME
(( $(pgrep ConkersBadFurDayRecompiled | wc -l) == 0 )) || { echo "games still running"; exit 1; }
rm -f $L.*(N)
P1=${P1:-k1.pad} P2=${P2:-k2.pad} HENV="CONKER_PAD_ICONS=keyboard ${HENV}" GENV="CONKER_PAD_ICONS=keyboard ${GENV}" \
  $K/runsim.sh $NAME $SECS $PORT > $W0/$NAME.result 2>&1 &
R=$!
typeset -A seen
while kill -0 $R 2>/dev/null; do
  for role in h g; do
    for vi in $(grep -ho "virtual pad: VI [0-9]*:[0-9]* 1 *$" $L.$role.out 2>/dev/null | sed 's/.*://; s/ .*//'); do
      [[ -n ${seen[$role$vi]} ]] && continue
      seen[$role$vi]=1
      pid=$(pgrep -f "^$BIN .*$([[ $role == h ]] && echo "net-host $PORT" || echo "net-join 127.0.0.1:$PORT")")
      w=$($WINID | awk -v p=$pid '$2 == p {print $1; exit}')
      [[ -n $w ]] && screencapture -x -o -l$w $L.$role.vi$vi.png
    done
  done
  sleep 0.2
done
cat $W0/$NAME.result
$K/check.py $NAME 2>&1 | tail -5
# Start (0x1000) per port in the merged input, as VI ranges
for role in h g; do
  echo "== $role input"
  python3 -c '
import sys
on, start = [False] * 4, [0] * 4
for line in open(sys.argv[1]):
    t = line.split()
    if line.startswith("#") or len(t) < 6: continue
    for p in range(4):
        held = (int(t[2 + p].split(":")[1].split(",")[0], 16) & 0x1000) != 0
        if held and not on[p]: start[p] = t[1]
        if not held and on[p]: print("port", p + 1, "Start VI", start[p], "-", t[1])
        on[p] = held
' $L.$role.input.log
done
