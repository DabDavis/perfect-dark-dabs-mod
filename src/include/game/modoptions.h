#ifndef _IN_GAME_MODOPTIONS_H
#define _IN_GAME_MODOPTIONS_H

#include <ultra64.h>
#include "types.h"

/**
 * Dab's Mod Options - everything this fork added that a player should be able
 * to turn off.
 *
 * These are global settings kept in pd.ini, not arena rules kept in
 * mpsetup.options, which is what Jump and Start Armed used to be. An arena rule
 * only exists while a Combat Sim match does, and half of what this fork added -
 * the jump, the roll, the melee combos, the body that flinches when it is shot
 * - is worth having in a solo mission too. The bits they used to live in are
 * still reserved in constants.h so that a setup file saved by an older build is
 * not read back as something else.
 */

#define MODWHO_EVERYONE    0
#define MODWHO_PLAYERSONLY 1

#define MODROLL_OFF         0
#define MODROLL_EVERYONE    1
#define MODROLL_PLAYERSONLY 2

/**
 * The third person camera: how far back it sits, how far to one side, how much
 * room it keeps from a wall, and the point below which it gives up and returns
 * to the eye. The stock values and what they mean are in constants.h.
 *
 * Camera Sideways is the over-the-shoulder offset, in units right of the eye
 * and negative for the left. It moves the camera and nothing else: the picture
 * does not turn to compensate, so Joanna slides across the screen and the view
 * past her opens up on the far side. Zero is the fork's own behaviour up to
 * now, which is why it is the default.
 *
 * The three of them are one offset from the eye and one trace clears it, so a
 * sideways offset costs nothing that the pull-back was not already spending.
 * playerPullBackCamera() has the rest.
 */

/**
 * Bodies: how many are left lying where they fell, and how long each one lies
 * there.
 *
 * The cost of the cap is paid at stage load, not when the bodies appear: a
 * simulant's body needs a chr of its own to stay behind in, so the cap is
 * reserved in chr, model, anim and prop slots by setupLoadFiles() whether the
 * match fills it or not - roughly two and a half kilobytes each.
 *
 * The cap is what is asked for rather than what is granted. modBodiesSetReserve()
 * takes what MEMPOOL_STAGE can carry at stage load and no more, and past that
 * point bodies are simply not kept and fade the way they always did. So a small
 * Game.MemorySize costs bodies, never a match. 64MB carries the full five
 * hundred alongside eighty simulants; a pd.ini written by an older build still
 * says 16 and wants raising by hand.
 *
 * The cap that costs nothing to raise still costs frames to fill. Five hundred
 * bodies on Temple runs at 16fps where sixty is normal - they are eighty
 * simulants' worth of models, drawn and ticked - so the default is the number
 * that leaves the game feeling like itself and the maximum is there for anyone
 * who wants to see what a massacre looks like.
 *
 * MODBODYTIME_OFF means a body lies there until the cap pushes it out, which on
 * a cap this size is most of a match.
 */
#define MODBODIES_OFF     0
#define MODBODIES_DEFAULT 128
#define MODBODIES_MAX     500
#define MODBODYTIME_OFF   0
#define MODBODYTIME_MAX   600

/**
 * How many kept bodies may be drawn in one frame, nearest first.
 *
 * Keeping a body and drawing it are separate costs, and only one of them is
 * large. Measured on Temple with two hundred props in view: the bodies cost
 * 49ms of a 68ms frame to draw, against 19ms for everything else including
 * their own ticking. Their models have no cheaper level of detail to fall back
 * on - scaling the LOD distance four times over moved 16fps to 18 - so the only
 * thing that buys frames is submitting fewer of them.
 *
 * The ones dropped are the furthest away, which are the smallest on screen, and
 * the choice is remade every frame from the draw order that already exists.
 * MODBODIESDRAWN_ALL draws all of them.
 */
#define MODBODIESDRAWN_ALL 0
#define MODBODIESDRAWN_MAX MODBODIES_MAX

