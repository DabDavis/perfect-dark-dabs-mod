# Dab's Mod

**[Download](https://github.com/DabDavis/perfect-dark-dabs-mod/releases/latest)**
— Windows, Linux and macOS. You supply the ROM; see [You need a ROM](#you-need-a-rom).
There is also a [rolling dev build](https://github.com/DabDavis/perfect-dark-dabs-mod/releases/tag/dabs-mod-dev)
of the newest commit, if you want fixes before they reach a release.

A fork of the [Perfect Dark PC port](https://github.com/perfect-dark-pc-port/perfect_dark),
which is itself a port of the [Perfect Dark decompilation](https://github.com/n64decomp/perfect_dark).

It adds movement the N64 game never had — jump, a combat roll, melee combos, a
third person camera — raises the Combat Simulator simulant cap from 8 to 80,
keeps bodies lying where they fell, and gives you a free-flying spectator
camera, a screenshot key and a video recorder for watching the results.

Everything here is engine-side and lives in the executable. No custom levels or
assets are bundled, and none are needed - but mods drop in, see [Mods](#mods).

## You need a ROM

This is an executable, not a game. It reads every texture, model, level and line
of dialogue out of a Perfect Dark ROM at startup, and ships with none of that.

You need `Perfect Dark (USA) (Rev 1)`, also called `ntsc-final` or `US V1.1` —
md5 `e03b088b6ac9e0080440efed07c1e40f`, and the one the boot screen calls
`NTSC version 8.7 final`. Dumping it from your own cartridge is on you; don't
ask here.

PAL and JPN ROMs are not supported by this fork. The stock port supports them if
you need one.

## Running it

1. Unpack the download somewhere.
2. Put your ROM in the `data` folder next to the executable, named exactly
   **`pd.ntsc-final.z64`**. (The folder is already there, with a
   `put_your_rom_here.txt` in it.)
3. Run it:
   - **Windows** — `pd.x86_64.exe`
   - **Linux** — `./pd.x86_64`
   - **macOS** — `./pd.x86_64` or `./pd.arm64`, whichever matches your machine

If the ROM is missing or is the wrong one, the game says so in a dialog box and
names the exact directory it looked in. On Linux and macOS you can put the `data`
folder in `~/.local/share/perfectdark` or `~/Library/Application Support/perfectdark`
instead, and `--basedir <path>` overrides both.

macOS will refuse to run an unsigned binary downloaded from the internet. Either
right-click → Open, or run `xattr -dr com.apple.quarantine .` in the unpacked
folder.

A GPU with OpenGL 3.0 / ES 3.0 or better is required.

## What it adds

All of the fork's settings are under **Options → Extended Options → Dab's Mod
Options**, on five pages that swipe left and right the way the Perfect Menu
swipes to Options: **Player**, **Camera**, **Display**, **Missions** and
**Recording**. A key bind sits on the page of the feature it drives. Each
setting persists to `pd.ini` under a `Mod.` key.

### Movement

| Setting | What it does |
| - | - |
| Jump | Off, or a height from 1 to 5. Bound to the **use** button, behind whatever that button opens — doors and objects still take priority |
| Jump For | Whether simulants jump too, or only players |
| Combat Roll | A dodge roll. **C** on the keyboard, right stick click on a pad |
| Melee Combos | The punch and kick combos solo play has always had, in multiplayer |
| Flinch When Shot | Bodies react to where the shot landed instead of ignoring it |
| Start Armed | Spawn holding a weapon — the arena's, or a random one. Off by default, because stock is off. In a solo mission, Random adds a random gun to the mission's kit; First Weapon changes nothing there, the mission having armed you already |
| Start Armed For | Whether simulants spawn armed too, or only players |
| Mission Respawn | A death in a solo mission is a new life where you fell, with full health and everything you were carrying, instead of Mission Failed. Off by default |
| Lives | How many lives a mission has in all: Unlimited, or 5 to 50 in fives. With 5, the fifth death ends the mission |

### Third person

**V** on the keyboard, Back on a pad, in solo and multiplayer both. You drop back
to first person while aiming. Camera Distance, Camera Wall Clearance and Camera
Minimum Distance tune the framing.

**Camera Tether** makes the camera a pole attached to you instead of one bolted
to the back of your head. The right stick orbits the camera all the way round
you, the left stick moves you in screen space and you turn to face the way you
go, and holding the trigger turns you to face the camera, which is where the
shot goes. The pole is elastic: strafing swings it round and it settles back
behind the camera. Loose lets it swing further and takes its time, Tight holds
it close and is nearly a rigid pole. Off is the rigid camera, with you always
facing the aim, and is the default. Body Turn Speed is how fast you come round
to face the way you are going, in degrees per frame: 30 by default, 90 is as
good as instant.

### Picture

Built in, no pack needed. Each is on the same options page.

| Setting | What it does |
| - | - |
| Clean Text Outlines | Outlined text gets a thin halo drawn by the renderer instead of the black cell the font bakes in, which at monitor resolutions is a slab behind every letter |
| Smooth Text | The font's glyphs are scaled up four times over with their edges sharpened, so letters have a clean edge instead of a staircase of blurred squares. On by default |
| Model LOD | The game's own swap to low-detail bodies at a distance. Off keeps the full model at any range |
| Enhance Textures | The game's own textures scaled up two, four or eight times over as they load, resampled through a curve rather than the GPU's straight blend. Nothing is invented, but a 32-texel wall stops being a grid of soft blobs. Textures that are themselves a dot pattern, like a halftone portrait or a screen of tiny text, are left alone. 2x by default; 4x costs four times the video memory and 8x sixteen |
| Vivid Colours | Saturation and contrast turned up on the finished frame, which was drawn for a CRT and looks flat on a panel. Light by default; Normal and Heavy go further. Screenshots and recordings get it too |
| Black Level | A floor taken off the frame's blacks, so black is black rather than dark grey, without crushing the shadows above it the way contrast would. Light takes 2% off and is the default; Normal 4%, Heavy 7% |

Texture packs, where installed, are left alone by Enhance Textures and Smooth
Text: their images are already whatever size their author chose.

### Simulants

Up to **80** in a Combat Simulator match, set the usual way in the simulant
menu. Stock hides any count above four until ten Combat Simulator challenges
are done; this fork does not gate its own cap, so all 80 are there on a fresh
profile. The four Bond heads that share that unlock are still earned normally.

The save file only grows past the stock 8-simulant format when a setup actually
needs it, so ordinary setups stay readable by an unmodified port.

### Bodies

Bodies stay where they fell instead of vanishing. **Bodies** caps how many are
kept (up to 500), **Body Time** how long each lies there, and **Bodies Drawn**
how many may be drawn in one frame — lower that one first if the frame rate
suffers, since the bodies still exist, they just aren't all rendered.

### Spectator

A camera that comes off the player and flies through the level, including
outside it. **F** on the keyboard. **Start Spectating** enters it automatically
on every stage; **Spectator Start Game** in the Combat Simulator menu arms it for
one match only. Spectator Speed sets how fast it flies. Combined with a match
with no time or score limit, it's the tool for watching 80 simulants fight.

### Screenshots and video

- **F12** writes a PNG to `screenshots/` beside your `pd.ini`.
- **F11** starts and stops recording an MP4 in `recordings/`, picture and sound.

Recording shells out to **ffmpeg**, which is not bundled — install it and make
sure it's on your `PATH`, or point `Mod.RecordEncoder` in `pd.ini` at the
binary. Frame rate, quality and the on-screen red dot are in the options page.
Screenshots need nothing extra.

### Texture packs

A pack goes in **`texture-packs/`** beside the executable - a folder, or the
`.zip` or `.7z` it came in - and is chosen in Extended Options under **Texture
Packs**, where **Use Texture Packs** turns the whole thing on. F8 toggles packs
in play, F9 re-reads the one you are using, F10 steps to the next.

**Community Packs**, on that page, does the whole of that for you for the packs
it knows about: it asks the pack's own release page which version is current,
downloads it, checks it against the hash the release publishes, unpacks it and
selects it. It also writes the row-order marker a pack needs when its images
are stored the way an emulator wants them, which is the one thing that is
invisible when a pack is installed by hand and turns every texture in the game
upside down when it is missing.

The list is short and everything on it is somebody else's work, credited on the
page with a link to where it came from:

| Pack | By |
| - | - |
| PD Plus HD Textures | Parabolee of Retro Foundry |

### Model packs

A model pack replaces the game's geometry the way a texture pack replaces its
pictures. It goes in **`model-packs/`** beside the executable, a folder per
pack, and is chosen on the same page, where **Use Model Packs** turns it on:

```
model-packs/<pack>/n64/<model name>.obj    replaces one of the game's own models
model-packs/<pack>/xbla/<model name>.obj   replaces the XBLA release's mesh for it
```

The names are the ROM's own (`Pcrate`, `CcarringtonZ`, ...), which is what the
dump below writes, so a pack is made by dumping, editing in Blender or
anything else that reads OBJ, and dropping the file back in. A material named
`n64_0a9a` draws with that texture of the game's, `xbla_1156` with that record
of the XBLA release's, and a material whose `map_Kd` file is beside the OBJ
draws with that picture. A pack chosen from the menu takes effect at the next
level.

### Dumping everything

**Dump All Assets To Disk**, on the Texture & Model Packs page (or
`--dump-assets` from the command line), writes every texture and every model
the game has, somewhere you can edit them:

```
texture-dumps/<romid>/         every texture in the ROM, the layout a texture pack reads back
texture-dumps/<romid>/xbla/    every texture of the XBLA release, the layout a pack's xbla/ folder is
model-dumps/n64/               every model in the ROM as OBJ, a group per part, textures in the MTL
model-dumps/xbla/              every mesh of the XBLA release the same way
```

The two XBLA folders are written when a copy of the release is in `xbla/`. It
takes a few minutes and the game stays usable while it runs; the line under the
row says where it is up to. F7 still writes out the textures the game draws as
it draws them, which is the way to learn which file a particular wall or jacket
is.

Each file is named for the model it belongs to, which is also the name a model
pack puts it back under. Thirty-nine of the release's meshes belong to models
this game has not - 4J's build had a few more - and those are named for where
they sit in the release's own order, `Ghand_a51guardZ+1` being the first mesh
after `Ghand_a51guardZ`'s. Nothing in the game draws them, so a pack cannot
put anything in their place; they are there to be looked at, and each one says
so at the top of the OBJ.

### The XBLA release

Put `Perfect Dark XBLA.7z` in **`xbla/`** beside the executable and nothing
else: the archive is unpacked once into `cache/xbla/`, and the Xbox 360 (XBLA)
page converts its textures into a pack and draws its models and rooms.

## Mods

The port can mount mod directories, and this fork extends that to stages,
Combat Simulator arenas, weapons and characters a mod supplies - including
console mods, the ROM patches made for the N64 game. Nothing is bundled here:
mods are other people's work, and you get them from their authors.

### Dropping a mod in

1. Make a folder called `mods` next to `pd.x86_64` (or `pd.x86_64.exe`).
2. Put the mod in it, exactly as you downloaded it. Any of these work:
   - a mod folder (one holding `files/`, `segs/` or `textures/`);
   - the `.zip`, `.rar` or `.7z` that folder came in;
   - a console mod's download: an archive with its `.xdelta`, `.bps` or
     `.ips` patch inside, or the bare patch file;
   - a whole collection of the above in one archive. Nested archives are
     fine.
3. Start the game. Archives are unpacked and patches are converted on that
   first start - a few seconds for one mod, up to a minute for a big
   collection, with nothing on screen yet while it happens.
4. Go to **Options > Extended Options > Load Mods** and pick the mod from
   the list. One mod at a time.
5. If the game offers **Restart Now**, take it: a mod that replaces ROM audio,
   textures or its data tables can only be read at start-up. The choice is
   remembered, so the next start comes up with the mod loaded.

To go back to the stock game, choose **None** on the same page.

### What ends up in `mods/`

Each zip is unpacked into a folder of the same name, and each console patch
becomes a folder named after the patch, with an `IMPORT.txt` inside that says
what came across and what the port could not use. Everything happens once;
delete a folder to have it done again, for instance after replacing the zip
with a newer version.

A patch that was made against a different ROM is refused, and `IMPORT.txt`
says so - the Japanese-region patches need the Japanese ROM. A patch made on
top of another mod's patch is tried on top of each patch found beside it, so
a download that ships both works as it comes. A mod that changed only the
game's code has nothing the port can carry over, and its folder says that
too. Neither shows up on the Load Mods page.

A console mod's download may include the hi-res texture pack made for it, as
an emulator cache file (`..._HIRESTEXTURES.htc` or `.dat`). That is picked up too and
loads with the mod - once **Use Texture Packs** is switched on, in Extended
Options under Texture Packs (it is off by default; F8 toggles it in play).
GE-X's is a text pack: its fonts.

What a console mod built for its own patched code cannot be used as is - a
setup or character model in a format the port does not read - is set aside in
`files.incompatible/` inside its folder rather than crashing the game. The
stock file stands in for it.

### Every mod's maps at once

**Options > Extended Options > Stage Loader** puts the maps of installed
mods into the Combat Simulator's arena list, beside the game's own, named
after the map and the mod. Tick a mod there, or "every installed mod", and
its maps appear at once (or after a restart, when the mod you have loaded
replaces ROM audio or textures). Nothing in the game's own maps is
replaced: a mod chosen here is read for its maps alone, whatever mod is
loaded on the Load Mods page. The game has room for 27 extra maps at a
time, so with a big collection choose the mods you want rather than all.
### From the command line

Pass one or more directories with `--moddir`, which wins over the menu choice:

```
pd.x86_64 --moddir mod_allinone
```

The directory is looked for next to the executable and in your home data
directory. Folders whose name starts with `mod` sitting next to the executable
are listed too, which is how mods have shipped for the stock port. The command
line flags the All in One Mod's own launcher uses (`--gexmoddir` and friends)
are accepted as aliases so its scripts work unchanged. `tools/importmod` in
the source tree does the same conversion of a console patch outside the game.

## Sharing a machine with the stock port

The config file is called `pd.ini` regardless of which build wrote it, so a
Dab's Mod build unpacked into the same folder as a stock port will share its
settings and saves. The `Mod.` keys are ignored by builds that don't know them,
and MP setups stay in the base format unless they need more than 8 simulants, so
nothing breaks — but if you want them kept apart, unpack into separate folders,
or pass `--savedir <path>`.

## Building from source

Same as the stock port:

```sh
git clone --recursive https://github.com/DabDavis/perfect-dark-dabs-mod.git
cd perfect-dark-dabs-mod
cmake -G"Unix Makefiles" -Bbuild .
cmake --build build -j8
```

You need gcc/g++ 10+, cmake, python3, SDL2 2.0.12+, libGL and zlib. Windows
builds go through MSYS2's MINGW64 prompt; see the
[upstream README](https://github.com/perfect-dark-pc-port/perfect_dark#building)
for the package list.

Non-debug builds compile at `-Og`, not `-O2` — that's upstream's setting, not an
oversight, and the official port binaries are built the same way. `-O2` breaks
decompiled code that relies on undefined behaviour.

## Anything not listed here

Controls and rebinding, PAL/JPN and Nintendo Switch builds, video and audio
settings, and the rest of the port's behaviour are unchanged from upstream and
documented in its [README](https://github.com/perfect-dark-pc-port/perfect_dark#readme)
and [wiki](https://github.com/perfect-dark-pc-port/perfect_dark/wiki).

## Credits and licence

MIT, same as the port and the decompilation it came from — see `LICENSE`.

- The decompilation: [n64decomp/perfect_dark](https://github.com/n64decomp/perfect_dark), Ryan Dwyer and contributors
- The PC port: [perfect-dark-pc-port/perfect_dark](https://github.com/perfect-dark-pc-port/perfect_dark) and contributors
- Perfect Dark is © Rare / Microsoft. This project ships no game assets and is
  not affiliated with either.
- RAR archives are read with RARLAB's UnRAR source (`port/src/external/unrar`),
  © Alexander Roshal, under its own licence: UnRAR source code may be used in
  any software to handle RAR archives without limitations free of charge, but
  cannot be used to develop RAR (WinRAR) compatible archiver and to re-create
  RAR compression algorithm, which is proprietary. Distribution of modified
  UnRAR source code in separate form or as a part of other software is
  permitted, provided that full text of this paragraph, starting from "UnRAR
  source code" words, is included in license, or in documentation if license
  is not available, and in source code comments of resulting package. The
  full text is in `license.txt` beside the source.
- 7z archives are read with the LZMA SDK (`port/src/external/lzma`), public
  domain, Igor Pavlov.
