#!/bin/bash
# netantitest.sh — counter-op online (protocol 32, CLAUDE-notes/netplay.md
# "Counter-op online"): a mission's agents against counter-ops in its guards.
#
# Loopback, offscreen on the GPU, a listen host and its clients on 127.0.0.1,
# the openings skipped. The sides come from --net-test-anti (a slot mask) in
# a --host session, from the room's teams in a lobby room:
#
#   pair3   dataDyne Defection, three players: the host and the second client
#           agents, the first client a counter-op. Every machine takes the same
#           sides (bond player 0, anti 1, coop 2); the host puts the counter-op
#           in a setup guard at the load and every client takes its copy of that
#           guard out of the world; the counter-op's body, head and place agree
#           on the host and both clients; no guard watches or targets it; killed
#           on the host, its START (the client's script) brings it back in
#           another guard on every machine; the host's objectives met end the
#           mission: the counter-op's end screen is the agents' win undone
#           ("failed"); prediction 95% or better on both clients.
#   ga      Defection with Guards Alerted! (the user, 2026-10-12: "counter-ops
#           should work with Guards Alerted! so players on the counter-ops side
#           can use the additional guard bodies"): the counter-op killed again
#           and again comes back in the mod's guards (run-time chrs) as well,
#           and none of them watches or targets it.
#   ge      GoldenEye's Dam: the counter-op's guard is a plain one (its body
#           GoldenEye's guard numbers, never a named character's), the client
#           in its body.
#   hostanti  the host the counter-op and the client the agent (bond a
#           client's seat): the sides on both, the host in a guard, the
#           agents' win the host's end screen "failed".
#   lobby   a local pdlobbyd: a room made as a Counter-Op Mission
#           (--net-lobby-anti), the joiner on the counter-ops' side
#           (--net-lobby-side 1): the room's summary says "Counter-Op Agent",
#           both take the sides from the room, the joiner plays in a guard,
#           the host's end at a frame brings both back to the room.
#   lobbyhost  a lobby host on the counter-ops' side, two agents, a room of
#           twelve (a player's crash, 2026-10-13): netCoopHostStart() sets the
#           sides before the stage changes and the Institute the room launched
#           from ticks once more, its one player the host's - a counter-op, so
#           no agent: the match ended at tick 0 and the end screen of the
#           Institute's solo player (MPINDEX_SOLO, past g_Menus[]) crashed. The
#           host runs under gdb: with the mission's stage asked for, the
#           Institute five seconds walked (300 frames, as the player had) and
#           netCoopAgentsGone() asked there - 0, else that stage's
#           mainEndStage() follows as it did; then the mission plays: the host
#           in a guard, GO, both agents see it, everyone back in the room.
#
#   netantitest.sh [BIN]   BIN a file name in build/ (pd.x86_64) or a path
#
# Env: BUILD (build/), OUT (build/netanti-out), PORT (27700; uses PORT .. PORT+11),
# CASES (pair3 ga ge hostanti lobby lobbyhost), MODDIR (mod_allinone, the lobby cases).
# Exit status: 0 all good, 1 a check failed.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=${BUILD:-$ROOT/build}
OUT=${OUT:-$BUILD/netanti-out}; PORT=${PORT:-27700}
CASES=${CASES:-pair3 ga ge hostanti lobby lobbyhost}
MODDIR=${MODDIR:-mod_allinone}
BIN=${1:-pd.x86_64}
case $BIN in /*) ;; */*) BIN=$(realpath "$BIN") ;; *) BIN=$BUILD/$BIN ;; esac
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
rm -f "$OUT"/*.log "$OUT"/*.script "$OUT"/stage.log
# no controller may reach a run (see replaytest.sh)
export SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-offscreen} SDL_GAMECONTROLLER_IGNORE_DEVICES=0x054c/0x0ce6,0x045e/0x028e \
	SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT=0x0000/0x0000 SDL_JOYSTICK_HIDAPI=0

{
status=0
prev=none
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

# stop every run of a case (a TERM is a clean exit: 143)
stopall() {
	local l p
	for l in "$@"; do
		p=$(gamepid "$l"); [ -n "$p" ] && kill -TERM "$p"
	done
}

# Staging through gdb: on the host between two ticks, on a client anywhere
cat > "$OUT/stage.py" <<'PY'
import gdb
def i(e): return int(gdb.parse_and_eval(e))
def f(e): return float(gdb.parse_and_eval(e))
def status(tag):
    out = []
    for pn in range(12):
        if not i("(long)g_Vars.players[%d]" % pn): break
        p = "g_Vars.players[%d]" % pn
        if not i("(long)%s->prop" % p) or not i("(long)%s->prop->chr" % p):
            out.append("p%d noprop" % pn); continue
        c = p + "->prop->chr"
        out.append("p%d dead %d body %d head %d at %.0f %.0f %.0f" % (pn, i(p+"->isdead") != 0, i(c+"->bodynum"), i(c+"->headnum"),
            f(p+"->prop->pos.x"), f(p+"->prop->pos.y"), f(p+"->prop->pos.z")))
    alive = 0
    for k in range(i("g_NumChrSlots")):
        c = "g_ChrSlots[%d]" % k
        if i(c+".chrnum") >= 0 and i("(long)"+c+".prop") and i(c+".prop->type") == 3 and i(c+".actiontype") not in (4, 5):
            alive += 1
    print("STAGE status %s tick %d mask 0x%x bond %d anti %d coop %d | %s | chrs %d" % (tag, i("g_NetTick"), i("g_CoopAntiMask"),
        i("g_Vars.bondplayernum"), i("g_Vars.antiplayernum"), i("g_Vars.coopplayernum"), " ; ".join(out), alive))
def targets(tag, pn):
    """how many living guards watch (p1p2) or target player pn"""
    watch = 0; target = 0; guards = 0
    for k in range(i("g_NumChrSlots")):
        c = "g_ChrSlots[%d]" % k
        if i(c+".chrnum") < 0 or not i("(long)"+c+".prop") or i(c+".prop->type") != 3 or i(c+".actiontype") in (4, 5): continue
        guards += 1
        if i(c+".p1p2") == pn: watch += 1
        t = i(c+".target")
        if t >= 0 and i("g_Vars.props[%d].type" % t) == 6 and i("playermgrGetPlayerNumByProp(&g_Vars.props[%d])" % t) == pn:
            target += 1
    print("STAGE targets %s tick %d: of %d guards %d watch player %d, %d target it" % (tag, i("g_NetTick"), guards, watch, pn, target))
def kill(pn):
    was = i("g_Vars.currentplayernum")
    gdb.execute("call (void)setCurrentPlayerNum(%d)" % pn)
    gdb.execute("call (void)playerDie(1)")
    gdb.execute("call (void)setCurrentPlayerNum(%d)" % was)
    print("STAGE kill: player %d killed at tick %d" % (pn, i("g_NetTick")))
def win():
    """the host's objectives all met (none left to meet) and the mission ended"""
    gdb.execute("set var g_ObjectiveLastIndex = -1")
    gdb.execute("call (void)mainEndStage()")
    print("STAGE win: the agents' objectives met, the mission ended at tick %d" % i("g_NetTick"))
def geguard(pn):
    """the GoldenEye character number of player pn's body"""
    print("STAGE geguard: player %d body %d is GoldenEye's character %d" % (pn, i("g_Vars.players[%d]->prop->chr->bodynum" % pn),
        i("gexPlusRomChrForRow(g_Vars.players[%d]->prop->chr->bodynum)" % pn)))
PY

stage() {
	local pid=$1 side=$2 what=$3
	if [ "$side" = host ]; then
		timeout 40 gdb -p "$pid" -batch -ex "source $OUT/stage.py" -ex "break netScenHostTickEnd" -ex "continue" \
			-ex "delete" -ex "python $what" 2>/dev/null | grep "^STAGE" | tee -a "$OUT/stage.log" | sed "s/^/     $side: /"
	else
		timeout 40 gdb -p "$pid" -batch -ex "source $OUT/stage.py" -ex "python $what" 2>/dev/null | grep "^STAGE" \
			| tee -a "$OUT/stage.log" | sed "s/^/     $side: /"
	fi
}

# a status line's player pn ("p1 dead 0 body 110 head 42 at x y z")
playerof() { grep -o "p$2 dead [01] body [-0-9]* head [-0-9]*" <<< "$1" | head -1; }

# the clients' START a few ticks in every 150 from tick 1500: the pause menu
# while alive, the respawn into the next guard when dead
for t in $(seq 1500 150 12000); do echo "$t $((t + 3)) 1000 0 0 0 0 0 0"; done > "$OUT/start.script"

predicts() {
	local name=$1 log=$2 pr pct compared
	pr=$(lastline "$log" "net: prediction ")
	compared=$(num "$pr" compared)
	pct=$(echo "$pr" | grep -o 'matched [0-9]* ([0-9.]*%)' | grep -o '[0-9.]*%' | tr -d %)
	if [ -n "$pct" ] && [ "${compared:-0}" -ge 200 ] && awk -v p="$pct" 'BEGIN { exit !(p >= 95) }'; then
		pass "$name: prediction matched $pct% of $compared ticks"
	else
		fail "$name: prediction: ${pr:-no summary}"
	fi
}

# ---------------------------------------------------------------- pair3
case_pair3() {
	local name=pair3 port=$PORT H=$OUT/pair3-host.log A=$OUT/pair3-anti.log G=$OUT/pair3-agent.log
	echo "== $name"
	: > "$OUT/stage.log"
	game pair3-host 400 '[Mod]\n' --host "$port" --net-test-host 2 --rng-seed 7 --net-test-coop 0 --net-test-anti 2 \
		--skip-cutscenes --exit-frame 12000 &
	waitfor "$H" "net: hosting on UDP port" 90 || { fail "$name: host did not start"; stopall pair3-host; wait; return; }
	game pair3-anti 390 '[Mod]\n' --connect "127.0.0.1:$port" --net-test-join --exit-frame 11000 --net-test-input "$OUT/start.script" &
	sleep 2
	game pair3-agent 390 '[Mod]\n' --connect "127.0.0.1:$port" --net-test-join --exit-frame 11000 &
	local hp ap gp
	if waitfor "$A" "net: match 1: GO" 150 && waitfor "$G" "net: match 1: GO" 60; then
		sleep 12
		hp=$(gamepid pair3-host); ap=$(gamepid pair3-anti); gp=$(gamepid pair3-agent)
		stage "$hp" host "status('host')"
		stage "$ap" anti "status('anti')"
		stage "$gp" agent "status('agent')"
		stage "$hp" host "targets('host', 1)"
		stage "$hp" host "kill(1)"
		# the death's fade, then the script's START
		waitfor "$A" "player 1 in a guard's body .*(spawn 2" 60 || echo "     no second guard on the counter-op's machine within 60 s"
		sleep 3
		stage "$hp" host "status('host')"
		stage "$ap" anti "status('anti')"
		stage "$gp" agent "status('agent')"
		stage "$hp" host "targets('host', 1)"
		stage "$hp" host "win()"
		waitfor "$A" "end screen" 30
		sleep 3
	else
		echo "     a client did not pass GO: no staging"
	fi
	stopall pair3-anti pair3-agent pair3-host
	wait

	local c
	for c in "$H" "$A" "$G"; do crashed "$c" && fail "$name: $(basename "$c" .log) crashed"; done
	for c in "$H" "$A" "$G"; do
		grep -q "net: counter-op: 3 players, counter-ops 0x002: bond is player 0, anti 1, coop 2" "$c" \
			&& pass "$name: $(basename "$c" .log): the sides (bond 0, anti 1, coop 2)" \
			|| fail "$name: $(basename "$c" .log): sides: $(grep -o 'counter-op: [0-9]* players.*' "$c" | head -1)"
	done
	grep -q "slot 1 (\"[^\"]*\") loaded the stage, every component the host's" "$H" && grep -q "slot 2 (\"[^\"]*\") loaded the stage, every component the host's" "$H" \
		&& pass "$name: every stage hash component equal on both clients (the host's guard choice moved no shared random)" \
		|| fail "$name: stage hashes differ"
	local taken; taken=$(sed -n 's/.*counter-op: player 1 takes setup chr \([0-9]*\) at tick 0 .*/\1/p' "$H" | head -1)
	[ -n "$taken" ] && pass "$name: the host put the counter-op in setup chr $taken at the load" || fail "$name: no guard taken at the load"
	for c in "$A" "$G"; do
		grep -q "setup chr ${taken:-x} taken by a counter-op: out of the world here" "$c" \
			&& pass "$name: $(basename "$c" .log): its copy of chr $taken out of the world" \
			|| fail "$name: $(basename "$c" .log): its copy of chr ${taken:-?} stayed"
	done
	local s1h s1a s1g
	s1h=$(grep "STAGE status host" "$OUT/stage.log" | sed -n 1p); s1a=$(grep "STAGE status anti" "$OUT/stage.log" | sed -n 1p)
	s1g=$(grep "STAGE status agent" "$OUT/stage.log" | sed -n 1p)
	[ -n "$(playerof "$s1h" 1)" ] && [ "$(playerof "$s1h" 1)" = "$(playerof "$s1a" 1)" ] && [ "$(playerof "$s1h" 1)" = "$(playerof "$s1g" 1)" ] \
		&& pass "$name: the counter-op's guard alike on all three: $(playerof "$s1h" 1)" \
		|| fail "$name: the counter-op: host '$(playerof "$s1h" 1)' anti '$(playerof "$s1a" 1)' agent '$(playerof "$s1g" 1)'"
	[ -n "$(num "$s1h" chrs)" ] && [ "$(num "$s1h" chrs)" = "$(num "$s1a" chrs)" ] && [ "$(num "$s1h" chrs)" = "$(num "$s1g" chrs)" ] \
		&& pass "$name: $(num "$s1h" chrs) guards standing on every machine" \
		|| fail "$name: guards standing: host $(num "$s1h" chrs) anti $(num "$s1a" chrs) agent $(num "$s1g" chrs)"
	local tg bad=0
	while read -r tg; do
		echo "$tg" | grep -q " 0 watch player 1, 0 target it" || bad=1
	done < <(grep "STAGE targets host" "$OUT/stage.log")
	[ "$bad" = 0 ] && [ -n "$(grep 'STAGE targets host' "$OUT/stage.log")" ] && pass "$name: no guard watches or targets the counter-op" \
		|| fail "$name: guards after the counter-op: $(grep 'STAGE targets host' "$OUT/stage.log" | tr '\n' ' ')"
	local t2; t2=$(grep -c "counter-op: player 1 takes" "$H")
	[ "${t2:-0}" -ge 2 ] && pass "$name: killed, it came back in another guard: $(grep -o 'counter-op: player 1 takes.*' "$H" | sed -n 2p)" \
		|| fail "$name: the counter-op's guards on the host: ${t2:-0}"
	for c in "$A" "$G"; do
		grep -q "player 1 in a guard's body [0-9]* head [0-9]* (spawn 2" "$c" || grep -q "player 1 in a guard's body .*(spawn 1" "$c" \
			&& pass "$name: $(basename "$c" .log): $(grep -o "player 1 in a guard's body.*" "$c" | tail -1)" \
			|| fail "$name: $(basename "$c" .log): never put player 1 in a guard"
	done
	local s2h s2a
	s2h=$(grep "STAGE status host" "$OUT/stage.log" | sed -n 2p); s2a=$(grep "STAGE status anti" "$OUT/stage.log" | sed -n 2p)
	[ -n "$(playerof "$s2h" 1)" ] && [ "$(playerof "$s2h" 1)" = "$(playerof "$s2a" 1)" ] && echo "$(playerof "$s2h" 1)" | grep -q "dead 0" \
		&& pass "$name: alive in its new guard on both: $(playerof "$s2h" 1)" \
		|| fail "$name: after the respawn: host '$(playerof "$s2h" 1)' anti '$(playerof "$s2a" 1)'"
	grep -q "net: counter-op: player 1's end screen: failed (the agents completed)" "$A" \
		&& pass "$name: the agents' win is the counter-op's end screen \"failed\"" \
		|| fail "$name: the counter-op's end screen: $(grep -o 'end screen: .*' "$A" | head -1)"
	grep -q "counter-op: player [0-9]*'s end screen" "$G" && fail "$name: the agent had a counter-op's end screen" || pass "$name: the agent's end screen is an agent's"
	predicts "$name: anti" "$A"
	predicts "$name: agent" "$G"
}

