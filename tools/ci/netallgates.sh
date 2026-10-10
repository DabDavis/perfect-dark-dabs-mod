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
#
# A gate that fails is run once more (RERUN=0 turns that off): passing the
# second time it counts as FLAKY, not failed, and its first run's log is kept
# as build/gate-NAME.try1.log; failing twice it is FAIL. The log ends with a
# summary: "== summary", then "pass N, flaky N, FAIL N" and one line per
# gate that was not a plain pass. The replay gate is not rerun: it is
# deterministic, and a DIFF is a real one. The script exits 0 when nothing
# failed (a FLAKY gate passed), 1 when anything did, replay, pdlobbyd and
# pd-nettest included.
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT" || exit 2
LOG=${1:-$ROOT/build/netallgates.log}
RERUN=${RERUN:-1}
GATES=${GATES:-"netclocktest netsessiontest netplayertest netsnaptest netpuppettest neteventtest netlobbytest netlobbyuitest netlobbywinetest netnattest netpredicttest netlagcomptest netscenariotest netcontenttest netjointest netchattest netwidetest nettwelvetest netcooptest netmigratetest"}

rungate() {
	local t=$1 s0 rc
	s0=$(date +%s)
	SDL_VIDEODRIVER=offscreen timeout 3600 tools/ci/$t.sh > build/gate-$t.log 2>&1
	rc=$?
	echo "exit $rc ($(( $(date +%s) - s0 )) s)"
	return $rc
}

{
cmake -Bbuild . >/dev/null && cmake --build build -j8 2>&1 | grep -E "error|Built target pd$"
cmake -Bbuild-win . >/dev/null 2>&1 && cmake --build build-win -j8 2>&1 | grep -E " error|Built target pd$"
npass=0; nflaky=0; nfail=0; notes=""
for t in $GATES; do
	[ -x tools/ci/$t.sh ] || { echo "MISSING $t"; nfail=$((nfail + 1)); notes="$notes\nFAIL   $t: no tools/ci/$t.sh"; continue; }
	echo "== $t"
	rm -f build/gate-$t.try1.log
	if rungate "$t"; then
		tail -3 build/gate-$t.log
		npass=$((npass + 1))
		continue
	fi
	tail -3 build/gate-$t.log
	if [ "$RERUN" = 0 ]; then
		nfail=$((nfail + 1)); notes="$notes\nFAIL   $t (not rerun)"
		continue
	fi
	mv -f build/gate-$t.log build/gate-$t.try1.log
	echo "-- $t: failed, run once more (the first run's log: build/gate-$t.try1.log)"
	if rungate "$t"; then
		tail -3 build/gate-$t.log
		echo "-- $t: FLAKY (failed once, passed on the rerun)"
		nflaky=$((nflaky + 1)); notes="$notes\nFLAKY  $t: first run in build/gate-$t.try1.log"
	else
		tail -3 build/gate-$t.log
		echo "-- $t: FAIL (failed twice)"
		nfail=$((nfail + 1)); notes="$notes\nFAIL   $t: build/gate-$t.try1.log and build/gate-$t.log"
	fi
done
echo "== replay"; SDL_VIDEODRIVER=offscreen tools/ci/replaytest.sh compare pd-base.x86_64 pd.x86_64 2>&1 | grep -E "^(same|DIFF|fail)"
[ "${PIPESTATUS[0]}" = 0 ] || { nfail=$((nfail + 1)); notes="$notes\nFAIL   replay: tools/ci/replaytest.sh compare pd-base.x86_64 pd.x86_64"; }
python3 tools/pdlobbyd/test_pdlobbyd.py 2>&1 | tail -2
[ "${PIPESTATUS[0]}" = 0 ] || { nfail=$((nfail + 1)); notes="$notes\nFAIL   pdlobbyd: python3 tools/pdlobbyd/test_pdlobbyd.py"; }
./build/pd-nettest 2>&1 | tail -1
[ "${PIPESTATUS[0]}" = 0 ] || { nfail=$((nfail + 1)); notes="$notes\nFAIL   pd-nettest: build/pd-nettest"; }
echo "== summary"
echo "gates: pass $npass, flaky $nflaky, FAIL $nfail (FAIL counts replay, pdlobbyd and pd-nettest too)"
[ -z "$notes" ] || printf "%b\n" "${notes#\\n}"
} > "$LOG" 2>&1
[ "$nfail" = 0 ]
