#!/bin/bash
# nettwelvetest.sh — phase 8: a full room, and what it costs on the wire.
#
# Loopback, offscreen on the GPU: a --dedicated host (no player of its own,
# no window) and N clients in a one-minute Combat Simulator match on Skedar
# (0x32) with SIMS sims and explosive weapons, for each N in COUNTS (default
# 2 4 8 12: the old cap's four, then up to the full room). Every client walks,
# turns and fires from a per-tick script (--net-test-input) for the whole
# match (the last one presses START now and then: its pause menu), sits on the end
# screen after the host's MATCH_END and leaves it at tick 4600 for the
# menus (the pause and end-screen dialogs draw the slot's number: slots
# 4-11 read past the ROM's four labels before phase 8). Checks, per run:
#
#   - nothing crashed: the host ran the match to MATCH_END; every client
#     had it, went back to the menus (their stage set up) and was still
#     running five seconds later (then it is stopped with SIGTERM);
#   - the last client's pause menu comes up from its own pad's START in
#     the script (ticks 630, 930, ... 2130: a press toggles it) as
#     MENUROOT_MPPAUSE (4), draws, and goes down again (from START, or a
#     death closes it); every command it sent while the menu was up was
#     neutral (spec-players.md §6), and the host played on meanwhile
#     (others moved on the host between those ticks) while that player
#     was played as paused there: only neutral commands, its walk speed 0
#     (its script walks in every window). That player
#     is invincible on the host (--net-test-invincible-pad): a press while
#     it is dead is a respawn instead, and with twelve players and
#     explosives it was dead at all six presses in one run in five
#     (netcontenttest geyolt checks the respawn);
#   - every client has every human's name in PD's form, ending in "\n";
#   - every slot played: each walked on the host from its own commands
#     (a path of > 100 units between ticks 120 and 1800, respawn jumps left
#     out), every command in order and
#     none lost;
#   - kills and scores agree: every client's kill table (deaths and kill
#     counts per mpchr, built from DEATH events) equals the host's at every
#     sampled tick (every 300) and at MATCH_END;
#   - bandwidth: the host's snapshot bytes per client (mean, and max, the
#     keyframe) and ENet's bytes to each client between ticks 600 and 3000;
#     a client's mean snapshot stays within one datagram (NET_MTU, 1200 B)
#     and its downstream within BUDGET bytes a second (default 32000).
#
# A table of the runs (players, chrs, snapshot bytes, per-client and host
# upload rates) ends the output. Screenshots of the last client's pause
# menu and end screen (its slot's number at the top right) and of the menus
# after go to build/net-shots/twelve-*.png.
#
#   nettwelvetest.sh [BIN]   BIN a file name in build/ (pd.x86_64) or a path
#
# Env: OUT (build/nettwelve-out), PORT (27190), MODDIR (mod_allinone),
# COUNTS, SIMS (6), BUDGET, HOSTBIN (the host's binary, BIN's by default:
# an ASan build there watches the host alone). Up to thirteen games at once:
# run it alone.
# Exit status: 0 all good, 1 a check failed, 2 a run failed to start.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=${BUILD:-$ROOT/build}
OUT=${OUT:-$BUILD/nettwelve-out}; PORT=${PORT:-27190}
MODDIR=${MODDIR:-mod_allinone}
COUNTS=${COUNTS:-2 4 8 12}; SIMS=${SIMS:-6}; BUDGET=${BUDGET:-32000}
BIN=${1:-pd.x86_64}
case $BIN in /*) ;; */*) BIN=$(realpath "$BIN") ;; *) BIN=$BUILD/$BIN ;; esac
HOSTBIN=${HOSTBIN:-$BIN}
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
SHOTDIR=$BUILD/net-shots
rm -rf "$OUT"/*.log "$OUT"/*.events "$OUT"/save-* "$OUT"/table.txt "$OUT"/*-pauseshot
export SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-offscreen} SDL_GAMECONTROLLER_IGNORE_DEVICES=0x054c/0x0ce6,0x045e/0x028e \
	SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT=0x0000/0x0000 SDL_JOYSTICK_HIDAPI=0

status=0
fail() { echo "FAIL $*"; status=1; }
pass() { echo "ok   $*"; }

# the whole match: walk with a slow turn, fire taps, a held burst now and
# then; START
# at 4600 (on the end screen, 1000 ticks after it came up) leaves the match for the menus
{
	echo "# from to buttons sx sy rsx rsy mdx mdy"
	for t in $(seq 60 300 3300); do
		echo "$t $((t + 140)) 0008 0 0 0 0 0.3 0"
		echo "$((t + 150)) $((t + 151)) 2000 0 0 0 0 0 0"
		echo "$((t + 180)) $((t + 230)) 2000 0 0 0 0 -0.4 0"
		echo "$((t + 250)) $((t + 251)) 2000 0 0 0 0 0 0"
	done
	echo "4600 4601 1000 0 0 0 0 0 0"
} > "$OUT/client.script"
# the last client's: START every 300 ticks from 630 (its pause menu up and
# down from the pad, never from gdb), first in the file (the first line that
# covers a tick is the one played) and where no other line's buttons are
{ for t in 630 930 1230 1530 1830 2130; do echo "$t $t 1000 0 0 0 0 0 0"; done; cat "$OUT/client.script"; } > "$OUT/last.script"

# game LABEL TIMEOUT INI ARGS... &  ($! is timeout's pid; timeout passes a
# TERM on, and a dedicated host stops cleanly on it)
game() {
	local label=$1 t=$2 ini=$3; shift 3
	local save=$OUT/save-$label
	rm -rf "$save"; mkdir -p "$save"
	printf '%b' "$ini" > "$save/pd.ini"
	cd "$BUILD" || exit 2
	local bin=$BIN
	[ "${label#h}" != "$label" ] && bin=$HOSTBIN
	exec timeout -k 5 "$t" stdbuf -oL -eL "$bin" --savedir "$save" --skip-intro --no-sound "$@" > "$OUT/$label.log" 2>&1
}

waitfor() {
	local log=$1 pattern=$2 n=$3
	while [ "$n" -gt 0 ]; do
		grep -q -- "$pattern" "$log" 2>/dev/null && return 0
		sleep 1; n=$((n - 1))
	done
	return 1
}

gamepid() {
	local label=$1 p
	for p in $(pgrep -x "$(basename "$BIN" | cut -c1-15)") $(pgrep -x "$(basename "$HOSTBIN" | cut -c1-15)"); do
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

pos() { grep -m1 "net: trace tick $2 player $3 " "$1" | sed -n 's/.* pos \([^ ]*\) [^ ]* \([^ ]*\) theta.*/\1 \2/p'; }
dist() { awk -v a="$1" -v b="$2" 'BEGIN { split(a, p); split(b, q); printf "%.1f", sqrt((p[1]-q[1])^2 + (p[2]-q[2])^2) }'; }

