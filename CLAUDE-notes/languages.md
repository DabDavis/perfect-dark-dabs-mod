# Languages

One setting (`Game.Language` in pd.ini, a dropdown on the Game options page)
changes every piece of text: Perfect Dark's own, GE Plus's, and the port's. The
game reads its English from the player's ROM as it always has; every other
language is **our own translation**, shipped with the port. Rare's PAL and JPN
text (`src/assets/{pal,jpn}-final/lang`) is a *reference* for names and
terminology only - never copied into a pack - so the port ships no text of
Rare's (user's decision, 2026-09-29). The same goes for GoldenEye's Japanese in
the US ROM: we write GE Plus's Japanese ourselves.

Languages at first: `fr` French, `de` German, `es` Spanish, `it` Italian, `ja`
Japanese, and `en-GB` English (UK). English (US) is the ROM's and has no pack.
`en-GB` is a small pack of our own British spellings (armour, colour, metre,
defence, …) with `"fallback": "en"`: only strings that change are listed. Rare's
own PAL "gb" text changed just five strings ("Head Colour" and the four
language names), so there is nothing of theirs to follow there. More are added by dropping a
pack in `lang/`.

## Source files (what a translator edits)

```
lang/<code>/meta.json        {"name": "Français", "script": "latin" | "cjk", "fallback": "en"}
lang/<code>/pd/<bank>.json   Perfect Dark's text, one file per ROM bank
lang/<code>/port.json        the port's own strings, keyed by their English
lang/<code>/ge.json          GE Plus's text, keyed ge.<bank>.<slot> or by English
```

- `pd/<bank>.json` is an object `{ "L_AME_000": "…", … }` keyed by the ids in
  `src/assets/ntsc-final/lang/<bank>.json`, whose `en` field is the English
  being translated. Every id of the ntsc-final file should appear; a missing id
  falls back to English.
- Text is UTF-8. Keep every control sequence exactly as the English has it:
  `\n` line breaks (and the trailing `\n` every ROM string ends with - a string
  without one measures zero height), `|` section markers in briefings, printf
  conversions (`%s`, `%d`, `%02d`, …) in the same order and number. The pack
  checker rejects a string whose conversions differ from the English, because a
  mismatch crashes.
