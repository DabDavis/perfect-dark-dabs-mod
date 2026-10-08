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
#   ge      GoldenEye's Dam as a co-op mission (protocol 14), both to GO;
#           at tick 1200 the client picks Unarmed as its pause inventory
#           would (--net-test-equip): the host must put it in that player's
#           hands (protocol 15) and the client's hand must still hold it
#           three seconds on (the host's block never took it back).
#   geend   GoldenEye's end of a mission online (protocol 16): a campaign's
#           Dam with a client joined, the host's list kicked into Dam's own
#           ending (the bungee objective, the exit's wait, Bond's dive) and
#           the client's player given 3 kills and 20 shots on the host; the
#           client learns of the wait, its START (a tick in every sixty)
#           skips the outro on the host, both screens fade, and each machine
#           ends on GoldenEye's REPORT page for its own player, Completed, the
#           client's with the host's 3 kills; no Perfect Dark end screen; the
#           client's two NEXTs close its folder.
#   campaign  a GoldenEye campaign's host starting Dam from its folder alone
#           (--net-test-campaign ge --net-test-campaign-mission 0): the
#           opening ends on the host's own player; a client then joins the
#           mission in progress with no opening, spawns beside the host's
#           player and predicts at 95% or better.
#
#   netcooptest.sh [BIN]   BIN a file name in build/ (pd.x86_64) or a path
#
# Env: OUT (build/netcoop-out), PORT (27600), CASES (pair twelve lobby ge campaign geend),
# FRAMES (twelve's client frames, 2700), MODDIR (mod_allinone, the lobby case).
# Exit status: 0 all good, 1 a check failed, 2 a run failed to start.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=${BUILD:-$ROOT/build}
OUT=${OUT:-$BUILD/netcoop-out}; PORT=${PORT:-27600}
CASES=${CASES:-pair twelve lobby ge campaign geend}
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

