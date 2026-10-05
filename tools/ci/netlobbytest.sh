#!/bin/bash
# netlobbytest.sh — do two games meet through a lobby room and come back to it?
#
# Phase 6a (PLANS/NETPLAY.md "Lobby"): a local pdlobbyd (tools/pdlobbyd,
# ephemeral ports, --auth open) and two pairs of games offscreen on the GPU,
# each game signed in with its own Ghost Trials account and driven by
# --net-lobby-script through the same calls the Online Game menus make:
#
#   host: create a room from its Combat Simulator setup (0x32, 2 sims),
#         wait for a member to be READY, LAUNCH
#   join: list the rooms, find it by name, join, READY
#   both: the 5 s countdown runs out; the host's session (open since the
#         room was made, lobby tickets required) admits the joiner, whose
#         CONNECT carries its ticket - the host logs it verified (HMAC,
#         room, expiry on the lobby's clock, roster, nonce) - and the 0x32
#         match loads and passes GO on both, the stage hashes equal
#
#   "leave" pair: at frame 600 the joiner ends its game (End Game): it is
#         back in the room; the host, its client gone, ends its match too,
#         closes its end screen, reopens the room and is back in it; the
#         joiner's chat line reaches it there. Both menus are the Game Lobby.
#   "rematch" pair: the host ends each match at frame 300 (MATCH_END: the
#         joiner's end screen, then back to the room still connected); the
#         joiner READYs again and a second match is launched and played on
#         the same connection, no new ticket.
#
#   netlobbytest.sh [BIN]   BIN a file name in build/ (pd.x86_64) or a path
#
# Env: OUT (build/netlobby-out), GPORT (27180: the hosts' game ports, +1),
# MODDIR (mod_allinone), FRAME (600).
# Exit status: 0 all good, 1 a check failed, 2 a run failed to start.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=${BUILD:-$ROOT/build}
OUT=${OUT:-$BUILD/netlobby-out}; GPORT=${GPORT:-27180}
MODDIR=${MODDIR:-mod_allinone}; FRAME=${FRAME:-600}
BIN=${1:-pd.x86_64}
case $BIN in /*) ;; */*) BIN=$(realpath "$BIN") ;; *) BIN=$BUILD/$BIN ;; esac
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
[ -n "$OUT" ] && [ "$OUT" != / ] && rm -f "$OUT"/*.log "$OUT"/*.stage
export SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-offscreen} SDL_GAMECONTROLLER_IGNORE_DEVICES=0x054c/0x0ce6,0x045e/0x028e \
	SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT=0x0000/0x0000 SDL_JOYSTICK_HIDAPI=0

status=0
fail() { echo "FAIL $*"; status=1; }
pass() { echo "ok   $*"; }

waitfor() {
	local log=$1 pattern=$2 n=$3
	while [ "$n" -gt 0 ]; do
		grep -q -- "$pattern" "$log" 2>/dev/null && return 0
		sleep 1; n=$((n - 1))
	done
	return 1
}

# the lobby, on ports the kernel picks
python3 -u "$ROOT/tools/pdlobbyd/pdlobbyd.py" --host 127.0.0.1 --port 0 --udp-host 127.0.0.1 --udp-port 0 --auth open \
	> "$OUT/pdlobbyd.log" 2>&1 &
LOBBY=$!
if ! waitfor "$OUT/pdlobbyd.log" "pdlobbyd listening on" 20; then
	echo "FAIL: pdlobbyd did not start (see $OUT/pdlobbyd.log)"; kill $LOBBY 2>/dev/null; exit 2
fi
LPORT=$(sed -n 's/.*listening on 127.0.0.1:\([0-9]*\).*/\1/p' "$OUT/pdlobbyd.log" | head -1)
echo "     pdlobbyd on 127.0.0.1:$LPORT (pid $LOBBY)"

