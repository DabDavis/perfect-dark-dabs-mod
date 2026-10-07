#!/bin/bash
# netcontenttest.sh — content breadth online (PLANS/NETPLAY.md phase 7b):
# GoldenEye's arenas, guns and scenarios, the Stage Loader's mod maps, an
# overlay mod's arena, the XBLA look as one machine's own, and the props
# phase 4 left unexercised (Mod.Bodies corpses, hats, lifts, glass, rockets,
# grenades and mines, the N-bomb and gas, ammo crates).
#
# Loopback: for each case a listen host and one client on 127.0.0.1,
# offscreen on the GPU, with sims (the players kept alive by the sims'
# shots: --net-test-god --net-test-invincible, so the client's view is of the
# match; only the staging kills them). The client runs 2400 frames (the match
# 1500+ host ticks of it) and quits; the host carries on 600 more. Checks:
#
#   - both ran to the end, no crash, never llvmpipe;
#   - the client loaded the stage the host's key named (the key's kind and
#     what it named), every stage hash component the host's;
#   - the puppets and events sane (no malformed snapshot or event, no bad
#     anim, no descriptor that did not fit), 1500+ match ticks on the client;
#   - the case's own feature seen on the client (per case below), counted
#     only where the client's code acted on it (an event dropped as
#     unresolved is not "applied");
#   - a screenshot of the client per case in build/net-shots/content-*.png.
#
# What the sims would not do in the time is staged on the host through gdb
# (stage.py): hats put on the sims and knocked off (GoldenEye's fur hats,
# which have no bbox, are left on: dropping one crashes offline too), a loose
# ammo crate (netTestLooseCrate), the glass furthest from the client broken
# out of its scope, the N-bomb, gas, and You Only Live Twice's deaths (sim 0
# and the client's player killed twice over; the client presses START, and
# nothing else, every second to ask for its respawn: a client's START is its
# own pause menu while it lives, a respawn on its death screen, as offline).
#
# A refusal case (gemust, modmissing) instead checks the client was refused
# with the key, or the map and its mod, named in the text it was given.
#
#   netcontenttest.sh [BIN]   BIN a file name in build/ (pd.x86_64) or a path
#
# Env: OUT (build/netcontent-out), PORT (27300), CASES (all: ge geyolt gegg
# gemust xbla modmap modmissing overlay props), CHECKONLY (1: only the checks,
# on the last run's files). The GoldenEye cases need the GoldenEye ROM
# converted (mods/GoldenEye Arenas); they are skipped, not failed, without it.
# Exit status: 0 all good, 1 a check failed, 2 a run failed to start.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=${BUILD:-$ROOT/build}
OUT=${OUT:-$BUILD/netcontent-out}; PORT=${PORT:-27300}
CASES=${CASES:-ge geyolt gegg gemust xbla modmap modmissing overlay props}
BIN=${1:-pd.x86_64}
case $BIN in /*) ;; */*) BIN=$(realpath "$BIN") ;; *) BIN=$BUILD/$BIN ;; esac
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
SHOTDIR=$BUILD/net-shots
FRAMES=2400
export SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-offscreen} SDL_GAMECONTROLLER_IGNORE_DEVICES=0x054c/0x0ce6,0x045e/0x028e \
	SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT=0x0000/0x0000 SDL_JOYSTICK_HIDAPI=0

status=0
fail() { echo "FAIL $*"; status=1; }
pass() { echo "ok   $*"; }

GEMAPS="ModDir=\nMapMods=GoldenEye Arenas\n"

