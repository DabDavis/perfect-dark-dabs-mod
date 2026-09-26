/**
 * GoldenEye's HUD over GE Plus's missions and arenas.
 *
 * Everything here is GoldenEye's own, drawn from its own numbers:
 *
 * - the ammunition (gunfire.c's generate_ammo_total_microcode()): the magazine,
 *   the ammunition's own picture out of the ROM's global image bank and the
 *   reserve, at the bottom right, and again mirrored at the bottom left for a
 *   gun in the left hand;
 * - the sight (gunDrawSight()): its one 32x32 crosshair, the folder screens'
 *   cursor, at 0x6e of 255 - or under the release's look the release's own
 *   (texture/bg/sight), drawn the release's way;
 * - the health and armour gauges (bondviewRenderGaugeBars()): the watch's two
 *   arcs either side of the view, under its own orthographic frame;
 * - the messages (hudmsgBottomRender() and sub_GAME_7F08AAE8()): Bank Gothic
 *   outlined in grey at the bottom left, Zurich Bold over a dark band across
 *   the top;
 * - the countdown (propobj.c's countdownTimerRender()).
 *
 * All of it is laid out on GoldenEye's in-game 320x240 frame. A window wider
 * than 4:3 has more than 320 of those units across it, and since every one of
 * these is placed from an edge of the view - 59 from the right, 30 from the
 * left - they go out to the edges with it rather than staying in a 4:3 middle.
 *
 * Perfect Dark keeps the state: what is in the hands and how much, when the
 * health shows, which messages are up and for how long. Only the drawing is
 * swapped, at the point each of Perfect Dark's own is drawn. A Perfect Dark
 * weapon in the hand keeps Perfect Dark's ammunition display and sight.
 */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <ultra64.h>
#include "constants.h"
#include "types.h"
#include "bss.h"
#include "data.h"
#include "gbiex.h"
#include "mod.h"
#include "modloader.h"
#include "system.h"
#include "video.h"
#include "gehud.h"
#include "gefolder.h"
#include "gewatch.h"
#include "gexfront.h"
#include "game/bondgun.h"
#include "game/camera.h"
#include "game/gfxmemory.h"
#include "game/game_1531a0.h"
#include "game/tex.h"
#include "lib/mtx.h"
#include "lib/vi.h"

// GoldenEye's in-game frame is 320x240, whatever the window: a view's height
// in its units is its share of 240
#define HUD_FRAME_H 240.0f

// gunDrawHudString()'s two colours: white, outlined in grey
#define COL_TEXT    0xffffffff
#define COL_OUTLINE 0x646464ff
// the top message's band (microcode_constructor_related_to_menus(.., 0x64))
#define COL_BAND    0x00000064

// the gauges' 23 pairs of vertices (gewatch.c's)
#define GAUGE_VERTICES 46

/**
 * The ammunition pictures, gun.c's ammo_related[]: the image's number in the
 * ROM (assets/oddtextures.c's s_*ammoimage rows of the global image bank),
 * its size and format as those rows have them, and the row's IconYOffset.
 */
enum {
	ICON_NONE,
	ICON_9MM,
	ICON_RIFLE,
	ICON_SHOTGUN,
	ICON_KNIFE,
	ICON_GRENADEROUND,
	ICON_ROCKET,
	ICON_GRENADE,
	ICON_MAGNUM,
	ICON_GOLDENGUN,
	ICON_REMOTEMINE,
	ICON_TIMEDMINE,
	ICON_PROXMINE,
	ICON_TANK,
	NUM_ICONS
};

static const struct {
	s32 image;
	u8 width, height, format, depth, wraps;
	s8 yoffset;
} g_IconRows[NUM_ICONS] = {
	[ICON_9MM]          = { 2231,  5, 12, G_IM_FMT_RGBA, G_IM_SIZ_32b, 0,  0 },
	[ICON_RIFLE]        = { 2232,  5, 28, G_IM_FMT_RGBA, G_IM_SIZ_32b, 0, -2 },
	[ICON_SHOTGUN]      = { 2167,  6, 20, G_IM_FMT_RGBA, G_IM_SIZ_32b, 0,  0 },
	[ICON_KNIFE]        = { 2166,  6, 24, G_IM_FMT_RGBA, G_IM_SIZ_32b, 0,  0 },
	[ICON_GRENADEROUND] = { 2165,  8, 21, G_IM_FMT_RGBA, G_IM_SIZ_32b, 1,  0 },
	[ICON_ROCKET]       = { 2161,  7, 22, G_IM_FMT_RGBA, G_IM_SIZ_32b, 0, -2 },
	[ICON_GRENADE]      = { 2163, 14, 18, G_IM_FMT_RGBA, G_IM_SIZ_32b, 0,  0 },
	[ICON_MAGNUM]       = { 2164,  5, 15, G_IM_FMT_RGBA, G_IM_SIZ_32b, 0,  0 },
	[ICON_GOLDENGUN]    = { 2233,  5, 12, G_IM_FMT_RGBA, G_IM_SIZ_32b, 0,  0 },
	[ICON_REMOTEMINE]   = { 2234, 14, 14, G_IM_FMT_RGBA, G_IM_SIZ_32b, 0,  1 },
	[ICON_TIMEDMINE]    = { 2238, 14, 14, G_IM_FMT_RGBA, G_IM_SIZ_32b, 0,  1 },
	[ICON_PROXMINE]     = { 2235, 14, 14, G_IM_FMT_RGBA, G_IM_SIZ_32b, 0,  1 },
	[ICON_TANK]         = { 2464,  7, 22, G_IM_FMT_IA,   G_IM_SIZ_8b,  0, -1 },
};

