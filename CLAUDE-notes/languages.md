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
  may be `"ctx|English"` where one English string needs two translations. The
  keys are `lang/_source/port.json`'s (see "Source catalogs").

## Source catalogs (the English a translator works from)

```
python3 tools/langpack/extract.py              # writes lang/_source/
python3 tools/langpack/extract.py --leftovers  # drawn-looking literals no hook reaches
python3 tools/langpack/check.py lang/fr        # checks against lang/_source/ when it is there
```

`lang/_source/` is **generated and gitignored**, never committed: `ge.json` is
GoldenEye's English, which is Rare's text (the same "no Rare text in the repo"
rule as the packs), and `port.json` is kept beside it so the two are made the
same way. Regenerate after any change to a port string; `check.py` then
rejects a pack key the game no longer asks for.

- `port.json` - `{ key: English }`, every port string the player can see, keyed
  exactly as `langTr()` looks it up: the English with its trailing `\n` dropped
  (one leading `\n` or inner ones stay), `ctx|English` for a `langTrCtx()` call.
  A translator copies it to `lang/<code>/port.json` and replaces the values.
  `port.ctx.json` is `{ key: ["file:line", ...] }`, where each one is used.
- `ge.json` - `{ "ge.<bank>.<slot>": English }` from the GoldenEye decomp's US
  English text files, `007/assets/obseg/text/L*E.c` (found beside this tree or
  a parent of it, or `--ge-text DIR`, or `$PD_GE_TEXT`), preprocessed as the US
  cartridge (`LANG_US`: LlenE's and LtitleE's Japanese-only rows are left out,
  and the slot numbers are the US ROM's). `u/` and `j/` there are Japanese only.
  Rows with no letter in them (`"\n"`, `"007"`) are left out.
- Perfect Dark's own needs no catalog: `src/assets/ntsc-final/lang/<bank>.json`'s
  `en` is the source (and is the ROM's own text, committed by the decomp).

What extract.py counts as a port string (it reads port/src, src/game and
src/game/mplayer; a decomp file with no marker is skipped): a menu item with
`MENUITEMFLAG_LITERAL_TEXT` (label and right-hand text), a dialog with
`MENUDIALOGFLAG_LITERAL_TEXT` (title), what a `MENUOP_GETOPTIONTEXT` handler
returns (a literal or any row of a string table it indexes), and the argument
of `langTr()`, `langTrFind()`, `langTrCtx()`, `langAddPortText()`, `LANG_N()`
and gebean.c's `POOLBODY()`. **`LANG_N("...")` is a no-op marker** (langpack.h)
for a string kept in a table, or written by a worker thread, and translated
where it is used: mark the table, `langTr()` the entry at the draw.

On 2026-09-29: port.json 973 strings, 17,452 English characters; ge.json 1,724
strings, 69,785 characters - title 278, gun 221, len 124, sevb 94, ark 68,
stat 68, misc 67, propobj 65, silo 63, arch 60, options 59, sev 45, arec 45,
tra 44, jun 42, cave 38, mpmenu 36, sevx 36, crad 34, dam 33, azt 33, dest 30,
sevxb 30, pete 29, depo 28, run 25, cryp 15, mpweapons 14 (the unused levels'
files - ame, ash, cat ... - are empty).

**Wiring a new port string.** A literal label: `MENUITEMFLAG_LITERAL_TEXT`. A
string built at run time: `snprintf(buf, n, langTr("Loaded: %s\n"), name)` - the
*format* is the key, check.py holds a translation's conversions to it, and a
format with none is `snprintf(buf, n, "%s", langTr("..."))`. Pluralise with a
whole sentence per case (`mods == 1 ? langTr("... %d mod ...") : langTr("... %d
mods ...")`), never `"mod%s"`. A text handler (`menutext*()`) returns
`(char *)langTr("...")` - only LITERAL items are translated by the menu code.
Never call `langTr()` off the main thread (it builds a value's newline copy on
first use): a worker writes `LANG_N("...")` and the menu that shows the message
`langTr()`s it (`ghostnetGetMessage()`, `communityGetStatus()`, the F3 and
crash dialogs' `g_Err`); a worker's formatted message stays English.

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

**`--lang-log-missing`** (log lines, each once):
- `lang: missing [fr] port "English"` - a port string asked of `langTr()` that
  the pack lacks (a string a lookup already handed out - a translation passed
  through `langTr()` again, a ROM string - is not logged);
- `lang: missing [fr] ge.dam.5` and `lang: missing [fr] pd.5608 "Red\n"` -
  GoldenEye's and Perfect Dark's by key (`pd.` is the text id in hex:
  bank = id >> 9 in check.py's `BANKS`, row = id & 0x1ff);
- `lang: not looked up "text"` - **the leftover finder**: a string Perfect
  Dark's text loops (`textRender()`, `textRenderProjected()`) or GE Plus's
  (`frontText()`) drew that no lookup handed out. `langTr()`, `langGet()` and the
  GE bank readers note what they return, `textWrapN()` carries a note to its
  wrapped copy; anything else drawn with two letters in a row is logged (up to
  1000). Text built by `snprintf()` from a translated format is logged too -
  judge by the source; `extract.py --leftovers` is the static half.
It works with English (US) selected as well (only the not-looked-up lines).

**Hooked on 2026-09-29 (second pass):** the Player N option pages' titles
(title handlers, `"Player %d ..."`), slider labels (`MENU_SLIDERLABEL_LEN`, 64
bytes, was 16), every drawn `snprintf()` in optionsmenu/ghostmenu/tracereport/
crashreportmenu/updatemenu/communitymenu/randommenu/mpsetups/patchnotes, the
bind-name tables, HUD notices (Enhancements/HD Assets/Texture Pack - its
`hudmsgRemoveByPrefix()` takes the translated format's prefix - and the
GoldenEye key analyser's), the GE Plus front's own rows (EXTRA, Cinema,
Monitor Programmes and their 52 names, Simulants), the watch's `FIRE MODE`/`PC`,
GoldenEye's pickup messages (`geHudPropobjString()`, `ge.propobj.<slot>`), the
Japanese credits tail (`g_CreditsJpText`; the US roll was already `ge.len.N`
through `LANGBANK_GEMISSION`), GE gun/gadget/function names, GE and PD head and
body names (`mpGetHeadName()`, `mpGetBodyName()`), the Randomizer's objective
and score texts.

**Not done yet (the next agent's list):**
- The XBLA font (`Mod.XblaFont`) and texture packs have no picture for a
  composed glyph (id 0x100+), so accented letters keep the ROM's look beside
  the release's letters. `gfx_pc.cpp`'s glyph branch would compose the base
  letter's release picture with the mark scaled up (`langfontGlyphSlot()` and
  the recipe give base, marks and the cell's row offset).
- GE Plus with the release's font on draws a string that has a character past
  ASCII in GoldenEye's N64 font (the release's has none). `frontWrap()`'s
  Japanese breaking (it breaks only at spaces) is not done, and
  `frontMissionName()` upper-cases ASCII only.
- Messages a network worker formats (update.c's "%s is out. You have %s.",
  ghostnet.c's "uploaded %d of your ghosts", community.c's "Installing %s")
  stay English: only their fixed messages are `LANG_N()`-marked and translated
  at the menu. They would need a code and arguments handed to the main thread.
- No `langTrCtx()` call sites yet (no English has needed two translations).
- `challengeLoadConfig()`'s mpstrings (challenge descriptions, simulant names)
  do not go through `langGet()` and are not translated.
- The GE gadgets' own names are port strings (`LANG_N` in gegadgets.c), so a
  translator writes "Door Decoder" in port.json and GoldenEye's same name again
  as `ge.gun.<slot>`.
- Japanese: menus that are exactly full at 9-13 px lines will need scrolling at
  14; the checker does not measure widths (only `--lang-audit` does, in the sm
  font, and needs the ROM).
