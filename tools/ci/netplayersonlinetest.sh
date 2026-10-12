#!/bin/bash
# netplayersonlinetest.sh — the Briefing Room's Players Online page
# (F3 20261012-115245; pdlobbyd API 7, GET /players: README "Players online")
#
# A local pdlobbyd (ephemeral ports, --auth open) with accounts put where a
# player can be, over its HTTP API and kept there while the test runs:
#
#   lurker     signed in, asking for the main menu's chat every 2 s: the Briefing Room
#   mimic      hosting "Dark Sim Friday Rematch League", open: its Game Lobby
#   vaultguy   a member of that room
#   mod_fan    hosting "GoldenEye Facility Night Rematch", launched: its match
#   spectre    watching that match (a spectator, joined after the launch)
#   dedi       a dedicated host ("Always On"): a server, not a player, never listed
#
# and one game offscreen on the GPU, signed in as "tester", run with
# --net-test-players-online under a join script whose room never comes (it
# keeps the lobby ticking): the Briefing Room comes up over the boot's
# menus, its players key is pressed as a player presses it (the real key
# path, input.c's test hold), the page opens and asks the lobby, its rows
# are logged and the page is shot, and the key again closes it (the
# Briefing Room shot then, for its footer's hints). Then the
# same against the lobby as it was before API 7 (OLD's pdlobbyd): the page
# says this lobby has no players list.
#
#   netplayersonlinetest.sh [BIN]   BIN a file name in build/ (pd.x86_64) or a path
#
# Env: OUT (build/netplayersonline-out), SIZE (1280x720: the window's
# [Video] DefaultWidth x DefaultHeight), OLD (3b069f957: a revision whose
# pdlobbyd has no players list; empty skips that run), MODDIR (mod_allinone),
# GPORT (27196).
# Screenshots: build/net-shots/players-online-SIZE.png, players-online-briefing-SIZE.png,
# players-online-old-SIZE.png.
# Exit status: 0 all good, 1 a check failed, 2 a run failed to start.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=${BUILD:-$ROOT/build}
OUT=${OUT:-$BUILD/netplayersonline-out}; GPORT=${GPORT:-27196}
MODDIR=${MODDIR:-mod_allinone}
SIZE=${SIZE:-1280x720}; W=${SIZE%x*}; H=${SIZE#*x}
OLD=${OLD-3b069f957}
SHOTDIR=$BUILD/net-shots
BIN=${1:-pd.x86_64}
case $BIN in /*) ;; */*) BIN=$(realpath "$BIN") ;; *) BIN=$BUILD/$BIN ;; esac
mkdir -p "$OUT" "$SHOTDIR" "$BUILD/screenshots"; OUT=$(cd "$OUT" && pwd)
[ -n "$OUT" ] && [ "$OUT" != / ] && rm -f "$OUT"/*.log
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

# lobby SCRIPT LABEL: a pdlobbyd on ports the kernel picks; LPORT, LOBBY
lobby() {
	python3 -u "$1" --host 127.0.0.1 --port 0 --udp-host 127.0.0.1 --udp-port 0 --auth open --relay-ports 0 \
		--admin-port 0 > "$OUT/pdlobbyd-$2.log" 2>&1 &
	LOBBY=$!
	if ! waitfor "$OUT/pdlobbyd-$2.log" "pdlobbyd listening on" 20; then
		echo "FAIL: pdlobbyd ($2) did not start (see $OUT/pdlobbyd-$2.log)"; kill $LOBBY 2>/dev/null; exit 2
	fi
	LPORT=$(sed -n 's/.*listening on 127.0.0.1:\([0-9]*\).*/\1/p' "$OUT/pdlobbyd-$2.log" | head -1)
	echo "     pdlobbyd ($2) on 127.0.0.1:$LPORT (pid $LOBBY)"
}

