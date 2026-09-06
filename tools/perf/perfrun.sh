#!/bin/bash
# perfrun.sh BIN LABEL [DUR] [STAGE] [SIMS] — boots an endless 80-sim match on the real GPU
# offscreen and reports CPU ms per rendered frame for each thread of the game.
BIN=$1; LABEL=$2; DUR=${3:-40}; STAGE=${4:-0x32}; SIMS=${5:-80}
SCR=${PDPERF_DIR:-/tmp/pdperf}
S=$SCR/pdsave
cd $(dirname "$0")/../../build
export SDL_VIDEODRIVER=offscreen SDL_GAMECONTROLLER_IGNORE_DEVICES=0x054c/0x0ce6
timeout -k 5 $((DUR+40)) ./$BIN --moddir mod_allinone --savedir $S --skip-intro --no-sound --boot-stage $STAGE --mpsims $SIMS --spectate --endless --gfxstats 60 --rng-seed ${SEED:-12345} --fixed-step $EXTRA > $SCR/$LABEL.log 2>&1 &
GP=$!
sleep 25
PID=$(pgrep -x $BIN)
snap() {
  gdb -batch -p $PID -ex "print 'video.c'::frames" -ex 'print g_Vars.lvframenum' -ex detach 2>/dev/null | grep '^\$' | awk '{print "counter", $3}'
  for t in /proc/$PID/task/*; do
    c=$(cat $t/comm); st=($(cat $t/stat)); echo "thread $c $(( ${st[13]} + ${st[14]} ))"
  done
}
snap > $SCR/$LABEL.t0
GL0=$(for t in /proc/$PID/task/*; do [ "$(cat $t/comm)" = "$BIN:gl0" ] && basename $t; done)
sudo perf stat -e cycles:u,instructions:u -t $PID,$GL0 --per-thread -x, -o $SCR/$LABEL.stat -- sleep $DUR 2>/dev/null
snap > $SCR/$LABEL.t1
python3 - $SCR/$LABEL.t0 $SCR/$LABEL.t1 $LABEL $SCR/$LABEL.log <<'PY'
import sys
def load(p):
    c=[];t={}
    for l in open(p):
        w=l.split()
        if w[0]=='counter': c.append(int(w[1]))
        else: t[w[1]]=t.get(w[1],0)+int(w[2])
    return c,t
(c0,t0),(c1,t1)=load(sys.argv[1]),load(sys.argv[2])
frames=c1[0]-c0[0]; lv=c1[-1]-c0[-1]  # with one counter (a -O2 build hides video.c's), both are the level tick
print(f"{sys.argv[3]}: {frames} frames, {lv} level ticks")
for k in sorted(t1, key=lambda k:-(t1[k]-t0.get(k,0))):
    ms=(t1[k]-t0.get(k,0))*10.0
    if ms>0: print(f"  {k:16s} {ms/frames:6.2f} ms/frame  ({ms/1000:.1f}s cpu)")
PY
wait $GP
python3 - $SCR/$LABEL.stat $SCR/$LABEL.t0 $SCR/$LABEL.t1 <<'PY'
import sys
c0=[int(l.split()[1]) for l in open(sys.argv[2]) if l.startswith('counter')][0]
c1=[int(l.split()[1]) for l in open(sys.argv[3]) if l.startswith('counter')][0]
fr=c1-c0
for l in open(sys.argv[1]):
    w=l.strip().split(',')
    if len(w)>3 and w[1].isdigit():
        print(f"  {w[0]:22s} {int(w[1])/fr/1e6:8.3f} M {w[3]:14s}/frame")
PY
python3 - $SCR/$LABEL.t0 $SCR/$LABEL.t1 $SCR/$LABEL.log <<'PY'
import sys,re
c=[int(l.split()[1]) for l in open(sys.argv[1]) if l.startswith('counter')][0]
c1=[int(l.split()[1]) for l in open(sys.argv[2]) if l.startswith('counter')][0]
g=[l for l in open(sys.argv[3]) if l.startswith('gfx: ') and ' draws, ' in l]
w=g[c//60:c1//60]
if w:
    d=[[int(x) for x in m[0]] for m in (re.findall(r'(\d+) draws, (\d+) tris, (\d+) verts',l) for l in w) if m]
    n=len(d); print(f"  geometry/frame over window: {sum(x[0] for x in d)/n:.0f} draws, {sum(x[1] for x in d)/n:.0f} tris, {sum(x[2] for x in d)/n:.0f} verts ({n} samples)")
PY
