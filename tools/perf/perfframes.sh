#!/bin/bash
# perfframes.sh BIN LABEL [FRAMES] [PXPERSEG] [STAGE] [SIMS] — the same match as perfrun.sh,
# run to level frame FRAMES (default 2400) and counted whole with perf stat, so two binaries
# are compared over the very same frames whatever speed they run at. Startup is included
# and is the same for both. PXPERSEG, if given, is poked into the renderer's
# gfx_smooth_px_per_segment with gdb once the game is up.
BIN=$1; LABEL=$2; FRAMES=${3:-2400}; PX=${4:-}; STAGE=${5:-0x32}; SIMS=${6:-80}
SCR=${PDPERF_DIR:-/tmp/pdperf}
cd $(dirname "$0")/../../build
export SDL_VIDEODRIVER=offscreen SDL_GAMECONTROLLER_IGNORE_DEVICES=0x054c/0x0ce6
ARGS="--moddir mod_allinone --savedir $SCR/pdsave --skip-intro --no-sound --boot-stage $STAGE --mpsims $SIMS --spectate --endless --gfxstats 60 --rng-seed ${SEED:-12345} --fixed-step $EXTRA"
if [ -n "$GDBEXIT" ]; then
  # a binary without --exit-frame: gdb stops it on the frame instead
  gdb -batch -ex "break videoEndFrame if g_Vars.lvframenum >= $FRAMES" -ex run -ex kill --args ./$BIN $ARGS > $SCR/$LABEL.log 2>&1 &
else
  ./$BIN $ARGS --exit-frame $FRAMES > $SCR/$LABEL.log 2>&1 &
fi
# The main thread alone, attached once the game is up: the driver's threads
# are the GPU's side of the frame, and a counter attached before they are
# spawned would be inherited by them (--no-inherit for the ones after).
# The first seconds of startup go uncounted, the same for every binary.
sleep 4
GP=$(pgrep -x $BIN)
sudo perf stat -e cycles:u,instructions:u -x, --per-thread --no-inherit -t $GP -o $SCR/$LABEL.stat 2>/dev/null &
PP=$!
if [ -n "$PX" ]; then
  sleep 6
  gdb -batch -p $GP -ex "set var gfx_smooth_px_per_segment = $PX" -ex "print gfx_smooth_px_per_segment" -ex detach 2>/dev/null | grep '^\$'
fi
while kill -0 $GP 2>/dev/null; do sleep 1; done; wait $PP
python3 - $SCR/$LABEL.stat $SCR/$LABEL.log $FRAMES $LABEL <<'PY'
import sys,re
fr=int(sys.argv[3])
for l in open(sys.argv[1]):
    w=l.strip().split(',')
    if len(w)>3 and w[1].isdigit():
        print(f"{sys.argv[4]:18s} {w[3]:14s} {int(w[1])/fr/1e6:8.3f} M/frame over {fr} frames")
g=[l for l in open(sys.argv[2]) if l.startswith('gfx: ') and ' draws, ' in l]
d=[[int(x) for x in m[0]] for m in (re.findall(r'(\d+) draws, (\d+) tris, (\d+) verts',l) for l in g) if m]
n=len(d)
if n: print(f"  geometry/frame: {sum(x[0] for x in d)/n:.0f} draws, {sum(x[1] for x in d)/n:.0f} tris, {sum(x[2] for x in d)/n:.0f} verts ({n} samples)")
s=[l for l in open(sys.argv[2]) if 'of which smoothed' in l]
if s:
    m=[[int(x) for x in re.findall(r'drawn (\d+) of which smoothed (\d+) \(patches: (\d+)',l)[0]] for l in s]
    n=len(m); print(f"  drawn {sum(x[0] for x in m)/n:.0f}, smoothed {sum(x[1] for x in m)/n:.0f}, patches {sum(x[2] for x in m)/n:.0f} per sampled frame")
PY
