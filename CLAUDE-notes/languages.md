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
  Dark's CI4 glyph format, 12x11 and 16x15 as Rare's own JP font was.
