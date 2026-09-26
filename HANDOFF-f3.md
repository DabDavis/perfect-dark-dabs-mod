# F3 20260925-225628: HD glass at distance vs the N64 look (2026-09-26), branch fix/f3-hd-glass-n64

Report (savantique, 718d5dc, Facility 0x7a = our 0x63, HD look): "draw distance anomaly
causes glass to render fog outside a certain range" - the lab windows solid blue.
User's decision: HD glass at distance should match the N64 look.

## Result

The windows are GoldenEye tinted glass (OBJTYPE_TINTEDGLASS, model 0x268, xludist 400,
opadist 600 on the two lab panes; the report camera is 634 and 689 units from them).
glassCalculateOpacity() gives 255 there, the portal behind shuts, and **both looks draw
the pane opaque** - that part is GoldenEye's own rule and already matched. The N64 look
paints it in GoldenEye's own glass picture (dark grey streaks), the HD look in Bean's
32x32 tinted pane (flat dark blue, 21,43,57). That colour is the only difference past
opadist, and it is the texture, not the behaviour.

**Between xludist and opadist the HD look was wrong** and is fixed (21ef9db8a,
port/src/xblamesh.c): the Bean pane is a fading span (vertex alpha 0.56), whose list
carries a two-cycle combiner that lays the prim alpha (the pane's opacity) over its own
in the second cycle. xblaMeshRenderNode() never set the cycle type for that draw, so it
inherited whatever drew last. In Facility that was one cycle: the pane stayed at its own
~26% alpha from xludist all the way to opadist (at 594 units, opacity 247, you still saw
straight through), then jumped to opaque at once - the "certain range" the tester saw.
In the N64 look it thickens smoothly over that range. Now gDPSetCycleType(G_CYC_2CYCLE)
before the fade list; at prim 0 (every non-mode-9 draw: the release's light beams) the
second cycle is a pass-through, so nothing else changes.

Answer for the tester: the windows going solid past a range is GoldenEye's own tinted
glass rule (the N64 look does the same, in GoldenEye's darker picture); the sudden pop
was ours and now fades in like GoldenEye's. Glass See-Through (Display page; Dab's
Settings = 50) keeps them partly clear at any distance, in both looks.

## Verification (pictures in /home/sdg/wt/f3glassn64-pics)

- `fac-cmp-0.png` / `fac-cmp-50.png`: Facility, camera on the report line at 689, 594,
  544, 444, 295 units (opacity 255, 247, 183, 56, 0); rows base HD / fixed HD / N64,
  slider off and at 50. Fixed HD follows the N64 curve at every distance; with 50 both
  looks keep about half see-through at 689.
- `tour-0x6e.png`: Archives' upstairs panes (xlu 0, opa 300) at 100/150/280/550: base HD
  see-through at 280 (opacity ~238), fixed HD and N64 opaque there.
- `tour-0x69.png`, `tour-0x15.png`, `tour-0x66.png`: Control, Dam, Caverns panes. The
  automatic cameras (on the pane's portal normal) mostly land in walls or dark corners in
  HD, so these show little; Control's one change is a small pane, Dam unchanged.
- A/B at frame 400 (spawn view): all 20 GE Plus missions HD and six PD stages in the
  XBLA look (Defection, Investigation, Extraction, G5, Chicago, Villa) pixel-identical.
- Only GL checked: `--vulkan` cannot make a window under SDL's offscreen driver.
- Bunker (sev, sevb) has no tinted panes; missions with them: Dam, Facility, Caverns,
  Control (0x69/0x6a, 52 each), Archives-set 0x6e (30), Aztec (3, no portal).

## a5355ec05 (fix/f3-dam-white-decals)

Cherry-picked on top for a test build only (not committed here): every glass picture
above (Facility series, Archives, Dam tours) pixel-identical with and without it. It does
not touch the glass.

## Open

- Past opadist the HD pane is flat blue where the N64's is GoldenEye's dark reflective
  picture. If the user wants the colour matched too, the HD equivalent is Bean's own
  reflection map `_0x00B5FD45` (256x256 DXT1, black with a few lights) that
  beanTexIsGlassOverlay() drops from the window material: laying it over the pane by the
  opacity would give the N64's dark look at distance. Not done - needs a user decision.

## Rig (outside the tree)

- run dir `/home/sdg/wt/f3glassn64-run` (copies of f3hdlevels-run's cache/mods/saves);
  build `/home/sdg/wt/f3glassn64-build`; binaries `f3glassn64-rig/pd.base` (1fc1832d8),
  `pd.fix`, `pd.dam` (fix + a5355ec05).
- `views.sh STAGE TAG` (as f3hdlevels'; `BIN=` must be an absolute path - a relative one
  silently runs the last binary), `N64=1`, `GROUND=`, `ARGS=`, `EXTRASED=` (add the slider
  with `/^\[Mod\]/a GlassSeeThrough=50`: the key is not in the saved ini).
  Facility report camera: `VIEWS='-3607.3,-159,2295.4,190.5,-2.1,18' GROUND=-319`.
- `glass.py` (gdb `dumpglass`: tinted/plain glass and doors near the camera with
  xlu/opa/opacity/portal), `tour.py` via `SCRIPT=... tour.sh` (`PANES=` indices or
  `PANEPOS='x,z;...'` - the N64 look lists props in another order), `ab.sh` + `abdiff.py`,
  `cmp.py` (picture grids).