// the radar's disc (image_bank.c's mpradarimages): 32x32 RGBA16 with six
// levels, of which only the alpha is used
#define RADAR_IMAGE 200
// radar.c: the disc's black at 0xa0, a blip's surround at 0x40, a blip in
// range at 0xa0 and one held at the rim at 0x60, the player in the middle white
#define RADAR_DISC_ALPHA 0xa0
#define COL_RADAR_SURROUND 0x00000040
#define COL_RADAR_BLIP 0xffff0000
#define COL_RADAR_SELF 0xffffff00

// GoldenEye's crosshair image (IMAGE_CROSSHAIR1), 32x32 RGBA32
#define SIGHT_IMAGE 2236
#define SIGHT_ALPHA 0x6e

// The release's own (texture/bg/sight), 256x256 over the same 32 units: the
// same crosshair, a clean ring and a faint bevel. It draws it darker and less
// see-through than GoldenEye does. Measured off its Dam in Xenia at 1280x720,
// PP7 and zoomed sniper rifle alike (it has no scope picture of its own): over
// a flat wall the view behind keeps 0.40 of itself under the bars, whose own
// red is 173 of 255 - 0x99 of 255 at a shade of 0xad.
#define SIGHT_HD_PICTURE "bg/sight"
#define SIGHT_HD_SHADE 0xad
#define SIGHT_HD_ALPHA 0x99

/**
 * What each of GoldenEye's weapons shows, from its gunWeaponStat row: the
 * picture of its AmmoType, and WEAPONSTATBITFLAG_NO_CLIP_RELOADS, which is a
 * weapon with no magazine to speak of - everything held is one number. A
 * weapon whose AmmoType is AMMO_NONE (the knife in the hand, the laser) or has
 * no picture and is flagged HIDE_AMMO_DISPLAY (the key) shows nothing, and the
 * gadgets' types have no picture and count nothing the player can spend.
 */
static const struct { u8 icon, noclip; } g_WeaponRows[NUM_GE_WEAPONS] = {
	[WEAPON_GE_PP7 - WEAPON_GE_FIRST]             = { ICON_9MM, 0 },
	[WEAPON_GE_PP7SILENCED - WEAPON_GE_FIRST]     = { ICON_9MM, 0 },
	[WEAPON_GE_DD44 - WEAPON_GE_FIRST]            = { ICON_9MM, 0 },
	[WEAPON_GE_KLOBB - WEAPON_GE_FIRST]           = { ICON_9MM, 0 },
	[WEAPON_GE_KF7SOVIET - WEAPON_GE_FIRST]       = { ICON_RIFLE, 0 },
	[WEAPON_GE_ZMG - WEAPON_GE_FIRST]             = { ICON_9MM, 0 },
	[WEAPON_GE_D5K - WEAPON_GE_FIRST]             = { ICON_9MM, 0 },
	[WEAPON_GE_D5KSILENCED - WEAPON_GE_FIRST]     = { ICON_9MM, 0 },
	[WEAPON_GE_PHANTOM - WEAPON_GE_FIRST]         = { ICON_9MM, 0 },
	[WEAPON_GE_AR33 - WEAPON_GE_FIRST]            = { ICON_RIFLE, 0 },
	[WEAPON_GE_RCP90 - WEAPON_GE_FIRST]           = { ICON_9MM, 0 },
	[WEAPON_GE_SHOTGUN - WEAPON_GE_FIRST]         = { ICON_SHOTGUN, 0 },
	[WEAPON_GE_AUTOSHOTGUN - WEAPON_GE_FIRST]     = { ICON_SHOTGUN, 0 },
	[WEAPON_GE_SNIPERRIFLE - WEAPON_GE_FIRST]     = { ICON_RIFLE, 0 },
	[WEAPON_GE_COUGARMAGNUM - WEAPON_GE_FIRST]    = { ICON_MAGNUM, 0 },
	[WEAPON_GE_GOLDENGUN - WEAPON_GE_FIRST]       = { ICON_GOLDENGUN, 0 },
	[WEAPON_GE_GRENADELAUNCHER - WEAPON_GE_FIRST] = { ICON_GRENADEROUND, 0 },
	[WEAPON_GE_ROCKETLAUNCHER - WEAPON_GE_FIRST]  = { ICON_ROCKET, 0 },
	[WEAPON_GE_THROWINGKNIFE - WEAPON_GE_FIRST]   = { ICON_KNIFE, 1 },
	[WEAPON_GE_GRENADE - WEAPON_GE_FIRST]         = { ICON_GRENADE, 1 },
	[WEAPON_GE_TIMEDMINE - WEAPON_GE_FIRST]       = { ICON_TIMEDMINE, 1 },
	[WEAPON_GE_PROXIMITYMINE - WEAPON_GE_FIRST]   = { ICON_PROXMINE, 1 },
	[WEAPON_GE_REMOTEMINE - WEAPON_GE_FIRST]      = { ICON_REMOTEMINE, 1 },
	[WEAPON_GE_TANKSHELLS - WEAPON_GE_FIRST]      = { ICON_TANK, 0 },
};

