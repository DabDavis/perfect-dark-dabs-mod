#!/bin/bash
# netjointest.sh — join in progress, spectators and reconnects (PLANS/NETPLAY.md
# phase 7; netproto.h "Join in progress", protocol 9).
#
# Loopback, offscreen on the GPU, every machine with a lobby ticket of its own
# (Net.RequireTicket, as a room's host): a listen host with Net.JoinInProgress
# (all four seats in the match) and two sims, and client "alpha", start a
# timed match on Skedar. Then:
#
#   1. "bravo" joins the match in progress (launched about host tick 500, in
#      by about 900) and plays: an open seat, its player back into play at a
#      respawn point, its commands played by the host, its kill table the
#      host's at every sample after it joined and at the end, its puppets and
#      events sane;
#   2. a spectator joins: no seat (the host's seats and PLAYERCOUNT the same),
#      snapshots and events, its camera following the players one after
#      another (--net-test-spec-cycle) and then flying free; screenshots;
#   3. alpha (given three kills at tick 1500, --net-test-givekills, since
#      nobody's shots reliably land here) is killed (SIGKILL) once the host's
#      table shows them, and started again
#      within the hold: the new game is given its seat back with its score
#      (the host's seat line, points, deaths and kills, when the seat was
#      taken back and after the return the same),
#      its kill table the host's. A new game in before the host has timed
#      the old one out takes the seat over from it (same account, a fresh
#      ticket); one in later finds it held;
#   4. alpha's game is killed again and left down 35 s: the hold runs out
#      30 s after its last message (not when the transport noticed), the
#      seat opens with its row cleared; started again it is a new player in
#      an open seat with nothing on its score;
#   5. Pop a Cap with two seats out of play (a second, shorter session):
#      the victim's turn passes over them, and never rests on one.
#
# The match ends on the host's clock; MATCH_END reaches everyone still in.
# Screenshots: build/net-shots/join-*.png.
#
#   netjointest.sh [BIN]   BIN a file name in build/ (pd.x86_64) or a path
#
# Env: OUT (build/netjoin-out), PORT (27400), CHECKONLY (1: the checks only).
# Exit status: 0 all good, 1 a check failed, 2 a run failed to start.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=${BUILD:-$ROOT/build}
OUT=${OUT:-$BUILD/netjoin-out}; PORT=${PORT:-27400}
BIN=${1:-pd.x86_64}
case $BIN in /*) ;; */*) BIN=$(realpath "$BIN") ;; *) BIN=$BUILD/$BIN ;; esac
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
SHOTDIR=$BUILD/net-shots
ROOM=4a6f696e
SECRET=6a6f696e2d696e2d70726f6772657373206761746520736563726574206b6579
export SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-offscreen} SDL_GAMECONTROLLER_IGNORE_DEVICES=0x054c/0x0ce6,0x045e/0x028e \
	SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT=0x0000/0x0000 SDL_JOYSTICK_HIDAPI=0

status=0
fail() { echo "FAIL $*"; status=1; }
pass() { echo "ok   $*"; }

# a ticket the way pdlobbyd makes one, good for 60 s, single use
ticket() {
	python3 -c '
import hmac, hashlib, os, sys, time
key = bytes.fromhex(sys.argv[1]); msg = "%s|%s|%d|%s" % (sys.argv[2], sys.argv[3], int(time.time()) + 60, os.urandom(16).hex())
print(msg + "|" + hmac.new(key, msg.encode(), hashlib.sha256).hexdigest())' "$SECRET" "$1" "$ROOM"
}

