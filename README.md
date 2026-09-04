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

All of the fork's settings are on one page: **Options → Extended Options → Dab's
Mod Options**. Each one persists to `pd.ini` under a `Mod.` key.

### Movement

| Setting | What it does |
| - | - |
| Jump | Off, or a height from 1 to 5. Bound to the **use** button, behind whatever that button opens — doors and objects still take priority |
| Jump For | Whether simulants jump too, or only players |
| Combat Roll | A dodge roll. **C** on the keyboard, right stick click on a pad |
| Melee Combos | The punch and kick combos solo play has always had, in multiplayer |
| Flinch When Shot | Bodies react to where the shot landed instead of ignoring it |
| Start Armed | Spawn holding a weapon — the arena's, or a random one. Off by default, because stock is off |
| Start Armed For | Whether simulants spawn armed too, or only players |

### Third person

**V** on the keyboard, Back on a pad, in solo and multiplayer both. You drop back
to first person while aiming. Camera Distance, Camera Wall Clearance and Camera
Minimum Distance tune the framing.

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

## Mods

The port can mount mod directories, and this fork extends that to stages,
Combat Simulator arenas, weapons and characters a mod supplies - including
console mods, the ROM patches made for the N64 game. Nothing is bundled here:
mods are other people's work, and you get them from their authors.

### Dropping a mod in

1. Make a folder called `mods` next to `pd.x86_64` (or `pd.x86_64.exe`).
2. Put the mod in it, exactly as you downloaded it. Any of these work:
   - a mod folder (one holding `files/`, `segs/` or `textures/`);
   - the `.zip` that folder came in;
   - a console mod's download: a `.zip` with its `.xdelta`, `.bps` or `.ips`
     patch inside, or the bare patch file;
   - a whole collection of the above in one archive. Nested zips are fine.
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
an emulator cache file (`..._HIRESTEXTURES.htc`). That is picked up too and
loads with the mod - once **Use Texture Packs** is switched on, in Extended
Options under Texture Packs (it is off by default; F8 toggles it in play).
GE-X's is a text pack: its fonts.

What a console mod built for its own patched code cannot be used as is - a
setup or character model in a format the port does not read - is set aside in
`files.incompatible/` inside its folder rather than crashing the game. The
stock file stands in for it.

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
