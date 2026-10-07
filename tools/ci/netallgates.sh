#!/bin/bash
# netallgates.sh — every netplay gate, one at a time, then the replay gate.
#
#   netallgates.sh [LOG]   LOG defaults to build/netallgates.log
#
# Builds Linux (build/) and mingw (build-win/, re-running cmake first so new
# source files link), runs each tools/ci/net*test.sh alone (several are load
# sensitive: CLAUDE-notes/netplay.md lists them), the replay gate against
# build/pd-base.x86_64 (the pre-netplay baseline), pdlobbyd's tests and
# pd-nettest. Read the log: a gate prints "exit N" and its last lines.
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT" || exit 2
LOG=${1:-$ROOT/build/netallgates.log}
GATES=${GATES:-"netclocktest netsessiontest netplayertest netsnaptest netpuppettest neteventtest netlobbytest netlobbyuitest netlobbywinetest netnattest netpredicttest netlagcomptest netscenariotest netcontenttest netjointest netwidetest nettwelvetest netcooptest"}
{
cmake -Bbuild . >/dev/null && cmake --build build -j8 2>&1 | grep -E "error|Built target pd$"
cmake -Bbuild-win . >/dev/null 2>&1 && cmake --build build-win -j8 2>&1 | grep -E " error|Built target pd$"
for t in $GATES; do
	[ -x tools/ci/$t.sh ] || { echo "MISSING $t"; continue; }
	echo "== $t"; s0=$(date +%s)
	SDL_VIDEODRIVER=offscreen timeout 3600 tools/ci/$t.sh > build/gate-$t.log 2>&1
	echo "exit $? ($(( $(date +%s) - s0 )) s)"; tail -3 build/gate-$t.log
done
echo "== replay"; SDL_VIDEODRIVER=offscreen tools/ci/replaytest.sh compare pd-base.x86_64 pd.x86_64 2>&1 | grep -E "^(same|DIFF|fail)"
python3 tools/pdlobbyd/test_pdlobbyd.py 2>&1 | tail -2
./build/pd-nettest 2>&1 | tail -1
} > "$LOG" 2>&1
