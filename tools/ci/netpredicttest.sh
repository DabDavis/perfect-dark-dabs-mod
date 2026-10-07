#!/bin/bash
# netpredicttest.sh — does the client's own player run ahead and agree with the host?
#
# Loopback (PLANS/NETPLAY.md "Prediction", phase 5a): a listen host and one
# client on 127.0.0.1, offscreen on the GPU, the 0x32 match with no sims
# (flat ground, nothing to bump into), Start Armed, jumping on. The client's
# pad and mouse come from a per-tick script (--net-test-input): walk,
# strafe, turn with the mouse and the stick, jump, crouch, fire, look up and
# down, over and over. The client logs every tick of its own player and
# every block it compares (--net-predict-log, netpredict.c). Checks, per run:
#
#   - matched: the share of the host's blocks whose state for command N
#     agrees with this client's after N: the position within 0.5 units, the
#     angles within 0.05 degrees and the discrete state (move mode, ladder,
#     falling, crouch, aiming) the same - everything that would make the
#     client correct (the position alone is reported beside it);
#   - corrections a minute (at most MAXCPM), and the mean and largest
#     position error at N;
#   - no snaps (corrections too large to ease) but respawns and teleports;
#   - no block refused for a future command or a bad head animation;
#   - at once: on the first tick of each walk in the script the client
#     moves its player as far as the host moves it for the same command
#     (the host's --net-predict-log), on that tick, not a round trip later.
#
# Runs (CASES):
#   clean   --net-sim 0,0 on both ends
#   lag     --net-sim 0,75 on both ends (150 ms round trip); screenshots of
#           the client go to build/net-shots/predict-lag-*.png
#   loss    --net-sim 2,75 on both ends (150 ms, 2% of datagrams lost)
#   wine    the Windows build (build-win/pd.x86_64.exe) as the client under
#           wine on the GPU's display (:0), the Linux host, 150 ms round trip
#           (all of it held by the host's simulator: the Windows build's own
#           never lets a connection finish under wine): prediction across
#           two compilers
#   sims    the lag run with 6 sims (bumps, deaths, respawns), reported: no
#           snaps but respawns, and after a respawn the client agrees with
#           the host at once (no run of corrections while the host's blocks
#           for ticks it spent dead here come in)
#   time    a 1-minute time limit: the host ends it at 3600 ticks (give or
#           take a few) and the client's level clock agrees
#
#   netpredicttest.sh [BIN]   BIN a file name in build/ (pd.x86_64) or a path
#
# Env: OUT (build/netpredict-out), PORT (27300; uses PORT..PORT+5), MODDIR
# (mod_allinone), CASES (clean lag loss wine sims time), FRAMES (3000),
# WINEEXE (build-win/pd.x86_64.exe), MINMATCH (99, the matched share,
# percent; the lossy, wine and sims runs are reported against it too),
# MAXCPM (10, corrections a minute), AIM (10: the script's aim button, R,
# held while the mouse turns and looks; 0 for none), SIMS (0).
# Exit status: 0 all good, 1 a check failed, 2 a run failed to start.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=${BUILD:-$ROOT/build}
OUT=${OUT:-$BUILD/netpredict-out}; PORT=${PORT:-27300}
MODDIR=${MODDIR:-mod_allinone}
CASES=${CASES:-clean lag loss wine sims time}
MAXCPM=${MAXCPM:-10}
FRAMES=${FRAMES:-3000}
MINMATCH=${MINMATCH:-99}
WINEEXE=${WINEEXE:-$ROOT/build-win/pd.x86_64.exe}
BIN=${1:-pd.x86_64}
case $BIN in /*) ;; */*) BIN=$(realpath "$BIN") ;; *) BIN=$BUILD/$BIN ;; esac
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
SHOTDIR=$BUILD/net-shots
rm -f "$OUT"/*.log "$OUT"/*.plog
# no controller may reach a run (see replaytest.sh)
export SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-offscreen} SDL_GAMECONTROLLER_IGNORE_DEVICES=0x054c/0x0ce6,0x045e/0x028e \
	SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT=0x0000/0x0000 SDL_JOYSTICK_HIDAPI=0

status=0
fail() { echo "FAIL $*"; status=1; }
pass() { echo "ok   $*"; }

# the client's script: from to buttons(hex) sx sy rsx rsy mdx mdy, by the
# match's tick. U_CBUTTONS 0x0008 walks, L/R_CBUTTONS 0x0002/0x0001
# strafe, B 0x4000 is use (a jump where nothing is in reach), 0x20000000 is
# the full crouch, Z 0x2000 fires. A 600-tick block, five times over.
python3 - "$OUT/client.script" <<'PY'
import sys, os
AIM = int(os.environ.get("AIM", "10"), 16)
lines = []
for k in range(5):
    b = 60 + k * 600
    lines += [
        (b + 0,   b + 89,  0x0008, 0, 0, 0, 0, 0, 0),        # walk
        (b + 110, b + 169, 0x0001, 0, 0, 0, 0, 0, 0),        # strafe right
        (b + 170, b + 229, 0x0008 | AIM, 0, 0, 0, 0, 0.45, 0.02),  # walk turning and looking, aiming (mouse; R 0x0010)
        (b + 230, b + 279, 0x0000, 40, 70, 0, 0, 0, 0),      # stick walk and turn
        (b + 300, b + 300, 0x0008, 0, 0, 0, 0, 0, 0),        # walk, then
        (b + 301, b + 302, 0x4008, 0, 0, 0, 0, 0, 0),        # ... jump
        (b + 303, b + 359, 0x0008, 0, 0, 0, 0, 0, 0),
        (b + 380, b + 439, 0x20000008, 0, 0, 0, 0, 0, 0),    # crouched, walking
        (b + 460, b + 461, 0x2002, 0, 0, 0, 0, 0, 0),        # fire, strafing left
        (b + 462, b + 489, 0x0002, 0, 0, 0, 0, 0, 0),
        (b + 490, b + 491, 0x2002, 0, 0, 0, 0, 0, 0),
        (b + 492, b + 519, 0x0002 | AIM, 0, 0, 0, 0, -0.3, 0.03),  # strafing, aiming, turning and looking up
        (b + 520, b + 549, 0x0000, 0, 0, 0, 0, 0, -0.03),    # looking down
    ]
with open(sys.argv[1], "w") as f:
    for l in lines:
        f.write("%d %d %x %d %d %d %d %g %g\n" % l)
PY

INI="[Mod]\nStartArmed=1\nJumpHeight=2\n"

# game LABEL TIMEOUT ARGS... &  (the Linux build)
game() {
	local label=$1 t=$2; shift 2
	local save=$OUT/save-$label
	rm -rf "$save"; mkdir -p "$save"
	printf '%b' "$INI" > "$save/pd.ini"
	cd "$BUILD" || exit 2
	exec timeout -k 5 "$t" stdbuf -oL -eL "$BIN" --savedir "$save" --skip-intro --no-sound "$@" > "$OUT/$label.log" 2>&1
}

# winegame LABEL TIMEOUT ARGS... &  (the Windows build under wine, run from
# a directory of its own with the DLLs it needs and the ROM and mods linked)
winegame() {
	local label=$1 t=$2; shift 2
	local save=$OUT/save-$label dir=$OUT/win
	rm -rf "$save"; mkdir -p "$save" "$dir"
	printf '%b' "$INI" > "$save/pd.ini"
	cp -f "$WINEEXE" "$dir/pd.x86_64.exe"
	local dll
	# (CLAUDE-notes/windows-build.md: SDL2 from the mingw prefix, the win32
	# threads libgcc)
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
	# on the GPU's display (wine has no offscreen video), woken first
	export DISPLAY=${WINEDISPLAY_X:-:0} WINEDEBUG=-all
	unset SDL_VIDEODRIVER
	xset dpms force on 2>/dev/null
	exec timeout -k 5 "$t" wine ./pd.x86_64.exe --savedir "$(winepath -w "$save" 2>/dev/null || echo "$save")" --skip-intro --no-sound "$@" > "$OUT/$label.log" 2>&1
}

waitfor() {
	local log=$1 pattern=$2 n=$3
	while [ "$n" -gt 0 ]; do
		grep -q -- "$pattern" "$log" 2>/dev/null && return 0
		sleep 1; n=$((n - 1))
	done
	return 1
}

# run LABEL PORT FRAMES WINE SIMARGS [EXTRA...]: host and client, the client
# scripted and logging its prediction
run() {
	local label=$1 port=$2 frames=$3 wine=$4 sim=$5; shift 5
	local simargs= csimargs=
	[ -n "$sim" ] && simargs="--net-sim $sim"
	csimargs=$simargs
	# the Windows build's own simulator never lets a connection finish under
	# wine (unexplained): the host holds both ways' worth of delay instead
	if [ "$wine" = 1 ] && [ -n "$sim" ]; then
		local d=${sim#*,}
		simargs="--net-sim ${sim%%,*},$((d * 2))"
		csimargs=
	fi
	game "$label-host" 300 --moddir "$MODDIR" --host "$port" --net-test-host 1 --net-test-stage 0x32 --net-predict-log "$OUT/$label-host.plog" \
		--net-test-sims ${SIMS:-0} --rng-seed 1 --mp-weapons 1,4,7,9,10,1 --endless \
		--exit-frame $((frames + 400)) $simargs ${HOSTARGS:-} "$@" &
	local host=$!
	waitfor "$OUT/$label-host.log" "net: hosting on UDP port" 60 || { echo "FAIL: $label host did not start"; kill -TERM $host; exit 2; }
	if [ "$wine" = 1 ]; then
		winegame "$label-client" 290 --moddir "$MODDIR" --connect "127.0.0.1:$port" --net-test-join \
			--net-test-input "$(winepath -w "$OUT/client.script" 2>/dev/null)" \
			--net-predict-log "$(winepath -w "$OUT/$label-client.plog" 2>/dev/null)" \
			--exit-frame "$frames" $csimargs ${CLIENTARGS:-} "$@" &
	else
		game "$label-client" 290 --moddir "$MODDIR" --connect "127.0.0.1:$port" --net-test-join \
			--net-test-input "$OUT/client.script" --net-predict-log "$OUT/$label-client.plog" \
			$([ "$label" = lag ] && echo --net-predict-shots 900,1250,1470) \
			--exit-frame "$frames" $csimargs ${CLIENTARGS:-} "$@" &
	fi
	local client=$!
	if [ "$label" = lag ]; then
		# the client's own screenshots (--net-predict-shots, below), copied
		# out as they land: mid-walk, mid-turn and crouched
		local before t f n
		before=$(ls "$BUILD/screenshots" 2>/dev/null | sort)
		mkdir -p "$SHOTDIR"
		for t in 900 1250 1470; do
			waitfor "$OUT/$label-client.log" "screenshot at tick $t" 200 || continue
			for n in $(seq 50); do
				f=$(comm -13 <(echo "$before") <(ls "$BUILD/screenshots" 2>/dev/null | sort) | head -1)
				[ -n "$f" ] && break
				sleep 0.1
			done
			[ -n "$f" ] && { sleep 0.3; cp "$BUILD/screenshots/$f" "$SHOTDIR/predict-lag-$t.png"; echo "     shot: $SHOTDIR/predict-lag-$t.png"; before=$(ls "$BUILD/screenshots" 2>/dev/null | sort); }
		done
	fi
	wait "$client"; echo "$label: client exit $?"
	wait "$host"; echo "$label: host exit $?"
}

# measure LABEL: the client's prediction log and stats line
measure() {
	local label=$1
	python3 - "$OUT/$label-client.plog" "$label" "$MINMATCH" "$OUT/$label-host.plog" "$MAXCPM" <<'PY'
import sys, math
path, label, minmatch, hpath, maxcpm = sys.argv[1], sys.argv[2], float(sys.argv[3]), sys.argv[4], float(sys.argv[5])
# the host's log: each remote player after each host tick, by the command it
# played (the last line for a command: dry ticks after it move it on)
H = {}
try:
    for line in open(hpath):
        w = line.split()
        if w and w[0] == "H":
            H[int(w[3])] = (float(w[7]), float(w[8]), float(w[9]))
except OSError:
    pass
T = {}
C = []
R = []
try:
    lines = open(path).read().split("\n")
except OSError:
    print("FAIL %s: no prediction log" % label); sys.exit(1)
for line in lines:
    w = line.split()
    if not w:
        continue
    if w[0] == "T" and len(w) >= 12:
        # tick buttons sx sy mdx x y z theta verta moved ...
        T[int(w[1])] = (int(w[2], 16), int(w[3]), int(w[4]), float(w[5]), float(w[6]), float(w[7]), float(w[8]), float(w[9]), float(w[10]), int(w[11]))
    elif w[0] == "C":
        C.append((int(w[1]), int(w[2]), float(w[3]), int(w[4]), int(w[5]), int(w[6])))
    elif w[0] == "R":
        R.append((int(w[1]), int(w[2]), float(w[3]), float(w[4]), int(w[5]), int(w[6])))
ok = True
if not C:
    print("FAIL %s: no blocks compared" % label); sys.exit(1)
ticks = max(T) - min(T) + 1 if T else 1
posmatch = sum(1 for c in C if c[3] == 0)
allmatch = sum(1 for c in C if c[3] == 0 and c[4] == 0 and c[5] == 0)
corr = [r for r in R if r[5] == 0]
errs = [r[2] for r in corr]
shifts = [r[3] for r in corr]
mins = ticks / 3600.0
share = 100.0 * allmatch / len(C)
cpm = len(corr) / mins if mins else 0
print("     %s: %d ticks, %d blocks compared: all state matched at the command %.2f%% (position within 0.5 %.2f%%); corrections %d (%.1f a minute), error at the command mean %.2f max %.2f, moved now mean %.2f max %.2f; replayed ticks a correction mean %.1f"
      % (label, ticks, len(C), share, 100.0 * posmatch / len(C), len(corr), cpm,
         sum(errs) / len(errs) if errs else 0, max(errs) if errs else 0, sum(shifts) / len(shifts) if shifts else 0,
         max(shifts) if shifts else 0, sum(r[4] for r in corr) / len(corr) if corr else 0))
if share < minmatch:
    print("FAIL %s: %.2f%% of blocks matched in all state, under %g%%" % (label, share, minmatch)); ok = False
if cpm > maxcpm:
    print("FAIL %s: %.1f corrections a minute, over %g" % (label, cpm, maxcpm)); ok = False
# after each respawn (an abs R line) the client's blocks agree at once: any
# correction in the 60 commands after one is reported, more than one fails
for r in R:
    if r[5] == 1:
        after = [c for c in corr if 0 < c[0] - r[0] <= 60]
        print("     %s: respawn or teleport at command %d: %d corrections in the 60 commands after%s"
              % (label, r[0], len(after), (" (" + ", ".join("%d err %.2f" % (c[0], c[2]) for c in after) + ")") if after else ""))
        if len(after) > 1:
            print("FAIL %s: a run of corrections after the respawn at command %d" % (label, r[0])); ok = False
# the walks: forward pressed from nothing, with control (the tick ran the
# walk, netpredict.c's "moved"), not crouched (the game itself spends that
# first tick going down)
starts = [t for t in sorted(T) if t - 1 in T and T[t][0] & 0x0008 and not T[t][0] & 0x60000000
          and not T[t - 1][0] & 0x000f and T[t - 1][2] == 0 and T[t][9]]
# at once: on a walk's first tick the client moves its player as far as the
# host moves it for the same command (a round trip before any snapshot of
# it), and walks do move it then. From nothing on the host too: a walk
# begun while the host had the player moving on the command before (a sim's
# shot or blast pushing it, which this machine learns of only from the
# blocks after; seen: 2.4 units a tick, then 4.1 against 0.8 on the walk's
# first) is counted apart, not compared
moved, pushed = [], []
for t in starts:
    if t - 1 in H and t - 2 in H and math.hypot(H[t - 1][0] - H[t - 2][0], H[t - 1][2] - H[t - 2][2]) > 0.05:
        pushed.append(t)
        continue
    a, b = T[t - 1], T[t]
    d = math.hypot(b[4] - a[4], b[6] - a[6])
    hd = math.hypot(H[t][0] - H[t - 1][0], H[t][2] - H[t - 1][2]) if t in H and t - 1 in H else None
    moved.append((t, d, hd))
print("     %s: %d walks started (%d more while the host had the player pushed); on its own tick each moved the player here / on the host for that command: %s"
      % (label, len(moved), len(pushed), "  ".join("%.2f/%s" % (m[1], "%.2f" % m[2] if m[2] is not None else "-") for m in moved[:9])))
apart = [m for m in moved if m[2] is not None and abs(m[1] - m[2]) > 0.05]
if not moved or not any(m[1] > 0.3 for m in moved):
    print("FAIL %s: no walk moved the player on its first tick" % label); ok = False
elif apart:
    print("FAIL %s: %d walks' first ticks moved the player otherwise than the host did (%s)" % (label, len(apart), apart[:4])); ok = False
sys.exit(0 if ok else 1)
PY
}

# snaps LABEL: no correction too large to ease, but respawns and teleports
snaps() {
	local label=$1 line n
	line=$(grep -h "net: prediction at the match's end\|net: prediction at exit" "$OUT/$label-client.log" | tail -1)
	[ -n "$line" ] || { fail "$label: no prediction summary in the client's log"; return; }
	echo "     $label: ${line#*net: }" | cut -c1-400
	n=$(echo "$line" | sed -n 's/.*ticks replayed [0-9]*, snaps \([0-9]*\),.*/\1/p')
	local fut head
	fut=$(echo "$line" | sed -n 's/.*future commands \([0-9]*\),.*/\1/p')
	head=$(echo "$line" | sed -n 's/.*head data refused \([0-9]*\).*/\1/p')
	[ "${fut:-x}" = 0 ] && [ "${head:-x}" = 0 ] && pass "$label: no blocks refused for a future command or head data" \
		|| fail "$label: blocks refused: future commands '$fut', head data '$head'"
	if [ "$label" = wine ]; then
		echo "     $label: $n snaps (reported, not gated)"
	else
		[ "${n:-x}" = 0 ] && pass "$label: no snaps (respawn/teleport snaps: $(echo "$line" | sed -n 's/.*teleport snaps \([0-9]*\),.*/\1/p'))" || fail "$label: $n snaps"
	fi
}

