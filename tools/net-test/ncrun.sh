#!/bin/zsh
# ncrun.sh NAME PORT SECS JOIN_AT PAD: a 2-player session (runsim) that a third machine (role g2)
# joins as a newcomer at JOIN_AT s, with pad script PAD.
source ${0:A:h}/env.sh
K=${0:A:h}; NAME=$1; PORT=$2; SECS=$3; JOIN_AT=$4; PAD=$5; W=$W0/w$PORT

(( $(pgrep ConkersBadFurDayRecompiled | wc -l) == 0 )) || { echo "games still running"; exit 1; }
BIN=$BIN PLAYERS=2 $K/runsim.sh $NAME $SECS $PORT > $W0/$NAME.result 2>&1 &
R=$!
sleep $JOIN_AT; L=$W0/logs/$NAME
mkdir -p $W/guest2/conker-recomp-data/saves; rm -rf $W/guest2/conker-recomp-data/saves/*(N)
(cd $W/guest2 && env ${=G2ENV} CONKER_DISPLAY=windowed CONKER_AUDIO=0 CONKER_ORIGINAL_SIZE=1 CONKER_VIRTUAL_PAD=$K/$PAD \
  CONKER_FRAME_INPUT_LOG=$L.g2.input.log CONKER_STATE_HASH_LOG=$L.g2.hash.log CONKER_STATE_HASH_EVERY=30 \
  CONKER_FRAME_LOG=$L.g2.frame.log $BIN $ROM \
  --net-join 127.0.0.1:$PORT > $L.g2.out 2>&1 &)
wait $R
pkill -9 -f "^$BIN .*net-host $PORT"; pkill -9 -f "^$BIN .*net-join 127.0.0.1:$PORT"
$K/check.py $NAME >> $W0/$NAME.result 2>&1
cat $W0/$NAME.result | grep -v "^\[conker\]"
grep -h "net: " $L.*.out | grep -v "audio\|virtual pad\|frame [0-9]*:" | grep -i "join\|plug\|snapshot\|playing again\|caught up\|turned away\|dropped\|OUT OF" | sort -u
