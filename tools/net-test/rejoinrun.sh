#!/bin/zsh
# rejoinrun.sh NAME PORT SECS [KILL_AT REJOIN_AT]: host + guest (runsim, GENV passes through); with
# KILL_AT the guest is killed then and a new guest (role g2) joins by hand at REJOIN_AT, from the
# guest's own folder (the same machine: it has its secret for this host, so it gets its port back) or,
# with REJOIN_DIR=guest2, from a fresh one (another machine: a newcomer).
# G2ARGS: extra arguments for the machine that joins again (HARGS/GARGS pass to runsim). FAULT=term
# quits the guest normally instead of killing it; FAULT=stop freezes it for STOP_SECS (3) and lets it go
# (no REJOIN_AT: it rejoins by itself). With KILL_AT and no REJOIN_AT, nobody joins again.
source ${0:A:h}/env.sh
K=${0:A:h}; NAME=$1; PORT=$2; SECS=$3; W=$W0/w$PORT
(( $(pgrep ConkersBadFurDayRecompiled | wc -l) == 0 )) || { echo "games still running"; exit 1; }
$K/runsim.sh $NAME $SECS $PORT > $W0/$NAME.result 2>&1 &
R=$!
# A guest whose frames stop for 40 s is sampled (which pauses it for 2 s): the newest guest's log counts,
# so that a guest that quit and came back as g2 is not taken for a stuck one.
( n=0; while kill -0 $R 2>/dev/null; do sleep 10; f=$W0/logs/$NAME.g.frame.log
    [[ -f $W0/logs/$NAME.g2.frame.log && $W0/logs/$NAME.g2.frame.log -nt $f ]] && f=$W0/logs/$NAME.g2.frame.log
    if [[ -f $f ]] && (( $(date +%s) - $(stat -f %m $f) > 40 )) && (( n < 2 )); then
      P=$(pgrep -f "net-join 127.0.0.1:$PORT" | head -1); [[ -n $P ]] && { sample $P 2 -file $W0/$NAME.stuck$n.sample >/dev/null 2>&1; echo "guest frames stopped: sampled $P" >> $W0/$NAME.notes; n=$((n+1)); }
    fi; done ) &
if [[ -n $4 && $FAULT == stop ]]; then
  # The guest's whole process (game and network) frozen for STOP_SECS, then let go: it must find itself
  # dropped, start again on its own and rejoin.
  sleep $4; P=$(pgrep -f "net-join 127.0.0.1:$PORT" | head -1); kill -STOP $P; echo "guest stopped at $4 s" >> $W0/$NAME.notes
  sleep ${STOP_SECS:-3}; kill -CONT $P; echo "guest let go after ${STOP_SECS:-3} s" >> $W0/$NAME.notes
elif [[ -n $4 ]]; then
  # FAULT=term: the guest quits normally (SIGTERM becomes SDL's quit) and says LEAVE; else it is killed.
  SIG=KILL; [[ $FAULT == term ]] && SIG=TERM
  sleep $4; pkill -$SIG -f "^$BIN .*net-join 127.0.0.1:$PORT"; echo "guest sent SIG$SIG at $4 s" >> $W0/$NAME.notes
  if [[ -z $5 ]]; then
    wait $R; pkill -9 -f "^$BIN .*net-host $PORT"; $K/check.py $NAME >> $W0/$NAME.result 2>&1; exit 0
  fi
  sleep $(( $5 - $4 ))
  L=$W0/logs/$NAME; RD=${REJOIN_DIR:-guest}
  mkdir -p $W/$RD/conker-recomp-data/saves; [[ $RD == guest2 ]] && rm -rf $W/guest2/conker-recomp-data/saves/*(N) $W/guest2/conker-recomp-data/net/secrets.txt(N)
  (cd $W/$RD && env ${=G2ENV} CONKER_DISPLAY=windowed CONKER_AUDIO=0 CONKER_ORIGINAL_SIZE=1 CONKER_VIRTUAL_PAD=$K/f2.pad \
    CONKER_FRAME_INPUT_LOG=$L.g2.input.log CONKER_STATE_HASH_LOG=$L.g2.hash.log CONKER_STATE_HASH_EVERY=30 \
    CONKER_FRAME_LOG=$L.g2.frame.log $BIN $ROM \
    --net-join 127.0.0.1:$PORT ${=G2ARGS} > $L.g2.out 2>&1 &)
  echo "guest started again at $5 s" >> $W0/$NAME.notes
fi
wait $R
pkill -9 -f "^$BIN .*net-host $PORT"; pkill -9 -f "^$BIN .*net-join 127.0.0.1:$PORT"
$K/check.py $NAME >> $W0/$NAME.result 2>&1
grep -h "net: " $W0/logs/$NAME.*.out | grep -v "audio\|virtual pad" | grep -i "rejoin\|catch\|caught\|back from\|plays again\|out of sync\|turned away\|dropped\|starting again" >> $W0/$NAME.result
