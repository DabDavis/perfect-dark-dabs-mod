#!/bin/bash
# netlobbywinetest.sh — the Windows build's lobby client (WinHTTP) through a
# whole room: list, join, ready, launch, a match, back in the room, chat
#
# Phase 7d (PLANS/NETPLAY.md "Lobby"): the lobby's HTTP runs over WinHTTP in
# the Windows build (port/src/ghostnet.c), curl in the Linux one; this runs
# the WinHTTP side for real. A local pdlobbyd (ephemeral ports, --auth open),
# the Linux build as the room's host offscreen on the GPU, and the mingw
# build (build-win/pd.x86_64.exe) under wine as the member, on the GPU's
# display (:0, woken first; wine has no offscreen video), both driven by
# --net-lobby-script:
#
#   host (Linux): create the room, wait for READY, LAUNCH, end the match at
#         frame 400 (MATCH_END), reopen the room, wait for the member's chat
#   join (wine):  sign in, list the rooms (GET /rooms), find it, join
#         (POST /rooms/<id>/join), long-poll the room (GET .../state, which
#         parks up to 20 s), ladder via the rendezvous, READY, connect at
#         launch with the lobby's ticket, play to the host's end, back in
#         the room still connected, say "back in the room" (POST .../chat)
#
#   netlobbywinetest.sh [BIN]   BIN the Linux host (build/pd.x86_64)
#
# Env: OUT (build/netlobbywine-out), GPORT (27196), MODDIR (mod_allinone),
# WINEEXE (build-win/pd.x86_64.exe), WINEDISPLAY_X (:0).
# Exit status: 0 all good, 1 a check failed, 2 a run failed to start.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=${BUILD:-$ROOT/build}
OUT=${OUT:-$BUILD/netlobbywine-out}; GPORT=${GPORT:-27196}
MODDIR=${MODDIR:-mod_allinone}
WINEEXE=${WINEEXE:-$ROOT/build-win/pd.x86_64.exe}
ROOMNAME="Wine Lobby Test"
BIN=${1:-pd.x86_64}
case $BIN in /*) ;; */*) BIN=$(realpath "$BIN") ;; *) BIN=$BUILD/$BIN ;; esac
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
[ -n "$OUT" ] && [ "$OUT" != / ] && rm -f "$OUT"/*.log
[ -f "$WINEEXE" ] || { echo "FAIL: no $WINEEXE (cmake --build build-win)"; exit 2; }
command -v wine >/dev/null || { echo "FAIL: no wine"; exit 2; }
export SDL_GAMECONTROLLER_IGNORE_DEVICES=0x054c/0x0ce6,0x045e/0x028e \
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

seed() {
	local save=$1 user=$2
	rm -rf "$save"; mkdir -p "$save"
	for f in eeprom.bin mpsetups.bin; do
		[ -f "$HOME/.local/share/perfectdark/$f" ] && cp "$HOME/.local/share/perfectdark/$f" "$save/"
	done
	printf '[Mod]\nGhostUser=%s\nGhostPin=1234\n[Net]\nLobbyServer=http://127.0.0.1:%s\nPort=%s\n' "$user" "$LPORT" "$GPORT" > "$save/pd.ini"
}

# the Linux host
host() {
	seed "$OUT/save-host" hostguy
	cd "$BUILD" || exit 2
	SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-offscreen} exec timeout -k 5 240 stdbuf -oL -eL "$BIN" --savedir "$OUT/save-host" --skip-intro --no-sound \
		--moddir "$MODDIR" --net-lobby-script host --net-lobby-room "$ROOMNAME" --net-lobby-end-frame 400 \
		--net-test-stage 0x32 --net-test-sims 2 --rng-seed 12 > "$OUT/host.log" 2>&1
}

# the member: the Windows build under wine, from a directory of its own with
# the DLLs it needs and the ROM and mods linked (CLAUDE-notes/windows-build.md)
winejoin() {
	local save=$OUT/save-join dir=$OUT/win dll src l
	seed "$save" winejoiner
	mkdir -p "$dir"
	cp -f "$WINEEXE" "$dir/pd.x86_64.exe"
	for dll in SDL2.dll libgcc_s_seh-1.dll; do
		[ -e "$dir/$dll" ] && continue
		for src in "$ROOT/build-win/$dll" "$HOME/.local/mingw64/bin/$dll" /usr/x86_64-w64-mingw32/bin/$dll \
				/usr/lib/gcc/x86_64-w64-mingw32/*-win32/$dll; do
			[ -e "$src" ] && { cp -f "$src" "$dir/$dll"; break; }
		done
	done
	for l in data "$MODDIR" mods added-content; do
		[ -e "$BUILD/$l" ] && [ ! -e "$dir/$l" ] && ln -s "$BUILD/$l" "$dir/$l"
	done
	cd "$dir" || exit 2
	export DISPLAY=${WINEDISPLAY_X:-:0} WINEDEBUG=-all
	unset SDL_VIDEODRIVER
	xset dpms force on 2>/dev/null
	exec timeout -k 5 240 wine ./pd.x86_64.exe --savedir "$(winepath -w "$save" 2>/dev/null || echo "$save")" --skip-intro --no-sound \
		--moddir "$MODDIR" --net-lobby-script join --net-lobby-room "$ROOMNAME" --net-lobby-leave-frame 0 > "$OUT/join.log" 2>&1
}

host &
P1=$!
winejoin &
P2=$!
wait $P1; r1=$?; wait $P2; r2=$?
kill $LOBBY 2>/dev/null; wait $LOBBY 2>/dev/null
echo "     exits: host $r1 join (wine) $r2"

H=$OUT/host.log; J=$OUT/join.log; L=$OUT/pdlobbyd.log
line() { grep -m1 -o -- "$2.*" "$1"; }
grep -q "renderD128: Permission denied" "$H" && fail "the host fell back to llvmpipe"
# the Windows build has one HTTP transport, WinHTTP (CMakeLists.txt): no curl to fall back on
if command -v x86_64-w64-mingw32-objdump >/dev/null; then
	x86_64-w64-mingw32-objdump -p "$WINEEXE" | grep -q "DLL Name: WINHTTP.dll" && pass "the exe's HTTP is WinHTTP (imports WINHTTP.dll)" \
		|| fail "the exe does not import WINHTTP.dll"
fi
room=$(sed -n 's/.*lobby: made room \([0-9a-f]*\).*/\1/p' "$H" | head -1)
[ -n "$room" ] && pass "host made room $room" || fail "host made no room"
grep -q "lobby: signed in as winejoiner" "$J" && pass "join (WinHTTP): signed in (POST /login)" || fail "join: never signed in"
grep -q "lobby script: found room $room" "$J" && pass "join (WinHTTP): listed it: $(line "$J" "found room" | tr -d '\r')" || fail "join: did not list the room"
grep -q "lobby: joined room $room" "$J" && pass "join (WinHTTP): joined room $room" || fail "join: did not join"
grep -q "join: .*READY" "$J" && pass "join (WinHTTP): $(grep -m1 -o "join: [a-z ]*path.*READY" "$J")" || fail "join: never READY"
grep -q "a member is ready; LAUNCH" "$H" && pass "host saw READY (the poll carried it) and LAUNCHed" || fail "host never saw READY"
grep -q "lobby ticket for \"winejoiner\" in room $room verified" "$H" && pass "host: $(line "$H" "lobby ticket for")" || fail "host never verified the wine member's ticket"
if grep -q "match 1: every machine has loaded; GO" "$H" && grep -q "net: match 1: GO" "$J"; then
	pass "both passed GO"
else
	fail "no GO on both"
fi
grep -q "join: the host ended the match" "$J" && pass "join: the host ended the match" || fail "join: no end of the match"
grep -q "lobby script: client back in room $room" "$J" && pass "join: $(line "$J" "client back in room" | tr -d '\r')" || fail "join: not back in the room"
grep -q "host heard winejoiner: \"back in the room\"" "$H" && pass "host heard the wine member's chat (POST .../chat over WinHTTP)" || fail "the chat never reached the host"
grep -q "Save Player prompt is up" "$H" "$J" && fail "a Save Player prompt came up"
[ "$r1" = 0 ] && [ "$r2" = 0 ] || fail "exit codes host $r1 join $r2"
exit $status
