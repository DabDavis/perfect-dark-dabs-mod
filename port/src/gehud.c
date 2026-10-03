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
 * - the countdown (propobj.c's countdownTimerRender()), and Perfect Dark's
 *   mission timer, which GoldenEye has none of, set in the countdown's figures.
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
#include "fs.h"
#include "config.h"
#include "mod.h"
#include "modloader.h"
#include "system.h"
#include "video.h"
#include "gehud.h"
#include "geguns.h"
#include "langpack.h"
#include "gefolder.h"
#include "gegadgets.h"
#include "gewatch.h"
#include "gexfront.h"
#include "gebean.h"
#include "xblamesh.h"
#include "game/bondgun.h"
#include "game/botact.h"
#include "game/camera.h"
#include "game/gfxmemory.h"
#include "game/options.h"
#include "game/game_1531a0.h"
#include "game/savebuffer.h"
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

/**
 * A ROM hack's own pictures (its conversion's menu/geammo.bin, a row an
 * AmmoType), numbered from ICON_OWN up by type. GE Editor's hacks redraw them
 * at other sizes: Goldfinger 64's 9mm round is 4x13 where GoldenEye's is 5x12,
 * drawn through GoldenEye's box it came out a smear with a stripe of the next
 * row's colours over it (F3 20261003-050551, 20261003-061317).
 */
#define HUD_OWN_TYPES 30
#define ICON_OWN NUM_ICONS

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

/**
 * The release's own pictures of the same (files/texture/bg/, the names the
 * decomp's oddtextures.c gives beside each row), drawn under its look over the
 * same boxes GoldenEye's are - which is what the release does: its 9mm round
 * is 80x180 over GoldenEye's 5x12 units. A name with a '#' is one the release
 * has only at GoldenEye's own size, made from another of its pictures
 * (gefolder.c's menuAmmoPicture()). The tank's shell is its own
 * bg/ammoicontankshell, which it has only at GoldenEye's 7x22 but which reads
 * whole over the box - GoldenEye's dark shell with its band. It had been the
 * magnum round, a white cartridge nothing like it (F3 20260930-185749, "wrong
 * tank shell icon").
 */
static const char *const g_IconHdPictures[NUM_ICONS] = {
	[ICON_9MM]          = "bg/ammoicon9mm",
	[ICON_RIFLE]        = "bg/ammoiconrifle",
	[ICON_SHOTGUN]      = "bg/ammoiconshell",
	[ICON_KNIFE]        = "bg/ammoiconknife",
	[ICON_GRENADEROUND] = "bg/ammoicongrenade",
	[ICON_ROCKET]       = "bg/ammoiconrocket",
	[ICON_GRENADE]      = "bg/ammogrenadehand",
	[ICON_MAGNUM]       = "bg/ammoiconmagnum",
	[ICON_GOLDENGUN]    = "bg/ammoicon9mm#gold",
	[ICON_REMOTEMINE]   = "bg/ammoiconmine",
	[ICON_TIMEDMINE]    = "bg/ammoiconmine#yellow",
	[ICON_PROXMINE]     = "bg/ammoiconmine#green",
	[ICON_TANK]         = "bg/ammoicontankshell",
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

/**
 * The release's ammunition, measured off its Dam in Xenia at 1280x720 (PP7 "7
 * 93", the sniper rifle's round, the shotgun's shell), 3 pixels a unit: the
 * pictures are GoldenEye's size and across where GoldenEye puts them in the
 * 4:3 middle (hudReleaseInsets()), but 5 units higher; the numbers are its
 * Bank Gothic at its own proportions, 14 pixels to a digit where GoldenEye's
 * are 23 (0.61 of the size), a unit higher again against the pictures, and
 * outlined in an opaque 85 of 255 about two pixels out.
 */
#define HUD_RELEASE_RAISE 5
// the nominal square a release picture is drawn over (hudReleaseIcon())
#define HUD_RELEASE_TEXELS 32
#define HUD_RELEASE_TEXT 0.61f
#define COL_RELEASE_OUTLINE 0x555555ff

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

static struct {
	s32 stagenum;
	s32 moddir;
	s32 on;
	// texSelect() turns a config's number into a pointer that lasts the stage
	struct textureconfig icons[NUM_ICONS];
	struct textureconfig sight;
	struct textureconfig radar;
	// a hack's own pictures by AmmoType (hudOwnIconsLoad()); numown 0 is none
	s32 numown;
	struct textureconfig own[HUD_OWN_TYPES];
	s8 ownyoffset[HUD_OWN_TYPES];
	// the radar's middle on the view's frame, set by geHudRadarBegin()
	s32 radarx, radary;
	f32 radarsx, radarsy;
	// the mission timer is up this frame (geHudSetMissionTimerShown())
	s32 timershown;
} g_Hud = { -1, -1, 0 };

/**
 * What each of GoldenEye's weapons shows, from its gunWeaponStat row: the
 * picture of its AmmoType, and WEAPONSTATBITFLAG_NO_CLIP_RELOADS, which is a
 * weapon with no magazine to speak of - everything held is one number. A
 * weapon whose AmmoType is AMMO_NONE (the knife in the hand, the laser) or has
 * no picture and is flagged HIDE_AMMO_DISPLAY (the key) shows nothing. A
 * weapon with an AmmoType that has no picture shows its count all the same,
 * with nothing beside it (`bare`, the picture's width taken as GoldenEye's 5):
 * the covert modem (ITEM_BUG, AMMO_BUG) and the plastique show "1" (F3
 * 20260929-092325). The watch magnet's AMMO_WATCH_MAGNET is the one more:
 * its charges are kept since 2026-10-01 (gegadgets.c, GEGADGET_MAGNET_AMMO)
 * and it shows "5" there on Archives' start, as the cartridge does (ares).
 * The camera's AmmoType is AMMO_NONE: it shows nothing and never runs out on
 * the cartridge either.
 */
#define GE_BARE_WIDTH 5

static const struct { u8 icon, noclip, bare; } g_WeaponRows[NUM_GE_WEAPONS] = {
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
	[WEAPON_GE_COVERTMODEM - WEAPON_GE_FIRST]     = { ICON_NONE, 1, 1 },
	[WEAPON_GE_PLASTIQUE - WEAPON_GE_FIRST]       = { ICON_NONE, 1, 1 },
	[WEAPON_GE_WATCHMAGNET - WEAPON_GE_FIRST]     = { ICON_NONE, 1, 1 },
};

/**
 * A gun's icon is its ammunition's, as GoldenEye's is (ammo_related's), by
 * its row's AmmoType - the gun set's, so a ROM hack's own guns show what they
 * load (Goldfinger 64's four pistols past GoldenEye's have no row above). The
 * table above where the row has no type: the gadgets, the tank.
 */
static s32 hudWeaponIcon(s32 weaponnum)
{
	static const u8 icons[] = {
		ICON_NONE, ICON_9MM, ICON_9MM, ICON_RIFLE, ICON_SHOTGUN, ICON_GRENADE, ICON_ROCKET,
		ICON_REMOTEMINE, ICON_PROXMINE, ICON_TIMEDMINE, ICON_KNIFE, ICON_GRENADEROUND, ICON_MAGNUM,
		ICON_GOLDENGUN,
	};
	const s32 ammotype = gegunsGeAmmoType(weaponnum);

	if (ammotype > 0 && ammotype < g_Hud.numown && g_Hud.own[ammotype].texturenum) {
		return ICON_OWN + ammotype;
	}

	if (ammotype > 0 && ammotype < (s32)ARRAYCOUNT(icons)) {
		return icons[ammotype];
	}

	return g_WeaponRows[weaponnum - WEAPON_GE_FIRST].icon;
}

/** A picture's texture config and its box in GoldenEye's units: its size and its row's IconYOffset. */
static struct textureconfig *hudIconBox(s32 icon, s32 *width, s32 *height, s32 *yoffset)
{
	if (icon >= ICON_OWN) {
		struct textureconfig *tex = &g_Hud.own[icon - ICON_OWN];

		*width = tex->width;
		*height = tex->height;
		*yoffset = g_Hud.ownyoffset[icon - ICON_OWN];

		return tex;
	}

	*width = icon != ICON_NONE ? g_IconRows[icon].width : GE_BARE_WIDTH;
	*height = g_IconRows[icon].height;
	*yoffset = g_IconRows[icon].yoffset;

	return &g_Hud.icons[icon];
}

/** Whether one of GoldenEye's weapons shows no ammunition at all. */
static s32 hudWeaponShowsNothing(s32 weaponnum)
{
	return hudWeaponIcon(weaponnum) == ICON_NONE
		&& !g_WeaponRows[weaponnum - WEAPON_GE_FIRST].bare;
}

/** The view in GoldenEye's units, and what one of them is worth on the frame buffer. */
struct hudframe {
	f32 sx, sy;
	s32 width, height;
};

static void geHudMigrateSightAlways(void);

/**
 * A ROM hack's own ammunition pictures, out of its conversion's
 * menu/geammo.bin (geconvert.c): gun.c's ammo_related[] with each picture's
 * row of the global image table. GoldenEye's conversion has none, and its
 * pictures are g_IconRows; so has a hack's converted before the file was.
 */
static void hudOwnIconsLoad(void)
{
	char path[1024];
	u32 len = 0;
	u8 *file;
	s32 rows;

	g_Hud.numown = 0;
	memset(g_Hud.own, 0, sizeof(g_Hud.own));
	memset(g_Hud.ownyoffset, 0, sizeof(g_Hud.ownyoffset));

	if (g_Hud.moddir < 0 || modloaderDirIndexIsGexPlus(g_Hud.moddir)) {
		return;
	}

	snprintf(path, sizeof(path), "%s/menu/geammo.bin", fsGetModDirAt(g_Hud.moddir));
	file = fsFileSize(path) > 0 ? fsFileLoad(path, &len) : NULL;

	if (!file || len < 8 || memcmp(file, "GEA1", 4) != 0) {
		sysMemFree(file);
		return;
	}

	rows = (file[4] << 8) | file[5];

	if (rows > HUD_OWN_TYPES) {
		rows = HUD_OWN_TYPES;
	}

	for (s32 i = 0; i < rows && 8 + 16 * (u32)(i + 1) <= len; i++) {
		const u8 *row = file + 8 + 16 * i;
		struct textureconfig *tex = &g_Hud.own[i];
		const u32 image = (u32)row[0] << 24 | (u32)row[1] << 16 | (u32)row[2] << 8 | row[3];
		const u32 bits = (u32)row[12] << 24 | (u32)row[13] << 16 | (u32)row[14] << 8 | row[15];
		f32 yoffset;

		memcpy(&yoffset, &bits, sizeof(yoffset));
		memset(tex, 0, sizeof(*tex));
		g_Hud.ownyoffset[i] = (s8)yoffset;

		// width, height, level, format, depth, s, t; a picture with no size
		// is none
		if (image == 0 || row[4] == 0 || row[5] == 0) {
			continue;
		}

		tex->texturenum = image;
		tex->width = row[4];
		tex->height = row[5];
		tex->level = row[6];
		tex->format = row[7];
		tex->depth = row[8];
		tex->s = row[9];
		tex->t = row[10];
	}

	g_Hud.numown = rows;
	sysMemFree(file);
}

void geHudStageStart(s32 stagenum)
{
	g_Hud.stagenum = stagenum;
	g_Hud.on = 0;
	g_Hud.numown = 0;
	g_Hud.moddir = modloaderGetStageModDirIndex(stagenum);

	if (!modloaderStageIsMission(stagenum) && !(g_GexPlusMode && modloaderStageIsRemake(stagenum))) {
		return;
	}

	geHudMigrateSightAlways();

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

	hudOwnIconsLoad();

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

/**
 * A string of GoldenEye's LpropobjE, the pickups' own bank, which the
 * conversion copies to the mod's menu/ with the watch's (geconvert.c's
 * g_WatchLang); NULL when it is not there. Loaded once for each mod folder
 * and kept: it is a few hundred bytes.
 */
const char *geHudPropobjString(s32 slot)
{
	static u8 *bank;
	static u32 banklen;
	static s32 bankdir = -2;
	const s32 moddir = modloaderGetStageModDirIndex(g_Vars.stagenum);
	u32 at;

	if (moddir != bankdir) {
		char path[FS_MAXPATH + 1];
		const char *dir = moddir >= 0 ? fsGetModDirAt(moddir) : NULL;

		sysMemFree(bank);
		bank = NULL;
		banklen = 0;
		bankdir = moddir;

		if (dir) {
			snprintf(path, sizeof(path), "%s/menu/LpropobjE", dir);
			bank = fsFileLoad(path, &banklen);
		}
	}

	if (!bank || slot < 0 || (u32)(slot + 1) * 4 > banklen) {
		return NULL;
	}

	at = ((u32)bank[slot * 4] << 24) | ((u32)bank[slot * 4 + 1] << 16) | ((u32)bank[slot * 4 + 2] << 8) | bank[slot * 4 + 3];

	if (at == 0 || at >= banklen || memchr(bank + at, 0, banklen - at) == NULL) {
		return NULL;
	}

	// the selected language's, keyed ge.propobj.<slot> (langpack.h)
	if (langpackActive()) {
		const char *tr = langpackGe("propobj", slot);

		if (tr) {
			return tr;
		}
	}

	return langpackNoted((const char *)bank + at);
}

/**
 * GoldenEye's sight with the gun lowered is Perfect Dark's Always Show Target
 * (the user's call, 2026-10-01; F3 20261001-065320): the watch's SIGHT
 * ON-SCREEN is GoldenEye's own option, which on the cartridge shows the sight
 * only while aiming (gunsightmode's GUNSIGHTREASON_NOTAIMING, read in ares),
 * and Always Show Target keeps it up with the gun lowered too (sight.c). The
 * separate "GE Plus: Crosshair When Not Aiming" toggle (Mod.GePlusSightAlways)
 * is gone: a pd.ini that still has it on turns Always Show Target on in every
 * player's options the first time a GE Plus level starts, and the key goes.
 */
static s32 g_GeSightAlwaysRetired = 0;

PD_CONSTRUCTOR static void geHudConfigInit(void)
{
	configRegisterIntRetired("Mod.GePlusSightAlways", &g_GeSightAlwaysRetired, 0, 1);
}

static void geHudMigrateSightAlways(void)
{
	if (!g_GeSightAlwaysRetired) {
		return;
	}

	for (s32 i = 0; i < ARRAYCOUNT(g_PlayerConfigsArray); i++) {
		optionsSetAlwaysShowTarget(i, true);
	}

	g_GeSightAlwaysRetired = 0;
	g_Vars.modifiedfiles |= MODFILE_GAME;
	sysLogPrintf(LOG_NOTE, "gehud: Mod.GePlusSightAlways carried over to Always Show Target");
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
static Gfx *hudStringOutlined(Gfx *gdl, s32 gothic, const char *text, s32 x, s32 halign, s32 y, s32 valign,
		s32 outline, u32 outlinecolour, u32 textcolour)
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
					gdl = gexFrontTextPrint(gdl, gothic, x + dx, y + dy, text, outlinecolour);
				}
			}
		}
	}

	return gexFrontTextPrint(gdl, gothic, x, y, text, textcolour);
}

