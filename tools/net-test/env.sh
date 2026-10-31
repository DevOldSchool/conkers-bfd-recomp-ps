# Shared settings for the online test scripts (sourced). Set CONKER_ROM to your ROM and NET_TEST_SAVE
# to an EEPROM save that has MULTI open (conker_us.bin from a game past the first chapters); runs go
# under NET_TEST_DIR (default /tmp/conker-net-test), logs in its logs/ folder.
REPO=${0:A:h:h:h}
BIN=${BIN:-$REPO/build/rt64/ConkersBadFurDayRecompiled}
ROM=${CONKER_ROM:?set CONKER_ROM to the US ROM}
SAVE=${NET_TEST_SAVE:?set NET_TEST_SAVE to a save with MULTI open}
W0=${NET_TEST_DIR:-/tmp/conker-net-test}
mkdir -p $W0/logs
WINID=$W0/winid
[[ -x $WINID ]] || swiftc -O ${0:A:h}/winid.swift -o $WINID 2>/dev/null
