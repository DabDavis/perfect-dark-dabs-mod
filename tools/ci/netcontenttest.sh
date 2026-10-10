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
# A refusal case (modmissing) instead checks the client was refused with
# the mod named in the text it was given. gelook: the look is each
# machine's own on GoldenEye's stages too (protocol 15): an HD host with the
# Community Edition and an N64 client on Bunker, whose tiles have a _ce
# copy; the client loads the host's data whatever its look (every stage
# hash component equal) and keeps its own Mod.XblaMeshes, and each switches
# its look in the middle of the match (--net-test-look-switch) as F6 does. Content follows
# the host (protocol 13, netcontent.c): modmount's client left the host's
# map mod out of its Mod.MapMods and mounts it on demand; modmissing's host
# plays with a mod the client has no copy of, which the client leaves over
# (NOMOD), named.
#
#   netcontenttest.sh [BIN]   BIN a file name in build/ (pd.x86_64) or a path
#
# pdmodown, pdmodfetch (protocol 25, 2026-10-09): a PD mod entered live
# (modmode.c) online. The host enters PDMOD (an installed data-only mod) with
# --net-test-pdmod; pdmodown's client has the same mod and enters it at the
# match's stage change, its stage key and RULES resolved again after the
# swap; pdmodfetch's client has no mods, is served the mod into memory
# ($N/PDMOD), enters it, and outlives the host: its session ends under it and
# it is back to Perfect Dark. modmissing: an overlay with nothing to serve
# (an empty files/) still leaves with NOMOD, named.
#
# Env: OUT (build/netcontent-out), PORT (27300), CASES (all: ge geyolt gegg
# gelook gfvariant gffetch gfcs gfcsfetch fetch xbla modmap modmount modmissing pdmodown pdmodfetch overlay props),
# PDMOD (PerfectDarkAllSoloLevelsInMultiplayer), CHECKONLY (1: only the checks,
# on the last run's files). The GoldenEye cases need the GoldenEye ROM
# converted (mods/GoldenEye Arenas), gfvariant Goldfinger 64 converted too
# (its zip in added-content/); they are skipped, not failed, without it.
# gfvariant: the host's Combat Simulator is Goldfinger 64's (its mode, so
# its weapon sets in the list's block), the client has neither its mode
# nor its maps mounted: the mode comes from RULES and the maps are mounted
# on demand (protocol 13). fetch: the client has no mods at all (its binary
# hard-linked into a folder of its own, with an empty mods/ and only the
# ROM), so it fetches GoldenEye Arenas from the host (protocol 14,
# netcontent.c) into memory and plays Complex from it; skipped with ge.
# gffetch: fetch's client in a Goldfinger 64 room - it fetches Goldfinger 64
# for the map and then GoldenEye Arenas for the characters the host's sims
# and players wear (GoldenEye's, from GoldenEye's own conversion in a hack's
# mode too: without them the host's list places were Dark Combat or Dr.
# Caroll here, whose skeleton holds no gun), and plays Junkyard in its mode.
# The host's --net-test-ge-variant finds the hack by its mounted folder (a
# build dir has mods/ but not the zip in added-content/), as a guest does.
# gfcs: Perfect Dark's own Combat Simulator on Complex with Goldfinger 64's
# guns chosen (Mod.CsHackGuns, gexplus.c): the host's SYNC key puts the
# hack's guns in at the client's stage load; the client has Goldfinger 64
# installed but left out of Mod.MapMods and mounts it for the guns
# (netContentCsHackGunsFollow). gfcsfetch: the same with fetch's client,
# which has no mods and fetches Goldfinger 64 from the host for its guns.
# Exit status: 0 all good, 1 a check failed, 2 a run failed to start.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=${BUILD:-$ROOT/build}
OUT=${OUT:-$BUILD/netcontent-out}; PORT=${PORT:-27300}
CASES=${CASES:-ge geyolt gegg gelook gfvariant gffetch gfcs gfcsfetch fetch xbla modmap modmount modmissing pdmodown pdmodfetch overlay props}
PDMOD=${PDMOD:-PerfectDarkAllSoloLevelsInMultiplayer}
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

