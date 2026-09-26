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

**The colour past opadist, second commit** (coordinator: the user's "match N64" covers
it). The window's Bean material carries the reflection map `_0x00B5FD45` (256x256 DXT1,
black with a few lights) as a second picture, which beanTexIsGlassOverlay() keeps out of
the pane. gebean.c now remembers it for the pane (`beanmodel.glassrefl`,
beanTexIsGlassReflection()) and binds it in gebeanBuildRigid() (`gebeanmats.glassrefl`);
xblamesh.c builds `m->glassrefl`, a copy of the mesh's list with those panes bound to
the map, the other materials' geometry dropped and the combiner TEXEL0*SHADE, alpha =
PRIMITIVE (xblaMeshGlassCopy(), the logo copies' pattern), and draws it straight after
the pane's fade list while the mode-9 opacity is above 0. So the map lies over the pane
by the pane's own opacity: nothing up close, all of it past opadist, and Glass
See-Through's cap caps it too. Only Bean meshes whose pane has that map (the window
prop; also gasplantcleardoor's `.bmp.bin` spelling, which would go dark with a windowed
door's fade as the N64 door window does - not seen in the sweeps).

Answer for the tester: the windows going solid past a range is GoldenEye's own tinted
glass rule (the N64 look does the same); the sudden pop was ours and now fades in like
GoldenEye's, and the far pane is now dark like GoldenEye's rather than flat blue. Glass
See-Through (Display page; Dab's Settings = 50) keeps them partly clear at any distance,
in both looks.

## Verification (pictures in /home/sdg/wt/f3glassn64-pics)

Reflection map (second commit): `facr-cmp-0.png` / `facr-cmp-50.png` (rows: first fix,
with the map, N64) - past opadist the HD pane reads dark with the map's few lights and
its glare where the N64 is dark streaked grey; at 444/295 units it is as before; at 50
the map is at half. `tourr-0x6e.png` (Archives-set panes, opaque from 280), `tourr-0x66.png`
(Caverns' far pane: blue -> dark, as the N64), `tourr-0x69.png` (Control: small panes darker).
Spawn sweeps first fix vs map: 20 GE Plus missions + 6 PD stages HD/XBLA identical;
20 GE Plus missions in the N64 look identical. The map is laid across the pane in the pane's
own UVs, so its big glare sits mid-window on every pane (the release samples it as a
reflection); if the user finds the glare too much, darken or crop it there.

First fix:

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

- The far HD pane now uses Bean's reflection map (done, see above). Open only as taste:
  its bright glare blob shows mid-pane on every window (whole map in the pane's UVs),
  where the N64 picture is evenly dark. User to judge from facr-cmp-0.png.
- Vulkan not checked (offscreen driver makes no Vulkan window).

## Rig (outside the tree)

- run dir `/home/sdg/wt/f3glassn64-run` (copies of f3hdlevels-run's cache/mods/saves);
  build `/home/sdg/wt/f3glassn64-build`; binaries `f3glassn64-rig/pd.base` (1fc1832d8),
  `pd.fix` (first fix), `pd.refl` (with the map), `pd.dam` (first fix + a5355ec05).
- `views.sh STAGE TAG` (as f3hdlevels'; `BIN=` must be an absolute path - a relative one
  silently runs the last binary), `N64=1`, `GROUND=`, `ARGS=`, `EXTRASED=` (add the slider
  with `/^\[Mod\]/a GlassSeeThrough=50`: the key is not in the saved ini).
  Facility report camera: `VIEWS='-3607.3,-159,2295.4,190.5,-2.1,18' GROUND=-319`.
- `glass.py` (gdb `dumpglass`: tinted/plain glass and doors near the camera with
  xlu/opa/opacity/portal), `tour.py` via `SCRIPT=... tour.sh` (`PANES=` indices or
  `PANEPOS='x,z;...'` - the N64 look lists props in another order), `ab.sh` (`SAVE=save_sw_n64` for the N64 look) + `abdiff.py`,
  `cmp.py` (picture grids).
