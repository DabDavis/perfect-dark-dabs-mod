#ifndef _IN_GEHUD_H
#define _IN_GEHUD_H

#include <ultra64.h>
#include <PR/ultratypes.h>

/**
 * GoldenEye's own HUD over GE Plus's missions and arenas (port/src/gehud.c):
 * the magazine and reserve either side of the ammunition's picture, its sight,
 * the health and armour gauges, the two kinds of message and the countdown,
 * each laid out on GoldenEye's 320x240 frame with its own two fonts and the
 * pictures out of the player's ROM.
 *
 * One thing stays Perfect Dark's: a Perfect Dark weapon in the player's hand
 * keeps Perfect Dark's own ammunition display and sight, since its functions,
 * its second ammunition and its sights have nothing of GoldenEye's to be drawn
 * with.
 */

// what a level of the remake needs of the conversion, or nothing (setup.c)
void geHudStageStart(s32 stagenum);

// The current level draws GoldenEye's HUD at all.
s32 geHudActive(void);

// And what is in the current player's hand is drawn GoldenEye's way: the HUD is
// on and the weapon is not one of Perfect Dark's own.
s32 geHudOwnsWeapon(void);

// Perfect Dark's own text (the weapon wheel's) in GoldenEye's font, measured
// and placed in Perfect Dark's units; for a level where geHudActive()
void geHudTextMeasure(const char *text, s32 *width, s32 *height);
Gfx *geHudText(Gfx *gdl, const char *text, s32 x, s32 y, u32 colour);

// A string of GoldenEye's pickups' bank (LpropobjE) by its slot, or NULL.
const char *geHudPropobjString(s32 slot);

// generate_ammo_total_microcode(), for bgunDrawHud()
Gfx *geHudRenderAmmo(Gfx *gdl);

// the watch's mission status: a GoldenEye gun's rounds, and their display
// (gunDrawWatchAmmoDisplay()) on the watch's text frame
s32 geHudWatchAmmo(s32 weaponnum, s32 *mag, s32 *reserve);
Gfx *geHudRenderWatchAmmo(Gfx *gdl, s32 weaponnum, s32 mag, s32 reserve, f32 ox, f32 oy, f32 sx, f32 sy);

// gunDrawSight(), at Perfect Dark's own crosshair position
s32 geHudHasHdSight(void);
Gfx *geHudRenderSight(Gfx *gdl, f32 x, f32 y);

// "GE Plus: Crosshair When Not Aiming" (Mod.GePlusSightAlways)
s32 geHudGetSightAlways(void);
void geHudSetSightAlways(s32 on);

// bondviewRenderGaugeBars(), for playerRenderHealthBar()
Gfx *geHudRenderGauges(Gfx *gdl);

/**
 * hudmsgBottomRender() and the top message (sub_GAME_7F08AAE8()): one of
 * Perfect Dark's messages drawn as GoldenEye draws that kind. `row` counts the
 * bottom messages drawn this frame, which stack upwards where Perfect Dark
 * would have had them in different corners; start it at 0.
 */
Gfx *geHudRenderMessage(Gfx *gdl, const char *text, s32 top, s32 intro, s32 *row);

// countdownTimerRender()
Gfx *geHudRenderCountdown(Gfx *gdl, s32 mins, s32 secs, s32 ms);

/**
 * display_red_blue_on_radar(): the disc, then each blip Perfect Dark's radar
 * decides on at `dx`, `dy` from its middle in GoldenEye's units (`plain` for
 * Perfect Dark's ordinary radar colour, which becomes GoldenEye's yellow and,
 * for the player, white; `atrim` for one held at the edge), then the state put
 * back. radar.c calls them; nothing of GoldenEye's is asked who is on it.
 */
Gfx *geHudRadarBegin(Gfx *gdl);
Gfx *geHudRadarDot(Gfx *gdl, s32 self, s32 dx, s32 dy, u32 rgb, s32 plain, s32 atrim);
Gfx *geHudRadarEnd(Gfx *gdl);

// How long GoldenEye shows a message of that kind, in sixtieths
s32 geHudMessageDuration(s32 top);

#endif