# game LABEL TIMEOUT INI ARGS... (CLIENTBIN, when set, is the binary a
# *-client label runs: the fetch case's copy in a folder with no mods)
game() {
	local label=$1 t=$2 ini=$3; shift 3
	local save=$OUT/save-$label
	local bin=$BIN
	rm -rf "$save"; mkdir -p "$save"
	printf '[Mod]\n%b' "$ini" > "$save/pd.ini"
	cd "$BUILD" || exit 2
	# (and from its own folder: the game scans ./mods and $E/mods, and the
	# build directory's ./mods would give it everything after all)
	case $label in *-client) [ -n "${CLIENTBIN:-}" ] && { bin=$CLIENTBIN; cd "$(dirname "$CLIENTBIN")" || exit 2; } ;; esac
	exec timeout -k 5 "$t" stdbuf -oL -eL "$bin" --savedir "$save" --skip-intro --no-sound "$@" > "$OUT/$label.log" 2>&1
}

# the fetch case's client: the same binary hard-linked (or copied) into a
# folder of its own, so its executable dir has an empty mods/ and no
# added-content/, and only the ROM through a link
fetchclient() {
	local dir=$OUT/fetchbin
	rm -rf "$dir"; mkdir -p "$dir/mods" "$dir/added-content"
	ln "$BIN" "$dir/pd.x86_64" 2>/dev/null || cp "$BIN" "$dir/pd.x86_64"
	ln -s "$BUILD/data" "$dir/data"
	echo "$dir/pd.x86_64"
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
	# a game writes its pictures beside its executable: the fetch client's
	# are in its own folder
	local src=$BUILD/screenshots
	[ -n "${CLIENTBIN:-}" ] && src=$(dirname "$CLIENTBIN")/screenshots
	rm -f "$dest"
	mkdir -p "$src" "$SHOTDIR"
	before=$(ls "$src" 2>/dev/null | sort)
	gdb -p "$pid" -batch -ex 'call (void)screenshotRequest()' >/dev/null 2>&1
	for i in $(seq 50); do
		f=$(comm -13 <(echo "$before") <(ls "$src" 2>/dev/null | sort) | head -1)
		[ -n "$f" ] && { sleep 0.3; cp "$src/$f" "$dest"; echo "     shot: $dest"; return; }
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

# Half a second after the attach: the attach stalls the host while a client
# goes on sending, and the first ticks after it play what queued up - a dead
# client's START among them, whose respawn a step run in that same tick would
# undo before the tick's end counted it (geyolt's "respawns from START alone:
# 0", three times in six on 2026-10-07: the host respawned it at 1099 and the
# second kill had it dead again at 1099's end, so no block ever said alive)
stage() {
	local hp=$1 what=$2
	timeout 30 gdb -p "$hp" -batch -ex "source $OUT/stage.py" -ex "break netScenHostTickEnd" -ex "ignore 1 30" -ex "continue" \
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
	# CLIENTFRAMES, CLIENTT: a client that outlives the host (pdmodfetch: its
	# session ends under it, and what it entered for the host is left). The
	# menus it drops to never reach a frame (the level's frame stays at ~300
	# there, a client without a mod too), so CLIENTT's timeout stops it and
	# exit 124 is its end; NOJOIN 1: no --net-test-join, whose exit 3 at a
	# session's end would cut that short
	game "$name-client" "${CLIENTT:-390}" "$cini" --connect "127.0.0.1:$port" $([ "${NOJOIN:-0}" = 1 ] || echo --net-test-join) \
		--exit-frame "${CLIENTFRAMES:-$FRAMES}" $cargs &
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
		x=$(grep -o "^$name-$c exit [0-9]*" "$OUT/run.log" | tail -1 | awk '{print $3}')
		[ "$c" = host ] && [ "$x" = 143 ] && x=0
		[ "$c" = client ] && [ "$x" = 124 ] && [ "${ALLOW124:-}" = "$name" ] && x=0
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

# chars NAME: the client's lists had the host's GoldenEye characters at the
# stage's load (the pool built again over the conversion fetched): every
# body; a head only the XBLA release has (the host has it, the client's pool
# is the ROM's) stands in
chars() {
	local name=$1 L
	L=$(lastline "$OUT/$name-client.log" "of the host's characters are GoldenEye's; the pool built again")
	if [ -z "$L" ]; then
		fail "$name: the client never built GoldenEye's characters over the fetched conversion"
	elif echo "$L" | grep -q ", 0 of them bodies"; then
		pass "$name: the host's characters' bodies in the client's lists: $(echo "$L" | sed 's/.*net: content: //')"
	else
		fail "$name: bodies missing: $(echo "$L" | sed 's/.*net: content: //')"
	fi
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
	local x; x=$(grep -o "^$name-client exit [0-9]*" "$OUT/run.log" | tail -1 | awk '{print $3}')
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
		gelook) runcase gelook "${GEMAPS}XblaMeshes=1\nGeXblaCommunityEdition=1\nStartArmed=1\n" "${GEMAPS}XblaMeshes=0\nGeXblaCommunityEdition=0\n" \
				"--net-test-map Bunker --net-test-ge 0 --net-test-look-switch 900" "--net-test-look-switch 1200" ;;
		# a ROM hack's mode: Goldfinger 64's Junkyard, its weapon sets; the
		# client mounts Goldfinger on demand and takes the mode from RULES
		gfvariant) runcase gfvariant "ModDir=\nMapMods=GoldenEye Arenas;Goldfinger 64\nStartArmed=1\n" "$GEMAPS" \
				"--net-test-map Junkyard --net-test-ge 0 --net-test-ge-variant gf64" "" ;;
		# fetch's client in a Goldfinger 64 room: only the host has the hack
		gffetch) CLIENTBIN=$(fetchclient) runcase gffetch "ModDir=\nMapMods=GoldenEye Arenas;Goldfinger 64\nStartArmed=1\n" "ModDir=\nMapMods=\n" \
				"--net-test-map Junkyard --net-test-ge 0 --net-test-ge-variant gf64" "" ;;
		# Perfect Dark's Combat Simulator with Goldfinger 64's guns on
		# Complex: the client mounts the hack for them (gfcs) or, with no
		# mods at all, fetches it from the host (gfcsfetch)
		gfcs) runcase gfcs "ModDir=\nMapMods=GoldenEye Arenas;Goldfinger 64\nStartArmed=1\nCsHackGuns=gf64\n" "$GEMAPS" \
				"--net-test-stage 0x1f --mp-weapons 49,50,51,52,74,75" "" ;;
		gfcsfetch) CLIENTBIN=$(fetchclient) runcase gfcsfetch "ModDir=\nMapMods=GoldenEye Arenas;Goldfinger 64\nStartArmed=1\nCsHackGuns=gf64\n" "ModDir=\nMapMods=\n" \
				"--net-test-stage 0x1f --mp-weapons 49,50,51,52,74,75" "" ;;
		# a client with nothing installed fetches the host's conversion
		fetch)  CLIENTBIN=$(fetchclient) runcase fetch "${GEMAPS}StartArmed=1\n" "ModDir=\nMapMods=\n" "--net-test-map Complex --net-test-ge 0" "" ;;
		# (4) the XBLA look is one machine's own on Perfect Dark's stages
		xbla)   runcase xbla "ModDir=\nMapMods=\nXblaMeshes=1\nStartArmed=1\n" "ModDir=\nMapMods=\nXblaMeshes=0\n" "--net-test-stage 0x32" "" ;;
		# (2) a Stage Loader map, keyed by its mod's dir and its name
		modmap) runcase modmap "ModDir=\nMapMods=PD_Kakariko\nStartArmed=1\n" "ModDir=\nMapMods=PD_Kakariko\n" "--net-test-map Playground" "" ;;
		# the client's Mod.MapMods left the host's map mod out: mounted on demand
		modmount) runcase modmount "ModDir=\nMapMods=PD_Kakariko\nStartArmed=1\n" "ModDir=\nMapMods=\n" "--net-test-map Playground" "" ;;
		# the host's overlay mod is one the client has no copy of (a folder
		# with a files/ dir is a mod): the client leaves over it, named
		modmissing) mkdir -p "$OUT/mod_only/files"
			runcase modmissing "StartArmed=1\n" "ModDir=\n" "--moddir $OUT/mod_only --net-test-stage 0x32" "" "" 1 ;;
		# protocol 25, a PD mod as a live mode: the host enters an installed
		# data-only mod (as its Perfect Dark Mods row would) and hosts; the
		# client has the same mod and enters it at the match's stage change,
		# no restart (pdmodown); a client with no mods at all is served the
		# whole mod into memory and enters it from there, and is back to
		# Perfect Dark when the host's session ends under it (pdmodfetch)
		pdmodown) runcase pdmodown "StartArmed=1\n" "ModDir=\n" "--net-test-pdmod $PDMOD --net-test-stage 0x32" "" ;;
		pdmodfetch) CLIENTBIN=$(fetchclient) NOJOIN=1 CLIENTFRAMES=$((FRAMES + 900)) CLIENTT=240 runcase pdmodfetch "StartArmed=1\n" "ModDir=\nMapMods=\n" \
				"--net-test-pdmod $PDMOD --net-test-stage 0x32" "" ;;
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
	ge|geyolt|gegg|gelook)
		if skipped "$c"; then echo "skip $c: GoldenEye is not converted here"; continue; fi ;;
	gfvariant|gffetch)
		if skipped "$c"; then echo "skip $c: Goldfinger 64 is not converted here"; continue; fi ;;
	gfcs|gfcsfetch)
		if ! grep -q "^mod: .* installed: .*Goldfinger 64" "$OUT/$c-host.log" 2>/dev/null; then echo "skip $c: Goldfinger 64 is not converted here"; continue; fi ;;
	fetch)
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
	gelook)
		checkplay gelook "map Bunker from mod GoldenEye Arenas"
		grep -q "net: the match plays the host's look data: GoldenEye's Community Edition copies on" "$OUT/gelook-host.log" \
			&& pass "gelook: the host's HD look with the Community Edition chose its copies" \
			|| fail "gelook: the host did not play the Community Edition's copies: $(grep -m1 -o "the match plays the host's look data.*" "$OUT/gelook-host.log")"
		# (the summary before the client's own switch at 1200)
		grep "net: content client so far (tick 900)" "$C" | grep -q "Mod.XblaMeshes 0" \
			&& pass "gelook: the client played its own N64 look on the host's data" || fail "gelook: the client's Mod.XblaMeshes was not its own 0 at tick 900"
		for c in host client; do
			L=$OUT/gelook-$c.log
			grep -q "net: --net-test-look-switch: the look switched to the XBLA release's\|net: --net-test-look-switch: the look switched to the N64's" "$L" \
				&& pass "gelook: the $c switched its look in the match: $(grep -m1 -o 'net-test-look-switch: the look.*' "$L")" \
				|| fail "gelook: the $c did not switch its look in the match"
		done
		;;
	fetch)
		checkplay fetch "map Complex from mod GoldenEye Arenas"
		grep -q "^mod: 0 installed" "$C" && pass "fetch: the client had no mods of its own" || fail "fetch: the client's mod list was not empty: $(grep -m1 '^mod: .* installed' "$C" | cut -c1-80)"
		grep -q "net: content: GoldenEye Arenas fetched from the host and mounted for its maps: [0-9]* files" "$C" \
			&& pass "fetch: $(grep -o 'net: content: GoldenEye Arenas fetched.*' "$C" | head -1 | cut -c14-)" || fail "fetch: the client did not fetch GoldenEye Arenas"
		grep -q "net: the STAGE_LOAD kept through the fetch" "$C" && pass "fetch: the stage load waited for the folder" || fail "fetch: the stage load was not kept"
		grep -q "net: content: GoldenEye Arenas served to peer" "$OUT/fetch-host.log" && pass "fetch: host: $(grep -o 'served to peer.*' "$OUT/fetch-host.log" | head -1)" || fail "fetch: the host did not serve"
		lastline "$C" "net: content client" | grep -q "GoldenEye mode 1" && pass "fetch: the GoldenEye mode on the fetched conversion" || fail "fetch: not in the GoldenEye mode"
		chars fetch
		;;
	gfvariant)
		checkplay gfvariant "map Junkyard from mod Goldfinger 64"
		ct=$(lastline "$C" "net: content client")
		# the host's mode first: a host in GoldenEye's own sends no tag, and
		# the client is not the one at fault
		hs=$(lastline "$OUT/gfvariant-host.log" "net: --net-test-host: starting a match")
		echo "$hs" | grep -q "variant gf64)" && pass "gfvariant: the host in Goldfinger 64's mode" || fail "gfvariant: the host not in Goldfinger 64's mode: ${hs:-no start line}"
		echo "$ct" | grep -q 'GoldenEye mode 1 scenario 0 (.*) variant "gf64"' && pass "gfvariant: Goldfinger 64's mode from the host's RULES" || fail "gfvariant: not in Goldfinger 64's mode: $ct"
		set1=$(echo "$ct" | grep -o "weapon set [0-9]*" | awk '{print $3}')
		grep -q "net: content: Goldfinger 64 mounted for its maps for the host's choice" "$C" && pass "gfvariant: the client mounted Goldfinger 64 on demand" || fail "gfvariant: no on-demand mount in the client's log"
		[ -n "$set1" ] && pass "gfvariant: the client's weapon set number $set1 (the host's list block is Goldfinger's)" || fail "gfvariant: no weapon set in the client's summary"
		;;
	gfcs|gfcsfetch)
		checkplay "$c" "stock stage 0x1f"
		grep -q "geguns: a ROM hack's guns, chosen for the match" "$OUT/$c-host.log" \
			&& pass "$c: the host put Goldfinger 64's guns in on Complex" || fail "$c: the host played GoldenEye's own guns"
		grep -q "net: rules: Mod.CsHackGuns .*-> \"gf64\" (the host.s, for the match)" "$C" \
			&& pass "$c: the host's Mod.CsHackGuns in the client's rules" || fail "$c: the client never took the host's Mod.CsHackGuns"
		if [ "$c" = gfcs ]; then
			grep -q "net: content: Goldfinger 64 mounted for the host's ROM hack guns" "$C" \
				&& pass "gfcs: the client mounted Goldfinger 64 for the guns" || fail "gfcs: no mount for the guns in the client's log"
		else
			grep -q "net: content: Goldfinger 64 fetched from the host and mounted for its maps: [0-9]* files" "$C" \
				&& pass "gfcsfetch: $(grep -o 'net: content: Goldfinger 64 fetched.*' "$C" | head -1 | cut -c14-)" || fail "gfcsfetch: the client did not fetch Goldfinger 64"
		fi
		grep -q "geguns: a ROM hack's guns, chosen for the match" "$C" \
			&& pass "$c: the client put Goldfinger 64's guns in at the stage's load" || fail "$c: the client played GoldenEye's own guns"
		want "$c" "GoldenEye-numbered guns put in puppets' hands" "$(pupnum "$c" "GE guns held")"
		;;
	gffetch)
		checkplay gffetch "map Junkyard from mod Goldfinger 64"
		ct=$(lastline "$C" "net: content client")
		grep -q "^mod: 0 installed" "$C" && pass "gffetch: the client had no mods of its own" || fail "gffetch: the client's mod list was not empty: $(grep -m1 '^mod: .* installed' "$C" | cut -c1-80)"
		lastline "$OUT/gffetch-host.log" "net: --net-test-host: starting a match" | grep -q "variant gf64)" \
			&& pass "gffetch: the host in Goldfinger 64's mode" || fail "gffetch: the host not in Goldfinger 64's mode"
		grep -q "net: content: Goldfinger 64 fetched from the host and mounted for its maps: [0-9]* files" "$C" \
			&& pass "gffetch: $(grep -o 'net: content: Goldfinger 64 fetched.*' "$C" | head -1 | cut -c14-)" || fail "gffetch: the client did not fetch Goldfinger 64"
		grep -q "net: content: GoldenEye Arenas fetched from the host and mounted for its maps" "$C" \
			&& pass "gffetch: GoldenEye Arenas fetched for the characters: $(grep -o '[0-9]* of the host.s characters are GoldenEye.s; fetching' "$C" | head -1)" \
			|| fail "gffetch: GoldenEye Arenas not fetched for the host's characters"
		chars gffetch
		echo "$ct" | grep -q 'GoldenEye mode 1 scenario 0 (.*) variant "gf64"' && pass "gffetch: Goldfinger 64's mode on the fetched conversion" || fail "gffetch: not in Goldfinger 64's mode: $ct"
		grep -q "geguns: a ROM hack's own guns" "$C" && pass "gffetch: Goldfinger 64's own guns on the client" || fail "gffetch: the client did not load Goldfinger 64's guns"
		want gffetch "ticks with a GoldenEye gun in the client player's hand" "$(echo "$ct" | grep -o "a GoldenEye gun [0-9]*" | awk '{print $NF}')"
		;;
	xbla)
		checkplay xbla "stock stage 0x32"
		lastline "$C" "net: content client" | grep -q "Mod.XblaMeshes 0" && grep -q "net: rules applied" "$C" \
			&& pass "xbla: the client kept its own Mod.XblaMeshes (0) beside the host's 1" || fail "xbla: Mod.XblaMeshes not the client's own"
		;;
	modmap) checkplay modmap "map Playground from mod PD_Kakariko" ;;
	modmount)
		checkplay modmount "map Playground from mod PD_Kakariko"
		grep -q "net: content: PD_Kakariko mounted for its maps for the host's choice" "$C" && pass "modmount: the client mounted PD_Kakariko on demand" || fail "modmount: no on-demand mount in the client's log"
		grep -q "Stage Loader map mods differ" "$OUT/modmount-host.log" && pass "modmount: joined with other map mods (noted, not refused)" || fail "modmount: refused at CONNECT for its map mods"
		;;
	pdmodown)
		checkplay pdmodown "stage 0x32 of mod $PDMOD"
		H=$OUT/pdmodown-host.log
		grep -q "modmode: entered $PDMOD" "$H" && pass "pdmodown: the host entered $PDMOD live" || fail "pdmodown: the host did not enter $PDMOD"
		grep -q "net: content: entering for the host at the next stage from .*mods/$PDMOD" "$C" \
			&& pass "pdmodown: the client entered its own copy: $(grep -m1 -o 'net: content: entering.*' "$C" | cut -c14-150)" \
			|| fail "pdmodown: the client did not take its own copy: $(grep -m1 -o 'net: content: .*' "$C" | cut -c1-150)"
		grep -q "modmode: entered $PDMOD" "$C" && pass "pdmodown: the client entered $PDMOD with no restart" || fail "pdmodown: no modmode entry on the client"
		grep -q "net: match 1: the host's mod entered; stage 0x32 of mod $PDMOD loads as 0x32" "$C" \
			&& pass "pdmodown: the key resolved and the RULES applied again after the swap" || fail "pdmodown: no re-resolution after the swap"
		grep -qi "restart" "$C" && fail "pdmodown: a restart was asked for" || pass "pdmodown: no restart asked for"
		;;
	pdmodfetch)
		ALLOW124=pdmodfetch checkplay pdmodfetch "stage 0x32 of mod $PDMOD"
		grep -q "^mod: 0 installed" "$C" && pass "pdmodfetch: the client had no mods of its own" || fail "pdmodfetch: the client's mod list was not empty: $(grep -m1 '^mod: .* installed' "$C" | cut -c1-80)"
		grep -q "net: content: the mod $PDMOD fetched from the host into \$N/$PDMOD" "$C" \
			&& pass "pdmodfetch: $(grep -m1 -o 'net: content: the mod .* fetched.*' "$C" | cut -c14-)" || fail "pdmodfetch: the client did not fetch $PDMOD"
		grep -q "net: content: $PDMOD served to peer" "$OUT/pdmodfetch-host.log" && pass "pdmodfetch: host: $(grep -o 'served to peer.*' "$OUT/pdmodfetch-host.log" | head -1)" || fail "pdmodfetch: the host did not serve its mod"
		grep -q "modmode: entered $PDMOD" "$C" && pass "pdmodfetch: the client entered the served copy" || fail "pdmodfetch: no modmode entry on the client"
		sed -n '/net: session ended/,$p' "$C" | grep -q "modmode: back to Perfect Dark" \
			&& pass "pdmodfetch: back to Perfect Dark after the session: $(grep -m1 -o 'net: session ended.*' "$C" | cut -c1-80)" \
			|| fail "pdmodfetch: the client was not back to Perfect Dark after the session"
		;;
	modmissing)
		checkrefused modmissing "nomod" "The host plays with the mod mod_only, which is not installed here, and the host could not send it"
		grep -q "loaded mod differs from this machine's" "$OUT/modmissing-host.log" && pass "modmissing: joined with another mod loaded (noted, not refused at CONNECT)" || fail "modmissing: refused at CONNECT for its mod"
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
