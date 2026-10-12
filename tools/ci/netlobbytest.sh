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
#         While its match runs the lobby says a notice (pdlobbyd's admin
#         port, POST /notice): both games put it in the match's chat as the
#         server's.
#   "rematch" pair: the host ends each match at frame 300 (MATCH_END: the
#         joiner's end screen, then back to the room still connected); the
#         joiner READYs again and a second match is launched and played on
#         the same connection, no new ticket.
#
#   "seat" (after the pairs; SEAT=0 skips it; F3 20261012-020922): a room
#         of three (host, alpha, bravo) launches on 0x32; a newcomer
#         (charlie) is turned away while the match has every seat (409
#         full); alpha quits the game at frame 900 (the lobby told on the
#         way out); charlie joins again and is taken into the running match
#         in alpha's seat, plays it to the host's end (frame 2400) and is
#         back in the room.
#
#   netlobbytest.sh [BIN]   BIN a file name in build/ (pd.x86_64) or a path
#
# Env: OUT (build/netlobby-out), GPORT (27180: the hosts' game ports, +1),
# ONLY (seat: the seat case alone), SEAT (0: without it),
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
# the rules pair's host's own settings: none of them stock
RULESINI='JumpHeight=3\nStartArmed=2\nAkimbo=1\nSimBrain=modern\n'
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
python3 -u "$ROOT/tools/pdlobbyd/pdlobbyd.py" --host 127.0.0.1 --port 0 --udp-host 127.0.0.1 --udp-port 0 --auth open --relay-ports 0 \
	--admin-port 0 > "$OUT/pdlobbyd.log" 2>&1 &
LOBBY=$!
if ! waitfor "$OUT/pdlobbyd.log" "pdlobbyd listening on" 20; then
	echo "FAIL: pdlobbyd did not start (see $OUT/pdlobbyd.log)"; kill $LOBBY 2>/dev/null; exit 2
fi
LPORT=$(sed -n 's/.*listening on 127.0.0.1:\([0-9]*\).*/\1/p' "$OUT/pdlobbyd.log" | head -1)
waitfor "$OUT/pdlobbyd.log" "notices on" 5
APORT=$(sed -n 's/.*notices on 127.0.0.1:\([0-9]*\).*/\1/p' "$OUT/pdlobbyd.log" | head -1)
echo "     pdlobbyd on 127.0.0.1:$LPORT, notices on $APORT (pid $LOBBY)"

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
	printf '[Mod]\nGhostUser=%s\nGhostPin=1234\n%b[Net]\nLobbyServer=http://127.0.0.1:%s\nPort=%s\n' "$user" "${MODINI:-}" "$LPORT" "$port" > "$save/pd.ini"
	cd "$BUILD" || exit 2
	exec timeout -k 5 "$t" stdbuf -oL -eL "$BIN" --savedir "$save" --skip-intro --no-sound --moddir "$MODDIR" "$@" > "$OUT/$label.log" 2>&1
}

if [ "${ONLY:-}" != seat ]; then
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
# the room's own rules (owner, F3 20261010-053136): the host plays with
# non-stock settings offline; its room starts at stock, one rule is changed
# for the room, the match plays it, and after the host leaves pd.ini and its
# settings are its own; a Create Room opened again starts at stock
MODINI="$RULESINI" game rules-host hostguy3 $((GPORT + 2)) 300 --net-lobby-script host --net-lobby-room "Rules Test" \
	--net-lobby-rules "Mod.JumpHeight=2" --net-lobby-end-frame 300 --net-lobby-host-leaves --net-test-stage 0x32 --net-test-sims 2 --rng-seed 9 &
