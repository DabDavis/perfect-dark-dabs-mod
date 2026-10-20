#!/bin/bash
# netlantest.sh — LAN and Direct Connect rooms, served by their hosts' games
#
# Protocol 33 (CLAUDE-notes/netplay.md "LAN and Direct Connect"): no pdlobbyd,
# no account. Games offscreen on the GPU, each driven by --net-lobby-script in
# LAN or Direct Connect (--net-lobby-mode), through the same calls the online
# pages make:
#
#   lan:       the joiner finds the host's room (broadcast and loopback),
#              joins, READY; the host launches; the match passes GO with the
#              stage hash equal; the host ends it at frame 600; both are back
#              in the room; the joiner's chat line reaches the host; the host
#              closes the room and the joiner is told why
#   broadcast: the same found by the LAN broadcast alone (no loopback query)
#   direct:    Direct Connect: the joiner asks 127.0.0.1:PORT by address
#   password:  a locked room: a wrong password refused with why, the right
#              one in
#   endgame:   the joiner's End Game at frame 400 keeps it in the room (in for
#              the next match, NETCONN_LOBBY); Back Into the Match takes it
#              into the match in progress; the host ends it at frame 2400
#   spectate:  a room of three: a joiner with the host's name is numbered
#              ("Hosty 2") and watches (moved to a spectator view, its
#              connection kept); the other plays
#
#   netlantest.sh [BIN]   BIN a file name in build/ (pd.x86_64) or a path
#
# Env: OUT (build/netlan-out), GPORT (27360: the hosts' game ports), ONLY (the
# cases, all by default), MODDIR (mod_allinone).
# Exit status: 0 all good, 1 a check failed.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=${BUILD:-$ROOT/build}
OUT=${OUT:-$BUILD/netlan-out}; GPORT=${GPORT:-27360}
MODDIR=${MODDIR:-mod_allinone}
BIN=${1:-pd.x86_64}
case $BIN in /*) ;; */*) BIN=$(realpath "$BIN") ;; *) BIN=$BUILD/$BIN ;; esac
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
export SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-offscreen} SDL_GAMECONTROLLER_IGNORE_DEVICES=0x054c/0x0ce6,0x045e/0x028e \
	SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT=0x0000/0x0000 SDL_JOYSTICK_HIDAPI=0

status=0
want() { [ -z "${ONLY:-}" ] || [[ " $ONLY " == *" $1 "* ]]; }
fail() { echo "FAIL $*"; status=1; }
pass() { echo "ok   $*"; }
has() { grep -q -- "$2" "$1" 2>/dev/null; }
check() { local what=$1 log=$2 pattern=$3; if has "$log" "$pattern"; then pass "$what"; else fail "$what (no \"$pattern\" in $(basename "$log"))"; fi; }

# game CASE LABEL PORT TIMEOUT ARGS... &  (its pd.ini's Net.Port: the finder asks it and the three after it)
game() {
	local case=$1 label=$2 port=$3 t=$4; shift 4
	local save=$OUT/$case-save-$label
	rm -rf "$save"; mkdir -p "$save"
	for f in eeprom.bin mpsetups.bin; do
		[ -f "$HOME/.local/share/perfectdark/$f" ] && cp "$HOME/.local/share/perfectdark/$f" "$save/"
	done
	printf '[Net]\nPort=%s\n' "$port" > "$save/pd.ini"
	cd "$BUILD" || exit 2
	exec timeout -k 5 "$t" stdbuf -oL -eL "$BIN" --savedir "$save" --skip-intro --no-sound --moddir "$MODDIR" "$@" > "$OUT/$case-$label.log" 2>&1
}

# pair CASE MODE HOSTARGS JOINARGS: a host and a joiner, the joiner 4 s later
pair() {
	local case=$1 mode=$2 hostargs=$3 joinargs=$4
	rm -f "$OUT/$case-"*.log
	# shellcheck disable=SC2086
	game "$case" host "$GPORT" 160 --net-lobby-script host --net-lobby-mode "$mode" --net-lobby-name Hosty --net-lobby-room "LAN Test" $hostargs &
	local h=$!
	sleep 4
	# shellcheck disable=SC2086
	game "$case" join "$GPORT" 160 --net-lobby-script join --net-lobby-mode "$mode" --net-lobby-name Joiny --net-lobby-room "LAN Test" $joinargs &
	local j=$!
	wait $h; local rh=$?
	wait $j; local rj=$?
	echo "     $case: exits host $rh join $rj"
}

