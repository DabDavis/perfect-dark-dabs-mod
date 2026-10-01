# xbla/ - the HD (GoldenEye XBLA, "Bean") draw census, three ways

Every draw a release model file holds, classed three ways:

1. **the file** (`beanref.py`): what the file's stream contains, read without the game's code;
2. **the release** (`xdraws.py`, `capture.py`): what Bean actually draws, running in Xenia with the
   draw-log hook (`tools/xblaintro/xenia-drawlog.patch`, PDD2/PDD3 records with fetch constants);
3. **ours** (`run.py`, `census.py`): what our HD look builds, from a build with the census hook
   (`feat/gefidelity-xbla` e80453245, the same as `census-hook.patch`; env-gated by `GEBEAN_CENSUS`).

| tool | contract |
|---|---|
| `beanref.py` | a release model file's draws: how each is reached, sections, piece, material pictures, triangles; `where()` its height band |
| `xdraws.py index` | every release file's draw signatures: Xenos primitive, index count, vertex buffer (size, offset), the material's pictures (w, h, format) |
| `xdraws.py match LOG IDX OUT --first F --last L --expect FILE` | the release's draws in a draw log matched to files: a logged draw must have the primitive and count, a vertex fetch as long as the buffer at a page-aligned base + the buffer's offset, and the pictures; a file's draws in a frame must share one base. Null: `--expect` (a file the capture certainly draws) must come out drawn and a planted fake file (two of its draws at bases no one page explains) must not; exit 2 otherwise |
| `xdraws.py merge REF NAME=drawn.json ...` | the release reference (`release-drawn.json`): per file, the draws seen, frames, captures |
| `capture.py --mission M --expect FILE` | one capture: Bean on the fidelity rig (`../xenia/rig.sh`, machine-wide lock), into the mission by `common/xeniage.py`, a turn, bursts of fire, BACK; then `xdraws.py match` on the mission's frames and the log deleted |
| `run.py` | our census runs (one run directory at a time, flock): 20 missions (the first plants a fault), a 25-gun first-person pass, a pool pass (every Combat Simulator character and head through the game's loader), four arenas |
| `census.py` | the three-way join: per file, draws ours never walks / walks but never builds / builds short, each with what the release does; pictures the game cannot decode; files no run loads. `--findings` writes the gate's report.json. Null: a struck draw and the planted fault must come back, else exit 2 |
| `sweep.py --bin ./pd.census --out DIR` | the gate's hd-census leg end to end: run.py, census.py, and the repeatability null (the first mission twice, same findings keys) |
| `accepted.json` | by-design drops with their reasons; used only where no capture shows the file (`hd.accepted.unconfirmed`); where one does, the release decides (`hd.accepted.release` when it never draws them either, `hd.undrawn` when it does) |

## The gate's findings (`DIR/report.json`, world/compare.py's shape)

`mission` = the release file (`new/char/greatguard2`), `key` = the draw's place in its stream (hex) or
`texN` for a picture or `-` for a whole file, `mag` = triangles not drawn (1 for a picture). Kinds:

- `hd.undrawn` / `hd.short` - a draw ours never draws (or draws short) that the release draws, or that
  no capture shows and nothing explains
- `hd.replaced` - a draw the release makes that ours deliberately replaces (an accepted.json entry
  with `replaced`) - listed and gated like a gap, since it is a choice to revisit. None is left: the
  muzzle-flash cards, the screen panes and the watch arm's glass were taken out of accepted.json on
  2026-10-01 (the user's H4 decision: the HD look draws them as the release does), so any of them ours
  does not build is an `hd.undrawn` like any other gap
- `hd.extra` - a draw ours builds that the release, drawing the same file, never makes (not for level
  files: a capture sees only the rooms in view)
- `hd.nodecode` - a picture our decoder refuses
- `hd.unloaded` - a file a mission capture shows that no census run of ours loads
- `hd.accepted.release`, `hd.accepted.unconfirmed`, `hd.accepted.placeholder` - behaviour, never failing

## Gating a change

The census needs the hook in the binary under test. In a scratch worktree of the change:

    git cherry-pick -n e80453245          # or: git apply tools/gefidelity/xbla/census-hook.patch
    cmake -G"Unix Makefiles" -Bbuild . && cmake --build build -j2
    cp build/pd.x86_64 ~/wt/gefidelity-run/xbla-run/pd.<name>
    python3 tools/gefidelity/xbla/sweep.py --bin ./pd.<name> --out DIR

Base and test both with the hook; without `GEBEAN_CENSUS` the hook draws exactly as the change does.
The release side (`release-drawn.json`) is captured once (`capture.py`, `xdraws.py merge`) and not
per sweep: the release does not change.

## The Community Edition, both sides

GE Plus's HD look runs with the CE, so both sides do: ours with `Mod.GeXblaCommunityEdition=1` and
`added-content/CommunityEditionUpdaterV6.zip` (the log's `path` is the CE's copy where it replaces a
file, and census.py reads that), the release as `../xenia/rig.sh`'s default `GF_BEAN=ce` build, indexed
from its own files: `GF_BEAN_FILES=~/perfect-dark/.xbla-work/ge-bean/BeanCE/filesCE xdraws.py index ...`.

## The rig (`~/wt/gefidelity-run/xbla-run`)

`data/pd.ntsc-final.z64`, `data/save/pd.ini` (`[Mod]` XblaMeshes/XblaMeshTextures/XblaStages/XblaSkies
=1, GeXblaCommunityEdition=1, MapMods=GoldenEye Arenas; `--savedir save` is relative to `data/`),
`added-content/` (the GoldenEye ROM, `goldeneye` -> the release, CommunityEditionUpdaterV6.zip),
`mods/` (converted on first start). run.py takes the directory's lock (`.census.lock`).
The HD level disk cache is emptied before each run. Never a player's install.

What it does not see: run-time states (a destroyed prop's crumple), how a drawn piece looks (the view
diff's job), and anything no capture or run shows (README's coverage lists in report.md).
