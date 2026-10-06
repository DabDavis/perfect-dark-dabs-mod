#!/bin/bash
# netlobbyuitest.sh — the Online Game pages as a player sees them, and a
# room's host reached by its second advertised endpoint
#
# Phase 7d polish (PLANS/NETPLAY.md "Lobby"): a local pdlobbyd (ephemeral
# ports, --auth open), three filler rooms made over its HTTP API (a long
# name, a locked one, other content) kept alive by heartbeats, and one pair
# of games offscreen on the GPU driven by --net-lobby-script with
# --net-lobby-shots:
#
#   host: creates "Stock Game Tuesday Night Brawl" (30 characters) with an
#         unreachable address advertised ahead of its real ones
#         (--net-test-endpoint), puts the Game Lobby up and screenshots it
#         once the joiner's path and ping are on the roster, then LAUNCH
#   join: the Briefing Room up until the room's PING is measured (its echo
#         to the lobby + the lobby's probe of the host), screenshot; joins,
#         READY, the Game Lobby up, screenshot; at launch it skips the
#         punched path (--net-test-skip-ladder) so the advertised endpoints
#         are tried in turn: the dead one first, then the next
#   lister: a third machine that only lists, its rendezvous ECHO ignored
#         (--net-test-no-echo, as if UDP to the lobby were blocked): the
#         room's PING still a number, its own leg from the list's HTTP
#         round trip ("~n" in the column)
#   both: the match runs to the host's End Game at frame 300; neither end
#         screen may put up PD's Save Player prompt (spec-stage trap 13);
#         the host's HUD text the joiner is sent names it hostguy
#
#   netlobbyuitest.sh [BIN]   BIN a file name in build/ (pd.x86_64) or a path
#
# Env: OUT (build/netlobbyui-out), GPORT (27190), MODDIR (mod_allinone).
# Screenshots: build/net-shots/lobby-{briefing,briefing-http,room-host,room-join}.png.
# Exit status: 0 all good, 1 a check failed, 2 a run failed to start.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=${BUILD:-$ROOT/build}
OUT=${OUT:-$BUILD/netlobbyui-out}; GPORT=${GPORT:-27190}
MODDIR=${MODDIR:-mod_allinone}
SHOTDIR=$BUILD/net-shots
ROOMNAME="Stock Game Tuesday Night Brawl"
BIN=${1:-pd.x86_64}
case $BIN in /*) ;; */*) BIN=$(realpath "$BIN") ;; *) BIN=$BUILD/$BIN ;; esac
mkdir -p "$OUT" "$SHOTDIR" "$BUILD/screenshots"; OUT=$(cd "$OUT" && pwd)
[ -n "$OUT" ] && [ "$OUT" != / ] && rm -f "$OUT"/*.log
rm -f "$SHOTDIR"/lobby-*.png
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

python3 -u "$ROOT/tools/pdlobbyd/pdlobbyd.py" --host 127.0.0.1 --port 0 --udp-host 127.0.0.1 --udp-port 0 --auth open --relay-ports 0 \
	> "$OUT/pdlobbyd.log" 2>&1 &
LOBBY=$!
if ! waitfor "$OUT/pdlobbyd.log" "pdlobbyd listening on" 20; then
	echo "FAIL: pdlobbyd did not start (see $OUT/pdlobbyd.log)"; kill $LOBBY 2>/dev/null; exit 2
fi
LPORT=$(sed -n 's/.*listening on 127.0.0.1:\([0-9]*\).*/\1/p' "$OUT/pdlobbyd.log" | head -1)
echo "     pdlobbyd on 127.0.0.1:$LPORT (pid $LOBBY)"