static struct {
	s32 stagenum;
	s32 moddir;
	s32 on;
	// texSelect() turns a config's number into a pointer that lasts the stage
	struct textureconfig icons[NUM_ICONS];
	struct textureconfig sight;
	struct textureconfig radar;
	// the radar's middle on the view's frame, set by geHudRadarBegin()
	s32 radarx, radary;
	f32 radarsx, radarsy;
} g_Hud = { -1, -1, 0 };

/** The view in GoldenEye's units, and what one of them is worth on the frame buffer. */
struct hudframe {
	f32 sx, sy;
	s32 width, height;
};

void geHudStageStart(s32 stagenum)
{
	g_Hud.stagenum = stagenum;
	g_Hud.on = 0;
	g_Hud.moddir = modloaderGetStageModDirIndex(stagenum);

	if (!modloaderStageIsMission(stagenum) && !(g_GexPlusMode && modloaderStageIsRemake(stagenum))) {
		return;
	}

	if (g_Hud.moddir < 0 || !gexFrontLoadText()) {
		sysLogPrintf(LOG_WARNING, "gehud: the conversion's fonts are not there; this level keeps Perfect Dark's HUD");
		return;
	}

	for (s32 i = 0; i < NUM_ICONS; i++) {
		struct textureconfig *tex = &g_Hud.icons[i];

		memset(tex, 0, sizeof(*tex));
		tex->texturenum = g_IconRows[i].image;
		tex->width = g_IconRows[i].width;
		tex->height = g_IconRows[i].height;
		tex->format = g_IconRows[i].format;
		tex->depth = g_IconRows[i].depth;
		tex->s = g_IconRows[i].wraps ? G_TX_WRAP : G_TX_CLAMP;
		tex->t = G_TX_CLAMP;
	}

	memset(&g_Hud.sight, 0, sizeof(g_Hud.sight));
	g_Hud.sight.texturenum = SIGHT_IMAGE;
	g_Hud.sight.width = 32;
	g_Hud.sight.height = 32;
	g_Hud.sight.format = G_IM_FMT_RGBA;
	g_Hud.sight.depth = G_IM_SIZ_32b;
	g_Hud.sight.s = G_TX_WRAP;
	g_Hud.sight.t = G_TX_WRAP;

	memset(&g_Hud.radar, 0, sizeof(g_Hud.radar));
	g_Hud.radar.texturenum = RADAR_IMAGE;
	g_Hud.radar.width = 32;
	g_Hud.radar.height = 32;
	g_Hud.radar.level = 6;
	g_Hud.radar.format = G_IM_FMT_RGBA;
	g_Hud.radar.depth = G_IM_SIZ_16b;
	g_Hud.radar.s = G_TX_WRAP;
	g_Hud.radar.t = G_TX_WRAP;

	g_Hud.on = 1;
}

s32 geHudActive(void)
{
	return g_Hud.on && g_Hud.stagenum == g_Vars.stagenum;
}

/**
 * Perfect Dark's own weapons keep Perfect Dark's display. Nothing in the hand
 * and the bare hands are nobody's: GoldenEye shows nothing for them, which is
 * what is wanted on its levels.
 */
static s32 hudWeaponIsGoldenEyes(s32 weaponnum)
{
	return weaponnum <= WEAPON_UNARMED || weaponnum >= WEAPON_GE_FIRST;
}

s32 geHudOwnsWeapon(void)
{
	return geHudActive() && hudWeaponIsGoldenEyes(g_Vars.currentplayer->hands[HAND_RIGHT].gset.weaponnum);
}

/**
 * The current player's view as GoldenEye's frame: 240 units to the height of
 * the window and square, so a view's own size in them is whatever share of
 * the window it has - 320x240 alone on 4:3, 160x120 in a corner of four, and
 * 426x240 alone on 16:9. The text frame is set to the same box, so an x here
 * is an x there, counted from the view's own left.
 */