# ---------------------------------------------------------------- ge
# A conversion's mission (protocol 14): GoldenEye's Dam from the host's room
# (--net-test-coop 0 --net-test-coop-game ge), the client taking the set
# from RULES and the stage by its mission number; both to GO and 1500 ticks
# on, no crash. Skipped where GoldenEye is not converted (mods/GoldenEye
# Arenas; the conversion is mounted for its maps whatever Mod.MapMods says,
# so there is no on-demand mount to see here: netcontenttest's modmount and
# fetch cover those). The opening is GoldenEye's own (gecinema.c), run on
# each machine, so prediction is not judged.
case_ge() {
	local name=ge port=$((PORT + 3)) H=$OUT/ge-host.log C=$OUT/ge-client.log
	echo "== $name"
	game ge-host 300 '[Mod]\nMapMods=GoldenEye Arenas\n' --host "$port" --net-test-host 1 --rng-seed 7 --net-test-coop 0 --net-test-coop-game ge --exit-frame 4000 &
	local host=$!
	waitfor "$H" "net: hosting on UDP port\|cannot be played online" 90 || { fail "$name: host did not start"; kill -TERM $host; wait $host; return; }
	if grep -q "cannot be played online" "$H"; then
		echo "skip $name: GoldenEye is not converted here"; kill -TERM $host; wait $host; return
	fi
	game ge-client 290 '[Mod]\nMapMods=\n' --connect "127.0.0.1:$port" --net-test-join --exit-frame 3000 --net-test-equip 1200,1 &
	local client=$!
	waitfor "$C" "net: match 1: GO" 150 || echo "     no GO on the client"
	waitfor "$C" "net: content client so far (tick 1500)" 120 || echo "     the client did not reach tick 1500"
	local cp; cp=$(gamepid ge-client); [ -n "$cp" ] && kill -TERM "$cp"
	wait "$client"; local cx=$?
	[ "$cx" = 143 ] && cx=0
	local hp; hp=$(gamepid ge-host); [ -n "$hp" ] && kill -TERM "$hp"
	wait "$host"; local hx=$?
	[ "$hx" = 143 ] && hx=0
	crashed "$H" && fail "$name: the host crashed" || { [ "$hx" = 0 ] && pass "$name: host ran to the end" || fail "$name: host exit $hx"; }
	crashed "$C" && fail "$name: the client crashed" || { [ "$cx" = 0 ] && pass "$name: client ran to the end" || fail "$name: client exit $cx"; }
	grep -q "net: co-op: starting Dam (GoldenEye, index 0, stage 0x[0-9a-f]*) on Agent with 2 players" "$H" \
		&& pass "$name: host: $(grep -o 'co-op: starting.*' "$H" | head -1)" || fail "$name: the host did not start GoldenEye's Dam"
	grep -q "net: co-op: the host's mission is Dam (GoldenEye, index 0, stage 0x[0-9a-f]*) on difficulty 0" "$C" \
		&& pass "$name: client: $(grep -o "the host's mission is.*" "$C" | head -1)" || fail "$name: the client did not take the set from RULES"
	grep -q "net: match 1: loading mission 0 of GoldenEye Arenas as 0x" "$C" && pass "$name: the client loaded the mission by its number" || fail "$name: the client did not load mission 0 of GoldenEye Arenas"
	grep -q "slot 1 (\"[^\"]*\") loaded the stage, every component the host's" "$H" && pass "$name: every stage hash component equal" || fail "$name: stage hashes differ"
	grep -q "net: match 1: GO" "$C" && pass "$name: the client passed GO" || fail "$name: no GO on the client"
	grep -q "co-op client: the first mission block" "$C" && pass "$name: $(grep -o 'co-op client: the first mission block.*' "$C" | head -1)" || fail "$name: no mission block reached the client"
	local tick; tick=$(lastline "$C" "net: content client so far" | grep -o "tick [0-9]*" | awk '{print $2}')
	[ "${tick:-0}" -ge 1500 ] && pass "$name: the client played $tick ticks of GoldenEye's Dam" || fail "$name: the client played ${tick:-no} ticks"
	# a gun picked from the client's menu (protocol 15)
	grep -q "net: slot 1 picked guns 0x01/0x00 from its menu" "$H" \
		&& pass "$name: the host took the client's pick: $(grep -o 'slot 1 picked guns.*' "$H" | head -1)" \
		|| fail "$name: the host never took the client's pick ($(grep -o "slot 1's pick.*" "$H" | head -1))"
	grep -q "net: --net-test-equip: tick 1380, the hand holds 0x01 (picked 0x01)" "$C" \
		&& pass "$name: the client's hand still held its pick three seconds on" \
		|| fail "$name: the client's hand: $(grep -o 'net-test-equip: tick.*' "$C" | head -1)"
}

