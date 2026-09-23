#!/bin/bash
# Play one seeded Combat Simulator match headlessly under gdb and record how the
# simulants get about (probe.gdb). The match is the same frame for frame each
# time (--rng-seed, --fixed-step), so two binaries can be compared on it.
#
#   tools/simstall/probe.sh <builddir> <stage id> <name> [exit frame]
#
#   BIN=pd.x86_64  the binary in <builddir>; keep the one to compare with
#                  beside it under another name
#   SIMS=4 SEED=1  simulants and seed
#   DIFF=2         force every simulant's difficulty (5 is DarkSim)
#   SPEED=40       force the speed slider (tenths over stock: 40 is 500%)
#   FALLLINE=N     chr.c line of the fall death's chrDie(); found in the
#                  working tree if not given, so give it for another binary
#   DETAIL=1       print the route state at each new stall
#
# Output goes to $OUT/<name>.probe (default /tmp/simprobe); summary.py reads it.
D=$(dirname "$(readlink -f "$0")")
B=$1; ST=$2; NAME=$3; EF=${4:-10800}
OUT=${OUT:-/tmp/simprobe}
mkdir -p "$OUT"; rm -rf "$OUT/save-$NAME"; mkdir -p "$OUT/save-$NAME"
FALLLINE=${FALLLINE:-$(grep -n 'chrDie(chr, shooter);' "$D/../../src/game/chr.c" | head -1 | cut -d: -f1)}
cd "$B" || exit 1
FALLLINE=$FALLLINE SDL_VIDEODRIVER=offscreen timeout -k 5 900 gdb -q -batch -x "$D/probe.gdb" \
	--args ./${BIN:-pd.x86_64} --savedir "$OUT/save-$NAME" --skip-intro --no-sound \
	--boot-stage "$ST" --mpsims "${SIMS:-4}" --endless --rng-seed "${SEED:-1}" --fixed-step --exit-frame "$EF" \
	> "$OUT/$NAME.probe" 2>&1
