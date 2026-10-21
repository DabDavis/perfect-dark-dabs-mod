# Windows, from Linux

## Digest (moved from CLAUDE.md, 2026-09-30)

The entries CLAUDE.md carried for this note, verbatim. The sections below are
the long form.

- **The Windows build, wine, the pd.ini format** — [windows-build.md](CLAUDE-notes/windows-build.md): mingw prefix, WinHTTP, and why `Mod.LoadTextures=1` on its own line does nothing
- **A longjmp that kills its thread on Windows only** — [windows-build.md](CLAUDE-notes/windows-build.md), "longjmp unwinds on Windows": mingw's setjmp makes msvcrt's longjmp an SEH unwind, which trusts GCC's unwind tables; geconvert.c defines `__USE_MINGW_SETJMP_NON_SEH` since the Goldfinger 64 24/27 freeze (v3.15.0)


The Windows build is worth doing before pushing anything that touches files,
paths, subprocesses or the network: those are where the two platforms differ and
where nothing in the Linux build will tell you.

```sh
cmake -Bbuild-win -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake .
cmake --build build-win -j8
```

The toolchain file expects a prefix at `~/.local/mingw64`, built once:

- **SDL2** — unpack `SDL2-devel-<ver>-mingw.tar.gz` from libsdl.org and copy its
  `x86_64-w64-mingw32/*` into the prefix.
- **zlib** — `make -f win32/Makefile.gcc PREFIX=x86_64-w64-mingw32- BINARY_PATH=...
  INCLUDE_PATH=... LIBRARY_PATH=...`, then again with `install`. Static `libz.a`,
  so there is no `zlib1.dll` to ship.
- `apt install g++-mingw-w64-x86-64` — the C compiler alone is not enough,
  fast3d is C++.

Watch the configure output for `using WinHTTP - ghost server support enabled`;
without it the updater and the encoder download are compiled out.

Running it under wine: copy `SDL2.dll` from the prefix and
`/usr/lib/gcc/x86_64-w64-mingw32/*-win32/libgcc_s_seh-1.dll` next to the exe, put
the ROM in `build-win/data/`, and

```sh
DISPLAY=:99 WINEDEBUG=-all wine pd.x86_64.exe --savedir 'C:\pdsave'
```

with that directory made under `~/.wine/drive_c/` first. The
`glDebugMessage*KHR` errors in the log are wine's GL lacking `KHR_debug`.

`pd.ini` is a sectioned INI, not a flat one: `Mod.LoadTextures=1` on its own
line is silently ignored, and has to be `[Mod]` then `LoadTextures=1`. The keys
the code registers are `Section.Key`.

## Reading a crash dialog a player sends back

The dialog (`port/src/crash.c`) prints `MAIN MODULE: [base]` and one
`[base]+offset` per frame, because a mingw build has no PDB and dbghelp finds
no names. The offsets are RVAs: `addr2line` wants `0x140000000 + offset`, the
image base mingw links at, and the exe still carries its DWARF.

**Symbolise against the binary that crashed, not the one in `build-win/`.** A
local build of a nearby commit answers with plausible function names for every
frame and all of them are wrong. Download the release the player is on
(`gh release download v3.3.3 -R DabDavis/perfect-dark-dabs-mod -p
'pd.x86_64-windows.exe'`) and check the answer before believing it: every frame
but the innermost is a **return** address, so the bytes just before it must be a
`call`. Disassemble with `--start-address` a little below the address and
`--stop-address` just past it; if the instruction ending there is not a call,
the binary is the wrong one. `+0x1157` sitting right after `call main` is the
CRT frame and is the same in every build, so it proves nothing on its own.

```sh
x86_64-w64-mingw32-addr2line -f -C -i -p -e pd.x86_64-windows.exe 0x1400623fa
x86_64-w64-mingw32-objdump -d --start-address=0x1400623e0 --stop-address=0x140062400 pd.x86_64-windows.exe
```

Two dialogs from two releases that name the same function at slightly different
offsets are a real crash, not a coincidence: the frames agreed because both were
builds of the same source, and the faulting instruction was the same store in
both (see chrs-and-memory.md, the heads Area 51 crash).

## longjmp unwinds on Windows

mingw's `setjmp(buf)` is `_setjmp(buf, __builtin_frame_address(0))`, and handing
msvcrt a frame makes its `longjmp` an SEH unwind: `RtlUnwindEx` walks every
frame between the `longjmp` and that frame, restoring each one's saved
registers from its `.pdata`/`.xdata` unwind codes. On Linux a longjmp just
reloads registers. So on Windows a longjmp is only as good as GCC's unwind
tables, and those can be wrong.

The one we met (v3.15.0, "CONVERTING GOLDFINGER 64 ONCE ONLY - 24/27" frozen on
every Windows machine, F3 20261020-235605): `geconvertRun()` has an 8.7 KB frame,
so GCC sets `rbp` at the top of the prologue (`push %rbp; mov %rsp,%rbp`, then
the pushes and `sub`). Converter 126's prop-scale code gave it an `xmm6` save at
`-0x50(%rbp)`; GCC wrote `.seh_savexmm %xmm6, 0x21e0` - from the final `rsp` -
while the unwinder counts a frame function's save offsets from the frame
register (`rbp` - offset * 16, here `rbp`). The unwind read `xmm6` 8 KB above
the frame, past the top of the converter's thread's stack, and the thread
vanished at `fail()`'s first longjmp (Goldfinger's character 108, the first one
left out) while the main thread drew the notice and waited for it forever. The
CI exe of the build before (3ee8ed0) had the same prologue with no `xmm6`, and
converted Goldfinger fine: any function edit can tip a function into this.
GoldenEye's own conversion never calls `fail()`, so it was never hit there.

`#define __USE_MINGW_SETJMP_NON_SEH` before `<setjmp.h>` makes `setjmp` pass
NULL, and `longjmp` then restores the registers itself without unwinding
(geconvert.c, a8c2ab2d7). Safe there because nothing between a `fail()` and its
`setjmp` has a `__finally` or C++ destructor to run. Do the same for any new
setjmp/longjmp, or avoid longjmp altogether.

Reading this kind of failure:

- The thread just disappears. Under wine `gdb -p` showed the main thread in
  `NtDelayExecution` (the notice loop's `SDL_Delay`) and no converter thread;
  on real Windows it may crash the game instead ("it seemed to crash").
- `x86_64-w64-mingw32-objdump -x pd.x86_64.exe` lists `.pdata` rows
  (begin, end, unwind info address) but does not decode the codes. Read the
  UNWIND_INFO bytes at the row's address (file offset = address - section VMA +
  section file offset, from `objdump -h`): version/flags, prologue size, code
  count, frame register/offset, then two bytes a code (op 8 = SAVE_XMM128 with
  its offset / 16 in the next slot).
- A quick test of a fix on the shipped exe: patch the `mov %rbp,%rdx` before
  each `call *__imp__setjmp` to `xor %edx,%edx; nop` (`48 89 ea` -> `31 d2 90`)
  and run it under wine. The local mingw (Debian GCC 14) did not reproduce the
  bug at all: the release is built by MSYS2's GCC 16, whose code and unwind
  tables are its own, so test the CI exe itself (`gh run download <run> -n
  pd-dabs-mod-x86_64-windows`).