printf "%-8s %-6s %-14s %-12s %-14s %-14s %s\n" players chrs "snap B mean" "snap B max" "client B/s" "host up B/s" "host up kbit/s" > "$OUT/table.txt"

run() {
	local n=$1 r=$OUT/n$1 k p pids="" host hp cp
	mkdir -p "$r"
	echo "== $n clients, $SIMS sims"
	game "h$n" 600 "[Mod]\nStartArmed=1\n[Net]\nMaxPlayers=12\n" --moddir "$MODDIR" --dedicated --host "$PORT" \
		--net-test-host "$n" --net-test-stage 0x32 --net-test-sims "$SIMS" --net-test-timelimit 1 --net-test-scorelimit 100 --net-test-teamscorelimit 400 --rng-seed 7 \
		--mp-weapons 10,13,22,28,23,1 --net-test-trace 30 --net-event-log "$OUT/h$n.events" \
		--net-test-invincible-pad $((n - 1)) &
	host=$!
	waitfor "$OUT/h$n.log" "net: hosting on UDP port" 60 || { echo "FAIL: the host did not start"; kill -TERM $host; exit 2; }
	mkdir -p "$BUILD/screenshots"
	local shotsbefore; shotsbefore=$(ls "$BUILD/screenshots" | sort)
	for k in $(seq 1 "$n"); do
		local script=$OUT/client.script extra=""
		# the last: its pause menu from its START, and the game's own picture of it
		[ "$k" = "$n" ] && script=$OUT/last.script && extra=--net-test-menu-shot
		game "c$n-$k" 560 "[Mod]\nStartArmed=1\n" --moddir "$MODDIR" --connect "127.0.0.1:$PORT" --net-test-join \
			--net-test-input "$script" --net-event-log "$OUT/c$n-$k.events" $extra &
		pids="$pids $!"
		sleep 0.5
	done
	# the last client: its pause menu (up from its script's START), then its
	# end screen
	cp=""
	for k in $(seq 40); do cp=$(gamepid "c$n-$n"); [ -n "$cp" ] && break; sleep 0.5; done
	if [ -n "$cp" ]; then
		# the picture the game took of its pause menu a third of a second
		# after it came up (a gdb stop would be late: a death shuts it)
		rm -f "$SHOTDIR/twelve-pause.png"
		if waitfor "$OUT/c$n-$n.log" "menu screenshot at tick [0-9]*$" 200; then
			for k in $(seq 50); do
				f=$(comm -13 <(echo "$shotsbefore") <(ls "$BUILD/screenshots" | sort) | tail -1)
				[ -n "$f" ] && { sleep 0.3; cp "$BUILD/screenshots/$f" "$SHOTDIR/twelve-pause.png"; touch "$OUT/n$n-pauseshot"; echo "     shot: $SHOTDIR/twelve-pause.png ($(grep -m1 -o "menu screenshot at tick [0-9]*" "$OUT/c$n-$n.log"))"; break; }
				sleep 0.1
			done
		fi
		waitfor "$OUT/c$n-$n.log" "the host ended it" 200 && sleep 4 && shot1 "$cp" "$SHOTDIR/twelve-endscreen.png"
	fi
	# each client back in the menus (the menus' stage set up), five seconds
	# on still running: stopped
	local deadline=$(( $(date +%s) + 200 )) left
	while [ "$(date +%s)" -lt $deadline ]; do
		left=0
		for k in $(seq 1 "$n"); do
			sed -n '/rules restored/,$p' "$OUT/c$n-$k.log" 2>/dev/null | grep -q "^setup: .* chr entries" || left=$((left + 1))
		done
		[ $left = 0 ] && break
		sleep 1
	done
	sleep 5
	[ "$n" -ge 9 ] && [ -n "$cp" ] && shot1 "$cp" "$SHOTDIR/twelve-menus.png"
	rm -f "$OUT/n$n-alive.txt"
	for k in $(seq 1 "$n"); do
		p=$(gamepid "c$n-$k")
		[ -n "$p" ] && { echo "client $k alive" >> "$OUT/n$n-alive.txt"; kill -TERM "$p"; }
	done
	k=0
	for p in $pids; do k=$((k + 1)); wait "$p"; echo "client $k exit $?"; done > "$OUT/n$n-exits.txt"
	# the dedicated host sits on its end screen: asked to quit
	hp=$(gamepid "h$n"); [ -n "$hp" ] && kill -TERM "$hp"
	wait "$host"
	check "$n"
}

