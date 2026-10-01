#!/bin/bash
# The GoldenEye XBLA ("Bean") oracle: Bean in the self-built xenia-canary on a
# private Xvfb, a virtual Xbox 360 pad, and the twin channel (xenia-twin.patch)
# on two FIFOs.
#
#   rig.sh start      Xvfb + pad + Xenia, state in $GF_XENIA_STATE
#   rig.sh stop       kill -9 each PID this rig started (Xenia ignores SIGTERM)
#   rig.sh shot PATH  the window's game area (under Xenia's 25 px menu bar), 1280x695
#   rig.sh pad CMD    one pad.py command ("press A", "stick LY -32000 1.0", ...)
#
# Copied from .xbla-work/ge-bean/xenia/run.sh (and the PD XBLA rig in
# .xbla-work/gunturn), which keep working as they are. Traps from those rigs:
# Xvfb has no DRI3, so MESA_VK_WSI_DEBUG=sw or the window stays grey; the
# user's DualSense is hidden from Xenia's SDL input; one Xenia at a time (both
# rigs share the binary's portable content directory, profile and all).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ST=${GF_XENIA_STATE:-$HOME/wt/gefidelity-run/xenia-out/rig}
DISP=${GF_XENIA_DISPLAY:-:118}
BIN=${GF_XENIA_BIN:-/home/sdg/perfect-dark/xenia-canary/build/bin/Linux/Release/xenia_canary}
# GE Plus's XBLA look uses the Community Edition, so the oracle is the CE build
# by default (GF_BEAN=ce): the release with CommunityEditionUpdaterV6 applied,
# made once into .xbla-work/ge-bean/BeanCE (defaultCE.xex = xex.diff applied
# with hpatchz, filesCE, musCE.xwb; CLAUDE-notes/ge-bean.md, "The Community
# Edition's fixes in the regular build"). GF_BEAN=retail runs the release as
# it shipped. GF_XENIA_PKG overrides both.
case "${GF_BEAN:-ce}" in
	ce) PKG=${GF_XENIA_PKG:-/home/sdg/perfect-dark/.xbla-work/ge-bean/BeanCE/defaultCE.xex} ;;
	retail) PKG=${GF_XENIA_PKG:-/home/sdg/perfect-dark/.xbla-work/ge-bean/Bean/default.xex} ;;
	*) echo "GF_BEAN is ce or retail"; exit 2 ;;
esac
LOCK=${GF_XENIA_LOCK:-$HOME/wt/gefidelity-run/xenia.lock}   # machine-wide: every Xenia rig takes it
mkdir -p "$ST"

case "${1:-}" in
start)
	# ONE RIG ON THIS MACHINE AT A TIME, held by an exclusive flock on $LOCK for
	# the rig's whole life. Not just one Xenia: the virtual pad is a /dev/uinput
	# device, global to the machine, and Xenia's SDL input (--hid=sdl) takes
	# every pad it sees - so two rigs (even one Xenia) send each other's presses
	# into the wrong game. pad.py is only ever started here, under the lock.
	# A pgrep-then-start check is a race; the lock is not.
	if [ -f "$ST/lock.pid" ] && kill -0 "$(cat "$ST/lock.pid")" 2>/dev/null; then
		echo "this rig already holds the lock (holder $(cat "$ST/lock.pid")); rig.sh stop first"; exit 1
	fi
	rm -f "$ST/locked"
	# (its output away from ours: a caller reading rig.sh's output to the end
	# would wait on this holder forever)
	( flock -x 9; echo "$ST $$ $(date +%F_%T)" > "$LOCK.who"; touch "$ST/locked"; exec sleep infinity ) 9>"$LOCK" </dev/null >/dev/null 2>&1 &
	echo $! > "$ST/lock.pid"
	waited=0
	while [ ! -f "$ST/locked" ]; do
		if [ $waited -gt 0 ] && [ $((waited % 30)) -eq 0 ]; then echo "waiting for the Xenia rig lock, held by: $(cat "$LOCK.who" 2>/dev/null)"; fi
		sleep 1; waited=$((waited + 1))
		if [ $waited -ge ${GF_XENIA_LOCK_WAIT:-3600} ]; then
			kill "$(cat "$ST/lock.pid")" 2>/dev/null; rm -f "$ST/lock.pid"
			echo "gave up waiting for the Xenia rig lock"; exit 1
		fi
	done
	other=$(pgrep -x xenia_canary | tr '\n' ' ')
	if [ -n "$other" ]; then
		# someone ran Xenia without the lock: do not add a second game to it
		echo "a xenia_canary outside the lock is running (pid $other); not starting"
		kill "$(cat "$ST/lock.pid")" 2>/dev/null; rm -f "$ST/lock.pid" "$ST/locked"; exit 1
	fi
	for p in xenia pad xvfb; do
		if [ -f "$ST/$p.pid" ] && kill -0 "$(cat "$ST/$p.pid")" 2>/dev/null; then
			echo "rig already running ($p $(cat "$ST/$p.pid")); rig.sh stop first"; exit 1
		fi
	done
	Xvfb "$DISP" -screen 0 1280x720x24 -nolisten tcp < /dev/null > "$ST/xvfb.log" 2>&1 &
	echo $! > "$ST/xvfb.pid"
	sleep 2
	rm -f "$ST/pad.fifo" "$ST/cmd" "$ST/ans"
	mkfifo "$ST/cmd" "$ST/ans"
	python3 "$HERE/pad.py" "$ST/pad.fifo" < /dev/null > "$ST/pad.log" 2>&1 &
	echo $! > "$ST/pad.pid"
	sleep 2
	cd "$(dirname "$BIN")"
	DISPLAY=$DISP MESA_VK_WSI_DEBUG=sw SDL_AUDIODRIVER=dummy \
	SDL_GAMECONTROLLER_IGNORE_DEVICES=0x054c/0x0ce6 XE_TWIN_DIR="$ST" \
		"$BIN" --license_mask=1 --logged_profile_slot_0_xuid=E030000019D6EFBA --discord=false \
		--mute=true --gpu=vulkan --hid=sdl --log_file="$ST/xenia.log" --log_to_stdout=false \
		${XENIA_EXTRA:-} "$PKG" < /dev/null > "$ST/xenia.out" 2>&1 &
	echo $! > "$ST/xenia.pid"
	echo "$PKG" > "$ST/package"
	echo "xvfb $(cat "$ST/xvfb.pid") pad $(cat "$ST/pad.pid") xenia $(cat "$ST/xenia.pid") on $DISP ($PKG)"
	;;
stop)
	[ -p "$ST/pad.fifo" ] && timeout 2 sh -c "echo quit > '$ST/pad.fifo'"
	for p in xenia pad xvfb lock; do
		if [ -f "$ST/$p.pid" ]; then
			pid=$(cat "$ST/$p.pid")
			kill -9 "$pid" 2>/dev/null
			# wait for it to be gone: start refuses while a xenia_canary exists
			for i in $(seq 50); do kill -0 "$pid" 2>/dev/null || break; sleep 0.2; done
		fi
		rm -f "$ST/$p.pid"
	done
	rm -f "$ST/locked"
	echo stopped
	;;
shot)
	DISPLAY=$DISP import -window root -crop 1280x695+0+25 +repage "$2" && echo "$2"
	;;
pad)
	shift
	echo "$*" > "$ST/pad.fifo"
	;;
*)
	echo "usage: rig.sh start|stop|shot PATH|pad CMD"; exit 2
	;;
esac