# the accounts, where they are, kept there until killed
fillers() {
	python3 - "$LPORT" > "$OUT/fillers-$1.log" 2>&1 <<'PY' &
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
def login(user):
    return call("/login", {"username": user, "pin": "1234"})["session"]
def room(session, name, **kw):
    body = dict(name=name, max_humans=4, stage="Skedar", scenario="Combat", proto=1, build="other", content="00" * 8)
    body.update(kw)
    r = call("/rooms", body, session)
    return r["room"], r["token"]
compat = {"proto": 1, "build": "other", "content": "00" * 8}
lurker = login("lurker")
open_id, open_host = room(login("mimic"), "Dark Sim Friday Rematch League")
member = call("/rooms/%s/join" % open_id, compat, login("vaultguy"))["token"]
match_id, match_host = room(login("mod_fan"), "GoldenEye Facility Night Rematch", stage="Facility", scenario="King of the Hill")
call("/rooms/%s/launch" % match_id, {}, member=match_host)
dedi_id, dedi_host = room(login("dedi"), "Always On", dedicated=True)
spectre = login("spectre")
watcher = None
print("fillers up", flush=True)
while True:
    try:
        call("/chat?since=0", token=lurker)
        for rid, tok in ((open_id, open_host), (match_id, match_host), (dedi_id, dedi_host)):
            call("/rooms/%s/heartbeat" % rid, {}, member=tok)
        call("/rooms/%s/state?since=0&wait=0" % open_id, member=member)
        if watcher is None:
            st = call("/rooms/%s/state?since=0&wait=0" % match_id, member=match_host)
            if st["room"]["state"] == "launched":
                watcher = call("/rooms/%s/join" % match_id, dict(compat, spectator=True), spectre)["token"]
                print("spectre watching", flush=True)
        else:
            call("/rooms/%s/state?since=0&wait=0" % match_id, member=watcher)
    except Exception as e:
        print("filler", e, flush=True)
    time.sleep(2)
PY
	FILL=$!
}

# game LABEL USER TIMEOUT ARGS...  (in the foreground)
game() {
	local label=$1 user=$2 t=$3; shift 3
	local save=$OUT/save-$label
	rm -rf "$save"; mkdir -p "$save"
	for f in eeprom.bin mpsetups.bin; do
		[ -f "$HOME/.local/share/perfectdark/$f" ] && cp "$HOME/.local/share/perfectdark/$f" "$save/"
	done
	printf '[Mod]\nGhostUser=%s\nGhostPin=1234\n[Net]\nLobbyServer=http://127.0.0.1:%s\nPort=%s\n[Video]\nDefaultFullscreen=0\nDefaultWidth=%s\nDefaultHeight=%s\n' \
		"$user" "$LPORT" "$GPORT" "$W" "$H" > "$save/pd.ini"
	(cd "$BUILD" && timeout -k 5 "$t" stdbuf -oL -eL "$BIN" --savedir "$save" --skip-intro --no-sound --moddir "$MODDIR" \
		--net-lobby-script join --net-lobby-room "(no such room)" --net-test-players-online "$@" > "$OUT/$label.log" 2>&1)
}

line() { grep -m1 -o -- "$2.*" "$1"; }
# shot LOG PATTERN DEST: the PNG the log's screenshot line names after PATTERN
shot() {
	local f
	f=$(sed -n "/$2/,\$p" "$1" | sed -n 's/.*screenshot: \(.*\.png\).*/\1/p' | head -1)
	[ -z "$f" ] && f=$(grep -o 'screenshot: .*\.png' "$1" | tail -1 | sed 's/screenshot: //')
	[ -n "$f" ] && [ -f "$f" ] && cp "$f" "$3" && { pass "screenshot $3"; return; }
	[ -n "$f" ] && [ -f "$BUILD/$f" ] && cp "$BUILD/$f" "$3" && { pass "screenshot $3"; return; }
	fail "no screenshot after '$2' in $1"
}

