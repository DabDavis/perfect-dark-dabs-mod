#!/bin/bash
# netclocktest.sh — does netplay's fixed-tick main loop play the stock game?
#
# Runs replaytest.sh's 0x32 match case (80 sims, seeded, --fixed-step) once
# through the stock loop and once per tick count through mainNetFrame with
# --net-clock-test N (N tick passes per presented frame, every pass but the
# last one's display list dropped, one pad sample per tick: PLANS/netplay/
# spec-tick.md), hashing the state at every level frame. Each N must print the
# stock run's lines exactly.
#
#   netclocktest.sh [BIN]     BIN a file name in build/ (pd.x86_64) or a path
#
# Env: FRAMES (default 3000), STEP (1: every frame), SEED (12345), TICKS
# ("2 3"), OUT (build/netclock-out). Needs the ROM in build/data and an
# offscreen-capable GPU driver; runs go side by side.
# Exit status: 0 identical, 1 a divergence, 2 a run that failed.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=${BUILD:-$ROOT/build}
FRAMES=${FRAMES:-3000}; STEP=${STEP:-1}; SEED=${SEED:-12345}
TICKS=${TICKS:-"2 3"}; OUT=${OUT:-$BUILD/netclock-out}
MODDIR=${MODDIR:-mod_allinone}
BIN=${1:-pd.x86_64}
case $BIN in /*) ;; */*) BIN=$(realpath "$BIN") ;; *) BIN=$BUILD/$BIN ;; esac
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
# no controller may reach the run (see replaytest.sh)
export SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-offscreen} SDL_GAMECONTROLLER_IGNORE_DEVICES=0x054c/0x0ce6,0x045e/0x028e \
	SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT=0x0000/0x0000 SDL_JOYSTICK_HIDAPI=0

# run LABEL [ARGS...] -> $OUT/LABEL.hash; replaytest.sh's "match" case
run() {
	local label=$1; shift
	local save=$OUT/save-$label log=$OUT/$label.log
	rm -rf "$save"; mkdir -p "$save"
	( cd "$BUILD" && timeout -k 5 $((FRAMES / 10 + 300)) "$BIN" --moddir "$MODDIR" \
		--savedir "$save" --skip-intro --no-sound \
		--boot-stage 0x32 --mpsims 80 --spectate --endless \
		--rng-seed "$SEED" --fixed-step --state-hash "$STEP" --exit-frame "$FRAMES" \
		"$@" > "$log" 2>&1 )
	local rc=$?
	grep -o 'statehash: frame [0-9]* [0-9a-f]*' "$log" > "$OUT/$label.hash"
	if [ $rc -ne 0 ] || ! grep -q "exit-frame $FRAMES reached" "$log"; then
		echo "FAIL $label: exit $rc, no exit-frame line (see $log)"
		return 2
	fi
	echo "ran  $label: $(wc -l < "$OUT/$label.hash") hashes to frame $FRAMES"
}

pids=()
run stock & pids+=($!)
for n in $TICKS; do
	run "ticks$n" --net-clock-test "$n" & pids+=($!)
done
status=0
for p in "${pids[@]}"; do
	wait "$p" || status=2
done
[ $status -eq 0 ] || exit 2

for n in $TICKS; do
	if [ ! -s "$OUT/stock.hash" ]; then
		echo "FAIL: the stock run hashed nothing"; exit 2
	fi
	if ! grep -q "net: clock test, $n ticks" "$OUT/ticks$n.log"; then
		echo "FAIL ticks$n: the binary has no --net-clock-test"; status=2; continue
	fi
	if cmp -s "$OUT/stock.hash" "$OUT/ticks$n.hash"; then
		echo "same ticks$n ($(wc -l < "$OUT/stock.hash") frames)"
	else
		paste -d' ' "$OUT/stock.hash" "$OUT/ticks$n.hash" | awk -v n="$n" '$4 != $8 || $3 != $7 {
			printf "DIFF ticks%s: first at frame %s (%s vs %s)\n", n, $3, $4, $8; found=1; exit }
			END { if (!found) printf "DIFF ticks%s: runs of different length\n", n }'
		status=1
	fi
done
exit $status