static void hudFrame(struct hudframe *f)
{
	f->sy = (f32)viGetHeight() / HUD_FRAME_H;
	f->sx = (f32)viGetWidth() / (HUD_FRAME_H * videoGetAspect());
	f->width = (s32)(viGetViewWidth() / f->sx + 0.5f);
	f->height = (s32)(viGetViewHeight() / f->sy + 0.5f);

	gexFrontTextFrame(viGetViewWidth() / f->sx, viGetViewHeight() / f->sy,
			viGetViewLeft(), viGetViewTop(), viGetViewWidth(), viGetViewHeight());
}

/** A picture of the conversion's over a box of the frame buffer, shaded by `shade` at `alpha`. */
static Gfx *hudImage(Gfx *gdl, struct textureconfig *tex, s32 mode, s32 point, s32 flip,
		f32 x1, f32 y1, f32 x2, f32 y2, s32 twidth, s32 theight, s32 shade, s32 alpha)
{
	const s32 prevsrc = modSetTextureSourceMod(g_Hud.moddir);

	texSelect(&gdl, tex, mode, 0, 2, 1, NULL);
	modSetTextureSourceMod(prevsrc);

	gDPSetCycleType(gdl++, G_CYC_1CYCLE);
	gDPSetTexturePersp(gdl++, G_TP_NONE);
	gDPSetAlphaCompare(gdl++, G_AC_NONE);
	gDPSetTextureLOD(gdl++, G_TL_TILE);
	gDPSetTextureFilter(gdl++, point ? G_TF_POINT : G_TF_BILERP);
	gDPSetRenderMode(gdl++, G_RM_XLU_SURF, G_RM_XLU_SURF2);
	gDPSetEnvColor(gdl++, shade, shade, shade, alpha);
	gDPSetCombineLERP(gdl++, TEXEL0, 0, ENVIRONMENT, 0, TEXEL0, 0, ENVIRONMENT, 0, TEXEL0, 0, ENVIRONMENT, 0, TEXEL0, 0, ENVIRONMENT, 0);
	// the picture's rows run bottom to top (gexfront.c's frontImage() with a
	// negative height), so t starts on the last row and counts back.
	//
	// The renderer's own wide rectangle, whose corners are signed: the N64
	// command holds twelve bits a corner, and the sight aimed at the edge of
	// the view - where its picture starts up to sixteen units off it, which
	// the mouse does at any zoom - wrapped to the far side and drew as a
	// screen of crosshairs (F3 20260922-003018, 003132).
	gSPTextureRectangleWideEXT(gdl++,
			(s32)(x1 * 4), (s32)(y1 * 4), (s32)(x2 * 4), (s32)(y2 * 4),
			G_TX_RENDERTILE, 0, flip ? (theight << 5) - 1 : 0,
			(s32)(twidth * 1024.0f / (x2 - x1)), (s32)((flip ? -theight : theight) * 1024.0f / (y2 - y1)), 0);

	return gdl;
}

/**
 * gunDrawHudString(): Bank Gothic or Zurich Bold at a place on the view's
 * frame, aligned about it GoldenEye's way (HUDHALIGN_*: 0 ends at x, 1 starts
 * at it, 2 is centred on it; HUDVALIGN_* the same down the screen), and
 * outlined as textRenderOutlined() does it - the text eight times in the
 * outline's colour, a unit out every way, and then itself over them.
 */
static Gfx *hudString(Gfx *gdl, s32 gothic, const char *text, s32 x, s32 halign, s32 y, s32 valign, s32 outline)
{
	s32 w, h;

	gexFrontTextMeasure(gothic, text, &w, &h);

	if (halign == 0) {
		x -= w;
	} else if (halign == 2) {
		x = x + w / 2 - w;
	}

	if (valign == 0) {
		y -= h;
	} else if (valign == 2) {
		y = y + h / 2 - h;
	}

	if (outline) {
		for (s32 dx = -1; dx <= 1; dx++) {
			for (s32 dy = -1; dy <= 1; dy++) {
				if (dx || dy) {
					gdl = gexFrontTextPrint(gdl, gothic, x + dx, y + dy, text, COL_OUTLINE);
				}
			}
		}
	}

	return gexFrontTextPrint(gdl, gothic, x, y, text, COL_TEXT);
}

static Gfx *hudInteger(Gfx *gdl, s32 value, s32 x, s32 halign, s32 y, s32 valign)
{
	char buffer[12];

	// g_GunHudIntegerFormat: the newline is where textMeasure() gets a height
	// from, which the alignment about y then halves
	snprintf(buffer, sizeof(buffer), "%d\n", value);

	return hudString(gdl, 1, buffer, x, halign, y, valign, 1);
}

