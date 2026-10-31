#!/bin/zsh
# sjrun.sh NAME PORT SECS [kill rejoin]: rejoinrun with the summary lines a snapshot rejoin prints.
source ${0:A:h}/env.sh
K=${0:A:h}; NAME=$1
while pgrep -q ConkersBadFurDayRecompiled; do sleep 10; done
$K/rejoinrun.sh "$@"
cat $W0/$NAME.result | grep -v "^\[conker\]" ; grep -h "snapshot\|SNAPSHOT\|playing again\|differs\|OUT OF SYNC at\|caught up at" $W0/logs/$NAME.*.out | sort -u
