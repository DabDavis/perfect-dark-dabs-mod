#!/bin/bash
# netsnaptest.sh — does a client decode the host's snapshots exactly?
#
# Loopback (PLANS/netplay/spec-entities.md §1, §6, §7): a listen host (its
# own player 0 on a scripted pad) and one client (slot 1) on 127.0.0.1, both
# offscreen on the GPU, the 0x32 match with 8 sims. Both dump entity state
# for host ticks DUMP_FROM..DUMP_TO (--net-snap-dump): the host what it
# offered the client with its true, unquantized values and each entity's
# status in the packet (sent, unchanged, deferred by the cap, excluded); the
# client what it decoded for the same host ticks. Checks:
#
#   - on 30 host ticks both have, every entity the host sent or had in sync
#     equals the client's within the quantization (position 1/8, yaw 1/65536
#     turn, anim frame 1/8, door frac 1/65535, rotation smallest-three), the
#     same anim number, generation and record kind; every deferred one is
#     present on the client, every excluded one absent, and the client has
#     nothing the host did not offer;
#   - bytes per snapshot (host log), and the client decoded nearly all of
#     them with nothing malformed.
#
# Then again with the transport's loss simulator on both ends (--net-sim
# 5,60): the same equality, keyframes counted, the baselines recovering.
# Then a hostile run (--net-test-hostile): mangled copies of every CMD into
# the host's parser and of every SNAP through the client's own decoder in
# probe mode (checked against its real baselines, nothing kept);
# both must live to the end and the client must keep decoding.
#
#   netsnaptest.sh [BIN]   BIN a file name in build/ (pd.x86_64) or a path
#
# Env: OUT (build/netsnap-out), PORT (27180; uses PORT..PORT+2), MODDIR
# (mod_allinone). Needs the ROM in build/data and an offscreen GPU.
# Exit status: 0 all good, 1 a check failed, 2 a run failed to start.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=${BUILD:-$ROOT/build}
OUT=${OUT:-$BUILD/netsnap-out}; PORT=${PORT:-27180}
MODDIR=${MODDIR:-mod_allinone}
BIN=${1:-pd.x86_64}
DUMP_FROM=${DUMP_FROM:-300}; DUMP_TO=${DUMP_TO:-1100}
case $BIN in /*) ;; */*) BIN=$(realpath "$BIN") ;; *) BIN=$BUILD/$BIN ;; esac
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
rm -f "$OUT"/*.log "$OUT"/*.dump
# no controller may reach a run (see replaytest.sh)
export SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-offscreen} SDL_GAMECONTROLLER_IGNORE_DEVICES=0x054c/0x0ce6,0x045e/0x028e \
	SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT=0x0000/0x0000 SDL_JOYSTICK_HIDAPI=0

status=0
fail() { echo "FAIL $*"; status=1; }
pass() { echo "ok   $*"; }

# the client walks, turns and fires; the host's own player walks the other way
cat > "$OUT/client.script" <<'EOF'
60 400 0008 0 0 0 0 0 0
300 360 0 0 0 0 0 0.5 0
420 421 2000 0 0 0 0 0 0
500 501 2000 0 0 0 0 0 0
600 900 0008 0 0 0 0 -0.3 0
EOF
cat > "$OUT/host.script" <<'EOF'
60 400 0008 0 0 0 0 0 0
300 360 0 0 0 0 0 -0.5 0
700 701 2000 0 0 0 0 0 0
EOF

INI="[Mod]\nStartArmed=1\n"

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

# run LABEL PORT [ARGS...]: one host and one client
run() {
	local label=$1 port=$2; shift 2
	game "$label-host" 180 --moddir "$MODDIR" --host "$port" --net-test-host 1 --net-test-stage 0x32 \
		--net-test-sims 8 --endless --rng-seed 1 --mp-weapons 1,4,7,9,10,1 \
		--net-test-input "$OUT/host.script" --net-snap-dump "$OUT/$label-host.dump" \
		--net-snap-dump-ticks "$DUMP_FROM,$DUMP_TO" --exit-frame 1400 "$@" &
	local host=$!
	waitfor "$OUT/$label-host.log" "net: hosting on UDP port" 60 || { echo "FAIL: $label host did not start"; kill -TERM $host; exit 2; }
	game "$label-client" 170 --moddir "$MODDIR" --connect "127.0.0.1:$port" --net-test-join \
		--net-test-input "$OUT/client.script" --net-snap-dump "$OUT/$label-client.dump" \
		--net-snap-dump-ticks "$DUMP_FROM,$DUMP_TO" --exit-frame 1300 ${CLIENT_EXTRA:-} "$@" &
	local client=$!
	wait "$client"; echo "$label: client exit $?"
	wait "$host"; echo "$label: host exit $?"
}

# compare LABEL: the dumps, 30 sampled ticks
compare() {
	python3 - "$OUT/$1-host.dump" "$OUT/$1-client.dump" "$1" <<'PY'
import math, sys
hpath, cpath, label = sys.argv[1:4]
H = {}; C = {}
def fl(xs): return [float(x) for x in xs]
for line in open(hpath):
    p = line.split()
    if p[0] != 'H' or p[2] != '1': continue
    tick, eid = int(p[1]), int(p[4])
    H.setdefault(tick, {})[eid] = (int(p[5]), int(p[6]), int(p[7]), fl(p[8:11]), float(p[11]), int(p[12]), float(p[13]), float(p[14]), fl(p[15:19]), int(p[19]))
for line in open(cpath):
    p = line.split()
    if p[0] != 'C': continue
    tick, eid = int(p[1]), int(p[3])
    C.setdefault(tick, {})[eid] = (int(p[4]), int(p[5]), None, fl(p[6:9]), float(p[9]), int(p[10]), float(p[11]), float(p[12]), fl(p[13:17]), int(p[17]))
common = sorted(set(H) & set(C))
if len(common) < 30:
    print(f"FAIL {label}: only {len(common)} host ticks in both dumps (host {len(H)}, client {len(C)})"); sys.exit(1)
step = len(common) / 30.0
sample = [common[int(i * step)] for i in range(30)]
TAU = 2 * math.pi
def angd(a, b):
    d = math.fmod(a - b, TAU)
    if d > math.pi: d -= TAU
    if d < -math.pi: d += TAU
    return abs(d)
REC = {1: 'chr', 2: 'obj', 3: 'door', 4: 'lift'}
cmp = {k: 0 for k in REC.values()}; bad = []; deferred = excluded = extra = 0; worst = [0.0, 0.0, 0.0, 0.0, 1.0]
for t in sample:
    h, c = H[t], C[t]
    for eid, (gen, rec, st, pos, yaw, anim, frame, door, q, tele) in h.items():
        got = c.get(eid)
        if st == 4:
            excluded += 1
            if got is not None and got[0] == gen: bad.append(f"tick {t} id {eid}: excluded but present")
            continue
        if got is None or got[0] != gen or got[1] != rec:
            bad.append(f"tick {t} id {eid} ({REC.get(rec)}): status {st} host gen {gen}, client {got[:2] if got else 'absent'}"); continue
        if st == 3:
            deferred += 1; continue
        _, _, _, cpos, cyaw, canim, cframe, cdoor, cq, ctele = got
        why = []
        if rec in (1, 2, 4):
            e = max(abs(a - b) for a, b in zip(pos, cpos)); worst[0] = max(worst[0], e)
            if e > 0.0626 + 1e-3 and not any(abs(a) > 1048575 for a in pos): why.append(f"pos {pos} vs {cpos}")
        if rec == 1:
            e = angd(yaw, cyaw); worst[1] = max(worst[1], e)
            if e > math.pi / 65536 + 2e-4: why.append(f"yaw {yaw} vs {cyaw}")
            if anim != canim: why.append(f"anim {anim} vs {canim}")
            if tele != ctele: why.append(f"teleports {tele} vs {ctele}")
            hf = min(max(frame, 0.0), 8191.875)
            e = abs(hf - cframe); worst[2] = max(worst[2], e)
            if e > 0.0626 + 1e-3: why.append(f"frame {frame} vs {cframe}")
        if rec == 3:
            e = abs(min(max(door, 0.0), 1.0) - cdoor); worst[3] = max(worst[3], e)
            if e > 1 / 65535 + 2e-5: why.append(f"door {door} vs {cdoor}")
        if rec == 2:
            d = abs(sum(a * b for a, b in zip(q, cq))); worst[4] = min(worst[4], d)
            if d < 0.9999: why.append(f"quat {q} vs {cq}")
        cmp[REC[rec]] += 1
        if why: bad.append(f"tick {t} id {eid} ({REC[rec]}): " + "; ".join(why))
    for eid in c:
        if eid not in h: extra += 1; bad.append(f"tick {t} id {eid}: on the client, never offered")
total = sum(cmp.values())
print(f"     {label}: {len(common)} ticks in both dumps; 30 sampled ({sample[0]}..{sample[-1]}): {total} entity states compared "
      f"({', '.join(f'{v} {k}' for k, v in cmp.items())}), {deferred} deferred, {excluded} excluded; worst error: pos {worst[0]:.4f}, "
      f"yaw {worst[1]:.6f} rad, frame {worst[2]:.4f}, door {worst[3]:.7f}, rotation |dot| {worst[4]:.6f}")
for b in bad[:12]: print("     mismatch " + b)
if bad or total == 0:
    print(f"FAIL {label}: {len(bad)} mismatches"); sys.exit(1)
print(f"ok   {label}: every sent and in-sync entity decoded within the quantization on 30 sampled ticks")
PY
}

check() {
	local label=$1 H=$OUT/$1-host.log C=$OUT/$1-client.log
	grep -q "net: slot 1 is remote" "$H" || { fail "$label: slot 1 never became remote on the host"; return; }
	grep -q "renderD128: Permission denied" "$H" "$C" && fail "$label: a run fell back to llvmpipe"
	grep -E "Segmentation|Aborted|crash|ASSERT" "$H" "$C" | head -3 | sed 's/^/     /'

	local hs cs
	hs=$(grep "net: snap slot 1 " "$H" | tail -1)
	cs=$(grep "net: snap client " "$C" | tail -1)
	[ -n "$hs" ] && echo "     $label host: ${hs#*net: }" || fail "$label: no snapshot stats on the host"
	[ -n "$cs" ] && echo "     $label client: ${cs#*net: }" || fail "$label: no snapshot stats on the client"

	local mean dec rec mal
	mean=$(echo "$hs" | sed -n 's/.*bytes mean \([0-9]*\).*/\1/p')
	rec=$(echo "$cs" | sed -n 's/.*: \([0-9]*\) received.*/\1/p')
	dec=$(echo "$cs" | sed -n 's/.* \([0-9]*\) decoded.*/\1/p')
	mal=$(echo "$cs" | sed -n 's/.* \([0-9]*\) malformed.*/\1/p')
	if [ -n "$mean" ] && [ "$mean" -gt 0 ] && [ "$mean" -le 1100 ]; then
		pass "$label: $mean bytes per snapshot on average"
	else
		fail "$label: bytes per snapshot '${mean:-?}'"
	fi
	if [ -n "$dec" ] && [ "$dec" -gt 300 ] && [ "${mal:-1}" = 0 ]; then
		pass "$label: client decoded $dec of $rec snapshots, none malformed"
	else
		fail "$label: client decoded ${dec:-?} of ${rec:-?}, malformed ${mal:-?}"
	fi
	# every id present in the client's newest snapshot ends mapped (or
	# unresolved: 4b creates it); none lost, none of the wrong kind
	local pu mis kept
	pu=$(echo "$cs" | sed -n 's/.*present unmapped \([0-9]*\).*/\1/p')
	mis=$(echo "$cs" | sed -n 's/.*misfits \([0-9]*\).*/\1/p')
	kept=$(echo "$cs" | sed -n 's/.*kept over gaps \([0-9]*\).*/\1/p')
	if [ "${pu:-1}" = 0 ] && [ "${mis:-1}" = 0 ]; then
		pass "$label: every present id mapped or unresolved at the end; $kept mapping(s) kept for ids back after a gap, no misfits"
	else
		fail "$label: present unmapped '${pu:-?}', misfits '${mis:-?}'"
	fi
	compare "$label" || status=1
}