/** Back to what Perfect Dark's own HUD code leaves behind it (text0f153780()). */
static Gfx *hudEnd(Gfx *gdl)
{
	gexFrontTextFrameDefault();

	gDPPipeSync(gdl++);
	gDPSetColorDither(gdl++, G_CD_BAYER);
	gDPSetTexturePersp(gdl++, G_TP_PERSP);
	gDPSetTextureLOD(gdl++, G_TL_LOD);
	gDPSetTextureFilter(gdl++, G_TF_BILERP);

	return gdl;
}

/**
 * One hand's numbers: what is in the gun and what is held for it. A weapon
 * with no magazine shows everything as one number where the reserve goes, both
 * hands' together when they hold the same thing.
 *
 * The tank's gun is the one weapon here with no magazine of Perfect Dark's
 * behind it - its shells are a count of ammunition and nothing else
 * (getank.c) - and GoldenEye shows it as a magazine of one and the rest.
 */
static s32 hudHandAmmo(s32 handnum, s32 *icon, s32 *mag, s32 *reserve, s32 *noclip)
{
	const struct player *player = g_Vars.currentplayer;
	const struct hand *hand = &player->hands[handnum];
	const struct hand *other = &player->hands[1 - handnum];
	const s32 weaponnum = hand->gset.weaponnum;
	s32 type;

	if (!hand->inuse || weaponnum < WEAPON_GE_FIRST || weaponnum >= NUM_WEAPONS) {
		return 0;
	}

	// GUN_ANIM_STATE_SWITCH_*: nothing while the gun is on its way up or down
	if (hand->state == HANDSTATE_CHANGEGUN) {
		return 0;
	}

	*icon = g_WeaponRows[weaponnum - WEAPON_GE_FIRST].icon;
	*noclip = g_WeaponRows[weaponnum - WEAPON_GE_FIRST].noclip;

	if (*icon == ICON_NONE) {
		return 0;
	}

	if (weaponnum == WEAPON_GE_TANKSHELLS) {
		const s32 shells = player->ammoheldarr[AMMOTYPE_1D];

		*mag = shells > 0 ? 1 : 0;
		*reserve = shells - *mag;

		return 1;
	}

	type = hand->ammotypes[0];

	if (type < 0) {
		return 0;
	}

	*mag = hand->loadedammo[0];
	*reserve = player->ammoheldarr[type];

	if (*noclip) {
		*reserve += *mag;
		*mag = 0;

		if (other->inuse && other->gset.weaponnum == weaponnum) {
			*reserve += other->loadedammo[0];
		}
	}

	return 1;
}

Gfx *geHudRenderAmmo(Gfx *gdl)
{
	struct hudframe f;
	const s32 playercount = PLAYERCOUNT();
	s32 leftx = 59;
	s32 rightx = 59;
	s32 bottom;

	hudFrame(&f);
	bottom = f.height;

	if (playercount >= 3) {
		if (g_Vars.currentplayernum & 1) {
			leftx = 43;
			rightx = 127;
		} else {
			rightx = 109;
		}
	}

	for (s32 handnum = 0; handnum < 2; handnum++) {
		const s32 left = handnum == HAND_LEFT;
		// the picture's middle, and each number from its own side of it
		const s32 cx = left ? leftx : f.width - rightx;
		s32 icon, mag, reserve, noclip;
		s32 width;
		s32 x0, y0;
		struct textureconfig *tex;

		if (!hudHandAmmo(handnum, &icon, &mag, &reserve, &noclip)) {
			continue;
		}

		// microcode_generation_ammo_related(): an odd picture sits half a unit
		// right of its middle and half a unit up, which is what keeps its
		// edges on whole units
		tex = &g_Hud.icons[icon];
		width = g_IconRows[icon].width;
		x0 = cx - width / 2;
		y0 = bottom - 20 + g_IconRows[icon].yoffset - (g_IconRows[icon].height + 1) / 2;

		gdl = hudImage(gdl, tex, 2, 1, 1,
				viGetViewLeft() + x0 * f.sx, viGetViewTop() + y0 * f.sy,
				viGetViewLeft() + (x0 + width) * f.sx, viGetViewTop() + (y0 + g_IconRows[icon].height) * f.sy,
				width, g_IconRows[icon].height, 255, 255);

		gdl = gexFrontTextSetup(gdl);

		// the right hand's magazine is on the inside of its picture and its
		// reserve on the outside, and the left hand's are the other way round
		if (!noclip) {
			if (left) {
				gdl = hudInteger(gdl, mag, cx + width / 2 + 3, 1, bottom - 18, 2);
			} else {
				gdl = hudInteger(gdl, mag, cx - width / 2 - 4, 0, bottom - 18, 2);
			}
		}

		if (reserve > 0 || noclip) {
			if (left) {
				gdl = hudInteger(gdl, reserve, cx - (width + 1) / 2 - 4, 0, bottom - 18, 2);
			} else {
				gdl = hudInteger(gdl, reserve, cx + (width + 1) / 2 + 3, 1, bottom - 18, 2);
			}
		}
	}

	return hudEnd(gdl);
}

