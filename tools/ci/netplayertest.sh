#!/bin/bash
# netplayertest.sh — does the host play a remote player from its usercmds?
#
# Loopback (PLANS/netplay/spec-players.md): a listen host (its own player 0
# on a scripted pad) and one client (slot 1) on 127.0.0.1, both offscreen on
# the GPU, the 0x32 match with no sims, Start Armed with the Falcon 2. The client's local pad
# and mouse come from a per-tick script (--net-test-input): walk forward,
# turn with the mouse, five taps of fire. The host's own player walks,
# turns the other way and fires three taps, from its own script. Both log
# every player every 30 ticks (--net-test-trace). Checks, on the host's log:
#
#   - slot 1 (remote) walked, turned and fired on the host: its position,
#     theta and loaded ammo moved in the script's windows;
#   - every command played in tick order, none lost, and as many fire
#     presses played as the client sent (and the script has);
#   - player 0 (the host's own) walked, turned and fired from its script,
#     unaffected by the remote slot.
#
# Then again with the transport's loss simulator on both ends (--net-sim
# 5,60: 5% of datagrams in dropped, the rest 60 ms late): still every
# command in order, none lost, every press played.
#
#   netplayertest.sh [BIN]   BIN a file name in build/ (pd.x86_64) or a path
#
# Env: OUT (build/netplayer-out), PORT (27170; uses PORT and PORT+1),
# MODDIR (mod_allinone). Needs the ROM in build/data and an offscreen GPU.
# Exit status: 0 all good, 1 a check failed, 2 a run failed to start.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=${BUILD:-$ROOT/build}
OUT=${OUT:-$BUILD/netplayer-out}; PORT=${PORT:-27170}
MODDIR=${MODDIR:-mod_allinone}
BIN=${1:-pd.x86_64}
case $BIN in /*) ;; */*) BIN=$(realpath "$BIN") ;; *) BIN=$BUILD/$BIN ;; esac
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
rm -f "$OUT"/*.log
# no controller may reach a run (see replaytest.sh)
export SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-offscreen} SDL_GAMECONTROLLER_IGNORE_DEVICES=0x054c/0x0ce6,0x045e/0x028e \
	SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT=0x0000/0x0000 SDL_JOYSTICK_HIDAPI=0

status=0
fail() { echo "FAIL $*"; status=1; }
pass() { echo "ok   $*"; }

# the scripts: from to buttons(hex) sx sy rsx rsy mdx mdy, by the match's tick
# (Z_TRIG 0x2000, U_CBUTTONS 0x0008, D_CBUTTONS 0x0004)
cat > "$OUT/client.script" <<'EOF'
# walk forward
60 239 0008 0 0 0 0 0 0
# turn right with the mouse
300 359 0 0 0 0 0 0.5 0
# five taps of fire
400 401 2000 0 0 0 0 0 0
410 411 2000 0 0 0 0 0 0
420 421 2000 0 0 0 0 0 0
430 431 2000 0 0 0 0 0 0
440 441 2000 0 0 0 0 0 0
EOF
CLIENT_TAPS=5
cat > "$OUT/host.script" <<'EOF'
# the host's own player: walk forward, turn left, fire three taps (after
# the client's checks, so neither shot changes the other's numbers)
60 239 0008 0 0 0 0 0 0
300 359 0 0 0 0 0 -0.5 0
600 601 2000 0 0 0 0 0 0
620 621 2000 0 0 0 0 0 0
640 641 2000 0 0 0 0 0 0
EOF

INI="[Mod]\nStartArmed=1\n"

# game LABEL TIMEOUT ARGS... &  ($! is timeout's pid)
game() {
	local label=$1 t=$2; shift 2
	local save=$OUT/save-$label
	rm -rf "$save"; mkdir -p "$save"
	printf '%b' "$INI" > "$save/pd.ini"
	cd "$BUILD" || exit 2
	exec timeout -k 5 "$t" "$BIN" --savedir "$save" --skip-intro --no-sound "$@" > "$OUT/$label.log" 2>&1
}

waitfor() {
	local log=$1 pattern=$2 n=$3
	while [ "$n" -gt 0 ]; do
		grep -q -- "$pattern" "$log" 2>/dev/null && return 0
		sleep 1; n=$((n - 1))
	done
	return 1
}

# trace LOG PLAYER TICK FIELD -> the value (pos: "x y z")
trace() {
	local log=$1 p=$2 tick=$3 field=$4
	local line; line=$(grep "net: trace tick $tick player $p " "$log" | head -1)
	case $field in
		pos) echo "$line" | sed -n 's/.* pos \([^ ]*\) \([^ ]*\) \([^ ]*\) theta.*/\1 \2 \3/p' ;;
		*) echo "$line" | sed -n "s/.* $field \([^ ]*\).*/\1/p" ;;
	esac
}

# dist "x y z" "x y z"
dist() { awk -v a="$1" -v b="$2" 'BEGIN { split(a, p); split(b, q); printf "%.1f", sqrt((p[1]-q[1])^2 + (p[3]-q[3])^2) }'; }
absdiff() { awk -v a="$1" -v b="$2" 'BEGIN { d = a - b; if (d < 0) d = -d; if (d > 180) d = 360 - d; printf "%.2f", d }'; }