/**
 * Guards Alerted!: the alarm never stops, and guards keep coming.
 *
 * In a stock mission the alarm is something a guard raises when the player is
 * seen or heard, and it has two effects: every guard who can hear it goes
 * alert and comes looking, and on a few stages the mission script spawns a
 * handful of reinforcements. Both stop after thirty seconds. This setting is
 * the alarm as a permanent condition, on every stage - solo and Combat
 * Simulator - with reinforcements the port spawns itself, so a stage whose
 * script never spawned any gets them too. The Institute is left out: it is
 * the hub under the menus and has no alarm of its own, and troopers spawned
 * there shot at the player behind the Perfect Menu (2026-09-07). How many
 * are on their feet at once and how quickly the next one arrives are
 * settings of their own; see modalarm.c.
 *
 * The siren is separate. It is the sound of the thing, but thirty seconds of
 * it is one matter and a whole match of it is another, so it can be turned off
 * on its own while the guards keep coming.
 */
#define MODALARM_OFF 0
#define MODALARM_ON  1

/**
 * How many alerted guards may be on their feet at once. The ceiling is the
 * simulant count's, and for the same reason: each one is a chr, a model, an
 * animation and a prop reserved at stage load, and eighty is where a match
 * stops feeling like a match.
 */
#define MODALARM_GUARDS_MIN     1
#define MODALARM_GUARDS_DEFAULT 6
#define MODALARM_GUARDS_MAX     80

/**
 * How fast they come, in guards per ten seconds. One is a guard every ten
 * seconds, which keeps a player moving; the default is one every five, about
 * what the Villa's four-in-two-minutes feels like when it never stops; fifty
 * is one every fifth of a second, which puts the whole eighty on their feet
 * inside twenty seconds - a swarm, for anyone who asks for one.
 */
#define MODALARM_SPEED_MIN     1
#define MODALARM_SPEED_DEFAULT 2
#define MODALARM_SPEED_MAX     50

/**
 * What a reinforcement carries. Stage weapons is the match's own six slots,
 * or the mission side arms outside a match. Random rolls the whole table of
 * guns a guard can hold, the way Start Armed's Random does for a player.
 */
#define MODALARM_WEAPONS_STAGE  0
#define MODALARM_WEAPONS_RANDOM 1

/**
 * Akimbo: whoever spawns armed spawns with two, whatever the weapon - a
 * rifle or a rocket launcher in each hand as readily as a pistol. Applies
 * to what Start Armed hands a player or simulant, to the gun a mission
 * starts the player with, and to what a Guards Alerted! guard is given,
 * separately or together; for players and simulants it also makes a second
 * one picked up a dual, which is what the stock "dual wield all guns" cheat
 * does. Under Start Armed's Random, or Guard Weapons' Random, the left
 * hand gets a roll of its own, so the pair is two different guns.
 */
#define MODAKIMBO_OFF            0
#define MODAKIMBO_EVERYONE       1
#define MODAKIMBO_PLAYERSANDSIMS 2
#define MODAKIMBO_GUARDS         3
#define MODAKIMBO_MAX            MODAKIMBO_GUARDS

/**
 * Akimbo Triggers: on a controller, the left trigger fires the left hand and
 * the right trigger the right hand, instead of one trigger alternating them.
 * Aim mode moves to the left bumper to make room, and the radial menu the
 * bumper held moves to D-pad up. The binds are rewritten when it is turned
 * on or off - see inputApplyAkimboTriggers() - and the left hand's trigger
 * is a key of its own, "Fire Left", that a keyboard can bind too.
 */

