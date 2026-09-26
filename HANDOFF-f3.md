# F3 20260925-234249 Dam opening gate is a blue void (HD) - branch fix/f3-dam-gate-void

Status: FIXED + VERIFIED, fix commit c55258431 (src/game/env.c). Not merged, pushed or deployed.

## Cause

The gate (door prop, modelnum 690, at 16792 13313 25803, room 135) was never flagged on
screen in HD (prop flags 0x4, N64 look 0xc6). In func0f08e8ac() every step passed but
envIsPosInDrawDistance(), the HD look's draw-distance test for objects: it took the prop's
depth as the dot product with `cam_look`. `cam_look` is a unit vector in play but the
look-at offset in a cutscene (length ~2010 in Dam's opening at frame 927, 190-450 in other
openings' swirls), so the gate 2000 units away measured ~4 million deep, past the release's
fog end (80000), and was culled. The blue is the sky behind where it stands.

## Fix

envIsPosInDrawDistance() divides the depth by the look's length (a zero-length look draws).
Nothing else calls it; the N64 look never reaches it (gebeanStageFog() is 0 there).

## Verification (rig ~/wt/f3damgate-rig, pics ~/wt/f3damgate-pics)

- Dam opening, frames 875 + 925 (`FRAMES=875,925 rungdb.sh gateshot.py`): gate drawn,
  prop flags 0xc6 as in the N64 look (gate-cmp0.png, gate-cmp1.png: before / after /
  N64). N64 look before vs after: pixel-identical.
- Gameplay frame 400 (`ab.sh`), HD look, Facility 0x63, Bunker 0x6f, Dam 0x15,
  Runway 0x5e, Archives 0x65: pixel-identical (play's look is a unit).
- Opening swirls (frames 400/460/520): Facility and Bunker identical; Runway changed for
  the better - its shutter door was also culled in HD (the outside showed through the
  doorway at frame 520) and now draws closed, as in the N64 look (swirl-0x5e-cmp.png,
  swirl-0x5e-n64.png).

## Open

- Any other port code reading `cam_look` as a unit during cutscenes: roomsheen.c's shift
  (look along/across) and trace.c print it raw; gebeanstage.c normalises its own copy.
  Not investigated further (no report).
- The other items from the f3hdlevels pass (Facility console, glass, Runway door gaps,
  Dam markings/sniper fog/lamp cone) remain as in that branch's HANDOFF.