# game LABEL TIMEOUT INI ARGS...
game() {
	local label=$1 t=$2 ini=$3; shift 3
	local save=$OUT/save-$label
	rm -rf "$save"; mkdir -p "$save"
	printf '[Mod]\n%b' "$ini" > "$save/pd.ini"
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

# shot1 PID FILE: one screenshot from that game into FILE
shot1() {
	local pid=$1 dest=$2 before f i
	rm -f "$dest"
	mkdir -p "$BUILD/screenshots" "$SHOTDIR"
	before=$(ls "$BUILD/screenshots" 2>/dev/null | sort)
	gdb -p "$pid" -batch -ex 'call (void)screenshotRequest()' >/dev/null 2>&1
	for i in $(seq 50); do
		f=$(comm -13 <(echo "$before") <(ls "$BUILD/screenshots" 2>/dev/null | sort) | head -1)
		[ -n "$f" ] && { sleep 0.3; cp "$BUILD/screenshots/$f" "$dest"; echo "     shot: $dest"; return; }
		sleep 0.1
	done
	echo "     shot: none from $pid"
}

# Host staging, between two ticks: what the sims would not do in the time
cat > "$OUT/stage.py" <<'PY'
import gdb, math
def i(e): return int(gdb.parse_and_eval(e))
def remote():
    for k in range(4):
        if i("(long)g_Vars.players[%d]" % k) and (i("g_Vars.playerstats[%d].mpindex" % k) & 3) == 1:
            return k
    return -1
def hats():
    """a GoldenEye hat on each sim (side caps, berets, a fur hat), as a
    mission's setup puts them on its guards"""
    n = 0
    models = [0x200 + 215, 0x200 + 220, 0x200 + 212, 0x200 + 216]
    for b in range(i("g_BotCount")):
        c = "g_MpBotChrPtrs[%d]" % b
        if not i("(long)" + c) or i("(long)%s->weapons_held[2]" % c) or not i("(long)%s->model" % c): continue
        if i("(long)hatCreateForChr(%s, %d, 0)" % (c, models[b % len(models)])): n += 1
    print("STAGE hats: %d sims wear a hat" % n)
def nbomb():
    """an N-bomb's storm beside the client's player, a sim's"""
    pn = remote()
    if pn < 0 or not i("g_BotCount"): print("STAGE nbomb: nobody"); return
    gdb.execute("call (void)nbombCreateStorm(&g_Vars.players[%d]->prop->pos, g_MpBotChrPtrs[0]->prop)" % pn)
    print("STAGE nbomb: a storm at player %d" % pn)
def gas():
    """gas let go where the client's player stands (no arena has a gas
    bottle: a mission's would). Only on a stage with a second sky for the
    gas to fade to (a GoldenEye arena's fog): gasTick reads it unchecked."""
    pn = remote()
    if pn < 0: print("STAGE gas: nobody"); return
    if not i("(long)g_EnvTransitionFrom"): print("STAGE gas: this stage has no sky for gas"); return
    gdb.execute("call (void)gasReleaseFromPos(&g_Vars.players[%d]->prop->pos)" % pn)
    print("STAGE gas: released at player %d" % pn)
def props():
    """every prop on the host's lists, active then paused"""
    p = gdb.parse_and_eval("g_Vars.activeprops")
    if not int(p): p = gdb.parse_and_eval("g_Vars.pausedprops")
    n = 0
    while int(p) and n < 4096:
        yield p
        p = p["next"]; n += 1
def hatdrop():
    """each sim's hat knocked off, as a shot to the hat does (chraction.c)"""
    n = 0
    for b in range(i("g_BotCount")):
        c = "g_MpBotChrPtrs[%d]" % b
        if not i("(long)" + c) or not i("(long)%s->weapons_held[2]" % c): continue
        h = "%s->weapons_held[2]->obj" % c
        print("STAGE hatdrop: sim %d hat model 0x%x bbox %x type %d" % (b, i(h + "->modelnum"), i("(long)objFindBboxRodata(%s)" % h), i("hatGetType(%s->weapons_held[2])" % c)))
        if not i("(long)objFindBboxRodata(%s)" % h): continue
        gdb.execute("call (void)objSetDropped(%s->weapons_held[2], 4)" % c)
        gdb.execute("set var %s->hidden = %s->hidden | 1" % (c, c))
        n += 1
    print("STAGE hatdrop: %d hats knocked off" % n)
def crate():
    """a loose ammo crate out of the multi-ammo crate nearest the client's player"""
    pn = remote()
    print("STAGE crate: %s" % ("made" if pn >= 0 and i("(long)netTestLooseCrate(%d)" % pn) else "none"))
def glassfar():
    """the glass furthest from the client's player broken: out of its scope,
    so the client finds its own by the event's setup command"""
    pn = remote()
    if pn < 0: print("STAGE glassfar: nobody"); return
    me = gdb.parse_and_eval("g_Vars.players[%d]->prop->pos" % pn)
    best = None; bd = -1
    for p in props():
        if int(p["type"]) != 1 or not int(p["obj"]): continue  # PROPTYPE_OBJ
        o = p["obj"].dereference()
        if int(o["type"]) not in (0x2a, 0x2f) or not int(o["model"]): continue  # GLASS, TINTEDGLASS
        if int(o["damage"]) >= int(o["maxdamage"]): continue
        d = (float(p["pos"]["x"]) - float(me["x"])) ** 2 + (float(p["pos"]["z"]) - float(me["z"])) ** 2
        if d > bd: best = p; bd = d
    if best is None: print("STAGE glassfar: no glass standing"); return
    gdb.execute("call (void)glassDestroy((struct defaultobj *)%d)" % int(best["obj"]))
    print("STAGE glassfar: glass %.0f units from player %d broken" % (math.sqrt(bd), pn))
def kill():
    """You Only Live Twice: sim 0 and the client's player killed (twice over
    two steps is out); a dead one is left be"""
    out = []
    c = "g_MpBotChrPtrs[0]"
    if i("g_BotCount") and i("(long)" + c) and i("%s->actiontype" % c) not in (4, 5):  # ACT_DIE, ACT_DEAD
        gdb.execute("call (void)chrDamageByMisc(%s, 1000.0f, &%s->prop->pos, 0, 0)" % (c, c))
        out.append("sim 0")
    pn = remote()
    if pn >= 0 and not i("g_Vars.players[%d]->isdead" % pn):
        was = i("g_Vars.currentplayernum")
        gdb.execute("call (void)setCurrentPlayerNum(%d)" % pn)
        gdb.execute("call (void)playerDie(1)")
        gdb.execute("call (void)setCurrentPlayerNum(%d)" % was)
        out.append("player %d" % pn)
    print("STAGE kill: %s" % (", ".join(out) or "nobody alive to kill"))
PY

# the client's START pressed a tick in every sixty: a dead player asks the
# host to get it up again (player.c's respawn press reads A, Z or START; a
# client's START reaches the host only while its player is dead there, and
# opens and shuts its own pause menu while it lives). START alone, so the
# first death's respawn, which the second kill needs, is START's
for t in $(seq 60 60 3000); do echo "$t $((t + 1)) 1000 0 0 0 0 0 0"; done > "$OUT/respawn.script"

stage() {
	local hp=$1 what=$2
	timeout 30 gdb -p "$hp" -batch -ex "source $OUT/stage.py" -ex "break netScenHostTickEnd" -ex "continue" \
		-ex "delete" -ex "python $what" 2>/dev/null | grep "^STAGE" | sed 's/^/     host: /'
}

# runcase NAME HOSTINI CLIENTINI "HOSTARGS" "CLIENTARGS" STAGING [REFUSAL]:
# STAGING (a|b, five seconds apart) on the host ten seconds into the match;
# REFUSAL 1: the client is to be refused, so no staging and no shot
runcase() {
	local name=$1 hini=$2 cini=$3 hargs=$4 cargs=$5 staging=${6:-} refusal=${7:-0}
	local port=$((PORT + ${#done_cases}))
	rm -f "$OUT/$name-"*
	echo "== $name"
	# shellcheck disable=SC2086
	game "$name-host" 400 "$hini" --host "$port" --net-test-host 1 --rng-seed 7 --net-test-sims ${SIMS:-4} --net-test-god --net-test-invincible \
		--exit-frame $((FRAMES + 600)) $hargs &
	local host=$!
	waitfor "$OUT/$name-host.log" "net: hosting on UDP port" 90 || { echo "FAIL: host did not start"; kill -TERM $host; wait $host; return; }
	# shellcheck disable=SC2086
	game "$name-client" 390 "$cini" --connect "127.0.0.1:$port" --net-test-join --exit-frame "$FRAMES" $cargs &
	local client=$!
	local hp cp n
	if [ "$refusal" = 0 ] && waitfor "$OUT/$name-client.log" "net: match 1: GO" 150; then
		for n in $(seq 20); do cp=$(gamepid "$name-client"); [ -n "$cp" ] && break; sleep 0.5; done
		hp=$(gamepid "$name-host")
		sleep 10
		if [ -n "$staging" ] && [ -n "$hp" ]; then
			local step first=1
			local IFS='|'
			for step in $staging; do
				[ "$first" = 1 ] || sleep "${STEPGAP:-5}"
				first=0
				stage "$hp" "$step"
			done
			unset IFS
		fi
		# SHOTNOW 1: the shot straight after the staging (what it made is in view)
		[ "${SHOTNOW:-0}" = 1 ] && sleep 0.5 || sleep 6
		[ -n "$cp" ] && shot1 "$cp" "$SHOTDIR/content-$name.png"
	fi
	wait "$client"; echo "$name-client exit $?"
	for n in $(seq 20); do kill -0 "$host" 2>/dev/null || break; sleep 1; done
	kill -0 "$host" 2>/dev/null && { hp=$(gamepid "$name-host"); [ -n "$hp" ] && kill -TERM "$hp"; }
	wait "$host"; echo "$name-host exit $?"
	done_cases="$done_cases $name"
}

# A GoldenEye case needs the conversion: a host that could not find the map
skipped() {
	grep -q "net-test-map .*: no such map" "$OUT/$1-host.log" 2>/dev/null
}

# lastline LOG PATTERN: the newest line with PATTERN
lastline() { grep -- "$2" "$1" 2>/dev/null | tail -1; }
num() { echo "$1" | grep -o -- "$2 [0-9]*" | head -1 | awk '{print $NF}'; }
numb() { echo "$1" | grep -o -- "[0-9]* $2" | head -1 | awk '{print $1}'; }

# the common checks for a case that plays
checkplay() {
	local name=$1 key=$2
	local H=$OUT/$name-host.log C=$OUT/$name-client.log x c L
	for c in host client; do
		L=$OUT/$name-$c.log
		x=$(grep -o "$name-$c exit [0-9]*" "$OUT/run.log" | tail -1 | awk '{print $3}')
		[ "$c" = host ] && [ "$x" = 143 ] && x=0
		if grep -qE "FATAL|Segmentation|Aborted" "$L"; then
			fail "$name: $c crashed"; grep -A14 "FATAL" "$L" | head -16 | sed 's/^/     /'
		elif [ "$x" = 0 ]; then
			pass "$name: $c ran to the end"
		else
			fail "$name: $c exit '$x'"
		fi
	done
	grep -q "renderD128: Permission denied" "$OUT/$name-"*.log && fail "$name: a run fell back to llvmpipe"
	local load; load=$(lastline "$C" "net: match 1: loading")
	if echo "$load" | grep -q -- "$key"; then pass "$name: the client loaded $(echo "$load" | sed 's/.*loading \(.*\) as \(0x[0-9a-f]*\).*/\1 as \2/')"
	else fail "$name: the client did not load '$key': ${load:-no load line}"; fi
	grep -q "slot 1 (\"[^\"]*\") loaded the stage, every component the host's" "$H" \
		&& pass "$name: every stage hash component equal ($(grep -c 'net: stage hash' "$C") components)" \
		|| { fail "$name: stage hashes differ"; grep "slot 1.*loaded\|STAGEHASH\|stagehash" "$H" | head -3 | sed 's/^/     /'; }
	local ct; ct=$(lastline "$C" "net: content client")
	local tick; tick=$(echo "$ct" | grep -o "(tick [0-9]*)" | grep -o "[0-9]*")
	[ "${tick:-0}" -ge 1500 ] && pass "$name: the client played $tick match ticks" || fail "$name: the client played ${tick:-no} match ticks"
	local sn pu ev
	sn=$(lastline "$C" "net: snap client"); pu=$(lastline "$C" "net: puppets"); ev=$(lastline "$C" "net: events client")
	if [ "$(numb "$sn" malformed)" = 0 ] && [ "$(num "$sn" misfits)" = 0 ] && [ "$(num "$pu" "bad anims")" = 0 ] \
			&& [ "$(num "$ev" malformed)" = 0 ] && [ "$(num "$ev" overflow)" = 0 ]; then
		pass "$name: snapshots, puppets and events sane ($(numb "$sn" decoded) snapshots decoded, $(echo "$ev" | grep -o "unresolved [0-9]*" | head -1) events)"
	else
		fail "$name: snapshot/puppet/event faults: malformed $(numb "$sn" malformed)/$(num "$ev" malformed), misfits $(num "$sn" misfits), bad anims $(num "$pu" "bad anims"), overflow $(num "$ev" overflow)"
	fi
	[ "$(num "$pu" "held guns not made")" = 0 ] && pass "$name: every gun a puppet held was made" \
		|| fail "$name: held guns not made: $(num "$pu" "held guns not made")"
	echo "     $(echo "$ct" | sed 's/^.*net: //')"
	echo "     $(echo "$pu" | grep -o "made: [^;]*"); $(echo "$pu" | grep -o "content: .*")"
	[ -s "$SHOTDIR/content-$name.png" ] && pass "$name: screenshot $SHOTDIR/content-$name.png" || fail "$name: no screenshot"
}

# want NAME WHAT VALUE: a feature counted at least once on the client
want() {
	local name=$1 what=$2 v=$3
	[ "${v:-0}" -gt 0 ] 2>/dev/null && pass "$name: $what: $v" || fail "$name: $what: ${v:-none}"
}

# the newest summaries' counters
pupnum() { num "$(lastline "$OUT/$1-client.log" "net: puppets")" "$2"; }
evnum() { lastline "$OUT/$1-client.log" "net: events client" | grep -o "$2 [0-9]*/[0-9]*" | head -1 | cut -d' ' -f2 | cut -d/ -f1; }

checkrefused() {
	local name=$1 code=$2 pattern=$3
	local C=$OUT/$name-client.log H=$OUT/$name-host.log
	grep -qE "FATAL|Segmentation|Aborted" "$C" "$H" && fail "$name: a crash"
	local x; x=$(grep -o "$name-client exit [0-9]*" "$OUT/run.log" | tail -1 | awk '{print $3}')
	local why; why=$(grep -m1 -E "net: (refused|left|the host refused|disconnected|match over).*|net: client end" "$C")
	if grep -q -- "$pattern" "$C"; then
		pass "$name: the client was told: $(grep -m1 -o -- "$pattern.*" "$C" | cut -c1-200)"
	else
		fail "$name: no '$pattern' in the client's log (${why:-nothing})"
	fi
	[ "$x" = 3 ] && pass "$name: the client left with the refusal (exit 3)" || fail "$name: client exit '$x'"
	grep -q -- "net: session ended \[$code\]" "$C" && pass "$name: the session ended [$code]" || fail "$name: the session did not end [$code]"
	grep -q -- "slot 1 (\"[^\"]*\") is leaving \[$code\]\|the slot is kicked\|kick.*\[$code\]\|\[$code" "$H" \
		&& pass "$name: the host has it: $(grep -m1 -o -- "\[$code.*" "$H" | cut -c1-160)" || fail "$name: the host did not log [$code]"
}

done_cases=""
run() {
	for c in $CASES; do
		case $c in
		# (1) GoldenEye: an arena converted from the ROM, its mode, weapon
		# sets (its guns, numbered past 0x7f), its simulants and HUD; hats on
		# the sims for the client to put on its puppets
		ge)     runcase ge "${GEMAPS}StartArmed=1\n" "$GEMAPS" "--net-test-map Complex --net-test-ge 0" "" "hats()|hatdrop()" ;;
		# You Only Live Twice on Facility, and gas let go there (its fog has
		# the second sky gas fades to); sim 0 and the client's player killed
		# twice over (three tries, seven seconds apart, for the respawns)
		geyolt) STEPGAP=7 SHOTNOW=1 runcase geyolt "${GEMAPS}StartArmed=1\n" "$GEMAPS" "--net-test-map Facility --net-test-ge 1 --net-test-timelimit 2" \
				"--net-test-input $OUT/respawn.script" "kill()|kill()|gas()|kill()" ;;
		gegg)   runcase gegg "${GEMAPS}StartArmed=1\n" "$GEMAPS" "--net-test-map Archives --net-test-ge 4" "" ;;
		# a GoldenEye stage's MUST key that differs: refused at the start, named
		gemust) runcase gemust "${GEMAPS}XblaMeshes=1\n" "${GEMAPS}XblaMeshes=0\n" "--net-test-map Complex --net-test-ge 0" "" "" 1 ;;
		# (4) the XBLA look is one machine's own on Perfect Dark's stages
		xbla)   runcase xbla "ModDir=\nMapMods=\nXblaMeshes=1\nStartArmed=1\n" "ModDir=\nMapMods=\nXblaMeshes=0\n" "--net-test-stage 0x32" "" ;;
		# (2) a Stage Loader map, keyed by its mod's dir and its name
		modmap) runcase modmap "ModDir=\nMapMods=PD_Kakariko\nStartArmed=1\n" "ModDir=\nMapMods=PD_Kakariko\n" "--net-test-map Playground" "" ;;
		modmissing) runcase modmissing "ModDir=\nMapMods=PD_Kakariko\n" "ModDir=\nMapMods=\n" "--net-test-map Playground" "" "" 1 ;;
		# (3) an arena only the overlay mod has (mod_allinone's Suburb)
		overlay) runcase overlay "StartArmed=1\n" "" "--moddir mod_allinone --net-test-stage 0x18" "--moddir mod_allinone" ;;
		# (5) Grid: a lift and glass; rockets, grenades and mines; Mod.Bodies;
		# an N-bomb staged where the client's player is
		# the glass furthest from the client broken out of its scope, a loose
		# ammo crate, then the storm, shot as it rolls
		props)  SIMS=6 SHOTNOW=1 runcase props "StartArmed=1\nBodies=20\n" "" \
				"--moddir mod_allinone --net-test-stage 0x47 --mp-weapons 23,22,28,29,31,30" "--moddir mod_allinone" "glassfar()|crate()|nbomb()" ;;
		esac
	done
}

