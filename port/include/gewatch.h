#ifndef _IN_GEWATCH_H
#define _IN_GEWATCH_H

#include <PR/ultratypes.h>

/**
 * GoldenEye's watch, as GE Plus's pause (port/src/gewatch.c).
 *
 * In a GE Plus level, Start lowers the gun, tilts the view down to the wrist,
 * brings GoldenEye's own left arm up with the watch on it and zooms into its
 * face, where GoldenEye's five screens are - mission status, inventory,
 * control, options and the briefing - drawn in its own fonts. Start or B backs
 * out of it the same way round.
 *
 * Everything comes out of the conversion of the player's ROM (geconvert.c):
 * the arm is GoldenEye's own suit_lf_hand model, the animation that raises it
 * is its `bond_watch`, and the text is its LoptionsE, LgunE and LpropobjE
 * banks with the folder screens' fonts (gexFrontLoadText()).
 */

// At a stage load: loads what the watch needs for a GE Plus level and lets go
// of it for any other stage.
void geWatchStageStart(s32 stagenum);

/**
 * Start pressed in a level: opens the watch, or closes the one that is open.
 * False when this level has no watch, and the caller pauses Perfect Dark's own
 * way (playerPause()).
 */
s32 geWatchPause(void);

// Anywhere but closed: the watch owns the pad and Perfect Dark's pause is off
s32 geWatchIsOpen(void);

// and whether the view model is out of the player's hands while it is
s32 geWatchHidesGun(void);

// Every frame of a level, from playerTick(), before or after the pause menu's
void geWatchTick(void);

// Over the player's view, from playerRenderHud()
Gfx *geWatchRender(Gfx *gdl);

/**
 * The watch's health and armour gauges, which are GoldenEye's HUD's too
 * (gehud.c): 46 vertices and their colours for `side` 1 (armour, blue, the
 * right) or -1 (health, red, the left) lit as far as `value`, and the list that
 * joins them into a bar under whatever matrices are loaded.
 */
void geWatchGaugeVertices(Vtx *v, Col *c, s32 side, f32 value);
Gfx *geWatchDrawGauge(Gfx *gdl, Vtx *v, Col *c);

#endif
