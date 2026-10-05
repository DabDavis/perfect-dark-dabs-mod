#!/bin/bash
# netscenariotest.sh — every Combat Simulator scenario online: does the
# client's scenario state, its HUD's inputs and its scores follow the host's?
#
# Loopback (PLANS/NETPLAY.md phase 7a): for each case a listen host and one
# client on 127.0.0.1, offscreen on the GPU, with sims playing the scenario
# to a time limit, so the host ends the match and sends MATCH_END. Both
# write --net-scen-log: the host a line per tick for the client's player
# (the scenario's state as its HUD reads it, and every mpchr's score), the
# client the same line each time it applies the host's block of a tick
# (after that tick's events) and at the end. Checks per case:
#
#   - both ran to the end, no crash; the host's MATCH_END reached the client;
#   - the client's line equals the host's of the same tick at every block it
#     applied (scores worked out from its own kill table and the block's
#     counts), and at MATCH_END;
#   - the scenario was played: holders changed, points scored, the hill
#     taken, victims rotated (per case below), on the client's side too;
#   - the scenario's props were made from the host's entities (never by the
#     client's own scenarioInitProps) and found by the block's references;
#   - a screenshot of the client's HUD once the HUD has something to show
#     for its player (staged on the host where the sims would not get it
#     there in time) in build/net-shots/scenario-*.png.
#
#   netscenariotest.sh [BIN]   BIN a file name in build/ (pd.x86_64) or a path
#
# Env: OUT (build/netscen-out), PORT (27260), MODDIR (mod_allinone),
# CASES (all: htb htm pac koh ctc htbteams pacteams htmloss pacloss), STAGE (0x32 Skedar),
# CHECKONLY (1: only the checks, on the last run's files).
# Exit status: 0 all good, 1 a check failed, 2 a run failed to start.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=${BUILD:-$ROOT/build}
OUT=${OUT:-$BUILD/netscen-out}; PORT=${PORT:-27260}
MODDIR=${MODDIR:-mod_allinone}
STAGE=${STAGE:-0x32}
CASES=${CASES:-htb htm pac koh ctc htbteams pacteams htmloss pacloss}
BIN=${1:-pd.x86_64}
case $BIN in /*) ;; */*) BIN=$(realpath "$BIN") ;; *) BIN=$BUILD/$BIN ;; esac
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
SHOTDIR=$BUILD/net-shots
export SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-offscreen} SDL_GAMECONTROLLER_IGNORE_DEVICES=0x054c/0x0ce6,0x045e/0x028e \
	SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT=0x0000/0x0000 SDL_JOYSTICK_HIDAPI=0

status=0
fail() { echo "FAIL $*"; status=1; }
pass() { echo "ok   $*"; }

INI="[Mod]\nStartArmed=1\n"