static Gfx *hudString(Gfx *gdl, s32 gothic, const char *text, s32 x, s32 halign, s32 y, s32 valign, s32 outline)
{
	return hudStringOutlined(gdl, gothic, text, x, halign, y, valign, outline, COL_OUTLINE, COL_TEXT);
}

static Gfx *hudInteger(Gfx *gdl, s32 value, s32 x, s32 halign, s32 y, s32 valign)
{
	char buffer[12];

	// g_GunHudIntegerFormat: the newline is where textMeasure() gets a height
	// from, which the alignment about y then halves
	snprintf(buffer, sizeof(buffer), "%d\n", value);

	return hudString(gdl, 1, buffer, x, halign, y, valign, 1);
}

/**
 * A number the release's way (HUD_RELEASE_TEXT): the same view laid out on a
 * frame of 1/0.61 as many units, so the text is that much smaller and its
 * outline a unit of that frame out, and the release's glyphs at their own
 * width. x and y are on the ordinary frame; the frame is put back after.
 */
static Gfx *hudReleaseInteger(Gfx *gdl, const struct hudframe *f, s32 value, s32 x, s32 halign, s32 y)
{
	const f32 k = HUD_RELEASE_TEXT;
	char buffer[12];

	snprintf(buffer, sizeof(buffer), "%d\n", value);

	gexFrontTextFrame(viGetViewWidth() / f->sx / k, viGetViewHeight() / f->sy / k,
			viGetViewLeft(), viGetViewTop(), viGetViewWidth(), viGetViewHeight());
	gexFrontTextNaturalWidth(1);

	gdl = hudStringOutlined(gdl, 1, buffer, (s32)lroundf(x / k), halign, (s32)lroundf(y / k), 2, 1, COL_RELEASE_OUTLINE, COL_TEXT);

	gexFrontTextNaturalWidth(0);
	gexFrontTextFrame(viGetViewWidth() / f->sx, viGetViewHeight() / f->sy,
			viGetViewLeft(), viGetViewTop(), viGetViewWidth(), viGetViewHeight());

	return gdl;
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
 * Text of Perfect Dark's own screens drawn in GoldenEye's font on its levels:
 * the weapon wheel (activemenu.c's amRenderText()), whose labels were Perfect
 * Dark's Handel Gothic over GoldenEye's HUD (F3 20260930-011230, "text still
 * uses PD's font"). GoldenEye's Bank Gothic, the watch's, whose small letters
 * are small capitals as they are on the watch's inventory.
 *
 * Positions and sizes are Perfect Dark's: x in columns of g_ScaleX pixels and
 * y in rows of the frame buffer, as the wheel lays its slots out, turned into
 * the HUD's own frame (hudFrame()).
 */
void geHudTextMeasure(const char *text, s32 *width, s32 *height)
{
	struct hudframe f;
	s32 w, h;

	hudFrame(&f);
	gexFrontTextFrameDefault();
	gexFrontTextMeasure(1, text, &w, &h);

	*width = (s32)(w * f.sx / (g_ScaleX > 0 ? g_ScaleX : 1) + 0.5f);
	*height = (s32)(h * f.sy + 0.5f);
}

Gfx *geHudText(Gfx *gdl, const char *text, s32 x, s32 y, u32 colour)
{
	struct hudframe f;
	s32 gx, gy;

	hudFrame(&f);

	gx = (s32)((x * (g_ScaleX > 0 ? g_ScaleX : 1) - viGetViewLeft()) / f.sx + 0.5f);
	gy = (s32)((y - viGetViewTop()) / f.sy + 0.5f);

	gdl = gexFrontTextSetup(gdl);
	gdl = gexFrontTextPrint(gdl, 1, gx, gy, text, colour);

	return hudEnd(gdl);
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

	// generate_ammo_total_microcode() hides it only for GUN_ANIM_STATE_SWITCH_SWAP
	// and _HOLD, the moment between the guns: the old gun's rounds stay up
	// while it goes down and the new one's show as it comes up. Hiding it for
	// the whole of CHANGEGUN put the new gun's display up only once the gun
	// had finished rising (F3 20260929-093431, "ammo icons show up with a
	// delay"). LOAD is the swap: gset may already be the new gun while its
	// magazine is not.
	if (hand->state == HANDSTATE_CHANGEGUN && hand->stateminor == HANDSTATEMINOR_CHANGEGUN_LOAD) {
		return 0;
	}

	*icon = hudWeaponIcon(weaponnum);
	*noclip = g_WeaponRows[weaponnum - WEAPON_GE_FIRST].noclip;

	if (hudWeaponShowsNothing(weaponnum)) {
		return 0;
	}

	if (weaponnum == WEAPON_GE_TANKSHELLS) {
		const s32 shells = player->ammoheldarr[AMMOTYPE_1D];

		*mag = shells > 0 ? 1 : 0;
		*reserve = shells - *mag;

		return 1;
	}

	// the watch magnet's charges, one number (NO_CLIP_RELOADS)
	if (weaponnum == WEAPON_GE_WATCHMAGNET) {
		*mag = 0;
		*reserve = player->ammoheldarr[GEGADGET_MAGNET_AMMO];

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

/** The release there and its look on (F6): the release's pictures, and its layout. */
static s32 hudReleaseLook(void)
{
	return gebeanGetEnabled() && xblaMeshGetEnabled();
}

/**
 * The release lays the ammunition out on GoldenEye's 320x240 as GoldenEye
 * does, but that frame is the 4:3 middle of its 16:9 screen, not the whole
 * width: measured off its Dam in Xenia at 1280x720 (PP7, "7 93"), the round's
 * middle is 58 units in from the right of the 4:3 box and 3 pixels a unit, and
 * GoldenEye's own rule is 59. So a view's edge that is the window's edge comes
 * in by the window's width past 4:3, half of it each side; one that is not (a
 * split screen's middle) stays where it is.
 */
static void hudReleaseInsets(const struct hudframe *f, s32 *left, s32 *right)
{
	const f32 spare = (HUD_FRAME_H * videoGetAspect() - HUD_FRAME_H * 4.0f / 3.0f) / 2.0f;

	*left = 0;
	*right = 0;

	if (spare < 1.0f) {
		return;
	}

	if (viGetViewLeft() <= 0) {
		*left = (s32)(spare + 0.5f);
	}

	if (viGetViewLeft() + viGetViewWidth() >= viGetWidth()) {
		*right = (s32)(spare + 0.5f);
	}
}

/** The release's picture for one of GoldenEye's, in a config drawn over GoldenEye's box; NULL when there is none. */
static struct textureconfig *hudReleaseIcon(s32 icon, struct textureconfig *tex)
{
	s32 w, h;
	const void *tile = g_IconHdPictures[icon] ? geFolderMenuPicture(g_IconHdPictures[icon], &w, &h) : NULL;

	if (!tile) {
		return NULL;
	}

	// the renderer draws the whole picture over the config's nominal size -
	// which must be a square it can take whole: at GoldenEye's own 5x12 the
	// tile was padded out to 8x16 and only the left five eighths and the
	// bottom three quarters of the round showed. The box it is drawn over is
	// GoldenEye's whatever this is.
	memset(tex, 0, sizeof(*tex));
	tex->textureptr = (u8 *)tile;
	tex->width = HUD_RELEASE_TEXELS;
	tex->height = HUD_RELEASE_TEXELS;
	tex->format = G_IM_FMT_RGBA;
	tex->depth = G_IM_SIZ_32b;
	tex->s = G_TX_CLAMP;
	tex->t = G_TX_CLAMP;

	return tex;
}

Gfx *geHudRenderAmmo(Gfx *gdl)
{
	struct hudframe f;
	const s32 playercount = PLAYERCOUNT();
	const s32 release = hudReleaseLook();
	s32 leftx = 59;
	s32 rightx = 59;
	s32 bottom;

	hudFrame(&f);
	bottom = f.height;

	if (release) {
		s32 insetl, insetr;

		hudReleaseInsets(&f, &insetl, &insetr);
		leftx += insetl;
		rightx += insetr;
		bottom -= HUD_RELEASE_RAISE;
	}

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
		s32 width, height, yoffset;
		s32 x0, y0;
		struct textureconfig *tex;
		struct textureconfig hd;

		if (!hudHandAmmo(handnum, &icon, &mag, &reserve, &noclip)) {
			continue;
		}

		// microcode_generation_ammo_related(): an odd picture sits half a unit
		// right of its middle and half a unit up, which is what keeps its
		// edges on whole units
		struct textureconfig *own = hudIconBox(icon, &width, &height, &yoffset);

		tex = release && icon != ICON_NONE && icon < ICON_OWN ? hudReleaseIcon(icon, &hd) : NULL;
		x0 = cx - width / 2;
		y0 = bottom - 20 + yoffset - (height + 1) / 2;

		// GoldenEye's own a texel a pixel, the release's filtered down
		if (icon != ICON_NONE) {
			gdl = hudImage(gdl, tex ? tex : own, 2, tex == NULL, 1,
					viGetViewLeft() + x0 * f.sx, viGetViewTop() + y0 * f.sy,
					viGetViewLeft() + (x0 + width) * f.sx, viGetViewTop() + (y0 + height) * f.sy,
					tex ? tex->width : width, tex ? tex->height : height, 255, 255);
		}

		gdl = gexFrontTextSetup(gdl);

		// the right hand's magazine is on the inside of its picture and its
		// reserve on the outside, and the left hand's are the other way round
		if (release) {
			const s32 y = bottom - 19;

			if (!noclip) {
				if (left) {
					gdl = hudReleaseInteger(gdl, &f, mag, cx + width / 2 + 3, 1, y);
				} else {
					gdl = hudReleaseInteger(gdl, &f, mag, cx - width / 2 - 4, 0, y);
				}
			}

			if (reserve > 0 || noclip) {
				if (left) {
					gdl = hudReleaseInteger(gdl, &f, reserve, cx - (width + 1) / 2 - 4, 0, y);
				} else {
					gdl = hudReleaseInteger(gdl, &f, reserve, cx + (width + 1) / 2 + 3, 1, y);
				}
			}

			continue;
		}

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

/**
 * What the watch's mission status shows beside the gun: its rounds, taken from
 * the hand while the gun is still in it (Perfect Dark's lowering puts the
 * magazine back in the reserve), and for a gun only picked on the watch the
 * magazine it will be loaded with.
 */
s32 geHudWatchAmmo(s32 weaponnum, s32 *mag, s32 *reserve)
{
	const struct player *player = g_Vars.currentplayer;
	const struct hand *hand = &player->hands[HAND_RIGHT];
	s32 icon, noclip, type, clip, total;

	if (weaponnum < WEAPON_GE_FIRST || weaponnum >= NUM_WEAPONS || hudWeaponShowsNothing(weaponnum)) {
		return 0;
	}

	if (hand->inuse && hand->gset.weaponnum == weaponnum && hudHandAmmo(HAND_RIGHT, &icon, mag, reserve, &noclip)) {
		return 1;
	}

	type = botactGetAmmoTypeByFunction(weaponnum, FUNC_PRIMARY);

	if (type < 0) {
		return 0;
	}

	total = player->ammoheldarr[type];
	clip = botactGetClipCapacityByFunction(weaponnum, FUNC_PRIMARY);

	if (g_WeaponRows[weaponnum - WEAPON_GE_FIRST].noclip || clip <= 0) {
		*mag = 0;
		*reserve = total;
	} else {
		*mag = total < clip ? total : clip;
		*reserve = total - *mag;
	}

	return 1;
}

/**
 * gunDrawWatchAmmoDisplay(): the ammunition's picture with its bottom at y 180
 * and its middle at x 200 of GoldenEye's frame, the magazine ending four units
 * left of it and the reserve starting three right of it, both in the watch's
 * green with no outline, their middles at y 177. The caller has set the text
 * frame; (ox, oy) and (sx, sy) put a point of that frame on the frame buffer.
 */
Gfx *geHudRenderWatchAmmo(Gfx *gdl, s32 weaponnum, s32 mag, s32 reserve, f32 ox, f32 oy, f32 sx, f32 sy)
{
	s32 icon, noclip, width, height, yoffset;
	struct textureconfig *own;
	f32 cx, cy;
	char buffer[12];
	s32 w, h;

	if (!g_Hud.on || weaponnum < WEAPON_GE_FIRST || weaponnum >= NUM_WEAPONS) {
		return gdl;
	}

	icon = hudWeaponIcon(weaponnum);
	noclip = g_WeaponRows[weaponnum - WEAPON_GE_FIRST].noclip;

	if (hudWeaponShowsNothing(weaponnum)) {
		return gdl;
	}

	own = hudIconBox(icon, &width, &height, &yoffset);
	cx = 200.0f + (width * 0.5f - (f32)(width / 2));
	cy = 180.0f - height * 0.5f;

	if (icon != ICON_NONE) {
		gdl = hudImage(gdl, own, 2, 1, 1,
				ox + (cx - width * 0.5f) * sx, oy + (cy - height * 0.5f) * sy,
				ox + (cx + width * 0.5f) * sx, oy + (cy + height * 0.5f) * sy,
				width, height, 255, 255);
	}

	gdl = gexFrontTextSetup(gdl);

	if (!noclip) {
		snprintf(buffer, sizeof(buffer), "%d\n", mag);
		gexFrontTextMeasure(1, buffer, &w, &h);
		gdl = gexFrontTextPrint(gdl, 1, 196 - width / 2 - w, 177 + h / 2 - h, buffer, geWatchTint(0x00ff00b0));
	} else {
		reserve += mag;
	}

	if (reserve > 0 || noclip) {
		snprintf(buffer, sizeof(buffer), "%d\n", reserve);
		gexFrontTextMeasure(1, buffer, &w, &h);
		gdl = gexFrontTextPrint(gdl, 1, 203 + (width + 1) / 2, 177 + h / 2 - h, buffer, geWatchTint(0x00ff00b0));
	}

	return gdl;
}

/** Whether the release's own sight picture is there to draw (the HD look). */
s32 geHudHasHdSight(void)
{
	s32 w, h;

	return geFolderMenuPicture(SIGHT_HD_PICTURE, &w, &h) != NULL;
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

	gSPMatrix(gdl++, osVirtualToPhysical(camGetPerspectiveMtxL()), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_PROJECTION | CAM_PROJ_MTX_FLAGS);

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

/**
 * Perfect Dark's mission timer on GoldenEye's HUD. GoldenEye has no clock of
 * its own up while playing, so this is set in its countdown's figures: Bank
 * Gothic outlined in grey, a digit to eight units and nine either side of a
 * colon (geHudRenderCountdown()), at the bottom messages' left margin.
 *
 * It stands in the lower left corner, under the bottom messages' own line
 * (BONDVIEW_VIEW_TOP_OFFSET_1, 0x0c over the bottom), where GoldenEye draws
 * nothing, so the messages and the opening's lines stay where GoldenEye puts
 * them (F3 20261001-121337: the timer stood on the raised line, "far above
 * the lower-left corner", and pushed "weapon pickup text and starting
 * cutscene subtitles" up over it). With a gun in the left hand that corner is
 * its ammunition's, which Perfect Dark's green timer sat over (F3
 * 20260930-211848), so the timer goes up to the raised line the messages
 * take over the left hand's ammunition (BONDVIEW_VIEW_TOP_OFFSET_2), and only
 * then do the messages stack over it.
 */
static s32 hudTimerRaised(void)
{
	return g_Vars.currentplayer->hands[HAND_LEFT].inuse;
}

static s32 hudTimerBottom(const struct hudframe *f)
{
	s32 y;

	if (hudTimerRaised()) {
		y = f->height - 0x28;

		// the release's ammunition stands higher, and the line with it
		if (hudReleaseLook()) {
			y -= HUD_RELEASE_RAISE;
		}
	} else {
		y = f->height - 2;
	}

	if (PLAYERCOUNT() < 3 && g_Vars.currentplayernum == 1) {
		y -= 8;
	}

	return y;
}

static s32 hudTimerHeight(void)
{
	s32 w, h;

	gexFrontTextMeasure(1, "0\n", &w, &h);

	return hudReleaseLook() ? (s32)lroundf(h * HUD_RELEASE_TEXT) : h;
}

void geHudSetMissionTimerShown(s32 shown)
{
	g_Hud.timershown = shown;
}

/**
 * The timer's columns: a digit's pitch and a colon's distance from the digit
 * either side of it. GoldenEye's own figures keep its countdown's, eight and
 * nine. The release's font is drawn at its natural widths, which the
 * countdown's columns are too narrow for - each digit ran into the next
 * (F3 20261001-121337, "digits are misaligned") - so its columns come from
 * the widest digit, still the same for every digit, so the figures do not
 * shift as they change.
 */
static void hudTimerPitch(s32 release, s32 *digit, s32 *colon)
{
	char glyph[3] = { '0', '\n', '\0' };
	s32 widest = 0;
	s32 w, h;

	if (!release) {
		*digit = 8;
		*colon = 9;
		return;
	}

	for (char c = '0'; c <= '9'; c++) {
		glyph[0] = c;
		gexFrontTextMeasure(1, glyph, &w, &h);

		if (w > widest) {
			widest = w;
		}
	}

	glyph[0] = ':';
	gexFrontTextMeasure(1, glyph, &w, &h);

	// a unit of outline either side of each, and a unit between
	*digit = widest + 3;
	*colon = (widest + w + 1) / 2 + 3;
}

/**
 * One of the timer's strings, a character to a column (left to right from x,
 * on the frame the text is set to), ending at y. Returns the x past it.
 */
static s32 hudTimerColumns(const char *text, s32 x, s32 digit, s32 colon, s32 *centres, s32 max)
{
	s32 n = 0;

	for (s32 i = 0; text[i] && n < max; i++) {
		const s32 iscolon = text[i] == ':';

		if (i == 0) {
			x += iscolon ? (colon + 1) / 2 : digit / 2;
		} else {
			x += iscolon || text[i - 1] == ':' ? colon : digit;
		}

		centres[n++] = x;
	}

	return x + digit / 2;
}

static Gfx *hudTimerString(Gfx *gdl, const char *text, s32 x, s32 y, s32 digit, s32 colon, u32 outline, u32 colour,
		s32 *endx)
{
	s32 centres[24];
	const s32 n = (s32)strlen(text);

	*endx = hudTimerColumns(text, x, digit, colon, centres, 24);

	for (s32 i = 0; i < n && i < 24; i++) {
		char glyph[3] = { text[i], '\n', '\0' };

		gdl = hudStringOutlined(gdl, 1, glyph, centres[i], 2, y, 0, 1, outline, colour);
	}

	return gdl;
}

Gfx *geHudRenderMissionTimer(Gfx *gdl, s32 time60, s32 hassplit, s32 split60)
{
	struct hudframe f;
	const s32 release = hudReleaseLook();
	const f32 k = release ? HUD_RELEASE_TEXT : 1.0f;
	const u32 outline = release ? COL_RELEASE_OUTLINE : COL_OUTLINE;
	char buffer[24];
	s32 x = 0x1e;
	s32 y;
	s32 endx;
	s32 digit;
	s32 colon;

	hudFrame(&f);

	if (PLAYERCOUNT() >= 3 && (g_Vars.currentplayernum & 1)) {
		x = 0xa;
	}

	y = hudTimerBottom(&f);

	formatTime(buffer, time60, TIMEPRECISION_HUNDREDTHS);

	if (release) {
		gexFrontTextFrame(viGetViewWidth() / f.sx / k, viGetViewHeight() / f.sy / k,
				viGetViewLeft(), viGetViewTop(), viGetViewWidth(), viGetViewHeight());
		gexFrontTextNaturalWidth(1);
	}

	hudTimerPitch(release, &digit, &colon);

	x = (s32)lroundf(x / k);
	y = (s32)lroundf(y / k);

	gdl = gexFrontTextSetup(gdl);
	gdl = hudTimerString(gdl, buffer, x, y, digit, colon, outline, COL_TEXT, &endx);

	// the ghost's split beside it, green ahead of the ghost and red behind
	// (hudmsgRenderMissionTimer()'s colours)
	if (hassplit) {
		buffer[0] = split60 < 0 ? '-' : '+';
		formatTime(buffer + 1, split60 < 0 ? -split60 : split60, TIMEPRECISION_HUNDREDTHS);

		gdl = hudTimerString(gdl, buffer, endx + 6, y, digit, colon, outline,
				split60 < 0 ? 0x40ff40ff : 0xff6040ff, &endx);
	}

	if (release) {
		gexFrontTextNaturalWidth(0);
	}

	return hudEnd(gdl);
}

s32 geHudMessageDuration(s32 top)
{
	// BONDVIEW_UPPER_TEXT_TIMER_C and BONDVIEW_INTRO_CAMERA_BONDMESSCNT_C
	return top ? 0xf0 : 0x78;
}

/**
 * `intro` is an opening shot's caption: GoldenEye prints those at the bottom
 * left too, but in Zurich Bold, the top message's font
 * (bondviewFrozenCameraTick(): setFontTables(ptrFontZurichBoldChars, ...)
 * before hudmsgBottomShow(); F3 20260928-210334).
 */
Gfx *geHudRenderMessage(Gfx *gdl, const char *text, s32 top, s32 intro, s32 *row)
{
	struct hudframe f;
	char wrapped[512];
	char ended[512];
	const s32 gothic = top || intro ? 0 : 1;
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

		// and over the mission timer when the left hand's ammunition has put
		// it on that raised line itself
		if (g_Hud.timershown && hudTimerRaised()) {
			const s32 over = hudTimerBottom(&f) - hudTimerHeight() - 2;

			if (y > over) {
				y = over;
			}
		}
	} else {
		y = 0x10 + h;
	}

	y -= *row;
	*row += h + 2;

	gdl = gexFrontTextSetup(gdl);
	gdl = hudString(gdl, gothic, text, x, 1, y, 0, 1);

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
