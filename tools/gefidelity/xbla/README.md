# xbla/ - the HD (GoldenEye XBLA, "Bean") draw census

| tool | contract |
|---|---|
| `beanref.py` | reads a release model file's draw stream without the game's code: every draw, how it is reached (the game's path, a switch's later target, or none), its sections, piece, material pictures, triangles; `where()` gives a draw's height band in the model |
| `run.py` | census runs of the hook build (`census-hook.patch`, branch `feat/gefidelity-xbla`) in the HD look, one game at a time: the 20 missions, a 25-gun first-person pass under gdb, four arenas; plants one fault (GEBEAN_CENSUS_SKIP) in the first mission |
| `census.py` | joins the runs' `census:` lines with `beanref.py`: per file, draws the release draws that we never walk, walk but never build, or build short; pictures the game could not decode or painted over; a static pass for animated pictures the decoder refuses in every release file; files no run loaded. Null on every invocation: a struck draw and the planted fault must both come back, else exit 2 and no report |
| `accepted.json` | draws left out by design, each with its reason (muzzle-flash cards, a body file's own head, the N64-look originals' hand, screen panes under monitor programmes) |

```sh
# once: the hook build
git -C ~/perfect-dark/perfect_dark worktree add ~/wt/gefid-xbla -b feat/gefidelity-xbla dabs-mod
git -C ~/wt/gefid-xbla apply ~/wt/gefidelity/tools/gefidelity/xbla/census-hook.patch
cmake -G"Unix Makefiles" -B ~/wt/gefid-xbla/build ~/wt/gefid-xbla && cmake --build ~/wt/gefid-xbla/build -j2
cp ~/wt/gefid-xbla/build/pd.x86_64 ~/wt/gefidelity-run/xbla-run/pd.census
# the rig: data/pd.ntsc-final.z64, data/save/pd.ini (Mod: XblaMeshes/XblaStages/... =1,
# GeXblaCommunityEdition=0, MapMods=GoldenEye Arenas), added-content/{GoldenEye ROM, goldeneye -> Bean}
python3 run.py --out OUT                       # ~4 minutes
python3 census.py OUT/runs/*.log --plant $(cat OUT/plant.txt) --md OUT/report.md --json OUT/report.json
```

`--savedir save` is relative to `data/`. The HD level disk cache is emptied before
each run (a cached level is never walked). The hook is env-gated: without
GEBEAN_CENSUS the build draws exactly as dabs-mod.

What it does not see: a state that only exists at run time (a destroyed prop's
crumple, a toggle the game sets mid-mission), how a drawn piece looks (placement,
material, colour - the view diff's job), and the release's own look (Bean in
Xenia; no paired oracle yet). Pool characters and heads (Combat Simulator,
Customize Character) are not loaded by these runs: `--mpsims` brings
Perfect Dark's own simulants.
