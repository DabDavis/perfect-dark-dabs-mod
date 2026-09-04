# Save format

`mpsetupfileSaveWad()`/`LoadWad()` take a version. The port's file has a version byte
and per-version block sizes (`MPSETUP_BLOCKSIZE_V1` vs `MPSETUP_BLOCKSIZE`). Files
stay in the base format unless a setup actually needs >8 simulants, so unmodified
builds can still read them; changing version re-encodes every block.

`savebufferOr()` does no bounds checking. `stagenum` is stored in 7 bits.

**Everything lives beside the executable now** — saves, `pd.ini`, screenshots and
recordings — because a copy of the game should be one folder a player can open,
back up or move. `~/.local/share/perfectdark` is where it used to be and is still
the fallback for a copy that cannot write to its own directory: installed under
`/usr`, on a read-only mount, inside an app bundle. `fsChooseOutputDir()` makes
that choice for the player's own files and `fsInit()` makes it for the saves.

**The first run of a build that does this copies the old save directory across**
(`fsMigrateSaves()`), because otherwise someone who has played before starts with
a fresh eeprom, no unlocks and the simulant count back at four — which looks
exactly like the fork being broken. It copies rather than moves and never writes
over a file already there. It takes every file at the top of the old directory
rather than a named list — that directory is one the game itself chose and has
been saving into, and a save missed by an allowlist is exactly the failure this
exists to prevent — plus `ghosts/` and `exported/`. Screenshots and recordings
are left behind deliberately, being potentially gigabytes and nothing that stops
working. A copy that fails part way removes what it wrote, because nothing here
overwrites an existing file and a truncated eeprom would be loaded every run
from then on.

The old default was the working directory whenever a config happened to be
sitting in one, and that was never checked for writability — it is now, or a
read-only working directory silently swallows every save.