# --- the lobby of this tree (API 7)
lobby "$ROOT/tools/pdlobbyd/pdlobbyd.py" new
fillers new
waitfor "$OUT/fillers-new.log" "spectre watching" 20 || fail "the fillers did not take their places (see $OUT/fillers-new.log)"
# what the lobby says, as any signed-in player reads it: no address in it
python3 - "$LPORT" > "$OUT/players.json" <<'PY'
import json, sys, urllib.request
base = "http://127.0.0.1:%s" % sys.argv[1]
s = json.loads(urllib.request.urlopen(urllib.request.Request(base + "/login", data=b'{"username":"checker"}',
    headers={"Content-Type": "application/json"}), timeout=5).read())["session"]
req = urllib.request.Request(base + "/players", headers={"Authorization": "Bearer " + s})
print(urllib.request.urlopen(req, timeout=5).read().decode())
# signed out again: not one of the players the game is to list
urllib.request.urlopen(urllib.request.Request(base + "/logout", data=b"{}", headers={"Authorization": "Bearer " + s,
    "Content-Type": "application/json"}), timeout=5).read()
PY
grep -q '127\.0\.0\.1' "$OUT/players.json" && fail "GET /players names an address: $(cat "$OUT/players.json")" \
	|| pass "GET /players: $(python3 -c 'import json,sys; r=json.load(open(sys.argv[1])); print(r["count"], "online:", ", ".join(p["user"] + " " + p["where"] for p in r["players"]))' "$OUT/players.json")"

game new tester 120
r=$?
kill $FILL $LOBBY 2>/dev/null; wait $FILL $LOBBY 2>/dev/null
G=$OUT/new.log
echo "     exit: $r"
grep -q "renderD128: Permission denied" "$G" && fail "the run fell back to llvmpipe"
grep -q "lobby test: Briefing Room up" "$G" && pass "the Briefing Room came up" || fail "no Briefing Room"
grep -q "lobby: Players Online opened" "$G" && pass "the players key opened Players Online" || fail "the players key did not open the page"
for want in "lurker: Briefing Room" "mimic: In room: Dark Sim Friday Rematch League" "vaultguy: In room: Dark Sim Friday Rematch League" \
		"mod_fan: In a match: GoldenEye Facility Night Rematch" "spectre: Watching: GoldenEye Facility Night Rematch" "tester: Briefing Room"; do
	grep -q "lobby test: online: $want" "$G" && pass "listed: $want" || fail "not listed: $want"
done
grep -q "lobby test: online: dedi:" "$G" && fail "the dedicated host is listed as a player" || pass "the dedicated host is not listed"
grep -q "lobby test: shot Players Online: 6 listed of 6, state 1" "$G" && pass "$(line "$G" "shot Players Online")" \
	|| fail "the page's shot: [$(line "$G" "shot Players Online")]"
shot "$G" "shot Players Online" "$SHOTDIR/players-online-$SIZE.png"
grep -q "lobby: Players Online closed by its button" "$G" && grep -q "after the button again: the Briefing Room" "$G" \
	&& pass "the key again closed it: back on the Briefing Room" || fail "the key again: [$(line "$G" "after the button again")]"
shot "$G" "shot the Briefing Room" "$SHOTDIR/players-online-briefing-$SIZE.png"

# --- the lobby as it was before API 7: no list, and the page says so
if [ -n "$OLD" ]; then
	git -C "$ROOT" show "$OLD:tools/pdlobbyd/pdlobbyd.py" > "$OUT/pdlobbyd-old.py" || { echo "FAIL: no pdlobbyd at $OLD"; exit 2; }
	lobby "$OUT/pdlobbyd-old.py" old
	game old tester 120
	r=$?
	kill $LOBBY 2>/dev/null; wait $LOBBY 2>/dev/null
	G=$OUT/old.log
	echo "     exit (old lobby): $r"
	grep -q "lobby: this lobby has no players list (404" "$G" && grep -q "lobby test: shot Players Online: 0 listed of 0, state -1: This lobby server has no players list yet" "$G" \
		&& pass "old lobby: $(line "$G" "shot Players Online" | sed 's/.*state -1: //')" \
		|| fail "old lobby: [$(line "$G" "shot Players Online")]"
	shot "$G" "shot Players Online" "$SHOTDIR/players-online-old-$SIZE.png"
fi
exit $status
