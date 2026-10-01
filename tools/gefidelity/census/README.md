# census - which ROM bytes the converter never reads

| tool | contract |
|---|---|
| `census.py` | runs the Python converter over the whole ROM with every byte it reads tracked, walks every file it read the way GoldenEye's loaders do, and lists per record kind and GoldenEye field the bytes that are **non-zero and never read**; `report.md` / `report.json` here. **Null on every run** (Dam's pad positions hidden from a second read map must all come back dropped, and all be read in the real one), else exit 4 |
| `tracker.py` | the byte tracker: a stand-in for `bytes` that is not one, so every read goes through it (`d[i]`, `struct.unpack[_from]`, `int.from_bytes`) or through `__buffer__` (a copy) |
| `gen_fields.py` | regenerates `fields.json`, every record layout field by field, from the DWARF of a probe built on 10.8.0.3 with the GE port's own flags (i386, so every offset is the ROM's) |

    python3 census.py                  # ~2.5 min: converter (tracked) + census
    python3 census.py --reuse          # ~25 s: census again over the saved read maps
    python3 census.py --levels dam     # one level (the menus, characters and models are still all read)

Outputs beyond the report go to `~/wt/gefidelity-run/census-out` (`--out`): the
tracked conversion (`py-tracked/`, byte-identical to an untracked one - check
with `../parity/compare.py`) and `maps.pkl`, the read maps `--reuse` loads.

## Reading the report

- **dropped** - a record whose field has a non-zero byte the converter never
  looked at. **partial** - the field was read in part (a byte of a word) and a
  non-zero byte was not: the shape of the render-mode bug (converter 36).
- **Carried without being looked at** - fields that only reached the output by a
  wholesale copy. Not lost, but Perfect Dark reads them with its own struct, so
  each is a place where the two games' meaning of the bytes must agree (the
  0x200 object flag was Perfect Dark's OBJFLAG_ORTHOGONAL). A field copied into a
  bytearray and parsed from the copy also lands here.
- **Bytes no walk placed** - non-zero bytes in a file no segmenter assigns to a
  record (debug strings, data past a terminator, a model's arrays reached only
  by a dropped pointer).

## Traps

- **It instruments the Python twin.** The game runs `port/src/geconvert.c`.
  A census row is a fact about the C converter only while `../parity/parity.sh`
  says IDENTICAL. When parity differs, check each row against geconvert.c
  before acting on it: on 2026-10-01 the C converter carried guard grenade
  probabilities (converter 93) and windowed doors' glass distances that the
  Python twin still dropped.
- A dropped field may be one GoldenEye itself never reads (debug pad names, the
  stan footer's "unstable" string, a duplicate AI list `ailistFindById` never
  finds) or one the port recomputes (model node links). Read the decomp's use
  of the field (`grep` its name in `src/game`) before calling it a gap.
- Setups copied to build the later cartridges' revisions are handed to
  `revision_setup()` untracked, so patching a copy is not taken for reading it.
