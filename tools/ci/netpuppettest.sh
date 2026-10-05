#!/bin/bash
# netpuppettest.sh — does the client pose the host's world?
#
# Loopback (PLANS/netplay/spec-entities.md §2-§4): a listen host (its own
# player 0 on a scripted pad) and clients on 127.0.0.1, offscreen on the GPU,
# with 8 sims. The host dumps what it offered each client, unquantized, for
# every snapshot (--net-snap-dump); each client traces its puppets as drawn
# every few ticks, sampled after the frame's lvRender so any later write in
# the frame shows (--net-puppet-trace): its render tick
# (the host tick it shows, about two snapshots behind), and each puppet's
# prop position, yaw, anim, action, door frac, object presence. Checks:
#
#   skedar  (0x32, 3400 host frames): the client lives to the end; on the
#           traced ticks every sim and player puppet's position and yaw are
#           the host's at the client's render tick (interpolated between the
#           host's two snapshots around it) within a small error, and its anim
#           the host's; every sim death and respawn on the host shows on the
#           client (ACT_DIE, then alive again); dropped weapons the host
#           offered appear on the client while offered and are gone after.
#   lossy   the same at --net-sim 5,60 (both ends): wider bounds, gaps
#           interpolated across or extrapolated at most 100 ms.
#   doors   (Area 52, 0x3b): door fracs the same as the host's at the render
#           tick, and doors did move.
#   two     host + two clients: on each client the other client's player is a
#           puppet that walks where the host has it.
#
# Side-by-side screenshots (shots: host and client a moment apart, the
# host's player stood just behind the client's, both looking at the same
# sim) go to build/net-shots/shots-{host,client}.png.
#
#   netpuppettest.sh [BIN]   BIN a file name in build/ (pd.x86_64) or a path
#
# Env: OUT (build/netpuppet-out), PORT (27200; uses PORT..PORT+4), MODDIR
# (mod_allinone), CASES (skedar lossy doors two shots; shots only takes the
# screenshots, its players invincible and moved, so it gates on nothing but a
# clean run).
# Exit status: 0 all good, 1 a check failed, 2 a run failed to start.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=${BUILD:-$ROOT/build}
OUT=${OUT:-$BUILD/netpuppet-out}; PORT=${PORT:-27200}
MODDIR=${MODDIR:-mod_allinone}
CASES=${CASES:-skedar lossy doors two shots}
BIN=${1:-pd.x86_64}
case $BIN in /*) ;; */*) BIN=$(realpath "$BIN") ;; *) BIN=$BUILD/$BIN ;; esac
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
SHOTDIR=$BUILD/net-shots
rm -f "$OUT"/*.log "$OUT"/*.dump "$OUT"/*.trace
# no controller may reach a run (see replaytest.sh)
export SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-offscreen} SDL_GAMECONTROLLER_IGNORE_DEVICES=0x054c/0x0ce6,0x045e/0x028e \
	SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT=0x0000/0x0000 SDL_JOYSTICK_HIDAPI=0

status=0
fail() { echo "FAIL $*"; status=1; }
pass() { echo "ok   $*"; }

# the clients walk, turn and fire; the host's own player walks the other way
# (the late taps of fire also ask for a respawn after a death)
cat > "$OUT/client.script" <<'EOF'
60 400 0008 0 0 0 0 0 0
300 360 0 0 0 0 0 0.5 0
420 421 2000 0 0 0 0 0 0
600 900 0008 0 0 0 0 -0.3 0
1000 1001 2000 0 0 0 0 0 0
1150 1151 2000 0 0 0 0 0 0
1200 1500 0008 0 0 0 0 0.2 0
1600 1601 2000 0 0 0 0 0 0
1750 1751 2000 0 0 0 0 0 0
1900 1901 2000 0 0 0 0 0 0
2000 2300 0008 0 0 0 0 -0.2 0
2400 2401 2000 0 0 0 0 0 0
2550 2551 2000 0 0 0 0 0 0
2700 2701 2000 0 0 0 0 0 0
2850 2851 2000 0 0 0 0 0 0
3000 3001 2000 0 0 0 0 0 0
3150 3151 2000 0 0 0 0 0 0
EOF
cat > "$OUT/client2.script" <<'EOF'
60 500 0008 0 0 0 0 0.15 0
700 1000 0008 0 0 0 0 -0.25 0
1300 1700 0008 0 0 0 0 0.3 0
EOF
cat > "$OUT/shots.script" <<'EOF'
60 300 0008 0 0 0 0 0 0
EOF
cat > "$OUT/host.script" <<'EOF'
60 400 0008 0 0 0 0 0 0
300 360 0 0 0 0 0 -0.5 0
700 701 2000 0 0 0 0 0 0
EOF

INI="[Mod]\nStartArmed=1\n"

game() {
	local label=$1 t=$2; shift 2
	local save=$OUT/save-$label
	rm -rf "$save"; mkdir -p "$save"
	printf '%b' "$INI" > "$save/pd.ini"
	cd "$BUILD" || exit 2
	exec timeout -k 5 "$t" stdbuf -oL -eL "$BIN" --savedir "$save" --skip-intro --no-sound "$@" > "$OUT/$label.log" 2>&1
}

waitfor() {
	local log=$1 pattern=$2 n=$3
	while [ "$n" -gt 0 ]; do
		grep -q -- "$pattern" "$log" 2>/dev/null && return 0
		sleep 1; n=$((n - 1))
	done
	return 1
}

# the game's pid for a run label (pgrep -x on the binary's name, then its
# own --savedir: pgrep -f would match this script)
gamepid() {
	local label=$1 p
	for p in $(pgrep -x "$(basename "$BIN" | cut -c1-15)"); do
		tr '\0' ' ' < "/proc/$p/cmdline" 2>/dev/null | grep -q -- "--savedir $OUT/save-$label " && { echo "$p"; return; }
	done
}

# shots LABEL: on the host (the authority), its own player is made
# invincible with the client's; at host tick 900 the client's player's
# place and facing are read (one short stop), the host's own player is put
# there looking the same way, and a sim is moved 220 units in front of it.
# A quarter second later both machines screenshot (host first, then client): the
# two views are from the same spot, the host's sims and the client's
# puppets of them in the frame. Nothing on the client is written by a
# debugger: its shot shows the snapshots as they came.
cat > "$OUT/look.py" <<'PY'
import gdb, math
def f(e): return float(gdb.parse_and_eval(e))
def invincible():
    for i in range(4):
        if int(gdb.parse_and_eval("(long)g_Vars.players[%d]" % i)):
            gdb.execute("set var g_Vars.players[%d]->invincible = 1" % i)
def sims():
    for i in range(int(gdb.parse_and_eval("g_BotCount"))):
        c = "g_MpBotChrPtrs[%d]" % i
        print("SIM %d pos %.0f %.0f %.0f room %d act %d chrflags %#x propflags %#x anim %d" % (i, f(c + "->prop->pos.x"), f(c + "->prop->pos.y"), f(c + "->prop->pos.z"),
              int(gdb.parse_and_eval(c + "->prop->rooms[0]")), int(gdb.parse_and_eval(c + "->actiontype")),
              int(gdb.parse_and_eval(c + "->chrflags")), int(gdb.parse_and_eval(c + "->prop->flags")),
              int(gdb.parse_and_eval(c + "->model->anim->animnum"))))
def where():
    """the client: where its own player stands and looks"""
    pn = int(gdb.parse_and_eval("g_NetLocalSlot"))
    p = "g_Vars.players[%d]" % pn
    rooms = [int(gdb.parse_and_eval("%s->prop->rooms[%d]" % (p, i))) for i in range(8)]
    print("WHERE %f %f %f %f %s" % (f(p + "->prop->pos.x"), f(p + "->prop->pos.y"), f(p + "->prop->pos.z"),
                                     f(p + "->vv_theta"), " ".join(str(r) for r in rooms)))
def stage(x, y, z, th, rooms, ahead):
    """the host: its own player 0 where the client's stands, looking the same
    way, and a sim moved ahead units in front of that spot, facing it"""
    p0 = "g_Vars.players[0]"
    a = math.radians(th)
    fx, fz = -math.sin(a), math.cos(a)
    gdb.execute("call (void)propDeregisterRooms(%s->prop)" % p0)
    for i, r in enumerate(rooms):
        gdb.execute("set var %s->prop->rooms[%d] = %d" % (p0, i, r))
    for fld in ("prop->pos", "bond2.unk10", "bondprevpos"):
        gdb.execute("set var %s->%s.x = %f" % (p0, fld, x)); gdb.execute("set var %s->%s.y = %f" % (p0, fld, y)); gdb.execute("set var %s->%s.z = %f" % (p0, fld, z))
    gdb.execute("call (void)propRegisterRooms(%s->prop)" % p0)
    gdb.execute("set var %s->vv_theta = %f" % (p0, th))
    gdb.execute("set var %s->vv_verta = 0" % p0)
    k = 0
    for i in range(int(gdb.parse_and_eval("g_BotCount"))):
        if int(gdb.parse_and_eval("g_MpBotChrPtrs[%d]->actiontype" % i)) not in (4, 5):
            k = i; break
    gdb.execute("set $c = (struct coord *)malloc(16)")
    gdb.execute("set var $c->x = %f" % (x + fx * ahead)); gdb.execute("set var $c->y = %f" % (y - 30)); gdb.execute("set var $c->z = %f" % (z + fz * ahead))
    ok = int(gdb.parse_and_eval("(int)chrMoveToPos(g_MpBotChrPtrs[%d], $c, %s->prop->rooms, %f, 1)" % (k, p0, (a + math.pi) % (2 * math.pi))))
    print("STAGED host player 0 where the client's player stands (%.0f %.0f %.0f, theta %.1f); sim %d moved %d units in front (%s)"
          % (x, y, z, th, k, ahead, "ok" if ok else "refused"))
PY

shots() {
	local label=$1 hp cp slot
	hp=$(gamepid "$label-host"); cp=$(gamepid "$label-client")
	[ -n "$hp" ] && [ -n "$cp" ] || { echo "     shots: no game pids ('$hp' '$cp')"; return; }
	mkdir -p "$SHOTDIR" "$BUILD/screenshots"
	gdb -p "$hp" -batch -ex "source $OUT/look.py" -ex "python invincible()" >/dev/null 2>&1
	waitfor "$OUT/$label-host.log" "snap slot 1 so far (tick 900)" 120 || return
	local w
	w=$(gdb -p "$cp" -batch -ex "source $OUT/look.py" -ex "python where()" 2>/dev/null | sed -n 's/^WHERE //p')
	[ -n "$w" ] || { echo "     shots: the client's place not read"; return; }
	set -- $w
	gdb -p "$hp" -batch -ex "source $OUT/look.py" -ex "python stage($1, $2, $3, $4, [$5, $6, $7, $8, $9, ${10}, ${11}, ${12}], 220)" 2>/dev/null | grep STAGED | sed "s/^/     $label host: /"
	sleep 0.25
	shot1 "$hp" "$SHOTDIR/$label-host.png"
	shot1 "$cp" "$SHOTDIR/$label-client.png"
	gdb -p "$hp" -batch -ex "source $OUT/look.py" -ex "python sims()" 2>/dev/null | grep "^SIM" > "$OUT/$label-host.sims"
	gdb -p "$cp" -batch -ex "source $OUT/look.py" -ex "python sims()" 2>/dev/null | grep "^SIM" > "$OUT/$label-client.sims"
}

# shot1 PID FILE: one screenshot from that game, the new file in
# screenshots/ copied to FILE
shot1() {
	local pid=$1 dest=$2 before f i
	rm -f "$dest"
	before=$(ls "$BUILD/screenshots" 2>/dev/null | sort)
	gdb -p "$pid" -batch -ex 'call (void)screenshotRequest()' >/dev/null 2>&1
	for i in $(seq 50); do
		f=$(comm -13 <(echo "$before") <(ls "$BUILD/screenshots" 2>/dev/null | sort) | head -1)
		[ -n "$f" ] && { sleep 0.3; cp "$BUILD/screenshots/$f" "$dest"; echo "     shot: $dest"; return; }
		sleep 0.1
	done
	echo "     shot: none from $pid"
}

# run LABEL PORT STAGE SIMS NCLIENTS FRAMES [ARGS...]
run() {
	local label=$1 port=$2 stage=$3 sims=$4 nclients=$5 frames=$6; shift 6
	# the host runs on past the clients' end (the shots run stops the client
	# under gdb now and then, and it must not find the host gone)
	local extra=100 cscript=client.script
	[ "$label" = shots ] && { extra=1500; cscript=shots.script; }
	game "$label-host" 240 --moddir "$MODDIR" --host "$port" --net-test-host "$nclients" --net-test-stage "$stage" \
		--net-test-sims "$sims" --endless --rng-seed 1 --mp-weapons 1,4,7,9,10,1 \
		--net-test-input "$OUT/host.script" --net-snap-dump "$OUT/$label-host.dump" \
		--net-snap-dump-ticks "200,$frames" --exit-frame $((frames + extra)) "$@" &
	local host=$!
	waitfor "$OUT/$label-host.log" "net: hosting on UDP port" 60 || { echo "FAIL: $label host did not start"; kill -TERM $host; exit 2; }
	game "$label-client" 230 --moddir "$MODDIR" --connect "127.0.0.1:$port" --net-test-join \
		--net-test-input "$OUT/$cscript" --net-puppet-trace "$OUT/$label-client.trace,5" --net-event-log "$OUT/$label-client.events" \
		--exit-frame "$frames" "$@" &
	local client=$!
	local client2=
	if [ "$nclients" -ge 2 ]; then
		waitfor "$OUT/$label-client.log" "net: accepted by" 60
		game "$label-client2" 230 --moddir "$MODDIR" --connect "127.0.0.1:$port" --net-test-join \
			--net-test-input "$OUT/client2.script" --net-puppet-trace "$OUT/$label-client2.trace,5" --net-event-log "$OUT/$label-client2.events" \
			--exit-frame "$frames" "$@" &
		client2=$!
	fi
	if [ "$label" = shots ]; then
		waitfor "$OUT/$label-host.log" "snap slot 1 so far (tick 300)" 150 && shots "$label"
	fi
	wait "$client"; echo "$label: client exit $?"
	[ -n "$client2" ] && { wait "$client2"; echo "$label: client2 exit $?"; }
	wait "$host"; echo "$label: host exit $?"
}

# compare LABEL CLIENTLABEL SLOT POSBOUND: the trace against the host's dump
compare() {
	python3 - "$OUT/$1-host.dump" "$OUT/$2.trace" "$1/$2" "$3" "$4" "$5" <<'PY'
import math, sys, bisect
hpath, tpath, label, slot, posbound, mode = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4]), float(sys.argv[5]), sys.argv[6]
TAU = 2 * math.pi
# the host: per id, its state at each snapshot it built for this slot
H = {}
for line in open(hpath):
    p = line.split()
    if p[0] != 'H' or int(p[2]) != slot: continue
    tick, eid = int(p[1]), int(p[4])
    H.setdefault(eid, []).append((tick, int(p[5]), int(p[6]), int(p[7]), [float(x) for x in p[8:11]], float(p[11]),
                                   int(p[12]), float(p[14]), int(p[19]), int(p[20]), int(p[21])))
for v in H.values(): v.sort()
HT = {e: [s[0] for s in v] for e, v in H.items()}
hostticks = sorted({s[0] for v in H.values() for s in v})
def at(eid, rt):
    """the host's two states around rt for eid (None if it was not offered on both)"""
    rt -= 0.00006   # the trace's rounding: a tick printed whole may be just short of it
    v = H.get(eid)
    if not v: return None
    i = bisect.bisect_right(HT[eid], rt) - 1
    if i < 0: return None
    a = v[i]
    if a[0] == rt or i + 1 >= len(v): return (a, a, 0.0)
    b = v[i + 1]
    # a gap in what was offered (out of scope, or a new generation)
    j = bisect.bisect_right(hostticks, a[0])
    if j < len(hostticks) and hostticks[j] != b[0] or a[1] != b[1]: return None
    return (a, b, (rt - a[0]) / (b[0] - a[0]))
def angd(x, y):
    d = math.fmod(x - y, TAU)
    if d > math.pi: d -= TAU
    if d < -math.pi: d += TAU
    return abs(d)
P = []; O = []; D = []; T = []; R = []
for line in open(tpath):
    p = line.split()
    if p[0] == 'P':
        P.append((int(p[1]), float(p[2]), int(p[3]), int(p[4]), [float(x) for x in p[5:8]], float(p[8]), int(p[9]), int(p[11]), int(p[12])))
        R.append((int(p[12]), int(p[13])))
    elif p[0] == 'O':
        O.append((int(p[1]), float(p[2]), int(p[3]), int(p[4])))
        R.append((int(p[8]), int(p[10])))
    elif p[0] == 'D': D.append((int(p[1]), float(p[2]), int(p[3]), float(p[4])))
    elif p[0] == 'T': T.append((int(p[1]), float(p[2])))
if not T or not H:
    print(f"FAIL {label}: nothing to compare (trace ticks {len(T)}, host ids {len(H)})"); sys.exit(1)
lo, hi = hostticks[0], hostticks[-1]
bad = []; poserr = []; yawerr = []; animok = animn = 0; byk = {}
for (ct, rt, eid, kind, pos, yaw, anim, act, hidden) in P:
    if rt < lo or rt > hi: continue
    r = at(eid, rt)
    if not r: continue
    a, b, t = r
    if a[3] not in (1, 2) or b[3] not in (1, 2): continue   # deferred or excluded: stale by design
    if a[8] != b[8]: continue                                 # a teleport between: a step
    if a[9] & 0x40 or b[9] & 0x40: continue                   # hidden on the host
    hp = [a[4][k] + (b[4][k] - a[4][k]) * t for k in range(3)]
    e = math.dist(hp, pos)
    d = math.fmod(b[5] - a[5], TAU)
    if d > math.pi: d -= TAU
    if d < -math.pi: d += TAU
    ye = angd(a[5] + d * t, yaw)
    poserr.append(e); yawerr.append(ye)
    byk[kind] = byk.get(kind, 0) + 1
    animn += 1
    if anim in (a[6], b[6]): animok += 1
    elif len(bad) < 400: bad.append(f"client tick {ct} rt {rt:.2f} id {eid}: anim {anim}, host {a[6]}/{b[6]}")
    if e > posbound * 4 and len(bad) < 400: bad.append(f"client tick {ct} rt {rt:.2f} id {eid} kind {kind}: pos {pos} host {['%.2f' % x for x in hp]} err {e:.2f}")
if animn < 200:
    print(f"FAIL {label}: only {animn} puppet poses compared"); sys.exit(1)
poserr.sort(); yawerr.sort()
q = lambda xs, f: xs[min(len(xs) - 1, int(len(xs) * f))]
p95, pmax, y95 = q(poserr, 0.95), poserr[-1], q(yawerr, 0.95)
ok = p95 <= posbound and y95 <= 0.05 and animok >= 0.98 * animn
print(f"     {label}: {animn} puppet poses compared on {len(T)} traced ticks ({', '.join(f'{v} kind {k}' for k, v in sorted(byk.items()))}): "
      f"position error median {q(poserr, .5):.3f} p95 {p95:.3f} max {pmax:.2f} units; yaw p95 {y95:.4f} rad; anim the host's {animok}/{animn}")
if not ok:
    for x in bad[:10]: print("     " + x)
    print(f"FAIL {label}: puppet poses off the host's (bound p95 {posbound} units, yaw 0.05 rad, anim 98%)"); sys.exit(1)
print(f"ok   {label}: puppets posed as the host had them at the client's render tick")
# rooms: a puppet with none is never drawn (the bg room flood starts from the rooms a prop has)
shown = [r for h, r in R if not h]
noroom = sum(1 for r in shown if r <= 0)
if not shown or noroom > 0.01 * len(shown):
    print(f"FAIL {label}: {noroom} of {len(shown)} shown puppet poses in no room"); sys.exit(1)
print(f"ok   {label}: {len(shown) - noroom} of {len(shown)} shown puppet poses (chrs and objects) in a room")
status = 0
if mode in ('full', 'lossy'):
    # sims' deaths and respawns: the host's life bits against the client's actions
    hdeath = []; hresp = []
    for eid, v in H.items():
        if not v or v[0][10] != 2: continue
        for x, y in zip(v, v[1:]):
            if y[1] != x[1]: continue
            if (x[9] & 3) == 0 and (y[9] & 3): hdeath.append((eid, y[0]))
            if (x[9] & 3) and (y[9] & 3) == 0: hresp.append((eid, y[0]))
    C = {}
    for (ct, rt, eid, kind, pos, yaw, anim, act, hidden) in P:
        if kind == 2: C.setdefault(eid, []).append((rt, act))
    def seen(eid, tick, want):
        return any(abs(rt - tick) < 40 and want(act) for rt, act in C.get(eid, []))
    cut = T[-1][1] - 60
    hd = [x for x in hdeath if lo + 20 < x[1] < cut]; hr = [x for x in hresp if lo + 20 < x[1] < cut]
    md = sum(1 for e, t in hd if seen(e, t, lambda a: a in (4, 5)))
    mr = sum(1 for e, t in hr if seen(e, t, lambda a: a == 25))
    if hd and hr and md >= 0.9 * len(hd) and mr >= 0.9 * len(hr):
        print(f"ok   {label}: sim deaths {md}/{len(hd)} and respawns {mr}/{len(hr)} on the host shown on the client")
    else:
        print(f"FAIL {label}: sim deaths {md}/{len(hd)}, respawns {mr}/{len(hr)} shown"); status = 1
    # dropped weapons: offered by the host (kind 5) <-> made on the client
    life = {}
    for eid, v in H.items():
        for s in v:
            if s[10] == 5 and s[3] in (1, 2): life.setdefault((eid, s[1]), []).append(s[0])
    CO = {}
    for (ct, rt, eid, kind) in O:
        if kind == 5: CO.setdefault(eid, []).append(rt)
    shown = gone = considered = 0
    for (eid, gen), ticks in life.items():
        a, b = min(ticks), max(ticks)
        if b - a < 30 or a < lo + 20 or b > cut: continue
        considered += 1
        rts = CO.get(eid, [])
        if any(a + 10 <= rt <= b - 10 for rt in rts): shown += 1
        nxt = [t for (e, g), tk in life.items() if e == eid and min(tk) > b for t in tk]
        after = [rt for rt in rts if b + 15 <= rt <= min(nxt + [b + 60]) - 5]
        if not after: gone += 1
    if considered and shown >= 0.9 * considered and gone >= 0.9 * considered:
        print(f"ok   {label}: {considered} dropped weapons the host offered: {shown} shown on the client while offered, {gone} gone after")
    else:
        print(f"FAIL {label}: dropped weapons: {considered} offered, {shown} shown, {gone} gone after"); status = 1
if mode == 'doors':
    errs = []; moved = set()
    for (ct, rt, eid, frac) in D:
        r = at(eid, rt)
        if not r: continue
        a, b, t = r
        if a[3] not in (1, 2) or b[3] not in (1, 2): continue
        hf = a[7] + (b[7] - a[7]) * t
        errs.append(abs(hf - frac))
        if 0.01 < frac < 0.99: moved.add(eid)
    errs.sort()
    if len(errs) >= 100 and moved and q(errs, 0.99) < 0.02:
        print(f"ok   {label}: {len(errs)} door states compared, {len(moved)} doors seen moving, frac error p99 {q(errs, .99):.4f} max {errs[-1]:.4f}")
    else:
        print(f"FAIL {label}: doors: {len(errs)} compared, {len(moved)} moving, p99 {q(errs, .99) if errs else -1:.4f}"); status = 1
if mode == 'player':
    # every other machine's player (the host's and the other client's) is a
    # puppet here, and each walked
    pl = {}
    for (ct, rt, eid, kind, pos, yaw, anim, act, hidden) in P:
        if kind == 3: pl.setdefault(eid, []).append(pos)
    spans = {e: max(math.dist(v[0], x) for x in v) for e, v in pl.items()}
    if len(pl) >= 2 and all(len(v) > 100 for v in pl.values()) and all(s > 300 for s in spans.values()):
        print(f"ok   {label}: {len(pl)} other machines' players are puppets here, each walked: " + ", ".join(f"id {e} {len(pl[e])} poses, {spans[e]:.0f} units" for e in pl))
    else:
        print(f"FAIL {label}: player puppets {', '.join(f'id {e} poses {len(v)} span {spans[e]:.0f}' for e, v in pl.items()) or 'none'}"); status = 1
sys.exit(status)
PY
}

check_run() {
	local label=$1; shift
	local H=$OUT/$label-host.log c
	grep -q "renderD128: Permission denied" "$OUT/$label"-*.log && fail "$label: a run fell back to llvmpipe"
	for c in "$@"; do
		local C=$OUT/$label-$c.log x
		x=$(grep -o "$label: $c exit [0-9]*" "$OUT/run.log" | awk '{print $4}')
		if grep -qE "FATAL|Segmentation|Aborted" "$C"; then
			fail "$label: $c crashed"; grep -A12 "FATAL" "$C" | head -14 | sed 's/^/     /'
		elif [ "$x" = 0 ]; then
			pass "$label: $c ran to the end (exit 0)"
		else
			fail "$label: $c exit '$x'"
		fi
		grep "net: puppets " "$C" | tail -1 | sed "s/^.*net: /     $label $c: /"
		grep "net: local block " "$C" | tail -1 | sed "s/^.*net: /     $label $c: /"
		# this machine's own player: every respawn the host counted was made
		# here too (the last may still be in flight at the end)
		local hr lr ld
		hr=$(grep "net: snap client " "$C" | tail -1 | sed -n 's/.*; respawns \([0-9]*\), teleports.*/\1/p')
		ld=$(grep "net: local block " "$C" | tail -1 | sed -n 's/.*; deaths \([0-9]*\),.*/\1/p')
		lr=$(grep "net: local block " "$C" | tail -1 | sed -n 's/.*, respawns \([0-9]*\), inventory.*/\1/p')
		if [ -n "$hr" ] && [ -n "$lr" ] && [ "$lr" -ge $((hr > 0 ? hr - 1 : 0)) ] && [ "$lr" -le "$hr" ]; then
			pass "$label: $c's own player: $ld death(s) from the host's block, $lr of the host's $hr respawn(s) made here"
		else
			fail "$label: $c's own player: deaths '${ld:-?}', respawns here '${lr:-?}', the host's '${hr:-?}'"
		fi
	done
	grep -qE "FATAL|Segmentation" "$H" && fail "$label: the host crashed"
}

