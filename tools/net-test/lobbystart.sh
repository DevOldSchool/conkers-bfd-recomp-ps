#!/bin/zsh
# lobbystart.sh NAME SECS GUEST_JOIN: a lobby on this Mac (host in lob/h, guest in lob/g) that STARTs a
# session; port mapping goes to the stand-in gateway (tools/fake_gateway.py), never the router.
source ${0:A:h}/env.sh
# The host's key is fixed (CONKER_NET_TEST_HOST_KEY) so that its join code is known: GUEST_JOIN is
# this Mac's LAN address and port 37810 as a code with that key.
K=${0:A:h}; NAME=$1; SECS=$2; JOIN=${3:?usage: lobbystart.sh NAME SECS GUEST_JOIN}
TEST_KEY=7465737420686f7374206b657920666f722074686520636f6e6b657220746573

(( $(pgrep ConkersBadFurDayRecompiled | wc -l) == 0 )) || { echo "games still running"; exit 1; }
L=$W0/logs/$NAME; LB=$W0/lob; mkdir -p $LB/h/conker-recomp-data/saves $LB/g/conker-recomp-data/saves
cp $SAVE $LB/h/conker-recomp-data/saves/conker_us.bin; rm -rf $LB/g/conker-recomp-data/saves/*(N) $LB/g/conker-recomp-data/online.txt $LB/h/conker-recomp-data/online.txt
cat > $LB/$NAME.h.script <<S2
1500 name Host
2000 host 37810
6000 shot $L.h.view1
12000 shot $L.h.view2
12500 setenv CONKER_VIRTUAL_PAD=$K/f1.pad
13000 start
S2
cat > $LB/$NAME.g.script <<S2
1500 name Guest
5000 shot $L.g.view1
5500 join $JOIN
8000 shot $L.g.view2
8500 ready
11000 shot $L.g.view3
11500 setenv CONKER_VIRTUAL_PAD=$K/f2.pad
S2
python3 $REPO/tools/fake_gateway.py --http-port 38111 > $L.gateway.log 2>&1 & GW=$!
sleep 1
export CONKER_NET_UPNP_URL=http://127.0.0.1:38111/rootDesc.xml CONKER_NET_NATPMP_GATEWAY=127.0.0.1
export CONKER_AUDIO=0 CONKER_DISPLAY=windowed CONKER_ORIGINAL_SIZE=1
run() { (cd $LB/$1 && env ${2:-CONKER_NET_NONE=1} CONKER_LOBBY_SCRIPT=$LB/$NAME.$1.script CONKER_FRAME_INPUT_LOG=$L.$1.input.log \
  CONKER_STATE_HASH_LOG=$L.$1.hash.log CONKER_STATE_HASH_EVERY=30 CONKER_FRAME_LOG=$L.$1.frame.log $BIN $ROM --net-lobby > $L.$1.out 2>&1 &) }
run h CONKER_NET_TEST_HOST_KEY=$TEST_KEY; sleep 1; run g
start=$SECONDS
sleep 7; for w in $($WINID | awk '$1 {print $1}'); do screencapture -x -o -l$w $L.shot7.$w.png 2>/dev/null; done
sleep 4; for w in $($WINID | awk '$1 {print $1}'); do screencapture -x -o -l$w $L.shot11.$w.png 2>/dev/null; done
sleep $(( SECS - 11 ))
pkill -9 -f "^$BIN $ROM"; kill $GW
echo "== $NAME ($((SECONDS-start)) s)"
for r in h g; do [[ -f $L.$r.frame.log ]] && echo "$r frames $(wc -l < $L.$r.frame.log)"; done