# game LABEL USER GAMEPORT TIMEOUT ARGS... &  ($! is timeout's pid)
game() {
	local label=$1 user=$2 port=$3 t=$4; shift 4
	local save=$OUT/save-$label
	rm -rf "$save"; mkdir -p "$save"
	# an agent file already chosen, as a player has one: a client back from a
	# match then lands on the menus, not Choose Your Reality (a copy of this
	# machine's save; the game is never pointed at the real dir)
	for f in eeprom.bin mpsetups.bin; do
		[ -f "$HOME/.local/share/perfectdark/$f" ] && cp "$HOME/.local/share/perfectdark/$f" "$save/"
	done
	printf '[Mod]\nGhostUser=%s\nGhostPin=1234\n[Net]\nLobbyServer=http://127.0.0.1:%s\nPort=%s\n' "$user" "$LPORT" "$port" > "$save/pd.ini"
	cd "$BUILD" || exit 2
	exec timeout -k 5 "$t" "$BIN" --savedir "$save" --skip-intro --no-sound --moddir "$MODDIR" "$@" > "$OUT/$label.log" 2>&1
}

game leave-host hostguy "$GPORT" 240 --net-lobby-script host --net-lobby-room "Leave Test" \
	--net-test-stage 0x32 --net-test-sims 2 --rng-seed 7 &
P1=$!
game leave-join joiner "$GPORT" 240 --net-lobby-script join --net-lobby-room "Leave Test" --net-lobby-leave-frame "$FRAME" &
P2=$!
game rematch-host hostguy2 $((GPORT + 1)) 300 --net-lobby-script host --net-lobby-room "Rematch Test" \
	--net-lobby-matches 2 --net-lobby-end-frame 300 --net-test-stage 0x32 --net-test-sims 2 --rng-seed 8 &
P3=$!
game rematch-join joiner2 $((GPORT + 1)) 300 --net-lobby-script join --net-lobby-room "Rematch Test" \
	--net-lobby-matches 2 --net-lobby-leave-frame 0 &
P4=$!
wait $P1; r1=$?; wait $P2; r2=$?; wait $P3; r3=$?; wait $P4; r4=$?
kill $LOBBY 2>/dev/null; wait $LOBBY 2>/dev/null
echo "     exits: leave host $r1 join $r2, rematch host $r3 join $r4"

L=$OUT/pdlobbyd.log
line() { grep -m1 -o -- "$2.*" "$1"; }

# the common path, for one pair
meet() {
	local label=$1 user=$2 H=$OUT/$1-host.log J=$OUT/$1-join.log room
	grep -q "renderD128: Permission denied" "$H" "$J" && fail "$label: a run fell back to llvmpipe"
	room=$(sed -n 's/.*lobby: made room \([0-9a-f]*\).*/\1/p' "$H" | head -1)
	ROOMID=$room
	[ -n "$room" ] && pass "$label: host made room $room: $(line "$L" "room $room '")" || { fail "$label: host made no room"; return; }
	grep -q "lobby script: found room $room" "$J" && pass "$label: join listed it: $(line "$J" "found room")" || fail "$label: join did not list the room"
	grep -q "lobby: joined room $room" "$J" && pass "$label: join joined room $room" || fail "$label: join did not join"
	grep -q "join: READY" "$J" && grep -q "a member is ready; LAUNCH" "$H" && pass "$label: join READY, host LAUNCH" || fail "$label: ready/launch missing"
	grep -q "room $room launched" "$L" && pass "$label: pdlobbyd: $(line "$L" "room $room launched")" || fail "$label: the room never launched"
	if grep -q "lobby ticket for \"$user\" in room $room verified" "$H"; then
		pass "$label: host: $(line "$H" "net: peer [0-9]*: lobby ticket")"
	else
		fail "$label: host never verified a ticket"
	fi
	grep -q "slot 1: \"$user\" joined" "$H" && pass "$label: host: $(line "$H" "net: slot 1: \"$user\" joined")" || fail "$label: $user never took slot 1"
	if grep -q "match 1: every machine has loaded; GO" "$H" && grep -q "net: match 1: GO" "$J"; then
		pass "$label: both passed GO on $(line "$H" "net: match 1 starting on" | sed 's/, seeds.*//')"
	else
		fail "$label: no GO on both"
	fi
	grep -o 'net: stage hash .*' "$H" | sed 's/ ([0-9]*)$//' | head -8 > "$OUT/$label-host.stage"
	grep -o 'net: stage hash .*' "$J" | sed 's/ ([0-9]*)$//' | head -8 > "$OUT/$label-join.stage"
	[ -s "$OUT/$label-host.stage" ] && cmp -s "$OUT/$label-host.stage" "$OUT/$label-join.stage" \
		&& pass "$label: stage hash equal ($(wc -l < "$OUT/$label-host.stage") components)" || fail "$label: stage hash differs"
}

