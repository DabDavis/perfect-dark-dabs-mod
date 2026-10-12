#!/bin/bash
# netteamtest.sh — Team Control online: do a player's orders to the sims on
# its team reach the host's sims? (protocol 31, NETCMD_ORDER)
#
# Loopback: a listen host (player 0, team 0) and one client (player 1, team
# 1) on 127.0.0.1, offscreen on the GPU, the 0x32 match with four sims dealt
# round two teams (bot slots 0 and 2 on the client's team, 1 and 3 on the
# host's), every player invincible, no weapons in the arena (a sim fetches
# what it lacks before it takes any order, stock, so it would follow late). Orders are given as the active menu's sim
# pages give them (--net-test-order: botcmdApply on the machine's own copy,
# and a client's to the host in its next command):
#
#   team    the client orders its first sim to Follow, its other sim (slot 2)
#           to Attack the host's player, and the host's sim (slot 1, not on
#           its team) to Hold; the host orders its own first sim to Follow.
#           Checks on the host: the client's Follow and Attack were taken and
#           its Hold refused (the sim is not on its team); at the check tick
#           the client's first sim follows player 1 (its action the follow),
#           slot 2 attacks player 0, the host's first sim follows player 0;
#           the client's own copies show the orders it gave (its menu's).
#   ffa     no teams: the client's order is refused (not a team match), and
#           the client had no sims to order (Team Control is a team match's).
#
#   netteamtest.sh [BIN]   BIN a file name in build/ (pd.x86_64) or a path
#
# Env: BUILD (build/: data/ and MODDIR in it), OUT (build/netteam-out), PORT
# (27290; uses PORT and PORT+1), MODDIR (mod_allinone), CASES (team ffa).
# Exit status: 0 all good, 1 a check failed, 2 a run failed to start.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=${BUILD:-$ROOT/build}
OUT=${OUT:-$BUILD/netteam-out}; PORT=${PORT:-27290}
MODDIR=${MODDIR:-mod_allinone}
CASES=${CASES:-team ffa}
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