# ---------------------------------------------------------------- ga
case_ga() {
	local name=ga port=$((PORT + 2)) H=$OUT/ga-host.log A=$OUT/ga-anti.log
	local ini='[Mod]\nGuardsAlerted=1\nAlertedGuards=12\nGuardSpawnSpeed=20\n'
	echo "== $name"
	: > "$OUT/stage.log"
	game ga-host 420 "$ini" --host "$port" --net-test-host 1 --rng-seed 7 --net-test-coop 0 --net-test-anti 2 \
		--skip-cutscenes --net-test-invincible --exit-frame 14000 &
	waitfor "$H" "net: hosting on UDP port" 90 || { fail "$name: host did not start"; stopall ga-host; wait; return; }
	game ga-anti 410 "$ini" --connect "127.0.0.1:$port" --net-test-join --exit-frame 13000 --net-test-input "$OUT/start.script" &
	local hp k
	if waitfor "$A" "net: match 1: GO" 150; then
		sleep 10
		hp=$(gamepid ga-host)
		for k in 1 2 3 4 5 6; do
			stage "$hp" host "kill(1)"
			waitfor "$A" "player 1 in a guard's body .*(spawn $((k + 1))" 30 || true
			sleep 2
			grep -q "takes chr [0-9]* (spawned, Guards Alerted!)" "$H" && break
		done
		stage "$hp" host "targets('host', 1)"
	else
		echo "     the client did not pass GO: no staging"
	fi
	stopall ga-anti ga-host
	wait
	crashed "$H" && fail "$name: the host crashed"; crashed "$A" && fail "$name: the client crashed"
	grep -q "takes chr [0-9]* (spawned, Guards Alerted!)" "$H" \
		&& pass "$name: a Guards Alerted! guard taken: $(grep -o 'counter-op: player 1 takes chr [0-9]* (spawned, Guards Alerted!).*' "$H" | head -1)" \
		|| fail "$name: no Guards Alerted! guard taken: $(grep -o 'counter-op: player 1 takes.*' "$H" | tr '\n' ' ')"
	grep "STAGE targets host" "$OUT/stage.log" | tail -1 | grep -q " 0 watch player 1, 0 target it" \
		&& pass "$name: no guard (the mod's either) watches or targets the counter-op: $(grep -o 'of [0-9]* guards.*' "$OUT/stage.log" | tail -1)" \
		|| fail "$name: guards after the counter-op: $(grep 'STAGE targets host' "$OUT/stage.log" | tail -1)"
	predicts "$name" "$A"
}

