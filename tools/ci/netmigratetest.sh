#!/bin/bash
# netmigratetest.sh — does a lobby room outlive its host? (host migration)
#
# A local pdlobbyd (ephemeral ports, --auth open) and, per case, a host and
# two joiners offscreen on the GPU, each driven by --net-lobby-script through
# the calls the Online Game menus make (netlobby.c). The host makes a room of
# three, LAUNCHes once both joiners are READY, and goes away; the joiners run
# with --net-lobby-migrate and play the room on under whichever of them the
# lobby makes its host (README "Host migration", netmigrate.c):
#
#   quit   a Combat Simulator match on 0x32 with two sims; the host gives the
#          player in seat 1 three kills (--net-test-givekills) and quits at
#          frame 900 the way a player quits (the lobby told, LEAVE sent). The
#          lobby hands the room on at once; the new host listens, adopts the
#          match's rules and relaunches; the other joiner connects to it with a
#          new ticket and sends its player's state (RESUME); the match starts
#          again on 0x32 and carries on: the level clock where it was (GO's
#          stagetime), the kill table with the three kills in the seat's row,
#          both players' lives back where they stood. The new host ends it and
#          both are back in the room.
#   crash  the same with the host gone at frame 900 as a crash is (no goodbye):
#          the joiners' connections time out, they report the host lost, and
#          the lobby moves the room on before its 15 s heartbeat timeout.
#   lobby  the host plays a match to its end (frame 300), then leaves the
#          room from the Game Lobby: the room stays open under a joiner, which
#          plays a second match in it with the other.
#   coop   a co-op room on Defection; the host quits at frame 900 of the
#          mission: the new host starts the mission again from its beginning
#          and the other joiner is carried into it.
#   served a GoldenEye arena (Complex, GoldenEye's Combat Simulator) whose
#          only joiner has no mods and no ROM of its own (a folder of its own,
#          as netcontenttest's fetch client): the host serves it GoldenEye
#          Arenas into memory, quits at frame 900, and the joiner takes the
#          room over and hosts the arena from that copy (the user,
#          2026-10-08: "they should still have the download from original
#          host"); a third game with nothing installed then joins the match in
#          progress and is served the conversion by the new host in turn.
#   midfetch the host goes in the middle of serving a folder: a host and alpha
#          with GoldenEye's arenas, bravo with nothing installed; the host
#          quits once it has sent bravo 3 MB of GoldenEye Arenas
#          (--net-test-serve-quit, before the match's GO: alpha waits at the
#          barrier for bravo's download, as long as the host does). Alpha takes
#          the room over; bravo's half-filled folder is dropped with the old
#          session and the new host serves it whole (an abandoned fetch had
#          blocked the folder for the process: "not installed here").
#   fetchend (no migration: the lifecycle round a download) a host and alpha
#          with PD_Kakariko's maps play two matches of 600 frames on its
#          Playground; charlie, with nothing installed, joins the first in
#          progress and is served the folder slowly (--net-test-serve-pace
#          650: a part, at most 48 KB, every 0.65 s; 139 files, ~90 s):
#          the first match ends while it downloads (its kept STAGE_LOAD is
#          dropped and LOBBY sent: it had loaded the finished match after the
#          download and sat at the barrier until it left), the second waits
#          for it at the barrier past the clients' own 60 s (the host's
#          PLAYERS keep alpha waiting: it had left with "No GO came"), and
#          all three play it.
#
#   netmigratetest.sh [BIN]   BIN a file name in build/ (pd.x86_64) or a path
#
# Env: OUT (build/netmigrate-out), GPORT (27190: the games' ports, +0..+2 per
# case, cases 10 apart), MODDIR (mod_allinone), CASES ("quit crash lobby coop").
# Exit status: 0 all good, 1 a check failed, 2 a run failed to start.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=${BUILD:-$ROOT/build}
OUT=${OUT:-$BUILD/netmigrate-out}; GPORT=${GPORT:-27190}
MODDIR=${MODDIR:-mod_allinone}
CASES=${CASES:-quit crash lobby coop served midfetch fetchend}
BIN=${1:-pd.x86_64}
case $BIN in /*) ;; */*) BIN=$(realpath "$BIN") ;; *) BIN=$BUILD/$BIN ;; esac
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
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