# check_stale: the client's own sim reused a mapped prop (--net-test-stale):
# it must have seen the stale mapping, asked for the descriptor, and the
# host must have sent it again
check_stale() {
	local label=$1 H=$OUT/$1-host.log C=$OUT/$1-client.log
	local t hs cs stale nsent hn
	t=$(grep "net: stale test:" "$C" | head -1)
	hs=$(grep "net: snap slot 1 " "$H" | tail -1)
	cs=$(grep "net: snap client " "$C" | tail -1)
	stale=$(echo "$cs" | sed -n 's/.*stale seen \([0-9]*\).*/\1/p')
	nsent=$(echo "$cs" | sed -n 's/.*nacks sent \([0-9]*\).*/\1/p')
	hn=$(echo "$hs" | sed -n 's/.*nacks \([0-9]*\).*/\1/p')
	if [ -n "$t" ] && [ "${stale:-0}" -ge 1 ] && [ "${nsent:-0}" -ge 1 ] && [ "${hn:-0}" -ge 1 ]; then
		pass "$label: ${t#*net: }; the client saw $stale stale, sent $nsent nack(s), the host took $hn and resent the descriptor"
	else
		fail "$label: stale test '${t:-none}', stale seen '${stale:-?}', nacks sent '${nsent:-?}', host nacks '${hn:-?}'"
	fi
}

