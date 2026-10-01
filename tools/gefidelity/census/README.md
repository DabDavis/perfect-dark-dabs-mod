# census - which ROM bytes the converter never reads, and which of those GoldenEye does

| tool | contract |
|---|---|
| `census.py` | builds `port/src/geconvert.c` (the only converter - the Python twin was retired 2026-10-01) with every load instrumented (`ctrack/`), converts the ROM, walks every file it read the way GoldenEye's loaders do, and lists per record kind and GoldenEye field the bytes that are **non-zero and never read**, with the cartridge's verdict on each beside it; `report.md` / `report.json` here. **Null on every run** (Dam's pad positions hidden from a second read map must all come back dropped, and all be read in the real one), else exit 4 |
| `ctrack/build.sh`, `ctrack/shim.c` | the read-tracked converter: geconvert.c compiled as it is with GCC's kernel-address instrumentation (every load calls `__asan_load*_noabort`, which shim.c implements), library reads wrapped at the link (memcpy/memmove/memcmp, zlib inflate/deflate, fwrite, and the allocator so provenance follows a moved buffer). Its conversion is byte for byte the plain build's (checked 2026-10-01 with `../convdiff/filecmp.py`: 2888 files identical). Nothing of it is in the game's build |
| `ares/run.py sweep` + `ares/readpass.py` | **the cartridge's column**: GoldenEye on the real cartridge in ares (`n64twin`'s `readwatch-*` commands over the core hook `../ares/ares-readhook.patch`), armed at the level's own load, each of the twenty missions played 60-90 s from the quiet start with its guards alive; every CPU read of the setup, bg (its resident part), stan, data segment and every loaded model is mapped back to the file offset through GoldenEye's own pointers. Outputs under `~/wt/gefidelity-run/census-ares/` |
| `ares/run.py join` | `cart.json`: per record kind and field, the records the cartridge was seen holding, those it read (during the load and during play) and the functions that read them. **Null**: every one of Dam's pads must have its plink read (prop.c's relocation walk) and at least half their positions; Dam's pad-name strings (debug only) must show no read at all; Archives' DOOR_SCALE scale must be read (g_DoorScale is 0.75 there - the read that showed the JIT's fast path hiding 93% of the loads); else exit 4 |
| `gatesweep.py --tree SRC --out DIR` | the gate's `census` leg: findings keyed `<record kind>.<field>` per mission - `census.dropped` (the cartridge reads it), `census.partial`, `census.unread` (it never does, or nothing observed it: listed, never failing). Null: the census's own, and two runs on one tree must give the same keys and magnitudes |
| `gen_fields.py` | regenerates `fields.json`, every record layout field by field, from the DWARF of a probe built on 10.8.0.3 with the GE port's own flags (i386, so every offset is the ROM's) |
| `romlayout.py` | where the census's walks find things in the ROM (data segment, file table, prop/character rows), geconvert.c's numbers |

    python3 census.py                       # ~80 s: build, convert (instrumented), census; reads ../../../wt/gefidelity-run/census-ares/cart.json if there
    python3 census.py --reuse               # ~40 s: the census again over the saved maps
    python3 census.py --tree OTHER_TREE     # another tree's geconvert.c
    python3 ares/run.py sweep               # ~1 h on the oracle host: all twenty missions (--missions dam,frigate --frames 3600 --jobs 3)
    python3 ares/run.py join                # seconds: cart.json, then census.py --reuse puts it in the report

## Reading the report

- **dropped** - a record whose field has a non-zero byte the converter never
  looked at. **partial** - the field was read in part (a byte of a word) and a
  non-zero byte was not: the shape of the render-mode bug (converter 36).
- **read by the cartridge** - `read N/M (functions)`: of M records of that kind
  the cartridge held in any mission, N had the field read, by those functions;
  `never (0/M)`; `not observed` (arena setups, the ROM-side animation records,
  room display lists: nothing the read watch maps). A field read only by
  `modelPromoteNodeOffsetsToPointers` is a pointer being relocated, not used.
- **Carried without being looked at** - fields that only reached the output by
  a copy (memcpy under 4096 bytes, or a bulk copy that reached zlib/fwrite).
  Not lost, but Perfect Dark reads them with its own struct, so each is a place
  where the two games' meaning of the bytes must agree (the 0x200 object flag
  was Perfect Dark's OBJFLAG_ORTHOGONAL).
- **Bytes no walk placed** - non-zero bytes in a file no segmenter assigns to a
  record (debug strings, data past a terminator, a model's arrays reached only
  by a dropped pointer).

## Traps

- **The JIT hides reads.** ares' recompiler reads cached RDRAM in its own code
  without calling `CPU::read`; the read watch forces its slow path while armed
  (`ares-readhook.patch`, recompiler.cpp). The first sweep (2026-10-01, kept in
  `census-ares/v1-jit-missed/`) ran without it and saw 7% of the reads: it said
  GoldenEye never read the door scale it had just applied.
- The PC a read is credited to is the last recompiled block's entry; after an
  interrupt that is the OS dispatcher's (`__osDispatchThread`, `70010a7c`), not
  the code that resumed.
- A copy of 4096 bytes or more is not a read: it hands its provenance to the
  destination (the bg file copied out of the ROM, a setup copied to build the
  later cartridges' revision), and the destination's own reads count. Shorter
  copies are reads (as copies).
- The cartridge's quiet minute is not the whole game: a field GoldenEye reads
  only when a guard is shot (collision vertices: `chrCreateBloodStain`), when
  an object is destroyed or in multiplayer will read `never`. `never` is
  "not in a quiet minute of every mission", not "junk".
- The bg file is mostly not resident on the cartridge: GoldenEye keeps the
  primary part and loads each room's compressed blob from the ROM when the room
  loads, so only the part before the first blob is mapped.
- A range whose memory agrees with the file on fewer than half its non-zero
  bytes is refused (a front-end model whose header still points at memory the
  level reused).
- A dropped field may be one GoldenEye itself never reads (debug pad names, the
  stan footer's "unstable" string, a duplicate AI list `ailistFindById` never
  finds) or one the port recomputes (model node links). Read the decomp's use
  of the field before calling it a gap; the cartridge column is that check.
