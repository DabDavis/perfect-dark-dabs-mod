#!/bin/bash
# netcooptest.sh — online co-op: the solo missions for up to twelve players
# (PLANS/netplay/spec-coop.md, CLAUDE-notes/netplay.md "Online co-op").
#
# Loopback, offscreen on the GPU, a listen host and its clients on 127.0.0.1:
#
#   pair    a host and one client on dataDyne Defection (Agent), the opening
#           played whole: the host's mission in RULES (the client loads it as
#           a mission, every stage hash the host's), the first mission block,
#           the opening's camera cuts started on the client at the host's
#           clock and ended when the host's did, the guards posed from
#           SETUPCHR records (the puppet trace), prediction matching after
#           the opening (nothing reconciled during it), then the staging on
#           the host through gdb: the client's player killed, its START (a
#           tick in every sixty: the pause menu while it lives, the respawn
#           when dead) bringing it back with half a living player's health,
#           and the host's Abort Mission ending it for both: the client's
#           end screen (closed by its START) and its stage stopped.
#   twelve  a host and eleven clients (twelve games at once: alone), the
#           opening skipped: every client loads and passes GO, every one's
#           prediction matches, the twelve spawn apart (the ring round the
#           pad), nobody crashes.
#   lobby   a local pdlobbyd and a pair through a room created as a co-op
#           mission (--net-lobby-coop): the room's summary names the mission
#           and "Co-op Agent", the joiner plays the mission, the host's end
#           at a frame reaches the joiner, both come back to the room.
#
#   netcooptest.sh [BIN]   BIN a file name in build/ (pd.x86_64) or a path
#
# Env: OUT (build/netcoop-out), PORT (27600), CASES (pair twelve lobby),
# FRAMES (twelve's client frames, 2700), MODDIR (mod_allinone, the lobby case).
# Exit status: 0 all good, 1 a check failed, 2 a run failed to start.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=${BUILD:-$ROOT/build}
OUT=${OUT:-$BUILD/netcoop-out}; PORT=${PORT:-27600}
CASES=${CASES:-pair twelve lobby}
FRAMES=${FRAMES:-2700}
MODDIR=${MODDIR:-mod_allinone}
BIN=${1:-pd.x86_64}
case $BIN in /*) ;; */*) BIN=$(realpath "$BIN") ;; *) BIN=$BUILD/$BIN ;; esac
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
rm -f "$OUT"/*.log "$OUT"/*.trace "$OUT"/*.script "$OUT"/stage.log
export SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-offscreen} SDL_GAMECONTROLLER_IGNORE_DEVICES=0x054c/0x0ce6,0x045e/0x028e \
	SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT=0x0000/0x0000 SDL_JOYSTICK_HIDAPI=0

status=0
fail() { echo "FAIL $*"; status=1; }
pass() { echo "ok   $*"; }

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

lastline() { grep -- "$2" "$1" 2>/dev/null | tail -1; }
num() { echo "$1" | grep -o -- "$2 [0-9][0-9]*" | head -1 | awk '{print $NF}'; }

crashed() { grep -qE "FATAL|Segmentation|Aborted" "$1"; }

# Host staging through gdb between two ticks (netcontenttest.sh's way)
cat > "$OUT/stage.py" <<'PY'
import gdb
def i(e): return int(gdb.parse_and_eval(e))
def kill(pn):
    """the player of slot pn killed on the host; its START asks for the co-op respawn"""
    if i("(long)g_Vars.players[%d]" % pn) and not i("g_Vars.players[%d]->isdead" % pn):
        was = i("g_Vars.currentplayernum")
        gdb.execute("call (void)setCurrentPlayerNum(%d)" % pn)
        gdb.execute("call (void)playerDie(1)")
        gdb.execute("call (void)setCurrentPlayerNum(%d)" % was)
        print("STAGE kill: player %d killed at tick %d" % (pn, i("g_NetTick")))
    else:
        print("STAGE kill: player %d not alive" % pn)
def abort():
    """the host's Abort Mission"""
    gdb.execute("set var g_Vars.players[0]->aborted = 1")
    gdb.execute("call (void)mainEndStage()")
    print("STAGE abort: the host aborted at tick %d" % i("g_NetTick"))
def status():
    print("STAGE status: tick %d, player 0 dead %d health %.3f, player 1 dead %d canrestart %d health %.3f, objectives %s" % (
        i("g_NetTick"), i("g_Vars.players[0]->isdead"), float(gdb.parse_and_eval("g_Vars.players[0]->bondhealth")),
        i("g_Vars.players[1]->isdead"), i("g_Vars.players[1]->coopcanrestart"), float(gdb.parse_and_eval("g_Vars.players[1]->bondhealth")),
        [i("objectiveCheck(%d)" % k) for k in range(i("g_ObjectiveLastIndex") + 1)]))
PY

stage() {
	local hp=$1 what=$2
	timeout 40 gdb -p "$hp" -batch -ex "source $OUT/stage.py" -ex "break netScenHostTickEnd" -ex "continue" \
		-ex "delete" -ex "python $what" 2>/dev/null | grep "^STAGE" | tee -a "$OUT/stage.log" | sed 's/^/     host: /'
}

# ---------------------------------------------------------------- pair
case_pair() {
	local name=pair port=$PORT H=$OUT/pair-host.log C=$OUT/pair-client.log
	echo "== $name"
	# the client's START a tick in every sixty from the opening's end (about
	# tick 3840 on Defection) on: the pause menu while it lives, the respawn
	# when dead, the end screen's close at the end
	for t in $(seq 3900 60 12000); do echo "$t $((t + 1)) 1000 0 0 0 0 0 0"; done > "$OUT/pair-start.script"
	# (--exit-frame counts a level's own frames: past the mission the games
	# are stopped here once the client's stage has stopped)
	game pair-host 600 '[Mod]\n' --host "$port" --net-test-host 1 --rng-seed 7 --net-test-coop 0 --exit-frame 12000 &
	local host=$!
	waitfor "$H" "net: hosting on UDP port" 90 || { fail "$name: host did not start"; kill -TERM $host; wait $host; return; }
	game pair-client 590 '[Mod]\n' --connect "127.0.0.1:$port" --net-test-join --exit-frame 11000 \
		--net-test-input "$OUT/pair-start.script" --net-puppet-trace "$OUT/pair-puppets.trace" &
	local client=$!
	local hp
	if waitfor "$C" "net: match 1: GO" 150; then
		hp=$(gamepid pair-host)
		# the opening over (Defection's runs about 64 s), then the staging
		if waitfor "$C" "co-op client: the cutscene ended" 200 && [ -n "$hp" ]; then
			sleep 4
			stage "$hp" "status()"
			stage "$hp" "kill(1)"
			sleep 12
			stage "$hp" "status()"
			sleep 6
			stage "$hp" "abort()"
			# the client's end screen closed by its START, its stage stopped (H12)
			waitfor "$C" "co-op client: [0-9]* mission blocks applied" 120 || echo "     the client's stage did not stop within 120 s"
			sleep 3
		else
			echo "     no cutscene end on the client, or no host pid: no staging"
		fi
	fi
	# both stopped here (a TERM is a clean exit for them: 143)
	local cp; cp=$(gamepid pair-client); [ -n "$cp" ] && kill -TERM "$cp"
	wait "$client"; local cx=$?
	[ "$cx" = 143 ] && cx=0
	hp=$(gamepid pair-host); [ -n "$hp" ] && kill -TERM "$hp"
	wait "$host"; local hx=$?
	[ "$hx" = 143 ] && hx=0

	crashed "$H" && fail "$name: the host crashed" || { [ "$hx" = 0 ] && pass "$name: host ran to the end" || fail "$name: host exit $hx"; }
	crashed "$C" && fail "$name: the client crashed" || { [ "$cx" = 0 ] && pass "$name: client ran to the end" || fail "$name: client exit $cx"; }
	grep -q "renderD128: Permission denied" "$H" "$C" && fail "$name: a run fell back to llvmpipe"
	grep -q "net: co-op: starting dataDyne Defection (index 0, stage 0x30) on Agent with 2 players" "$H" \
		&& pass "$name: host: $(grep -o 'co-op: starting.*' "$H" | head -1)" || fail "$name: the host did not start the mission"
	grep -q "net: co-op: the host's mission is dataDyne Defection (index 0, stage 0x30) on difficulty 0" "$C" \
		&& pass "$name: client: $(grep -o "the host's mission is.*" "$C" | head -1)" || fail "$name: the client did not take the mission from RULES"
	grep -q "net: match 1: loading stock stage 0x30 as 0x30" "$C" && pass "$name: the client loaded the mission's stage" || fail "$name: the client did not load stage 0x30"
	grep -q "slot 1 (\"[^\"]*\") loaded the stage, every component the host's" "$H" \
		&& pass "$name: every stage hash component equal" || fail "$name: stage hashes differ"
	grep -q "net: match 1: GO" "$C" && pass "$name: the client passed GO" || fail "$name: no GO on the client"
	grep -q "co-op client: the first mission block" "$C" && pass "$name: $(grep -o 'co-op client: the first mission block.*' "$C" | head -1)" || fail "$name: no mission block reached the client"
	local starts; starts=$(grep -c "co-op client: cutscene anim [0-9]* starts" "$C")
	[ "${starts:-0}" -ge 5 ] && pass "$name: the opening's camera cuts started on the client: $starts (first: $(grep -o 'cutscene anim [0-9]* starts.*' "$C" | head -1))" \
		|| fail "$name: cutscene starts on the client: ${starts:-0}"
	grep -q "co-op client: the cutscene ended" "$C" && pass "$name: $(grep -o 'co-op client: the cutscene ended.*' "$C" | head -1)" || fail "$name: the client's cutscene never ended"
	# the guards: SETUPCHR (kind 10) poses in the puppet trace
	local kinds; kinds=$(awk '$1=="P" && $5==10 {n++} END {print n+0}' "$OUT/pair-puppets.trace" 2>/dev/null)
	[ "${kinds:-0}" -ge 1000 ] && pass "$name: setup chrs posed from SETUPCHR records: $kinds poses" || fail "$name: SETUPCHR poses: ${kinds:-0}"
	# prediction after the opening (nothing reconciled during it)
	local pr; pr=$(lastline "$C" "net: prediction ")
	local matched compared pct left
	matched=$(num "$pr" matched); compared=$(num "$pr" compared); left=$(num "$pr" "cutscene blocks left")
	pct=$(echo "$pr" | grep -o 'matched [0-9]* ([0-9.]*%)' | grep -o '[0-9.]*%' | tr -d %)
	if [ -n "$pct" ] && [ "${compared:-0}" -ge 200 ] && awk -v p="$pct" 'BEGIN { exit !(p >= 95) }'; then
		pass "$name: prediction matched $pct% of $compared ticks after the opening ($left blocks left alone during it)"
	else
		fail "$name: prediction: ${pr:-no summary}"
	fi
	local lb; lb=$(lastline "$C" "net: local block")
	[ "$(num "$lb" respawns)" = 1 ] && [ "$(num "$lb" deaths)" = 1 ] \
		&& pass "$name: the client's player died once and respawned once by its START" \
		|| fail "$name: deaths/respawns: $(echo "$lb" | grep -o 'deaths [0-9]*, respawns [0-9]*')"
	grep -q "STAGE status: tick [0-9]*, player 0 dead 0 health 0.5" "$OUT/stage.log" 2>/dev/null \
		&& pass "$name: the respawn took half of bond's health on the host" \
		|| fail "$name: bond's health after the respawn not 0.5: $(grep -o 'STAGE status.*' "$OUT/stage.log" 2>/dev/null | tail -1)"
	grep -q "co-op client: the mission ended: aborted" "$C" && pass "$name: $(grep -o 'co-op client: the mission ended.*' "$C" | head -1)" \
		|| fail "$name: the client did not see the host's abort"
	grep -q "net: match 1 ended; scores and awards sent" "$H" && pass "$name: the host sent MATCH_END" || fail "$name: no MATCH_END from the host"
	grep -q "co-op client: [0-9]* mission blocks applied" "$C" && pass "$name: the client's stage stopped after its end screen: $(grep -o 'co-op client: [0-9]* mission blocks applied.*' "$C" | head -1)" \
		|| fail "$name: the client never left the stage (its end screen not closed?)"
	local sn; sn=$(lastline "$C" "net: snap client")
	[ "$(echo "$sn" | grep -o '[0-9]* malformed' | awk '{print $1}')" = 0 ] && pass "$name: no malformed snapshot" || fail "$name: malformed snapshots"
}

# ---------------------------------------------------------------- twelve
case_twelve() {
	local name=twelve port=$((PORT + 1)) H=$OUT/twelve-host.log k
	echo "== $name"
	# a direct host seats four unless its ini says more (Net.MaxPlayers)
	game twelve-host 420 '[Mod]\n[Net]\nMaxPlayers=12\n' --host "$port" --net-test-host 11 --rng-seed 7 --net-test-coop 0 --skip-cutscenes \
		--net-test-trace 30 --exit-frame $((FRAMES + 900)) &
	local host=$!
	waitfor "$H" "net: hosting on UDP port" 90 || { fail "$name: host did not start"; kill -TERM $host; wait $host; return; }
	local pids=""
	for k in $(seq 1 11); do
		game "twelve-c$k" 400 '[Mod]\n' --connect "127.0.0.1:$port" --net-test-join --skip-cutscenes --exit-frame "$FRAMES" &
		pids="$pids $!"
	done
	local p x cx=0
	for p in $pids; do wait "$p"; x=$?; [ "$x" = 0 ] || cx=$x; done
	for n in $(seq 40); do kill -0 "$host" 2>/dev/null || break; sleep 1; done
	kill -0 "$host" 2>/dev/null && { p=$(gamepid twelve-host); [ -n "$p" ] && kill -TERM "$p"; }
	wait "$host"; local hx=$?
	[ "$hx" = 143 ] && hx=0

	crashed "$H" && fail "$name: the host crashed" || { [ "$hx" = 0 ] && pass "$name: host ran to the end" || fail "$name: host exit $hx"; }
	local crashes=0 gos=0 loaded preds=0 worst=100
	loaded=$(grep -c "loaded the stage, every component the host's" "$H")
	for k in $(seq 1 11); do
		local C=$OUT/twelve-c$k.log pr pct
		crashed "$C" && crashes=$((crashes + 1))
		grep -q "net: match 1: GO" "$C" && gos=$((gos + 1))
		pr=$(lastline "$C" "net: prediction at exit")
		pct=$(echo "$pr" | grep -o 'matched [0-9]* ([0-9.]*%)' | grep -o '[0-9.]*%' | tr -d %)
		if [ -n "$pct" ] && awk -v p="$pct" 'BEGIN { exit !(p >= 95) }'; then preds=$((preds + 1)); fi
		[ -n "$pct" ] && awk -v p="$pct" -v w="$worst" 'BEGIN { exit !(p < w) }' && worst=$pct
	done
	[ "$crashes" = 0 ] && [ "$cx" = 0 ] && pass "$name: eleven clients ran to the end" || fail "$name: client crashes $crashes, a client exit $cx"
	[ "$loaded" = 11 ] && pass "$name: the host saw all eleven load with every component equal" || fail "$name: clients loaded with equal hashes: $loaded of 11"
	[ "$gos" = 11 ] && pass "$name: every client passed GO" || fail "$name: clients at GO: $gos of 11"
	grep -q "co-op: starting dataDyne Defection .* with 12 players" "$H" && pass "$name: $(grep -o 'co-op: starting.*' "$H" | head -1)" || fail "$name: the host did not start with 12 players"
	[ "$preds" = 11 ] && pass "$name: every client's prediction matched 95% or more (worst $worst%)" || fail "$name: clients with prediction >= 95%: $preds of 11 (worst $worst%)"
	# the spawn ring: at the host's first trace past the opening every player apart from every other
	local tick; tick=$(grep -o "net: trace tick [0-9]* player 0 " "$H" | awk '{print $4}' | awk '$1 >= 600' | head -1)
	if [ -n "$tick" ]; then
		local near; near=$(grep "net: trace tick $tick player" "$H" | sed 's/.*pos \([-0-9.]*\) \([-0-9.]*\) \([-0-9.]*\).*/\1 \3/' \
			| awk '{x[NR]=$1; z[NR]=$2} END { n=0; for (i=1;i<=NR;i++) for (j=i+1;j<=NR;j++) { d=sqrt((x[i]-x[j])^2+(z[i]-z[j])^2); if (d < 20) n++ } print NR, n }')
		local cnt; cnt=$(echo "$near" | awk '{print $1}'); local pairs; pairs=$(echo "$near" | awk '{print $2}')
		[ "$cnt" = 12 ] && [ "$pairs" = 0 ] && pass "$name: at tick $tick the twelve players stand apart (no two within 20 units)" \
			|| fail "$name: at tick $tick: $cnt players traced, $pairs pairs within 20 units"
	else
		fail "$name: no host trace past tick 600"
	fi
	grep -q "renderD128: Permission denied" "$OUT"/twelve-*.log && fail "$name: a run fell back to llvmpipe"
}

