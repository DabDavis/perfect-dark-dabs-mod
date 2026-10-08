#!/bin/bash
# netchattest.sh — the online HUD (protocol 18; port/src/net/nethud.c,
# netproto.h CHAT and PLAYERS).
#
# Loopback, offscreen on the GPU, every client with a lobby ticket of its own
# (Net.RequireTicket, so a drop holds the seat): a listen host with
# Net.JoinInProgress, a ten-second Net.ReconnectHold and two sims, and client
# "alpha", start a timed match on the host's arena. Then:
#
#   1. chat: the host's line and alpha's reach every machine in the session,
#      in order, under the sender's name (the log's "net: chat:" lines);
#      a line long enough to wrap is drawn wrapped (screenshot);
#   2. a burst: alpha says seven lines on one tick; the host passes
#      NETCHAT_BURST (5) and drops the rest, and alpha alone hears why;
#   3. "bravo" joins the match in progress: every machine's HUD says
#      "bravo joined the game (3/4)" (the seats played out of the match's);
#      bravo's first PLAYERS gives each seat's ping, the host's 0;
#   4. a spectator: "charlie is watching" everywhere, and in bravo's players
#      panel (--net-test-players, screenshot);
#   5. alpha killed (a drop): "alpha lost the connection: the seat is kept
#      10 s", and back within the hold: "alpha is back (3/4)";
#   6. bravo killed and left down: the drop, then "bravo did not come back:
#      the seat is open"; the spectator killed: "charlie stopped watching";
#   7. the host's pause menu (--net-test-players-page): its Control page
#      with the Players row, then the Players page (screenshots);
#   8. the host's open chat line (--net-test-chat-type, screenshot).
#
# Screenshots: build/net-shots/chat-*.png.
#
#   netchattest.sh [BIN]   BIN a file name in build/ (pd.x86_64) or a path
#
# Env: OUT (build/netchat-out), PORT (27480), CHECKONLY (1: the checks only).
# Exit status: 0 all good, 1 a check failed, 2 a run failed to start.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=${BUILD:-$ROOT/build}
OUT=${OUT:-$BUILD/netchat-out}; PORT=${PORT:-27480}
BIN=${1:-pd.x86_64}
case $BIN in /*) ;; */*) BIN=$(realpath "$BIN") ;; *) BIN=$BUILD/$BIN ;; esac
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
SHOTDIR=$BUILD/net-shots
ROOM=63686174
SECRET=6f6e6c696e652068756420676174652073656372657420666f72206368617421
export SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-offscreen} SDL_GAMECONTROLLER_IGNORE_DEVICES=0x054c/0x0ce6,0x045e/0x028e \
	SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT=0x0000/0x0000 SDL_JOYSTICK_HIDAPI=0

status=0
fail() { echo "FAIL $*"; status=1; }
pass() { echo "ok   $*"; }

ticket() {
	python3 -c '
import hmac, hashlib, os, sys, time
key = bytes.fromhex(sys.argv[1]); msg = "%s|%s|%d|%s" % (sys.argv[2], sys.argv[3], int(time.time()) + 60, os.urandom(16).hex())
print(msg + "|" + hmac.new(key, msg.encode(), hashlib.sha256).hexdigest())' "$SECRET" "$1" "$ROOM"
}

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

gamepid() {
	local label=$1 p
	for p in $(pgrep -x "$(basename "$BIN" | cut -c1-15)"); do
		tr '\0' ' ' < "/proc/$p/cmdline" 2>/dev/null | grep -q -- "--savedir $OUT/save-$label " && { echo "$p"; return; }
	done
}

shot1() {
	local pid=$1 dest=$2 before f i
	rm -f "$dest"
	[ -n "$pid" ] || { echo "     shot: no game for $dest"; return; }
	mkdir -p "$BUILD/screenshots" "$SHOTDIR"
	before=$(ls "$BUILD/screenshots" 2>/dev/null | sort)
	gdb -p "$pid" -batch -ex 'call (void)screenshotRequest()' >/dev/null 2>&1
	for i in $(seq 50); do
		f=$(comm -13 <(echo "$before") <(ls "$BUILD/screenshots" 2>/dev/null | sort) | head -1)
		[ -n "$f" ] && { sleep 0.3; mv "$BUILD/screenshots/$f" "$dest"; echo "     shot: $dest"; return; }
		sleep 0.1
	done
	echo "     shot: none from $pid"
}

hosttick() { grep -o "net: trace tick [0-9]*" "$OUT/host.log" 2>/dev/null | tail -1 | awk '{print $4}'; }
waittick() {
	local want=$1 n=$2 t
	while [ "$n" -gt 0 ]; do
		t=$(hosttick); [ "${t:-0}" -ge "$want" ] && return 0
		sleep 1; n=$((n - 1))
	done
	return 1
}