/**
 * Camera Tilt: the view moves the way the head would, Quake's way. Strafing
 * rolls the picture a few degrees into the direction of travel, looking up
 * or down leans the camera a little further in that direction while the
 * view is still moving, and walking bobs the eye up and down with the
 * steps, higher the faster the run, so a look and a sidestep have some
 * weight to them rather than sliding the world past a fixed eye.
 *
 * Only the picture moves. The camera's own basis vectors and position, and
 * everything that aims, walks or traces along them, are untouched - the
 * roll is about the centre of the screen, so the crosshair points where it
 * always did, the lean is a degree or two that lasts only as long as the
 * look does, and the bob is a few centimetres of eye height.
 *
 * It is the classic head bob's worst habit, exaggerated motion at a retro
 * field of view, that gives people headaches, so the setting is how much
 * rather than whether. Normal is two degrees of roll at a full sidestep and
 * half of Quake's bob; Light is half that again, and Heavy is Quake as
 * shipped.
 */
#define MODTILT_OFF    0
#define MODTILT_LIGHT  1
#define MODTILT_NORMAL 2
#define MODTILT_HEAVY  3
#define MODTILT_MAX    MODTILT_HEAVY

/**
 * Gun Sway With Tilt: the gun's own step motion scaled up alongside the
 * bob, so the two move as one body. The gun is drawn in screen space and
 * rides with the picture, so at Heavy the world bobs under a steady gun,
 * which reads as floating; with this on the gun swings with the walk by
 * the same factor - one and a half times stock at Light, twice at Normal,
 * three times at Heavy. Nothing without Camera Tilt itself, and the idle
 * breathing is left at stock either way.
 */

/**
 * Increase Poly Models (modelsmoothing in the code, Mod.ModelSmoothing in
 * pd.ini): the characters, weapons and props drawn with more triangles than
 * they were built from, each one bent into a curved patch so that a
 * six-sided arm reads as a round one.
 *
 * The models were made for a console that could afford a few hundred
 * triangles per character, and on a monitor every edge of that budget shows.
 * There is no mesh to refine at runtime - the renderer sees a display list,
 * which is triangles one at a time with no idea of their neighbours - so the
 * renderer uses curved PN triangles: each corner's normal is taken as the
 * surface's true direction there, the flat triangle is bent into a cubic
 * patch that honours all three, and the patch is drawn as a grid of smaller
 * triangles, each lit by the same lights with a normal blended across the
 * patch. Two triangles that share an edge bend it the same way, so the
 * surface stays sealed.
 *
 * Which normal a corner bends with is decided once per model at load, from
 * the mesh (port/src/modelsmooth.c): the average of the faces around the
 * corner that this face joins smoothly, so a box top whose neighbours all
 * meet it at right angles stays flat and keeps its edges, while a barrel's
 * sides round together and stop at the rim. Without that the technique
 * pillows every flat face whose normals were averaged across its edges,
 * which is most of them. The model's own normals still shade it.
 *
 * Only geometry that came with normals qualifies, which is the lit things;
 * rooms are coloured per vertex and are left alone, and the odd room
 * surface drawn with lighting on for its reflection (the Institute lobby's
 * glass table top) is drawn flat, as no mesh pass has read it. On at any setting it
 * also holds Model LOD off, below. The level is the same at every distance
 * - this is asked for as a look - so the cost is on the GPU and in the
 * renderer's vertex loop for everything lit on screen: Light is four
 * triangles per triangle, Normal nine, Heavy sixteen.
 *
 * An amount as well as a count: Light pulls the new vertices half way to
 * the curve, Normal three quarters, Heavy all the way.
 */
#define MODSMOOTH_OFF    0
#define MODSMOOTH_LIGHT  1
#define MODSMOOTH_NORMAL 2
#define MODSMOOTH_HEAVY  3
#define MODSMOOTH_MAX    MODSMOOTH_HEAVY

/**
 * Model LOD: whether the game swaps in its low-detail bodies past a few
 * metres, as it always did. Off, every distance node keeps its near model,
 * which is the whole model at any range. Increase Poly Models forces this
 * off while it is on - a rounded figure that turns into the flat far one
 * across the room is not the look that was asked for - and the menu greys
 * the checkbox out to say so; the value kept here is what comes back when
 * the poly setting is turned off again.
 */