meet leave joiner
H=$OUT/leave-host.log; J=$OUT/leave-join.log
grep -q "join: frame $FRAME; End Game" "$J" && pass "leave: join played to frame $FRAME and left the match" || fail "leave: join did not reach frame $FRAME in the match"
grep -q "the client left the match; End Game" "$H" && pass "leave: host ended its match after the client left" || fail "leave: host did not end its match"
grep -q "lobby script: host back in room $ROOMID" "$H" && pass "leave: $(line "$H" "host back in room")" || fail "leave: host not back in the room"
grep -q "lobby script: client back in room $ROOMID" "$J" && pass "leave: $(line "$J" "client back in room")" || fail "leave: client not back in the room"
grep -q "lobby: back in room $ROOMID's lobby" "$H" && pass "leave: host: Game Lobby menu up again" || fail "leave: host: no Game Lobby menu after the match"
grep -q "lobby: back in room $ROOMID's lobby" "$J" && pass "leave: join: Game Lobby menu up again" || fail "leave: join: no Game Lobby menu after the match"
grep -q "host heard joiner: \"back in the room\"" "$H" && pass "leave: host heard the client's chat in the room" || fail "leave: the chat line never reached the host"
[ "$r1" = 0 ] && [ "$r2" = 0 ] || fail "leave: exit codes host $r1 join $r2"

meet rematch joiner2
H=$OUT/rematch-host.log; J=$OUT/rematch-join.log
[ "$(grep -c 'host: frame [0-9]*; End Game' "$H")" = 2 ] && pass "rematch: host ended both matches ($(grep -o 'host: frame [0-9]*; End Game' "$H" | tr '\n' ' '))" \
	|| fail "rematch: host did not end two matches"
[ "$(grep -c 'join: the host ended the match' "$J")" = 2 ] && pass "rematch: join saw both end (MATCH_END, end screen closed)" || fail "rematch: join did not see two ends"
grep -q "client back in room $ROOMID after match 1 .*session still connected" "$J" && pass "rematch: $(line "$J" "client back in room $ROOMID after match 1")" \
	|| fail "rematch: join not back in the room still connected after match 1"
if grep -q "match 2: every machine has loaded; GO" "$H" && grep -q "net: match 2: GO" "$J"; then
	pass "rematch: match 2 passed GO on both"
else
	fail "rematch: no second match"
fi
[ "$(grep -c 'lobby ticket for' "$H")" = 1 ] && pass "rematch: one ticket, one connection for both matches" || fail "rematch: $(grep -c 'lobby ticket for' "$H") ticket checks"
grep -q "lobby script: host back in room $ROOMID after match 2" "$H" && pass "rematch: $(line "$H" "host back in room $ROOMID after match 2")" || fail "rematch: host not back after match 2"
grep -q "lobby script: client back in room $ROOMID after match 2" "$J" && pass "rematch: $(line "$J" "client back in room $ROOMID after match 2")" || fail "rematch: join not back after match 2"
[ "$(grep -c "lobby: back in room $ROOMID's lobby" "$J")" = 2 ] && pass "rematch: join: Game Lobby menu after each match" || fail "rematch: join: Game Lobby menu count $(grep -c "lobby: back in room $ROOMID's lobby" "$J")"
[ "$r3" = 0 ] && [ "$r4" = 0 ] || fail "rematch: exit codes host $r3 join $r4"
exit $status