P5=$!
game rules-join joiner3 $((GPORT + 2)) 300 --net-lobby-script join --net-lobby-room "Rules Test" --net-lobby-leave-frame 0 &
P6=$!
# the lobby's notice while the leave pair's match runs
NOTICE="Restarting Server for Update in 1 minute, apologies for the inconvenience!"
(
	waitfor "$OUT/leave-join.log" "net: match 1: GO" 200 || exit 0
	python3 - "$APORT" "$NOTICE" > "$OUT/notice.log" 2>&1 <<'PY'
import json, sys, urllib.request
req = urllib.request.Request("http://127.0.0.1:%s/notice" % sys.argv[1], data=json.dumps({"text": sys.argv[2]}).encode(),
                             headers={"Content-Type": "application/json"})
print(urllib.request.urlopen(req, timeout=5).read().decode())
PY
) &
PN=$!
wait $P1; r1=$?; wait $P2; r2=$?; wait $P3; r3=$?; wait $P4; r4=$?; wait $P5; r5=$?; wait $P6; r6=$?
wait $PN
fi
# the seat case (F3 20261012-020922): a room of three fills and launches;
# a newcomer is turned away while it is full; a player quits mid-match and the
# newcomer, joining again, is taken into the running match in that seat
if [ "${SEAT:-1}" = 1 ]; then
	SP=$((GPORT + 3))
	game seat-host seathost "$SP" 300 --net-lobby-script host --net-lobby-room "Seat Test" --net-lobby-size 3 --net-lobby-wait 2 \
		--net-lobby-end-frame 2400 --net-test-stage 0x32 --net-test-sims 0 --rng-seed 10 &
	S1=$!
	game seat-alpha seatalpha $((SP + 1)) 300 --net-lobby-script join --net-lobby-room "Seat Test" --net-lobby-leave-frame 0 --net-lobby-quit-frame 900 &
	S2=$!
	sleep 3
	game seat-bravo seatbravo $((SP + 2)) 300 --net-lobby-script join --net-lobby-room "Seat Test" --net-lobby-leave-frame 0 &
	S3=$!
	# the newcomer while all three seats are taken: the lobby says full
	waitfor "$OUT/seat-alpha.log" "net: match 1: GO" 200
	game seat-full seatcharlie $((SP + 3)) 60 --net-lobby-script join --net-lobby-room "Seat Test" &
	S4=$!
	wait $S2; rs2=$?
	wait $S4; rs4=$?
	# the same account again once alpha has quit
	game seat-charlie seatcharlie $((SP + 3)) 200 --net-lobby-script join --net-lobby-room "Seat Test" --net-lobby-leave-frame 0 &
	S5=$!
	wait $S1; rs1=$?; wait $S3; rs3=$?; wait $S5; rs5=$?
	echo "     seat: exits host $rs1 alpha $rs2 bravo $rs3 full $rs4 charlie $rs5"
fi
kill $LOBBY 2>/dev/null; wait $LOBBY 2>/dev/null
[ "${ONLY:-}" != seat ] && echo "     exits: leave host $r1 join $r2, rematch host $r3 join $r4"

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
	grep -q "join: .*READY" "$J" && grep -q "a member is ready; LAUNCH" "$H" && pass "$label: join READY, host LAUNCH" || fail "$label: ready/launch missing"
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

if [ "${ONLY:-}" != seat ]; then
meet leave joiner
H=$OUT/leave-host.log; J=$OUT/leave-join.log
grep -q "join: frame $FRAME; End Game" "$J" && pass "leave: join played to frame $FRAME and left the match" || fail "leave: join did not reach frame $FRAME in the match"
grep -q "the client left the match; End Game" "$H" && pass "leave: host ended its match after the client left" || fail "leave: host did not end its match"
grep -q "lobby script: host back in room $ROOMID" "$H" && pass "leave: $(line "$H" "host back in room")" || fail "leave: host not back in the room"
grep -q "lobby script: client back in room $ROOMID" "$J" && pass "leave: $(line "$J" "client back in room")" || fail "leave: client not back in the room"
grep -q "lobby: back in room $ROOMID's lobby" "$H" && pass "leave: host: Game Lobby menu up again" || fail "leave: host: no Game Lobby menu after the match"
grep -q "lobby: back in room $ROOMID's lobby" "$J" && pass "leave: join: Game Lobby menu up again" || fail "leave: join: no Game Lobby menu after the match"
grep -q "host heard joiner: \"back in the room\"" "$H" && pass "leave: host heard the client's chat in the room" || fail "leave: the chat line never reached the host"
grep -q "notice to [0-9]* room(s): $NOTICE" "$L" && pass "leave: pdlobbyd: $(line "$L" "notice to")" || fail "leave: the lobby said no notice ($(cat "$OUT/notice.log" 2>/dev/null))"
for l in host join; do
	has_n=$(grep -c "net: server notice: $NOTICE" "$OUT/leave-$l.log")
	[ "$has_n" = 1 ] && pass "leave: $l: the notice in the match's chat, once" || fail "leave: $l: the notice seen $has_n times"