check_run() {
	local label=$1
	grep -q "renderD128: Permission denied" "$OUT/$label"-*.log && fail "$label: a run fell back to llvmpipe"
	grep -q "net: accepted by" "$OUT/$label-client.log" || { fail "$label: the client was never accepted"; return 1; }
	grep -q "GO" "$OUT/$label-client.log" || { fail "$label: the match never started on the client"; return 1; }
	grep -E "segfault|Segmentation|SIGSEGV|crash" "$OUT/$label"-*.log | head -3 | sed 's/^/     /'
	return 0
}

# time: a 1-minute limit, no --endless
run_time() {
	local port=$1
	game "time-host" 300 --moddir "$MODDIR" --host "$port" --net-test-host 1 --net-test-stage 0x32 \
		--net-test-sims 2 --rng-seed 1 --net-test-timelimit 1 --net-test-scorelimit 100 --exit-frame 5000 &
	local host=$!
	waitfor "$OUT/time-host.log" "net: hosting on UDP port" 60 || { echo "FAIL: time host did not start"; kill -TERM $host; exit 2; }
	game "time-client" 290 --moddir "$MODDIR" --connect "127.0.0.1:$port" --net-test-join \
		--net-test-input "$OUT/client.script" --exit-frame 5000 &
	local client=$!
	waitfor "$OUT/time-host.log" "match 1 ended" 200
	sleep 3
	kill -TERM "$client" "$host" 2>/dev/null
	wait "$client"; wait "$host"
}