{
	i=0
	for c in $CASES; do
		case $c in
		skedar) run skedar $((PORT + i)) 0x32 8 1 3400 ;;
		lossy)  run lossy $((PORT + i)) 0x32 8 1 2400 --net-sim 5,60 ;;
		doors)  run doors $((PORT + i)) 0x3b 8 1 2400 ;;
		two)    run two $((PORT + i)) 0x32 4 2 2000 ;;
		shots)  run shots $((PORT + i)) 0x32 8 1 1300 ;;
		esac
		i=$((i + 1))
	done
} 2>&1 | tee "$OUT/run.log"

for c in $CASES; do
	case $c in
	skedar) check_run skedar client; compare skedar skedar-client 1 0.5 full || status=1 ;;
	lossy)  check_run lossy client; compare lossy lossy-client 1 6 lossy || status=1 ;;
	doors)  check_run doors client; compare doors doors-client 1 0.5 doors || status=1 ;;
	two)    check_run two client client2
	        s1=$(sed -n 's/.*into slot \([0-9]*\).*/\1/p' "$OUT/two-client.log" | head -1)
	        s2=$(sed -n 's/.*into slot \([0-9]*\).*/\1/p' "$OUT/two-client2.log" | head -1)
	        compare two two-client "${s1:-1}" 0.5 player || status=1
	        compare two two-client2 "${s2:-2}" 0.5 player || status=1 ;;
	shots)  check_run shots client
	        for f in host client; do [ -s "$SHOTDIR/shots-$f.png" ] || fail "shots: no $f screenshot"; done ;;
	esac
done
exit $status