[ "${CHECKONLY:-0}" = 1 ] || run 2>&1 | tee "$OUT/run.log"

for c in $CASES; do
	case $c in
	ge|geyolt|gegg|gemust)
		if skipped "$c"; then echo "skip $c: GoldenEye is not converted here"; continue; fi ;;
	esac
	C=$OUT/$c-client.log
	case $c in
	ge)
		checkplay ge "map Complex from mod GoldenEye Arenas"
		ct=$(lastline "$C" "net: content client")
		echo "$ct" | grep -q "GoldenEye mode 1 scenario 0" && pass "ge: the GoldenEye mode from the host's RULES" || fail "ge: not in the GoldenEye mode"
		echo "$ct" | grep -q "GoldenEye HUD on" && pass "ge: GoldenEye's HUD drawn on the client" || fail "ge: GoldenEye's HUD off"
		want ge "ticks with a GoldenEye gun in the client player's hand" "$(echo "$ct" | grep -o "a GoldenEye gun [0-9]*" | awk '{print $NF}')"
		want ge "GoldenEye guns put in puppets' hands" "$(pupnum ge "GE guns held")"
		want ge "hats worn by puppets" "$(pupnum ge "hats worn")"
		want ge "loose hats made (knocked off on the host)" "$(pupnum ge hats)"
		;;
	geyolt)
		checkplay geyolt "map Facility from mod GoldenEye Arenas"
		lastline "$C" "net: content client" | grep -q "scenario 1 (You Only Live Twice)" && pass "geyolt: You Only Live Twice from the host's RULES" || fail "geyolt: not YOLT"
		want geyolt "deaths the client took from the host" "$(evnum geyolt death)"
		yl=$(lastline "$C" "net: yolt client")
		want geyolt "chrs out of lives on the client" "$(num "$yl" "chrs out")"
		want geyolt "respawns of the client's own player from its START alone" "$(lastline "$C" "net: local block" | sed -n 's/.*, respawns \([0-9]*\), inventory.*/\1/p')"
		want geyolt "ticks the client's own player was out (no respawn)" "$(echo "$yl" | grep -o "player out [0-9]*" | awk '{print $NF}')"
		[ "$(num "$yl" "out yet alive")" = 0 ] && pass "geyolt: no chr out of lives came back on the client ($(num "$yl" "out ticks") chr-ticks out)" \
			|| fail "geyolt: chrs out of lives alive again on the client: ${yl:-no yolt line}"
		grep "STAGE kill" "$OUT/run.log" | tail -3 | sed 's/^ *//; s/^/     /'
		want geyolt "gas released on the client" "$(echo "$(lastline "$C" "net: events client")" | grep -o "gas released [0-9]*" | awk '{print $NF}')"
		;;
	gegg)
		checkplay gegg "map Archives from mod GoldenEye Arenas"
		lastline "$C" "net: content client" | grep -q "scenario 4 (The Man with the Golden Gun)" && pass "gegg: The Man with the Golden Gun from the host's RULES" || fail "gegg: not the Golden Gun"
		want gegg "the Golden Gun made from the host's entity" "$(pupnum gegg "Golden Gun")"
		want gegg "the Golden Gun in a puppet's hand" "$(pupnum gegg "in a puppet's hand")"
		echo "     Golden Gun holders in turn on the client: $(pupnum gegg "holders in turn")"
		;;
	gemust) checkrefused gemust "must" "Mod.XblaMeshes must match" ;;
	xbla)
		checkplay xbla "stock stage 0x32"
		lastline "$C" "net: content client" | grep -q "Mod.XblaMeshes 0" && grep -q "net: rules applied" "$C" \
			&& pass "xbla: the client kept its own Mod.XblaMeshes (0) beside the host's 1" || fail "xbla: Mod.XblaMeshes not the client's own"
		;;
	modmap) checkplay modmap "map Playground from mod PD_Kakariko" ;;
	modmissing)
		checkrefused modmissing "nostage" "map Playground from mod PD_Kakariko, which is not installed here"
		grep -q "Stage Loader map mods differ" "$OUT/modmissing-host.log" && pass "modmissing: joined with other map mods (noted, not refused)" || fail "modmissing: refused at CONNECT for its map mods"
		;;
	overlay) checkplay overlay "stage 0x18 of mod mod_allinone" ;;
	props)
		checkplay props "stage 0x47 of mod mod_allinone"
		want props "Mod.Bodies corpses made on the client" "$(pupnum props "bodies made")"
		want props "lift moves posed" "$(pupnum props "lift moves")"
		want props "glass broken (events that acted)" "$(evnum props glass)"
		want props "glass broken that the client never had in scope (by its setup command)" \
			"$(lastline "$C" "net: events client" | grep -o "glass by setup command [0-9]*" | awk '{print $NF}')"
		want props "loose ammo crates made" "$(pupnum props crates)"
		want props "explosions (rockets, grenades, mines)" "$(evnum props explosion)"
		want props "projectile trails" "$(pupnum props trails)"
		want props "thrown or dropped weapons made" "$(lastline "$C" "net: puppets" | grep -o "made: weapons [0-9]*" | awk '{print $NF}')"
		want props "ammo crates picked up and back (regens)" "$(pupnum props regens)"
		want props "N-bomb storms" "$(evnum props nbomb)"
		ev=$(lastline "$C" "net: events client")
		echo "     events: $(echo "$ev" | grep -o "unresolved by type.*")"
		;;
	esac
done

exit $status
