#!/bin/sh
# Build n64twin on the oracle host from THIS tree's ares/twin.cpp, one build at a
# time, and install it atomically: several agents changed twin.cpp at once and
# rebuilt in place while sweeps were starting it.
#
#   ares/build.sh            push twin.cpp and ares-readhook.patch, apply the patch
#                            if the host's core lacks it, build, install
#
# The tree's tools/gefidelity/ares/twin.cpp is the source of truth: edit it here
# (only the n64twin owner, see OWNER), never the host's copy. The core change the
# read watch needs (cpuReadHook, ares-readhook.patch) is applied under the same
# lock when ares/n64/cpu/memory.cpp or recompiler.cpp does not carry it yet (hunks
# already there are skipped), and checked to carry it afterwards. The binary is built as build/n64twin, copied to
# build/n64twin.new and moved over build/n64twin.installed, so a reader sees the
# old binary or the new one, never half of one; aresge.py runs a snapshot of it.
set -eu
HOST=${GF_GE_HOST:-sdg@10.8.0.3}
HERE=$(cd "$(dirname "$0")" && pwd)
ssh -o BatchMode=yes "$HOST" "mkdir -p ~/gefidelity-bin"
scp -q "$HERE/twin.cpp" "$HOST:gefidelity-bin/twin.cpp.incoming.$$"
scp -q "$HERE/ares-readhook.patch" "$HOST:gefidelity-bin/ares-readhook.patch.incoming.$$"
ssh -o BatchMode=yes "$HOST" "exec 9>~/gefidelity-bin/build.lock && flock -w 1800 9 && \
  cd ~/claude-007/ares/ares-nightly && \
  { grep -q cpuReadHook ares/n64/cpu/memory.cpp && grep -q cpuReadHook ares/n64/cpu/recompiler.cpp || \
    patch -p1 -N -r - < ~/gefidelity-bin/ares-readhook.patch.incoming.$$ || true; } && \
  grep -q cpuReadHook ares/n64/cpu/memory.cpp && grep -q cpuReadHook ares/n64/cpu/cpu.hpp && \
  grep -q cpuReadHook ares/n64/cpu/recompiler.cpp && \
  rm -f ~/gefidelity-bin/ares-readhook.patch.incoming.$$ && \
  cp ~/gefidelity-bin/twin.cpp.incoming.$$ oracle/twin.cpp && rm -f ~/gefidelity-bin/twin.cpp.incoming.$$ && \
  cmake --build build --target n64twin -j4 > ~/gefidelity-bin/build.log 2>&1 && \
  cp build/n64twin build/n64twin.new && mv -f build/n64twin.new build/n64twin.installed && \
  echo built: \$(md5sum oracle/twin.cpp) \$(stat -c '%s bytes, %y' build/n64twin.installed)" \
  || { echo "build failed; see $HOST:gefidelity-bin/build.log" >&2; exit 2; }
