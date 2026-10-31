#!/bin/zsh
# exitrun.sh NAME lobby|session|playing [UPNP NATPMP]: a host maps its port on the stand-in gateway
# (lobby: from its lobby; session: --net-host --net-map, still waiting for its guest; playing: the
# same with a guest from this Mac playing), is stopped with SIGTERM after 12 s (playing: 30 s), and
# the stand-in must have seen the mapping removed. UPNP/NATPMP are fake_gateway.py's --upnp/--natpmp
# (default ok ok).
source ${0:A:h}/env.sh
K=${0:A:h}; NAME=$1; HOW=$2; L=$W0/logs/$NAME; D=$W0/exit
(( $(pgrep ConkersBadFurDayRecompiled | wc -l) == 0 )) || { echo "games still running"; exit 1; }
mkdir -p $D/conker-recomp-data/saves; cp $SAVE $D/conker-recomp-data/saves/conker_us.bin
python3 $REPO/tools/fake_gateway.py --http-port 38111 --upnp ${3:-ok} --natpmp ${4:-ok} > $L.gateway.log 2>&1 & GWP=$!
sleep 1
export CONKER_NET_UPNP_URL=http://127.0.0.1:38111/rootDesc.xml CONKER_NET_NATPMP_GATEWAY=127.0.0.1
export CONKER_AUDIO=0 CONKER_DISPLAY=windowed CONKER_ORIGINAL_SIZE=1
print "1000 host 37810" > $D/host.script
# A mapping test: nothing is mapped without --net-unsafe-internet (the stand-in gateway only, still).
if [[ $HOW == lobby ]]; then ARGS=(--net-lobby --net-unsafe-internet); else ARGS=(--net-host 37810 --net-map --net-unsafe-internet); fi
(cd $D && CONKER_LOBBY_SCRIPT=$D/host.script exec $BIN $ROM $ARGS > $L.out 2>&1) & P=$!
if [[ $HOW == playing ]]; then
  mkdir -p $D/g; (cd $D/g && CONKER_NET_PORTMAP=0 exec $BIN $ROM --net-join 127.0.0.1:37810 > $L.g.out 2>&1) & G=$!
  sleep 30
else
  sleep 12
fi
kill -TERM $P
for i in {1..20}; do kill -0 $P 2>/dev/null || break; sleep 0.5; done
kill -0 $P 2>/dev/null && { echo "still running after 10 s: killed"; kill -9 $P; }
[[ -n $G ]] && kill -9 $G
sleep 0.5; kill $GWP
echo "== $NAME ($HOW)"; grep -i "map\|delete\|remov" $L.gateway.log; grep -h "net: .*\(router\|port\|mapping\)" $L.out