# game LABEL TIMEOUT INI ARGS...
game() {
	local label=$1 t=$2 ini=$3; shift 3
	local save=$OUT/save-$label
	rm -rf "$save"; mkdir -p "$save"
	printf '%b' "$ini" > "$save/pd.ini"
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

# the game's pid for a run label, by its own --savedir (never pgrep -f)
gamepid() {
	local label=$1 p
	for p in $(pgrep -x "$(basename "$BIN" | cut -c1-15)"); do
		tr '\0' ' ' < "/proc/$p/cmdline" 2>/dev/null | grep -q -- "--savedir $OUT/save-$label " && { echo "$p"; return; }
	done
}

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

# alpha's kills of others in the host's latest kill table sample (slot 1,
# the first client's), printed; status 1 while it has none
alphakills() {
	python3 - "$OUT/host.events" <<'PY2'
import sys
row = None
for l in open(sys.argv[1]):
    p = l.split()
    if len(p) >= 3 and p[0] == 'K' and p[1] == 'tick':
        row = next((e for e in p[3:] if e.startswith('1:')), '')
if not row: sys.exit(1)
k = sum(int(x.split('=')[1]) for x in row.split(':', 2)[2].split(',') if '=' in x and x.split('=')[0] != '1')
print(row) if k else None
sys.exit(0 if k else 1)
PY2
}

# the host's match tick, from its trace lines
hosttick() { grep -o "net: trace tick [0-9]*" "$OUT/host.log" 2>/dev/null | tail -1 | awk '{print $4}'; }
waittick() {
	local want=$1 n=$2 t
	while [ "$n" -gt 0 ]; do
		t=$(hosttick); [ "${t:-0}" -ge "$want" ] && return 0
		sleep 1; n=$((n - 1))
	done
	return 1
}

# fire now and then and walk about: a dead player's press respawns it, and
# the shots make the kill table move
for t in $(seq 60 60 15300); do echo "$t $((t + 20)) 2000 0 40 0 0 0.6 0"; done > "$OUT/play.script"

# the spectator's stick: forward and turning, once its camera flies free
echo "2500 2700 0 0 70 0 0 0.5 0" > "$OUT/spec.script"

HINI="[Net]\nRequireTicket=1\nRoomId=$ROOM\nRoomSecret=$SECRET\nJoinInProgress=1\nReconnectHold=30\n[Mod]\nStartArmed=1\n"
CINI="[Mod]\nStartArmed=1\n"

if [ "${CHECKONLY:-0}" != 1 ]; then
	rm -f "$OUT"/*.log "$OUT"/*.events "$OUT"/*.scen "$SHOTDIR"/join-*.png
	{
	echo "== host + alpha"
	game host 720 "$HINI" --host "$PORT" --net-test-host 1 --rng-seed 11 --net-test-sims 2 --net-test-timelimit 3 --net-test-givekills 1,1500,3 --net-test-scorelimit 100 --net-test-teamscorelimit 400 \
		--net-test-trace 60 --net-event-log "$OUT/host.events" --net-test-input "$OUT/play.script" &
	HOST=$!
	waitfor "$OUT/host.log" "net: hosting on UDP port" 90 || { echo "FAIL: the host did not start"; exit 2; }
	game alpha1 680 "$CINI" --connect "127.0.0.1:$PORT" --net-ticket "$(ticket alpha)" --net-test-join \
		--net-event-log "$OUT/alpha1.events" --net-test-input "$OUT/play.script" &
	waitfor "$OUT/alpha1.log" "net: match 1: GO" 150 || echo "     alpha never had GO"

	echo "== bravo joins at host tick ~500"
	waittick 500 120
	game bravo 640 "$CINI" --connect "127.0.0.1:$PORT" --net-ticket "$(ticket bravo)" --net-test-join \
		--net-event-log "$OUT/bravo.events" --net-test-input "$OUT/play.script" &
	BRAVO=$!
	waitfor "$OUT/bravo.log" "net: match 1: GO, in progress" 120 || echo "     bravo never had GO"

	echo "== a spectator joins"
	game spec 620 "$CINI" --connect "127.0.0.1:$PORT" --net-ticket "$(ticket charlie)" --net-test-join --net-spectate \
		--net-test-spec-cycle 150 --net-test-spec-free 2400 --net-test-input "$OUT/spec.script" &
	SPEC=$!
	waitfor "$OUT/spec.log" "net: match 1: GO, in progress" 120 || echo "     the spectator never had GO"
	sleep 6
	SP=$(gamepid spec)
	for k in 1 2 3; do
		[ -n "$SP" ] && shot1 "$SP" "$SHOTDIR/join-spectator-$k.png"
		sleep 2.6
	done
	BP=$(gamepid bravo)
	[ -n "$BP" ] && shot1 "$BP" "$SHOTDIR/join-bravo.png"

	echo "== alpha killed (once it has a kill) and back within the hold"
	AP=$(gamepid alpha1)
	sleep 4
	for i in $(seq 60); do
		alphakills && break
		sleep 1
	done
	alphakills && echo "     alpha has kills: $(alphakills)" || echo "     alpha has no kills by host tick $(hosttick)"
	[ -n "$AP" ] && kill -KILL "$AP"
	echo "     alpha1 killed at host tick $(hosttick)"
	sleep 3
	game alpha2 500 "$CINI" --connect "127.0.0.1:$PORT" --net-ticket "$(ticket alpha)" --net-test-join \
		--net-event-log "$OUT/alpha2.events" --net-test-input "$OUT/play.script" &
	waitfor "$OUT/alpha2.log" "net: match 1: GO, in progress" 120 || echo "     alpha2 never had GO"
	sleep 15
	SP=$(gamepid spec)
	[ -n "$SP" ] && shot1 "$SP" "$SHOTDIR/join-spectator-free.png"

	echo "== alpha killed and left down 35 s"
	AP=$(gamepid alpha2)
	[ -n "$AP" ] && kill -KILL "$AP"
	echo "     alpha2 killed at host tick $(hosttick)"
	waitfor "$OUT/host.log" "\"alpha\") dropped; held" 40 || echo "     the host never saw alpha2 drop"
	sleep 35
	game alpha3 400 "$CINI" --connect "127.0.0.1:$PORT" --net-ticket "$(ticket alpha)" --net-test-join \
		--net-event-log "$OUT/alpha3.events" --net-test-input "$OUT/play.script" &
	waitfor "$OUT/alpha3.log" "net: match 1: GO, in progress" 120 || echo "     alpha3 never had GO"

	echo "== to the match's end"
	waitfor "$OUT/host.log" "scores and awards sent" 420 || echo "     no MATCH_END"
	sleep 8
	for l in alpha3 bravo spec host; do
		p=$(gamepid "$l"); [ -n "$p" ] && kill -TERM "$p"
	done
	wait

	echo "== Pop a Cap with two seats out of play"
	game pachost 400 "$HINI" --host "$((PORT + 1))" --net-test-host 1 --rng-seed 5 --net-test-sims 3 --net-test-scenario 3 --net-test-timelimit 2 \
		--net-scen-log "$OUT/pachost.scen" --net-test-input "$OUT/play.script" &
	waitfor "$OUT/pachost.log" "net: hosting on UDP port" 90 || echo "     the Pop a Cap host did not start"
	game pacalpha 380 "$CINI" --connect "127.0.0.1:$((PORT + 1))" --net-ticket "$(ticket alpha)" --net-test-join \
		--net-test-input "$OUT/play.script" &
	waitfor "$OUT/pachost.log" "scores and awards sent" 300 || echo "     no MATCH_END in Pop a Cap"
	sleep 5
	for l in pacalpha pachost; do
		p=$(gamepid "$l"); [ -n "$p" ] && kill -TERM "$p"
	done
	wait
	echo "== runs done"
	} 2>&1 | tee "$OUT/run.log"
fi

H=$OUT/host.log
lastline() { grep -- "$2" "$1" 2>/dev/null | tail -1; }
num() { echo "$1" | grep -o -- "$2 [0-9]*" | head -1 | awk '{print $NF}'; }

grep -q "renderD128: Permission denied" "$OUT"/*.log && fail "a run fell back to llvmpipe"
for l in host alpha1 alpha2 alpha3 bravo spec pachost pacalpha; do
	if grep -qE "FATAL|Segmentation|Aborted" "$OUT/$l.log"; then
		fail "$l crashed"; grep -A14 "FATAL" "$OUT/$l.log" | head -16 | sed 's/^/     /'
	fi
done
grep -q "scores and awards sent" "$H" && pass "the match ended on the host's clock: $(lastline "$H" "net: match clock at the end" | sed 's/.*net: //')" \
	|| fail "no MATCH_END from the host"

# --- 1. bravo
B=$OUT/bravo.log
jb=$(grep -m1 -o "slot [0-9] (\"bravo\") is in the match in progress from tick [0-9]* ([^)]*)" "$H")
[ -n "$jb" ] && pass "host: $jb" || fail "host: bravo never came into the match"
grep -q "into slot [0-9] (an open seat of the match in progress)" "$B" && pass "bravo: $(grep -m1 -o "accepted.*" "$B")" || fail "bravo: no open seat"
grep -q "net: match 1: GO, in progress" "$B" && pass "bravo: $(grep -m1 -o "match 1: GO, in progress.*" "$B")" || fail "bravo: no GO in progress"
bslot=$(echo "$jb" | grep -o "slot [0-9]" | awk '{print $2}')
bline=$(grep "net: slot ${bslot:-9} commands \(so far\|at the match's end\)" "$H" | tail -1)
bplayed=$(echo "$bline" | grep -o "played [0-9]*" | awk '{print $2}')
[ "${bplayed:-0}" -ge 3000 ] && pass "host: bravo's commands: $(echo "$bline" | sed 's/.*commands [a-z ]*: //')" || fail "host: bravo's commands played: ${bplayed:-none}"
bsn=$(lastline "$B" "net: snap client"); bpu=$(lastline "$B" "net: puppets"); bev=$(lastline "$B" "net: events client")
if [ "$(echo "$bsn" | grep -o "[0-9]* malformed" | awk '{print $1}')" = 0 ] && [ "$(num "$bpu" "bad anims")" = 0 ] \
		&& [ "$(num "$bev" malformed)" = 0 ] && [ "$(num "$bev" overflow)" = 0 ]; then
	pass "bravo: snapshots, puppets and events sane ($(num "$bpu" poses) poses, $(echo "$bev" | grep -o "scores [0-9/]*") scores, $(echo "$bev" | grep -o "death [0-9/]*") deaths)"
else
	fail "bravo: faults: $(echo "$bsn" | grep -o "[0-9]* malformed"), bad anims $(num "$bpu" "bad anims"), events malformed $(num "$bev" malformed)"
fi

# kill tables: a client's against the host's at every sample after it came in
python3 - "$OUT" <<'PY' || status=1
import sys, re, os
out = sys.argv[1]
st = 0
def bad(m):
    global st; print("FAIL " + m); st = 1
def ok(m): print("ok   " + m)
def table(path):
    k = {}
    if not os.path.exists(path): return k
    for l in open(path):
        p = l.split()
        if p and p[0] == 'K': k[(p[1], int(p[2]))] = " ".join(p[3:])
    return k
H = table(out + "/host.events")
hend = [t for (w, t) in H if w == 'end']
for c, need_end in (("bravo", 1), ("alpha2", 0), ("alpha3", 1)):
    C = table(out + "/%s.events" % c)
    samples = sorted(t for (w, t) in C if w == 'tick' and ('tick', t) in H)
    same = [t for t in samples if C[('tick', t)] == H[('tick', t)]]
    diff = [t for t in samples if C[('tick', t)] != H[('tick', t)]]
    if samples and not diff and len(same) >= 2:
        ok(f"{c}: kill table the host's at all {len(same)} samples since it came in (ticks {samples[0]}..{samples[-1]})")
    else:
        bad(f"{c}: kill tables: {len(same)} equal, {len(diff)} differ")
        for t in diff[:3]: print(f"     tick {t}: host [{H[('tick', t)]}] {c} [{C[('tick', t)]}]")
    if need_end:
        ce = [v for (w, t), v in C.items() if w == 'end']
        he = [v for (w, t), v in H.items() if w == 'end']
        if ce and he and ce[0] == he[0]:
            ok(f"{c}: kill table at MATCH_END the host's: {he[0]}")
        else:
            bad(f"{c}: kill table at MATCH_END: host [{he[0] if he else '-'}] {c} [{ce[0] if ce else '-'}]")
sys.exit(st)
PY

# --- 2. the spectator
S=$OUT/spec.log
grep -q "accepted by .* as a spectator of the match in progress" "$S" && pass "spectator: $(grep -m1 -o "accepted.*" "$S")" || fail "spectator: not accepted as one"
grep -q "spectator view [0-9]* (\"charlie\") is in the match in progress" "$H" && pass "host: $(grep -m1 -o "spectator view [0-9]* (\"charlie\") is in the match in progress.*" "$H")" \
	|| fail "host: the spectator never came in"
seatsj=$(grep -o "net: seats after a join (tick [0-9]*): .*" "$H" | tail -1)
nseat=$(echo "$seatsj" | grep -o "[0-3] [a-z]* \"" | wc -l)
[ "$nseat" = 4 ] && ! echo "$seatsj" | grep -q "charlie" && pass "host: four seats, none the spectator's ($(echo "$seatsj" | sed 's/.*: \(0 .*\); joins.*/\1/'))" \
	|| fail "host: seats after the spectator: $seatsj"
grep -q "match 1: loading .* 4 players, this machine a spectator" "$S" && pass "spectator: $(grep -m1 -o "loading .*players, this machine a spectator[^,]*" "$S")" \
	|| fail "spectator: did not load as one"
follows=$(grep -o "following player [0-9]" "$S" | sort -u | wc -l)
[ "$follows" -ge 2 ] && pass "spectator: its camera followed $follows players: $(grep -o "following player [0-9] (\"[^\"]*\")" "$S" | sort -u | tr '\n' ' ')" \
	|| fail "spectator: followed $follows players"
flown=$(grep -o "flown [0-9]* units" "$S" | tail -1 | awk '{print $2}')
grep -q "net: spectator (tick [0-9]*): free camera" "$S" && [ "${flown:-0}" -ge 300 ] && pass "spectator: the free camera flew $flown units ($(grep -m1 -o "free camera at .*" "$S"))" \
	|| fail "spectator: free camera flown ${flown:-no} units"
ssum=$(lastline "$S" "net: spectator at\|net: spectator so far")
spu=$(lastline "$S" "net: puppets")
[ "$(num "$spu" poses)" -ge 1000 ] 2>/dev/null && [ "$(num "$spu" "bad anims")" = 0 ] && pass "spectator: $(num "$spu" poses) poses, its camera: $(echo "$ssum" | sed 's/.*): //')" \
	|| fail "spectator: puppets $spu"
sev=$(lastline "$S" "net: events client")
[ "$(num "$sev" malformed)" = 0 ] && pass "spectator: events $(echo "$sev" | grep -o "death [0-9/]*"), shot sounds $(num "$sev" "shot sounds")" || fail "spectator: events $sev"
for k in 1 2 3 free; do
	[ -s "$SHOTDIR/join-spectator-$k.png" ] && pass "screenshot $SHOTDIR/join-spectator-$k.png" || fail "no screenshot join-spectator-$k"
done

# --- 3. alpha back within the hold
A1=$OUT/alpha1.log; A2=$OUT/alpha2.log; A3=$OUT/alpha3.log
aslot=$(grep -m1 -o "into slot [0-9]" "$A1" | awk '{print $3}')
drop1=$(grep -m1 -o "seat [0-9] (\"alpha\") dropped; held for it [0-9]* s" "$H")
[ -n "$drop1" ] && pass "host: $drop1" || fail "host: alpha's first drop not held"
before=$(grep -m1 -o "net: seats after a drop (tick [0-9]*): .*" "$H")
back=$(grep -m1 -o "slot [0-9] (\"alpha\") is in the match in progress from tick [0-9]* (its held seat, its score kept)" "$H")
[ -n "$back" ] && pass "host: $back" || fail "host: alpha never back in its held seat"
grep -q "into slot $aslot (the seat this account held" "$A2" && pass "alpha2: $(grep -m1 -o "accepted.*" "$A2")" || fail "alpha2: not given its seat back (slot $aslot)"
python3 - "$H" "$aslot" <<'PY' || status=1
import sys, re
h, slot = sys.argv[1], sys.argv[2]
lines = [l for l in open(h) if "net: seats " in l]
def seat(l):
    m = re.search(r"\b%s (\w+) \"([^\"]*)\" points (-?\d+) deaths (\d+) kills (\d+)" % slot, l)
    return m.groups() if m else None
# the seats as the host had them when alpha's seat was taken back, and
# after its return
i = next((k for k, l in enumerate(lines) if "before a seat is taken back" in l), None)
j = next((l for l in lines[i + 1:] if "after a join" in l and seat(l) and seat(l)[1] == "alpha"), None) if i is not None else None
if i is None or j is None:
    print("FAIL alpha's seat lines: none before its seat was taken back, or none after"); sys.exit(1)
d, j = seat(lines[i]), seat(j)
if d[2:] == j[2:] and int(d[4]) > 0:
    print("ok   alpha's score through the drop: seat %s %s points %s deaths %s kills %s before, %s points %s deaths %s kills %s back" % (slot, d[0], d[2], d[3], d[4], j[0], j[2], j[3], j[4]))
else:
    print("FAIL alpha's score through the drop (kills must be some): points %s deaths %s kills %s before, %s %s %s back" % (d[2], d[3], d[4], j[2], j[3], j[4])); sys.exit(1)
PY

# --- 4. alpha after the hold ran out
exp=$(grep -m1 -o "seat [0-9] (\"alpha\"): the hold ran out ([0-9]* s); the seat is open, its score cleared" "$H")
[ -n "$exp" ] && pass "host: $exp" || fail "host: alpha's hold never ran out"
# the hold runs from alpha2's last message (its kill), not from when the
# transport noticed it gone (some 5 s later): the trace's tick at the kill
# is at most a second behind it
ktick=$(grep -o "alpha2 killed at host tick [0-9]*" "$OUT/run.log" | awk '{print $NF}')
etick=$(grep -o "net: seats after a hold ran out (tick [0-9]*)" "$H" | head -1 | grep -o "[0-9]*)" | tr -d ')')
if [ -n "$ktick" ] && [ -n "$etick" ] && [ $((etick - ktick)) -ge 1740 ] && [ $((etick - ktick)) -le 1950 ]; then
	pass "host: the hold ran out $((etick - ktick)) ticks after the kill (30 s is 1800; $(grep -m2 -o "seat [0-9] (\"alpha\") dropped; held for it .*" "$H" | tail -1))"