done
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
meet rules joiner3
H=$OUT/rules-host.log; J=$OUT/rules-join.log; I=$OUT/save-rules-host/pd.ini
for kv in "Mod.JumpHeight 3 -> 0" "Mod.StartArmed 2 -> 0" "Mod.Akimbo 1 -> 0" 'Mod.SimBrain "modern" -> "stock"'; do
	[ "$(grep -c "net: rules: $kv (the new room's)" "$H")" = 2 ] && pass "rules: both rooms started at stock: $kv" \
		|| fail "rules: '$kv (the new room's)' seen $(grep -c "net: rules: $kv (the new room's)" "$H") times, not 2"
done
grep -q "net: rules: Mod.JumpHeight 0 -> 2 (the host's, for the match)" "$J" && pass "rules: the match played the room's Jump (2)" \
	|| fail "rules: the client did not take the room's Mod.JumpHeight 2: $(grep -m1 'net: rules: Mod.JumpHeight' "$J")"
grep -q "net: rules: Mod.StartArmed\|net: rules: Mod.Akimbo" "$J" && fail "rules: the host's own Start Armed or Akimbo reached the match" \
	|| pass "rules: the host's own Start Armed and Akimbo stayed out of the match"
[ "$(grep -c "hosting over; this machine's own setup and settings are back" "$H")" = 2 ] && pass "rules: own setup back after the room and after Create Room's Back" \
	|| fail "rules: own setup back $(grep -c "hosting over; this machine's own setup" "$H") times, not 2"
if grep -qx "JumpHeight=3" "$I" && grep -qx "StartArmed=2" "$I" && grep -qx "Akimbo=1" "$I" && grep -qx 'SimBrain="\?modern"\?' "$I"; then
	pass "rules: pd.ini still has the host's own values"
else
	fail "rules: pd.ini lost the host's own values: $(grep -E '^(JumpHeight|StartArmed|Akimbo|SimBrain)=' "$I" | tr '\n' ' ')"
fi
# byte for byte: pd.ini before any room, written in a room (Create Room's
# own rules on), and after the rooms were left
S=$OUT/save-rules-host
if [ -s "$S/pd-before.ini" ] && cmp -s "$S/pd-before.ini" "$S/pd-inroom.ini" && cmp -s "$S/pd-before.ini" "$I"; then
	pass "rules: pd.ini byte-identical before the room, written in a room, and after ($(wc -c < "$I") bytes)"
else
	fail "rules: pd.ini differs: $(diff "$S/pd-before.ini" "$I" 2>&1 | head -4 | tr '\n' ' ') / in room: $(diff "$S/pd-before.ini" "$S/pd-inroom.ini" 2>&1 | head -4 | tr '\n' ' ')"
fi
b=$(grep -o "host: setup before the room: .*" "$H" | cut -d: -f3-); a=$(grep -o "host: setup after the rooms: .*" "$H" | cut -d: -f3-)
[ -n "$b" ] && [ "$b" = "$a" ] && pass "rules: the Combat Simulator setup is the host's own again:$a" || fail "rules: setup before '$b', after '$a'"
[ "$r5" = 0 ] || fail "rules: host exit $r5"
fi

if [ "${SEAT:-1}" = 1 ]; then
	H=$OUT/seat-host.log; A=$OUT/seat-alpha.log; F=$OUT/seat-full.log; C=$OUT/seat-charlie.log
	room=$(sed -n 's/.*lobby: made room \([0-9a-f]*\).*/\1/p' "$H" | head -1)
	[ -n "$room" ] && pass "seat: host made room $room (3 players)" || fail "seat: host made no room"
	grep -q "match 1: every machine has loaded; GO" "$H" && pass "seat: $(line "$H" "net: match 1 starting on" | sed 's/, seeds.*//')" \
		|| fail "seat: the first match never passed GO"
	aslot=$(sed -n 's/.*net: slot \([0-9]\): "seatalpha" joined.*/\1/p' "$H" | head -1)
	grep -q 'net: slot [0-9]: "seatbravo" joined' "$H" && [ -n "$aslot" ] && pass "seat: alpha in slot $aslot, bravo in too" \
		|| fail "seat: alpha or bravo never seated"
	grep -q "lobby: join failed (409)" "$F" && [ "$rs4" = 4 ] && pass "seat: a newcomer while full: $(line "$F" "lobby: join failed")" \
		|| fail "seat: the newcomer was not turned away while full (exit $rs4): $(line "$F" "lobby: join")"
	grep -q "join: frame 900, level time [0-9]*; quitting the game" "$A" && grep -q "lobby: left room $room on the way out" "$A" \
		&& pass "seat: alpha quit mid-match, the lobby told" || fail "seat: alpha did not quit mid-match"
	grep -q "lobby: joined room $room" "$C" && pass "seat: charlie joined the launched room" || fail "seat: charlie could not join: $(line "$C" "lobby: join")"
	grep -q "join: in the room's match in progress" "$C" && pass "seat: charlie launched into the running match" \
		|| fail "seat: charlie was never launched into the running match"
	grep -q "(an open seat of the match in progress)" "$C" && pass "seat: charlie: $(line "$C" "net: accepted by")" \
		|| fail "seat: charlie not accepted into an open seat: $(line "$C" "net: accepted")"
	grep -q "net: slot $aslot (\"seatcharlie\") is in the match in progress" "$H" \
		&& pass "seat: host: $(line "$H" "net: slot $aslot (\"seatcharlie\")")" || fail "seat: charlie not in alpha's seat $aslot: $(line "$H" "(\"seatcharlie\") is in")"
	grep -q "net: match 1: GO, in progress" "$C" && pass "seat: $(line "$C" "net: match 1: GO, in progress" | cut -c1-120)" || fail "seat: charlie had no GO in progress"
	grep -q "join: the host ended the match" "$C" && grep -q "client back in room $room" "$C" && pass "seat: charlie played to the host's end and is back in the room" \
		|| fail "seat: charlie not back in the room after the match"
	[ "$rs1" = 0 ] && [ "$rs2" = 0 ] && [ "$rs3" = 0 ] && [ "$rs5" = 0 ] || fail "seat: exit codes host $rs1 alpha $rs2 bravo $rs3 charlie $rs5"
fi
exit $status
