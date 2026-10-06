#!/bin/bash
# nettwelvetest.sh — phase 8: a full room, and what it costs on the wire.
#
# Loopback, offscreen on the GPU: a --dedicated host (no player of its own,
# no window) and N clients in a one-minute Combat Simulator match on Skedar
# (0x32) with SIMS sims and explosive weapons, for each N in COUNTS (default
# 2 4 8 12: the old cap's four, then up to the full room). Every client walks,
# turns and fires from a per-tick script (--net-test-input) for the whole
# match (the last one's pause menu up for a second early on), sits on the end
# screen after the host's MATCH_END and leaves it at tick 4600 for the
# menus (the pause and end-screen dialogs draw the slot's number: slots
# 4-11 read past the ROM's four labels before phase 8). Checks, per run:
#
#   - nothing crashed: the host ran the match to MATCH_END; every client
#     had it, went back to the menus (their stage set up) and was still
#     running five seconds later (then it is stopped with SIGTERM);
#   - the last client's pause menu, put up from gdb early in the match, is
#     up (menu root MENUROOT_MPPAUSE, a dialog in its slot's menu) and draws;
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
rm -rf "$OUT"/*.log "$OUT"/*.events "$OUT"/save-* "$OUT"/table.txt
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
		--mp-weapons 10,13,22,28,23,1 --net-test-trace 30 --net-event-log "$OUT/h$n.events" &
	host=$!
	waitfor "$OUT/h$n.log" "net: hosting on UDP port" 60 || { echo "FAIL: the host did not start"; kill -TERM $host; exit 2; }
	for k in $(seq 1 "$n"); do
		game "c$n-$k" 560 "[Mod]\nStartArmed=1\n" --moddir "$MODDIR" --connect "127.0.0.1:$PORT" --net-test-join \
			--net-test-input "$OUT/client.script" --net-event-log "$OUT/c$n-$k.events" &
		pids="$pids $!"
		sleep 0.5
	done
	# the last client: its pause menu, then its end screen
	cp=""
	for k in $(seq 40); do cp=$(gamepid "c$n-$n"); [ -n "$cp" ] && break; sleep 0.5; done
	if [ -n "$cp" ]; then
		# its pause menu put up from gdb (a pad's START never reaches a
		# client's own player: see the notes) two seconds into the match,
		# while everyone is alive, and taken down a second later
		if waitfor "$OUT/c$n-$n.log" "net: match 1: GO" 200; then
			sleep 2
			# up to four tries a second apart: a player dead just then (an
			# early grenade) has no pause menu
			for k in 1 2 3 4; do
				gdb -p "$cp" -batch -ex 'set $p = g_Vars.currentplayernum' -ex "call (void)setCurrentPlayerNum($((n - 1)))" \
					-ex 'call (void)mpPushPauseDialog()' -ex 'call (void)setCurrentPlayerNum($p)' >/dev/null 2>&1
				sleep 0.6
				gdb -p "$cp" -batch -ex 'print g_MenuData.root' -ex "print g_Menus[$((n - 1))].curdialog != 0" -ex 'print g_NetTick' \
					-ex "print g_Vars.players[$((n - 1))]->isdead" 2>/dev/null | grep '^\$' | tr '\n' ' ' > "$OUT/n$n-pause.txt"
				grep -Eq '^\$1 = 4 \$2 = (1|true) ' "$OUT/n$n-pause.txt" && break
				sleep 1
			done
			shot1 "$cp" "$SHOTDIR/twelve-pause.png"
			gdb -p "$cp" -batch -ex "set var g_MpPlayerNum = $((n - 1))" -ex 'call (void)menuPopDialog()' -ex 'set var g_MpPlayerNum = 0' >/dev/null 2>&1
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
	if [ -f "$OUT/n$n-pause.txt" ]; then
		grep -Eq '^\$1 = 4 \$2 = (1|true) ' "$OUT/n$n-pause.txt" && pass "$n: slot $((n - 1))'s pause menu was up ($(cat "$OUT/n$n-pause.txt"))" \
			|| fail "$n: slot $((n - 1))'s pause menu was not up: $(cat "$OUT/n$n-pause.txt") (MENUROOT_MPPAUSE is 4)"
	fi
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
