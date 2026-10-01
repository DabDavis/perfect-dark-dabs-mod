# parity - the converter twin gate

| tool | contract |
|---|---|
| `parity.sh` | converts the ROM with `tools/geconvert/geconvert.py` and with `port/src/geconvert.c` (the game binary booted once in a scratch run dir, or `--standalone`: the C built alone with `-DGECONVERT_MAIN`) and compares every file; exit 0 only when identical, 1 differs, 2 a converter failed, 3 the null failed |
| `compare.py` | the comparison: raw bytes, else the inflated bytes of 1173-compressed files, else a bg `.seg` part by part (room pointers as ordinals); first differing offset per file, one-sided files against `allow.txt`. **Null on every run**: a one-byte flip in a copy of one raw and one compressed output must both be reported |
| `allow.txt` | one-sided files that are not drift, each with its reason |

    tools/gefidelity/parity/parity.sh                    # ~85 s, C side from ~/wt/gefidelity-run/pd.base
    tools/gefidelity/parity/parity.sh --bin build/pd.x86_64
    tools/gefidelity/parity/parity.sh --standalone       # C from this tree, no game build needed
    python3 tools/gefidelity/parity/compare.py A B       # two conversions already on disk

Run it after any converter change and before trusting `../census`, which
instruments the Python twin.