- Proper names stay as the game has them (Joanna Dark, Carrington, dataDyne,
  Elvis, the weapon brand names like Falcon 2, CMP150, DY357-LX); a translated
  descriptive name (a level's subtitle, "Laptop Gun") may be translated.
- Length: menu labels do not wrap. Keep a label within about 1.3x the English
  width; `--lang-audit` reports anything over.
- `port.json` is `{ "English exactly as in the source": "translation" }`. A key
  may be `"ctx|English"` where one English string needs two translations.

## Built packs

`tools/langpack/build.py` compiles `lang/<code>/` into one `<code>.lang`
(plain text: `@name`, `@script`, `@fallback`, then `key<TAB>value` lines with
`\n` escaped), checks it (`tools/langpack/check.py`: printf conversions, byte
limits, every character has a glyph), and the build embeds the packs in the
binary. A `.lang` dropped in the executable's `lang/` folder adds a language.

## Fonts

- Accented Latin letters are composed at font load from the ROM's own glyph
  plus a diacritic drawn per font size (`port/src/langfont.c`).
- Japanese is **M PLUS Rounded 1c** (SIL OFL 1.1, committed with its licence
  under `tools/langfont/fonts/`), baked by `tools/langfont/bake.py` into Perfect
  Dark's CI4 glyph format: 12x11 at an 11 px em for xs/sm/md and 14x15 at 14 px for lg (a 16 texel row has to hold the outline either side). 3358 characters, 154 KB, body stored at one bit a texel and the outline built on first use; `python3 tools/langfont/bake.py --extra-from lang/ja` after the ja pack gains characters.

## How it is wired (2026-09-29, P0-P2)

**Keys, exactly.** A `.lang` line is `key<TAB>value`, `\\`, `\n`, `\t` escaped:

| key | source | what reads it |
|---|---|---|
| `pd.0200` | `pd/<bank>.json`, `L_AME_000` = bank index (mklang's order) * 512 + row, hex | `langGet()` for a **stock** bank (`romdataFileIsStock()`), so a mod's own text wins |
| `port:English` | `port.json` | `langTr()`; a key or value's trailing `\n` is optional - looked up without it and put back as the English has it |
| `ge.<bank>.<slot>` | `ge.json` | `frontString()` (`ge.title.N`), `frontLangString()`/watch/`LANGBANK_GEMISSION` (`ge.dam.N`, ...), the watch's `ge.options.N`, `ge.mpmenu.N`, `ge.gun.N` |
| `ge:English` | `ge.json` keys that are not `ge.x.y` | `LANGBANK_PORT` strings (`langAddPortText()`: GoldenEye guns' names) after `port:` |

`<bank>` in a GE key is GoldenEye's **language file name** without its `L` and
`E`, lower case (`LtitleE` -> `title`, `LdamE` -> `dam`, `LarecE` -> `arec`), not
GoldenEye's bank number; `<slot>` is GoldenEye's text id `& 0x3ff`, decimal.
`meta.json` may carry `"sparse": true` to silence "not translated" warnings;
a code starting `en` (en-GB) is sparse by default. Every pack falls back to the
ROM's English for what it leaves out.

**Code.** `port/src/langpack.c` (packs, `Game.Language`, `--lang`,
`--lang-log-missing`, generation counter, dropdown list - English (US) is index
0 and has no pack), `port/src/langfont.c` + generated `langfonttable.h`
(`tools/langpack/latin.py --header port/src/langfonttable.h` - the same list
check.py uses to decide "has a glyph"), `src/game/lang.c` (the `langGet()` hook,
team names on a switch, `--lang-audit`), `src/game/game_1531a0.c` (the text
loops, `textWrapN()`), `port/src/gexfront.c` (GE Plus's I8 glyphs).

**Things that were not obvious:**
- NTSC's text loops read a byte >= 0x80 as the *first of two* bytes of GE's
  leftover Japanese font; that branch is where UTF-8 goes now. The ROM's English
  has no byte >= 0x80 (checked over every ntsc-final bank), so ASCII keeps the
  ROM's own path and English is pixel-identical (file select, Defection's
  briefing and the Perfect Menu diffed frame-exactly against the base build: only
  the fps box differs, as it always does).
- `chars['[']` is **not** '['. `chars` starts at 0x21, so `chars['[']` =
  `chars[0x5b]` = the glyph for 0x7c, `'|'`: its baseline + height is every
  line height, which is why PAL's +1 on the pipe's baseline spaced its lines.
  langfont.c applies it to xs/sm/md only while a Latin pack with any non-ASCII
  character is selected (so not English (UK)), and takes it off again live.
- A composed capital's accent is above the line (negative baseline). Two ROM
  clip tests then misbehave: a menu line's own top clip cut the accent off
  (the first line's É drew as E - `PORT_ACCENT_ABOVE`), and turned text (the
  side marquees) takes a "partly clipped" glyph down a branch that draws it
  *unturned* in the middle of the screen (`PORT_ROTATED_OURS`). Both relaxations
  are for langfont glyphs only.
- A glyph's cell: CI4, 8 bytes a row, height + 2 rows; ink at texels 1..width,
  rows 1..height; body 9-15, band 1-7 (the cell is solid band, corners partial).
  A mark is drawn as body and the band regrown one texel round it. Width may
  grow to fit a mark (i with a diaeresis), up to 14.
- The ROM's `textWrap()` cut a word at 32 bytes into a 32-byte buffer with no
  bound, `menuitemObjectivesRenderOne()` wrapped into 80 bytes, the HUD's
  subtitle splitter could overrun 250 and `hudmsgCreate...()` 400: all bounded
  or raised, and scrollables are 16384.
- A literal menu label is looked up by the English in `param2`; code that
  searches a label (`updateMaxAnisotropyLevel()`) must read `param2`, not the
  resolved text.
- `menuPushDialog(&g_CiMenuViaPcMenuDialog)` from gdb at CI shows the Perfect
  Menu without driving the file select, and `menuPushRootDialog(&g_SoloMission
  BriefingMenuDialog, 2)` a mission's briefing; `build-wt/cmpgdb.sh`-style
  runs (gdb `break lvTick if g_Vars.lvframenum == N`, `--fixed-step
  --rng-seed`, `--screenshot-frame`) give frame-exact pairs. The pause briefing
  of Defection is `L_AME_003`, not `_000` (that is the solo menu's).

**Not done yet (the next agent's list):**
- The XBLA font (`Mod.XblaFont`) and texture packs have no picture for a
  composed glyph (id 0x100+), so accented letters keep the ROM's look beside
  the release's letters. `gfx_pc.cpp`'s glyph branch would compose the base
  letter's release picture with the mark scaled up (`langfontGlyphSlot()` and
  the recipe give base, marks and the cell's row offset).
- GE Plus with the release's font on draws a string that has a character past
  ASCII in GoldenEye's N64 font (the release's has none). GE credits' role
  titles and literals in `ge*.c`, `gehud.c` notices, and `frontWrap()`'s
  Japanese breaking (it breaks only at spaces) are not hooked.
- Port strings built with `snprintf()` ("Player %d Game Options", HUD notices,
  F3 dialog, updater) are not translated; they need `langTr()` on the format
  and a key per format. No `langTrCtx()` call sites yet.
- `challengeLoadConfig()`'s mpstrings (challenge descriptions, simulant names)
  do not go through `langGet()` and are not translated.
- No whole-string safety net in the draw loops (`--lang-log-missing` only sees
  strings that reach `langTr()`).
- Japanese: menus that are exactly full at 9-13 px lines will need scrolling at
  14; the checker does not measure widths (only `--lang-audit` does, in the sm
  font, and needs the ROM).
