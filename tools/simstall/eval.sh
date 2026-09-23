#!/bin/bash
# Run probe.sh over G5, Complex, Pipes and Skedar, three seeds each, with
# NormalSims and with 500% DarkSims: 24 three-minute matches, eight at once.
#
#   tools/simstall/eval.sh <builddir> <binary> <tag>
#   python3 tools/simstall/summary.py $OUT/<tag>
D=$(dirname "$(readlink -f "$0")")
B=$1; BIN=$2; TAG=$3
OUT=${OUT:-/tmp/simprobe}
for st in 0x20 0x1f 0x29 0x32; do for sd in 1 2 3; do
	echo "BIN=$BIN DIFF=2 SEED=$sd $D/probe.sh $B $st $TAG-n-$st-$sd"
	echo "BIN=$BIN DIFF=5 SPEED=40 SEED=$sd $D/probe.sh $B $st $TAG-f-$st-$sd"
done; done | OUT=$OUT FALLLINE=$FALLLINE xargs -P 8 -I{} bash -c "{} >/dev/null 2>&1"
