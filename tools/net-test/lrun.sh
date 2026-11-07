#!/bin/zsh
# lrun.sh NAME SECS: two lobbies on this Mac ($NET_TEST_DIR/lob/h and g) driven by $K/NAME.h.script and
# $K/NAME.g.script, in which @L stands for the log prefix, @N for this folder and @A for this Mac's LAN
# address (as the LAN list shows the host). Env: HENV/GENV
# extra env, GW (fake_gateway options, "none" for no gateway: mapping off), SHOTS (seconds to
# screenshot at), NOG=1 for the host alone. Port mapping never reaches the real router.
source ${0:A:h}/env.sh
K=${0:A:h}; NAME=$1; SECS=$2

(( $(pgrep ConkersBadFurDayRecompiled | wc -l) == 0 )) || { echo "games still running"; exit 1; }
L=$W0/logs/$NAME; LB=$W0/lob; mkdir -p $LB/h/conker-recomp-data/saves $LB/g/conker-recomp-data/saves
cp $SAVE $LB/h/conker-recomp-data/saves/conker_us.bin; rm -rf $LB/g/conker-recomp-data/saves/*(N) $LB/g/conker-recomp-data/online.txt $LB/h/conker-recomp-data/online.txt
for r in h g; do [[ -f $K/$NAME.$r.script ]] && sed "s|@L|$L|g; s|@N|$K|g; s|@A|$(ipconfig getifaddr en0 || ipconfig getifaddr en1)|g" $K/$NAME.$r.script > $LB/$NAME.$r.run; done
if [[ $GW == none ]]; then export CONKER_NET_PORTMAP=0; else
  python3 $REPO/tools/fake_gateway.py --http-port 38111 ${=GW} > $L.gateway.log 2>&1 & GWP=$!; sleep 1
  export CONKER_NET_UPNP_URL=http://127.0.0.1:38111/rootDesc.xml CONKER_NET_NATPMP_GATEWAY=127.0.0.1; fi
export CONKER_AUDIO=0 CONKER_DISPLAY=windowed CONKER_ORIGINAL_SIZE=1
run() { local e=$2; (cd $LB/$1 && env ${=e} CONKER_LOBBY_SCRIPT=$LB/$NAME.$1.run CONKER_FRAME_INPUT_LOG=$L.$1.input.log \
  CONKER_STATE_HASH_LOG=$L.$1.hash.log CONKER_STATE_HASH_EVERY=30 CONKER_FRAME_LOG=$L.$1.frame.log $BIN $ROM --net-lobby > $L.$1.out 2>&1 &) }
run h "$HENV"; sleep 1; [[ $NOG == 1 ]] || run g "$GENV"
start=$SECONDS
for t in ${=SHOTS}; do sleep $(( t - (SECONDS - start) )); for w in $($WINID | awk '{print $1}'); do screencapture -x -o -l$w $L.s$t.$w.png 2>/dev/null; done; done
sleep $(( SECS - (SECONDS - start) ))
pkill -9 -f "^$BIN $ROM"; [[ -n $GWP ]] && kill $GWP
echo "== $NAME ($((SECONDS-start)) s)"
for f in $L.*.view*(N); do echo "--- ${f:t}"; cat $f; done
