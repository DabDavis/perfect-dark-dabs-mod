# Credits, and where everything in this project came from

Dab's Mod is a fork of the Perfect Dark PC port, which is a port of the Perfect
Dark decompilation, which is Rare's game. Almost nothing here starts from
nothing. This file is the complete account of whose work this is built on, what
is used from each, what is original to this fork, and how claims made here can
be checked — because a scene that borrows this heavily should be able to read
the ledger rather than argue about it.

Nothing on this list was taken quietly. If your work is here and credited
wrongly, credited too thinly, or you want it off, say so and it changes — see
[Corrections](#corrections) at the end.

## How this list is ordered

Ranked by one question: **how much of what a player actually runs would be
missing without it.** That weighs whole bodies of work that the executable
depends on above individual features, and it weighs work that ships inside the
binary above work that was only read as reference. It is a judgement, not a
measurement, and it is not a ranking of anyone's skill or of how hard the work
was.

---

## 1. Rare — *Perfect Dark* and *GoldenEye 007*

Everything else on this page is scaffolding around their two games. Every
texture, model, level, line of dialogue, animation, sound and piece of music
this program draws is Rare's, read out of a ROM at startup. The design being
extended here — the guards, the simulants, the weapon feel, forty levels of
level design — is theirs, and the fork's own features are judged by whether
they sit right beside it.

*Perfect Dark* and *GoldenEye 007* are © Rare. Perfect Dark is published by
Microsoft; GoldenEye 007 was published by Nintendo, with Eon/Danjaq/MGM's Bond
licence behind it. This project is not affiliated with, endorsed by, or
connected to any of them, and ships none of their data — see
[What we ship](#what-we-ship-and-what-we-dont).

## 2. Ryan Dwyer and the Perfect Dark decompilation

[n64decomp/perfect_dark](https://github.com/n64decomp/perfect_dark) — **6,684
commits** in the history this fork carries. This is not a dependency, it *is*
the source code: `src/game`, `src/lib`, the header tree, the struct
definitions, the matched function names. Every gameplay change in this fork is
an edit to code Ryan Dwyer and the decomp contributors recovered instruction by
instruction from the N64 binary, and the reason a feature like the simulant cap
or the body limit can be moved at all is that somebody already worked out what
the number meant.

MIT licensed; `LICENSE` in this repo is theirs, unchanged, and that is the
licence this fork carries.

## 3. fgsfds and the Perfect Dark PC port

[perfect-dark-pc-port/perfect_dark](https://github.com/perfect-dark-pc-port/perfect_dark)
— **38 contributors**, led by fgsfds (724 commits). The decomp is N64 code; the
port is what makes it a program you can run: the platform layer, the renderer
host, input, audio mixing, save files, the mod loader this fork's importer grew
out of, the netplay the fork inherits and does not touch, and the PAL/JPN and
Switch support this fork leaves to upstream.

By commit count after fgsfds and Ryan Dwyer: Raf (130), Catherine Reprobate
(42), Matthew Coppola (33), Jonaeru (23), Phnod (21), MaikelChan (21),
joshuarwood (21), AL2009man (19), Florian Piesche (17), Bobby Lovell (16),
tmyqlfpir (15), peerchemist, TartanSpartan, Richard M, Dominic Della Valle,
GreaseMonkey, Bronley Plumb, Al. Lopez, and the rest of the 38. Upstream's own
history is the authoritative list; this is a snapshot of it.

## 4. This fork — Dab's Mod

**1,801 commits and +248,618 lines** on top of that base (+287,524 counting the
vendored libraries below; 810 files touched), measured against merge-base
`c55f9c805`. What is original here, rather than borrowed:

- **GE Plus** — GoldenEye 007 converted out of its own ROM at runtime: 20 solo
  missions, the mission cinema, the watch as the pause menu, GoldenEye's HUD
  and radar, its per-level music, eleven gadgets, its tile-graph collision, its
  door and pickup sounds, the drivable tank, the monitor programmes. The
  converter is at version 59 and lives in `port/src/geconvert.c` and
  `tools/geconvert`.
- **The XBLA loaders** — STFS, LZX, `Textures.raw` and `PackedSegFile` readers
  (`port/src/x360.c`, `xblatex.c`, `xblamesh.c`, `xblaimport.c`), the mesh and
  pose path, the skies, the stage loader, the font fitter, cube-map
  reflections, and the Project Bean character path (`gebean.c`).
- **The mod importer** — reads console mods' *code*, not just their data, and
  writes out what it learned (`modimport.c`, `mod.c`, `moddata.c`).
- **The rest** — third person, jump and roll, the 80-simulant cap, texture and
  model packs, Community Packs, the ghost trials server (`tools/pdghostd`), the
  crash and F3 report pipeline, the updater, the recorder, Stage Loader, the
  head-fit rig, and the test harnesses in `tools/`.

Written by **DabDavis**, with **Claude (Anthropic)** doing the bulk of the
reverse engineering, the writing of the code and these notes, at his direction
and under his review. Both halves of that are stated plainly because a credits
file that hides how the work was made is not a transparent one. The research
behind each subsystem — including the wrong turns — is in `CLAUDE-notes/`, and
those notes are part of the deliverable, not scratch.

## 5. The GoldenEye X team

GE Plus no longer *comes from* GoldenEye X. It converts GoldenEye out of the
player's own ROM at startup - the twenty missions, the arenas, all eighty
characters and their animations, the music, the sound bank, the HUD and radar,
the watch, the gadgets and the monitor programmes - and a converted mission is
GoldenEye's own level whose guards are GoldenEye's own guards
(`gexplus.c`: "A mission dresses its chrs out of that table and nothing else").

What GoldenEye X still is, which is a great deal:

- **the models the HD characters are drawn on.** Every one of the 98 rows in
  `gebeantable.h` is a GoldenEye X model file. Without the mod there are no
  XBLA-quality characters at all.
- **about half the HD levels.** Of the 45 pairings in `gebeanstagetable.h`, 24
  are GoldenEye X backgrounds and 21 are the converter's own arenas.
- **what the arenas wear when it is installed** - its characters, its music
  and its guns, borrowed whole rather than converted. The reason the borrowing
  code exists at all is recorded at the top of `modborrow.c`: *"ge-x is a good
  reference honestly, they implemented the hand grips, everything correctly,
  even the reload animations."*
- **the oracle the whole conversion was checked against.** GoldenEye X keeps
  GoldenEye's N64 vertices byte for byte under Perfect Dark's file names, which
  is what made it possible to tell a conversion bug from a guess, and it is
  still the mission list GE Plus falls back to when it is the loaded mod.

Credits as the mod's own `ge-x_6a_credits.txt` gives them:

**Wreck** (props, weapons, characters, level files, textures, text, custom
arms, general editing, mod direction) · **SubDrag** (music porting, tools, ASM
hacks, all Bond heads) · **Carnivorous** (weapon animations, motorbike sounds)
· **Zoinkity** (technical information, Connery tuxedo, Labyrinth port) ·
**00Action** (Facility and Archives VR maps, clipping, Surface i/ii, Bunker and
Runway rooms) · **Sogun** (Citadel, Icicle Pyramid) · **Trevor** (GE-X logo,
Santa and Elf heads, Aztec moon, lattice glass) · **GERage** (Cliff Base) ·
**Monkeyface** (Connery head base) · **Dark Reyn** (weapon set format) · **Ryan
Dwyer** (custom actions, decomp information) · **ZMG88** and **Kode-Z** (music
tracks) · **MRKane** (death jingle) · **TH126** (video) ·
**Dragonsbrethren** (music porting) · **Octan Baron** (Caves AI paths) ·
**Jonaeru** (PC port experimentation).

And GE-X's own beta testers and reporters, listed here because they earned it
there: AL64inthedark, AstroBomber, Atari-Dude, AXDOOMER, Carnivorous, Catherine
Reprobate, connery as bond, Conquerallmusic, Dark Reyn, DF Ank1,
Dragonsbrethren, Fillerthefreak, freem, GoldenEyeCentral, Graslu00, HackBond /
Nyxem, Hagmax, Jonaeru, killedbyurmom, Kerr Avon, Lazlo52, OmegaCop13, Raf, S40
Games, SATURN_81, stlntxtrs, TH126, The Renagadist / Ren, Thundera8589, Trevor.

## 6. 4J Studios — the Xbox 360 releases

The XBLA releases of Perfect Dark (2010) and GoldenEye (Rare's 2007 "Project
Bean" build, 4J's follow-on work) are where this fork's high-resolution models,
textures, skies, fonts, explosions and HD levels come from. Their packaging and
formats — STFS, `Textures.raw`, `PackedSegFile`, Rare's CAFF
`07.08.06.0036` bundles — were taken apart from the release binaries to do it.
The player supplies the release; nothing from it is redistributed here.

## 7. The testers

Credited this high because the list of things they found is long and much of it
would still be broken. **164 F3 problem reports and 159 crash reports** have
come in, and they are the reason the Dam stair rail, the Facility vent crouch,
the hanging TVs in Bunker, the Frigate sky, the ladder-head simulant stall, the
Windows F3-send crash, the guard-spawn lag and dozens more were found at all —
usually with a screenshot, a trace and a description better than most bug
trackers get. **51 people** have signed into ghost trials on top of that.

**savantique** · **Paramania** · **Graslu00** · **Odeyseis** · **Myles De
Vries** · **Velvet Dark** · **dickalan** · **trevor** · **PerfectDark023** ·
**spider64** · **ElmoBear** · **Jerry R** · **Glazier** · **Linkmendez** ·
**and others not named**

That last entry is not a formality. The names above had to be asked for,
because reports used to arrive with no name attached at all: the F3 pipeline
sent a trace, a screenshot and your note, and the server logged an IP for rate
limiting and nothing else. Since 2026-09-22 the report dialog has a **Name
(Optional)** field, typed once and remembered, so a report can now say who to
credit - it is still optional, and still the only thing in a report that
identifies anyone.

Nobody above was identified from a report — they are the people who happened
to see the question and answer it, which means the list is certainly still
short of people who earned a place on it. **If you
have sent a report and are not on it, ask and you go on.** The same offer
stands for anyone who has reported a crash, tested a build on Windows, or run
the GE-X and GE Plus missions and written up what was wrong — and for anyone
here who wants their name spelled differently or taken off.

## 8. The GoldenEye 007 decompilation

[n64decomp/007](https://github.com/n64decomp/007), and
[kholdfuzion/goldeneye_src](https://github.com/kholdfuzion/goldeneye_src) and
the goldeneye_docs notes behind it. **kholdfuzion** and the GE decomp
contributors are the reference GE Plus is checked against: names, tables and
struct layouts, cited in the source where they are used —
`port/src/geexplosiontypes.h` is generated from the decomp's
`propExplosionDetailsRecords.inc.c`, and `gexfront.c`, `gewatch.c`,
`geintro.c`, `geguns.c`, `gexplus.c`, `gebean.c`, `moddata.c`,
`gemonitortable.h`, `geaitable.py` and `gemodelconv.py` all name it in comments.
`tools/xblaxex/tablediff.py` reads its structs out of the debug info of our own
port of that decomp, which is derived work kept in a separate tree and used
only as an oracle.

To be exact about a claim that has gone around: **no GoldenEye XBLA / "Bean" HD
texture or model code from any third party is in this repository.** That path
(`gebean.c`, `xblamesh.c`, `xblatex.c`, `x360.c`) was written here against the
release binaries. The debt to the GE decomp is real and is the one above:
names, tables and structs for GE Plus. Both statements can be checked with
`grep -ri kholdfuzion` and `git log` on those files.

## 9. The texture pack makers

Installed from inside the game through Community Packs, which downloads them
from their authors' own release pages and embeds their cover art in the binary
as the menu thumbnail (`port/src/communityart.c`):

- **Parabolee of Retro Foundry** — *PD Ultimate Plus HD* and *XBLA Plus HD*, and
  work on *PD Forever Plus HD*.
- **Howard Phillips** — *PD Forever Plus HD*, converted and completed by
  **Rafccq**, **Enigmata**, **Atari-Dude** and **Parabolee**.
- **Trov** — the fonts in all three packs, and the Perfect Dark texture pack
  that GoldenEye X ships for emulators (CC BY-NC-ND 3.0, used there with
  permission).

## 10. The mod authors

The importer exists to run other people's mods, and is tested against a corpus
of about fifty console patches. Named from the mods themselves: **Jonaeru**
(dataDyne Facility, G5 Car Park, dataDyne Warehouse), **MRKane** (Darknoon
Valley), **OmegaCop13** (G5 Base), **AC67**, **Punk7890** (PD Investigation),
**LZT**, and the authors of PD Plus, PD Kai Chaos, Total Darkness, Dark Corps,
Deep Sea X, Suburb, Spooky Dark, PD Kakariko, CSMP/CVMP, Mr X, PD Classics, the
Mario Characters mods, the weather mods, the JPN English patch, the 2XW-NR
patch and the All Solo Levels in Multiplayer patch. This list is short on names
because many of these patches do not carry one — corrections welcome, and
wanted.

## 11. Libraries and tools

Shipping inside the binary:

- **fast3d** (`port/fast3d`) — © 2020 **Emill**, **MaikelChan**, MIT. The N64
  graphics backend.
- **UnRAR** (`port/src/external/unrar`) — © **Alexander Roshal**, RARLAB, under
  its own licence, reproduced in [Licences](#licences) below.
- **LZMA SDK** (`port/src/external/lzma`) — **Igor Pavlov**, public domain.
- **SDL2**, **zlib** and optionally **libcurl** — linked, not vendored.

Used to build and to find things out:

- **Xenia** and **Xenia Canary** — the Xbox 360 emulator. A self-built canary
  with a draw log added is how the XBLA releases' own rendering was read.
- **ares** — the N64 emulator our headless oracle is built on.
- **GLideN64**, **Glide64** and **Rice Video** — their texture pack and
  high-res cache formats (`.htc`, `.dat`, the Rice naming) are what the pack
  loader reads, so packs made for emulators work here unchanged.
- **xdelta** (RFC 3284 and xdelta3's format) — how console mods are patched.
- GCC, Clang, mingw-w64, CMake, GDB, Xvfb and xdotool.

## 12. Friends of Joanna

**Catherine Reprobate**'s campaign mod and its monorepo
(`pd-fojo-monorepo-collab`), a fork of the same port. Read closely, not merged —
both trees rewrite `mod.c`, `romdata.c` and `fs.c` in incompatible directions,
so the exchange has been ideas rather than code. Their sixteen subsystem docs
are better written than most codebases' comments and several things here are
sharper for having read them.

Also read and not used: **SunJaycy**'s
[GoldenEye-Recomp](https://github.com/SunJaycy/GoldenEye-Recomp), and the
community GoldenEye function-label research bundle (which credits
`kholdfuzion/goldeneye_src` as its own corpus).

---

## What we ship, and what we don't

- **No game data, from anybody.** The executable contains no Perfect Dark or
  GoldenEye textures, models, levels, audio or text. It reads them from a ROM
  you supply, and from the Xbox 360 releases and mods you put in
  `added-content/` and `mods/`. None of that is distributed here, and none of
  it is obtainable from this project.
- **Third-party art that is shipped**, all of it small and all of it credited:
  the three texture-pack cover thumbnails in `port/src/communityart.c`
  (Parabolee / Retro Foundry and the Forever Plus HD team), embedded so the
  Community Packs menu can show a pack before you download it.
- **Downloads go to the authors' own release pages.** Community Packs fetches
  over HTTPS from the pack's GitHub release, checks it, and installs it. It
  does not rehost, and it does not remove anyone's licence or readme.
- **Format work is ours and is documented.** STFS, LZX, CAFF, `Textures.raw`,
  `PackedSegFile`, the GE ROM's setup and model records, xdelta, `.htc`/`.dat`
  — each was worked out here from the files themselves, and each is written up
  in `CLAUDE-notes/` with the dead ends included.

## How claims here are checked

Three independent oracles, kept separate on purpose so that a question is
answered by the thing that can actually answer it:

- **The ares headless oracle** (`oracle/oracle.cpp`, 1,418 lines, ours) boots
  the real N64 ROM with no window and no sound, plays a scripted controller
  sequence, screenshots it, and reads live game structures out of RDRAM by name
  and pointer rather than by value, so a check cannot be circular. This is what
  settles "what does the real game do".
- **Our port of the GoldenEye decompilation** dumps the real boot frame by
  frame as PPMs, for questions about exact output.
- **A self-built Xenia canary** logs the Xbox 360 releases' draws — shaders,
  texture fetches, constant blocks — for questions about how 4J's version
  rendered something.

Where a note in `CLAUDE-notes/` says something is confirmed, one of those three
confirmed it, and the note says which and how to re-run it. Where nothing
could confirm it, the note says that too.

## Privacy

F3 reports send a trace, a screenshot and the note you type. The server keeps
an IP address for rate limiting. Ghost trials store a username and a salted PIN
hash. No usernames are published anywhere, including in this file, and no
report is attributed to anyone without them asking for it.

## Licences

This fork is MIT, the same licence as the port and the decompilation, and the
copyright line in `LICENSE` is Ryan Dwyer's, unchanged.

**UnRAR** (`port/src/external/unrar`), reproduced as its licence requires:

> UnRAR source code may be used in any software to handle RAR archives without
> limitations free of charge, but cannot be used to develop RAR (WinRAR)
> compatible archiver and to re-create RAR compression algorithm, which is
> proprietary. Distribution of modified UnRAR source code in separate form or as
> a part of other software is permitted, provided that full text of this
> paragraph, starting from "UnRAR source code" words, is included in license, or
> in documentation if license is not available, and in source code comments of
> resulting package.

The full text is in `port/src/external/unrar/license.txt`. **fast3d** is MIT
(`port/fast3d/LICENSE.txt`). The **LZMA SDK** is public domain.

## Corrections

The ordering above is a judgement and the attributions are best effort from
what the files themselves say. If something here is wrong — a name missing, a
name spelled wrong, a mod credited to the wrong person, work of yours used in a
way you did not expect, or a rank you think is unfair — open an issue or say so
anywhere it will reach this repo, and it gets fixed. Removal of your work on
request, too, no argument.

Everyone on this page built on someone else on this page. That is the whole
arrangement, and it works as long as the ledger is kept honestly.