# a lobby of its own for each case (its sign-in limit counts 127.0.0.1)
LOBBY=; LPORT=; L=
lobby() {
	L=$OUT/pdlobbyd-$1.log
	python3 -u "$ROOT/tools/pdlobbyd/pdlobbyd.py" --host 127.0.0.1 --port 0 --udp-host 127.0.0.1 --udp-port 0 --auth open --relay-ports 0 \
		> "$L" 2>&1 &
	LOBBY=$!
	if ! waitfor "$L" "pdlobbyd listening on" 20; then
		echo "FAIL: pdlobbyd did not start (see $L)"; kill $LOBBY 2>/dev/null; exit 2
	fi
	LPORT=$(sed -n 's/.*listening on 127.0.0.1:\([0-9]*\).*/\1/p' "$L" | head -1)
}

# game LABEL USER GAMEPORT TIMEOUT ARGS... &  ($! is timeout's pid)
# Env per call: GEMAPS 1 (the GoldenEye arenas mounted, no overlay mod),
# BARE 1 (the binary in a folder of its own with no mods and no added
# content, netcontenttest's fetch client), MAPMODS (no overlay, these map mods)
game() {
	local label=$1 user=$2 port=$3 t=$4; shift 4
	local save=$OUT/save-$label bin=$BIN dir=$BUILD mod=(--moddir "$MODDIR") ini=
	rm -rf "$save"; mkdir -p "$save"
	for f in eeprom.bin mpsetups.bin; do
		[ -f "$HOME/.local/share/perfectdark/$f" ] && cp "$HOME/.local/share/perfectdark/$f" "$save/"
	done
	if [ "${GEMAPS:-0}" = 1 ] || [ "${BARE:-0}" = 1 ]; then
		mod=(); ini='ModDir=\nMapMods=GoldenEye Arenas\n'
	fi
	if [ -n "${MAPMODS:-}" ]; then
		mod=(); ini="ModDir=\\nMapMods=$MAPMODS\\n"
	fi
	if [ "${BARE:-0}" = 1 ]; then
		dir=$OUT/bare-$label
		rm -rf "$dir"; mkdir -p "$dir/mods" "$dir/added-content"
		ln "$BIN" "$dir/pd.x86_64" 2>/dev/null || cp "$BIN" "$dir/pd.x86_64"
		ln -s "$BUILD/data" "$dir/data"
		bin=$dir/pd.x86_64
	fi
	printf '[Mod]\nGhostUser=%s\nGhostPin=1234\n%b[Net]\nLobbyServer=http://127.0.0.1:%s\nPort=%s\n' "$user" "$ini" "$LPORT" "$port" > "$save/pd.ini"
	cd "$dir" || exit 2
	exec timeout -k 5 "$t" stdbuf -oL -eL "$bin" --savedir "$save" --skip-intro --no-sound "${mod[@]}" "$@" > "$OUT/$label.log" 2>&1
}

line() { grep -m1 -o -- "$2.*" "$1"; }
num() { grep -m1 -o -- "$2" "$1" | grep -o '[-0-9]*$'; }