# ---------------------------------------------------------------- ge
case_ge() {
	local name=ge port=$((PORT + 4)) H=$OUT/ge-host.log A=$OUT/ge-anti.log
	echo "== $name"
	: > "$OUT/stage.log"
	game ge-host 400 '[Mod]\nMapMods=GoldenEye Arenas\n' --host "$port" --net-test-host 1 --rng-seed 7 --net-test-coop 0 \
		--net-test-coop-game ge --net-test-anti 2 --skip-cutscenes --exit-frame 6000 &
	waitfor "$H" "net: hosting on UDP port" 90 || { fail "$name: host did not start"; stopall ge-host; wait; return; }
	game ge-anti 390 '[Mod]\nMapMods=GoldenEye Arenas\n' --connect "127.0.0.1:$port" --net-test-join --exit-frame 5500 &
	local hp ap
	if waitfor "$A" "net: match 1: GO" 180; then
		sleep 10
		hp=$(gamepid ge-host); ap=$(gamepid ge-anti)
		stage "$hp" host "status('host'); geguard(1)"
		stage "$ap" anti "status('anti')"
	else
		echo "     the client did not pass GO: no staging"
	fi
	stopall ge-anti ge-host
	wait
	crashed "$H" && fail "$name: the host crashed"; crashed "$A" && fail "$name: the client crashed"
	if grep -q "not converted here\|cannot be played online" "$H"; then
		echo "skip $name: GoldenEye is not converted here"; return
	fi
	grep -q "net: co-op: starting Dam (GoldenEye, index 0, stage 0x15) on Agent with 2 players, .*counter-op" "$H" \
		&& pass "$name: the host started Dam as counter-op" || fail "$name: no counter-op Dam: $(grep -o 'co-op: starting.*' "$H" | head -1)"
	local gc; gc=$(sed -n 's/.*is GoldenEye.s character \([-0-9]*\).*/\1/p' "$OUT/stage.log" | tail -1)
	case " 0 1 2 3 4 17 18 19 20 21 37 38 39 40 " in
		*" ${gc:-x} "*) pass "$name: the counter-op is in a plain guard's body (GoldenEye's character $gc)" ;;
		*) fail "$name: the counter-op's body is GoldenEye's character ${gc:-?}" ;;
	esac
	local sh sa
	sh=$(grep "STAGE status host" "$OUT/stage.log" | tail -1); sa=$(grep "STAGE status anti" "$OUT/stage.log" | tail -1)
	[ -n "$(playerof "$sh" 1)" ] && [ "$(playerof "$sh" 1)" = "$(playerof "$sa" 1)" ] \
		&& pass "$name: the client in the same guard: $(playerof "$sh" 1)" \
		|| fail "$name: the counter-op: host '$(playerof "$sh" 1)' client '$(playerof "$sa" 1)'"
}