check_hostile() {
	local label=$1 H=$OUT/$1-host.log C=$OUT/$1-client.log hx cx
	hx=$(grep -o "$label: host exit [0-9]*" "$OUT/run.log" | awk '{print $4}')
	cx=$(grep -o "$label: client exit [0-9]*" "$OUT/run.log" | awk '{print $4}')
	local cs; cs=$(grep "net: snap client " "$C" | tail -1)
	echo "     $label client: ${cs#*net: }"
	grep "net: hostile:" "$H" | tail -1 | sed "s/^.*net: /     $label host: /"
	local dec fed rej acc drop
	dec=$(echo "$cs" | sed -n 's/.* \([0-9]*\) decoded.*/\1/p')
	fed=$(echo "$cs" | sed -n 's/.*hostile fed \([0-9]*\).*/\1/p')
	rej=$(echo "$cs" | sed -n 's/.*hostile fed [0-9]* rejected \([0-9]*\).*/\1/p')
	acc=$(echo "$cs" | sed -n 's/.* accepted \([0-9]*\).*/\1/p')
	drop=$(echo "$cs" | sed -n 's/.* accepted [0-9]* dropped \([0-9]*\).*/\1/p')
	# depth: the mangled copies go through the client's real decoder (probe
	# mode) against its real baselines, so many must parse whole and many
	# be refused deep, not just dropped at the seq check
	if [ "$hx" = 0 ] && [ "$cx" = 0 ] && [ -n "$dec" ] && [ "$dec" -gt 200 ] && [ "${fed:-0}" -gt 1000 ] \
			&& [ "${acc:-0}" -gt 100 ] && [ "${rej:-0}" -gt 100 ]; then
		pass "$label: both ran to the end (exit 0) through $fed mangled snapshots ($acc parsed whole, $rej refused, $drop dropped) and the host's mangled commands; the client still decoded $dec"
	else
		fail "$label: host exit '$hx', client exit '$cx', decoded '${dec:-?}', mangled fed '${fed:-?}' parsed '${acc:-?}' refused '${rej:-?}'"
	fi
}

{
	CLIENT_EXTRA="--net-test-stale 600" run clean "$PORT"
	run lossy $((PORT + 1)) --net-sim 5,60
	run hostile $((PORT + 2)) --net-test-hostile --net-sim 5,60
} 2>&1 | tee "$OUT/run.log"
check clean
check_stale clean
check lossy
check_hostile hostile
exit $status