# run CASE PORT HOSTARGS... -- JOINARGS...: a host and two joiners
run() {
	local c=$1 port=$2; shift 2
	local hargs=() jargs=()
	while [ $# -gt 0 ] && [ "$1" != -- ]; do hargs+=("$1"); shift; done
	shift
	jargs=("$@")
	lobby "$c"
	game "$c-host" "mh$c" "$port" 300 --net-lobby-script host --net-lobby-room "Migrate $c" --net-lobby-size 3 --net-lobby-wait 2 \
		"${hargs[@]}" &
	local ph=$!
	game "$c-alpha" "alpha$c" $((port + 1)) 300 --net-lobby-script join --net-lobby-room "Migrate $c" "${jargs[@]}" &
	local pa=$!
	sleep 4
	game "$c-bravo" "bravo$c" $((port + 2)) 300 --net-lobby-script join --net-lobby-room "Migrate $c" "${jargs[@]}" &
	local pb=$!
	wait $ph; RH=$?; wait $pa; RA=$?; wait $pb; RB=$?
	kill $LOBBY 2>/dev/null; wait $LOBBY 2>/dev/null
	echo "     $c: exits host $RH alpha $RA bravo $RB"
}

# which joiner hosts now, from its log; the other
newhost() {
	local c=$1
	if grep -q "lobby: hosting room .* taken over from mh$c" "$OUT/$c-alpha.log"; then NH=$OUT/$c-alpha.log; NM=$OUT/$c-bravo.log; NHU=alpha$c; NMU=bravo$c
	elif grep -q "lobby: hosting room .* taken over from mh$c" "$OUT/$c-bravo.log"; then NH=$OUT/$c-bravo.log; NM=$OUT/$c-alpha.log; NHU=bravo$c; NMU=alpha$c
	else NH=; NM=; fi
}

# the carried-on match, checked on the new host and the other joiner
carried() {
	local c=$1 H=$OUT/$1-host.log room t0 t1 t2
	room=$(sed -n 's/.*lobby: made room \([0-9a-f]*\).*/\1/p' "$H" | head -1)
	[ -n "$room" ] && pass "$c: host made room $room" || { fail "$c: no room"; return; }
	grep -q "net: match 1: every machine has loaded; GO" "$H" && pass "$c: the first match passed GO: $(line "$H" "net: match 1 starting on" | sed 's/, seeds.*//')" \
		|| fail "$c: the first match never passed GO"
	newhost "$c"
	[ -n "$NH" ] && pass "$c: $(line "$NH" "lobby: hosting room")" || { fail "$c: no joiner took the room over"; return; }
	grep -q "room $room: host mh$c .* $NHU hosts it now (epoch 2, match to resume)" "$L" \
		&& pass "$c: pdlobbyd: $(line "$L" "room $room: host mh$c")" || fail "$c: pdlobbyd never handed the room to $NHU"
	grep -q "room $room relaunched by $NHU" "$L" && pass "$c: pdlobbyd: room relaunched by its new host" || fail "$c: the room was not relaunched"
	grep -q "net: migrate: kept room $room's match, to carry on" "$NH" && pass "$c: new host: $(line "$NH" "net: migrate: kept room")" \
		|| fail "$c: the new host kept no match"
	grep -q "net: rules: the last match's rules adopted" "$NH" && pass "$c: new host: $(line "$NH" "net: rules: the last match's rules adopted")" \
		|| fail "$c: the new host did not adopt the match's rules"
	grep -q "net: migrate: carrying room $room's match on: stage 0x32" "$NH" && pass "$c: new host: $(line "$NH" "net: migrate: carrying room")" \
		|| fail "$c: the new host did not carry the match on"
	grep -q "net: migrate: this player's state sent to the new host" "$NM" && pass "$c: $NMU sent its player's state (RESUME)" \
		|| fail "$c: $NMU sent no RESUME"
	grep -q "net: accepted by \"$NHU\"" "$NM" && pass "$c: $NMU: $(line "$NM" "net: accepted by \"$NHU\"")" || fail "$c: $NMU never joined the new host"
	grep -q "net: migrate: match 1 carries on at level time" "$NM" && pass "$c: $NMU: $(line "$NM" "net: migrate: match 1 carries on")" \
		|| fail "$c: $NMU's GO did not carry the clock"
	# the clock: the old host's at its going, the new host's kept one, the resumed match's
	t0=$(sed -n 's/.*lobby script: host: frame [0-9]*, level time \([0-9]*\);.*/\1/p' "$H" | head -1)
	t1=$(sed -n 's/.*carrying room .* level time \([0-9]*\),.*/\1/p' "$NH" | head -1)
	t2=$(sed -n 's/.*the match carries on at level time \([0-9]*\) .*/\1/p' "$NH" | head -1)
	if [ -n "$t0" ] && [ -n "$t1" ] && [ -n "$t2" ] && [ $((t1 - t0)) -lt 240 ] && [ $((t0 - t1)) -lt 240 ] && [ $((t2 - t1)) -lt 120 ] && [ "$t2" -ge "$t1" ]; then
		pass "$c: level clock carried: the old host's $t0 at its going, kept $t1, resumed at $t2 (60ths)"
	else
		fail "$c: level clock: old host $t0, kept $t1, resumed $t2"
	fi
	if grep -q "net: migrate: the seats' scores carried: .*kills 3 " "$NH"; then
		pass "$c: kill table carried: $(line "$NH" "net: migrate: the seats' scores carried" | cut -c1-200)"
	else
		fail "$c: the three kills were not carried: $(line "$NH" "net: migrate: the seats' scores carried")"
	fi
	# a player dead when the host went (the sims kill the idle test players)
	# starts a life of its own; every one alive is put back
	local alive back
	alive=$(grep "net: migrate: seat [0-9]*'s record" "$NH" | grep -vc "(dead)")
	back=$(grep -c "net: migrate: player [0-9]* (seat [0-9]*) back where it was" "$NH")
	[ "$alive" -ge 1 ] && [ "$back" = "$alive" ] \
		&& pass "$c: $back of $(grep -c "net: migrate: seat [0-9]*'s record" "$NH") records alive, each life back where it stood ($(grep -o "net: migrate: player [0-9]* (seat [0-9]*) back where it was: [-0-9. ]*" "$NH" | sed 's/net: migrate: //' | tr '\n' ';'))" \
		|| fail "$c: $back player(s) given their life back, $alive record(s) alive"
	# the member's record against where the new host put it
	local rx rz bx bz seat
	seat=$(grep "net: migrate: seat [0-9]*'s record: at" "$NH" | grep -v "this machine's" | head -1)
	if echo "$seat" | grep -q "(dead)"; then
		pass "$c: $NMU was dead when the host went: a life of its own"
		seat=
	else
		seat=$(echo "$seat" | sed -n 's/.*net: migrate: seat \([0-9]*\)'"'"'s record: at.*/\1/p')
	fi
	if [ -n "$seat" ]; then
		read -r rx _ rz <<< "$(sed -n 's/.*net: migrate: seat '"$seat"''"'"'s record: at \([-0-9]*\) \([-0-9]*\) \([-0-9]*\),.*/\1 \2 \3/p' "$NH" | head -1)"
		read -r bx _ bz <<< "$(sed -n 's/.*(seat '"$seat"') back where it was: \([-0-9]*\) \([-0-9]*\) \([-0-9]*\),.*/\1 \2 \3/p' "$NH" | head -1)"
		if [ -n "$rx" ] && [ -n "$bx" ] && [ $(( (rx - bx) * (rx - bx) + (rz - bz) * (rz - bz) )) -lt 2500 ]; then
			pass "$c: $NMU's player is back at $bx,$bz where its record had it ($rx,$rz)"
		else
			fail "$c: $NMU's record at $rx,$rz, put back at $bx,$bz"
		fi
	elif ! grep "net: migrate: seat [0-9]*'s record: at" "$NH" | grep -v "this machine's" | grep -q "(dead)"; then
		fail "$c: the new host got no record from $NMU"
	fi
	echo "     $c: back in the match after the host went: $(grep -h -o "in the room's match again [0-9]* ms" "$NH" "$NM" | sed 's/.*again //' | tr '\n' ' ')"
	grep -q "lobby script: resumed: frame 240" "$NM" && pass "$c: $NMU playing on: $(line "$NM" "lobby script: resumed:" | sed 's/lobby script: //')" \
		|| fail "$c: $NMU never played on in the match"
	grep -q "join: back in room $room after the carried-on match" "$NH" && grep -q "join: back in room $room after the carried-on match" "$NM" \
		&& pass "$c: both back in the room: $(line "$NM" "join: back in room" | sed 's/join: //')" || fail "$c: not both back in the room after"
	[ "$RA" = 0 ] && [ "$RB" = 0 ] || fail "$c: exit codes alpha $RA bravo $RB"
}