# the last client's pause menu from its pad's START (see the top)
pausecheck() {
	local n=$1 c=$OUT/c$1-$1.log H=$OUT/h$1.log up down moved=0 s a b
	up=$(grep -m1 "this machine's menu up at tick" "$c" | sed -n 's/.*up at tick \([0-9]*\) (menu root \([0-9]*\)).*/\1 \2/p')
	down=$(grep -m1 "this machine's menu down at tick" "$c" | sed -n 's/.*down at tick \([0-9]*\): \([0-9]*\) commands.*, \([0-9]*\) not neutral/\1 \2 \3/p')
	set -- $up; local uptick=${1:-} root=${2:-}
	set -- $down; local downtick=${1:-} cmds=${2:-0} busy=${3:-}
	if [ -z "$uptick" ]; then
		fail "$n: slot $((n - 1))'s pause menu never came up from its pad's START"; return
	fi
	case $uptick in 63[0-2]|93[0-2]|123[0-2]|153[0-2]|183[0-2]|213[0-2]) ;; *) fail "$n: slot $((n - 1))'s menu came up at tick $uptick, not at a START of its script" ;; esac
	# (the game's own line: its slot's menu has a dialog, under the root it
	# names; a look from gdb a second later can find it shut by a death)
	[ "$root" = 4 ] && pass "$n: slot $((n - 1))'s pause menu up from its pad's START at tick $uptick (MENUROOT_MPPAUSE)" \
		|| fail "$n: slot $((n - 1))'s menu at tick $uptick: root ${root:-?} (MENUROOT_MPPAUSE is 4)"
	[ -e "$OUT/n$n-pauseshot" ] && pass "$n: the game's own picture of it: $SHOTDIR/twelve-pause.png" \
		|| fail "$n: no picture of the pause menu (--net-test-menu-shot)"
	if [ -z "$downtick" ]; then
		fail "$n: slot $((n - 1))'s pause menu never went down"; return
	fi
	[ "$busy" = 0 ] && [ "$cmds" -ge 30 ] && pass "$n: down again at tick $downtick; all $cmds commands sent meanwhile neutral" \
		|| fail "$n: menu down at tick $downtick: $busy of $cmds commands sent while it was up were not neutral"
	# the host played on meanwhile: another slot moved between those ticks
	for s in $(seq 0 $((n - 2))); do
		a=$(pos "$H" $(( (uptick / 30) * 30 )) "$s"); b=$(pos "$H" $(( (downtick / 30 + 1) * 30 )) "$s")
		[ -n "$a" ] && [ -n "$b" ] && awk -v d="$(dist "$a" "$b")" 'BEGIN { exit !(d > 1) }' && moved=$((moved + 1))
	done
	[ "$moved" -ge 1 ] && pass "$n: the host played on while it was up ($moved other slots moved)" \
		|| fail "$n: nobody else moved on the host between ticks $uptick and $downtick"
	# and the host played it as paused: from the first trace 30 ticks after
	# each up to the down (alive throughout), the host played only neutral
	# commands for that slot (the trace's busy count, the commands played
	# with any input, stands still) and its walk's own speed stayed 0 at
	# every trace, though its script walks 140 ticks in every 300. Its place
	# is printed, not gated: shots and explosions push a player standing
	# still (seen: 1100 units over 150 ticks, the push nonzero, busy and
	# speed 0), and this one is invincible
	python3 - "$c" "$H" $((n - 1)) <<'PY2' || { status=1; echo "FAIL $n: slot $((n - 1)) was not played as paused on the host while its menu was up"; }