# filler rooms, heartbeated until the lobby goes
python3 - "$LPORT" > "$OUT/fillers.log" 2>&1 <<'PY' &
import json, sys, time, urllib.request
base = "http://127.0.0.1:%s" % sys.argv[1]
def call(path, body=None, token=None, member=None):
    req = urllib.request.Request(base + path, data=None if body is None else json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"})
    if token:
        req.add_header("Authorization", "Bearer " + token)
    if member:
        req.add_header("X-PD-Member", member)
    return json.loads(urllib.request.urlopen(req, timeout=5).read() or b"{}")
rooms = [("mimic", "Dark Sim Friday Rematch League", "Skedar", "Combat", ""),
         ("vaultguy", "Pass Only", "Complex", "Hold the Briefcase", "secret"),
         ("mod_fan", "GoldenEye Facility Night", "Facility", "King of the Hill", "")]
held = []
for user, name, stage, scen, pw in rooms:
    s = call("/login", {"username": user, "pin": "1234"})["session"]
    r = call("/rooms", {"name": name, "password": pw, "max_humans": 4, "stage": stage, "scenario": scen,
                        "proto": 1, "build": "other", "content": "00" * 8}, s)
    held.append((r["token"], r["room"]))
    print("filler", r["room"], name, flush=True)
while True:
    for tok, rid in held:
        try:
            call("/rooms/%s/heartbeat" % rid, {}, member=tok)
        except Exception as e:
            print("heartbeat", rid, e, flush=True)
    time.sleep(4)
PY
FILL=$!

# game LABEL USER GAMEPORT TIMEOUT ARGS... &
game() {
	local label=$1 user=$2 port=$3 t=$4; shift 4
	local save=$OUT/save-$label
	rm -rf "$save"; mkdir -p "$save"
	for f in eeprom.bin mpsetups.bin; do
		[ -f "$HOME/.local/share/perfectdark/$f" ] && cp "$HOME/.local/share/perfectdark/$f" "$save/"
	done
	printf '[Mod]\nGhostUser=%s\nGhostPin=1234\n[Net]\nLobbyServer=http://127.0.0.1:%s\nPort=%s\n' "$user" "$LPORT" "$port" > "$save/pd.ini"
	cd "$BUILD" || exit 2
	exec timeout -k 5 "$t" stdbuf -oL -eL "$BIN" --savedir "$save" --skip-intro --no-sound --moddir "$MODDIR" "$@" > "$OUT/$label.log" 2>&1
}

game host hostguy "$GPORT" 240 --net-lobby-script host --net-lobby-room "$ROOMNAME" --net-lobby-shots \
	--net-lobby-end-frame 300 --net-test-endpoint 10.255.255.1:$GPORT --net-test-stage 0x32 --net-test-sims 2 --rng-seed 9 &
P1=$!
game join joiner "$GPORT" 240 --net-lobby-script join --net-lobby-room "$ROOMNAME" --net-lobby-shots \
	--net-lobby-leave-frame 0 --net-test-skip-ladder &
P2=$!
# a third machine whose UDP to the lobby is as if blocked (--net-test-no-echo):
# its PING is this machine's leg from the room list's HTTP round trip
game lister lister "$GPORT" 120 --net-lobby-script join --net-lobby-room "$ROOMNAME" --net-lobby-shots \
	--net-test-no-echo &
P3=$!
wait $P1; r1=$?; wait $P2; r2=$?; wait $P3; r3=$?
kill $FILL $LOBBY 2>/dev/null; wait $FILL $LOBBY 2>/dev/null
echo "     exits: host $r1 join $r2 lister $r3"

H=$OUT/host.log; J=$OUT/join.log
line() { grep -m1 -o -- "$2.*" "$1"; }
# shot LOG PATTERN DEST: the PNG the log's screenshot line names after PATTERN
shot() {
	local f
	f=$(sed -n "/$2/,\$p" "$1" | sed -n 's/.*screenshot: \(.*\.png\).*/\1/p' | head -1)
	[ -n "$f" ] && [ -f "$f" ] && cp "$f" "$3" && { pass "screenshot $3"; return; }
	[ -n "$f" ] && [ -f "$BUILD/$f" ] && cp "$BUILD/$f" "$3" && { pass "screenshot $3"; return; }
	fail "no screenshot after '$2' in $1"
}

grep -q "renderD128: Permission denied" "$H" "$J" && fail "a run fell back to llvmpipe"
grep -q "lobby script: shot the Briefing Room" "$J" && pass "join: $(line "$J" "shot the Briefing Room")" || fail "join: no Briefing Room shot"
grep -q "shot the Briefing Room: [0-9]* rooms, \"$ROOMNAME\" ping [0-9]" "$J" && pass "join: the room's PING was measured" || fail "join: the room's PING was not measured"
shot "$J" "shot the Briefing Room" "$SHOTDIR/lobby-briefing.png"
L=$OUT/lister.log
grep -q "shot the Briefing Room: [0-9]* rooms, \"$ROOMNAME\" ping [0-9]* by HTTP" "$L" \
	&& pass "lister (UDP to the lobby mute): $(line "$L" "shot the Briefing Room" | sed 's/.*rooms, //')" \
	|| fail "lister: no PING by HTTP with the echo mute"
shot "$L" "shot the Briefing Room" "$SHOTDIR/lobby-briefing-http.png"
grep -q "lobby script: shot the host's Game Lobby: joiner path [a-z]" "$H" && pass "host: $(line "$H" "shot the host's Game Lobby")" || fail "host: no Game Lobby shot with the joiner's path"
shot "$H" "shot the host's Game Lobby" "$SHOTDIR/lobby-room-host.png"
grep -q "lobby script: shot the joiner's Game Lobby" "$J" && pass "join: $(line "$J" "shot the joiner's Game Lobby")" || fail "join: no Game Lobby shot"
shot "$J" "shot the joiner's Game Lobby" "$SHOTDIR/lobby-room-join.png"
if grep -q "connecting to 10.255.255.1 port $GPORT (endpoint 1 of" "$J" && grep -q "trying the next (2 of" "$J" \
		&& grep -q "net: connected to the host; sending CONNECT" "$J"; then
	pass "join: the dead $(line "$J" "10.255.255.1 port $GPORT (endpoint 1 of" | sed 's/ with the.*//'), then $(line "$J" "port $GPORT (endpoint 2 of" | sed 's/ with the.*//')"
else
	fail "join: the advertised endpoints were not tried in turn"
fi
grep -q "match 1: every machine has loaded; GO" "$H" && grep -q "net: match 1: GO" "$J" && pass "both passed GO" || fail "no GO on both"
grep -q "lobby host: slot 0 plays as \"hostguy\"" "$H" && grep -q "slot 0's profile name is back" "$H" \
	&& pass "host: $(line "$H" "slot 0 plays as" | sed 's/ (its.*//') in its own match (HUD text it sends), profile name back after" \
	|| fail "host: slot 0 did not play as hostguy on the host, or its profile name did not come back"
grep -q "\"hostguy\" in slot 0" "$J" && pass "join: $(line "$J" "hostguy\" in slot 0")" || fail "join: the host's slot 0 is not named hostguy"
grep -q "join: the host ended the match" "$J" && pass "join: the host ended the match; end screen closed" || fail "join: no end of the match"
if grep -q "Save Player prompt is up" "$H" "$J"; then
	fail "a Save Player prompt came up: $(grep -h -m1 -o "a Save Player prompt.*" "$H" "$J")"
else
	pass "no Save Player prompt on either end screen"
fi
exit $status
