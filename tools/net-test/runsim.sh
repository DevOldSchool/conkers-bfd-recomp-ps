#!/bin/zsh
# runsim.sh NAME SECS PORT: an online session on this Mac, host + 1 or 2 guests, each in its own folder
# under w$PORT. Env: PLAYERS (2|3), DELAY (host's --net-delay), HSIM/GSIM/G2SIM (CONKER_NET_SIM for
# host/guest/guest2), HENV/GENV/G2ENV extra env, HARGS/GARGS extra arguments (GARGS for every guest),
# P1/P2/P3 pad scripts, BIN.
source ${0:A:h}/env.sh
K=${0:A:h}; NAME=$1; DUR=$2; PORT=$3; W=$W0/w$PORT
(( $(pgrep ConkersBadFurDayRecompiled | wc -l) == 0 )) || [[ -n $NET_TEST_NESTED ]] || { echo "games still running"; exit 1; }
mkdir -p $W/host/conker-recomp-data/saves $W/guest/conker-recomp-data/saves $W/guest2/conker-recomp-data/saves $W0/logs
cp $SAVE $W/host/conker-recomp-data/saves/conker_us.bin
rm -rf $W/guest/conker-recomp-data/saves/*(N) $W/guest2/conker-recomp-data/saves/*(N)
export CONKER_DISPLAY=windowed CONKER_AUDIO=0 CONKER_ORIGINAL_SIZE=1
L=$W0/logs/$NAME
run() {  # role dir pad sim extra-env args...
  local role=$1 dir=$2 pad=$3 sim=$4 extra=$5; shift 5
  (cd $dir && env CONKER_NET_SIM="$sim" CONKER_VIRTUAL_PAD=$K/$pad CONKER_FRAME_INPUT_LOG=$L.$role.input.log \
    CONKER_STATE_HASH_LOG=$L.$role.hash.log CONKER_STATE_HASH_EVERY=${HASH_EVERY:-30} CONKER_FRAME_LOG=$L.$role.frame.log ${=extra} \
    $BIN $ROM "$@" > $L.$role.out 2>&1) &
}
run h $W/host ${P1:-f1.pad} "$HSIM" "$HENV" --net-host $PORT --net-players ${PLAYERS:-2} ${DELAY:+--net-delay} ${DELAY} --net-open ${=HARGS}
sleep 1
run g $W/guest ${P2:-f2.pad} "$GSIM" "$GENV" --net-join 127.0.0.1:$PORT ${=GARGS}
if [[ ${PLAYERS:-2} == 3 ]]; then sleep 1; run g2 $W/guest2 ${P3:-f3.pad} "$G2SIM" "$G2ENV" --net-join 127.0.0.1:$PORT ${=GARGS}; fi
start=$SECONDS
sleep $DUR
pkill -9 -f "^$BIN .*net-host $PORT" ; pkill -9 -f "^$BIN .*net-join 127.0.0.1:$PORT"
wait 2>/dev/null
echo "== $NAME ($((SECONDS-start)) s)"
for r in h g g2; do [[ -f $L.$r.frame.log ]] && echo "$r frames $(wc -l < $L.$r.frame.log)"; done
grep -h "net: \(player\|OUT\|the host\|every player\|simulating\)" $L.*.out | sort | uniq | head -20