# ---------------------------------------------------------------- hostanti
case_hostanti() {
	local name=hostanti port=$((PORT + 6)) H=$OUT/hostanti-host.log G=$OUT/hostanti-agent.log
	echo "== $name"
	: > "$OUT/stage.log"
	game hostanti-host 300 '[Mod]\n' --host "$port" --net-test-host 1 --rng-seed 7 --net-test-coop 0 --net-test-anti 1 \
		--skip-cutscenes --exit-frame 8000 &
	waitfor "$H" "net: hosting on UDP port" 90 || { fail "$name: host did not start"; stopall hostanti-host; wait; return; }
	game hostanti-agent 290 '[Mod]\n' --connect "127.0.0.1:$port" --net-test-join --exit-frame 7500 &
	local hp gp
	if waitfor "$G" "net: match 1: GO" 150; then
		sleep 8
		hp=$(gamepid hostanti-host); gp=$(gamepid hostanti-agent)
		stage "$hp" host "status('host')"
		stage "$gp" agent "status('agent')"
		stage "$hp" host "targets('host', 0)"
		stage "$hp" host "win()"
		waitfor "$H" "end screen" 30
		sleep 2
	else
		echo "     the client did not pass GO: no staging"
	fi
	stopall hostanti-agent hostanti-host
	wait
	crashed "$H" && fail "$name: the host crashed"; crashed "$G" && fail "$name: the client crashed"
	for c in "$H" "$G"; do
		grep -q "net: counter-op: 2 players, counter-ops 0x001: bond is player 1, anti 0" "$c" \
			&& pass "$name: $(basename "$c" .log): bond is the client's player, the host the counter-op" \
			|| fail "$name: $(basename "$c" .log): sides: $(grep -o 'counter-op: [0-9]* players.*' "$c" | head -1)"
	done
	local sh sg
	sh=$(grep "STAGE status host" "$OUT/stage.log" | tail -1); sg=$(grep "STAGE status agent" "$OUT/stage.log" | tail -1)
	[ -n "$(playerof "$sh" 0)" ] && [ "$(playerof "$sh" 0)" = "$(playerof "$sg" 0)" ] \
		&& pass "$name: the host's player in a guard on both: $(playerof "$sh" 0)" \
		|| fail "$name: the host's player: host '$(playerof "$sh" 0)' client '$(playerof "$sg" 0)'"
	grep "STAGE targets host" "$OUT/stage.log" | tail -1 | grep -q " 0 watch player 0, 0 target it" \
		&& pass "$name: no guard watches or targets the host's counter-op" || fail "$name: $(grep 'STAGE targets host' "$OUT/stage.log" | tail -1)"
	grep -q "net: counter-op: player 0's end screen: failed (the agents completed)" "$H" \
		&& pass "$name: the host's end screen \"failed\" at the agents' win" \
		|| fail "$name: the host's end screen: $(grep -o 'end screen: .*' "$H" | head -1)"
	predicts "$name" "$G"
}