/**
 * Smooth Text: the font's glyphs scaled up four times over on their way to
 * the GPU, with the letter's edge sharpened as they go (port/fast3d/
 * gfx_texscale.cpp). The fonts were drawn one texel to one pixel of a
 * 320x240 screen, and on a monitor each glyph is a magnified handful of
 * blurred squares; this reads them as the shapes they are and draws a clean
 * edge instead. The font's own anti-aliasing texels are kept. A pack's
 * replacement glyph is left alone, being already whatever size its author
 * chose. A fix rather than a look, so on by default.
 */

/**
 * Enhance Textures: the game's textures scaled up two or four times over as
 * they are uploaded, resampled through a cubic curve rather than the GPU's
 * straight-line blend, so a 32-texel wall reads as a surface rather than a
 * grid of soft blobs. Nothing is invented: it is a better guess at what
 * lies between the texels than the bilinear filter's. A texture that is
 * itself a one-texel pattern - a halftone portrait, a screen of text-sized
 * dashes, a blind of single lines - was drawn to be seen through the blur
 * and is left as it came (port/fast3d/gfx_texscale.cpp). A texture pack's
 * images, being bigger already, are not touched. The cost is at upload,
 * once per texture, and in GPU memory: four times over is sixteen times
 * the texels.
 */
#define MODENHANCE_OFF  0
#define MODENHANCE_2X   1
#define MODENHANCE_4X   2
#define MODENHANCE_8X   3
#define MODENHANCE_MAX  MODENHANCE_8X

/**
 * Vivid Colours: the finished frame's saturation and contrast turned up, as
 * the last thing before it is shown (and before a screenshot or a recording
 * reads it, so those match the screen). The game's palette was made for a
 * CRT through a composite cable, which crushed its blacks and bled its
 * colours into each other; on a flat panel it is flat and grey. Light is a
 * touch of both, Normal what a television's picture presets would call
 * vivid, Heavy more than that.
 */
#define MODVIVID_OFF     0
#define MODVIVID_LIGHT   1
#define MODVIVID_NORMAL  2
#define MODVIVID_HEAVY   3
#define MODVIVID_MAX     MODVIVID_HEAVY

/**
 * Black Level: a floor taken off the finished frame's blacks, with the rest
 * stretched back to full range so only the bottom moves. The washed-out
 * look is mostly a raised pedestal - black shown as dark grey - and Vivid
 * Colours' contrast can only take it off by crushing the shadows above it,
 * which is why this is its own setting. Light takes two percent off, Normal
 * four, Heavy seven. Same pass as Vivid Colours, applied before it.
 */
#define MODBLACK_OFF     0
#define MODBLACK_LIGHT   1
#define MODBLACK_NORMAL  2
#define MODBLACK_HEAVY   3
#define MODBLACK_MAX     MODBLACK_HEAVY

/**
 * Mission Respawn: a death in a mission is a new life where the player
 * fell - full health, the inventory and the guns they had, the mission's
 * clocks and objectives untouched - rather than Mission Failed. Lives is
 * how many in all, counting the first: with five, the fifth death ends the
 * mission as it always did. Unlimited is the first setting, because the
 * point of the option is to keep playing. A mission only; co-operative and
 * counter-operative keep their own rules for a dead player. See
 * modrespawn.c.
 */
#define MODLIVES_UNLIMITED 0
#define MODLIVES_STEP      5
#define MODLIVES_MAX       50