Gfx *geHudRenderSight(Gfx *gdl, f32 x, f32 y)
{
	struct hudframe f;
	struct textureconfig release;
	s32 w, h;
	// the release's crosshair where the release is there and its look is on
	// (gefolder.c's stand-in, which the renderer draws whole over the config's
	// nominal 32 texels, and the right way up as GoldenEye's is drawn)
	const void *tile = geFolderMenuPicture(SIGHT_HD_PICTURE, &w, &h);

	hudFrame(&f);

	gDPPipeSync(gdl++);

	if (tile) {
		memset(&release, 0, sizeof(release));
		release.textureptr = (u8 *)tile;
		release.width = 32;
		release.height = 32;
		release.format = G_IM_FMT_RGBA;
		release.depth = G_IM_SIZ_32b;
		release.s = G_TX_CLAMP;
		release.t = G_TX_CLAMP;

		gdl = hudImage(gdl, &release, 4, 0, 0,
				x - 16.0f * f.sx, y - 16.0f * f.sy, x + 16.0f * f.sx, y + 16.0f * f.sy,
				32, 32, SIGHT_HD_SHADE, SIGHT_HD_ALPHA);
	} else {
		gdl = hudImage(gdl, &g_Hud.sight, 4, 0, 0,
				x - 16.0f * f.sx, y - 16.0f * f.sy, x + 16.0f * f.sx, y + 16.0f * f.sy,
				32, 32, 255, SIGHT_ALPHA);
	}

	return hudEnd(gdl);
}

/**
 * bondviewRenderGaugeBars(): the watch's own two gauges (gewatch.c) seen from
 * straight above through GoldenEye's orthographic frame, 1600 by 1200 about
 * the middle of the view, which puts armour's arc down the right of the view
 * and health's down the left. The frame is as many units wide as keeps the
 * arcs round in the view there is.
 */
