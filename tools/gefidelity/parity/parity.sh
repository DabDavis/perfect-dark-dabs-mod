#!/bin/sh
# The converter twin gate: tools/geconvert/geconvert.py and port/src/geconvert.c
# must write the same conversion. See README.md.
#
#   parity.sh [--bin PD] [--standalone] [--tree DIR] [--rom GE.z64] [--pdrom PD.z64] [--out DIR]
#
#   --bin PD       the game binary whose converter is the C side (default
#                  ~/wt/gefidelity-run/pd.base); it is booted once in a scratch
#                  run directory of its own and converts at startup
#   --standalone   instead build port/src/geconvert.c alone (-DGECONVERT_MAIN)
#                  from --tree and run that: no game build needed
#   --tree DIR     the source tree both converters come from (default: this
#                  script's own tree)
#   --out DIR      where the two conversions go (default ~/wt/gefidelity-run/parity-out)
#
# Exit: 0 identical, 1 differs, 2 a converter failed, 3 the null failed.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
TREE=$(cd "$HERE/../../.." && pwd)
BIN=$HOME/wt/gefidelity-run/pd.base
ROM="$HOME/wt/gefidelity-run/added-content/GoldenEye 007 (U) [!].n64"
PDROM=$HOME/wt/gefidelity-run/data/pd.ntsc-final.z64
OUT=$HOME/wt/gefidelity-run/parity-out
STANDALONE=0
while [ $# -gt 0 ]; do
	case $1 in
	--bin) BIN=$2; shift 2;;
	--standalone) STANDALONE=1; shift;;
	--tree) TREE=$(cd "$2" && pwd); shift 2;;
	--rom) ROM=$2; shift 2;;
	--pdrom) PDROM=$2; shift 2;;
	--out) OUT=$2; shift 2;;
	*) echo "parity: unknown argument $1" >&2; exit 2;;
	esac
done
[ -f "$ROM" ] || { echo "parity: no GoldenEye ROM at $ROM" >&2; exit 2; }
rm -rf "$OUT/py" "$OUT/c" "$OUT/crun"
mkdir -p "$OUT"
ROMABS=$(readlink -f "$ROM")

# A: the Python converter (about 80 s), in the background
( GE_ROM="$ROMABS" python3 "$TREE/tools/geconvert/geconvert.py" "$OUT/py" > "$OUT/py.log" 2>&1; echo $? > "$OUT/py.status" ) &
PYPID=$!

# B: the C converter
if [ $STANDALONE = 1 ]; then
	mkdir -p "$OUT/crun"
	gcc -O2 -DGECONVERT_MAIN -I"$TREE/port/include" -I"$TREE/include" -I"$TREE/src/include" \
		"$TREE/port/src/geconvert.c" -o "$OUT/crun/geconvert" -lz -lm > "$OUT/c.log" 2>&1 \
		&& "$OUT/crun/geconvert" "$ROMABS" "$OUT/c" >> "$OUT/c.log" 2>&1
	CST=$?
	CDIR=$OUT/c
	CWHO="port/src/geconvert.c built alone from $TREE"
else
	[ -x "$BIN" ] || { echo "parity: no game binary at $BIN" >&2; kill $PYPID; exit 2; }
	[ -f "$PDROM" ] || { echo "parity: no Perfect Dark ROM at $PDROM" >&2; kill $PYPID; exit 2; }
	mkdir -p "$OUT/crun/data" "$OUT/crun/added-content" "$OUT/crun/mods"
	ln -s "$(readlink -f "$PDROM")" "$OUT/crun/data/pd.ntsc-final.z64"
	ln -s "$ROMABS" "$OUT/crun/added-content/$(basename "$ROM")"
	cp "$BIN" "$OUT/crun/pd"
	# the converter runs at startup, before the mods mount; a few frames is enough
	( cd "$OUT/crun" && SDL_AUDIODRIVER=dummy SDL_VIDEODRIVER=offscreen timeout -k 5 300 \
		./pd --savedir save --skip-intro --no-sound --exit-frame 3 --log > c.out 2>&1 )
	CST=$?
	CDIR="$OUT/crun/mods/GoldenEye Arenas"
	[ -f "$CDIR/CONVERT.txt" ] || CST=2
	CWHO="the game's converter in $BIN ($(head -1 "$CDIR/CONVERT.txt" 2>/dev/null))"
fi
wait $PYPID
PST=$(cat "$OUT/py.status" 2>/dev/null || echo 2)
if [ "$PST" != 0 ]; then echo "parity: the Python converter failed, see $OUT/py.log" >&2; tail -5 "$OUT/py.log" >&2; exit 2; fi
if [ "$CST" != 0 ]; then echo "parity: the C converter failed ($CST), see $OUT/crun" >&2; exit 2; fi
echo "A: tools/geconvert/geconvert.py from $TREE"
echo "B: $CWHO"
python3 "$HERE/compare.py" "$OUT/py" "$CDIR" --json "$OUT/parity.json"
