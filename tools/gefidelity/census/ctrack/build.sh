#!/bin/sh
# Build the read-tracked standalone converter: port/src/geconvert.c from TREE,
# instrumented (every load calls shim.c), linked with shim.c's main and wraps.
#   build.sh TREE OUTBIN
# Nothing here touches the game's own build; geconvert.c is compiled as is.
set -eu
TREE=$(cd "$1" && pwd)
OUT=$2
HERE=$(cd "$(dirname "$0")" && pwd)
B=$(dirname "$OUT")
mkdir -p "$B"
INC="-I$TREE/port/include -I$TREE/include -I$TREE/src/include"
gcc -O2 -std=gnu11 -fno-builtin -fsanitize=kernel-address \
	--param asan-instrumentation-with-call-threshold=0 --param asan-stack=0 --param asan-globals=0 \
	$INC -c "$TREE/port/src/geconvert.c" -o "$B/geconvert.census.o"
gcc -O2 -std=gnu11 -fno-builtin $INC -c "$HERE/shim.c" -o "$B/shim.o"
gcc "$B/geconvert.census.o" "$B/shim.o" -o "$OUT" -lz -lm \
	-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free,--wrap=memcpy,--wrap=memmove \
	-Wl,--wrap=memcmp,--wrap=inflate,--wrap=deflate,--wrap=fwrite
