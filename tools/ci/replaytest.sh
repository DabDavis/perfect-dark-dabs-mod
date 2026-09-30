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
# note when the GoldenEye ROM has not been converted. Then the mod's own
# options, which are all off in the cases above: optmatch is the 0x32 match
# with jump, combat roll, random start weapons, akimbo, melee combos and
# flinch on for everyone; optsolo is the 0x34 mission with Guards Alerted
# (80, fast spawns, random weapons; the alarm raised at frame 200 by
# --alarm-at, since nobody trips one), akimbo guards and mission respawn; and
# the Randomizer: randrun lands a run on G5 (0x1e, seed 12345, no hops) and
# randmission deals the 0x34 mission again (to frame 1500 at most). Env: FRAMES (default
# 3000), STEP (100), SEED (12345), CASES (all eight), GOLDEN
# (build/replay-golden), OUT (build/replay-out), MODDIR (mod_allinone), EXTRA
# (more arguments for every run, e.g. EXTRA="--simbrain modern" for the
# simulants' modern movement; none by default). Needs the ROM in build/data and an
# offscreen-capable GPU driver (SDL_VIDEODRIVER=offscreen). The mission cases
# pass --skip-cutscenes: Attack Ship's opening runs past frame 1400, and a
# case that hashes only a cutscene tests very little.
#
# Exit status: 0 identical, 1 a divergence, 2 a run that failed.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=$ROOT/build
FRAMES=${FRAMES:-3000}; STEP=${STEP:-100}; SEED=${SEED:-12345}
CASES=${CASES:-"match solo gematch gesolo optmatch optsolo randrun randmission"}
GOLDEN=${GOLDEN:-$BUILD/replay-golden}; OUT=${OUT:-$BUILD/replay-out}
MODDIR=${MODDIR:-mod_allinone}
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)  # runs cd into build/, so no relative paths
case $GOLDEN in /*) ;; *) GOLDEN=$PWD/$GOLDEN ;; esac
EXTRA=${EXTRA:-}
export SDL_VIDEODRIVER=offscreen SDL_GAMECONTROLLER_IGNORE_DEVICES=0x054c/0x0ce6

caseargs() {
	case $1 in
	match) echo "--boot-stage 0x32 --mpsims 80 --spectate --endless" ;;
	solo)  echo "--boot-stage 0x34 --skip-cutscenes" ;;
	gematch) echo "--boot-map Complex --mpsims 16 --spectate --endless" ;;
	gesolo)  echo "--boot-ge-mission 0 --skip-cutscenes" ;;
	optmatch) echo "--boot-stage 0x32 --mpsims 80 --spectate --endless" ;;
	optsolo) echo "--boot-stage 0x34 --skip-cutscenes --alarm-at 200" ;;
	randrun) echo "--boot-stage 0x1e --random-run --run-stage 0x1e" ;;
	randmission) echo "--boot-stage 0x34 --random-mission --skip-cutscenes" ;;
	*) echo "unknown case $1" >&2; exit 2 ;;
	esac
}

# caseframes CASE: FRAMES, or less for a case that cannot run that long. A
# Randomizer run with nobody at the controls is over when its player dies
# (frame ~815 on seed 12345), and the level loads again from frame 0.
caseframes() {
	case $1 in
	randrun) echo $((FRAMES < 750 ? FRAMES : 750)) ;;
	# the dealt mission's player dies at frame ~1608 (seed 12345) and the
	# endscreen stops the level's frame count there, so --exit-frame past it
	# is never reached and the run only ends at the timeout
	randmission) echo $((FRAMES < 1500 ? FRAMES : 1500)) ;;
	*) echo "$FRAMES" ;;
	esac
}

# caseini CASE: the [Mod] lines a case's pd.ini starts with, if any
caseini() {
	case $1 in
	ge*) printf 'ModDir=\nMapMods=GoldenEye Arenas\n' ;;
	optmatch) printf 'JumpHeight=3\nJumpFor=0\nCombatRoll=1\nStartArmed=2\nStartArmedFor=0\nAkimbo=2\nMeleeCombos=1\nFlinchWhenShot=1\n' ;;
	optsolo) printf 'GuardsAlerted=1\nAlertedGuards=80\nGuardSpawnSpeed=10\nGuardWeapons=1\nAkimbo=3\nMissionRespawn=1\nFlinchWhenShot=1\n' ;;
	rand*) printf 'RandomizerSeed=12345\n' ;;
	esac
}

# run BIN CASE LABEL -> $OUT/LABEL.CASE.hash
run() {
	local bin=$1 c=$2 label=$3 log=$OUT/$3.$2.log frames; frames=$(caseframes "$2")
	case $bin in /*) ;; */*) bin=$(realpath "$bin") ;; *) bin=$BUILD/$bin ;; esac
	local save=$OUT/save-$label-$c moddir="--moddir $MODDIR"
	rm -rf "$save"; mkdir -p "$save"
	case $c in ge*) moddir= ;; esac
	local ini; ini=$(caseini "$c")
	[ -n "$ini" ] && printf '[Mod]\n%s\n' "$ini" > "$save/pd.ini"
	( cd "$BUILD" && timeout -k 5 $((frames / 10 + 300)) "$bin" $moddir \
		--savedir "$save" --skip-intro --no-sound $(caseargs "$c") \
		--rng-seed "$SEED" --fixed-step --state-hash "$STEP" --exit-frame "$frames" \
		${EXTRA:-} > "$log" 2>&1 )
	local rc=$?
	if grep -q "boot map .*no such map\|boot GE mission .*not registered" "$log"; then
		echo "skip $label $c: GE Plus is not converted here (see $log)"
		: > "$OUT/$label.$c.hash"
		return 3
	fi
	grep -o 'statehash: frame [0-9]* [0-9a-f]*' "$log" > "$OUT/$label.$c.hash"
	if [ $rc -ne 0 ] || ! grep -q "exit-frame $frames reached" "$log"; then
		echo "FAIL $label $c: exit $rc, no exit-frame line (see $log)"
		return 2
	fi
	echo "ran  $label $c: $(wc -l < "$OUT/$label.$c.hash") hashes to frame $frames"
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
		run "$1" "$c" a > "$OUT/a.$c.status" & run "$2" "$c" b > "$OUT/b.$c.status" & wait
		cat "$OUT/a.$c.status" "$OUT/b.$c.status"
		if grep -q '^FAIL' "$OUT/a.$c.status" "$OUT/b.$c.status"; then status=2; continue; fi
		if { [ -s "$OUT/a.$c.hash" ] && [ ! -s "$OUT/b.$c.hash" ]; } \
				|| { [ ! -s "$OUT/a.$c.hash" ] && [ -s "$OUT/b.$c.hash" ]; }; then
			echo "FAIL $c: only one binary produced hashes"; status=2; continue
		fi
		cmpcase "$OUT/a.$c.hash" "$OUT/b.$c.hash" "$c" || status=1
	done ;;
self)
	[ $# -eq 1 ] || { echo "self BIN"; exit 2; }
	for c in $CASES; do
		run "$1" "$c" a > "$OUT/a.$c.status" & run "$1" "$c" b > "$OUT/b.$c.status" & wait
		cat "$OUT/a.$c.status" "$OUT/b.$c.status"
		if grep -q '^FAIL' "$OUT/a.$c.status" "$OUT/b.$c.status"; then status=2; continue; fi
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
	sed -n '2,24p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
exit $status
