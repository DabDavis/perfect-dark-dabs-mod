#!/bin/bash
# netwidetest.sh — phase 8: a room of twelve humans.
#
# Loopback: a listen host with Net.MaxPlayers=12 and two sims, and eleven
# clients (slots 1-11), all offscreen on the GPU, on the 0x32 match. Every
# client walks forward, turns with the mouse and fires twice from a per-tick
# script (--net-test-input); every game logs every player every 30 ticks
# (--net-test-trace). Checks, on the host's log:
#
#   - slots 1-11 are remote and each walked (> 100 units between ticks 30
#     and 300) from its own commands, every command played in order and
#     none lost;
#   - RULES carried the human slots 4-11 (humanslotshi 0xff) beside the
#     sims' chrslots bits (0x003f: humans 0-3, sims 0-1), and every client
#     named all twelve players;
#   - nothing crashed: every run reached its exit frame.
#
# Then a room of two (Net.MaxPlayers=2): a third client is refused FULL.
#
#   netwidetest.sh [BIN]   BIN a file name in build/ (pd.x86_64) or a path
#
# Env: OUT (build/netwide-out), PORT (27180; uses PORT and PORT+1), MODDIR
# (mod_allinone). Needs the ROM in build/data and an offscreen GPU. Twelve
# games at once: run it alone. Exit status: 0 all good, 1 a check failed,
# 2 a run failed to start.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=${BUILD:-$ROOT/build}
OUT=${OUT:-$BUILD/netwide-out}; PORT=${PORT:-27180}
MODDIR=${MODDIR:-mod_allinone}
BIN=${1:-pd.x86_64}
case $BIN in /*) ;; */*) BIN=$(realpath "$BIN") ;; *) BIN=$BUILD/$BIN ;; esac
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
rm -rf "$OUT"/*.log "$OUT"/save-*
export SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-offscreen} SDL_GAMECONTROLLER_IGNORE_DEVICES=0x054c/0x0ce6,0x045e/0x028e \
	SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT=0x0000/0x0000 SDL_JOYSTICK_HIDAPI=0

status=0
fail() { echo "FAIL $*"; status=1; }
pass() { echo "ok   $*"; }

cat > "$OUT/client.script" <<'EOS'
# walk forward, turn right with the mouse, two taps of fire
60 239 0008 0 0 0 0 0 0
300 359 0 0 0 0 0 0.5 0
400 401 2000 0 0 0 0 0 0
420 421 2000 0 0 0 0 0 0
EOS

# game LABEL TIMEOUT INI ARGS... &  ($! is timeout's pid)
game() {
	local label=$1 t=$2 ini=$3; shift 3
	local save=$OUT/save-$label
	rm -rf "$save"; mkdir -p "$save"
	printf '%b' "$ini" > "$save/pd.ini"
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

pos() { grep -m1 "net: trace tick $2 player $3 " "$1" | sed -n 's/.* pos \([^ ]*\) [^ ]* \([^ ]*\) theta.*/\1 \2/p'; }
dist() { awk -v a="$1" -v b="$2" 'BEGIN { split(a, p); split(b, q); printf "%.1f", sqrt((p[1]-q[1])^2 + (p[2]-q[2])^2) }'; }

N=11
game host 420 "[Mod]\nStartArmed=1\n[Net]\nMaxPlayers=12\n" --moddir "$MODDIR" --host "$PORT" --net-test-host "$N" \
	--net-test-stage 0x32 --endless --rng-seed 1 --mp-weapons 1,4,7,9,10,1 --net-test-trace 30 --net-test-sims 2 \
	--exit-frame 1400 &
host=$!
waitfor "$OUT/host.log" "net: hosting on UDP port" 60 || { echo "FAIL: the host did not start"; kill -TERM $host; exit 2; }
pids=""
for k in $(seq 1 $N); do
	game c$k 400 "[Mod]\nStartArmed=1\n" --moddir "$MODDIR" --connect "127.0.0.1:$PORT" --net-test-join \
		--net-test-trace 30 --net-test-input "$OUT/client.script" --exit-frame 1000 &
	pids="$pids $!"
	sleep 0.5
done
for p in $pids; do wait "$p"; done
wait "$host"; echo "twelve: host exit $?"

H=$OUT/host.log
grep -q "renderD128: Permission denied" "$OUT"/*.log && fail "a run fell back to llvmpipe"
grep -q "exit-frame 1400 reached" "$H" && pass "host: reached its exit frame" || fail "host: no exit frame (crashed or hung?)"
n=0
for k in $(seq 1 $N); do grep -q "exit-frame 1000 reached" "$OUT/c$k.log" && n=$((n + 1)); done
[ "$n" = $N ] && pass "all $N clients reached their exit frame" || fail "only $n of $N clients reached their exit frame"

walked=0; inorder=0
for s in $(seq 1 $N); do
	d=$(dist "$(pos "$H" 30 "$s")" "$(pos "$H" 300 "$s")")
	awk -v d="$d" 'BEGIN { exit !(d > 100) }' && walked=$((walked + 1)) || echo "     slot $s walked only ${d:-?}"
	st=$(grep "net: slot $s commands at" "$H" | tail -1)
	echo "$st" | grep -q "0 out of order.*lost 0" && inorder=$((inorder + 1)) || echo "     slot $s: ${st:-no command stats}"
done
[ "$walked" = $N ] && pass "slots 1-$N each walked on the host from their own commands" || fail "$walked of $N remote slots walked"
[ "$inorder" = $N ] && pass "slots 1-$N: every command played in order, none lost" || fail "$inorder of $N slots played every command in order"

r=$(grep -m1 -o "net: rules applied: match [0-9]*, scenario [0-9]*, chrslots 0x[0-9a-f]*, human slots 4-11 0x[0-9a-f]*" "$OUT/c$N.log")
echo "$r" | grep -q "chrslots 0x003f, human slots 4-11 0xff" && pass "client $N: $r" || fail "client $N: rules '${r:-none}'"
np=$(grep -c "net: the match's players:" "$OUT/c$N.log")
[ "$np" = 12 ] && pass "client $N: twelve players named" || fail "client $N: $np players named"

# a room of two: the third is refused
game host2 120 "[Net]\nMaxPlayers=2\n" --moddir "$MODDIR" --host "$((PORT + 1))" --exit-frame 2400 &
h2=$!
waitfor "$OUT/host2.log" "net: hosting on UDP port" 60 || { echo "FAIL: the second host did not start"; kill -TERM $h2; exit 2; }
game d1 60 "" --moddir "$MODDIR" --connect "127.0.0.1:$((PORT + 1))" --exit-frame 900 &
d1=$!
waitfor "$OUT/d1.log" "accepted" 40
game d2 60 "" --moddir "$MODDIR" --connect "127.0.0.1:$((PORT + 1))" --exit-frame 900 &
d2=$!
wait "$d1"; wait "$d2"; kill -TERM $h2 2>/dev/null; wait "$h2"
grep -q "net: accepted by .* into slot 1" "$OUT/d1.log" && pass "room of two: the first client is in" || fail "room of two: the first client was not accepted"
grep -q "net: session ended \[full\]" "$OUT/d2.log" && pass "room of two: the second client is refused ($(grep -m1 -o "session ended.*" "$OUT/d2.log"))" \
	|| fail "room of two: the second client was not refused FULL"
exit $status
