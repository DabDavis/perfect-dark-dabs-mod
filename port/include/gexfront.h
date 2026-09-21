#ifndef _IN_GEXFRONT_H
#define _IN_GEXFRONT_H

#include <ultra64.h>
#include <stddef.h>
#include <PR/ultratypes.h>

/**
 * GE Plus's menus as GoldenEye's own folder screens (port/src/gexfront.c):
 * the folder model, GoldenEye's fonts and strings and its crosshair cursor, all
 * from the conversion of the player's ROM (menu/ and the models block of
 * mods/GoldenEye Arenas).
 *
 * While open it replaces the Perfect Menu's ticking and drawing: menuTick()
 * and menuRender() hand over to it and return.
 */

// Opens the folder at its mode select. False when the conversion's menu files
// are not there, and the caller shows Perfect Dark's GE Plus dialog instead.
s32 gexFrontOpen(void);
s32 gexFrontIsActive(void);
// Opens the folder at Multiplayer Options after a GE Plus match. False when
// it cannot be drawn, and the caller keeps Perfect Dark's Combat Simulator.
s32 gexFrontOpenAfterMatch(void);
// Opens the folder at the Cinema page after one of GoldenEye's opening camera
// sequences has played (gecinema.c), with that mission under the cursor.
s32 gexFrontOpenAfterCinema(s32 mission);
// Inside GE Plus: from the folder opening until the player backs out of its
// mode select to the Perfect Menu, through whatever levels it starts.
s32 gexFrontIsInside(void);
// Leaves the level for the Institute the way a match does, for the folder to
// open over the Perfect Menu there (menutick.c) - never by way of the title.
void gexFrontGoBack(void);
// A solo mission's endscreen has closed for good. True when the folder started
// the mission: the ending is taken, and comes back to GE Plus's own main menu.
s32 gexFrontMissionEnded(void);
// A solo mission is ending (mainEndStage()). True when the folder started it:
// how it went is kept, Perfect Dark's endscreen is not put up, and the level is
// left for the folder's own report and statistics pages, as GoldenEye's is.
s32 gexFrontMissionReport(void);
s32 gexFrontWantsMain(void);
s32 gexFrontOpenAfterMission(void);
void gexFrontTick(void);
Gfx *gexFrontRender(Gfx *gdl);

// GoldenEye's folders theme while open (menuChooseMusic() asks), else -1
s32 gexFrontMusic(void);

/**
 * The conversion's menu files (the two fonts, the title screen's strings and
 * the folder model) loaded without opening the folder, for the intro
 * (port/src/geintro.c), which runs before it and draws GoldenEye's own text
 * with the same fonts. False when they are not there. Everything loaded stays
 * loaded until the folder closes.
 */
s32 gexFrontLoadShared(void);

/**
 * The same two fonts and strings without the folder model, for the watch
 * (gewatch.c), which draws GoldenEye's own text in a level where the folder
 * is closed. Idempotent, and never frees what the folder is holding.
 */
s32 gexFrontLoadText(void);

// The mounted mod the conversion's files come from, or -1
s32 gexFrontModDir(void);

/**
 * A mission's briefing file, the text bank that file indexes and LtitleE's
 * name for it - GoldenEye's mission folder's own row. False for a number that
 * is not one of its twenty. The watch's briefing screen reads the same three.
 */
s32 gexFrontMissionFiles(s32 mission, const char **brief, const char **lang, s32 *nameid);

// A string of LtitleE by its index, "" when there is none
const char *gexFrontTitleString(s32 index);

// The state GoldenEye's text renderer draws under (microcode_constructor())
Gfx *gexFrontTextSetup(Gfx *gdl);

/**
 * GoldenEye's own text, in its Zurich Bold (gothic 0) or Bank Gothic (1), at a
 * position on the 440x330 frame both games lay their screens out on.
 * gexFrontTextMeasure() gives what it will take, unscaled.
 */
Gfx *gexFrontTextPrint(Gfx *gdl, s32 gothic, s32 x, s32 y, const char *text, u32 colour);

/**
 * The frame that text and rectangles are laid out on: GoldenEye's own frame in
 * its own units, and the box on the screen it is fitted into - its height
 * fills the box and its x is centred. The menus leave it at their 440x330 over
 * the whole window; the watch sets GoldenEye's in-game 320x240 over the
 * player's viewport and puts it back afterwards.
 */
void gexFrontTextFrame(f32 gew, f32 geh, s32 left, s32 top, s32 width, s32 height);
void gexFrontTextFrameDefault(void);
void gexFrontTextMeasure(s32 gothic, const char *text, s32 *width, s32 *height);

// A filled rectangle on the same frame, in the same colour word
Gfx *gexFrontFillRect(Gfx *gdl, s32 x1, s32 y1, s32 x2, s32 y2, u32 colour);

// Text broken into lines no wider than `width`, in GoldenEye's own units
void gexFrontTextWrap(s32 gothic, const char *text, char *out, size_t len, s32 width);

#endif