# ---------------------------------------------------------------- campaign
# A GoldenEye campaign (2026-10-07): a --host session opens GoldenEye's
# folder as a campaign room's launch does (--net-test-campaign ge) and the
# folder starts Dam itself (--net-test-campaign-mission 0) with nobody
# connected - a solo start, the host's own player beside the seats join in
# progress keeps open (Net.JoinInProgress). GoldenEye's opening must end on
# the host's own player (it ran on whoever was current: the fade to black
# and back landed on two players and the host played on a black screen).
# Then a client joins the mission in progress: no opening of its own
# (gecinema.c), its life beside the host's player (netCoopJoinSpawn), and
# its prediction must hold from there (its opening had held the player
# through 270 corrections, 49% matched).
case_campaign() {
	local name=campaign port=$((PORT + 4)) H=$OUT/campaign-host.log C=$OUT/campaign-client.log
	echo "== $name"
	game campaign-host 300 '[Mod]\nMapMods=GoldenEye Arenas\n[Net]\nJoinInProgress=1\n' --host "$port" --rng-seed 7 \
		--net-test-campaign ge --net-test-campaign-mission 0 --exit-frame 5400 &
	local host=$!
	waitfor "$H" "net: hosting on UDP port" 90 || { fail "$name: host did not start"; kill -TERM $host; wait $host; return; }
	waitfor "$H" "net: co-op: starting\|not converted here\|the folder is not open" 60
	if grep -q "not converted here" "$H"; then
		echo "skip $name: GoldenEye is not converted here"; kill -TERM $host; wait $host; return
	fi
	waitfor "$H" "gecinema: the opening is over for player" 120 || echo "     the host's opening did not end"
	game campaign-client 240 '[Mod]\nMapMods=\n' --connect "127.0.0.1:$port" --net-test-join --exit-frame 2400 &
	local client=$!
	waitfor "$C" "net: match 1: GO" 120 || echo "     no GO on the client"
	waitfor "$C" "net: prediction so far (tick" 120 || echo "     no prediction line on the client"
	local hold=0
	while [ $hold -lt 60 ] && [ "$(grep -c 'net: prediction so far' "$C")" -lt 3 ]; do sleep 1; hold=$((hold + 1)); done
	local cp; cp=$(gamepid campaign-client); [ -n "$cp" ] && kill -TERM "$cp"
	wait "$client"; local cx=$?
	[ "$cx" = 143 ] && cx=0
	local hp; hp=$(gamepid campaign-host); [ -n "$hp" ] && kill -TERM "$hp"
	wait "$host"; local hx=$?
	[ "$hx" = 143 ] && hx=0
	crashed "$H" && fail "$name: the host crashed" || { [ "$hx" = 0 ] && pass "$name: host ran to the end" || fail "$name: host exit $hx"; }
	crashed "$C" && fail "$name: the client crashed" || { [ "$cx" = 0 ] && pass "$name: client ran to the end" || fail "$name: client exit $cx"; }
	grep -q "net: co-op: the GoldenEye campaign begins" "$H" && pass "$name: the campaign opened the host's folder" || fail "$name: no campaign"
	grep -q "net: co-op: starting Dam (GoldenEye, index 0" "$H" && pass "$name: the folder started Dam" || fail "$name: the folder did not start Dam"
	if grep -q "gecinema: the opening is over for player 0$" "$H" && ! grep -q "gecinema: the opening is over for player [1-9]" "$H"; then
		pass "$name: the host's opening ended on its own player"
	else
		fail "$name: the host's opening ended on $(grep -o 'the opening is over for player [0-9]*' "$H" | head -1 | awk '{print "player", $NF}')"
	fi
	grep -q "an open seat of the match in progress" "$C" && pass "$name: the client joined the mission in progress" || fail "$name: the client did not join in progress"
	grep -q "gecinema: joined the mission in progress; no opening" "$C" && pass "$name: no opening on the joiner" || fail "$name: the joiner played an opening"
	grep -q "gecinema: the opening is over" "$C" && fail "$name: the joiner's opening ran"
	grep -q "net: co-op: player 1 joined in progress; spawns beside player 0" "$H" \
		&& pass "$name: $(grep -o 'player 1 joined in progress; spawns beside.*' "$H" | head -1)" || fail "$name: the joiner did not spawn beside the host's player"
	local pr; pr=$(lastline "$C" "net: prediction so far" | grep -o 'matched [0-9]* ([0-9.]*%)' | grep -o '[0-9.]*%' | tr -d '%')
	awk -v p="${pr:-0}" 'BEGIN { exit !(p >= 95) }' && pass "$name: the joiner's prediction matched ${pr}%" || fail "$name: the joiner's prediction matched ${pr:-no}%"
}