import sys, re
c, h, slot = sys.argv[1], sys.argv[2], int(sys.argv[3])
ups, wins = None, []
for l in open(c):
    m = re.search(r"this machine's menu (up|down) at tick (\d+)", l)
    if m and m.group(1) == 'up': ups = int(m.group(2))
    elif m and ups is not None: wins.append((ups, int(m.group(2)))); ups = None
tr = {}
for l in open(h):
    m = re.match(r"net: trace tick (\d+) player %d .* pos (\S+) \S+ (\S+) theta.* dead (\d+) busy (\d+) speed (\S+) (\S+)" % slot, l)
    if m: tr[int(m.group(1))] = (float(m.group(2)), float(m.group(3)), int(m.group(4)), int(m.group(5)), float(m.group(6)), float(m.group(7)))
seen, far, bad = 0, 0.0, []
for a, b in wins:
    ts = [t for t in range((a + 30 + 29) // 30 * 30, b + 1, 30) if t in tr]
    if len(ts) < 3 or any(tr[t][2] for t in ts): continue
    seen += 1
    far = max([far] + [((tr[t][0] - tr[ts[0]][0]) ** 2 + (tr[t][1] - tr[ts[0]][1]) ** 2) ** 0.5 for t in ts])
    busy = tr[ts[-1]][3] - tr[ts[0]][3]
    walk = [t for t in ts if tr[t][4] != 0 or tr[t][5] != 0]
    if busy or walk: bad.append((a, b, "%d commands with input" % busy, "walking at ticks %s" % walk))
print("     menu up %s: %d windows looked at (alive throughout); pushed at most %.0f units meanwhile" % (" ".join("%d-%d" % w for w in wins), seen, far))
if bad: print("     not paused: %s" % bad); sys.exit(1)
if not seen: print("     no window to look at (dead, or too short)"); sys.exit(1)
print("ok   slot %d: the host played only neutral commands for it while its menu was up, its walk speed 0 (%d windows)" % (slot, seen))
PY2
}

check() {
	local n=$1 H=$OUT/h$1.log c k s d st walked=0 inorder=0 ok=0
	grep -q "renderD128: Permission denied" "$OUT"/c"$n"-*.log && fail "$n: a run fell back to llvmpipe"
	grep -qE "FATAL|Segmentation|Aborted|AddressSanitizer" "$H" && fail "$n: the host crashed"
	grep -q "scores and awards sent" "$H" && pass "$n: the host ended the match (MATCH_END)" || fail "$n: no MATCH_END from the host"
	for k in $(seq 1 "$n"); do
		c=$OUT/c$n-$k.log
		x=$(grep "^client $k exit" "$OUT/n$n-exits.txt" | awk '{print $4}')
		if grep -qE "FATAL|Segmentation|Aborted|AddressSanitizer" "$c"; then
			echo "     client $k crashed: $(grep -m1 -oE "FATAL.*|Segmentation.*|Aborted.*|AddressSanitizer.*" "$c")"
		elif ! grep -q "the host ended it" "$c"; then
			echo "     client $k: no MATCH_END ($(grep -m1 -o "session ended.*" "$c"))"
		elif ! sed -n '/rules restored/,$p' "$c" | grep -q "^setup: .* chr entries"; then
			echo "     client $k: never left the end screen for the menus"
		elif ! grep -q "^client $k alive" "$OUT/n$n-alive.txt" 2>/dev/null; then
			echo "     client $k: gone before it was stopped"
		elif [ "$x" != 0 ] && [ "$x" != 143 ]; then
			echo "     client $k: exit $x"
		else
			ok=$((ok + 1))
		fi
	done
	pausecheck "$n"
	# every human's name in PD's form, "text\n" (else the end screen's
	# "Title:" row draws over its name row: netNameSet)
	k=$(cat "$OUT"/c"$n"-*.log | grep -c "net: the match's players:")
	nn=$(cat "$OUT"/c"$n"-*.log | grep -c "net: the match's players:.*(its name has no newline)")
	[ "$k" -ge $((n * n)) ] && [ "$nn" = 0 ] && pass "$n: every client had all $n names with PD's newline ($k lines)" \
		|| fail "$n: names: $k lines of $((n * n)), $nn without their newline"
	[ "$ok" = "$n" ] && pass "$n: all $n clients played, had MATCH_END, left the end screen for the menus and ran on" || fail "$n: $ok of $n clients came through"

	for s in $(seq 0 $((n - 1))); do
		d=$(grep "net: trace tick [0-9]* player $s " "$H" | sed -n 's/^net: trace tick \([0-9]*\) .* pos \([^ ]*\) [^ ]* \([^ ]*\) theta.*/\1 \2 \3/p' \
			| awk '$1 >= 120 && $1 <= 1800 { if (n++) { d = sqrt(($2 - x)^2 + ($3 - z)^2); if (d < 200) m += d } x = $2; z = $3 } END { printf "%.1f", m }')
		awk -v d="$d" 'BEGIN { exit !(d > 100) }' && walked=$((walked + 1)) || echo "     slot $s walked only ${d:-?}"
		st=$(grep "net: slot $s commands at" "$H" | tail -1)
		echo "$st" | grep -q "0 out of order.*lost 0" && inorder=$((inorder + 1)) || echo "     slot $s: ${st:-no command stats}"
	done
	[ "$walked" = "$n" ] && pass "$n: slots 0-$((n - 1)) each walked on the host from their own commands" || fail "$n: $walked of $n slots walked"
	[ "$inorder" = "$n" ] && pass "$n: every slot's commands played in order, none lost" || fail "$n: $inorder of $n slots played every command in order"

	python3 - "$n" "$OUT" "$BUDGET" <<'PY' || status=1
import sys, re
n, out, budget = int(sys.argv[1]), sys.argv[2], int(sys.argv[3])
st = 0
def bad(m):
    global st; print("FAIL %d: %s" % (n, m)); st = 1
def ok(m): print("ok   %d: %s" % (n, m))
def ktab(path):
    t = {}
    for l in open(path):
        p = l.split()
        if p and p[0] == 'K': t[(p[1], p[2])] = " ".join(p[3:])
    return t
hk = ktab("%s/h%d.events" % (out, n))
hend = [v for (w, _), v in hk.items() if w == 'end']
endtick = min([int(t) for (w, t) in hk if w == 'end'] or [1 << 30])
samples = sorted(int(t) for (w, t) in hk if w == 'tick' and int(t) <= endtick)
deaths = sum(1 for l in open("%s/h%d.events" % (out, n)) if l.split()[2:3] == ['death'] and l[0] == 'E')
agree = 0
for k in range(1, n + 1):
    ck = ktab("%s/c%d-%d.events" % (out, n, k))
    diffs = [t for t in samples if ('tick', str(t)) in ck and ck[('tick', str(t))] != hk[('tick', str(t))]]
    seen = sum(1 for t in samples if ('tick', str(t)) in ck)
    cend = [v for (w, _), v in ck.items() if w == 'end']
    if not diffs and seen >= 10 and hend and cend and cend[0] == hend[0]:
        agree += 1
    else:
        print("     client %d: %d samples, %d differ%s; end %s" % (k, seen, len(diffs), " (first tick %d)" % diffs[0] if diffs else "",
              "equal" if hend and cend and cend[0] == hend[0] else "host [%s] client [%s]" % (hend[0] if hend else '-', cend[0] if cend else '-')))
if agree == n and deaths > 0:
    ok("kill tables: all %d clients equal the host's at all %d samples and MATCH_END (%d deaths)" % (n, len(samples), deaths))
else:
    bad("kill tables: %d of %d clients agree (%d deaths)" % (agree, n, deaths))
# bandwidth
log = open("%s/h%d.log" % (out, n)).read()
def snaps(tick):
    r = {}
    for m in re.finditer(r"net: snap slot (\d+) so far \(tick %d\): (\d+) sent, bytes mean (\d+) min (\d+) max (\d+), keyframes (\d+).*?rate (\d+) Hz" % tick, log):
        r[int(m.group(1))] = tuple(int(x) for x in m.groups()[1:])
    return r
# the window's own numbers: sent, mean, min (cumulative), max (cumulative), keyframes, rate
s0, s1 = snaps(600), snaps(3000)
snap = {}
for k in s1:
    if k in s0 and s1[k][0] > s0[k][0]:
        sent = s1[k][0] - s0[k][0]
        snap[k] = (sent, round((s1[k][0] * s1[k][1] - s0[k][0] * s0[k][1]) / sent), s1[k][2], s1[k][3], s1[k][4] - s0[k][4], s1[k][5])
def traffic(tick):
    r = {}
    for m in re.finditer(r"net: traffic slot (\d+) so far \(tick %d\): sent (\d+) bytes, received (\d+) bytes" % tick, log):
        r[int(m.group(1))] = (int(m.group(2)), int(m.group(3)))
    h = re.search(r"net: traffic host so far \(tick %d\): (\d+) clients, sent (\d+) bytes, received (\d+) bytes" % tick, log)
    return r, (int(h.group(2)), int(h.group(3))) if h else None
t0, t1 = 600, 3000
a, ha = traffic(t0); b, hb = traffic(t1)
secs = (t1 - t0) / 60.0
players = [s for s in range(n) if s in snap]
if len(players) != n:
    bad("snapshot stats for %d of %d slots" % (len(players), n))
    sys.exit(1)
means = [snap[s][1] for s in players]; maxes = [snap[s][3] for s in players]
rates = {s: (b[s][0] - a[s][0]) / secs for s in players if s in a and s in b}
hup = (hb[0] - ha[0]) / secs if ha and hb else 0
hdown = (hb[1] - ha[1]) / secs if ha and hb else 0
for s in players:
    sent, mean, mn, mx, keys, hz = snap[s]
    print("     slot %2d: %5d snaps at %d Hz, bytes mean %4d min %4d max %4d (keyframes %d), ENet %6.0f B/s down" % (s, sent, hz, mean, mn, mx, keys, rates.get(s, -1)))
mean = sum(means) / len(means)
cmax = max(rates.values()) if rates else 1e9
if max(means) <= 1200: ok("snapshots: mean %.0f B (worst client %d), max %d B (a keyframe), within one %d B datagram" % (mean, max(means), max(maxes), 1200))
else: bad("snapshots: a client's mean %d B is over a datagram" % max(means))
if len(rates) == n and cmax <= budget: ok("per client: %.0f-%.0f B/s down (budget %d); host upload %.0f B/s (%.0f kbit/s), host download %.0f B/s" % (min(rates.values()), cmax, budget, hup, hup * 8 / 1000, hdown))
else: bad("per client: worst %.0f B/s against a budget of %d (%d of %d measured)" % (cmax, budget, len(rates), n))
chrs = n + int(re.search(r"with (\d+) sims", log).group(1)) if re.search(r"with (\d+) sims", log) else n
with open("%s/table.txt" % out, "a") as f:
    f.write("%-8d %-6d %-14.0f %-12d %-14.0f %-14.0f %.0f\n" % (n, chrs, mean, max(maxes), sum(rates.values()) / max(1, len(rates)), hup, hup * 8 / 1000))
sys.exit(st)
PY
}

for n in $COUNTS; do
	run "$n" 2>&1
done | tee "$OUT/run.log"
grep -q "^FAIL" "$OUT/run.log" && status=1
echo "== bandwidth (ticks 600-3000, dedicated host, $SIMS sims)"
cat "$OUT/table.txt"
exit $status
