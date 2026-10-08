#!/bin/bash
# netcrowd.sh - not a gate: a crowded match measured (snapshot ramp, stale
# records posed, the reliable EVENTS channel), for before/after comparisons.
# MODE=mp (default): a --dedicated host on Skedar with SIMS (24) sims
# spraying SMGs, two clients from the start and a third joining in progress
# LATE seconds in; MODE=coop: a listen host on Defection co-op and two
# clients. Every game at SIM (--net-sim LOSS,ONEWAYMS; 2,75 = 150 ms RTT,
# 2% loss each way). Read the host's "snap slot"/"traffic slot" lines and
# the clients' "puppets"/"events client" lines (CLAUDE-notes/netplay.md,
# Traps "Snapshots in a crowded match").
#   netcrowd.sh BIN LABEL      logs in build/crowd/LABEL; CMOD= for coop clients
set -u
BIN=$1; LABEL=$2
SIMS=${SIMS:-24}; SIM=${SIM:-2,75}; PORT=${PORT:-27370}; LATE=${LATE:-25}
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
if [ "${MODE:-mp}" = coop ]; then
game h 150 "[Mod]\nMissionRespawn=1\n[Net]\nMaxPlayers=12\n" --host $PORT --net-test-host 2 --rng-seed 7 --net-test-coop 0 --skip-cutscenes --net-sim $SIM &
else
game h 150 "[Mod]\nStartArmed=1\n[Net]\nMaxPlayers=12\nJoinInProgress=1\n" --moddir mod_allinone --dedicated --host $PORT \
	--net-test-host 2 --net-test-stage 0x32 --net-test-sims $SIMS --net-test-timelimit 2 --net-test-scorelimit 100 --net-test-teamscorelimit 400 --rng-seed 7 \
	--mp-weapons 9,9,9,9,9,9 --net-sim $SIM &
fi
H=$!
waitfor "$OUT/h.log" "net: hosting on UDP port" 60 || { echo "host did not start"; kill -TERM $H; exit 2; }
P=""
for k in 1 2; do
	game c$k 140 "[Mod]\nStartArmed=1\n" ${CMOD---moddir mod_allinone} --connect 127.0.0.1:$PORT --net-sim $SIM --net-test-input "$OUT/client.script" &
	P="$P $!"; sleep 0.5
done
sleep $LATE
game c3 100 "[Mod]\nStartArmed=1\n" ${CMOD---moddir mod_allinone} --connect 127.0.0.1:$PORT --net-sim $SIM --net-test-input "$OUT/client.script" &
P="$P $!"
sleep 50
kill -TERM $H $P 2>/dev/null
for p in $H $P; do while kill -0 $p 2>/dev/null; do sleep 1; done; done
echo done
