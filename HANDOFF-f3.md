# F3 20260925-234351-67928cc7: GoldenEye XBLA crosshair in the HD look (2026-09-26)

Branch `feat/ge-hd-bean-crosshair` (from dabs-mod 1fc1832d8). Not merged, pushed or deployed.

Report (Dam, GE Plus, HD look): "THE CROSSHAIR when aiming uses n64 sprite/texture on
xbla version." The user decided the HD look uses the release's (Bean's) crosshair.

## Status: done

Under the release's look (`gebeanGetEnabled() && xblaMeshGetEnabled()`, i.e. F6 on the
HD side), a GoldenEye weapon's sight on a GE Plus level draws the release's own
crosshair instead of the ROM's 32x32 IMAGE_CROSSHAIR1. The N64 look, PD weapons and PD
missions are untouched (`geHudRenderSight()` is only reached for `geHudOwnsWeapon()`).

## What the release uses

Seen in the release itself: the Bean rig (`.xbla-work/ge-bean/xenia/run.sh`), Dam via
Cheat Select Mission with All Guns, LT held to aim.

- `files/texture/bg/sight/default.rba`: 256x256 A8R8G8B8, the HD twin of GoldenEye's
  `bg/sight` (IMAGE_CROSSHAIR1). Same framing as the 32x32 (centred, bars to 6..249),
  a clean ring with a faint bevel lit from above. Like the menu cursor `texture/sight`,
  it is stored blue: its red is the blue channel. The CE overlay's copy is
  byte-identical.
- It is drawn only while aiming, over the same 32 GoldenEye units as the N64 one
  (about 90 px at 720p), centred on the screen. The release's gameplay is 16:9. At
  4:3 we keep the same 32 units of the 240 high frame.
- **No separate aim reticle and no scope overlay.** The zoomed sniper rifle shows the
  same crosshair. `bg/sightcorner` (a quarter arc) and `test/sight_b_and_e_*` (bevel
  experiments; `_together` equals `bg/sight`) are not drawn in these views.
- Blend, measured on flat walls in the release's frames: the background keeps 0.40
  of itself under the bars, and the bars' red is about 173. So the port uses env alpha
  0x99 and shade 0xad. GoldenEye's own values are white at 0x6e. Port against release
  on the same kind of wall: bar (111, 8, 8) against (111, 9, 10).

## Commits / files

- `port/src/gehud.c`: `geHudRenderSight()` takes `geFolderMenuPicture("bg/sight")`
  when it exists and draws it as a stand-in config (nominal 32 texels, clamp) at
  SIGHT_HD_SHADE/SIGHT_HD_ALPHA. `hudImage()` gained a `shade` argument, and the old
  callers pass 255.
- `port/src/gefolder.c`: `geFolderMenuPicture()` also swaps red/blue for `bg/sight`
  and turns its rows over, so it draws upright with a positive t step. Drawing it
  with hudImage's flipped rectangle put it one nominal texel (3 px at 720p) high.
- `port/src/gebean.c`: archive unpack also takes `files/texture/bg/sight/`, and the
  marker is now `.extracted12`, so an archive cache unpacks once more.

## Verification

Rig: `~/wt/f3crosshair-run` (`run.sh TAG probes/sight.py`; env HD=0/1, W/H,
WEAPON=WEAPON_GE_SNIPERRIFLE ZOOM=8, BIN). Release frames are in `release/`, and
`compare_release_port.png` shows, left to right: release PP7, release sniper, port
PP7, port sniper, and the N64 look.

- HD 16:9 1280x720 (`shots_v169`): bbox x 596-686 y 316-405. The release's is
  596-686 / 316-405.
- HD 4:3 1024x768 (`shots_v43`): same 32 units, centred (465-561 / 337-433).
- The zoomed sniper uses the same crosshair, as the release does.
- N64 look, new binary against a baseline built from the same tree without the change
  (`pd-base.x86_64`): pixel-identical on all 3 frames (`shots_vn64` vs `shots_vn64b`).
- Bevel orientation was checked with a temporary 200-unit test build: the pale edge
  is along the top, as in the release's frames.

## Open

- Only the loose-folder release was run. The archive path (7z/zip) and its new
  `.extracted12` unpack were not exercised.
- `bg/sightcorner` is unused. Nothing seen in the release drew it (solo Dam: PP7 and
  sniper, aiming or not). If it matters it might be MP-only.
- The probe forces `sighton`, and this save shows the sight when not aiming too
  (Always Show Target). That behaviour is unchanged. The release shows nothing when
  not aiming.
- Rig trap: `run.sh` deletes `data/save_<tag>`, so never use a tag named like a base
  save (`hdbase`).