# run LABEL PORT HOSTARGS -- CLIENTARGS
run() {
	local label=$1 port=$2; shift 2
	local hargs=() cargs=()
	while [ $# -gt 0 ] && [ "$1" != "--" ]; do hargs+=("$1"); shift; done
	[ $# -gt 0 ] && shift
	cargs=("$@")
	( game "$label-host" 150 --moddir "$MODDIR" --host "$port" --net-test-host 1 --net-test-stage 0x32 \
		--endless --rng-seed 3 --net-test-sims 4 --net-test-invincible --net-test-order-check 300 \
		--exit-frame 1300 "${hargs[@]}" ) &
	local host=$!
	waitfor "$OUT/$label-host.log" "net: hosting on UDP port" 60 || { echo "FAIL: $label host did not start"; kill -TERM $host; exit 2; }
	( game "$label-client" 140 --moddir "$MODDIR" --connect "127.0.0.1:$port" --net-test-join \
		--net-test-order-check 300 --exit-frame 1100 "${cargs[@]}" ) &
	local client=$!
	wait "$client"; echo "$label: client exit $?"
	wait "$host"; echo "$label: host exit $?"
}

# sim LOG SLOT FIELD: a field of the order check's line for bot slot SLOT
sim() {
	grep "net: order check tick 900 sim [0-9]* slot $2 " "$1" | head -1 | sed -n "s/.* $3 \([^ ]*\).*/\1/p"
}

case_team() {
	run team "$PORT" --net-test-teams 2 --mp-weapons 0,0,0,0,0,0 --net-test-order 250:0 -- \
		--net-test-order 250:0,280:1:2:0,310:3:1
	local H=$OUT/team-host.log C=$OUT/team-client.log
	grep -q "net: slot 1 is remote" "$H" || { fail "team: slot 1 never became remote on the host"; return; }

	grep -q "net: slot 1 ordered 1 sim (sim 0) to Follow" "$H" \
		&& pass "team: the client's Follow reached the host ($(grep -o 'net: slot 1 ordered 1 sim (sim 0) to Follow.*' "$H" | head -1))" \
		|| fail "team: no Follow from the client taken on the host"
	grep -q "net: slot 1 ordered 1 sim (sim 2) to Attack (command 1, target 0)" "$H" \
		&& pass "team: the client's Attack on player 0 reached the host" \
		|| fail "team: no Attack from the client taken on the host"
	grep -q "net: slot 1's order to sim 1 (command 3) not taken: no such sim on its team" "$H" \
		&& pass "team: the client's order to the host's sim was refused (not on its team)" \
		|| fail "team: the client's order to sim 1 was not refused"

	local cmd f a d
	cmd=$(sim "$H" 0 command); f=$(sim "$H" 0 follows); d=$(sim "$H" 0 dist)
	if [ "$cmd" = 0 ] && [ "$f" = 1 ]; then
		pass "team: on the host the client's sim (slot 0) follows player 1 (distance $d)"
	else
		fail "team: on the host slot 0 has command '$cmd', follows '$f'"
	fi
	# a following sim breaks off to fight what it sees (stock: Follow, not
	# Protect), so its follow is looked for over the checks, every 150 ticks
	local nf nc
	nc=$(grep -c "net: order check tick [0-9]* sim [0-9]* slot 0 " "$H")
	nf=$(grep "net: order check tick [0-9]* sim [0-9]* slot 0 " "$H" | grep -c "myaction 50 (following 1)")
	if [ "$nf" -gt 0 ]; then
		pass "team: ... and went after it (MA_AIBOTFOLLOW following 1 at $nf of $nc checks; distance $(grep "order check tick [0-9]* sim [0-9]* slot 0 " "$H" | sed -n 's/.* dist \([^ ]*\).*/\1/p' | tr '\n' ' '))"
	else
		fail "team: slot 0 never MA_AIBOTFOLLOW following player 1 in $nc checks"
	fi
	cmd=$(sim "$H" 2 command); a=$(sim "$H" 2 attacks)
	[ "$cmd" = 1 ] && [ "$a" = 0 ] && pass "team: on the host slot 2 attacks player 0" \
		|| fail "team: on the host slot 2 has command '$cmd', attacks '$a'"
	cmd=$(sim "$H" 1 command); f=$(sim "$H" 1 follows)
	[ "$cmd" = 0 ] && [ "$f" = 0 ] && pass "team: the host's own order: slot 1 follows player 0" \
		|| fail "team: on the host slot 1 has command '$cmd', follows '$f' (the host's own Follow)"
	cmd=$(sim "$C" 0 command); a=$(sim "$C" 2 command)
	[ "$cmd" = 0 ] && [ "$a" = 1 ] && pass "team: the client's menu shows its orders (slot 0 Follow, slot 2 Attack)" \
		|| fail "team: the client's copies show slot 0 '$cmd', slot 2 '$a'"
	grep "net: order check tick 900" "$H" | sed 's/^.*net: /     host: /'
}

case_ffa() {
	run ffa $((PORT + 1)) -- --net-test-order 250:0
	local H=$OUT/ffa-host.log C=$OUT/ffa-client.log
	grep -q "net: slot 1 is remote" "$H" || { fail "ffa: slot 1 never became remote on the host"; return; }
	grep -q "net: --net-test-order: tick 250, player 1 (0 sims on its team)" "$C" \
		&& pass "ffa: the client has no sims to order" || fail "ffa: the client had sims on its team"
	grep -q "net: slot 1's order to sim 255 (command 0) not taken: not a team match" "$H" \
		&& pass "ffa: the host refused the order (not a team match)" || fail "ffa: the host did not refuse the order"
	grep -q "net: slot 1 ordered" "$H" && fail "ffa: the host took an order"
	! grep "net: order check tick 900" "$H" | grep -qv "command 4 (Normal)" \
		&& pass "ffa: every sim on the host still plays Normal" || fail "ffa: a sim's command changed"
}

for c in $CASES; do
	case $c in
		team) case_team ;;
		ffa) case_ffa ;;
		*) echo "unknown case $c"; exit 2 ;;
	esac
done
exit $status