# game LABEL TIMEOUT ARGS...
game() {
	local label=$1 t=$2; shift 2
	local save=$OUT/save-$label
	rm -rf "$save"; mkdir -p "$save"
	printf '%b' "$INI" > "$save/pd.ini"
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

# Host staging, between two ticks (a breakpoint on the scenario's tick end)
cat > "$OUT/stage.py" <<'PY'
import gdb, math
def i(e): return int(gdb.parse_and_eval(e))
def f(e): return float(gdb.parse_and_eval(e))
def remote():
    for k in range(4):
        if i("(long)g_Vars.players[%d]" % k) and (i("g_Vars.playerstats[%d].mpindex" % k) & 3) == 1:
            return k
    return -1
def htb(keep=0):
    """the briefcase to the client's player, as a pickup would leave it
    (keep: the player made invincible, so it keeps it and scores)"""
    pn = remote()
    t = i("(long)g_ScenarioData.htb.token")
    if pn < 0 or not t: print("STAGE htb: nothing to stage"); return
    if i("g_ScenarioData.htb.token->type") != 4: print("STAGE htb: the briefcase is held already"); return
    gdb.execute("call (void)objFreePermanently(g_ScenarioData.htb.token->obj, 1)")
    gdb.execute("call (void)setCurrentPlayerNum(%d)" % pn)
    gdb.execute("call (void)invGiveSingleWeapon(0x57)")
    gdb.execute("call (void)setCurrentPlayerNum(0)")
    if keep: gdb.execute("set var g_Vars.players[%d]->invincible = 1" % pn)
    print("STAGE htb: the briefcase given to player %d%s" % (pn, " (invincible)" if keep else ""))
def pac():
    """the client's player the victim"""
    pn = remote()
    if pn < 0: print("STAGE pac: no remote player"); return
    for n in range(i("g_MpNumChrs") + 1):
        vi = i("g_ScenarioData.pac.victimindex")
        if vi >= 0 and i("g_ScenarioData.pac.victims[%d]" % vi) == pn: break
        gdb.execute("call (void)pacApplyNextVictim()")
    print("STAGE pac: victim index %d, player %d" % (i("g_ScenarioData.pac.victimindex"), pn))
def ctc():
    """the other team's case to the client's player, as its pickup would
    leave it (invincible, so it still has it for capture() later)"""
    pn = remote()
    team = i("g_PlayerConfigsArray[1].base.team")
    for t in range(4):
        if t == team or not i("(long)g_ScenarioData.ctc.tokens[%d]" % t): continue
        if i("g_ScenarioData.ctc.tokens[%d]->type" % t) != 4: continue
        gdb.execute("call (void)objFreePermanently(g_ScenarioData.ctc.tokens[%d]->obj, 1)" % t)
        gdb.execute("set var g_ScenarioData.ctc.tokens[%d] = g_Vars.players[%d]->prop" % (t, pn))
        gdb.execute("call (void)setCurrentPlayerNum(%d)" % pn)
        gdb.execute("call (void)invGiveSingleWeapon(0x57)")
        gdb.execute("call (void)setCurrentPlayerNum(0)")
        gdb.execute("set var g_Vars.players[%d]->invincible = 1" % pn)
        print("STAGE ctc: team %d's case given to player %d (team %d)" % (t, pn, team))
        return
    print("STAGE ctc: no case on the ground to give")
def capture():
    """the client's player, holding the other team's case, reaches its own
    home case: scenarioPickUpBriefcase scores it as a walk there would"""
    pn = remote()
    team = i("g_PlayerConfigsArray[1].base.team")
    own = "g_ScenarioData.ctc.tokens[%d]" % team
    if not i("(long)" + own) or i(own + "->type") != 4: print("STAGE capture: the home case is not home"); return
    gdb.execute("call (void)setCurrentPlayerNum(%d)" % pn)
    if not i("(int)invHasBriefcase()"):
        gdb.execute("call (void)setCurrentPlayerNum(0)"); print("STAGE capture: player %d holds no case" % pn); return
    gdb.execute("call (int)scenarioPickUpBriefcase(g_Vars.players[%d]->prop->chr, %s)" % (pn, own))
    gdb.execute("call (void)setCurrentPlayerNum(0)")
    print("STAGE capture: player %d captured; its points %d" % (pn, i("g_PlayerConfigsArray[1].base.numpoints")))
def htm1():
    """the uplink to the client's player, in its hand, and the terminal put
    in front of it: what a walk there would leave"""
    pn = remote()
    up = "g_ScenarioData.htm.uplink"
    if pn < 0 or not i("(long)g_ScenarioData.htm.terminals[0].prop"): print("STAGE htm: nothing to stage"); return
    if i("(long)" + up) and i(up + "->type") == 4:
        gdb.execute("call (void)objFreePermanently(%s->obj, 1)" % up)
    gdb.execute("call (void)setCurrentPlayerNum(%d)" % pn)
    if not i("(int)invHasDataUplink()"): gdb.execute("call (void)invGiveSingleWeapon(0x36)")
    gdb.execute("call (void)bgunEquipWeapon(0x36)")
    gdb.execute("call (void)setCurrentPlayerNum(0)")
    gdb.execute("set var g_Vars.players[%d]->invincible = 1" % pn)
    p = "g_Vars.players[%d]" % pn
    a = math.radians(f(p + "->vv_theta"))
    t = "g_ScenarioData.htm.terminals[0].prop->pos"
    gdb.execute("set var %s.x = %f" % (t, f(p + "->prop->pos.x") - math.sin(a) * 120))
    gdb.execute("set var %s.z = %f" % (t, f(p + "->prop->pos.z") + math.cos(a) * 120))
    gdb.execute("set var %s.y = %f" % (t, f(p + "->prop->pos.y")))
    print("STAGE htm: the uplink in player %d's hand, the terminal 120 units in front of it" % pn)
def htm2():
    """the client's player uses the terminal (currentPlayerInteract's mark)"""
    pn = remote()
    o = "g_ScenarioData.htm.terminals[0].prop->obj->hidden"
    gdb.execute("set var %s = (%s & 0x0fffffff) | 0x4000 | (%d << 28)" % (o, o, pn))
    print("STAGE htm: player %d used the terminal" % pn)
def invincible():
    pn = remote()
    if pn >= 0: gdb.execute("set var g_Vars.players[%d]->invincible = 1" % pn); print("STAGE player %d invincible" % pn)
PY

stage() {
	local hp=$1 what=$2
	timeout 30 gdb -p "$hp" -batch -ex "source $OUT/stage.py" -ex "break netScenHostTickEnd" -ex "continue" \
		-ex "delete" -ex "python $what" 2>/dev/null | grep "^STAGE" | sed 's/^/     host: /'
}

# the client's newest scenario line has PATTERN for its player
clientshows() {
	tail -n 3 "$OUT/$1-client.scen" 2>/dev/null | grep -q -- "$2"
}

# runcase NAME SCENARIO TEAMS MINUTES STAGING HUDPATTERN [LATER [HOSTARGS [CLIENTARGS]]]:
# STAGING on the host once the match runs, LATER 20 seconds after the shot
runcase() {
	local name=$1 scen=$2 teams=$3 mins=$4 staging=$5 hudpat=$6 later=${7:-} hostargs=${8:-} clientargs=${9:-}
	local frames=$(( mins * 3600 + 1500 ))
	local port=$((PORT + ${#done_cases}))
	rm -f "$OUT/$name-"*
	echo "== $name: scenario $scen, teams $teams, $mins min, stage $STAGE"
	game "$name-host" $((mins * 60 + 200)) --moddir "$MODDIR" --host "$port" --net-test-host 1 --net-test-stage "$STAGE" \
		--net-test-sims 6 --rng-seed 7 --net-test-timelimit "$mins" --net-test-scenario "$scen" --net-test-teams "$teams" \
		--net-test-hilltime 0 $hostargs --net-scen-log "$OUT/$name-host.scen" --exit-frame $((frames + 600)) &
	local host=$!
	waitfor "$OUT/$name-host.log" "net: hosting on UDP port" 60 || { echo "FAIL: host did not start"; kill -TERM $host; return; }
	game "$name-client" $((mins * 60 + 190)) --moddir "$MODDIR" --connect "127.0.0.1:$port" --net-test-join $clientargs \
		--net-scen-log "$OUT/$name-client.scen" --net-event-log "$OUT/$name-client.events" --exit-frame "$frames" &
	local client=$!
	local hp cp n
	waitfor "$OUT/$name-client.log" "net: accepted by" 60
	for n in $(seq 20); do cp=$(gamepid "$name-client"); [ -n "$cp" ] && break; sleep 0.5; done
	hp=$(gamepid "$name-host")
	if waitfor "$OUT/$name-client.log" "net: match 1: GO" 120 && [ -n "$hp" ] && [ -n "$cp" ]; then
		sleep 3
		# a staging of several steps: a|b, four seconds apart
		local step first=1
		local IFS='|'
		for step in $staging; do
			[ "$first" = 1 ] || sleep 4
			first=0
			stage "$hp" "$step"
		done
		unset IFS
		# the HUD with something on it for the client's player, then a shot
		for n in $(seq 90); do clientshows "$name" "$hudpat" && break; sleep 1; done
		clientshows "$name" "$hudpat" && echo "     the client's HUD shows its state ($hudpat) after ${n}s" || echo "     the client's HUD never showed $hudpat"
		sleep 2
		shot1 "$cp" "$SHOTDIR/scenario-$name.png"
		if [ -n "$later" ]; then
			sleep 20
			stage "$hp" "$later"
		fi
	else
		echo "     staging skipped (host '$hp', client '$cp')"
	fi
	wait "$client"; echo "$name-client exit $?"
	for n in $(seq 15); do kill -0 "$host" 2>/dev/null || break; sleep 1; done
	kill -0 "$host" 2>/dev/null && { hp=$(gamepid "$name-host"); [ -n "$hp" ] && kill -TERM "$hp"; }
	wait "$host"; echo "$name-host exit $?"
	done_cases="$done_cases $name"
}

check() {
	local name=$1 what=$2
	local L
	for c in host client; do
		L=$OUT/$name-$c.log
		x=$(grep -o "$name-$c exit [0-9]*" "$OUT/run.log" | tail -1 | awk '{print $3}')
		[ "$c" = host ] && [ "$x" = 143 ] && x=0
		if grep -qE "FATAL|Segmentation|Aborted" "$L"; then
			fail "$name: $c crashed"; grep -A12 "FATAL" "$L" | head -14 | sed 's/^/     /'
		elif [ "$x" = 0 ]; then
			pass "$name: $c ran to the end"
		else
			fail "$name: $c exit '$x'"
		fi
	done
	grep -q "renderD128: Permission denied" "$OUT/$name-"*.log && fail "$name: a run fell back to llvmpipe"
	grep -q "scores and awards sent" "$OUT/$name-host.log" && grep -q "the host ended it" "$OUT/$name-client.log" \
		&& pass "$name: the host ended the match on its clock; the client took its MATCH_END" || fail "$name: no MATCH_END end to end"
	grep -q "net: scenario $what: its props are the host's" "$OUT/$name-client.log" \
		&& pass "$name: the client made no scenario props of its own" || fail "$name: the client's scenario props not skipped"
	grep "net: scenario client:" "$OUT/$name-client.log" | tail -1 | sed 's/^.*net: /     /'
	grep "net: puppets at" "$OUT/$name-client.log" | tail -1 | grep -o "made: [^;]*" | sed 's/^/     /'
	[ -s "$SHOTDIR/scenario-$name.png" ] && pass "$name: screenshot $SHOTDIR/scenario-$name.png" || fail "$name: no screenshot"
	python3 - "$name" "$what" "$OUT/$name-host.scen" "$OUT/$name-client.scen" "$OUT/$name-client.log" "$OUT/$name-client.events" <<'PY' || status=1
import sys, re
name, what, hp, cp, clog, cev = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4], sys.argv[5], sys.argv[6]
st = 0
def bad(m):
    global st; print("FAIL %s: %s" % (name, m)); st = 1
def ok(m): print("ok   %s: %s" % (name, m))
def lines(p):
    try: return [l.rstrip("\n").split(" ", 3) for l in open(p)]
    except OSError: return []
H = lines(hp); C = lines(cp)
hs = {(l[1], l[2]): l[3] for l in H if l[0] == "S"}
cs = [(l[1], l[2], l[3]) for l in C if l[0] == "S"]
same = 0; diffs = []
missing = 0; ahead = 0
def own(txt):
    # the player's own inventory (hb/hu): the local-player block's, which
    # runs ahead of the render clock the scenario's block is applied at
    return re.sub(r" h[bu]=\d", "", txt)
for t, pn, txt in cs:
    h = hs.get((t, pn))
    if h is None: missing += 1
    elif h == txt: same += 1
    elif own(h) == own(txt) and any(hs.get((str(int(t) + k), pn), "") == txt or
            re.findall(r" h[bu]=\d", hs.get((str(int(t) + k), pn), "")) == re.findall(r" h[bu]=\d", txt) for k in range(1, 41)):
        # the host's own line says the same within 40 ticks (the most the
        # render clock sits behind, NETPUP_MAXDELAY): the client's player's
        # death or pickup came in the newer local-player block
        ahead += 1
    else: diffs.append("tick %s: host [%s] client [%s]" % (t, h, txt))
if cs and same >= 200 and not diffs and missing == 0:
    ok("the client's state equals the host's at all %d blocks it applied (host ticks %s..%s)%s" % (same + ahead, cs[0][0], cs[-1][0],
        "; at %d its own inventory was the host's of up to 40 ticks later (its local-player block runs ahead of the render clock)" % ahead if ahead else ""))
else:
    bad("%d blocks equal, %d differ, %d with no host line, %d with the inventory ahead (of %d)" % (same, len(diffs), missing, ahead, len(cs)))
    for d in diffs[:4]: print("     " + d)
he = [l[3] for l in H if l[0] == "E"]; ce = [l[3] for l in C if l[0] == "E"]
if he and ce and he[-1] == ce[-1]:
    ok("at MATCH_END equal: %s" % ce[-1])
else:
    bad("at MATCH_END: host [%s] client [%s]" % (he[-1] if he else "-", ce[-1] if ce else "-"))
# was the scenario played, as the client saw it
def field(txt, key):
    m = re.search(r"\b%s=(\S+)" % key, txt)
    return m.group(1) if m else None
ctxt = [c[2] for c in cs]
def changes(key):
    vals = [field(t, key) for t in ctxt]
    return sum(1 for a, b in zip(vals, vals[1:]) if a != b)
def points(txt):
    return sum(int(x.split("/")[2]) for x in re.findall(r"\d+:-?\d+/\d+/-?\d+", txt.split("|")[1])) if "|" in txt else 0
def scores(txt):
    return [int(x.split(":")[1].split("/")[0]) for x in re.findall(r"\d+:-?\d+/\d+/-?\d+", txt.split("|")[1])] if "|" in txt else []
end = ce[-1] if ce else ""
played = {}
if what == 1:
    played = {"briefcase holder changes": changes("tok"), "client held it": sum(1 for t in ctxt if field(t, "hb") == "1")}
    if name.endswith("teams"): played["points"] = points(end)  # the client kept it long enough
elif what == 2:
    played = {"uplink holder changes": changes("up"), "download starts": sum(1 for a, b in zip(ctxt, ctxt[1:]) if field(a, "dl") == "-1" and field(b, "dl") != "-1"),
              "client's downloads": int(re.findall(r" 1:(-?\d+)/", end.split("|")[1])[0]) // 2 if "|" in end and re.findall(r" 1:(-?\d+)/", end.split("|")[1]) else 0}
elif what == 3:
    played = {"victims": changes("v"), "scored": max(scores(end) or [0])}
elif what == 4:
    played = {"hill taken/lost": changes("occ"), "points": points(end), "client's team held it": sum(1 for t in ctxt if field(t, "hud") == "1")}
elif what == 5:
    played = {"case holder changes": sum(changes("t%d" % k) for k in range(4)), "bases lit": sum(1 for k in range(4) if (field(end, "t%d" % k) or "-/-1/-1").split("/")[2] != "-1"),
              "captures": points(end)}
if played and all(v > 0 for v in played.values()):
    ok("played: " + ", ".join("%s %d" % kv for kv in played.items()))
else:
    bad("not played enough: " + ", ".join("%s %d" % kv for kv in played.items()))
# the scenario's hudmsgs the host made for the client's player
try:
    hud = [l.rstrip("\n").split(" ", 6)[-1] for l in open(cev) if l.startswith("A ") and l.split()[3] == "hudmsg"]
except OSError:
    hud = []
texts = sorted(set(hud))
if hud: ok("%d hudmsgs from the host shown: %s" % (len(hud), ", ".join(texts[:6])))
else: bad("no hudmsg from the host")
log = open(clog).read()
m = re.findall(r"(\d+) of (\d+) entity references unresolved", log)
if m:
    u, r = int(m[-1][0]), int(m[-1][1])
    if r == 0 or u * 20 <= r: ok("entity references found: %d of %d unresolved" % (u, r))
    else: bad("entity references unresolved: %d of %d" % (u, r))
m = re.findall(r"(\d+) blocks applied \(last at host tick (\d+)\), (\d+) skipped", log)
if m and int(m[-1][2]) == 0: ok("%s blocks applied, none skipped" % m[-1][0])
else: bad("blocks skipped: %s" % (m[-1] if m else "?"))
m = re.findall(r"(\d+) held for their events \((\d+) given up\)", log)
if m and int(m[-1][1]) == 0: ok("%s blocks waited for events sent again, none applied without them" % m[-1][0])
else: bad("blocks applied without their events: %s" % (m[-1][1] if m else "?"))
sys.exit(st)
PY
}

done_cases=""
run() {
	for c in $CASES; do
		case $c in
		htb)      runcase htb 1 0 2 "htb()" "hb=1 ht=[1-9][0-9][0-9]" ;;
		htm)      runcase htm 2 0 2 "htm1()|htm2()" "dl=1 term=0" ;;
		pac)      runcase pac 3 0 1 "pac()" "v=1 " ;;
		koh)      runcase koh 4 2 2 "invincible()" "hud=1" "" "--net-test-simteam 1" ;;
		ctc)      runcase ctc 5 2 2 "ctc()" "t[0-3]=c" "capture()" ;;
		htbteams) runcase htbteams 1 2 1 "htb(1)" "hb=1 ht=[1-9][0-9][0-9]" ;;
		pacteams) runcase pacteams 3 2 1 "pac()" "v=1 " ;;
		# a lossy link (the client's --net-sim: drop %, delay ms, jitter ms):
		# a block waits for the events sent again before it
		htmloss)  runcase htmloss 2 0 2 "htm1()|htm2()" "dl=1 term=0" "" "" "--net-sim 5,80,60" ;;
		pacloss)  runcase pacloss 3 0 1 "pac()" "v=1 " "" "" "--net-sim 10,30,20" ;;
		esac
	done
}

# CHECKONLY=1: the checks on the last run's files
[ "${CHECKONLY:-0}" = 1 ] || run 2>&1 | tee "$OUT/run.log"

for c in $CASES; do
	case $c in
	htb|htbteams) check "$c" 1 ;;
	htm|htmloss) check "$c" 2 ;;
	pac|pacteams|pacloss) check "$c" 3 ;;
	koh) check "$c" 4 ;;
	ctc) check "$c" 5 ;;
	esac
done

exit $status
