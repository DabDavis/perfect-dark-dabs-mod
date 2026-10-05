#!/bin/bash
# netsessiontest.sh — do two processes meet in a net game, and is a mismatch refused?
#
# Loopback (PLANS/netplay/spec-stage.md): a --dedicated host and one client
# (offscreen, on the GPU) on 127.0.0.1. The host starts the 0x32 match once
# the client has joined (--net-test-host 1); both must load the same stage
# (every stage hash component equal, the post-load RNG among them), pass the
# GO barrier, tick past frame 600, and the host must free the slot when the
# client quits at frame 700. The client's pd.ini has its own Mod.JumpHeight:
# the match plays the host's (SYNC) and the client's pd.ini keeps its own
# after the exit (H13), Game.MaxExplosions likewise. The client's --moddir
# ends in a slash (the stage key's mod name must still match), and its LEAVE
# on quitting must free the slot without an ENet timeout.
#
# Refusals, against a second host that wants a lobby ticket
# (Net.RequireTicket, pdlobbyd's key and room): a newer protocol, another
# --moddir (the "mod" component), no ticket, a MUST key that differs
# (Mod.BorrowGoldenEyeGuns) and a REFUSE key off stock (Mod.SimBrain) - the
# last two with a good ticket, so the ticket check is passed first. Each must
# be refused with its reason and component named, and the client exit 3.
# Host quits: a third host is stopped (SIGTERM) in the middle of a match;
# its client, with no --net-test-join, must say why, take the rules off
# (H12) and go back to the menus (the Carrington Institute).
# And --net-ticket-selftest: pdlobbyd's pinned vector.
#
#   netsessiontest.sh [BIN]   BIN a file name in build/ (pd.x86_64) or a path
#
# Env: OUT (build/netsession-out), PORT (27160; uses PORT to PORT+2),
# MODDIR (mod_allinone), FRAMES (700, the client's exit frame).
# Needs the ROM in build/data and an offscreen-capable GPU.
# Exit status: 0 all good, 1 a check failed, 2 a run failed to start.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=${BUILD:-$ROOT/build}
OUT=${OUT:-$BUILD/netsession-out}; PORT=${PORT:-27160}
MODDIR=${MODDIR:-mod_allinone}; FRAMES=${FRAMES:-700}
BIN=${1:-pd.x86_64}
case $BIN in /*) ;; */*) BIN=$(realpath "$BIN") ;; *) BIN=$BUILD/$BIN ;; esac
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
# the last run's logs go first: waitfor would find their lines before a
# new process has truncated its log (and kill a host that just started)
rm -f "$OUT"/*.log "$OUT"/*.stage
# no controller may reach a run (see replaytest.sh)
export SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-offscreen} SDL_GAMECONTROLLER_IGNORE_DEVICES=0x054c/0x0ce6,0x045e/0x028e \
	SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT=0x0000/0x0000 SDL_JOYSTICK_HIDAPI=0

ROOM=0a1b2c3d
SECRET=000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f
status=0
fail() { echo "FAIL $*"; status=1; }
pass() { echo "ok   $*"; }

# game LABEL INI TIMEOUT ARGS... &  - only ever run in the background: the
# subshell execs timeout, so $! is timeout's pid, and timeout passes a TERM
# on to the game (a dedicated host stops cleanly on it)
game() {
	local label=$1 ini=$2 t=$3; shift 3
	local save=$OUT/save-$label
	rm -rf "$save"; mkdir -p "$save"
	[ -n "$ini" ] && printf '%b' "$ini" > "$save/pd.ini"
	cd "$BUILD" || exit 2
	exec timeout -k 5 "$t" "$BIN" --savedir "$save" --skip-intro --no-sound "$@" > "$OUT/$label.log" 2>&1
}

# waits for a line in a log, up to N seconds
waitfor() {
	local log=$1 pattern=$2 n=$3
	while [ "$n" -gt 0 ]; do
		grep -q -- "$pattern" "$log" 2>/dev/null && return 0
		sleep 1; n=$((n - 1))
	done
	return 1
}

# a ticket the way pdlobbyd makes one, good for 60 s
ticket() {
	python3 -c '
import hmac, hashlib, os, sys, time
key = bytes.fromhex(sys.argv[1]); msg = "%s|%s|%d|%s" % (sys.argv[2], sys.argv[3], int(time.time()) + 60, os.urandom(16).hex())
print(msg + "|" + hmac.new(key, msg.encode(), hashlib.sha256).hexdigest())' "$SECRET" "$1" "$ROOM"
}

# --- the ticket self test ------------------------------------------------
mkdir -p "$OUT/save-selftest"
if ( cd "$BUILD" && timeout -k 5 60 "$BIN" --savedir "$OUT/save-selftest" --net-ticket-selftest > "$OUT/selftest.log" 2>&1 ); then
	pass "ticket self test (pdlobbyd's vector)"
else
	fail "ticket self test: $(grep -m1 'ticket self test' "$OUT/selftest.log")"
fi

# --- loopback and the refusal host, side by side ------------------------
game host "[Mod]\nJumpHeight=1\n[Game]\nMaxExplosions=96\n" 150 --moddir "$MODDIR" --dedicated --host "$PORT" \
	--net-test-host 1 --net-test-stage 0x32 --net-test-sims 4 --endless --rng-seed 12345 --state-hash 100 &
HOST=$!
game thost "[Net]\nRequireTicket=1\nRoomId=$ROOM\nRoomSecret=$SECRET\n" 150 --moddir "$MODDIR" --dedicated --host $((PORT + 1)) &
THOST=$!
# the hosts boot before anyone connects (a connect gives up after 10 s)
game qhost "" 150 --moddir "$MODDIR" --dedicated --host $((PORT + 2)) \
	--net-test-host 1 --net-test-stage 0x32 --net-test-sims 4 --endless &
QHOST=$!
for l in host thost qhost; do
	waitfor "$OUT/$l.log" "net: hosting on UDP port" 60 || { echo "FAIL: $l did not start (see $OUT/$l.log)"; kill -TERM $HOST $THOST $QHOST; exit 2; }
done

game client "[Mod]\nJumpHeight=3\n[Game]\nMaxExplosions=48\n" 120 --moddir "$MODDIR/" --connect "127.0.0.1:$PORT" --net-test-join \
	--state-hash 100 --exit-frame "$FRAMES" &
CLIENT=$!

T="127.0.0.1:$((PORT + 1))"
game r-protocol "" 90 --moddir "$MODDIR" --connect "$T" --net-test-join --net-test-protocol 99 & R1=$!
game r-mod "" 90 --connect "$T" --net-test-join & R2=$!
game r-ticket "" 90 --moddir "$MODDIR" --connect "$T" --net-test-join & R3=$!
game r-must "[Mod]\nBorrowGoldenEyeGuns=nosuchmod\n" 90 --moddir "$MODDIR" --connect "$T" --net-test-join \
	--net-ticket "$(ticket joiner)" & R4=$!
game r-notstock "[Mod]\nSimBrain=modern\n" 90 --moddir "$MODDIR" --connect "$T" --net-test-join \
	--net-ticket "$(ticket other)" & R5=$!

# the host that quits mid-match
game qclient "" 150 --moddir "$MODDIR" --connect "127.0.0.1:$((PORT + 2))" --state-hash 100 &
QCLIENT=$!
if waitfor "$OUT/qclient.log" "statehash: frame 300" 90; then
	kill -TERM "$QHOST" 2>/dev/null
	waitfor "$OUT/qclient.log" "net: back to the menus" 30
	waitfor "$OUT/qclient.log" "net: rules restored" 30
	sleep 2
fi
kill -TERM "$QHOST" "$QCLIENT" 2>/dev/null
wait "$QHOST"; wait "$QCLIENT"

wait $CLIENT; crc=$?
for p in $R1 $R2 $R3 $R4 $R5; do wait "$p"; done
sleep 3
kill -TERM "$HOST" "$THOST" 2>/dev/null
wait "$HOST"; wait "$THOST"

H=$OUT/host.log; C=$OUT/client.log
if ! grep -q "net: hosting on UDP port $PORT" "$H"; then
	echo "FAIL: the host did not start (see $H)"; exit 2
fi

# the same stage, component by component, the RNG after lvReset among them
grep -o 'net: stage hash .*' "$H" | sed 's/ ([0-9]*)$//' > "$OUT/host.stage"
grep -o 'net: stage hash .*' "$C" | sed 's/ ([0-9]*)$//' > "$OUT/client.stage"
if [ "$(wc -l < "$OUT/host.stage")" -ge 8 ] && cmp -s "$OUT/host.stage" "$OUT/client.stage"; then
	pass "loopback: stage hash equal ($(grep -c . "$OUT/host.stage") components, $(grep -o 'rng .*' "$OUT/host.stage"))"
else
	fail "loopback: stage hash differs or missing:"; paste "$OUT/host.stage" "$OUT/client.stage" | head -10
fi

grep -q "every machine has loaded; GO" "$H" && grep -q "net: match 1: GO" "$C" \
	&& pass "loopback: GO barrier released on both" || fail "loopback: no GO"

# past frame 600 on both
hf=$(sed -n '/every machine has loaded; GO/,$p' "$H" | grep -o 'statehash: frame [0-9]*' | awk '{print $3}' | sort -n | tail -1)
cf=$(sed -n '/net: match 1: GO/,$p' "$C" | grep -o 'statehash: frame [0-9]*' | awk '{print $3}' | sort -n | tail -1)
if [ "${hf:-0}" -gt 600 ] && [ "${cf:-0}" -gt 600 ] && [ $crc -eq 0 ] && grep -q "exit-frame $FRAMES reached" "$C"; then
	pass "loopback: host to frame $hf, client to frame $cf"
else
	fail "loopback: host to frame ${hf:-none}, client to frame ${cf:-none}, client exit $crc"
fi

# the client's LEAVE frees the slot at once, never an ENet timeout
if grep -qE 'net: slot 0 \("[^"]*"\) (left|is leaving [^;]*); the slot is free' "$H" && ! grep -q 'timed out' "$H"; then
	pass "loopback: the host freed the client's slot when it quit"
else
	fail "loopback: no slot freed on the host, or only by a timeout"
fi

if grep -q "Mod.JumpHeight 3 -> 1 (the host's" "$C" && grep -qx "JumpHeight=3" "$OUT/save-client/pd.ini"; then
	pass "loopback: Mod.JumpHeight synced for the match (3 -> 1), the client's pd.ini kept 3"
else
	fail "loopback: JumpHeight: $(grep -m1 'Mod.JumpHeight' "$C"), pd.ini $(grep -m1 '^JumpHeight' "$OUT/save-client/pd.ini")"
fi

# H13 covers Game.MaxExplosions too: the client took the host's 96 for the
# match, its pd.ini keeps its own 48
if grep -qx "MaxExplosions=48" "$OUT/save-client/pd.ini"; then
	pass "loopback: Game.MaxExplosions 48 kept in the client's pd.ini (the host has 96)"
else
	fail "loopback: client pd.ini $(grep -m1 '^MaxExplosions' "$OUT/save-client/pd.ini")"
fi

# the client's --moddir carried a trailing slash: the stage key still matched
grep -q "net: match 1: loading stage 0x32 of mod $MODDIR as" "$C" \
	&& pass "loopback: --moddir $MODDIR/ resolved the host's stage key" || fail "loopback: stage key with a trailing slash"

if grep -q "renderD128: Permission denied" "$C"; then
	fail "loopback: the client fell back to llvmpipe"
fi

# refusals: the reason and the component, and exit 3 on the client
refused() {
	local label=$1 pattern=$2 log=$OUT/$1.log
	local line; line=$(grep -m1 "net: the host refused" "$log")
	if [ -n "$line" ] && echo "$line" | grep -q -- "$pattern" && grep -q "net-test-join: exiting 3" "$log"; then
		pass "$label: ${line#*net: }"
	else
		fail "$label: wanted [$pattern], got: ${line:-nothing (see $log)}"
	fi
}
refused r-protocol "\[protocol protocol\]: This host runs netplay protocol"
refused r-mod "\[content mod\]: Your loaded mod"
refused r-ticket "\[ticket ticket\]: This room needs a join ticket"
refused r-must "\[must Mod.BorrowGoldenEyeGuns\]"
refused r-notstock "\[notstock Mod.SimBrain\]"

# the host quit mid-match: the client says why and goes back to the menus
Q=$OUT/qclient.log
if grep -q "net: the host is leaving \[shutdown\]: The host has quit" "$Q" && grep -q "net: back to the menus" "$Q" \
		&& grep -q "net: rules restored" "$Q"; then
	pass "host quit: $(grep -m1 'net: the host is leaving' "$Q" | sed 's/.*net: //'); rules restored, back to the menus"
else
	fail "host quit: $(grep -E 'net: (the host|session ended|back to|rules restored)' "$Q" | tr '\n' ';')"
fi

exit $status
