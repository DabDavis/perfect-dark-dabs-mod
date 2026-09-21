#!/bin/bash
# How much of their walking time do the simulants spend going nowhere?
#
#   tools/simstall/run.sh <builddir> <stage id> <name> [samples]
#
# Boots a Combat Simulator match headlessly on the GPU, samples every bot's
# position and action about once a second from gdb, and prints the stalled
# share (metric.py) and each stall of three samples or more (stuck.py).
# Output goes to $OUT (default /tmp/simstall).
#
#   SIMS=4      how many simulants
#   DIFF=0      force every simulant's difficulty (0 is a MeatSim); --mpsims
#               leaves them at 6, which no menu offers
#   XFLAGS=...  extra flags; leave --spectate OUT - see CLAUDE-notes/
#               chrs-and-memory.md, "Simulants running on the spot"
#
# One run is noise: stock upstream measured 2% and 21% on the same arena. Run
# both binaries at least twice and read stuck.py's places, not only the share.
D=$(dirname "$(readlink -f "$0")")
B=$1; ST=$2; NAME=$3; N=${4:-100}
OUT=${OUT:-/tmp/simstall}
mkdir -p "$OUT"; rm -rf "$OUT/save-$NAME"; mkdir -p "$OUT/save-$NAME"
cd "$B" || exit 1
SDL_VIDEODRIVER=offscreen ./pd.x86_64 --savedir "$OUT/save-$NAME" --skip-intro --no-sound \
	--boot-stage "$ST" --mpsims "${SIMS:-4}" ${XFLAGS---endless} --rng-seed 1 --log > "$OUT/$NAME.log" 2>&1 &
PID=$!
sleep 12
if [ -n "$DIFF" ]; then
	gdb -q -batch -p $PID -ex "python [gdb.execute('set var g_BotConfigsArray[%d].difficulty=$DIFF' % i) for i in range(8)]" >/dev/null 2>&1
fi
for i in $(seq 1 "$N"); do
	gdb -q -batch -p $PID -x "$D/sample.gdb" 2>/dev/null | grep "^F "
	sleep 1
done > "$OUT/$NAME.txt"
kill $PID
python3 "$D/metric.py" "$OUT/$NAME.txt"
python3 "$D/stuck.py" "$OUT/$NAME.txt"