port=$GPORT
for c in $CASES; do
	case $c in
	quit)
		run quit $port --net-test-stage 0x32 --net-test-sims 2 --rng-seed 21 --net-test-givekills 1,300,3 --net-lobby-quit-frame 900 \
			-- --net-lobby-leave-frame 0 --net-lobby-migrate 600
		carried quit
		;;
	crash)
		run crash $port --net-test-stage 0x32 --net-test-sims 2 --rng-seed 22 --net-test-givekills 1,300,3 --net-lobby-crash-frame 900 \
			-- --net-lobby-leave-frame 0 --net-lobby-migrate 600
		carried crash
		H=$OUT/crash-host.log
		if grep -q "room [0-9a-f]*: host mhcrash was reported lost by" "$L"; then
			pass "crash: pdlobbyd: $(line "$L" "host mhcrash was reported lost")"
		else
			fail "crash: the room moved on by $(grep -o "host mhcrash [a-z ]*" "$L" | head -1), not by the joiners' reports"
		fi
		# a joiner whose connection had not timed out yet when the room moved
		# on leaves the old session for the new host instead
		if grep -q "lost room .*'s host" "$OUT/crash-alpha.log" "$OUT/crash-bravo.log"; then
			pass "crash: $(grep -l "lost room .*'s host" "$OUT/crash-alpha.log" "$OUT/crash-bravo.log" | sed 's/.*crash-\(.*\).log/\1/' | tr '\n' ' ')told the lobby the host was lost"
		else
			fail "crash: no joiner reported the host lost"
		fi
		;;
	lobby)
		run lobby $port --net-test-stage 0x32 --net-test-sims 2 --rng-seed 23 --net-lobby-end-frame 300 --net-lobby-host-leaves \
			-- --net-lobby-leave-frame 0 --net-lobby-matches 2 --net-lobby-migrate 300
		H=$OUT/lobby-host.log
		room=$(sed -n 's/.*lobby: made room \([0-9a-f]*\).*/\1/p' "$H" | head -1)
		grep -q "host: frame 300; End Game" "$H" && grep -q "host: leaving the room" "$H" \
			&& pass "lobby: the host played its match to the end, then left room $room from the Game Lobby" || fail "lobby: the host did not end its match and leave"
		newhost lobby
		[ -n "$NH" ] && pass "lobby: $(line "$NH" "lobby: hosting room")" || fail "lobby: no joiner took the room over"
		if [ -n "$NH" ]; then
			grep -q "room $room: host mhlobby left; $NHU hosts it now (epoch 2)$" "$L" && pass "lobby: pdlobbyd: $(line "$L" "room $room: host mhlobby")" \
				|| fail "lobby: pdlobbyd did not hand the open room on"
			grep -q "taken over from mhlobby; its launch to follow" "$NH" && fail "lobby: an open room was relaunched"
			grep -q "net: migrate: kept room $room's setup" "$NH" && grep -q "net: rules: the last match's rules adopted" "$NH" \
				&& pass "lobby: the new host plays the room's last setup: $(line "$NH" "net: migrate: kept room")" || fail "lobby: the room's setup was not adopted"
			grep -q "join: hosting the room now; a member is ready; LAUNCH" "$NH" && pass "lobby: $NHU launched the room's next match" || fail "lobby: $NHU never launched"
			grep -q "net: match 1 starting on stage 0x32" "$NH" && pass "lobby: $(line "$NH" "net: match 1 starting on" | sed 's/, seeds.*//') (the room's arena)" \
				|| fail "lobby: the new host's match is not on the room's arena"
			grep -q "net: accepted by \"$NHU\"" "$NM" && grep -q "join: in the room's match 2" "$NM" && pass "lobby: $NMU played the second match with $NHU" \
				|| fail "lobby: $NMU did not play a match with $NHU"
			grep -q "join: hosting the room's match; frame 300; End Game" "$NH" && grep -q "client back in room $room after match 2" "$NH" \
				&& grep -q "client back in room $room after match 2" "$NM" && pass "lobby: both back in the room after it" || fail "lobby: not both back in the room after match 2"
		fi
		[ "$RA" = 0 ] && [ "$RB" = 0 ] || fail "lobby: exit codes alpha $RA bravo $RB"
		;;
	coop)
		run coop $port --net-lobby-coop 0 --net-test-sims 0 --net-lobby-quit-frame 900 \
			-- --net-lobby-leave-frame 0 --net-lobby-migrate 600
		H=$OUT/coop-host.log
		room=$(sed -n 's/.*lobby: made room \([0-9a-f]*\).*/\1/p' "$H" | head -1)
		grep -q "net: co-op: starting dataDyne Defection" "$H" && grep -q "host: frame 900, level time [0-9]*; quitting the game" "$H" \
			&& pass "coop: the host started Defection for the room and quit at frame 900" || fail "coop: the host's mission did not run to its quit"
		newhost coop
		[ -n "$NH" ] && pass "coop: $(line "$NH" "lobby: hosting room")" || fail "coop: no joiner took the room over"
		if [ -n "$NH" ]; then
			grep -q "room $room: host mhcoop left; $NHU hosts it now (epoch 2, match to resume)" "$L" && grep -q "room $room relaunched by $NHU" "$L" \
				&& pass "coop: pdlobbyd handed the room on and $NHU relaunched it" || fail "coop: pdlobbyd did not hand on and relaunch"
			grep -q "net: migrate: kept room $room's mission, to start again" "$NH" && pass "coop: $(line "$NH" "net: migrate: kept room")" || fail "coop: no mission kept"
			grep -q "net: migrate: starting mission 0 of Perfect Dark again" "$NH" && grep -q "net: co-op: starting dataDyne Defection" "$NH" \
				&& pass "coop: $(line "$NH" "net: co-op: starting dataDyne Defection")" || fail "coop: the new host did not start the mission again"
			[ "$(grep -c "net: co-op: the host's mission is dataDyne Defection" "$NM")" -ge 2 ] && grep -q "net: accepted by \"$NHU\"" "$NM" \
				&& pass "coop: $NMU carried into the mission again under $NHU" || fail "coop: $NMU was not carried into the mission again"
			grep -q "this player's state sent" "$NM" && fail "coop: $NMU sent a RESUME for a mission (it starts again)"
			echo "     coop: back in the mission after the host went: $(grep -h -o "in the room's match again [0-9]* ms" "$NH" "$NM" | sed 's/.*again //' | tr '\n' ' ')"
			grep -q "join: hosting the carried-on match; frame 600" "$NH" && grep -q "join: back in room $room after the carried-on match" "$NM" \
				&& pass "coop: the new host ended the mission; both back in the room" || fail "coop: not both back in the room"
		fi
		[ "$RA" = 0 ] && [ "$RB" = 0 ] || fail "coop: exit codes alpha $RA bravo $RB"
		;;
	served)
		lobby served
		GEMAPS=1 game served-host mhserved "$port" 300 --net-lobby-script host --net-lobby-room "Migrate served" --net-lobby-size 3 \
			--net-lobby-wait 1 --net-test-map Complex --net-test-ge 0 --net-test-sims 0 --rng-seed 24 --net-lobby-quit-frame 900 &
		ph=$!
		BARE=1 game served-alpha alphaserved $((port + 1)) 300 --net-lobby-script join --net-lobby-room "Migrate served" \
			--net-lobby-leave-frame 0 --net-lobby-migrate 1500 &
		pa=$!
		# the third once the match carries on under its new host
		if waitfor "$OUT/served-alpha.log" "in the room's match again" 200; then
			BARE=1 game served-charlie charlieserved $((port + 2)) 240 --net-lobby-script join --net-lobby-room "Migrate served" \
				--net-lobby-leave-frame 0 &
			pc=$!
		else
			pc=
		fi
		wait $ph; RH=$?; wait $pa; RA=$?
		RC=-; [ -n "$pc" ] && { wait $pc; RC=$?; }
		kill $LOBBY 2>/dev/null; wait $LOBBY 2>/dev/null
		echo "     served: exits host $RH alpha $RA charlie $RC"
		H=$OUT/served-host.log; A=$OUT/served-alpha.log; C=$OUT/served-charlie.log
		room=$(sed -n 's/.*lobby: made room \([0-9a-f]*\).*/\1/p' "$H" | head -1)
		grep -q "net: match 1 starting on Complex (GoldenEye Arenas)" "$H" && pass "served: $(line "$H" "net: match 1 starting on" | sed 's/, seeds.*//')" \
			|| fail "served: the host's match was not GoldenEye's Complex"
		grep -q "^mod: 0 installed" "$A" && pass "served: alpha had no mods of its own" || fail "served: alpha's mod list was not empty"
		grep -q "net: content: GoldenEye Arenas fetched from the host and mounted for its maps" "$A" \
			&& pass "served: alpha: $(line "$A" "net: content: GoldenEye Arenas fetched" | cut -c14-)" || fail "served: alpha was not served GoldenEye Arenas"
		grep -q "room $room: host mhserved left; alphaserved hosts it now (epoch 2, match to resume)" "$L" \
			&& pass "served: pdlobbyd handed the room to alpha (it said it could host from its served copy)" || fail "served: the room was not handed to alpha"
		grep -q "lobby: hosting room $room .* taken over from mhserved" "$A" && pass "served: alpha: $(line "$A" "lobby: hosting room")" \
			|| fail "served: alpha did not take the room over"
		grep -q "net: migrate: carrying room $room's match on: stage 0x[0-9a-f]*" "$A" && grep -q "net: match 1 starting on Complex (GoldenEye Arenas)" "$A" \
			&& pass "served: alpha hosts Complex from its copy: $(line "$A" "net: match 1 starting on" | sed 's/, seeds.*//')" || fail "served: alpha did not host the arena"
		grep -q "join: in the room's match again" "$A" && pass "served: alpha: $(line "$A" "join: in the room's match again" | cut -c1-80)" || fail "served: alpha's match did not come back"
		if [ -n "$pc" ]; then
			grep -q "^mod: 0 installed" "$C" && pass "served: charlie had no mods of its own" || fail "served: charlie's mod list was not empty"
			grep -q "net: accepted by \"alphaserved\"" "$C" && pass "served: charlie: $(line "$C" "net: accepted by")" || fail "served: charlie did not join alpha's match"
			grep -q "net: content: GoldenEye Arenas served to peer" "$A" && pass "served: alpha: $(line "$A" "net: content: GoldenEye Arenas served to peer")" \
				|| fail "served: alpha did not serve its copy on"
			grep -q "net: content: GoldenEye Arenas fetched from the host and mounted for its maps" "$C" && grep -q "join: in the room's match in progress" "$C" \
				&& pass "served: charlie fetched it from alpha and played the match in progress" || fail "served: charlie did not fetch and play"
			grep -q "client back in room $room after match 1" "$C" && pass "served: charlie back in the room after alpha ended the match" || fail "served: charlie not back in the room"
		else
			fail "served: alpha's match never came back; charlie was not started"
		fi
		[ "$RA" = 0 ] || fail "served: alpha exit $RA"
		;;
	midfetch)
		lobby midfetch
		GEMAPS=1 game midfetch-host mhmidfetch "$port" 300 --net-lobby-script host --net-lobby-room "Migrate midfetch" --net-lobby-size 3 \
			--net-lobby-wait 2 --net-test-map Complex --net-test-ge 0 --net-test-sims 0 --rng-seed 24 --net-test-serve-quit 3000000 &
		ph=$!
		GEMAPS=1 game midfetch-alpha alphamidfetch $((port + 1)) 300 --net-lobby-script join --net-lobby-room "Migrate midfetch" \
			--net-lobby-leave-frame 0 --net-lobby-migrate 900 &
		pa=$!
		sleep 4
		BARE=1 game midfetch-bravo bravomidfetch $((port + 2)) 300 --net-lobby-script join --net-lobby-room "Migrate midfetch" \
			--net-lobby-leave-frame 0 --net-lobby-migrate 900 &
		pb=$!
		wait $ph; RH=$?; wait $pa; RA=$?; wait $pb; RB=$?
		kill $LOBBY 2>/dev/null; wait $LOBBY 2>/dev/null
		echo "     midfetch: exits host $RH alpha $RA bravo $RB"
		H=$OUT/midfetch-host.log; A=$OUT/midfetch-alpha.log; B=$OUT/midfetch-bravo.log
		grep -q "net: content: --net-test-serve-quit" "$H" && pass "midfetch: host: $(line "$H" "net: content: --net-test-serve-quit" | cut -c14-)" \
			|| fail "midfetch: the host did not quit mid-transfer"
		grep -q "^mod: 0 installed" "$B" && pass "midfetch: bravo had no mods of its own" || fail "midfetch: bravo's mod list was not empty"
		grep -q "net: content: the session ended [0-9]* of [0-9]* bytes into GoldenEye Arenas; dropped" "$B" \
			&& pass "midfetch: bravo: $(line "$B" "net: content: the session ended" | cut -c14-)" || fail "midfetch: bravo did not drop the fetch with the session"
		grep -q "lobby: hosting room .* taken over from mhmidfetch" "$A" && pass "midfetch: alpha: $(line "$A" "lobby: hosting room")" \
			|| fail "midfetch: alpha did not take the room over"
		grep -q "net: content: GoldenEye Arenas served to peer" "$A" && pass "midfetch: alpha: $(line "$A" "net: content: GoldenEye Arenas served to peer")" \
			|| fail "midfetch: alpha did not serve the folder"
		grep -q "net: content: GoldenEye Arenas fetched from the host and mounted for its maps" "$B" \
			&& pass "midfetch: bravo: $(line "$B" "net: content: GoldenEye Arenas fetched" | cut -c14-)" || fail "midfetch: bravo never had the folder whole"
		grep -q "not installed here" "$B" && fail "midfetch: bravo left: $(line "$B" "not installed here")"
		grep -q "net: accepted by \"alphamidfetch\"" "$B" && grep -q "net: match [0-9]*: every machine has loaded; GO" "$A" \
			&& pass "midfetch: bravo played the room's match under alpha" || fail "midfetch: bravo did not play under alpha"
		[ "$RA" = 0 ] && [ "$RB" = 0 ] || fail "midfetch: exit codes alpha $RA bravo $RB"
		;;
	fetchend)
		lobby fetchend
		MAPMODS=PD_Kakariko game fetchend-host mhfetchend "$port" 330 --net-lobby-script host --net-lobby-room "Migrate fetchend" --net-lobby-size 3 \
			--net-lobby-wait 1 --net-lobby-matches 2 --net-lobby-end-frame 600 --net-test-map Playground --net-test-sims 0 \
			--rng-seed 24 --net-test-serve-pace 650 &
		ph=$!
		MAPMODS=PD_Kakariko game fetchend-alpha alphafetchend $((port + 1)) 330 --net-lobby-script join --net-lobby-room "Migrate fetchend" \
			--net-lobby-leave-frame 0 --net-lobby-matches 2 &
		pa=$!
		if waitfor "$OUT/fetchend-host.log" "net: match 1: every machine has loaded; GO" 120; then
			BARE=1 game fetchend-charlie charliefetchend $((port + 2)) 300 --net-lobby-script join --net-lobby-room "Migrate fetchend" \
				--net-lobby-leave-frame 0 --net-lobby-matches 1 &
			pc=$!
		else
			pc=
		fi
		wait $ph; RH=$?; wait $pa; RA=$?
		RC=-; [ -n "$pc" ] && { wait $pc; RC=$?; }
		kill $LOBBY 2>/dev/null; wait $LOBBY 2>/dev/null
		echo "     fetchend: exits host $RH alpha $RA charlie $RC"
		H=$OUT/fetchend-host.log; A=$OUT/fetchend-alpha.log; C=$OUT/fetchend-charlie.log
		grep -q "net: match 1 ended while its folder was still coming; its stage load dropped, back in the room" "$C" \
			&& pass "fetchend: charlie: $(line "$C" "net: match 1 ended while" | cut -c6-)" || fail "fetchend: charlie did not drop match 1's stage load at its end"
		grep -q "is still on the last match's end screen; it sits this match out" "$H" \
			&& fail "fetchend: host: $(line "$H" "is still on the last match's end screen")" || pass "fetchend: nobody sat match 2 out"
		grep -q "net: content: PD_Kakariko fetched from the host and mounted for its maps" "$C" \
			&& pass "fetchend: charlie: $(line "$C" "net: content: PD_Kakariko fetched" | cut -c14-)" || fail "fetchend: charlie never had the folder"
		grep -q "net: match 2: every machine has loaded; GO" "$H" && pass "fetchend: host: match 2 passed GO" || fail "fetchend: match 2 never passed GO"
		grep -q "net: match 2: GO" "$C" && pass "fetchend: charlie played match 2" || fail "fetchend: charlie did not play match 2"
		w=$(sed -n 's/.*net: match 2: GO, \([0-9]*\) s after this machine had loaded.*/\1/p' "$A" | head -1)
		[ -n "$w" ] && [ "$w" -gt 60 ] && pass "fetchend: alpha waited $w s at the barrier (its own limit 60) and played match 2" \
			|| fail "fetchend: alpha's wait at match 2's barrier: ${w:-no GO} s (more than 60 wanted)"
		grep -q "No GO came\|did not start the match" "$A" && fail "fetchend: alpha gave up: $(line "$A" "session ended")"
		grep -q "net: match 2: the host is still waiting at the barrier" "$A" && pass "fetchend: alpha: $(line "$A" "net: match 2: the host is still waiting" | cut -c6-)" \
			|| fail "fetchend: alpha was never told the host still waited"
		[ "$RA" = 0 ] || fail "fetchend: alpha exit $RA"
		;;
	esac
	port=$((port + 10))
done

exit $status