for t in $(seq 60 60 15300); do echo "$t $((t + 20)) 2000 0 40 0 0 0.6 0"; done > "$OUT/play.script"
# the host's stops before its pause pages: Z is the menus' select
for t in $(seq 60 60 3900); do echo "$t $((t + 20)) 2000 0 40 0 0 0.6 0"; done > "$OUT/host.script"

LONG="a line from alpha that is long enough to wrap onto a second line of the feed, and then some more"
BURST="700:burst 1|700:burst 2|700:burst 3|700:burst 4|700:burst 5|700:burst 6|700:burst 7"
HINI="[Net]\nRequireTicket=1\nRoomId=$ROOM\nRoomSecret=$SECRET\nJoinInProgress=1\nReconnectHold=10\nMaxPlayers=4\n[Mod]\nStartArmed=1\n"
CINI="[Mod]\nStartArmed=1\n"

if [ "${CHECKONLY:-0}" != 1 ]; then
	rm -f "$OUT"/*.log "$SHOTDIR"/chat-*.png
	{
	echo "== host + alpha"
	game host 600 "$HINI" --host "$PORT" --net-test-host 1 --rng-seed 11 --net-test-sims 2 --net-test-timelimit 4 --net-test-scorelimit 100 --net-test-trace 60 \
		--net-test-input "$OUT/host.script" --net-test-chat "300:hello from the host" \
		--net-test-players-page 4200 --net-test-chat-type "5400:typing a line on the host" &
	waitfor "$OUT/host.log" "net: hosting on UDP port" 90 || { echo "FAIL: the host did not start"; exit 2; }
	game alpha1 300 "$CINI" --connect "127.0.0.1:$PORT" --net-ticket "$(ticket alpha)" --net-test-join \
		--net-test-input "$OUT/play.script" --net-test-chat "400:$LONG|$BURST" &
	waitfor "$OUT/alpha1.log" "net: match 1: GO" 150 || echo "     alpha never had GO"
	waittick 760 120
	AP=$(gamepid alpha1)
	shot1 "$AP" "$SHOTDIR/chat-alpha-feed.png"

	echo "== bravo joins in progress"
	waittick 900 60
	game bravo 500 "$CINI" --connect "127.0.0.1:$PORT" --net-ticket "$(ticket bravo)" --net-test-join \
		--net-test-input "$OUT/play.script" --net-test-players 10 &
	waitfor "$OUT/bravo.log" "net: match 1: GO, in progress" 120 || echo "     bravo never had GO"

	echo "== a spectator"
	game spec 480 "$CINI" --connect "127.0.0.1:$PORT" --net-ticket "$(ticket charlie)" --net-test-join --net-spectate &
	waitfor "$OUT/spec.log" "net: match 1: GO, in progress" 120 || echo "     the spectator never had GO"
	sleep 5
	shot1 "$(gamepid bravo)" "$SHOTDIR/chat-bravo-panel.png"

	echo "== alpha dropped, and back within the hold"
	kill -KILL "$AP"
	echo "     alpha1 killed at host tick $(hosttick)"
	waitfor "$OUT/host.log" "alpha lost the connection" 30 || echo "     the host never saw alpha go"
	game alpha2 400 "$CINI" --connect "127.0.0.1:$PORT" --net-ticket "$(ticket alpha)" --net-test-join \
		--net-test-input "$OUT/play.script" &
	waitfor "$OUT/alpha2.log" "net: match 1: GO, in progress" 120 || echo "     alpha2 never had GO"
	sleep 4

	echo "== bravo dropped for good, the spectator gone"
	kill -KILL "$(gamepid bravo)"
	echo "     bravo killed at host tick $(hosttick)"
	waitfor "$OUT/host.log" "bravo did not come back" 60 || echo "     bravo's hold never ran out"
	kill -KILL "$(gamepid spec)"
	waitfor "$OUT/host.log" "charlie stopped watching" 30 || echo "     the host never saw charlie go"

	echo "== the host's pause menu and its open line"
	waittick 4240 200
	shot1 "$(gamepid host)" "$SHOTDIR/chat-host-control.png"
	waittick 4440 60
	shot1 "$(gamepid host)" "$SHOTDIR/chat-host-players.png"
	waittick 5480 120
	shot1 "$(gamepid host)" "$SHOTDIR/chat-host-line.png"
	sleep 2
	for l in alpha2 host; do
		p=$(gamepid "$l"); [ -n "$p" ] && kill -TERM "$p"
	done
	wait
	echo "== runs done"
	} 2>&1 | tee "$OUT/run.log"
fi

H=$OUT/host.log
grep -q "renderD128: Permission denied" "$OUT"/*.log && fail "a run fell back to llvmpipe"
for l in host alpha1 alpha2 bravo spec; do
	if grep -qE "FATAL|Segmentation|Aborted" "$OUT/$l.log"; then
		fail "$l crashed"; grep -A14 "FATAL" "$OUT/$l.log" | head -16 | sed 's/^/     /'
	fi
done

has() { grep -qF -- "$2" "$OUT/$1.log"; }
count() { grep -cF -- "$2" "$OUT/$1.log"; }

# --- 1. chat everywhere, under the sender's name, in order
hn=$(grep -o 'net: chat: [^:]*: hello from the host' "$H" | head -1 | sed 's/net: chat: //; s/: hello.*//')
[ -n "$hn" ] && has alpha1 "net: chat: $hn: hello from the host" && pass "the host's line reached alpha under its name (\"$hn\")" \
	|| fail "the host's line: host [$hn], alpha [$(grep -c 'hello from the host' "$OUT/alpha1.log")]"
