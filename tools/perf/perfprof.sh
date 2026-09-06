#!/bin/bash
# perfprof.sh BIN LABEL [STAGE] [SIMS] — same match as perfrun.sh, sampled with perf for 20s
BIN=$1; LABEL=$2; STAGE=${3:-0x32}; SIMS=${4:-80}
SCR=${PDPERF_DIR:-/tmp/pdperf}
cd $(dirname "$0")/../../build
export SDL_VIDEODRIVER=offscreen SDL_GAMECONTROLLER_IGNORE_DEVICES=0x054c/0x0ce6
timeout -k 5 60 ./$BIN --moddir mod_allinone --savedir $SCR/pdsave --skip-intro --no-sound --boot-stage $STAGE --mpsims $SIMS --spectate --endless --rng-seed ${SEED:-12345} --fixed-step > $SCR/$LABEL.log 2>&1 &
GP=$!; sleep 25; PID=$(pgrep -x $BIN)
sudo perf record -F 1000 --call-graph dwarf,16384 -t $PID -o $SCR/$LABEL.data -- sleep 20 2>&1 | tail -1
wait $GP; sudo chown sdg $SCR/$LABEL.data
echo "== flat (main thread)"; perf report -i $SCR/$LABEL.data --no-children --sort symbol --stdio -g none 2>/dev/null | grep -v "^#" | grep -v "^$" | sed 's/  */ /g' | cut -c1-110 | head -${TOP:-40}
echo "== inclusive"; perf report -i $SCR/$LABEL.data --children --sort symbol --stdio -g none 2>/dev/null | grep -v "^#" | grep -v "^$" | sed 's/  */ /g' | cut -c1-110 | grep -v "0x0000" | head -${TOP:-40}