else
	fail "host: the hold ran out at tick ${etick:-?}, the kill at ${ktick:-?} ($(( ${etick:-0} - ${ktick:-0} )) ticks; 30 s is 1800)"
fi
new=$(grep -o "slot [0-9] (\"alpha\") is in the match in progress from tick [0-9]* ([^)]*)" "$H" | sed -n 2p)
echo "$new" | grep -q "an open seat" && pass "host: $new" || fail "host: alpha's third game: ${new:-never came in}"
grep -q "(an open seat of the match in progress)" "$A3" && pass "alpha3: $(grep -m1 -o "accepted.*" "$A3")" || fail "alpha3: not a new player"
last=$(grep -o "net: seats after a join (tick [0-9]*): .*" "$H" | tail -1)
s3=$(echo "$new" | grep -o "slot [0-9]" | awk '{print $2}')
echo "$last" | grep -q "$s3 taken \"alpha\" points 0 deaths 0 kills 0" && pass "host: the new alpha's score starts at 0 ($(echo "$last" | grep -o "$s3 taken \"alpha\" points 0 deaths 0[^;]*"))" \
	|| fail "host: the new alpha's seat: $last"
for c in alpha3; do
	ev=$(lastline "$OUT/$c.log" "net: events client")
	[ "$(num "$ev" malformed)" = 0 ] && [ -n "$ev" ] && pass "$c: events sane ($(echo "$ev" | grep -o "scores [0-9/]*") scores)" || fail "$c: events: ${ev:-none}"
