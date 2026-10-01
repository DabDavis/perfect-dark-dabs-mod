#!/bin/sh
# Convert the GoldenEye ROM with one source tree's port/src/geconvert.c, built
# alone (-DGECONVERT_MAIN): no game build, no game run.
#
#   convert.sh --tree SRC --out DIR [--rom GE.z64]
#
# Writes DIR/conv (the conversion), DIR/convert.log, DIR/stamp (the tree's
# commit and GECONVERT_VERSION). Exit 0 converted, 2 the build or the
# conversion failed.
set -u
TREE=""
OUT=""
ROM="$HOME/wt/gefidelity-run/added-content/GoldenEye 007 (U) [!].n64"
while [ $# -gt 0 ]; do
	case $1 in
	--tree) TREE=$(cd "$2" && pwd); shift 2;;
	--out) OUT=$2; shift 2;;
	--rom) ROM=$2; shift 2;;
	*) echo "convert: unknown argument $1" >&2; exit 2;;
	esac
done
[ -n "$TREE" ] && [ -n "$OUT" ] || { echo "convert: --tree and --out" >&2; exit 2; }
[ -f "$ROM" ] || { echo "convert: no GoldenEye ROM at $ROM" >&2; exit 2; }
rm -rf "$OUT/conv" "$OUT/bin"
mkdir -p "$OUT/bin"
gcc -O2 -DGECONVERT_MAIN -I"$TREE/port/include" -I"$TREE/include" -I"$TREE/src/include" \
	"$TREE/port/src/geconvert.c" -o "$OUT/bin/geconvert" -lz -lm > "$OUT/convert.log" 2>&1 \
	|| { echo "convert: the build failed, see $OUT/convert.log" >&2; exit 2; }
"$OUT/bin/geconvert" "$(readlink -f "$ROM")" "$OUT/conv" >> "$OUT/convert.log" 2>&1 \
	|| { echo "convert: the conversion failed, see $OUT/convert.log" >&2; exit 2; }
{ git -C "$TREE" log --oneline -1 2>/dev/null; grep -h 'GECONVERT_VERSION_STR' "$TREE/port/include/geconvert.h"; } > "$OUT/stamp"
exit 0