# ---------------------------------------------------------------- lobby
case_lobby() {
	local name=lobby port=$((PORT + 8)) L=$OUT/lobby-pdlobbyd.log H=$OUT/lobby-host.log J=$OUT/lobby-join.log
	echo "== $name"
	: > "$OUT/stage.log"
	python3 -u "$ROOT/tools/pdlobbyd/pdlobbyd.py" --host 127.0.0.1 --port 0 --udp-host 127.0.0.1 --udp-port 0 --auth open --relay-ports 0 \
		> "$L" 2>&1 &
	local lobby=$!
	if ! waitfor "$L" "pdlobbyd listening on" 20; then
		fail "$name: pdlobbyd did not start"; kill $lobby 2>/dev/null; return
	fi
	local lport; lport=$(sed -n 's/.*listening on 127.0.0.1:\([0-9]*\).*/\1/p' "$L" | head -1)
	local ini="[Mod]\nGhostUser=%s\nGhostPin=1234\n[Net]\nLobbyServer=http://127.0.0.1:$lport\nPort=$port\n"
	# shellcheck disable=SC2059
	game lobby-host 300 "$(printf "$ini" antihost)" --moddir "$MODDIR" --net-lobby-script host --net-lobby-room "Anti Test" \
		--net-lobby-coop 0 --net-lobby-anti --net-lobby-end-frame 1500 --skip-cutscenes --rng-seed 7 &
	local p1=$!
	# shellcheck disable=SC2059
	game lobby-join 300 "$(printf "$ini" antijoiner)" --moddir "$MODDIR" --net-lobby-script join --net-lobby-room "Anti Test" \
		--net-lobby-side 1 --net-lobby-leave-frame 0 --skip-cutscenes &
	local p2=$!
	wait $p1; local r1=$?; wait $p2; local r2=$?
	kill $lobby 2>/dev/null; wait $lobby 2>/dev/null
	local room; room=$(sed -n 's/.*lobby: made room \([0-9a-f]*\).*/\1/p' "$H" | head -1)
	[ -n "$room" ] && pass "$name: the host made room $room" || { fail "$name: the host made no room"; return; }
	grep -q "Counter-Op Agent" "$J" "$H" "$L" && pass "$name: the room's summary says Counter-Op Agent" || fail "$name: no 'Counter-Op Agent' in the room's summary"
	grep -q "lobby: joined room $room" "$J" && pass "$name: the joiner joined" || fail "$name: the joiner did not join"
	for c in "$H" "$J"; do
		grep -q "net: counter-op: 2 players, counter-ops 0x002: bond is player 0, anti 1" "$c" \
			&& pass "$name: $(basename "$c" .log): the joiner's room team its side (counter-ops 0x002)" \
			|| fail "$name: $(basename "$c" .log): sides: $(grep -o 'counter-op: [0-9]* players.*' "$c" | head -1)"
	done
	grep -q "counter-op client: player 1 in a guard's body" "$J" && pass "$name: the joiner plays in a guard" || fail "$name: the joiner never in a guard"
	grep -q "lobby: back in room $room's lobby" "$H" && pass "$name: the host is back in the room" || fail "$name: the host is not back in the room"
	grep -q "lobby: back in room $room's lobby" "$J" && pass "$name: the joiner is back in the room" || fail "$name: the joiner is not back in the room"
	crashed "$H" && fail "$name: the host crashed"; crashed "$J" && fail "$name: the joiner crashed"
	[ "$r1" = 0 ] && [ "$r2" = 0 ] && pass "$name: exits 0 0" || fail "$name: exits host $r1 join $r2"
}

