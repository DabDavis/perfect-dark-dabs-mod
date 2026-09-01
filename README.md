# Dab's Mod

**[Download the latest build](https://github.com/DabDavis/perfect-dark-dabs-mod/releases/tag/dabs-mod-dev)**
— Windows, Linux and macOS. You supply the ROM; see [You need a ROM](#you-need-a-rom).

A fork of the [Perfect Dark PC port](https://github.com/perfect-dark-pc-port/perfect_dark),
which is itself a port of the [Perfect Dark decompilation](https://github.com/n64decomp/perfect_dark).

It adds movement the N64 game never had — jump, a combat roll, melee combos, a
third person camera — raises the Combat Simulator simulant cap from 8 to 80,
keeps bodies lying where they fell, and gives you a free-flying spectator
camera, a screenshot key and a video recorder for watching the results.

Everything here is engine-side and lives in the executable. No custom levels or
assets are bundled, and none are needed.

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

### Third person

**V** on the keyboard, Back on a pad, in solo and multiplayer both. You drop back
to first person while aiming. Camera Distance, Camera Wall Clearance and Camera
Minimum Distance tune the framing.

### Simulants

Up to **80** in a Combat Simulator match, set the usual way in the simulant
menu. The save file only grows past the stock 8-simulant format when a setup
actually needs it, so ordinary setups stay readable by an unmodified port.

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

## Optional: custom levels

The port can mount mod directories, and this fork extends that to stages and
Combat Simulator arenas a mod supplies. Pass one or more with `--moddir`:

```
pd.x86_64 --moddir mod_allinone
```

The directory is looked for next to the executable and in your home data
directory. Nothing is bundled here — the **All in One Mod** and similar packs
are other people's work, and you should get them from their authors. The command
line flags that mod's own launcher uses (`--gexmoddir` and friends) are accepted
as aliases so its scripts work unchanged.

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