# run LABEL PORT [SIMARGS...]: one host and one client
run() {
	local label=$1 port=$2; shift 2
	game "$label-host" 150 --moddir "$MODDIR" --host "$port" --net-test-host 1 --net-test-stage 0x32 \
		--endless --rng-seed 1 --mp-weapons 1,4,7,9,10,1 --net-test-trace 30 \
		--net-test-input "$OUT/host.script" --exit-frame 1400 "$@" &
	local host=$!
	waitfor "$OUT/$label-host.log" "net: hosting on UDP port" 60 || { echo "FAIL: $label host did not start"; kill -TERM $host; exit 2; }
	game "$label-client" 140 --moddir "$MODDIR" --connect "127.0.0.1:$port" --net-test-join --net-test-trace 30 \
		--net-test-input "$OUT/client.script" --exit-frame 1000 "$@" &
	local client=$!
	wait "$client"; echo "$label: client exit $?"
	wait "$host"; echo "$label: host exit $?"
}

check() {
	local label=$1 H=$OUT/$1-host.log C=$OUT/$1-client.log

	grep -q "net: slot 1 is remote" "$H" || { fail "$label: slot 1 never became remote on the host"; return; }
	grep -q "renderD128: Permission denied" "$H" "$C" && fail "$label: a run fell back to llvmpipe"

	# slot 1 on the host, moved by the client's commands
	local p0 p1 d t0 t1 dt a0 a1
	p0=$(trace "$H" 1 30 pos); p1=$(trace "$H" 1 300 pos)
	d=$(dist "$p0" "$p1")
	if awk -v d="$d" 'BEGIN { exit !(d > 100) }'; then
		pass "$label: remote player 1 walked $d units on the host (tick 30 $p0 -> tick 300 $p1)"
	else
		fail "$label: remote player 1 moved only ${d:-?} (tick 30 '$p0', tick 300 '$p1')"
	fi
	t0=$(trace "$H" 1 270 theta); t1=$(trace "$H" 1 450 theta)
	dt=$(absdiff "$t0" "$t1")
	if awk -v d="$dt" 'BEGIN { exit !(d > 5) }'; then
		pass "$label: remote player 1 turned $dt degrees by the mouse (theta $t0 -> $t1)"
	else
		fail "$label: remote player 1 turned only ${dt:-?} (theta '$t0' -> '$t1')"
	fi
	a0=$(trace "$H" 1 390 ammo); a1=$(trace "$H" 1 540 ammo)
	if [ -n "$a0" ] && [ -n "$a1" ] && [ "$a1" -lt "$a0" ]; then
		pass "$label: remote player 1 fired on the host (weapon $(trace "$H" 1 390 weapon), loaded ammo $a0 -> $a1)"
	else
		fail "$label: remote player 1 ammo '$a0' -> '$a1'"
	fi

	# every command, in order, every press
	local stats sent played lost ooo presses
	stats=$(grep "net: slot 1 commands at" "$H" | tail -1)
	sent=$(grep -o "fire presses sent [0-9]*" "$C" | tail -1 | awk '{print $4}')
	played=$(echo "$stats" | sed -n 's/.*played \([0-9]*\).*/\1/p')
	ooo=$(echo "$stats" | sed -n 's/.* \([0-9]*\) out of order.*/\1/p')
	lost=$(echo "$stats" | sed -n 's/.*lost \([0-9]*\).*/\1/p')
	presses=$(echo "$stats" | sed -n 's/.*fire presses \([0-9]*\).*/\1/p')
	if [ -n "$stats" ] && [ "$ooo" = 0 ] && [ "$lost" = 0 ] && [ "$presses" = "$CLIENT_TAPS" ] && [ "$sent" = "$CLIENT_TAPS" ]; then
		pass "$label: ${stats#*net: } (client sent $sent presses)"
	else
		fail "$label: ${stats:-no command stats}; client sent ${sent:-?} presses, script has $CLIENT_TAPS"
	fi
	grep "net: trace tick [0-9]* client:" "$C" | tail -1 | sed "s/^.*net: /     $label client: /"

	# the host's own player, on its own scripted pad
	p0=$(trace "$H" 0 30 pos); p1=$(trace "$H" 0 300 pos)
	d=$(dist "$p0" "$p1")
	t0=$(trace "$H" 0 270 theta); t1=$(trace "$H" 0 450 theta)
	dt=$(absdiff "$t0" "$t1")
	a0=$(trace "$H" 0 570 ammo); a1=$(trace "$H" 0 720 ammo)
	if awk -v d="$d" -v t="$dt" 'BEGIN { exit !(d > 100 && t > 5) }' && [ -n "$a0" ] && [ -n "$a1" ] && [ "$a1" -lt "$a0" ]; then
		pass "$label: host player 0 walked $d, turned $dt degrees, ammo $a0 -> $a1 (its own script)"
	else
		fail "$label: host player 0 walked ${d:-?}, turned ${dt:-?}, ammo '$a0' -> '$a1'"
	fi
}

run clean "$PORT"
run lossy $((PORT + 1)) --net-sim 5,60
check clean
check lossy
exit $status
