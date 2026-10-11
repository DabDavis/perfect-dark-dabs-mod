#!/bin/bash
# netlagcomptest.sh — does the host count a remote player's shot where that player saw its target?
#
# Loopback (PLANS/NETPLAY.md "Hits", phase 5b): a listen host and two
# clients on 127.0.0.1, offscreen on the GPU, Temple (0x25; with --rng-seed 1
# the two clients start in sight of each other), Start Armed with the DY357
# (no spread, so a shot goes where it is aimed on both machines),
# --net-test-god on the host (nobody dies, nobody runs dry). Slot 1, the
# shooter, stands still and turns its mouse toward slot 2 as it draws it
# (--net-test-aimat: its interpolated puppet, AIMY units off its place, which
# for a player is the eye: -50 is the chest) and fires whenever the gun is
# ready, about once a second. Slot 2, the target, walks to within 700 units
# of the shooter, aims back at it and strafes right and left a second each:
# sideways to the shot at a steady 7-8 units a tick. Both clients and the host log every shot (--net-lagcomp-log): the
# shooter what its own shot hit as it fired, the host what the same command's
# shot hit there. The hit rate is the share of the shots the shooter saw hit
# the target that the host also scored on it.
#
# Runs (CASES):
#   on       150 ms round trip (--net-sim 0,75 on all three), Net.LagComp=1:
#            the rate at least MINON (95)
#   off      the same with Net.LagComp=0: reported, and it must be clearly
#            under the on run's (by MINGAP, 20 points)
#   loss     150 ms and 2% loss, lag compensation on: at least MINON
#   still    the target standing still (no strafe), lag compensation on and
#            --net-lagcomp-compare: each shot is tested on the host twice, the
#            target rewound and as it is now; the two agree on every shot (no
#            regression for a target that does not move)
#   soak     8 sims and both clients firing all the time (an SMG), 3000
#            frames, lag compensation on, --net-lagcomp-debug: the host ends
#            cleanly, rebuilt rewinds happen, none went without gfx space,
#            and the vtx pool's peak in the second half is no more than in
#            the first (the rewinds' matrices are a tick's, never kept); and
#            no chr is rebuilt over 1000 units from where it is now (sims
#            die and respawn here: a pose from an earlier life is never used)
#   blast    a guest's rockets at a moving target (F3 20261010-030750): the
#            shooter is given the rocket launcher (--net-test-give; every
#            player invincible, so nobody respawns away) and fires at the
#            feet of a target strafing two seconds each way, 150 ms round
#            trip. The host logs every explosion (E), every player where it
#            is and where the guest saw it (P) and every first-frame blast
#            damage (B). Over the rockets that went off while the target was
#            a steady way on, the blast must be BLASTGAIN (40) units nearer
#            where the guest saw the target than where the host has it, the
#            first-frame damage the guest's view gives more than the host's
#            place does, and the damage dealt at least the latter (it is the
#            nearer of the two). Offline the round lands the same way and the
#            target is where the guest saw it (the rewind is the offline
#            answer by construction: CLAUDE-notes/netplay.md).
#
#   netlagcomptest.sh [BIN]   BIN a file name in build/ (pd.x86_64) or a path
#
# Env: OUT (build/netlagcomp-out), PORT (27400; uses PORT..PORT+8), MODDIR
# (mod_allinone), CASES (on off loss still soak), FRAMES (3600), RATEFRAMES
# (8400: the on and loss runs, at least 30 shots), MINON (95),
# MINGAP (20), AIMY (-50), STAGE (0x25), SEED (1), NEAR (700).
# Exit status: 0 all good, 1 a check failed, 2 a run failed to start.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=${BUILD:-$ROOT/build}
OUT=${OUT:-$BUILD/netlagcomp-out}; PORT=${PORT:-27400}
MODDIR=${MODDIR:-mod_allinone}
CASES=${CASES:-on off loss still soak blast}
FRAMES=${FRAMES:-3600}
# the on and loss runs, whose rate is held to MINON: 8400 frames, the
# shooter firing to tick 7800, 36-65 shots the shooter saw hit (one
# minute's 33 put a run at 93.9% on two misses)
RATEFRAMES=${RATEFRAMES:-8400}
MINON=${MINON:-95}
MINGAP=${MINGAP:-20}
AIMY=${AIMY:--50}
BIN=${1:-pd.x86_64}
case $BIN in /*) ;; */*) BIN=$(realpath "$BIN") ;; *) BIN=$BUILD/$BIN ;; esac
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
rm -f "$OUT"/*.log "$OUT"/*.lc
export SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-offscreen} SDL_GAMECONTROLLER_IGNORE_DEVICES=0x054c/0x0ce6,0x045e/0x028e \
	SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT=0x0000/0x0000 SDL_JOYSTICK_HIDAPI=0

status=0
fail() { echo "FAIL $*"; status=1; }
pass() { echo "ok   $*"; }

# the scripts (from to buttons sx sy rsx rsy mdx mdy, by the match's tick):
# Z 0x2000 fires, R_CBUTTONS 0x0001 strafes right
python3 - "$OUT" <<'PY'
import sys
out = sys.argv[1]
def write(name, lines):
    with open("%s/%s" % (out, name), "w") as f:
        for l in lines:
            f.write("%d %d %x %d %d %d %d %g %g\n" % l)
# the shooter fires until tick 7800 (what the 256-line cap left it before;
# the target can be pinned on Temple's wall at x 6270, from as early as tick
# 1300, where grazing shots used to flip between the two machines: the
# host's autoaim locked on 17 ticks before the shooter's, its line of sight
# tested to the target's live place; netplay.md, "Found 2026-10-07")
write("shooter.script", [(t, t, 0x2000, 0, 0, 0, 0, 0, 0) for t in range(150, 7800, 30)])
write("target.script", [(t, t + 59, 0x0001 if (t // 60) % 2 else 0x0002, 0, 0, 0, 0, 0, 0) for t in range(60, 20000, 60)])
write("still.script", [(60, 20000, 0x0000, 0, 0, 0, 0, 0, 0)])
# the blast case's target: two seconds each way, so the lag puts it a steady
# distance on from where the shooter saw it
write("strafe2.script", [(t, t + 119, 0x0001 if (t // 120) % 2 else 0x0002, 0, 0, 0, 0, 0, 0) for t in range(60, 20000, 120)])
write("spray.script", [(t, t + 49, 0x2000, 0, 0, 0, 0, 0, 0) for t in range(150, 20000, 60)])
PY

# game LABEL TIMEOUT INI ARGS... &
game() {
	local label=$1 t=$2 ini=$3; shift 3
	local save=$OUT/save-$label
	rm -rf "$save"; mkdir -p "$save"
	printf '%b' "$ini" > "$save/pd.ini"
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

INI="[Mod]\nStartArmed=1\n[Game]\nPlayer1.CrosshairSway=0\n"

# run LABEL PORT LAGCOMP SIM TARGETSCRIPT [HOSTARGS...]
run() {
	local label=$1 port=$2 lc=$3 sim=$4 tscript=$5; shift 5
	local simargs="--net-sim $sim" weapons=${WEAPONS:-1,1,1,1,1,1}
	local tmo=$((FRAMES / 60 + 340))
	game "$label-host" $((tmo + 10)) "${INI}[Net]\nLagComp=$lc\n${HOSTNET:-}" --moddir "$MODDIR" --host "$port" --net-test-host 2 --net-test-stage ${STAGE:-0x25} \
		--net-test-sims ${SIMS:-0} --rng-seed ${SEED:-1} --mp-weapons "$weapons" --endless --net-test-god \
		--net-lagcomp-log "$OUT/$label-host.lc" --net-lagcomp-compare ${HOSTDEBUG:-} --exit-frame $((FRAMES + 500)) $simargs "$@" &
	local host=$!
	waitfor "$OUT/$label-host.log" "net: hosting on UDP port" 60 || { echo "FAIL: $label host did not start"; kill -TERM $host; exit 2; }
	game "$label-shooter" $tmo "$INI" --moddir "$MODDIR" --connect "127.0.0.1:$port" --net-test-join \
		--net-test-input "$OUT/${SHOOTSCRIPT:-shooter.script}" --net-test-aimat "2,$AIMY,0.5,${NEAR:-700}" --net-lagcomp-log "$OUT/$label-shooter.lc" \
		--exit-frame "$FRAMES" $simargs ${SHOOTARGS:-} &
	local shooter=$!
	# the shooter first, so it is slot 1 and the target slot 2
	waitfor "$OUT/$label-shooter.log" "net: accepted by" 60 || { echo "FAIL: $label shooter was not accepted"; kill -TERM $host $shooter; exit 2; }
	game "$label-target" $tmo "$INI" --moddir "$MODDIR" --connect "127.0.0.1:$port" --net-test-join \
		--net-test-input "$OUT/$tscript" --net-test-aimat "1,$AIMY,0.5,${NEAR:-700}" --net-lagcomp-log "$OUT/$label-target.lc" \
		--exit-frame "$FRAMES" $simargs &
	local target=$!
	wait "$shooter"; echo "     $label: shooter exit $?"
	wait "$target"; echo "     $label: target exit $?"
	wait "$host"; echo "     $label: host exit $?"
}

# rate LABEL: the shooter's hits on the target the host also scored
rate() {
	python3 - "$OUT/$1-shooter.lc" "$OUT/$1-host.lc" "$1" <<'PY'
import sys
cpath, hpath, label = sys.argv[1:4]
C = {}
for l in open(cpath):
    w = l.split()
    if w and w[0] == "C":
        C[int(w[5])] = w[9].split(",")
H = {}
N = {}
rew = []
for l in open(hpath):
    w = l.split()
    if w and w[0] == "H" and w[3] == "1":
        H[int(w[5])] = (w[19].split(","), float(w[9]), int(w[11]))
    elif w and w[0] == "N" and w[3] == "1":
        N[int(w[5])] = w[7].split(",")
saw = [t for t in C if "p2" in C[t]]
match = [t for t in saw if t in H]
both = [t for t in match if "p2" in H[t][0]]
hostonly = [t for t in C if "p2" not in C[t] and t in H and "p2" in H[t][0]]
rw = [H[t][1] for t in match]
capped = sum(1 for t in match if H[t][2])
r = 100.0 * len(both) / len(match) if match else 0
# the same shots against the target as it was when the host played them
nown = [t for t in match if t in N]
now = sum(1 for t in nown if "p2" in N[t])
print("%s %.1f %d %d %d %d %d %.2f %.2f %d %d %d" % (label, r, len(C), len(saw), len(match), len(both), len(hostonly),
      sum(rw) / len(rw) if rw else 0, max(rw) if rw else 0, capped, now, len(nown)))
PY
}

check_ok() {
	local label=$1
	grep -q "renderD128: Permission denied" "$OUT/$label"-*.log && fail "$label: a run fell back to llvmpipe"
	grep -E "segfault|Segmentation|SIGSEGV|crash|net: gfx: " "$OUT/$label"-*.log | head -3 | sed 's/^/     /'
	grep -q "GO" "$OUT/$label-shooter.log" || { fail "$label: the match never started on the shooter"; return 1; }
	return 0
}

declare -A R M
show() {
	# label rate shots saw matched both hostonly rewind-mean rewind-max capped
	set -- $1
	echo "     $1: shooter fired $3, saw $4 hit the target ($5 found on the host); the host scored $6 of them: $2%; host-only hits $7; rewind mean $8 ticks, most $9, capped ${10}; the same shots unrewound (--net-lagcomp-compare) hit ${11} of ${12}"
	R[$1]=$2
	M[$1]=$5
}

# blast LABEL OWNER: the host's explosions of OWNER's rockets and the
# target's (p2) place at each, where it is (the host without the blast
# rewind) and where the guest saw it (with it). Over the rockets that went
# off while the target was a steady way on from that (MOVED, 100 units: the
# lag's distance at a walk), the mean distance from the blast both ways and
# the first-frame damage each would give (a rocket's: 4 x 8 x (1 - d/400)^2,
# the farther floor axis), and B's damage, which the host dealt
blast() {
	python3 - "$OUT/$1-host.lc" "$1" "$2" "${MOVED:-100}" <<'PY'
import sys, math
path, label, owner, moved = sys.argv[1], sys.argv[2], sys.argv[3], float(sys.argv[4])
exps = []
cur = {}
dmg = {}
def hurt(dx, dz):
    d = max(abs(dx), abs(dz))
    return 32.0 * (1 - d / 400.0) ** 2 if d < 400 else 0.0
for l in open(path):
    w = l.split()
    if not w: continue
    if w[0] == "E":
        k = (int(w[1]), w[3])
        cur[w[3]] = k
        if w[5] == owner and w[9] == "1":
            exps.append({"k": k, "x": float(w[11]), "z": float(w[13])})
    elif w[0] == "P" and w[4] == "p2" and w[3] in cur:
        for e in exps:
            if e["k"] == cur[w[3]]:
                cx, cz, rx, rz = float(w[6]), float(w[7]), float(w[9]), float(w[10])
                e["cur"] = math.hypot(cx - e["x"], cz - e["z"])
                e["rew"] = math.hypot(rx - e["x"], rz - e["z"])
                e["hc"] = hurt(cx - e["x"], cz - e["z"])
                e["hr"] = hurt(rx - e["x"], rz - e["z"])
                e["mv"] = math.hypot(cx - rx, cz - rz)
    elif w[0] == "B" and w[5] == owner and w[11] == "p2" and w[3] in cur:
        dmg[cur[w[3]]] = dmg.get(cur[w[3]], 0) + float(w[13])
E = [e for e in exps if "cur" in e]
M = [e for e in E if e["mv"] >= moved]
n = len(M)
avg = lambda f: sum(f(e) for e in M) / n if n else 0
dealt = [dmg.get(e["k"], 0) for e in M]
print("%s %d %d %.0f %.0f %.2f %.2f %.2f" % (label, len(E), n, avg(lambda e: e["cur"]), avg(lambda e: e["rew"]),
      avg(lambda e: e["hc"]), avg(lambda e: e["hr"]), sum(dealt) / n if n else 0))
PY
}

declare -A BN BM BC BR BHC BHR BD
showblast() {
	set -- $1
	echo "     $1: rockets $2, $3 with the target moving on; blast to target where it is $4, where the guest saw it $5 (mean); first-frame damage those ways $6 / $7, dealt $8"
	BN[$1]=$2; BM[$1]=$3; BC[$1]=$4; BR[$1]=$5; BHC[$1]=$6; BHR[$1]=$7; BD[$1]=$8
}

i=0
for c in $CASES; do
	case $c in
		on)    FRAMES=$RATEFRAMES run on $((PORT + i)) 1 "0,75" target.script; check_ok on && show "$(rate on)" ;;
		off)   run off $((PORT + i)) 0 "0,75" target.script; check_ok off && show "$(rate off)" ;;
		loss)  FRAMES=$RATEFRAMES run loss $((PORT + i)) 1 "2,75" target.script; check_ok loss && show "$(rate loss)" ;;
		still) HOSTDEBUG=--net-lagcomp-debug run still $((PORT + i)) 1 "0,75" still.script; check_ok still && show "$(rate still)" ;;
		blast) FRAMES=${BLASTFRAMES:-6000} NEAR=${BLASTNEAR:-1000} AIMY=${BLASTAIMY:--165} run blast $((PORT + i)) 1 "0,75" strafe2.script \
		           --net-test-give 300,0x18 --net-test-invincible
		       check_ok blast && showblast "$(blast blast 1)" ;;
		soak)  FRAMES=3000 SIMS=8 WEAPONS=9,9,9,9,9,9 SHOOTSCRIPT=spray.script HOSTDEBUG=--net-lagcomp-debug \
		           run soak $((PORT + i)) 1 "0,75" spray.script ;;
	esac
	i=$((i + 1))
done

ge() { python3 -c "import sys; sys.exit(0 if float('$1') >= float('$2') else 1)"; }

for c in $CASES; do
	case $c in
		on|loss)
			[ -n "${R[$c]:-}" ] || { fail "$c: no rate"; continue; }
			ge "${R[$c]}" "$MINON" && pass "$c: $c-run hit rate ${R[$c]}% of ${M[$c]} shots (at least $MINON%)" || fail "$c: hit rate ${R[$c]}% under $MINON% (${M[$c]} shots)"
			# a rate of a few shots says little: the run must have enough
			[ "${M[$c]:-0}" -ge 30 ] || fail "$c: only ${M[$c]:-0} shots the shooter saw hit found on the host (want 30)"
			;;
		off)
			[ -n "${R[off]:-}" ] || { fail "off: no rate"; continue; }
			if [ -n "${R[on]:-}" ]; then
				ge "$(python3 -c "print(${R[on]} - ${R[off]})")" "$MINGAP" && pass "off: without lag compensation ${R[off]}%, with ${R[on]}%" \
					|| fail "off: ${R[off]}% is not clearly under the on run's ${R[on]}%"
			else
				echo "     off: ${R[off]}% (no on run to compare)"
			fi
			;;
		still)
			python3 - "$OUT" <<'PY' && pass "still: every shot at the standing target hits the same rewound and as it is now" || fail "still: rewound and unrewound differ"
import sys
out = sys.argv[1]
S = {}
for l in open("%s/still-shooter.lc" % out):
    w = l.split()
    if w and w[0] == "C": S[int(w[5])] = "p2" in w[9].split(",")
H = {}; N = {}; M = {}
cmd = None
for l in open("%s/still-host.lc" % out):
    w = l.split()
    if w and w[0] == "H" and w[3] == "1": cmd = int(w[5]); H[cmd] = "p2" in w[19].split(",")
    elif w and w[0] == "H": cmd = None
    if w and w[0] == "N" and w[3] == "1": N[int(w[5])] = "p2" in w[7].split(",")
    # how far the target moved over the rewind (the debug line): standing still is under a unit
    if w and w[0] == "D" and cmd is not None and w[5] == "p2": M[cmd] = float(w[w.index("moved") + 1])
still = [t for t in H if t in N and M.get(t, 1e9) < 1.0]
moving = [t for t in H if t in N and t not in still]
differ = [t for t in still if H[t] != N[t]]
saw = [t for t in S if S[t] and t in H]
print("     still: %d shots at the target standing still: rewound it was hit %d times, as it is now %d; %d differ (%s). "
      "%d more while it walked up (rewound %d, now %d). The shooter saw %d hits, the host scored %d"
      % (len(still), sum(H[t] for t in still), sum(N[t] for t in still), len(differ), differ[:5],
         len(moving), sum(H[t] for t in moving), sum(N[t] for t in moving), len(saw), sum(H[t] for t in saw)))
sys.exit(0 if len(still) >= 10 and sum(H[t] for t in still) >= 10 and not differ else 1)
PY
			;;
		blast)
			[ -n "${BN[blast]:-}" ] || { fail "blast: no measure"; continue; }
			[ "${BN[blast]}" -ge 20 ] && [ "${BM[blast]}" -ge 5 ] || fail "blast: ${BN[blast]} rockets, ${BM[blast]} with the target moving on (want 20 and 5)"
			python3 -c "
import sys
c, r, hc, hr, d = ${BC[blast]}, ${BR[blast]}, ${BHC[blast]}, ${BHR[blast]}, ${BD[blast]}
sys.exit(0 if c - r >= float('${BLASTGAIN:-40}') and hr > hc and d >= hc else 1)" \
				&& pass "blast: a guest's rockets at a target moving on went off ${BR[blast]} from where it saw it, ${BC[blast]} from where the host had it; the host dealt ${BD[blast]} a rocket (${BHC[blast]} without the rewind, ${BHR[blast]} where the guest saw it)" \
				|| fail "blast: ${BR[blast]} / ${BC[blast]} from the target; damage dealt ${BD[blast]}, ${BHC[blast]} without the rewind, ${BHR[blast]} with"
			;;
		soak)
			[ -e "$OUT/soak-host.log" ] || continue
			line=$(grep -h "net: lagcomp at" "$OUT/soak-host.log" | tail -1)
			echo "     soak: ${line#*net: }" | cut -c1-500
			grep -E "segfault|Segmentation|SIGSEGV|vtx pool|gfx pool|overflow" "$OUT/soak-host.log" | head -3 | sed 's/^/     /'
			python3 - "$line" "$OUT/soak-host.lc" <<'PY' && pass "soak: 3000 frames, 8 sims, two clients spraying: rewinds rebuilt, never short of gfx, the vtx pool's peak flat" || fail "soak"
import sys, re
line, lc = sys.argv[1], sys.argv[2]
m = re.search(r"remote shots (\d+), rewound (\d+).*rebuilt (\d+) \(\d+ not on screen now\), no history (\d+) \(another life then \d+\), claims past the host's bound \d+, no gfx space (\d+), debug lines \d+; vtx pool peak (\d+) / (\d+) bytes \(first half\), (\d+) \(second half", line)
if not m: print("     no summary"); sys.exit(1)
shots, rew, reb, nohist, nospace, p1, pool, p2 = map(int, m.groups())
d = 0
far = []
for l in open(lc):
    if l.startswith("D "):
        d += 1
        w = l.split()
        # a chr rebuilt far from where it is now: a pose from before a
        # respawn or teleport (never: struct netlclife ends the history there)
        if float(w[15]) > 1000:
            far.append(l.strip()[:160])
print("     soak: %d remote shots, %d rewound, %d chrs rebuilt (%d debug lines, %d rebuilt over 1000 units from where they are now), vtx peak %.1f%% then %.1f%% of %d" % (shots, rew, reb, d, len(far), 100.0 * p1 / pool, 100.0 * p2 / pool, pool))
for l in far[:3]:
    print("       " + l)
ok = shots > 200 and rew > 100 and reb > 50 and nospace == 0 and p2 <= p1 * 1.10 + 4096 and p2 < pool and d > 0 and not far
sys.exit(0 if ok else 1)
PY
			grep -q "net: lagcomp at" "$OUT/soak-host.log" || fail "soak: the host never logged its summary (crashed?)"
			;;
	esac
done

[ $status = 0 ] && echo "netlagcomptest: all good" || echo "netlagcomptest: FAILED"
exit $status
