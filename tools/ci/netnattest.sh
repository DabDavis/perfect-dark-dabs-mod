#!/bin/bash
# netnattest.sh — does the connectivity ladder find its way through NATs?
#
# Phase 6b (PLANS/NETPLAY.md "Lobby"): a joiner reaches its room's host over
# the first rung that answers - LAN, direct, a hole punched through both NATs,
# or pdlobbyd's relay - and the match then connects over it with the lobby's
# ticket. Here every rung is made to be the one that answers, on emulated
# networks built from Linux network namespaces, nftables masquerade and
# veth pairs, inside an unprivileged user namespace (`unshare -Urn`: no root,
# nothing outside this process tree is touched; the machine's own network
# never sees a packet):
#
#   inet (203.0.113.0/24, a bridge)   pdlobbyd at 203.0.113.1: HTTP 8091,
#                                     rendezvous UDP 27101, relay 27110-27141
#     rA 203.0.113.10  cone NAT   -> 10.0.1.0/24: a1 (.2) b1 (.3) d1 (.5) d2 (.6)
#     rB 203.0.113.20  cone NAT   -> 10.0.2.0/24: a2 (.2) c2 (.3)
#     rC 203.0.113.30  symmetric  -> 10.0.3.0/24: b2 (.2)
#     c1 203.0.113.40  no NAT, on the bridge itself
#     e1 203.0.113.50, e2 203.0.113.51  no NAT; e2 drops UDP to the rendezvous
#
# Each NAT is `masquerade` (symmetric: `masquerade fully-random`, a new
# random port for every destination) with a stateful firewall that drops
# anything unsolicited from the WAN - a home router. Linux's masquerade keeps
# the inside port when it can, which makes rA and rB endpoint-independent
# mappings with address-and-port-dependent filtering: the classic cone NAT
# that needs a punch from both sides (a true full cone would let the joiner
# straight in, which is case c's shape). The firewall matters: without it an
# unsolicited punch makes a conntrack entry on the router that later shifts
# the host's own mapping.
#
#   case a  host a1 behind rA, joiner a2 behind rB   -> punch
#   case b  host b1 behind rA, joiner b2 behind rC   -> relay (symmetric: no punch)
#   case c  host c1 public,    joiner c2 behind rB   -> direct
#   case d  host d1, joiner d2, both behind rA       -> lan
#   case e  host e1, joiner e2 public, but e2 cannot reach the rendezvous
#           (UDP 27101 dropped on its way out): the ladder gives up within
#           seconds and the launch goes straight to the advertised endpoint
#           (or the public address the lobby saw), with no hold for a
#           ladder that cannot climb
#
# Each pair is driven by --net-lobby-script (as netlobbytest.sh): the host
# makes a room, the joiner lists it, joins, waits for its ladder, READY; the
# host LAUNCHes, the joiner connects over the path with its ticket, the host
# verifies it, the 0x32 match passes GO on both with equal stage hashes, the
# host ends it at frame 300 and both come back to the room. The roster's
# path and ping (POST /rooms/<id>/netinfo) reach the host's room too.
#
#   netnattest.sh [BIN]   BIN a file name in build/ (pd.x86_64) or a path
#
# Env: OUT (build/netnat-out), MODDIR (mod_allinone), CASES ("a b c d e"),
# NETNAT_RUN (see below: the networks for a command of your own).
# Exit status: 0 all good, 1 a check failed, 2 a run failed to start,
# 3 user namespaces are not available here.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=${BUILD:-$ROOT/build}
OUT=${OUT:-$BUILD/netnat-out}
MODDIR=${MODDIR:-mod_allinone}
CASES=${CASES:-a b c d e}
BIN=${1:-pd.x86_64}
case $BIN in /*) ;; */*) BIN=$(realpath "$BIN") ;; *) BIN=$BUILD/$BIN ;; esac
PATH=$PATH:/usr/sbin:/sbin

