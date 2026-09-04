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
without it the updater and the Upscayl download are compiled out.

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