check_time() {
	local h c ht hl ct
	h=$(grep -h "net: match clock at the end" "$OUT/time-host.log" | tail -1)
	c=$(grep -h "net: match clock at the end" "$OUT/time-client.log" | tail -1)
	echo "     host:   ${h#*net: }"
	echo "     client: ${c#*net: }"
	ht=$(echo "$h" | sed -n 's/.*tick \([0-9]*\),.*/\1/p')
	hl=$(echo "$h" | sed -n 's/.*level time \([0-9]*\) .*/\1/p')
	ct=$(echo "$c" | sed -n 's/.*level time \([0-9]*\) .*/\1/p')
	[ -n "$ht" ] && [ "$ht" -ge 3590 ] && [ "$ht" -le 3610 ] && pass "time: the host ended the 1-minute match at tick $ht" \
		|| fail "time: the host ended it at tick '${ht}', not 3600"
	[ -n "$hl" ] && [ -n "$ct" ] && [ $((hl - ct)) -le 30 ] && [ $((ct - hl)) -le 30 ] \
		&& pass "time: the client's level clock ($ct) agrees with the host's ($hl) within half a second" \
		|| fail "time: the client's level clock '$ct' against the host's '$hl'"
}

i=0
for c in $CASES; do
	case $c in
		clean) run clean $((PORT + i)) "$FRAMES" 0 "0,0" ;;
		lag)   run lag $((PORT + i)) "$FRAMES" 0 "0,75" ;;
		loss)  run loss $((PORT + i)) "$FRAMES" 0 "2,75" ;;
		sims)  SIMS=6 run sims $((PORT + i)) "$FRAMES" 0 "0,75" ;;
		wine)  if [ -x "$(command -v wine)" ] && [ -e "$WINEEXE" ]; then run wine $((PORT + i)) "$FRAMES" 1 "${WINESIM-0,75}"; else echo "skip wine: no wine or no $WINEEXE"; fi ;;
		time)  run_time $((PORT + i)) ;;
	esac
	i=$((i + 1))
done

for c in $CASES; do
	case $c in
		clean|lag|loss|wine|sims)
			[ -e "$OUT/$c-client.log" ] || continue
			check_run "$c" || continue
			if [ "$c" = loss ] || [ "$c" = wine ] || [ "$c" = sims ]; then
				MINMATCH=0 MAXCPM=100000 measure "$c" && pass "$c: measured (reported, not gated on $MINMATCH%)" || fail "$c: measure"
			else
				measure "$c" && pass "$c: prediction" || fail "$c: prediction"
			fi
			snaps "$c"
			;;
		time) [ -e "$OUT/time-host.log" ] && check_time ;;
	esac
done

[ $status = 0 ] && echo "netpredicttest: all good" || echo "netpredicttest: FAILED"
exit $status