if [ -z "${NETNAT_INSIDE:-}" ]; then
	if ! unshare -Urn true 2>/dev/null; then
		echo "SKIP: unprivileged user namespaces are not available (kernel.unprivileged_userns_clone?)"
		exit 3
	fi
	mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
	[ -n "$OUT" ] && [ "$OUT" != / ] && rm -rf "$OUT"/*.log "$OUT"/*.stage "$OUT"/save-* "$OUT"/*.nft
	export NETNAT_INSIDE=1 OUT BUILD BIN MODDIR CASES
	exec unshare -Urn "$0" "$@"
fi

export SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-offscreen} SDL_GAMECONTROLLER_IGNORE_DEVICES=0x054c/0x0ce6,0x045e/0x028e \
	SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT=0x0000/0x0000 SDL_JOYSTICK_HIDAPI=0

status=0
fail() { echo "FAIL $*"; status=1; }
pass() { echo "ok   $*"; }

# --- the networks --------------------------------------------------------

declare -A NS
KEEP=()
cleanup() { kill "${KEEP[@]}" 2>/dev/null; wait 2>/dev/null; }
trap cleanup EXIT

mkns() { unshare -n sleep 100000 </dev/null >/dev/null 2>&1 & NS[$1]=$!; KEEP+=($!); sleep 0.05; }
nsx() { local n=$1; shift; nsenter -t "${NS[$n]}" -n "$@"; }

die() { echo "FAIL: $*"; exit 2; }

ip link set lo up || die "no network namespace"
ip link add br0 type bridge && ip addr add 203.0.113.1/24 dev br0 && ip link set br0 up || die "bridge"

router() { # name wan-ip lan-prefix cone|sym
	local r=$1 wan=$2 lan=$3 mode=$4 fr=""
	mkns "$r"
	ip link add "w$r" type veth peer name "x$r" || die "veth $r"
	ip link set "x$r" netns "${NS[$r]}"; ip link set "w$r" master br0 up
	nsx "$r" ip link set lo up
	nsx "$r" ip link set "x$r" name wan
	nsx "$r" ip addr add "$wan/24" dev wan; nsx "$r" ip link set wan up
	nsx "$r" ip link add lanbr type bridge
	nsx "$r" ip addr add "$lan.1/24" dev lanbr; nsx "$r" ip link set lanbr up
	nsx "$r" sh -c 'echo 1 > /proc/sys/net/ipv4/ip_forward' || die "ip_forward in $r"
	[ "$mode" = sym ] && fr=fully-random
	cat > "$OUT/$r.nft" <<-EOF
	table ip nat {
		chain post {
			type nat hook postrouting priority 100;
			oifname "wan" masquerade $fr
		}
	}
	table inet fw {
		chain fw_input {
			type filter hook input priority 0;
			iifname "wan" ct state new drop
		}
		chain fw_forward {
			type filter hook forward priority 0;
			iifname "wan" ct state new drop
		}
	}
	EOF
	nsx "$r" nft -f "$OUT/$r.nft" || die "nft in $r"
}

inside() { # name router lan-ip
	local c=$1 r=$2 ip=$3
	mkns "$c"
	nsx "$r" ip link add "l$c" type veth peer name "e$c" || die "veth $c"
	nsx "$r" ip link set "e$c" netns "${NS[$c]}"
	nsx "$r" ip link set "l$c" master lanbr up
	nsx "$c" ip link set lo up
	nsx "$c" ip link set "e$c" name eth0
	nsx "$c" ip addr add "$ip/24" dev eth0; nsx "$c" ip link set eth0 up
	nsx "$c" ip route add default via "${ip%.*}.1"
}

public() { # name ip
	local c=$1 ip=$2
	mkns "$c"
	ip link add "w$c" type veth peer name "e$c" || die "veth $c"
	ip link set "e$c" netns "${NS[$c]}"; ip link set "w$c" master br0 up
	nsx "$c" ip link set lo up
	nsx "$c" ip link set "e$c" name eth0
	nsx "$c" ip addr add "$ip/24" dev eth0; nsx "$c" ip link set eth0 up
}

router rA 203.0.113.10 10.0.1 cone
router rB 203.0.113.20 10.0.2 cone
router rC 203.0.113.30 10.0.3 sym
inside a1 rA 10.0.1.2
inside b1 rA 10.0.1.3
inside d1 rA 10.0.1.5
inside d2 rA 10.0.1.6
inside a2 rB 10.0.2.2
inside c2 rB 10.0.2.3
inside b2 rC 10.0.3.2
public c1 203.0.113.40
public e1 203.0.113.50
public e2 203.0.113.51
cat > "$OUT/e2.nft" <<-EOF
table inet mute {
	chain out {
		type filter hook output priority 0;
		ip daddr 203.0.113.1 udp dport 27101 drop
	}
}
EOF
nsx e2 nft -f "$OUT/e2.nft" || die "nft in e2"

nsx a2 ping -c1 -W2 203.0.113.1 >/dev/null || die "rB's inside cannot reach the lobby's network"
echo "     networks up: rA cone, rB cone, rC symmetric, c1 public (user namespace, pid $$)"

# --- the lobby -----------------------------------------------------------

python3 -u "$ROOT/tools/pdlobbyd/pdlobbyd.py" --host 203.0.113.1 --port 8091 --udp-host 203.0.113.1 --udp-port 27101 \
	--auth open --relay-ports 27110-27141 > "$OUT/pdlobbyd.log" 2>&1 &
LOBBY=$!
KEEP+=($LOBBY)
for _ in $(seq 20); do grep -q "pdlobbyd listening" "$OUT/pdlobbyd.log" && break; sleep 0.5; done
grep -q "pdlobbyd listening" "$OUT/pdlobbyd.log" || die "pdlobbyd did not start (see $OUT/pdlobbyd.log)"
echo "     $(head -1 "$OUT/pdlobbyd.log")"

# NETNAT_RUN=CMD: the networks and the lobby only, then CMD instead of the
# test, with NS_<name>=<pid> in its environment for each namespace (enter one
# with `nsenter -t $NS_a1 -n`, or from outside `nsenter -t $NS_a1 -U -n
# --preserve-credentials`): for looking at a case by hand
if [ -n "${NETNAT_RUN:-}" ]; then
	for k in "${!NS[@]}"; do export "NS_$k=${NS[$k]}"; done
	"$NETNAT_RUN"
	exit $?
fi

# --- the games -----------------------------------------------------------

# game NS LABEL USER PORT ARGS...  (in the background; $! is its timeout's pid)
game() {
	local ns=$1 label=$2 user=$3 port=$4 save=$OUT/save-$2; shift 4
	mkdir -p "$save"
	for f in eeprom.bin mpsetups.bin; do
		[ -f "$HOME/.local/share/perfectdark/$f" ] && cp "$HOME/.local/share/perfectdark/$f" "$save/"
	done
	printf '[Mod]\nGhostUser=%s\nGhostPin=1234\n[Net]\nLobbyServer=http://203.0.113.1:8091\nPort=%s\n' "$user" "$port" > "$save/pd.ini"
	( cd "$BUILD" && exec nsenter -t "${NS[$ns]}" -n timeout -k 5 300 "$BIN" --savedir "$save" --skip-intro --no-sound \
		--moddir "$MODDIR" "$@" > "$OUT/$label.log" 2>&1 )
}

declare -A HOSTNS=([a]=a1 [b]=b1 [c]=c1 [d]=d1 [e]=e1) JOINNS=([a]=a2 [b]=b2 [c]=c2 [d]=d2 [e]=e2)
declare -A ROSTER=([e]=none)
declare -A WANT=([a]=punch [b]=relay [c]=direct [d]=lan [e]="an advertised endpoint\|the host's public address")
declare -A RUNS
n=0
for c in $CASES; do
	n=$((n + 1))
	game "${HOSTNS[$c]}" "$c-host" "host$c" $((27180 + n)) --net-lobby-script host --net-lobby-room "Case $c" \
		--net-lobby-end-frame 300 --net-test-stage 0x32 --net-test-sims 2 --rng-seed $((20 + n)) &
	RUNS[$c-host]=$!
	game "${JOINNS[$c]}" "$c-join" "joiner$c" 27180 --net-lobby-script join --net-lobby-room "Case $c" \
		--net-lobby-leave-frame 0 &
	RUNS[$c-join]=$!
done
for k in "${!RUNS[@]}"; do wait "${RUNS[$k]}"; eval "rc_${k//-/_}=$?"; done
kill $LOBBY 2>/dev/null; wait $LOBBY 2>/dev/null

# --- the checks ----------------------------------------------------------

L=$OUT/pdlobbyd.log
line() { grep -m1 -o -- "$2.*" "$1"; }

for c in $CASES; do
	H=$OUT/$c-host.log J=$OUT/$c-join.log want=${WANT[$c]}
	eval "rh=\$rc_${c}_host rj=\$rc_${c}_join"
	grep -q "renderD128: Permission denied" "$H" "$J" && fail "$c: a run fell back to llvmpipe"
	room=$(sed -n 's/.*lobby: made room \([0-9a-f]*\).*/\1/p' "$H" | head -1)
	[ -n "$room" ] && pass "$c: host made room $room" || { fail "$c: host made no room"; continue; }
	grep -q "lobby: joined room $room" "$J" && pass "$c: joiner joined it" || fail "$c: joiner did not join"
	if [ "$c" = e ]; then
		if grep -q "rdv: path to the host:" "$J"; then
			fail "e: a path through a rendezvous it cannot reach: $(line "$J" "rdv: path to the host")"
		elif grep -q "rdv: the lobby's rendezvous did not answer; no path from it" "$J"; then
			pass "e: joiner: $(line "$J" "rdv: the lobby's rendezvous did not answer")"
		else
			fail "e: the joiner's ladder never gave up on the mute rendezvous"
		fi
		grep -q "holding for the path to the host" "$J" && fail "e: the launch held for a ladder that could not climb" \
			|| pass "e: the launch did not hold for the ladder"
	elif grep -q "rdv: path to the host: $want via" "$J"; then
		pass "$c: joiner: $(line "$J" "rdv: path to the host")"
	else
		fail "$c: joiner's path is not $want: $(line "$J" "rdv: path to the host" || line "$J" "rdv: no path")"
	fi
	[ "$c" = b ] && { grep -q "asking the lobby for a relay" "$J" && pass "b: joiner: $(line "$J" "rdv: no direct")" || fail "b: never asked for a relay"
		grep -q "relay [0-9a-f]*: port [0-9]* for joinerb in room $room" "$L" && pass "b: pdlobbyd: $(line "$L" "relay [0-9a-f]*: port")" || fail "b: pdlobbyd made no relay"
		grep -q "rdv: relay for joinerb bound" "$H" && pass "b: host: $(line "$H" "rdv: relay for")" || fail "b: host never bound the relay"; }
	[ "$c" = a ] && { grep -q "rdv: joinera has not reached us; spraying" "$H" && pass "a: host: $(line "$H" "rdv: joinera has not")" || fail "a: host never sprayed"; }
	[ "$c" = c ] && { grep -q "spraying" "$H" && fail "c: host sprayed a joiner that reached it unaided" || pass "c: host never sprayed (reachable)"; }
	if grep -q "connecting to .* (endpoint [0-9] of [0-9]: $want) with the lobby's ticket" "$J"; then
		pass "$c: joiner: $(line "$J" "lobby: room $room launched; connecting")"
	else
		fail "$c: joiner did not connect over $want: $(line "$J" "lobby: room $room launched")"
	fi
	grep -q "lobby ticket for \"joiner$c\" in room $room verified" "$H" && pass "$c: host: $(line "$H" "lobby ticket for")" || fail "$c: host never verified the ticket"
	grep -q "slot 1: \"joiner$c\" joined" "$H" && pass "$c: host: $(line "$H" "slot 1: \"joiner$c\" joined" | sed 's/ (fov.*//')" || fail "$c: joiner never took slot 1"
	if grep -q "match 1: every machine has loaded; GO" "$H" && grep -q "net: match 1: GO" "$J"; then
		pass "$c: the match passed GO on both"
	else
		fail "$c: no GO on both"
	fi
	grep -o 'net: stage hash .*' "$H" | sed 's/ ([0-9]*)$//' | head -8 > "$OUT/$c-host.stage"
	grep -o 'net: stage hash .*' "$J" | sed 's/ ([0-9]*)$//' | head -8 > "$OUT/$c-join.stage"
	[ -s "$OUT/$c-host.stage" ] && cmp -s "$OUT/$c-host.stage" "$OUT/$c-join.stage" \
		&& pass "$c: stage hash equal ($(wc -l < "$OUT/$c-host.stage") components)" || fail "$c: stage hash differs"
	grep -q "lobby: roster: joiner$c reaches the host by ${ROSTER[$c]:-$want}" "$H" && pass "$c: host's roster: $(line "$H" "lobby: roster: joiner$c")" \
		|| fail "$c: the host's roster never showed joiner$c's path"
	grep -q "host back in room $room after match 1" "$H" && grep -q "client back in room $room after match 1" "$J" \
		&& pass "$c: both back in the room after the match" || fail "$c: not both back in the room"
	[ "$rh" = 0 ] && [ "$rj" = 0 ] || fail "$c: exit codes host $rh join $rj"
done
exit $status