done
[ -s "$SHOTDIR/join-bravo.png" ] && pass "screenshot $SHOTDIR/join-bravo.png" || fail "no screenshot join-bravo"
grep "net: seats at the match's end" "$H" | tail -1 | sed 's/^.*net: /     /'

# --- 5. Pop a Cap: seats 2 and 3 out of play all match
P=$OUT/pachost.log
grep -q "scores and awards sent" "$P" && pass "Pop a Cap: the match ran to its end ($(lastline "$P" "net: match clock at the end" | grep -o "tick [0-9]*"))" \
	|| fail "Pop a Cap: no MATCH_END"
grep -q "net: seats at the match.s end (tick [0-9]*): 0 host .*; 2 open .*; 3 open" "$P" && pass "Pop a Cap: seats 2 and 3 open" || fail "Pop a Cap: the seats were not host, alpha, open, open"
nskip=$(grep -c "net: Pop a Cap (tick [0-9]*): the turn passes over" "$P")
[ "$nskip" -ge 1 ] && pass "Pop a Cap: the victim's turn passed over seats out of play $nskip times ($(grep -m1 -o "Pop a Cap (tick [0-9]*): the turn passes over.*" "$P"))" \
	|| fail "Pop a Cap: the turn never passed over a seat out of play"
python3 - "$OUT/pachost.scen" <<'PY' || status=1
import sys, re
vs = []
for l in open(sys.argv[1]):
    m = re.search(r" vi=(-?\d+) v=(-?\d+) ", l)
    if l.startswith("S ") and m:
        t = int(l.split()[1])
        v = int(m.group(2))
        if not vs or vs[-1][1] != v: vs.append((t, v))
on = [(t, v) for t, v in vs if v in (2, 3)]
turns = [v for t, v in vs if v >= 0]
if on:
    print("FAIL Pop a Cap: the victim rested on a seat out of play: %s" % on[:4]); sys.exit(1)
if len(turns) < 3:
    print("FAIL Pop a Cap: only %d victims over the match: %s" % (len(turns), vs)); sys.exit(1)
print("ok   Pop a Cap: %d victims, never seat 2 or 3 (slots in turn: %s)" % (len(turns), " ".join(str(v) for v in turns[:16])))
PY
exit $status
