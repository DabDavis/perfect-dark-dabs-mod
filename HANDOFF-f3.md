# F3 Dam modem monitor + gate switch (2026-09-26), branch fix/f3-dam-monitor-button

Reports (trace + screenshot in /home/sdg/wt/f3-0926/), both HD look, Dam 0x15,
tester build ef6bffc (x86_64-windows, Community Edition on):
- 20260925-235034-ae5a6c08: "this monitor ... the one where you throw the modem bug
  onto is the wrong model. it should be the monitor, not the gate switch"
- 20260925-234625-7f17e175: "this button normally glows green when pressed to open
  the gate for the truck ... pushing it doesn't change its color"

## Cause (one fault, both reports) - FIXED + VERIFIED

The models are right. The monitor at (11228 13277 10335) is `Pgx335Z` =
GoldenEye `PROP_MODEMBOX` (setup index 290, SingleMonitor, tag 5, programme 5
"green text up"). The switch at (14075 13256 17402) is `Pgx336Z` = `PROP_DOORPANEL`
(MultiMonitor, tags 15-18, two per gate). Dam's ai_26/ai_27 set its screens 0/1 from
gate 8's/9's door state with `tv_change_screen_bank` (our 0x00da): closed 49/48
(red), closing 46/48, opening 48/47, open 48/50 (green). Both mapped by name in
geproptable.h to Bean's `prop/modembox` and `prop/doorpanel`.

Bean's two meshes are the same frame, and each has its screen as a separate quad on
one flat grey texel (all UVs equal): modembox draw 1, `.gpu` offset 1264, vertices
0-3; doorpanel draw 2, offset 1160, vertices 0-7 (the two lamps). The quad lies in
the screen's plane in front of GoldenEye's own screen quad. c5ed72971 already hands
the screen node (part 0-3) back to the game so tvscreenRender()'s programme draws
- it did, underneath Bean's grey card. So in HD the modem screen showed no green
text and looked exactly like the gate switch, and the switch's lamps never showed
the AI's red/green. The N64 look was always right; the AI and the gate were fine.

Fix, commit on this branch: `port/src/gebean.c` `beanVertexDrops[]` gains the two
quads (same mechanism as console2/console3). Notes: CLAUDE-notes/ge-bean.md,
paragraph after "The door consoles' lamp". Offline check
(`.xbla-work/ge-bean/bean2obj.py` Model): of every GoldenEye monitor prop with a
Bean mesh (tv1, console1-3, consolesev2b, doorconsole, modembox, doorpanel) only
these two have a flat-texel screen card. No converter change, no version bump.

## Verification (pictures in /home/sdg/wt/f3dammon-pics/)

- Look check: HD runs use `data/save_hd` (XblaMeshes=1); their logs build
  `gebean: Pgx335Z <- new/prop/modembox` (52 verts/31 tris before, 48/29 after) and
  `Pgx336Z <- new/prop/doorpanel` (52/32 before, 44/28 after). `save_hdbase`
  (copied from f3gemission-run) has XblaMeshes=0 = HD levels, N64 props; used only
  for the N64-look shots.
- Before, HD: `hd-before-modem.png`, `hd-before-panel.png` - grey cards, as reported.
- Before, N64: `n64-before-modem.png` (green text), `n64-before-panel.png` (red lamp).
- After, HD: `hd-after-modem.png` green text; `hd-after-press-sheet.png`: modem,
  switch before the press (red, gate 8 closing), then `propobjInteract()` on tag 15:
  gate opens (door 8 mode 1 -> idle frac 0.95), lamp goes dark -> green brightening
  -> green solid. `n64-after-press-sheet.png`: the same sequence in the N64 look,
  identical colours.
- Modem objective, HD with the fix (`hd-after-modem-objective-sheet.png`,
  modemprobe.py USE=6): thrown at tag 5 -> "Covert modem installed.", objective 2
  complete, using tag 6 starts the countdown.

## Rig (outside the tree)

- build `/home/sdg/wt/f3dammon-build` (RelWithDebInfo), run dir
  `/home/sdg/wt/f3dammon-run` (own ROM copy, Bean symlink, CE zip, own converted
  mods; `pd.before` = 1fc1832d8, `pd.fix` = the fix). Saves `data/save_hd`
  (XblaMeshes=1, CE on) and `data/save_hdbase` (N64 look).
- `views.sh TAG SAVEBASE` with `VIEWS="x y z theta verta"` lines; `runpy.sh TAG
  SAVEBASE probe.py`; `button.py` (both report views, press the nearest switch,
  shots at +20/+60/+150/+400 with door and screen state printed);
  `dumpmodel.py` (gdb `dumpobj TAG`: parts and node tree with live screen lists);
  `modemprobe.py` (copy of build/gexrom's).

## Open

Nothing for these two reports.
