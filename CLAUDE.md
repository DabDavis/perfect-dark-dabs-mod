# Perfect Dark port — simulant/mod fork

Fork of the [fgsfdsfgs/perfect_dark](https://github.com/fgsfdsfgs/perfect_dark) port.
`port` tracks upstream; work happens on `dabs-mod`.

`git checkout port` returns to stock at any time.

## What is written down here

Each note in `CLAUDE-notes/` is a thing that was got wrong once. Read the note
for an area **before** touching it — none of them are inferable from the code,
and each one cost a detour. This file keeps only what every session needs; the
notes are read when their area comes up.

- **The Windows build, wine, the pd.ini format** — [windows-build.md](CLAUDE-notes/windows-build.md): mingw prefix, WinHTTP, and why `Mod.LoadTextures=1` on its own line does nothing
- **chrs, bodies, heads, simulants, memory pools, mpconfig** — [chrs-and-memory.md](CLAUDE-notes/chrs-and-memory.md): a chr's prop is read before its tick; the ~50KB head copy that empties the stage pool; one head modeldef cannot sit on two bodies; ROM-resident structures never grow; why kept bodies still vanished (the chr vertex store's reaper, sized for an N64, fades every off-screen corpse when the blood copies run out)
- **Saves, eeprom, where pd.ini lives, the migration** — [save-format.md](CLAUDE-notes/save-format.md)
- **Menu text, `textMeasure()`, reaching the widescreen pillars** — [text-rendering.md](CLAUDE-notes/text-rendering.md)
- **Adding stages** — [stage-numbers.md](CLAUDE-notes/stage-numbers.md): `STAGE_IS_LEVEL()` admits 0x5e-0xff as well as the 27 free below the title; four ids are taken outside the table; the MP save format holds 7 bits
- **The Stage Loader: every mod's maps as arenas beside the mod loaded** — mods.md, "The Stage Loader": maps-only mounts never overlay; the registrar rescans on a swap; the branch's fixes (allocation, pool checks, room sizing, textures by stage); the importer splits a rebuilt texture table (30) and writes the `maps` block (31) so a mod's arenas come from its own tables under its own names — a file name is Perfect Dark's slot, not the map (GE-X's `crad` is Aztec)
- **Screenshots, the recorder, ffmpeg, GL capture** — [recording.md](CLAUDE-notes/recording.md): the frame is presented before `videoEndFrame()`; NV12 on the GPU; encoder detection; why it must never wait for the encoder; running on the real GPU with no window (llvmpipe hides driver limits); **the F3 trace dump** (`Mod.TraceKey`, port/src/trace.c): a text dump of the pools, the loaders, the renderer's cache, the camera and every chr with what chrRender() did with it, paired with a screenshot - what to ask a tester to press when something is missing from a frame
- **Ghost Trials networking** — [ghost-trials.md](CLAUDE-notes/ghost-trials.md): WinHTTP and libcurl, why not one of them, and what the worker thread may touch; the three security questions' wire format and `rec_count`; the red Streamer Beware door and the Offline/Online page; **the pd.ini key is `Mod.GhostServer`** - point a scratch ini at a local daemon before any headless drive that presses Create Account
- **Check for Updates** — [updater.md](CLAUDE-notes/updater.md): `update.txt`, the baked-in channel, the two-rename swap
- **Mod directories, Load Mods, modconfig, `modcodediff`, the ROM symbol file, the data segment and importing a mod's weapon definitions** — [mods.md](CLAUDE-notes/mods.md): only the first mod dir joins the file search; files swap live, segments cannot; the `datasegment` block, `moddata.c`, and "where this stands" for continuing the import work
- **Texture packs** — [texture-packs.md](CLAUDE-notes/texture-packs.md): where a pack goes (one of four directories is read by nothing); decoding off the render thread and the backlog; the kept store and why a decode must never be handed over (textures flicker to the original otherwise); an emulator pack's image is the tile, the renderer maps the padded row; the *other* stretch is the game's own - a tile sampled past its edge, which no image edit can fix and **Stretched Edges** (`Video.StretchedEdges`: Original/Mirror/Repeat) can; PNG is ours, JPEG is stb_image, and the two row orders; font glyphs (the image is the whole tile, the outline pass wants both images); F7–F10; a folder named **`xbla`** inside a pack replaces the XBLA release's own texture records, by record rather than by number, on its models *and* its rooms (F7 writes them out in the same layout, which is the only way to learn which record a surface is); and **Community Packs**, which installs a pack from its author's release page - what is in the binary is the pack and not the release, the row-order marker is written at install time because v0.09 of the PD Plus pack renamed the `ext_tex` folder that used to say so, and the cover art is drawn through a stand-in tile the way an XBLA mesh's texture is (and is *not* turned over)
- **The Xbox 360 XBLA release: its textures, and drawing its models** — [xbla.md](CLAUDE-notes/xbla.md): the player's copy goes in `xbla/` the way a mod goes in `mods/` — the archive, the package, or a folder holding either, two deep, packages preferred — and a `.7z` is unpacked once into `cache/xbla/` (never into `xbla/` itself, which is the player's) by whichever of the conversion and the mesh loader wants it first (three seconds, 250MB, `xblaImportGetStfsPath()` blocks and `xblaImportIsAvailable()` does not); the STFS volume descriptor is one byte from a 257-block file table of garbage; LZX is a 17 bit window and a chunk is not a stream; Textures.raw is two 52-byte tables and the second is a D3DTexture, which is where the format and the tiling come from; pitch aligns to 32 texels linear and 128 block; record index *is* the texture number for the first NUM_TEXTURES and half of them still do not match stock because 4J redrew them; PackedSegFile slot *i* is file id *i+1* and its plain files are stored already inflated; the geometry is 596 files in 4J's own format, named by a mesh id in `modelnode`'s padding whose low 12 bits are a file id and so **one more than the slot** (reading it as the slot gives every model the mesh next door, which looks nearly right), drawn by `port/src/xblamesh.c` as a Perfect Dark display list — 1:1 in the model's own coordinates, 25 vertices to a batch, a list per group so the node carrying part p draws group p (and a piece the game toggles off stays off), all of them under the first part's matrix, and the node's own `G_MTX` copied or it lands nowhere; two menu checkboxes on the *Xbox 360 (XBLA)* page, meshes and textures separately, and a texture pack goes on underneath both and never has to come off (a mesh's records are past the numbered ones, so the two cannot collide); **Enable Textures is the whole of the release's art since 2026-09-11** - a numbered texture is served its record out of the package through the pack's own branch of `gfx_pc.cpp`, pixel-identical to the converted pack and behind any pack the player selected, so nothing has to be converted to see it (and the page's "Enlarged Textures Only" is gone, the button reading "Make Into Texture Pack"); both switches are live - the textures one because it is read where a picture is handed to the renderer rather than where a list is built, the meshes one because every model is matched as it loads whether or not it is on (a list of loaded modeldefs to go back over instead **crashes**: one can be freed inside a stage and its memory reused), which costs nothing measurable and never unpacks an archive (the empty "no package ready" answer is remembered, or every model load re-decides it - 57 opens of the player's archive over one level load - while the unpacking call still ignores it - and the level the switch was flipped in keeps its stock models, which is the one case on the page where a checkbox does nothing visible and the one line under it that is ever shown says so); the release's own **48 frame explosion** goes in the ROM's place one picture at a time (`port/src/xblaexpl.c`, `Mod.XblaExplosions`, "Enable Explosions"): 4J never upscaled the ROM's explosion records because their renderer drew its own animation at 0e1f-0e4e, the ROM's colour ramp **stays** on tile 1 (its alpha averages two fifths and replacing it with white made the fireball half again as strong as the game's), frame 0 stays the ROM's blank one, and two explosion runs a second apart in wall time are not comparable - judge it frame-exactly under `--fixed-step` and gdb or not at all; the release's **interface art** is catalogued in xbla.md ("The interface art") and its **logo** is the XBLA page's banner (`port/src/xblaui.c`, through `menuimage.c`, which can now name a loader instead of a PNG in the binary) - where the row order caught us out again, since the font atlases are stored top row first and the logo bottom row first; **the release's font is served the same way since 2026-09-11** (`port/src/xblafont.c`, `Mod.XblaFont`, "Enable Font"): seven atlases in records 0db7-0dbe with their cells in `DataFiles/*.abc`, one per font file and paired with the record by the size of the picture (two are 1024x670 and only the subject tells those apart), fitted into the box the ROM glyph's own *body* texels fill rather than by 4J's metrics - so the ROM's widths, baselines and kerning still lay the text out and nothing reflows - and the outline pass's border is built here because the shader measures its half texel in texels of what was uploaded; a model drops whatever was registered against its own address as it loads, because a modeldef is freed and reused inside a stage and the draw path's definition test cannot tell that from a live one, which leaves `lvReset()`'s drop as the backstop for models never loaded again; textured by `port/src/xblatex.c` from the release's own records 3741-5746, which no pack can ship *by number* (an `xbla` folder in one replaces them by record, through texpack's queue and store, and the release's art is drawn until that decodes), through a 32x32 stand-in tile whose *address* names the record (the trick `texpackLoadReplacement` plays) and whose size is what a Vtx's 10.5 s and t can hold; the picture's rows are **not** flipped (a pack proves the decode order is the game's own) but the mesh's **`v` is**, D3D counting rows from the end the game's `t` counts away from — and a mirrored atlas does not look upside down, it looks like art that is simply wrong (sleeves across a chest, a waistband round the hips); skinned by `Mod.XblaMeshPose`, which is on: palette entry *i* is matrix *i* of the model, the palette holds each bone's **inverse** bind with a translation in its last column (not a bone position), and the header's float at +0x18 is the mesh's scale times 100 — a tenth for nearly every skinned mesh; heads are `Chead*Z` models grafted onto the body at its `HEADSPOT`, so they reach the hook with the body's model and the head's modeldef and are let through by walking the parents to the root through a headspot, and Joanna's three restructured heads pair by vertex count instead of node for node; a head's **stock hair** is a toggled node of its own that the release leaves at id 0 while marking a toggled piece it keeps with `0xFFFF` (54 of those, every one a gun's or a console's, against 232 zeros of which 166 are a head's), so a zero under a head's hat toggle draws nothing - decided by the head skeleton, not by the graft, since the Character page zooms on a head loaded as a model of its own; **a plain zero is the mesh's geometry, not the node's** — the id is written on one node of a model and the mesh named there is the whole model (`CcarringtonZ` is one named list of thirty, and its mesh is 4792 vertices over a fifteen-bone palette), so reading a zero as "keeps what it has" drew the N64 body inside every character and the stock sofa inside the Institute's, which is what "both models loaded together, intermingled" was; a zero now draws nothing (946 lists) unless it is a far LOD alternative (864) or a toggled piece (125: muzzle flashes, the six heads' glasses), and nothing is suppressed unless the mesh itself is named on a list the game draws beside them (`CheadgreyZ` names its only mesh on a toggled alternative), which is also why the node table is 16384 entries now; **a mod's model is left alone** — the release's package is keyed on the game's own file ids and a mod keeps the id while replacing the contents, so GE-X's file 447 was paired by size against `CbiotechZ`'s mesh, one list of twenty-five took the whole of it and the other twenty-four kept drawing through it, which is what "both models on screen at once and the hair floating above the head" is (`romdataFileIsStock()`, plus the pairing by size having to account for every list it did not take — only a far LOD alternative may be left over); `--xbla-mesh-verbose` answers "are two models on top of each other" directly now, one `OVERLAP` line per (model, slot, reason); a matrix handed over as floats (`G_MTX_FLOATS`) also skips the **stage scale** `mtxF2L()` folds into the first three columns of everything else (`scale_bg2gfx`, a half on Villa, Crash Site and Air Base), which put every posed mesh at twice its true depth on those three - the right picture, because the scale is about the eye, and behind the floor it stands on in the z buffer, which is what "transparency issues with every xbla texture" was (`mtxApplyGfxScale()`); the pose arena is **a list of chunks** rather than one block a side, because a block can only be grown between frames and the frame that first wanted more drew a head a body's height above its body (one chunk a side for eight simulants, six for eighty); and four faults that were in here — `Mod.XblaMeshes=1` in pd.ini with the release still in its `.7z` drew nothing for ever because a model load never unpacks and the checkbox that would have was already ticked, the node registry never reused a tombstone and filled up over a match, the pose arena stopped growing at the frame that wanted more than the cap, and three bounds were a word short on a slot that a mesh id names but is not a mesh
- **Model packs and the asset dump** — [model-packs.md](CLAUDE-notes/model-packs.md): `model-packs/<pack>/n64/` and `xbla/` hold OBJ replacements named by the ROM's file names, read through `objmesh.c` and drawn by the XBLA mesh loader (an OBJ is written back out in 4J's own layout and goes through the same builder); the one **Dump All Assets To Disk** row (`assetdump.c`, `--dump-assets`) writes `texture-dumps/<romid>/` (+`xbla/`) and `model-dumps/n64/` and `xbla/`; an N64 model's list pointers are *not* promoted with the rest and stay segment 5 until the texture rewrite; a group of an N64 model is a list node in the node's own space and the dump adds the rest offset the loader takes off; skinning does not survive OBJ and is transferred by nearest vertex; and everything about a pack is **live** since 2026-09-11 - both halves of a node (the release's mesh and the pack's file) are filed as every model loads, whichever draws is decided at the draw and settled by **Mod.ModelPackPrefer** where a model has both, F9 re-reads an edited OBJ, and a material named for one of the ROM's textures binds the *ROM's* picture plus its number so a texture pack repaints it (the dump's `map_Kd` points out of the pack and is a reference, not the author's picture - reading it as one is what stopped that)
- **The Friends of Joanna collab tree, `../pd-fojo-monorepo-collab/`** — [fojo-collab.md](CLAUDE-notes/fojo-collab.md): what their mod loader does that ours does not, why the trees cannot merge, and what is worth borrowing
- **Measuring a crowded match, `--rng-seed`/`--fixed-step`/`--exit-frame`, where the frame goes** — [performance.md](CLAUDE-notes/performance.md): compare instructions per frame on a seeded fixed-step match; the renderer is 60% of the main thread and is built at -O2; why the decomp at -O2 played a different game (game-defined sinf/cosf, an uninitialised pad flag) and how a divergence is bisected; run to a level frame rather than a wall-clock span, and check a renderer change with a pixel diff; why Model LOD does nothing
- **The third person camera, and why melee, rockets and beams came out of it** — [third-person.md](CLAUDE-notes/third-person.md): the player's shot is fired from the camera and not the eye, so the crosshair is honest at any offset but everything that measures *from* the origin was two metres out; the two halves of the fix and the melee site that does not go through the shot path; the floor flags the camera trace needs; driving the camera from gdb; **Camera Tether** (`Mod.ThirdPersonTether`), a rod that pivots about `bond2.unk10` and not the shaken copy, and the game's `atan2f()` answering in 0 to tau; light glares are depth-less sprites, drawn before the gun and occluded by the body through `shotTestLos()`
- **Defaults and the Settings Preset** — `src/game/modoptions.c` and `g_ModPresets` in `port/src/optionsmenu.c`: since 2026-09-09 a fresh pd.ini is stock Perfect Dark with the fixes on (Smooth Text, Clean Text Outlines) and the additions off, because a tester met a game that jumped, rolled, tilted and flew (F was the Spectator); the fork's old defaults are the "Dab's Settings" preset, and a preset is applied on selection and remembered by nothing - the dropdown reads Custom once any covered setting differs. The third person camera has its own preset and is not covered; a pd.ini written by an older build keeps every value it has, including F
- **Weapon numbers, `flags2`, converting a `weaponnum` comparison** — [weapons.md](CLAUDE-notes/weapons.md): the four checks, and what is deliberately not converted; a launcher branch keyed on the number must test the function's type, or a mod's table crashes it
- **The Randomizer: a mission dealt again from its own pieces** — [randomizer.md](CLAUDE-notes/randomizer.md): the roll goes between `setupLoadFiles()`'s writable copy and `setupCreateProps()`'s walk; `chrGetPadRoom()` hands a pad number back as a room; the intro stream's commands are 8, 12, 16, 32 and 40 bytes and have no length function; rewriting the spawn pad does not move a mission that opens with a cutscene; a tagged object survives being picked up, which is what makes a generated collect objective finishable; each decision draws from its own stream so a change to one part cannot move the others, and the log's fold is how that is checked; Endless Mode keeps dealing objectives and scores rooms covered; the Carrington Institute is a level and must be excluded from anything that runs on one; Chicago (0x1d) is the test bed and Villa never reaches gameplay under `--boot-stage`
- **The Randomizer's run: a room at a time across every map** — [randomizer-run.md](CLAUDE-notes/randomizer-run.md): every door out of the landing room is a portal and a portal is a whole stage load (the co-operative Deep Sea chain is the pattern, and `mainChangeToStage()` takes effect at the *end* of the frame); a mission's opening cutscene held the first landing off for 45 seconds; the kit goes back through the spawn (inventory inside `playerStartNewLife()`, hands from `playerSpawnWeapons()`, health after `playerSpawn()`) and never from the tick, which left a hand drawing a model nothing had loaded; the door test is "not in `prop->rooms[]`", not "`rooms[0]` changed"; a death must not end the stage; no objective type fits one room, so the run owns the top bit of `g_StageFlags`; the room is sealed until that objective is done and the doorway is a wall from the inside, and what is sealed is the landing room **plus the rooms touching it** (`modRunBuildZone()`, one door deep, built at the landing and dropped back to the one room when it would swallow a whole small map) since a single room is sometimes a stairwell (a refused move must leave a collision behind it, or the push reads the last real one and a door damages the player through a wall that is not there), the barrier's test is the portal's own so the two cannot disagree, and a room sealed too long is dealt a clock; landings are waypoints in rooms that have a portal; guards are modalarm.c with the body overridden, and a body drawn from the whole game has to be spawned the way a setup spawns one (`bodyInitSpecialChr()`: the robot's fireslots, or the beam render dereferences them); `--random-run` and `--run-autohop N` drive it headlessly
- **A rule or colour a mod's code changes that is not a weapon's** — mods.md, "The tail": `game/modrules.h` holds it with the stock default, a modconfig block sets it through one setter in mod.c, both importers read it; the branch `lua-pipeline` is the Lua experiment of 2026-09-07, kept and not merged

**[DabDavisGitHub.md](DabDavisGitHub.md)** is the companion to this file: the
GitHub remote, how commits are written, how a push becomes a release, and how the
stable and dev channels reach a player. Read it before pushing or tagging —
`dabs-mod` is a public default branch and a push to it rebuilds what every
dev-channel player's Check for Updates points at. A push that changes only
`CLAUDE.md`, `CLAUDE-notes/` or `DabDavisGitHub.md` does not build.

When a session gets something wrong that the code could not have told it, write
it down: a new note, or a section in the one for its area, and a line here.

## GE-X import: where to pick up (2026-09-05)

The console mod GE-X 6a is the reference case for the mod loader. Its assets,
data tables, missions, music, environments, star field, weather, shield
colours, hit sounds, co-op buddies, the weapon lists behind eighteen flags, two damage rules and the unlocks all import (`build/mods/GE-X_6a_01-19-25/`, importer version 23); what is left is the code
GE-X *rewrote*, which `modcodediff` lists and nothing follows yet. Read
[mods.md](CLAUDE-notes/mods.md) from "GE-X's solo missions in the port" to the
end before touching any of it, then:

1. **Regenerate the list** — the summary is the work queue, largest first:
   ```sh
   python3 tools/modcodediff --rom ../pd-upstream/pd.ntsc-final.z64 \
       --patch build/mods/GE-X_6a_01-19-25/GE-X_6a_01-19-25.xdelta --summary
   ```
   `constants` regions are tables to follow (most are done); `rewritten` ones
   need reading. Every rewritten function of 9 words or more is read, and
   the tail's unlocks family too; what remains of the tail is listed in
   mods.md ("The tail, and the unlocks": the menu palette, the run-speed
   cave, King of the Hill, GE-X's hats ...), and the weapon-number sites
   the port still tests literally (weapons.md: ~60 in bondgun.c, ~63 in
   propobj.c), each a `FLAG_SITES` row once converted. GE-X's own
   weapon-number regions are all read as of importer 27 (mods.md, the
   four "number sites" sections; the second says why a row also needs a
   `codeSyms[]` entry in modimport.c), and five of its tail as of 29
   (mods.md, "The tail": settings in `game/modrules.h`, and what is left).
   `--prepare-diff DIR` writes both binaries; `mips-linux-gnu-objdump -b binary
   -m mips:4300 -EB -D --adjust-vma=0x7f000000` reads them.
2. **Two ways to follow a change.** A renumbered compare is a
   `follow_immediate(s)` site (the `playerconst`/`bgstage` pattern, one table
   row in both importers). A rewritten function is run on the toy MIPS
   (`emulate()` in `tools/importmod`, `emuRunArgs()` in `port/src/modimport.c`,
   the weather is the worked example, the shield colour the one for a
   function that tests its caller, the hit sounds the one for a list of
   weapon numbers: seed the tables, run per number, read what it stored)
   and written out as the port's own config. A weapon-number test in the
   port becomes a flag (weapons.md) and the importer writes `weaponflags`;
   arguments to a call are followed by call and register
   (`follow_call_args()`, the co-op buddies), since one function can hold
   four number spaces; a weapon test that became a list is read as a
   compare chain (`follow_compare_chain()`, `FLAG_SITES`), and a flag is
   written only when every site of it agrees. Across the archive the most rewritten functions are
   `player_tick` (38 mods, 27 of them a no-op word - see mods.md), then a
   block of 23 that every "all solos in multi" patch shares (`tex_init`,
   `setup_create_props`, `mp_start_match`, the unlock handlers);
   `modcodediff --summary` over every patch takes four minutes with `xargs
   -P 8` and is how to know whether a function is one mod's or the
   archive's. Whatever is added goes in **both importers**, `IMPORT.txt`
   lines identical, and bumps `MODIMPORT_VERSION` so old imports redo
   themselves.
3. **Test headlessly** on Runway (0x22) or later — Dam (0x30) and Facility
   (0x33) are unfinished in the patch and prove nothing:
   ```sh
   cd build && timeout -k 5 60 xvfb-run -a ./pd.x86_64 --moddir mods/GE-X_6a_01-19-25 \
       --savedir /tmp/pdsave --skip-intro --no-sound --boot-stage 0x22 --log
   ```
   Exit 124 is "ran until stopped"; 137 is a crash or hang, read the log.
   `--moddata-trace`, `--chr-trace`, `--setup-trace` and the
   `weather:`/`music:` log lines say what applied. `--boot-stage` takes the
   stage id, not the menu slot: GE-X's `g_SoloStages` remaps six missions.
4. **Open tester reports**: no guards on Dam (GE-X spawns them by AI as the
   player advances; a headless run cannot exercise it — a person must). Check
   the tester's binary commit before debugging a report from
   `sdg@10.8.0.3:~/pd-test/` (`strings pd.x86_64-linux | grep -m1 -E
   '^[0-9a-f]{7}$'`).
5. **Known gaps, deliberate**: stages a mod took weather away from keep the
   port's stock weather entry (inert without a rain/snow script command); the
   two importers order the `playerconst`/`bgstage`/`roomstage`/`buddyconst`
   lines differently (content identical); mod-only stage ids (GE-X's 0x4d–0x50)
   are skipped by `stage {}` blocks with a warning.

## Build and run

```sh
cmake -G"Unix Makefiles" -Bbuild .     # only after adding/removing source files
cmake --build build -j8
./build/pd.x86_64                      # needs build/data/pd.ntsc-final.z64
./build/pd-modded.sh                   # with the All in One mod
```

Release builds use `-Og` for the decomp except the hot files (`PD_HOT_O2`) — see the comment in `CMakeLists.txt` and [performance.md](CLAUDE-notes/performance.md). Warnings
about uninitialised locals in `collision.c`, `model.c`, `menu.c` and `mplayer/setup.c`
are pre-existing decomp artifacts; check `git diff` before assuming one is yours.

`build/` is gitignored, including the mod directories and `pd-modded.sh`. Keep the
mod zip: a clean rebuild takes them with it.

The Windows cross-build is in [CLAUDE-notes/windows-build.md](CLAUDE-notes/windows-build.md).

## This is a decompilation

Much of `src/` is decompiled N64 code. Two consequences that matter:

**Structs with offset comments** (`/*0x1be7*/`) document the original ROM layout.
Growing them breaks matching builds. Ours sets `MATCHING=0`, but treat it as a real
cost when considering upstreaming.

**Some code is unreachable by construction on N64** and becomes reachable the moment
a limit is raised. Every bug in this fork's history was of that shape: a fixed-size
table or an assumption nobody wrote down. `chrInit()` dereferenced NULL when the chr
pool was exhausted; `mpCreateBotFromProfile()` looped forever once chrs outnumbered
the 53 available heads; `langGetLangBankIndexFromStagenum()` had `default: while(true){}`.
When raising any cap, grep for fixed-size arrays indexed by the thing you are scaling.

## Debugging

Guessing from source failed repeatedly here; the stack was right every time.

```sh
# crash: symbolise the offsets the game prints
addr2line -f -C -e build/pd.x86_64 0x11abf5

# hang: main thread only, Mesa worker threads are noise
gdb -p $(pgrep -x pd.x86_64) -batch -ex "thread 1" -ex "bt 14"
```

**A Windows crash dialog** gives `PC` and `MAIN MODULE: [base]`, and a
backtrace of `[base]+offset` lines. The offset is from the image base, which
the release exe has at `0x140000000`, and the exe keeps its DWARF (30 MB for a
reason), so:

```sh
gh release download v3.1.2 -p pd.x86_64-windows.exe -O pd-v3.1.2.exe
x86_64-w64-mingw32-addr2line -f -C -i -e pd-v3.1.2.exe 0x1401ba7a3   # 0x140000000 + offset
```

Plain `addr2line` says `??` for every line. When the report does not say
which build, try each stable exe: the right one symbolises to a stack that
makes sense as a call chain, the wrong ones to functions that could never
have called each other. The reporter's data is not ours: a crash in a mod's
model that no stage here reproduces is a model their import has and ours does
not, and the question to ask them is which mission and what the first line of
their `mods/<mod>/IMPORT.txt` says.

stdout is block-buffered when redirected, so log lines sit unwritten. Flush a live
process before reading or killing it — `SIGKILL` discards the buffer:

```sh
gdb -p $(pgrep -x pd.x86_64) -batch -ex 'call (int)fflush(0)'
```

A hung process ignores `SIGTERM`, because the shutdown handler cannot run.

For "state disappears" bugs, instrument every mutating path rather than reading code.
The simulant-clearing hunt was solved by logging that showed a count of zero at every
suspected site — nothing was being cleared; nothing had ever been created.