for l in host alpha1; do
	has "$l" "net: chat: alpha: $LONG" && pass "$l: alpha's long line, whole" || fail "$l: alpha's long line missing"
done
order=$(grep -o 'net: chat: alpha: burst [0-9]' "$OUT/alpha1.log" | awk '{print $NF}' | tr '\n' ' ')
[ "$order" = "1 2 3 4 5 " ] && pass "alpha: the burst's lines in order, five of them: $order" || fail "alpha: the burst's lines: [$order]"

# --- 2. the burst's limit
hb=$(count host "net: chat: alpha: burst"); hd=$(count host "dropped: too many at once")
[ "$hb" = 5 ] && [ "$hd" = 2 ] && pass "host: the burst cut to 5 lines, 2 dropped" || fail "host: burst lines $hb, dropped $hd"
ab=$(count alpha1 "net: notice: Too many lines at once")
bb=$(count bravo "Too many lines at once")
[ "$ab" = 2 ] && [ "$bb" = 0 ] && pass "alpha alone heard why, twice" || fail "too many: alpha $ab, bravo $bb"

# --- 3. a join in progress
for l in host alpha1 bravo; do
	has "$l" "net: notice: bravo joined the game (3/4)" && pass "$l: \"bravo joined the game (3/4)\"" || fail "$l: no join notice"
done
pl=$(grep -m1 -o "net: players (tick [0-9]*): seat pings [^;]*; [0-9]* watching" "$OUT/bravo.log")
echo "$pl" | grep -q "pings 0:0 " && pass "bravo: $(echo "$pl" | sed 's/net: //')" || fail "bravo: PLAYERS [$pl]"

# --- 4. the spectator
for l in host alpha1 bravo; do
	has "$l" "net: notice: charlie is watching" && pass "$l: \"charlie is watching\"" || fail "$l: no spectator notice"
done

# --- 5. alpha's drop and return
has host "net: notice: alpha lost the connection: the seat is kept 10 s" && pass "host: alpha's drop, the seat kept" || fail "host: no drop notice for alpha"
has bravo "net: notice: alpha lost the connection: the seat is kept 10 s" && pass "bravo: alpha's drop" || fail "bravo: no drop notice for alpha"
has host "net: notice: alpha is back (3/4)" && has bravo "net: notice: alpha is back (3/4)" && pass "host and bravo: \"alpha is back (3/4)\"" \
	|| fail "no return notice for alpha"

# --- 6. bravo gone for good, the spectator gone
has host "net: notice: bravo did not come back: the seat is open" && has alpha2 "net: notice: bravo did not come back: the seat is open" \
	&& pass "host and alpha2: \"bravo did not come back: the seat is open\"" || fail "no hold-ran-out notice for bravo"
has host "net: notice: charlie stopped watching" && has alpha2 "net: notice: charlie stopped watching" \
	&& pass "host and alpha2: \"charlie stopped watching\"" || fail "no notice of the spectator going"

# --- 7. the pause menu
has host "--net-test-players-page: the Control page" && has host "--net-test-players-page: the Players page" \
	&& pass "host: the pause's Control page and its Players page opened" || fail "host: the pause pages never opened"

for f in alpha-feed bravo-panel host-control host-players host-line; do
	[ -s "$SHOTDIR/chat-$f.png" ] || fail "no screenshot chat-$f.png"
done

echo
[ "$status" = 0 ] && echo "netchattest: all good" || echo "netchattest: FAILED"
exit $status
