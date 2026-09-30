#!/bin/bash
# replaytest.sh — does a build still play the same game?
#
# Runs seeded, fixed-step headless replays and logs a hash of the simulation's
# state every STEP level frames (--state-hash, port/src/statehash.c). Two runs
# that play the same game print the same lines; the first differing line is the
# frame a divergence reached the state.
#
#   replaytest.sh compare BIN_A BIN_B   both binaries, every case, diff the hashes
#   replaytest.sh record  BIN           write the golden hashes (GOLDEN dir)
#   replaytest.sh check   BIN           compare BIN against the golden hashes
#   replaytest.sh self    BIN           the same binary twice: is a run repeatable at all?
#
# BIN is a file name in build/ (pd.x86_64) or a path. Cases: an 80-sim match on
# 0x32 and a solo mission on 0x34 (the match alone missed the pad2.flags read,
# CLAUDE-notes/performance.md), and GE Plus's own: a 16-sim match on its
# Complex arena (gematch) and GoldenEye's Dam mission (gesolo), found by name
# (--boot-map, --boot-ge-mission) since a converted map's id depends on the
# mods installed. The GE cases run without --moddir (which mounts no map mods)
# and with Mod.MapMods seeded to "GoldenEye Arenas"; they are skipped with a
# note when the GoldenEye ROM has not been converted. Env: FRAMES (default
# 3000), STEP (100), SEED (12345), CASES ("match solo gematch gesolo"), GOLDEN
# (build/replay-golden), OUT (build/replay-out), MODDIR (mod_allinone), EXTRA
# (more arguments for every run). Needs the ROM in build/data and an
# offscreen-capable GPU driver (SDL_VIDEODRIVER=offscreen).
#
# Exit status: 0 identical, 1 a divergence, 2 a run that failed.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=$ROOT/build
FRAMES=${FRAMES:-3000}; STEP=${STEP:-100}; SEED=${SEED:-12345}
CASES=${CASES:-"match solo gematch gesolo"}
GOLDEN=${GOLDEN:-$BUILD/replay-golden}; OUT=${OUT:-$BUILD/replay-out}
MODDIR=${MODDIR:-mod_allinone}
export SDL_VIDEODRIVER=offscreen SDL_GAMECONTROLLER_IGNORE_DEVICES=0x054c/0x0ce6

caseargs() {
	case $1 in
	match) echo "--boot-stage 0x32 --mpsims 80 --spectate --endless" ;;
	solo)  echo "--boot-stage 0x34" ;;
	gematch) echo "--boot-map Complex --mpsims 16 --spectate --endless" ;;
	gesolo)  echo "--boot-ge-mission 0" ;;
	*) echo "unknown case $1" >&2; exit 2 ;;
	esac
}

# run BIN CASE LABEL -> $OUT/LABEL.CASE.hash
run() {
	local bin=$1 c=$2 label=$3 log=$OUT/$3.$2.log
	case $bin in /*) ;; */*) bin=$(realpath "$bin") ;; *) bin=$BUILD/$bin ;; esac
	local save=$OUT/save-$label-$c moddir="--moddir $MODDIR"
	rm -rf "$save"; mkdir -p "$save"
	case $c in ge*)
		moddir=
		printf '[Mod]\nModDir=\nMapMods=GoldenEye Arenas\n' > "$save/pd.ini" ;;
	esac
	( cd "$BUILD" && timeout -k 5 $((FRAMES / 10 + 300)) "$bin" $moddir \
		--savedir "$save" --skip-intro --no-sound $(caseargs "$c") \
		--rng-seed "$SEED" --fixed-step --state-hash "$STEP" --exit-frame "$FRAMES" \
		${EXTRA:-} > "$log" 2>&1 )
	local rc=$?
	if grep -q "boot map .*no such map\|boot GE mission .*not registered" "$log"; then
		echo "skip $label $c: GE Plus is not converted here (see $log)"
		: > "$OUT/$label.$c.hash"
		return 3
	fi
	grep -o 'statehash: frame [0-9]* [0-9a-f]*' "$log" > "$OUT/$label.$c.hash"
	if [ $rc -ne 0 ] || ! grep -q "exit-frame $FRAMES reached" "$log"; then
		echo "FAIL $label $c: exit $rc, no exit-frame line (see $log)"
		return 2
	fi
	echo "ran  $label $c: $(wc -l < "$OUT/$label.$c.hash") hashes to frame $FRAMES"
}

# cmp A B CASE: first differing frame, or identical
cmpcase() {
	local a=$1 b=$2 c=$3
	if [ ! -s "$a" ] && [ ! -s "$b" ]; then
		echo "skip $c"
		return 0
	fi
	if cmp -s "$a" "$b"; then
		echo "same $c"
		return 0
	fi
	paste -d' ' "$a" "$b" | awk -v c="$c" '$4 != $8 || $3 != $7 {
		printf "DIFF %s: first at frame %s (%s vs %s)\n", c, $3, $4, $8; found=1; exit }
		END { if (!found) printf "DIFF %s: runs of different length\n", c }'
	return 1
}

mode=${1:-}; shift || true
mkdir -p "$OUT"
status=0
case $mode in
compare)
	[ $# -eq 2 ] || { echo "compare BIN_A BIN_B"; exit 2; }
	for c in $CASES; do
		run "$1" "$c" a & run "$2" "$c" b & wait
		if { [ -s "$OUT/a.$c.hash" ] && [ ! -s "$OUT/b.$c.hash" ]; } \
				|| { [ ! -s "$OUT/a.$c.hash" ] && [ -s "$OUT/b.$c.hash" ]; }; then
			echo "FAIL $c: only one binary produced hashes"; status=2; continue
		fi
		cmpcase "$OUT/a.$c.hash" "$OUT/b.$c.hash" "$c" || status=1
	done ;;
self)
	[ $# -eq 1 ] || { echo "self BIN"; exit 2; }
	for c in $CASES; do
		run "$1" "$c" a & run "$1" "$c" b & wait
		cmpcase "$OUT/a.$c.hash" "$OUT/b.$c.hash" "$c" || status=1
	done ;;
record)
	[ $# -eq 1 ] || { echo "record BIN"; exit 2; }
	mkdir -p "$GOLDEN"
	for c in $CASES; do
		run "$1" "$c" g; [ $? -eq 2 ] && status=2
		cp "$OUT/g.$c.hash" "$GOLDEN/$c.hash"
	done
	echo "golden hashes in $GOLDEN (seed $SEED, $FRAMES frames, every $STEP)" ;;
check)
	[ $# -eq 1 ] || { echo "check BIN"; exit 2; }
	for c in $CASES; do
		run "$1" "$c" t; [ $? -eq 2 ] && { status=2; continue; }
		cmpcase "$GOLDEN/$c.hash" "$OUT/t.$c.hash" "$c" || status=1
	done ;;
*)
	sed -n '2,22p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
exit $status
