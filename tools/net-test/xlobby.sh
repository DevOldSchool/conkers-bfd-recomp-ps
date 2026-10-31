#!/bin/zsh
# xlobby.sh NAME h|g SECS: ONE lobby peer on this machine, for runs across two machines (start the
# host on one and the guest on the other at about the same time). Runs $K/NAME.<role>.script with
# @L (log prefix) and @N (this folder) filled in; the host plays on a copy of NET_TEST_SAVE and maps
# its port on the stand-in gateway only. Env: XENV extra env, XCODE fills @CODE (a join code),
# NET_TEST_SAME_MATCH=1 to add a peer to the match already running here (role g2, say), XARGS extra
# arguments (--net-unsafe-internet for a mapping test: nothing is mapped without it). Logs in $W0/logs/NAME.<role>.*.
source ${0:A:h}/env.sh
K=${0:A:h}; NAME=$1; R=$2; SECS=$3; L=$W0/logs/$NAME; D=$W0/x$R
(( $(pgrep ConkersBadFurDayRecompiled | wc -l) == 0 )) || [[ -n $NET_TEST_SAME_MATCH ]] || { echo "games still running"; exit 1; }
mkdir -p $D/conker-recomp-data/saves; rm -rf $D/conker-recomp-data/saves/*(N) $D/conker-recomp-data/online.txt
[[ $R == h ]] && cp $SAVE $D/conker-recomp-data/saves/conker_us.bin
sed "s|@L|$L|g; s|@N|$K|g; s|@CODE|$XCODE|g" $K/$NAME.$R.script > $D/$NAME.run
if [[ $R == h ]]; then
  python3 $REPO/tools/fake_gateway.py --http-port 38111 > $L.gateway.log 2>&1 & GWP=$!; sleep 1
  export CONKER_NET_UPNP_URL=http://127.0.0.1:38111/rootDesc.xml CONKER_NET_NATPMP_GATEWAY=127.0.0.1
else
  export CONKER_NET_PORTMAP=0
fi
export CONKER_AUDIO=0 CONKER_DISPLAY=windowed CONKER_ORIGINAL_SIZE=1
(cd $D && exec env ${=XENV} CONKER_LOBBY_SCRIPT=$D/$NAME.run CONKER_FRAME_INPUT_LOG=$L.$R.input.log CONKER_STATE_HASH_LOG=$L.$R.hash.log \
  CONKER_STATE_HASH_EVERY=30 CONKER_FRAME_LOG=$L.$R.frame.log $BIN $ROM --net-lobby ${=XARGS} > $L.$R.out 2>&1) & P=$!
start=$SECONDS
while (( SECONDS - start < SECS )) && kill -0 $P 2>/dev/null; do sleep 1; done
kill -TERM $P 2>/dev/null; sleep 3; kill -9 $P 2>/dev/null; [[ -n $GWP ]] && kill $GWP
echo "== $NAME $R ($((SECONDS-start)) s): $(wc -l < $L.$R.frame.log 2>/dev/null) frames"
for f in $L.$R.view*(N); do echo "--- ${f:t}"; cat $f; done