struct modoptions {
	s32 jumpheight;  // 0 for off, else the height multiplier, up to JUMPHEIGHT_MAX
	s32 jumpwho;     // MODWHO_*: whether simulants jump too
	s32 roll;        // MODROLL_*
	s32 melee;       // the punch and kick combo
	s32 flinch;      // the body twitching where a shot landed
	s32 spawnweapon; // SPAWNWEAPON_*: what everyone spawns holding in an arena
	s32 spawnweaponwho; // MODWHO_*: whether simulants spawn armed too
	f32 camdist;     // third person camera, units behind the eye
	f32 camclearance;// how far short of a wall it stops
	f32 cammindist;  // below which it is not worth leaving the eye at all
	f32 camside;     // units to one side of the eye, negative for the left
	s32 bodies;      // how many bodies are left lying around, 0 for off
	s32 bodytime;    // seconds one lies there, 0 for until the cap takes it
	s32 bodiesdrawn; // how many may be drawn at once, 0 for all of them
	s32 guardsalerted; // MODALARM_ON: the alarm as a permanent condition
	s32 alertedguards; // how many reinforcements may be up at once
	s32 guardspawnspeed; // how fast they come, in guards per ten seconds
	s32 guardweapons; // MODALARM_WEAPONS_*: what they carry
	s32 akimbo;      // MODAKIMBO_*: who spawns holding two
	s32 akimbotriggers; // a trigger per hand on a controller, aim on the left bumper
	s32 explosionshake; // whether an explosion shakes the screen at all
	s32 codaiming;   // aim down the sights: gun to centre, a little zoom, keep moving
	s32 codaimlock;  // ... with the crosshair held in the centre and the aim stick turning the view
	s32 alarmsound;  // whether the siren plays while the alarm is on
	s32 cleantext;   // outlined text drawn with a halo, not the font's filled cell
	s32 cameratilt;  // MODTILT_*: how far the view leans into a sidestep or a look, and bobs with a step
	s32 gunsway;     // the gun's step motion scaled up with the bob
	s32 modelsmoothing; // MODSMOOTH_*: Increase Poly Models, lit triangles drawn as curved patches
	s32 modellod;    // the game's distance models; Increase Poly Models pushes the switch out
	s32 smoothtext;  // font glyphs scaled up with their edges sharpened
	s32 enhancetextures; // MODENHANCE_*: the game's textures scaled up on upload
	s32 vividcolours; // MODVIVID_*: the frame's saturation and contrast turned up
	s32 blacklevel;  // MODBLACK_*: the floor taken off the frame's blacks
	s32 missionrespawn; // a death in a mission is a new life where the player fell
	s32 missionlives; // how many in all, MODLIVES_UNLIMITED or a multiple of MODLIVES_STEP
};

extern struct modoptions g_ModOptions;

s32 modGetJumpHeight(void);
bool modIsJumpEnabled(void);
bool modCanChrJump(void);
f32 modGetJumpImpulse(void);
f32 modGetJumpApex(void);
bool modCanPlayerRoll(void);
bool modCanChrRoll(void);
bool modIsMeleeComboEnabled(void);
bool modIsFlinchEnabled(void);
s32 modGetSpawnWeapon(void);
bool modCanChrSpawnArmed(void);
s32 modGetBodiesKept(void);
s32 modGetBodyTime(void);
s32 modGetBodiesDrawn(void);
bool modKeepsBodies(void);
bool modIsGuardsAlertedOn(void);
s32 modGetAlertedGuards(void);
s32 modGetGuardSpawnSpeed(void);
s32 modGetGuardWeapons(void);
bool modIsAkimboForPlayers(void);
bool modIsAkimboForGuards(void);
bool modIsAkimboTriggersOn(void);
bool modIsExplosionShakeOn(void);
bool modIsCodAimingOn(void);
bool modIsCodAimLockOn(void);
bool modCanAkimbo(s32 weaponnum);
bool modIsWeaponAGun(s32 weaponnum);
bool modIsAlarmSoundEnabled(void);
bool modIsCleanTextOn(void);
f32 modGetCameraTiltScale(void);
f32 modGetGunSwayScale(void);
s32 modGetModelSmoothingLevel(void);
f32 modGetModelSmoothingAmount(void);
bool modIsModelSmoothingOn(void);
bool modIsModelLodOn(void);
#ifndef PLATFORM_N64
f32 modGetModelLodDistanceScale(void);
#endif
s32 modGetSmoothTextScale(void);
s32 modGetTextureEnhanceScale(void);
f32 modGetVividSaturation(void);
f32 modGetVividContrast(void);
f32 modGetBlackLevelLift(void);
bool modIsMissionRespawnOn(void);
s32 modGetMissionLives(void);

#endif