# ---------------------------------------------------------------- lobbyhost
case_lobbyhost() {
	local name=lobbyhost port=$((PORT + 9)) L=$OUT/lobbyhost-pdlobbyd.log H=$OUT/lobbyhost-host.log
	local A=$OUT/lobbyhost-agent1.log B=$OUT/lobbyhost-agent2.log
	echo "== $name"
	python3 -u "$ROOT/tools/pdlobbyd/pdlobbyd.py" --host 127.0.0.1 --port 0 --udp-host 127.0.0.1 --udp-port 0 --auth open --relay-ports 0 \
		> "$L" 2>&1 &
	local lobby=$!
	if ! waitfor "$L" "pdlobbyd listening on" 20; then
		fail "$name: pdlobbyd did not start"; kill $lobby 2>/dev/null; return
	fi
	local lport; lport=$(sed -n 's/.*listening on 127.0.0.1:\([0-9]*\).*/\1/p' "$L" | head -1)
	local ini="[Mod]\nGhostUser=%s\nGhostPin=1234\n[Net]\nLobbyServer=http://127.0.0.1:$lport\nPort=%s\n"
	# the host as the player was: an agent file chosen (a copy of this
	# machine's save, as netlobbytest.sh), the Online Game from the Perfect
	# Menu, under gdb for the start's one frame on the Institute
	local save=$OUT/save-lobbyhost-host f
	rm -rf "$save"; mkdir -p "$save"
	for f in eeprom.bin mpsetups.bin; do
		[ -f "$HOME/.local/share/perfectdark/$f" ] && cp "$HOME/.local/share/perfectdark/$f" "$save/"
	done
	# shellcheck disable=SC2059
	printf "$ini" antihost "$port" > "$save/pd.ini"
	cat > "$OUT/lobbyhost.gdb" <<'G'
set pagination off
set confirm off
handle SIGPIPE nostop noprint
handle SIGUSR1 nostop noprint
set $started = 0
break netCoopHostStart
commands
silent
set $started = 1
continue
end
break menuStop if $started == 1 && g_MainChangeToStageNum >= 0
commands
silent
set $started = 2
set var g_Vars.lvframenum = 300
set $gone = (int)netCoopAgentsGone()
printf "STAGE oldstage: stage 0x%x with 0x%x asked for, 300 frames walked, its player 0 mpindex %d: netCoopAgentsGone() %d\n", g_StageNum, g_MainChangeToStageNum, g_Vars.playerstats[0].mpindex, $gone
if $gone
  call (void)mainEndStage()
end
continue
end
run
G
	( cd "$BUILD" && exec timeout -k 5 300 gdb -batch -x "$OUT/lobbyhost.gdb" --args "$BIN" --savedir "$save" --skip-intro --no-sound \
		--moddir "$MODDIR" --profile 0 --net-lobby-menus --net-lobby-script host --net-lobby-room "Host Anti" --net-lobby-coop 0 \
		--net-lobby-anti --net-lobby-side 1 --net-lobby-size 12 --net-lobby-wait 2 --net-lobby-end-frame 1500 --skip-cutscenes \
		--rng-seed 7 > "$H" 2>&1 ) &
	local p1=$!
	sleep 3
	# shellcheck disable=SC2059
	game lobbyhost-agent1 290 "$(printf "$ini" agentone $((port + 1)))" --moddir "$MODDIR" --net-lobby-script join \
		--net-lobby-room "Host Anti" --net-lobby-side 0 --net-lobby-leave-frame 0 --skip-cutscenes &
	local p2=$!
	# shellcheck disable=SC2059
	game lobbyhost-agent2 290 "$(printf "$ini" agenttwo $((port + 2)))" --moddir "$MODDIR" --net-lobby-script join \
		--net-lobby-room "Host Anti" --net-lobby-side 0 --net-lobby-leave-frame 0 --skip-cutscenes &
	local p3=$!
	wait $p1 $p2 $p3
	kill $lobby 2>/dev/null; wait $lobby 2>/dev/null
	local room; room=$(sed -n 's/.*lobby: made room \([0-9a-f]*\).*/\1/p' "$H" | head -1)
	[ -n "$room" ] && pass "$name: the host made room $room" || { fail "$name: the host made no room"; return; }
	grep -q "STAGE oldstage: stage 0x26 with 0x30 asked for.*netCoopAgentsGone() 0" "$H" \
		&& pass "$name: on the Institute, the mission asked for: no agents judged gone" \
		|| fail "$name: on the old stage: $(grep -o 'STAGE oldstage.*' "$H" | head -1)"
	grep -q "no agent is left in the room: the mission is the counter-ops' (tick 0)" "$H" \
		&& fail "$name: the match ended at tick 0 with no agent" || pass "$name: no end at tick 0"
	grep -q "net: counter-op: 12 players, counter-ops 0x001: bond is player 1, anti 0, coop 2" "$H" \
		&& pass "$name: twelve seats, the host the counter-op" || fail "$name: sides: $(grep -o 'counter-op: [0-9]* players.*' "$H" | head -1)"
	grep -q "net: counter-op: player 0 takes setup chr" "$H" && grep -q "every machine has loaded; GO" "$H" \
		&& pass "$name: the mission loaded: the host in a guard, GO" || fail "$name: the host never in a guard or no GO"
	for c in "$A" "$B"; do
		grep -q "counter-op client: player 0 in a guard's body" "$c" \
			&& pass "$name: $(basename "$c" .log) sees the host in a guard" || fail "$name: $(basename "$c" .log) never saw the host in a guard"
		grep -q "lobby: back in room $room's lobby" "$c" && pass "$name: $(basename "$c" .log) back in the room" \
			|| fail "$name: $(basename "$c" .log) is not back in the room"
	done
	grep -q "lobby: back in room $room's lobby" "$H" && pass "$name: the host back in the room" || fail "$name: the host is not back in the room"
	for c in "$H" "$A" "$B"; do
		{ crashed "$c" || grep -q "received signal SIGSEGV" "$c"; } && fail "$name: $(basename "$c" .log) crashed"
	done
}

for c in $CASES; do
	[ -s "$OUT/stage.log" ] && cp "$OUT/stage.log" "$OUT/stage-$prev.log"
	prev=$c
	case $c in
		pair3) case_pair3 ;;
		ga) case_ga ;;
		ge) case_ge ;;
		hostanti) case_hostanti ;;
		lobby) case_lobby ;;
		lobbyhost) case_lobbyhost ;;
		*) fail "unknown case $c" ;;
	esac
done
[ -s "$OUT/stage.log" ] && cp "$OUT/stage.log" "$OUT/stage-$prev.log"
exit $status
} 2>&1 | tee "$OUT/run.log"
grep -q '^FAIL' "$OUT/run.log" && exit 1
echo "netantitest: $(grep -c '^ok' "$OUT/run.log") ok, $(grep -c '^FAIL' "$OUT/run.log") failed"
exit 0
