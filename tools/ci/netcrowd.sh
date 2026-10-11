#!/bin/bash
# netcrowd.sh - not a gate: a crowded match measured (snapshot ramp, stale
# records posed, the reliable EVENTS channel), for before/after comparisons.
# MODE=ge: GoldenEye's Facility (GEMAP=) from build/mods; MODE=mp (default): a --dedicated host on Skedar with SIMS (24) sims
# spraying SMGs, two clients from the start and a third joining in progress
# LATE seconds in; MODE=coop: a listen host on Defection co-op and two
# clients. Every game at SIM (--net-sim LOSS,ONEWAYMS; 2,75 = 150 ms RTT,
# 2% loss each way). Read the host's "snap slot"/"traffic slot" lines and
# the clients' "puppets"/"events client" lines (CLAUDE-notes/netplay.md,
# Traps "Snapshots in a crowded match").
#   netcrowd.sh BIN LABEL      logs in build/crowd/LABEL; CMOD= for coop clients
# STAGE= the mp arena (0x32); WRECK=T1,T2 has the host break half the
# stage's glass and windowed doors, crumple six objects and start an N-bomb
# storm at each host tick (the first lets the gas go too: networld.c);
# CENSUS=T has every machine count its world at tick T ("world census"
# lines), so a late joiner's broken glass, doors, deforms, storms and gas
# can be held against the host's. Each client logs "events lag by type".
# MPWEAPONS= the mp weapon set (9,9,9,9,9,9: SMGs; 23 rockets, 22 grenades
# put explosions in the match).
set -u
BIN=$1; LABEL=$2
SIMS=${SIMS:-24}; SIM=${SIM:-2,75}; PORT=${PORT:-27370}; LATE=${LATE:-25}
STAGE=${STAGE:-0x32}
TEST=""; [ -n "${WRECK:-}" ] && TEST="--net-test-wreck $WRECK"
CTEST=""; [ -n "${CENSUS:-}" ] && { TEST="$TEST --net-test-census $CENSUS"; CTEST="--net-test-census $CENSUS"; }
BUILD=$(cd "$(dirname "$0")/../.." && pwd)/build
OUT=$BUILD/crowd/$LABEL; rm -rf "$OUT"; mkdir -p "$OUT"
export SDL_VIDEODRIVER=offscreen SDL_GAMECONTROLLER_IGNORE_DEVICES=0x054c/0x0ce6,0x045e/0x028e \
	SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT=0x0000/0x0000 SDL_JOYSTICK_HIDAPI=0
{
	for t in $(seq 60 300 9000); do
		echo "$t $((t + 140)) 0008 0 0 0 0 0.3 0"
		echo "$((t + 150)) $((t + 230)) 2000 0 0 0 0 -0.4 0"
		echo "$((t + 240)) $((t + 290)) 2000 0 0 0 0 0 0"
	done
} > "$OUT/client.script"
game() {
	local label=$1 t=$2 ini=$3; shift 3
	local save=$OUT/save-$label
	mkdir -p "$save"; printf '%b' "$ini" > "$save/pd.ini"
	cd "$BUILD" || exit 2
	exec timeout -k 5 "$t" stdbuf -oL -eL "$BIN" --savedir "$save" --skip-intro --no-sound "$@" > "$OUT/$label.log" 2>&1
}
waitfor() { local n=$3; while [ $n -gt 0 ]; do grep -q -- "$2" "$1" 2>/dev/null && return 0; sleep 1; n=$((n-1)); done; return 1; }
GEINI="[Mod]\nModDir=\nMapMods=GoldenEye Arenas\nStartArmed=1\n"
if [ "${MODE:-mp}" = ge ]; then
# MODE=ge: GoldenEye's GEMAP (Facility: its fog has the second sky the gas
# fades to) from build/mods/GoldenEye Arenas, a --dedicated host and SIMS sims
game h 150 "${GEINI}[Net]\nMaxPlayers=12\nJoinInProgress=1\n" --dedicated --host $PORT --net-test-host 2 \
	--net-test-map "${GEMAP:-Facility}" --net-test-ge 1 --net-test-sims $SIMS --net-test-timelimit 2 --rng-seed 7 --net-sim $SIM $TEST &
CMOD=""
elif [ "${MODE:-mp}" = coop ]; then
# COOP= the mission index (0), COOPGAME=ge GoldenEye's (4 Bunker: cameras)
if [ "${COOPGAME:-}" = ge ]; then
game h 150 "[Mod]\nMapMods=GoldenEye Arenas\nMissionRespawn=1\n[Net]\nMaxPlayers=12\nJoinInProgress=1\n" --host $PORT --net-test-host 2 --rng-seed 7 \
	--net-test-coop ${COOP:-4} --net-test-coop-game ge --skip-cutscenes --net-sim $SIM $TEST &
else
game h 150 "[Mod]\nMissionRespawn=1\n[Net]\nMaxPlayers=12\nJoinInProgress=1\n" --host $PORT --net-test-host 2 --rng-seed 7 --net-test-coop ${COOP:-0} --skip-cutscenes --net-sim $SIM $TEST &
fi
else
game h 150 "[Mod]\nStartArmed=1\n[Net]\nMaxPlayers=12\nJoinInProgress=1\n" --moddir mod_allinone --dedicated --host $PORT \
	--net-test-host 2 --net-test-stage $STAGE --net-test-sims $SIMS --net-test-timelimit 2 --net-test-scorelimit 100 --net-test-teamscorelimit 400 --rng-seed 7 \
	--mp-weapons ${MPWEAPONS:-9,9,9,9,9,9} --net-sim $SIM $TEST &
fi
H=$!
CINI="[Mod]\nStartArmed=1\n"; [ "${MODE:-mp}" = ge ] && CINI=$GEINI; [ "${COOPGAME:-}" = ge ] && CINI="[Mod]\nMapMods=\n"
waitfor "$OUT/h.log" "net: hosting on UDP port" 60 || { echo "host did not start"; kill -TERM $H; exit 2; }
P=""
for k in 1 2; do
	game c$k 140 "$CINI" ${CMOD---moddir mod_allinone} --connect 127.0.0.1:$PORT --net-sim $SIM --net-test-input "$OUT/client.script" $CTEST &
	P="$P $!"; sleep 0.5
done
sleep $LATE
game c3 100 "$CINI" ${CMOD---moddir mod_allinone} --connect 127.0.0.1:$PORT --net-sim $SIM --net-test-input "$OUT/client.script" $CTEST &
P="$P $!"
sleep 50
kill -TERM $H $P 2>/dev/null
for p in $H $P; do while kill -0 $p 2>/dev/null; do sleep 1; done; done
echo done
