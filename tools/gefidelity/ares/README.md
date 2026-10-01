# ares/ - GoldenEye on the real cartridge

The twin driver's GoldenEye side on the US ROM in ares, instead of the
decomp's native port (user 2026-10-01: "the ge007 port honestly is not
complete ... Ares would be the way for total accuracy"). Select it with
`twin.py --oracle ares` (`world/sweep.py` uses it by default); `--oracle port`
is the native port under gdb, unchanged.

| tool | contract |
|---|---|
| `twin.cpp` (built on the oracle host as `~/claude-007/ares/ares-nightly/build/n64twin`, target added by `CMakeLists.twin.txt` to `oracle/CMakeLists.txt`) | ares headless, steered one line per command on stdin, every answer an `ok`/`err` line: `frames`, `frame`, `until-word ADDR OP VAL`, `until-pc PC`, `on-pc PC [if REG VAL] set-gpr REG VAL / poke ADDR HEX`, `until-fired N`, `fired`, `peek`, `poke`, `pad`, `cue`, `shot`, `quit`. Reads and writes go through the CPU's data cache; PCs are function entries only (the recompiler's prologue) |
| `../common/aresge.py` | gdbge.py's API over n64twin: boot (START through the front end until GoldenEye asks for Dam, the level swapped in at bossSetLoadedStage's a0, the difficulty written at proplvreset2, the opening still dismissed 190 frames in), tick/until_tick/frames, place/hold on a named tile, shot, pads/props/chrs/player/world. Controls in boot: the difficulty must read back and both one-shot actions must have fired |
| `../common/ares/gdbge.py` | the shim that hands an unchanged scenario aresge when twin.py runs it on ares |
| `../common/gen_ares_layout.py` -> `../common/ares_layout.json` | symbols from the N64 ELF (`build/u/ge007.u.elf`, the US ROM byte for byte) and record layouts from the -m32 port's DWARF; regenerate with `gdb -batch -x gen_ares_layout.py build/port/ge007` from `~/claude-007/007`. Fails loudly (`problems`) if a ROM struct's size moved |
| `padshot.py` | Bond on a pad's own tile, a picture per heading, using only the API (no function calls): `twin.py ge ares/padshot.py --oracle ares --mission frigate --env GF_PAD=151` |

Measured 2026-10-01: Dam's world dump in 18 s (the native port: ~55 s); all
twenty missions in 87 s. At tick 1 the cartridge and the native port agree on
every pad, every setup record and every chr field but the random heads (Dam:
462 pads, 328 records), which is the driver's cross-check; every placement
finding of the 20-mission world diff is the same on both.

## Traps

- The cartridge reaches each folder screen tens of frames later than the
  native port, so a fixed pad script (solo-quiet.padscript) stalls on the
  briefing; boot() presses START until the Dam load fires instead.
- A stop is at the end of a video frame: `until_tick(300)` lands on 300-303.
- No function calls: record sizes are sizepropdef()'s table in aresge.py, Bond
  is placed on a tile you name (`pad_tile(n)`), the game's walk sets his height.
- `currentFrameCounter` holds garbage until the game initialises it; do not
  wait on it before the first frames have run.
- The native port with rendering off (`PORT_RENDER_FROM`) never sets the
  on-screen prop flag and spawns fewer chrs by tick 300 than the cartridge;
  behaviour after the opening is the cartridge's to judge.
