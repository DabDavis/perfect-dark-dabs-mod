#!/bin/sh
# moddatadump.sh NAME args...: boot, write --mod-dump-data to $OUTDIR/dump-NAME.txt, exit at frame 2.
# SELECT=mod: boot with the mod chosen in pd.ini (a copy of STOCKINI, a stock run's pd.ini) -
# the restart path to compare a --mod-data-swap run against (CLAUDE-notes/mods.md, "A mod entered live: data")
n=$1; shift
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
d=${OUTDIR:-$ROOT/build/moddatadump}; mkdir -p $d
rm -rf $d/save-$n; mkdir -p $d/save-$n
if [ -n "$SELECT" ]; then sed "s|^ModDir=.*|ModDir=$SELECT|" ${STOCKINI:-$d/stock.ini} > $d/save-$n/pd.ini; fi
cd $ROOT/build
SDL_VIDEODRIVER=offscreen flock ${GAMELOCK:-/home/sdg/wt/pdmods/game.lock} timeout -k 5 ${TMO:-120} ./pd.x86_64 --savedir $d/save-$n --skip-intro --no-sound --log --exit-frame ${FRAME:-2} --mod-dump-data $d/dump-$n.txt "$@" > $d/log-$n.txt 2>&1
echo "$n exit $?"