# ---------------------------------------------------------------- geend
# GoldenEye's end of a mission online (2026-10-07, the user: "the outro
# cannot be skipped, then the completion screen goes to a PD screen instead
# of GE"). The exit's wait (TriggerFadeAndExitLevelOnButtonPress) read the
# pad of whichever player was current at the top of lvTick - on a campaign's
# host one of four seats, often an open one - and never a client's START;
# and the co-op end screen was Perfect Dark's. Dam's list 0x1004 is kicked
# into its exit body (the first label 7) at tick 2000, as Bond reaching the
# platform does.
cat > "$OUT/geend.py" <<'PY'
import gdb
def i(e): return int(gdb.parse_and_eval(e))
idx = [k for k in range(i("g_NumBgChrs")) if i("g_BgChrs[%d].chrnum" % k) == 4004]
base = int(gdb.parse_and_eval("g_BgChrs[%d].ailist" % idx[0])) if idx else 0
off, found = 0, None
while base and off < 4000:
    b = bytes(gdb.selected_inferior().read_memory(base + off, 3))
    if b[:3] == b"\x00\x02\x07":
        found = off
        break
    if b[:2] == b"\x00\x04":
        break
    off += i("chraiGetCommandLength((u8*)%d, 0)" % (base + off))
if found is not None:
    gdb.execute("set var g_BgChrs[%d].aioffset = %d" % (idx[0], found))
    gdb.execute("set var g_BgChrs[%d].sleep = 0" % idx[0])
    gdb.execute("set var g_Vars.playerstats[1].killcount = 3")
    gdb.execute("set var g_Vars.playerstats[1].shotcount[0] = 20")
print("STAGE geend: Dam's ending kicked at offset %s at tick %d" % (found, i("g_NetTick")))
PY
cat > "$OUT/geend-press.gdb" <<'GDB'
break gexFrontTick if 'gexfront.c'::g_Front.inputdelay == 0 && 'gexfront.c'::g_Front.active
continue
delete
set var g_JoyDataPtr->buttonspressed[0] = 0x8000
printf "STAGE geend: the client's NEXT on folder page %d\n", 'gexfront.c'::g_Front.screen
GDB

