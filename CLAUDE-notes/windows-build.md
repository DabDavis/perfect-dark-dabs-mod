# Windows, from Linux

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
