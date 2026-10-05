#!/bin/bash
# neteventtest.sh — do the host's events reach the client, in order, and
# does the client's scoreboard follow the host's?
#
# Loopback (PLANS/netplay/spec-entities.md §5): a listen host and one client
# on 127.0.0.1, offscreen on the GPU, Skedar (0x32) with 8 sims and a one
# minute time limit, so the match ends on its own and the host sends
# MATCH_END. Both write every event to --net-event-log (the host each it
# recorded, with the player it was for or left out; the client each it
# applied, with its tick and render tick). Checks:
#
#   - both ran to the end; the match ended on the host's clock;
#   - kills: the kill table (deaths and kill counts per mpchr) the client
#     builds from DEATH events equals the host's at every sampled tick
#     (every 300) and at MATCH_END, before the host's own table is put
#     over it;
#   - order: the client applied events in tick order, none malformed;
#   - counts: per type, the client applied every event the host sent it
#     (everything not for another player and not left out for this one);
#   - kinds: shots with tracers (sims' and the host's player's), sparks,
#     an explosion, chr hits, grunts and deaths came through;
#   - the client's scripted player fires at a sim the host keeps putting in
#     front of it: the host registers the hit (a chrdamage with the client's
#     player the attacker) and the client gets that hit event and at least
#     one hudmsg the host made for its player;
#   - audio: the client's mixed output (SDL's disk driver, music volume 0)
#     against the times it applied shot sounds: its loud onsets follow them
#     (correlation over 100 ms bins at the best lag).
#
# Screenshots of the client's view (a staged explosion in front of it, sims'
# tracers) go to build/net-shots/events-*.png, the client's audio spectrogram
# to build/net-shots/events-audio.png when ffmpeg is there.
#
#   neteventtest.sh [BIN]   BIN a file name in build/ (pd.x86_64) or a path
#
# Env: OUT (build/netevent-out), PORT (27240), MODDIR (mod_allinone).
# Exit status: 0 all good, 1 a check failed, 2 a run failed to start.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=${BUILD:-$ROOT/build}
OUT=${OUT:-$BUILD/netevent-out}; PORT=${PORT:-27240}
MODDIR=${MODDIR:-mod_allinone}
BIN=${1:-pd.x86_64}
case $BIN in /*) ;; */*) BIN=$(realpath "$BIN") ;; *) BIN=$BUILD/$BIN ;; esac
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
SHOTDIR=$BUILD/net-shots
rm -f "$OUT"/*.log "$OUT"/*.events "$OUT"/*.pcm "$OUT"/*.raw "$OUT"/*.png
export SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-offscreen} SDL_GAMECONTROLLER_IGNORE_DEVICES=0x054c/0x0ce6,0x045e/0x028e \
	SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT=0x0000/0x0000 SDL_JOYSTICK_HIDAPI=0

status=0
fail() { echo "FAIL $*"; status=1; }
pass() { echo "ok   $*"; }

# the host stages a sim in front of the client's player (ticks 600-1700):
# till 1100 the client only stands there (the sim's shots are what it hears),
# then it holds fire (its Cyclone, StartArmed's gun from slot 1, is automatic);
# later it taps fire now and then, and walks a bit
cat > "$OUT/client.script" <<'EOF'
1100 1700 2000 0 0 0 0 0 0
1800 2000 0008 0 0 0 0 0.3 0
2100 2101 2000 0 0 0 0 0 0
2300 2301 2000 0 0 0 0 0 0
2500 2800 0008 0 0 0 0 -0.2 0
2900 2901 2000 0 0 0 0 0 0
3100 3101 2000 0 0 0 0 0 0
3300 3301 2000 0 0 0 0 0 0
EOF
cat > "$OUT/host.script" <<'EOF'
60 400 0008 0 0 0 0 0 0
300 360 0 0 0 0 0 -0.5 0
700 701 2000 0 0 0 0 0 0
1900 2400 0008 0 0 0 0 0.2 0
2500 2501 2000 0 0 0 0 0 0
EOF

INI="[Mod]\nStartArmed=1\n"

# game LABEL TIMEOUT SOUND ARGS...: SOUND 1 mixes (SDL's disk driver, paced
# in real time) and the game writes what it queued to $OUT/LABEL.pcm
game() {
	local label=$1 t=$2 sound=$3; shift 3
	local save=$OUT/save-$label snd=--no-sound
	rm -rf "$save"; mkdir -p "$save"
	printf '%b' "$INI" > "$save/pd.ini"
	cd "$BUILD" || exit 2
	if [ "$sound" = 1 ]; then
		snd="--audio-dump $OUT/$label.pcm"
		export SDL_AUDIODRIVER=disk SDL_DISKAUDIOFILE="$OUT/$label.raw"
	fi
	exec timeout -k 5 "$t" stdbuf -oL -eL "$BIN" --savedir "$save" --skip-intro $snd "$@" > "$OUT/$label.log" 2>&1
}

waitfor() {
	local log=$1 pattern=$2 n=$3
	while [ "$n" -gt 0 ]; do
		grep -q -- "$pattern" "$log" 2>/dev/null && return 0
		sleep 1; n=$((n - 1))
	done
	return 1
}

# the game's pid for a run label, by its own --savedir (never pgrep -f)
gamepid() {
	local label=$1 p
	for p in $(pgrep -x "$(basename "$BIN" | cut -c1-15)"); do
		tr '\0' ' ' < "/proc/$p/cmdline" 2>/dev/null | grep -q -- "--savedir $OUT/save-$label " && { echo "$p"; return; }
	done
}

cat > "$OUT/stage.py" <<'PY'
import gdb, math
def f(e): return float(gdb.parse_and_eval(e))
def i(e): return int(gdb.parse_and_eval(e))
def remote():
    """the host's player for slot 1 (the client's)"""
    for k in range(4):
        if i("(long)g_Vars.players[%d]" % k) and (i("g_Vars.playerstats[%d].mpindex" % k) & 3) == 1:
            return k
    return -1
def invincible(pn):
    gdb.execute("set var g_Vars.players[%d]->invincible = 1" % pn)
def ahead(pn, dist):
    p = "g_Vars.players[%d]" % pn
    a = math.radians(f(p + "->vv_theta"))
    return (f(p + "->prop->pos.x") - math.sin(a) * dist, f(p + "->prop->pos.y"), f(p + "->prop->pos.z") + math.cos(a) * dist, a)
def sim(dist):
    """a living sim moved dist units in front of the client's player, facing it"""
    pn = remote()
    if pn < 0: print("STAGE no remote player"); return
    invincible(pn)
    p = "g_Vars.players[%d]" % pn
    x, y, z, a = ahead(pn, dist)
    k = -1
    for s in range(i("g_BotCount")):
        if i("g_MpBotChrPtrs[%d]->actiontype" % s) not in (4, 5):
            k = s; break
    if k < 0: print("STAGE no living sim"); return
    gdb.execute("set $c = (struct coord *)malloc(16)")
    gdb.execute("set var $c->x = %f" % x); gdb.execute("set var $c->y = %f" % (y - 30)); gdb.execute("set var $c->z = %f" % z)
    ok = i("(int)chrMoveToPos(g_MpBotChrPtrs[%d], $c, %s->prop->rooms, %f, 1)" % (k, p, (a + math.pi) % (2 * math.pi)))
    print("STAGE sim %d %d units in front of player %d (%s)" % (k, dist, pn, "ok" if ok else "refused"))
def boom(dist):
    """an explosion dist units in front of the client's player (made
    invincible first), from no one"""
    pn = remote()
    if pn < 0: print("STAGE no remote player"); return
    invincible(pn)
    x, y, z, a = ahead(pn, dist)
    gdb.execute("set $e = (struct coord *)malloc(16)")
    gdb.execute("set var $e->x = %f" % x); gdb.execute("set var $e->y = %f" % y); gdb.execute("set var $e->z = %f" % z)
    ok = i("(int)explosionCreateSimple(0, $e, g_Vars.players[%d]->prop->rooms, 13, -1)" % pn)
    print("STAGE explosion %d units in front of player %d (%s)" % (dist, pn, "ok" if ok else "refused"))
PY

# shot1 PID FILE: one screenshot from that game into FILE
shot1() {
	local pid=$1 dest=$2 before f i
	rm -f "$dest"
	mkdir -p "$BUILD/screenshots" "$SHOTDIR"
	before=$(ls "$BUILD/screenshots" 2>/dev/null | sort)
	gdb -p "$pid" -batch -ex 'call (void)screenshotRequest()' >/dev/null 2>&1
	for i in $(seq 50); do
		f=$(comm -13 <(echo "$before") <(ls "$BUILD/screenshots" 2>/dev/null | sort) | head -1)
		[ -n "$f" ] && { sleep 0.3; cp "$BUILD/screenshots/$f" "$dest"; echo "     shot: $dest"; return; }
		sleep 0.1
	done
	echo "     shot: none from $pid"
}

stage() {
	local hp=$1 what=$2
	gdb -p "$hp" -batch -ex "source $OUT/stage.py" -ex "python $what" 2>/dev/null | grep "^STAGE" | sed 's/^/     host: /'
}

run() {
	local frames=4600
	game host 300 0 --moddir "$MODDIR" --host "$PORT" --net-test-host 1 --net-test-stage 0x32 \
		--net-test-sims 8 --rng-seed 1 --mp-weapons 10,4,7,9,28,1 --net-test-timelimit 1 \
		--net-test-input "$OUT/host.script" --net-event-log "$OUT/host.events" --exit-frame $((frames + 200)) &
	local host=$!
	waitfor "$OUT/host.log" "net: hosting on UDP port" 60 || { echo "FAIL: host did not start"; kill -TERM $host; exit 2; }
	game client 290 1 --moddir "$MODDIR" --connect "127.0.0.1:$PORT" --net-test-join \
		--net-test-input "$OUT/client.script" --net-event-log "$OUT/client.events" --exit-frame "$frames" ${CLIENTARGS:-} &
	local client=$!
	local hp cp n
	waitfor "$OUT/client.log" "net: accepted by" 60
	for n in $(seq 20); do cp=$(gamepid client); [ -n "$cp" ] && break; sleep 0.5; done
	hp=$(gamepid host)
	# the client's music off once its match runs (the stage sets the volume
	# from the save), so its mix is the game's sounds
	waitfor "$OUT/client.log" "net: match 1: GO" 120 && [ -n "$cp" ] &&
		gdb -p "$cp" -batch -ex 'call (void)optionsSetMusicVolume(0)' >/dev/null 2>&1
	# the client's player invincible from the start on the host: a sim that
	# killed it before the staging (tick 600) left it to spend the held
	# fire at 1100 on its respawn, and its own shots were never fired
	[ -n "$hp" ] && gdb -p "$hp" -batch -ex "source $OUT/stage.py" -ex "python invincible(remote())" >/dev/null 2>&1
	if waitfor "$OUT/host.log" "snap slot 1 so far (tick 600)" 150 && [ -n "$hp" ] && [ -n "$cp" ]; then
		for n in 1 2 3 4 5 6 7 8 9; do
			stage "$hp" "sim(220)"
			sleep 2
		done
		stage "$hp" "boom(450)"
		sleep 0.35
		shot1 "$cp" "$SHOTDIR/events-explosion.png"
		sleep 4
		shot1 "$cp" "$SHOTDIR/events-client.png"
		shot1 "$hp" "$SHOTDIR/events-host.png"
	else
		echo "     staging skipped (host '$hp', client '$cp')"
	fi
	wait "$client"; echo "client exit $?"
	# the host sits on its end screen once the match is over (its level
	# frames stop): a few seconds, then asked to quit
	for n in $(seq 10); do kill -0 "$host" 2>/dev/null || break; sleep 1; done
	kill -0 "$host" 2>/dev/null && { hp=$(gamepid host); [ -n "$hp" ] && kill -TERM "$hp"; }
	wait "$host"; echo "host exit $?"
}

run 2>&1 | tee "$OUT/run.log"

for c in host client; do
	L=$OUT/$c.log
	x=$(grep -o "$c exit [0-9]*" "$OUT/run.log" | awk '{print $3}')
	# the host is stopped on its end screen: SIGTERM's 143 is its end
	[ "$c" = host ] && [ "$x" = 143 ] && x=0
	if grep -qE "FATAL|Segmentation|Aborted" "$L"; then
		fail "$c crashed"; grep -A12 "FATAL" "$L" | head -14 | sed 's/^/     /'
	elif [ "$x" = 0 ]; then
		pass "$c ran to the end (exit 0)"
	else
		fail "$c exit '$(grep -o "$c exit [0-9]*" "$OUT/run.log" | awk '{print $3}')'"
	fi
	grep "net: events " "$L" | tail -1 | sed "s/^.*net: /     $c: /"
done
grep -q "renderD128: Permission denied" "$OUT"/*.log && fail "a run fell back to llvmpipe"
grep -q "scores and awards sent" "$OUT/host.log" && pass "the host ended the match on its clock and sent MATCH_END" || fail "no MATCH_END from the host"
grep -q "the host ended it" "$OUT/client.log" && pass "the client took the host's MATCH_END" || fail "the client never had MATCH_END"

python3 - "$OUT/host.events" "$OUT/client.events" "$OUT/client.log" <<'PY' || status=1
import sys, re
hpath, cpath, clog = sys.argv[1:4]
st = 0
def bad(m):
    global st; print("FAIL " + m); st = 1
def ok(m): print("ok   " + m)
H = [l.split() for l in open(hpath)]
C = [l.rstrip("\n") for l in open(cpath)]
slot = None
for l in open(clog):
    m = re.search(r"into slot (\d+)", l)
    if m: slot = int(m.group(1)); break
# the client's playernum is its slot here (same numbering on every machine)
pn = slot
# kill tables
hk = {(p[1], p[2]): " ".join(p[3:]) for p in H if p and p[0] == 'K'}
ck = {}
for l in C:
    p = l.split()
    if p and p[0] == 'K': ck[(p[1], p[2])] = " ".join(p[3:])
hendtick = min([int(t) for (w, t) in hk if w == 'end'] or [1 << 30])
# the host's samples while the match ran (it logs on through its end screen)
samples = sorted(int(t) for (w, t) in hk if w == 'tick' and int(t) <= hendtick)
same = diff = 0; diffs = []
for t in samples:
    k = ('tick', str(t))
    if k not in ck: continue
    if ck[k] == hk[k]: same += 1
    else: diff += 1; diffs.append(f"tick {t}: host [{hk[k]}] client [{ck[k]}]")
hend = [v for (w, t), v in hk.items() if w == 'end']
cend = [v for (w, t), v in ck.items() if w == 'end']
kills = sum(1 for p in H if p and p[0] == 'E' and p[2] == 'death')
if same >= 6 and same == len(samples) and diff == 0:
    ok(f"kill tables: the client's equals the host's at all {same} sampled ticks ({kills} deaths in the match)")
else:
    bad(f"kill tables: {same} equal, {diff} differ (of {len(samples)} host samples)")
    for d in diffs[:5]: print("     " + d)
if hend and cend and hend[0] == cend[0] and hend[0]:
    ok(f"kill tables at MATCH_END equal: {hend[0]}")
else:
    bad(f"kill tables at MATCH_END: host [{hend[0] if hend else '-'}] client [{cend[0] if cend else '-'}]")
# order
A = [l.split() for l in C if l.startswith("A ")]
ticks = [int(a[1]) for a in A]
ooo = sum(1 for x, y in zip(ticks, ticks[1:]) if y < x)
late = [float(a[2]) - int(a[1]) for a in A if float(a[2]) >= 0]
if A and ooo == 0:
    ok(f"{len(A)} events applied in tick order (render clock past the tick by {min(late):.2f}..{max(late):.2f} ticks, mean {sum(late)/len(late):.2f})")
else:
    bad(f"events applied: {len(A)}, {ooo} out of tick order")
m = re.findall(r"out of order (\d+), malformed (\d+)", open(clog).read())
if m and m[-1] == ('0', '0'): ok("no EVENTS message out of order, no event malformed")
else: bad(f"out of order / malformed: {m[-1] if m else '?'}")
# counts per type: what the host sent this client against what it applied
sent = {}; got = {}
for p in H:
    if not p or p[0] != 'E': continue
    target, excl = int(p[4]), int(p[6])
    if (target >= 0 and target != pn) or (excl >= 0 and excl == pn): continue
    sent[p[2]] = sent.get(p[2], 0) + 1
for a in A: got[a[3]] = got.get(a[3], 0) + 1
allsame = all(sent.get(k, 0) == got.get(k, 0) for k in set(sent) | set(got))
line = ", ".join(f"{k} {got.get(k, 0)}/{sent.get(k, 0)}" for k in sorted(set(sent) | set(got)))
if allsame and sent: ok(f"every event the host sent was applied (applied/sent: {line})")
else: bad(f"applied/sent differ: {line}")
left = sum(1 for p in H if p and p[0] == 'E' and int(p[6]) == pn)
ok(f"{left} events left out for the client (its own shots, their sparks and flames)") if left else bad("no event was left out for the client's own shots")
# kinds
def has(kind, cond=lambda l: True): return sum(1 for l in C if l.startswith("A ") and l.split()[3] == kind and cond(l))
beams = has("fireslot", lambda l: "beam 1" in l); pbeams = has("playershot", lambda l: "beam 1" in l)
kinds = {"sim tracers": beams, "player shots": has("playershot"), "explosions": has("explosion"), "sparks": has("sparks"),
         "chr hits": has("chrdamage"), "grunts": has("choke"), "deaths": has("death")}
if all(v > 0 for v in kinds.values()): ok("kinds applied: " + ", ".join(f"{k} {v}" for k, v in kinds.items()) + f" (player tracers {pbeams})")
else: bad("kinds missing: " + ", ".join(f"{k} {v}" for k, v in kinds.items()))
# the client's own hits, registered by the host
hh = sum(1 for p in H if p and p[0] == 'E' and p[2] == 'chrdamage' and f"attacker player {pn}" in " ".join(p) and "victim id" in " ".join(p))
ch = has("chrdamage", lambda l: "BY-ME" in l)
hud = [l for l in C if l.startswith("A ") and l.split()[3] == "hudmsg"]
if hh and ch:
    ok(f"the client's player hit sims {hh} times on the host; the client had {ch} of those hit events back")
else:
    bad(f"the client's hits: host registered {hh}, client got {ch}")
if hud:
    texts = sorted({h.split(' ', 6)[-1] for h in hud})
    ok(f"{len(hud)} hudmsgs the host made for the client's player shown there: {', '.join(texts[:8])}")
else:
    bad("no hudmsg came for the client's player")
sys.exit(st)
PY

# audio: the shot sounds the client applied, each at the audio frame it was
# mixed from (the game's own dump of what it queued, so the two line up),
# against that mix. The match is loud most of the time (eight sims fighting
# round the client), so the measure is the sound a shot starts in a quiet
# moment: a sim within 1500 units whose gun's sound began with the event,
# the mix quiet for the 100 ms before; does the mix turn loud within 120 ms,
# as against any quiet moment of the run? (Further off, behind the arena's
# walls, the game's own room-by-room volume lets few through: printed only.)
python3 - "$OUT/client.pcm" "$OUT/client.events" "$SHOTDIR" <<'PY' || status=1
import sys, os, subprocess
import numpy as np
pcm, evp, shotdir = sys.argv[1:4]
rate = 22020
if not os.path.exists(pcm) or os.path.getsize(pcm) < rate * 4 * 10:
    print(f"FAIL audio: no mix from the client ({pcm})"); sys.exit(1)
a = np.fromfile(pcm, dtype='<i2').reshape(-1, 2).astype(np.float32).mean(axis=1)
H = int(rate * 0.01)                       # 10 ms hops
nb = len(a) // H
lr = np.log(np.sqrt((a[:nb * H].reshape(nb, H) ** 2).mean(axis=1) + 1.0))
QUIET, LOUD = np.log(250), np.log(1100)
def dist(j): return float(j.split(" dist ")[1].split()[0]) if " dist " in j else 1e9
ev = [l.rstrip("\n") for l in open(evp) if l.startswith("A ")]
sounds = sum(1 for l in ev if (l.split()[3] == "fireslot" and "sound 1" in l) or l.split()[3] == "playershot")
def began(lo, hi): return np.array([int(l.split()[5]) // H for l in ev if l.split()[3] == "fireslot" and "started 1" in l and lo <= dist(l) < hi])
near = began(0, 1500)
def outcome(k):
    k = k[(k >= 10) & (k + 13 < nb)]
    quiet = np.array([lr[i - 10:i].mean() < QUIET for i in k], dtype=bool)
    loud = np.array([lr[i:i + 12].max() > LOUD for i in k], dtype=bool)
    return quiet.sum(), (quiet & loud).sum()
nq, nl = outcome(near)
rq, rl = outcome(np.random.default_rng(1).integers(10, nb - 14, 20000))
p, pr = nl / max(nq, 1), rl / max(rq, 1)
far = ", ".join("%d-%s units %d/%d" % ((lo, hi if hi < 1e9 else "", ) + outcome(began(lo, hi))[::-1]) for lo, hi in ((1500, 3000), (3000, 1e9)))
print(f"     audio: {len(a) / rate:.0f} s mixed, {sounds} shot sounds applied, {len(near)} began a sim's gun sound within 1500 units; "
      f"of the {nq} that began in a quiet moment the mix turned loud within 120 ms after {nl} ({p:.0%}); after a random quiet moment {pr:.0%} ({rq} tried); further off: {far}")
try:
    os.makedirs(shotdir, exist_ok=True)
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-f", "s16le", "-ar", str(rate), "-ac", "2", "-i", pcm,
                    "-lavfi", "showspectrumpic=s=1200x300:legend=0", "-frames:v", "1", "-update", "1", os.path.join(shotdir, "events-audio.png")], check=False)
except Exception:
    pass
if nq >= 5 and p >= 0.6 and p >= 4 * pr:
    print("ok   audio: the host's shots are heard in the client's mix")
else:
    print(f"FAIL audio: shots not heard ({nl}/{nq} against {pr:.0%})"); sys.exit(1)
PY

for f in explosion client host; do [ -s "$SHOTDIR/events-$f.png" ] || fail "no events-$f screenshot"; done
exit $status