case_geend() {
	local name=geend port=$((PORT + 5)) H=$OUT/geend-host.log C=$OUT/geend-client.log
	echo "== $name"
	# the client's START a tick in every sixty from tick 2600: the exit's
	# wait starts about tick 2420 (the dive after Bond's fall)
	for t in $(seq 2600 60 5000); do echo "$t $((t + 1)) 1000 0 0 0 0 0 0"; done > "$OUT/geend-start.script"
	game geend-host 400 '[Mod]\nMapMods=GoldenEye Arenas\n[Net]\nJoinInProgress=1\n' --host "$port" --rng-seed 7 \
		--net-test-campaign ge --net-test-campaign-mission 0 &
	local host=$!
	waitfor "$H" "net: hosting on UDP port" 90 || { fail "$name: host did not start"; kill -TERM $host; wait $host; return; }
	waitfor "$H" "net: co-op: starting\|not converted here\|the folder is not open" 60
	if grep -q "not converted here" "$H"; then
		echo "skip $name: GoldenEye is not converted here"; kill -TERM $host; wait $host; return
	fi
	waitfor "$H" "gecinema: the opening is over for player" 120 || echo "     the host's opening did not end"
	game geend-client 360 '[Mod]\nMapMods=\n' --connect "127.0.0.1:$port" --net-test-join --net-test-input "$OUT/geend-start.script" &
	local client=$!
	waitfor "$C" "net: match 1: GO" 120 || echo "     no GO on the client"
	local hp; hp=$(gamepid geend-host)
	timeout 60 gdb -p "$hp" -batch -ex "break netScenHostTickEnd if g_NetTick >= 2000" -ex "continue" -ex "delete" \
		-ex "source $OUT/geend.py" 2>/dev/null | grep "^STAGE" | tee -a "$OUT/stage.log" | sed 's/^/     host: /'
	waitfor "$C" "the online mission's report is up\|did not open for the online\|briefing did not load" 120 || echo "     no report on the client"
	waitfor "$H" "back in the folder on mission" 30 || echo "     no report on the host"
	local cp; cp=$(gamepid geend-client)
	if [ -n "$cp" ] && grep -q "the online mission's report is up" "$C"; then
		local n
		for n in 1 2; do
			timeout 40 gdb -p "$cp" -batch -x "$OUT/geend-press.gdb" 2>/dev/null | grep "^STAGE" | sed 's/^/     client: /'
			sleep 1
		done
		waitfor "$C" "the online mission's report closed" 20
	fi
	cp=$(gamepid geend-client); [ -n "$cp" ] && kill -TERM "$cp"
	wait "$client"; local cx=$?
	[ "$cx" = 143 ] && cx=0
	hp=$(gamepid geend-host); [ -n "$hp" ] && kill -TERM "$hp"
	wait "$host"; local hx=$?
	[ "$hx" = 143 ] && hx=0
	crashed "$H" && fail "$name: the host crashed" || { [ "$hx" = 0 ] && pass "$name: host ran to the end" || fail "$name: host exit $hx"; }
	crashed "$C" && fail "$name: the client crashed" || { [ "$cx" = 0 ] && pass "$name: client ran to the end" || fail "$name: client exit $cx"; }
	grep -q "STAGE geend: Dam's ending kicked at offset [0-9]" "$OUT/stage.log" && pass "$name: Dam's ending kicked on the host" || fail "$name: Dam's ending was not found to kick"
	grep -q "net: co-op client: the host's GoldenEye exit waits for a press" "$C" && pass "$name: the client learnt of the exit's wait" || fail "$name: the client never learnt of the exit's wait"
	grep -q "gexplus: the exit's press at frame" "$H" && pass "$name: $(grep -o "the exit's press at frame.*" "$H" | head -1) (the client's START)" || fail "$name: the host never took a press to leave"
	grep -q "net: co-op client: the host's GoldenEye exit fades out" "$C" && pass "$name: the client's screen faded with the host's" || fail "$name: no fade on the client"
	grep -q "this machine's menu up" "$C" && fail "$name: the client's START opened its pause during the exit's wait"
	grep -q "gexfront: online mission 0 over at [0-9]* for player 0 (completed" "$H" && pass "$name: host: $(grep -o 'online mission 0 over.*' "$H" | head -1)" || fail "$name: host: $(grep -o 'online mission 0 over.*' "$H" | head -1)"
	grep -q "gexfront: online mission 0 over at [0-9]* for player 1 (completed, 3 kills)" "$C" && pass "$name: client: $(grep -o 'online mission 0 over.*' "$C" | head -1)" || fail "$name: client: $(grep -o 'online mission 0 over.*' "$C" | head -1)"
	grep -q "net: co-op: no end screen here" "$H" && grep -q "net: co-op: no end screen here" "$C" && pass "$name: no Perfect Dark end screen on either" || fail "$name: a Perfect Dark end screen went up"
	grep -q "gexfront: back in the folder on mission 0's report" "$H" && pass "$name: the host's folder opened on Dam's report" || fail "$name: the host's folder did not open on the report"
	grep -q "gexfront: the online mission's report is up (mission 0)" "$C" && pass "$name: the client's folder opened on Dam's report" || fail "$name: the client's report did not open"
	grep -q "gexfront: the online mission's report closed" "$C" && pass "$name: the client's two NEXTs closed its folder" || fail "$name: the client's folder did not close"
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
	grep -q "net: co-op: starting dataDyne Defection (Perfect Dark, index 0, stage 0x30) on Agent with 2 players" "$H" \
		&& pass "$name: host: $(grep -o 'co-op: starting.*' "$H" | head -1)" || fail "$name: the host did not start the mission"
	grep -q "net: co-op: the host's mission is dataDyne Defection (Perfect Dark, index 0, stage 0x30) on difficulty 0" "$C" \
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
		ge) case_ge ;;
		campaign) case_campaign ;;
		geend) case_geend ;;
		twelve) case_twelve ;;
		lobby) case_lobby ;;
		*) fail "unknown case $c" ;;
	esac
done
} 2>&1 | tee "$OUT/run.log"
grep -q '^FAIL' "$OUT/run.log" && status=1
echo "netcooptest: $(grep -c '^ok' "$OUT/run.log") ok, $(grep -c '^FAIL' "$OUT/run.log") failed"
exit $status