# the usual round: in, a match, back, the chat line
round() {
	local case=$1
	local H=$OUT/$case-host.log J=$OUT/$case-join.log
	check "$case: host made the room" "$H" "lan: room .* made"
	check "$case: join found it" "$J" "found room .* \"LAN Test\" by Hosty"
	check "$case: join in the room" "$J" "lan: in room .* as Joiny"
	check "$case: host launched" "$H" "lan: room .* launched"
	check "$case: both passed GO" "$J" "net: match 1: GO"
	if [ "$(grep -h 'net: stage hash' "$H" | head -8 | md5sum)" = "$(grep -h 'net: stage hash' "$J" | head -8 | md5sum)" ]; then
		pass "$case: stage hash equal"
	else
		fail "$case: stage hash differs"
	fi
	check "$case: host back in the room" "$H" "host back in room"
	check "$case: join back in the room, still connected" "$J" "client back in room .* session still connected"
	check "$case: the joiner's chat reached the host" "$H" "host heard Joiny: \"back in the room\""
}

if want lan; then
	pair lan lan "--net-lobby-end-frame 600" "--net-lobby-leave-frame 0"
	round lan
	check "lan: the host closed the room on its way out" "$OUT/lan-host.log" "lan: room .* closed by its host"
fi

if want broadcast; then
	pair broadcast lan "--net-lobby-end-frame 600 --net-test-lan-broadcast" "--net-lobby-leave-frame 0 --net-test-lan-broadcast"
	round broadcast
	if grep -q "lan: joining room .* at 127\.0\.0\.1:" "$OUT/broadcast-join.log"; then
		fail "broadcast: joined by loopback, not the broadcast"
	else
		pass "broadcast: found by the broadcast ($(grep -o 'joining room .* at [0-9.:]*' "$OUT/broadcast-join.log" | head -1))"
	fi
fi

if want direct; then
	pair direct direct "--net-lobby-end-frame 600" "--net-lobby-leave-frame 0 --net-lobby-address 127.0.0.1:$GPORT"
	check "direct: join asked the address" "$OUT/direct-join.log" "Direct Connect: asking 127.0.0.1 port $GPORT"
	round direct
fi

if want password; then
	rm -f "$OUT/password-"*.log
	game password host "$GPORT" 60 --net-lobby-script host --net-lobby-mode lan --net-lobby-name Hosty --net-lobby-room "LAN Test" \
		--net-lobby-password secret &
	h=$!
	sleep 4
	(game password bad "$GPORT" 40 --net-lobby-script join --net-lobby-mode lan --net-lobby-name Joiny --net-lobby-room "LAN Test" \
		--net-lobby-join-password wrong)
	check "password: a wrong one refused, with why" "$OUT/password-bad.log" "could not join: Wrong room password"
	kill $h 2>/dev/null; wait $h 2>/dev/null
	pair password2 lan "--net-lobby-end-frame 600 --net-lobby-password secret" "--net-lobby-leave-frame 0 --net-lobby-join-password secret"
	round password2
fi

if want endgame; then
	pair endgame lan "--net-lobby-end-frame 2400" "--net-lobby-leave-frame 400 --net-lobby-rejoin"
	check "endgame: End Game kept the joiner in the room" "$OUT/endgame-join.log" "in the room, out of its match"
	check "endgame: the host took it in for the next match" "$OUT/endgame-host.log" "out of the match in progress (its End Game): in for the next"
	check "endgame: Back Into the Match offered and taken" "$OUT/endgame-join.log" "Back Into the Match"
	check "endgame: back into the match in progress" "$OUT/endgame-host.log" "an open seat of the match in progress"
	check "endgame: both back in the room after it" "$OUT/endgame-join.log" "client back in room"
fi

if want spectate; then
	rm -f "$OUT/spectate-"*.log
	game spectate host "$GPORT" 160 --net-lobby-script host --net-lobby-mode lan --net-lobby-name Hosty --net-lobby-room "LAN Test" \
		--net-lobby-size 3 --net-lobby-end-frame 700 &
	h=$!
	sleep 4
	game spectate spec "$GPORT" 160 --net-lobby-script join --net-lobby-mode lan --net-lobby-name Hosty --net-lobby-room "LAN Test" \
		--net-lobby-leave-frame 0 --net-lobby-spectate &
	s=$!
	sleep 6
	game spectate join "$GPORT" 160 --net-lobby-script join --net-lobby-mode lan --net-lobby-name Joiny --net-lobby-room "LAN Test" \
		--net-lobby-leave-frame 0 &
	j=$!
	wait $h; wait $s; wait $j
	check "spectate: a name taken is numbered" "$OUT/spectate-spec.log" "lan: in room .* as Hosty 2"
	check "spectate: moved to a spectator view, still connected" "$OUT/spectate-host.log" "\"Hosty 2\" moves from slot .* to spectator view"
	check "spectate: it watched the match" "$OUT/spectate-spec.log" "GO, spectating"
	check "spectate: the player played it" "$OUT/spectate-join.log" "net: match 1: GO"
	check "spectate: both back in the room" "$OUT/spectate-spec.log" "client back in room"
fi

echo "== summary: $([ $status = 0 ] && echo all good || echo FAILED)"
exit $status
