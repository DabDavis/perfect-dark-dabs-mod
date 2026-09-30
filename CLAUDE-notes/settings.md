# Settings, defaults and presets

## Digest (moved from CLAUDE.md, 2026-09-30)

The entry CLAUDE.md carried for this, verbatim; the code is the long form.

- **Defaults and the Settings Preset** — `src/game/modoptions.c` and `g_ModPresets` in `port/src/optionsmenu.c`: since 2026-09-09 a fresh pd.ini is stock Perfect Dark with the fixes on and the additions off (and since 2026-09-13 Smooth Text and Thin Text Outlines - "Clean Text Outlines" until 2026-09-12, and still that in pd.ini and in the code - are off too, in the defaults and in the Vanilla and Ghost Trials presets, so the text is the ROM's), because a tester met a game that jumped, rolled, tilted and flew (F was the Spectator); the fork's old defaults are the "Dab's Settings" preset, and a preset is applied on selection and remembered by nothing - the dropdown reads Custom once any covered setting differs. The third person camera has its own preset and is not covered; a pd.ini written by an older build keeps every value it has, including F

