# F3 Dam white markings + white pyramid (2026-09-26), branch fix/f3-dam-white-decals

Reports (/home/sdg/wt/f3-0926/), Dam 0x15, HD look:
- 20260925-234751-eaa59e4e: painted ground markings by the truck park drawn as
  opaque pure-white patches.
- 20260925-235806-a256acd5: "weird pyramid object" in the server room - the
  release's lamp light cone drawn near solid white, with a solid white skirt.

Earlier analysis: /home/sdg/wt/f3hdlevels/HANDOFF-f3.md sections 7 and 9.

## Cause

Both are Bean room triangles whose look comes from their vertex alpha: the
markings (tex 61, a white DXT5 dash picture) carry 0x80 (rooms 121/122) or
0x88 tan (room 111); the cone (tex 59, 32x32 white, alpha 251 -> 6) carries
0x82. Three things lost it:

1. bg.c's fog swap (g_GfxGroup01/05; also the no-transparency swap 06/07)
   rewrites writeLeaf()'s picture combiner (G_CC_TRILERP, G_CC_MODULATEIA2 =
   fc26a004 1f1093ff) to (G_CC_TRILERP, G_CC_CUSTOM_06 = fc26a004 1f1493ff):
   alpha = texel x ENV alpha instead of texel x SHADE alpha, because on the
   N64 the RSP writes fog into shade alpha. The PC renderer keeps fog apart
   (gfx_pc.cpp keeps vcn->a under G_FOG), so the vertex alpha was just dropped.
   Traced with a temporary gfx_sp_tri1 hook (DBGTRI): cycle type was 2-cycle
   throughout (the "one cycle" guess was wrong), vertex colours arrived intact
   (ffffff80 / ffffff82), combiner cycle-2 alpha read ENV. Only fog levels
   were hit: Silo and Depot (no fog) already drew the same kind of data faded.
2. The cone's picture has >1% texels at >= 0xf0, so it is not "soft": it sat
   in the opaque leaf as a cut-out, and the renderer's texture edge sets alpha
   to 1 above 0.19 - solid white wherever texel x 0x82 cleared the threshold.
3. The cone's v runs 0 (lamp) .. 1.32 (floor), wrapped: its foot sampled the
   lamp end again (alpha ~250) - the white skirt round the base.

## Fix (commit on this branch)

- gebeanstage.c gebeanStageFogRoom(..., fog): after bg.c's swap, puts the
  picture combiner back (1f1493ff -> 1f1093ff) in served HD rooms; the fog
  render-mode part only when fog is on. bg.c calls it from both swap branches
  (fog, and no-transparency with fog = false). N64 look untouched (HD rooms
  only, gated as before on built/roomData/xblaStageIsRelease()).
- triFades(): a triangle of a picture with alpha whose vertex alpha is under
  0xf0 (xblamesh.c's XBLAMESH_FADE_ALPHA rule for meshes) goes to the
  translucent leaf, not the cut-out path.
- clampCutouts(): a picture used by such faded triangles is clamped in t when
  its v starts at a repeat, runs past it by less than half, and its first and
  last rows' mean alpha differ by >= 0x80 (a fade that does not tile). New
  xblaTexImageEdgeAlpha() in xblatex.c. On all 20 missions this hits only Dam
  tex 59 (logged). A looser first version (any fade texture, range < 2)
  clamped Silo's tex 42/43/45 and erased Silo's catwalk rails - do not loosen.

## Verification

- Report cameras, GL and Vulkan (RX 580, Xvfb + MESA_VK_WSI_DEBUG=sw):
  markings now white/tan at about half opacity; cone a faint glow fading to the
  floor, no skirt. GL vs Vulkan: 11 and 5 pixels > 24 apart.
  Pictures: /home/sdg/wt/f3damwhite-pics/report-cams-gl.png,
  report-cams-vk.png (left base, right fix).
- 20-mission A/B at frame 400, both looks (rig ab2.sh): N64 look 20/20
  identical. HD: 17 identical, Dam (a shoreline grass fade), 0x64 and 0x6d
  (sub-threshold) differ.
- Every affected triangle was logged per level (temporary FADETRI log) and
  cameras aimed at the biggest groups (fvcmp3-<stage>.png). What changes, all
  vertex-alpha Bean triangles, all in fog levels:
  Dam terrain blend strips (tex 71, soft, alpha 0..ff) now fade instead of a
  hard edge; a grating's black overlay gone; markings half white.
  Jungle light shafts (tex 51) fade at their edges. Archives window shadows on
  the floor (tex 41) soft instead of solid black bars. Surface fence (tex 43)
  fades where its vertices say. Aztec floor markings and BAY-4 lettering, and
  Control's floor diamonds/stripes, at their 0x80 (half). Silo/Depot signs at
  0xb3/0x93 look the same before and after (no fog there: they already drew
  faded, so the fog levels now match them).

## Open

- Pictures without an alpha channel that carry vertex alpha (Dam tex 16/47,
  0/ff) still draw opaque, as before - untouched on purpose.
- The room palette (buildPalette(), 64 entries) can still merge an alpha level
  into a neighbour in a very colourful room; not seen here.
- Whether the release really shows Aztec's BAY-4 / Control's floor paint at
  half is inferred from the data (and Silo/Depot already drew so); a Xenia
  draw log would settle it.

## Rig (outside the tree)

- run dirs /home/sdg/wt/f3damwhite-run (A/B) and -run2 (views, logs); rig
  /home/sdg/wt/f3damwhite-rig: views.sh (RUN=, ARGS=--vulkan, VDRV=x11, per-view
  7th field = ground), ab2.sh (both looks, LOOKS=), mkviews.py (cameras from a
  FADETRI log), texprobe.py (gdb: per-texture DBGTRI over frames; needs a
  binary with the hook - pd.dbgtoggle). Binaries: pd.base0 (1fc1832d8), pd.fix3
  (this fix). Pictures: /home/sdg/wt/f3damwhite-pics.