Gfx *geHudRenderGauges(Gfx *gdl)
{
	Vtx *health = gfxAllocateVertices(GAUGE_VERTICES);
	Col *healthc = gfxAllocate(GAUGE_VERTICES * sizeof(Col));
	Vtx *armour = gfxAllocateVertices(GAUGE_VERTICES);
	Col *armourc = gfxAllocate(GAUGE_VERTICES * sizeof(Col));
	Mtx *projection = gfxAllocateMatrix();
	Mtx *modelview = gfxAllocateMatrix();
	Mtxf ortho;
	Mtxf lookat;
	const f32 aspect = videoGetAspect()
		* ((f32)viGetViewWidth() / (f32)viGetWidth())
		/ ((f32)viGetViewHeight() / (f32)viGetHeight());
	// never narrower than GoldenEye's own, or the arcs leave the view
	const f32 halfwidth = aspect > 4.0f / 3.0f ? 600.0f * aspect : 800.0f;

	geWatchGaugeVertices(armour, armourc, 1, g_Vars.currentplayer->apparentarmour);
	geWatchGaugeVertices(health, healthc, -1, g_Vars.currentplayer->apparenthealth);

	guOrthoF(ortho.m, -halfwidth, halfwidth, -600.0f, 600.0f, -100.0f, 1000.0f, 1.0f);
	guMtxF2L(ortho.m, projection);
	mtx00016ae4(&lookat, 0.0f, 500.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, -1.0f);
	guMtxF2L(lookat.m, modelview);

	gSPMatrix(gdl++, osVirtualToPhysical(projection), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_PROJECTION);
	gSPMatrix(gdl++, osVirtualToPhysical(modelview), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
	gDPPipeSync(gdl++);
	gDPSetCycleType(gdl++, G_CYC_1CYCLE);
	gDPSetRenderMode(gdl++, G_RM_AA_XLU_SURF, G_RM_AA_XLU_SURF2);
	gDPSetAlphaCompare(gdl++, G_AC_NONE);
	gDPSetCombineMode(gdl++, G_CC_SHADE, G_CC_SHADE);
	gDPSetPrimColor(gdl++, 0, 0, 0xe6, 0xe6, 0xe6, 0x00);
	gSPClearGeometryMode(gdl++, G_CULL_BOTH | G_ZBUFFER);

	gdl = geWatchDrawGauge(gdl, armour, armourc);
	gdl = geWatchDrawGauge(gdl, health, healthc);

	gSPMatrix(gdl++, osVirtualToPhysical(camGetPerspectiveMtxL()), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_PROJECTION);

	return gdl;
}

/**
 * display_red_blue_on_radar(): the disc, 41 in from the view's right and 26
 * down, black at 0xa0 through the picture's alpha. Who is on it, and whether
 * it is up at all, stays Perfect Dark's (radarRender()), which draws every
 * blip through radarDrawDot() - and that hands them to geHudRadarDot() between
 * this and geHudRadarEnd().
 */
Gfx *geHudRadarBegin(Gfx *gdl)
{
	struct hudframe f;
	const s32 prevsrc = modSetTextureSourceMod(g_Hud.moddir);
	f32 x1, y1, x2, y2;

	hudFrame(&f);

	g_Hud.radarsx = f.sx;
	g_Hud.radarsy = f.sy;
	g_Hud.radarx = f.width - 0x29;
	g_Hud.radary = 0x1a;

	if (PLAYERCOUNT() >= 3 && !(g_Vars.currentplayernum & 1)) {
		g_Hud.radarx += 0xf;
	}

	x1 = viGetViewLeft() + (g_Hud.radarx - 16) * f.sx;
	// less the picture's first row, which is not the disc's: a few stray
	// opaque texels three rows clear of it, a row of grey dashes over the
	// radar at this size that a 240 line screen never resolved
	y1 = viGetViewTop() + (g_Hud.radary - 15) * f.sy;
	x2 = viGetViewLeft() + (g_Hud.radarx + 16) * f.sx;
	y2 = viGetViewTop() + (g_Hud.radary + 16) * f.sy;

	texSelect(&gdl, &g_Hud.radar, 2, 0, 2, 1, NULL);
	modSetTextureSourceMod(prevsrc);

	gDPPipeSync(gdl++);
	gDPSetCycleType(gdl++, G_CYC_1CYCLE);
	gDPSetColorDither(gdl++, G_CD_DISABLE);
	gDPSetTexturePersp(gdl++, G_TP_NONE);
	gDPSetAlphaCompare(gdl++, G_AC_NONE);
	gDPSetTextureLOD(gdl++, G_TL_TILE);
	gDPSetTextureFilter(gdl++, G_TF_BILERP);
	gDPSetRenderMode(gdl++, G_RM_XLU_SURF, G_RM_XLU_SURF2);
	gDPSetCombineLERP(gdl++, 0, 0, 0, PRIMITIVE, PRIMITIVE, 0, TEXEL0, 0, 0, 0, 0, PRIMITIVE, PRIMITIVE, 0, TEXEL0, 0);
	gDPSetPrimColor(gdl++, 0, 0, 0x00, 0x00, 0x00, RADAR_DISC_ALPHA);
	gSPTextureRectangle(gdl++,
			(s32)(x1 * 4), (s32)(y1 * 4), (s32)(x2 * 4), (s32)(y2 * 4),
			// GoldenEye draws it one texel a pixel from half a texel in, which
			// samples the 32 texel centres and nothing past them; stretched
			// over more pixels than that, the same span is 31 texels from
			// centre to centre, or the last rows wrap round to the first
			G_TX_RENDERTILE, 0x10, 0x30,
			(s32)(31 * 1024.0f / (x2 - x1)), (s32)(30 * 1024.0f / (y2 - y1)));

	return gdl;
}

/**
 * A filled box in the view's units, to a quarter of a frame buffer pixel. The
 * ordinary fill rectangle takes whole pixels of a frame buffer that is far
 * coarser than the window, which is nothing to a band across the screen and
 * everything to a blip two units across: they came out four pixels by seven
 * and nine by seven, whichever way each edge happened to round.
 */
static Gfx *hudFillBox(Gfx *gdl, f32 x1, f32 y1, f32 x2, f32 y2, u32 colour)
{
	const s32 ulx = (s32)((viGetViewLeft() + x1 * g_Hud.radarsx) * 4.0f);
	const s32 uly = (s32)((viGetViewTop() + y1 * g_Hud.radarsy) * 4.0f);
	const s32 lrx = (s32)((viGetViewLeft() + x2 * g_Hud.radarsx) * 4.0f);
	const s32 lry = (s32)((viGetViewTop() + y2 * g_Hud.radarsy) * 4.0f);
	Gfx *g0;
	Gfx *g1;

	gDPSetRenderMode(gdl++, G_RM_XLU_SURF, G_RM_XLU_SURF2);
	gDPSetCombineMode(gdl++, G_CC_PRIMITIVE, G_CC_PRIMITIVE);
	gDPSetPrimColor(gdl++, 0, 0, colour >> 24, (colour >> 16) & 0xff, (colour >> 8) & 0xff, colour & 0xff);

	// gDPFillRectangleWideEXT() with the two fraction bits it shifts away
	g0 = gdl++;
	g1 = gdl++;
	g0->words.w0 = _SHIFTL(G_FILLRECT_WIDE_EXT, 24, 8) | _SHIFTL(lrx, 0, 24);
	g0->words.w1 = _SHIFTL(lry, 0, 24);
	g1->words.w0 = _SHIFTL(ulx, 0, 24);
	g1->words.w1 = _SHIFTL(uly, 0, 24);

	return gdl;
}

/**
 * A blip `dx`, `dy` from the middle: four units of black at 0x40 with two of
 * the colour inside it, brighter in range than held at the rim. GoldenEye's
 * blips are yellow and its own player white; a colour of Perfect Dark's that
 * means something - a team's, a scenario's - is kept, and its plain radar
 * colour is what becomes GoldenEye's. There are no height arrows in GoldenEye.
 */
Gfx *geHudRadarDot(Gfx *gdl, s32 self, s32 dx, s32 dy, u32 rgb, s32 plain, s32 atrim)
{
	const s32 x = g_Hud.radarx + dx;
	const s32 y = g_Hud.radary + dy;
	u32 colour = rgb & 0xffffff00;

	if (plain) {
		colour = self ? COL_RADAR_SELF : COL_RADAR_BLIP;
	}

	colour |= atrim ? 0x60 : 0xa0;

	gdl = hudFillBox(gdl, x - 2, y - 2, x + 2, y + 2, COL_RADAR_SURROUND);
	gdl = hudFillBox(gdl, x - 1, y - 1, x + 1, y + 1, colour);

	return gdl;
}

Gfx *geHudRadarEnd(Gfx *gdl)
{
	return hudEnd(gdl);
}

s32 geHudMessageDuration(s32 top)
{
	// BONDVIEW_UPPER_TEXT_TIMER_C and BONDVIEW_INTRO_CAMERA_BONDMESSCNT_C
	return top ? 0xf0 : 0x78;
}

Gfx *geHudRenderMessage(Gfx *gdl, const char *text, s32 top, s32 *row)
{
	struct hudframe f;
	char wrapped[512];
	char ended[512];
	const s32 gothic = top ? 0 : 1;
	s32 w, h;
	s32 x = 0x1e;
	s32 y;

	hudFrame(&f);

	if (PLAYERCOUNT() >= 3 && (g_Vars.currentplayernum & 1)) {
		x = 0xa;
	}

	// textMeasure() counts a line at its newline, and GoldenEye's messages
	// all end in one
	if (text[0] && text[strlen(text) - 1] != '\n') {
		snprintf(ended, sizeof(ended), "%s\n", text);
		text = ended;
	}

	// GoldenEye's own messages are broken for these fonts already; one of
	// Perfect Dark's may not be
	gexFrontTextMeasure(gothic, text, &w, &h);

	if (w > f.width - 2 * x) {
		gexFrontTextWrap(gothic, text, wrapped, sizeof(wrapped), f.width - 2 * x);
		text = wrapped;
		gexFrontTextMeasure(gothic, text, &w, &h);
	}

	if (top) {
		y = 0xd;

		gdl = gexFrontFillRect(gdl, 0, y - 2, f.width + 1, y + h, COL_BAND);
		gdl = gexFrontTextSetup(gdl);
		gdl = gexFrontTextPrint(gdl, 0, x, y, text, COL_TEXT);

		return hudEnd(gdl);
	}

	if (PLAYERCOUNT() < 3) {
		// BONDVIEW_VIEW_TOP_OFFSET_1 and _2: over the left hand's ammunition
		// and the countdown when either is there
		const s32 raised = g_Vars.currentplayer->hands[HAND_LEFT].inuse || !g_CountdownTimerOff;

		y = f.height - (raised ? 0x28 : 0x0c);

		if (g_Vars.currentplayernum == 1) {
			y -= 8;
		}
	} else {
		y = 0x10 + h;
	}

	y -= *row;
	*row += h + 2;

	gdl = gexFrontTextSetup(gdl);
	gdl = hudString(gdl, 1, text, x, 1, y, 0, 1);

	return hudEnd(gdl);
}

Gfx *geHudRenderCountdown(Gfx *gdl, s32 mins, s32 secs, s32 ms)
{
	struct hudframe f;
	s32 x;
	s32 y;

	hudFrame(&f);

	// GoldenEye's columns are about the middle of its 320
	x = f.width / 2 - 160;
	y = f.height - 18;

	gdl = gexFrontTextSetup(gdl);
	gdl = hudInteger(gdl, (mins % 100) / 10, x + 0x82, 2, y, 2);
	gdl = hudInteger(gdl, mins % 10, x + 0x8a, 2, y, 2);
	gdl = hudString(gdl, 1, ":\n", x + 0x93, 2, y, 2, 1);
	gdl = hudInteger(gdl, (secs % 60) / 10, x + 0x9c, 2, y, 2);
	gdl = hudInteger(gdl, secs % 10, x + 0xa4, 2, y, 2);
	gdl = hudString(gdl, 1, ":\n", x + 0xad, 2, y, 2, 1);
	gdl = hudInteger(gdl, (ms % 100) / 10, x + 0xb6, 2, y, 2);
	gdl = hudInteger(gdl, ms % 10, x + 0xbe, 2, y, 2);

	return hudEnd(gdl);
}