# ---------------------------------------------------------------- lobby
case_lobby() {
	local name=lobby port=$((PORT + 2)) L=$OUT/lobby-pdlobbyd.log
	echo "== $name"
	python3 -u "$ROOT/tools/pdlobbyd/pdlobbyd.py" --host 127.0.0.1 --port 0 --udp-host 127.0.0.1 --udp-port 0 --auth open --relay-ports 0 \
		> "$L" 2>&1 &
	local lobby=$!
	if ! waitfor "$L" "pdlobbyd listening on" 20; then
		fail "$name: pdlobbyd did not start"; kill $lobby 2>/dev/null; return
	fi
	local lport; lport=$(sed -n 's/.*listening on 127.0.0.1:\([0-9]*\).*/\1/p' "$L" | head -1)
	local ini="[Mod]\nGhostUser=%s\nGhostPin=1234\n[Net]\nLobbyServer=http://127.0.0.1:$lport\nPort=$port\n"
	# an agent file, as a player has one (a copy; never the real dir)
	local H=$OUT/lobby-host.log J=$OUT/lobby-join.log
	# shellcheck disable=SC2059
	game lobby-host 300 "$(printf "$ini" coophost)" --moddir "$MODDIR" --net-lobby-script host --net-lobby-room "Coop Test" \
		--net-lobby-coop 0 --net-lobby-end-frame 900 --skip-cutscenes --rng-seed 7 &
	local p1=$!
	# shellcheck disable=SC2059
	game lobby-join 300 "$(printf "$ini" coopjoiner)" --moddir "$MODDIR" --net-lobby-script join --net-lobby-room "Coop Test" \
		--net-lobby-leave-frame 0 --skip-cutscenes &
	local p2=$!
	wait $p1; local r1=$?; wait $p2; local r2=$?
	kill $lobby 2>/dev/null; wait $lobby 2>/dev/null
	local room; room=$(sed -n 's/.*lobby: made room \([0-9a-f]*\).*/\1/p' "$H" | head -1)
	[ -n "$room" ] && pass "$name: the host made room $room" || { fail "$name: the host made no room"; return; }
	grep -q "Co-op Agent" "$J" "$H" "$L" && pass "$name: the room's summary says Co-op Agent" || fail "$name: no 'Co-op Agent' in the room's summary"
	grep -q "lobby: joined room $room" "$J" && pass "$name: the joiner joined" || fail "$name: the joiner did not join"
	grep -q "room $room launched" "$L" && pass "$name: pdlobbyd: the room launched" || fail "$name: the room never launched"
	grep -q "net: co-op: starting dataDyne Defection" "$H" && pass "$name: the host started the mission from the room" || fail "$name: the host did not start the mission"
	grep -q "net: co-op: the host's mission is dataDyne Defection" "$J" && pass "$name: the joiner took the mission from RULES" || fail "$name: the joiner did not take the mission"
	grep -q "net: match 1: GO" "$J" && pass "$name: the joiner passed GO" || fail "$name: no GO on the joiner"
	grep -q "host: frame [0-9]*; End Game" "$H" && pass "$name: $(grep -o 'host: frame [0-9]*; End Game' "$H" | head -1)" || fail "$name: the host did not end the mission at its frame"
	grep -q "co-op client: the mission ended" "$J" && pass "$name: $(grep -o 'co-op client: the mission ended.*' "$J" | head -1)" || fail "$name: the joiner did not see the end"
	grep -q "lobby: back in room $room's lobby" "$H" && pass "$name: the host is back in the room" || fail "$name: the host is not back in the room"
	grep -q "lobby: back in room $room's lobby" "$J" && pass "$name: the joiner is back in the room" || fail "$name: the joiner is not back in the room"
	crashed "$H" && fail "$name: the host crashed"; crashed "$J" && fail "$name: the joiner crashed"
	[ "$r1" = 0 ] && [ "$r2" = 0 ] && pass "$name: exits 0 0" || fail "$name: exits host $r1 join $r2"
}

{
for c in $CASES; do
	case $c in
		pair) case_pair ;;
		twelve) case_twelve ;;
		lobby) case_lobby ;;
		*) fail "unknown case $c" ;;
	esac
done
} 2>&1 | tee "$OUT/run.log"
grep -q '^FAIL' "$OUT/run.log" && status=1
echo "netcooptest: $(grep -c '^ok' "$OUT/run.log") ok, $(grep -c '^FAIL' "$OUT/run.log") failed"
exit $status
