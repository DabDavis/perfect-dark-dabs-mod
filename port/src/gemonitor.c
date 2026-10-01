/**
 * GoldenEye's TVs and its big projection screens, on a converted mission.
 *
 * A GoldenEye monitor runs a little programme - use this image, scroll it,
 * hold, zoom, tint, jump to another programme one time in ten - and Perfect
 * Dark's tvscreen is the same machine: the sixteen commands have the same
 * numbers, arguments and widths in both games (GoldenEye's MON* macros in
 * chrai.h against tvcmds.h), and tvscreenTick() is GoldenEye's tick. What
 * differs is the data. A record's image number picks a programme out of
 * GoldenEye's own fifty-two and a programme's pictures are an index into
 * GoldenEye's own fifty, and Perfect Dark has tables of its own for both - so
 * until converter 44 a record's number was not even carried, and every screen
 * in a converted level ran Perfect Dark's programme 0 over Perfect Dark's
 * pictures. The user: "we are also missing the big projection screens for ge
 * plus, it uses pd".
 *
 * The conversion writes the programmes and the picture table as
 * menu/gemonitors.bin (geconvert.c, gemonitortable.h). Three places in
 * propobj.c ask here before they use Perfect Dark's own: which programme a
 * number means, where a jump goes, and which picture an index means.
 *
 * A picture is loaded the first time it is drawn (texSelect()), which turns
 * its number into a pointer into the stage's texture pool - so the table is
 * written afresh from the file's numbers on every load.
 */
#include <stdio.h>
#include <string.h>
#include <ultra64.h>
#include "constants.h"
#include "types.h"
#include "bss.h"
#include "data.h"
#include "fs.h"
#include "system.h"
#include "modloader.h"
#include "gemonitor.h"
#include "game/texdecompress.h"
#include "gebean.h"
#include "video.h"
#include "xblamesh.h"
#include "xblatex.h"

#ifndef PLATFORM_N64

#define GEMON_MAX_PROGRAMS 64
#define GEMON_MAX_IMAGES   100   // an index under 100 is a picture (tvscreenRender)

static u32 *g_GeMonWords;                       // the programmes, in this machine's byte order
static u32 g_GeMonNumWords;
static u32 g_GeMonPrograms[GEMON_MAX_PROGRAMS]; // the word each one starts at
static s32 g_GeMonNumPrograms;
static struct textureconfig g_GeMonImages[GEMON_MAX_IMAGES];
static u32 g_GeMonImageNums[GEMON_MAX_IMAGES];  // as the file has them
static s32 g_GeMonNumImages;
static s32 g_GeMonModDir = -2;                  // the mod the tables were read from
static s32 g_GeMonOn;                           // this stage plays them
static s32 g_GeMonFolder;                       // or the folder's page is showing them

/**
 * The release's own pictures for GoldenEye's fifty (fidelity H4; the user,
 * 2026-10-01: in the HD look the screens show what the release's show). The
 * release runs GoldenEye's programmes unchanged - its screens scroll, cycle
 * and tint as ours do - over pictures of its own, files/new/texture/monitors/,
 * kept under the names assets/oddtextures.c gives GoldenEye's: the scrolling
 * text (screennew2) is a list of the release's staff names where GoldenEye's
 * is lines of dashes, Dam's modem showing it in a Xenia capture of the
 * release. In the HD look each picture is bound at the loaded texture's
 * address, as geimpact.c binds the bullet holes; the N64 look keeps the ROM's.
 */
static const struct { u16 image; const char *bean; } g_GeMonBean[] = {
	{ 2187, "bondlogo" },        // IMAGE_MONITOR_BOND
	{ 2188, "cyrlocation" },     // IMAGE_MONITOR_LOCATION
	{ 2189, "cyrweaponarmed" },  // IMAGE_MONITOR_BEGINARMING
	{ 2190, "cyrtarget" },       // IMAGE_MONITOR_TARGET
	{ 2191, "cyrsevernaya" },    // IMAGE_MONITOR_SEVERNAYA
	{ 2192, "cyrtimetotarget" }, // IMAGE_MONITOR_BREAKTARGET
	{ 2193, "cyrxhairspanel" },  // IMAGE_MONITOR_AIMER
	{ 2194, "screen1" },         // IMAGE_MONITOR_EARTH
	{ 2195, "screen2" },         // IMAGE_MONITOR_DESKTOPBANG
	{ 2196, "screen3" },         // IMAGE_MONITOR_HEATMAP
	{ 2197, "screen4" },         // IMAGE_MONITOR_3DMATH
	{ 1185, "screen5" },         // IMAGE_MONITOR_DESKTOPBARS
	{ 2198, "screen6" },         // IMAGE_MONITOR_2DMATH
	{ 2199, "screen7" },         // IMAGE_MONITOR_SATELLITE
	{ 1186, "screen8" },         // IMAGE_MONITOR_DESKTOP
	{ 1187, "screen9" },         // IMAGE_MONITOR_DESKTOPSTAGGERED
	{ 2200, "anim2.0" },         // IMAGE_MONITOR_CUBE1
	{  582, "drax1" },           // IMAGE_MONITOR_SHUTTLE1
	{  583, "drax2" },           // IMAGE_MONITOR_SHUTTLE2
	{  584, "drax3" },           // IMAGE_MONITOR_EARTHFULL1
	{ 2201, "drax4" },           // IMAGE_MONITOR_EARTHFULL2
	{ 2202, "drax5" },           // IMAGE_MONITOR_BLUESTARS
	{ 2203, "drax6" },           // IMAGE_MONITOR_GALAXY1
	{ 2204, "drax7" },           // IMAGE_MONITOR_GALAXY2
	{  581, "drax10" },          // IMAGE_MONITOR_EARTHTEXT
	{ 2205, "drax11" },          // IMAGE_MONITOR_TARGETEARTH
	{ 2206, "drax12" },          // IMAGE_MONITOR_GALAXY3
	{ 2227, "screenstatic" },    // IMAGE_MONITOR_STATIC
	{ 2223, "screennew1" },      // IMAGE_MONITOR_SINE
	{ 2224, "screennew2" },      // IMAGE_MONITOR_TEXT
	{ 2225, "screennew3" },      // IMAGE_MONITOR_BARS
	{ 2226, "screennew4" },      // IMAGE_MONITOR_SQUARES
	{ 2219, "anim4.1" },         // IMAGE_MONITOR_FIST1
	{ 2220, "anim4.2" },         // IMAGE_MONITOR_FIST2
	{ 2221, "anim4.3" },         // IMAGE_MONITOR_FIST3
	{ 2222, "anim4.4" },         // IMAGE_MONITOR_FIST4
	{ 2218, "anim5.1" },         // IMAGE_MONITOR_SKATEBOARD4
	{ 2207, "anim5.2" },         // IMAGE_MONITOR_SKATEBOARD1
	{ 2208, "anim5.3" },         // IMAGE_MONITOR_SKATEBOARD2
	{ 2209, "anim5.4" },         // IMAGE_MONITOR_SKATEBOARD3
	{ 2210, "anim3.1" },         // IMAGE_MONITOR_TALK1
	{ 2211, "anim3.2" },         // IMAGE_MONITOR_TALK2
	{ 2212, "anim3.3" },         // IMAGE_MONITOR_TALK3
	{ 2213, "anim3.4" },         // IMAGE_MONITOR_TALK4
	{ 2214, "earthmap" },        // IMAGE_MONITOR_WORLDMAP
	{ 2215, "anim2.1" },         // IMAGE_MONITOR_CUBE2
	{ 2216, "anim2.2" },         // IMAGE_MONITOR_CUBE3
	{ 2217, "anim2.3" },         // IMAGE_MONITOR_CUBE4
	{ 2263, "animradar" },       // IMAGE_MONITOR_TRIANGLE
	{  837, "keyboardkey" },     // IMAGE_MONITOR_KEYBOARDKEY
};

// the release's picture bound at each loaded picture's address, and the look
// it was bound for
static const void *g_GeMonBound[GEMON_MAX_IMAGES];
static s32 g_GeMonBoundHd[GEMON_MAX_IMAGES];

/** Last stage's pictures were pointers into last stage's pool: forget what was bound there. */
static void geMonitorForgetPictures(void)
{
	for (s32 i = 0; i < GEMON_MAX_IMAGES; i++) {
		if (g_GeMonBound[i]) {
			xblaTexForgetPicture(g_GeMonBound[i]);
			g_GeMonBound[i] = NULL;
		}

		g_GeMonBoundHd[i] = 0;
	}
}

static u32 monBe32(const u8 *p)
{
	return ((u32)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

static s32 geMonitorLoadFrom(s32 moddir, const char *dir);

static s32 geMonitorLoad(s32 stagenum)
{
	return geMonitorLoadFrom(modloaderGetStageModDirIndex(stagenum), modloaderGetStageModDir(stagenum));
}

static s32 geMonitorLoadFrom(s32 moddir, const char *dir)
{
	char path[FS_MAXPATH + 1];
	u32 len = 0;
	u8 *d;
	u32 numprogs, numimages, numwords;
	const u8 *p;

	if (moddir < 0 || !dir) {
		return 0;
	}

	if (g_GeMonModDir == moddir) {
		return g_GeMonWords != NULL;
	}

	g_GeMonModDir = moddir;
	sysMemFree(g_GeMonWords);
	g_GeMonWords = NULL;
	g_GeMonNumWords = 0;

	snprintf(path, sizeof(path), "%s/menu/gemonitors.bin", dir);
	d = fsFileLoad(path, &len);

	if (!d || len < 12 || memcmp(d, "GEM1", 4)) {
		sysLogPrintf(LOG_WARNING, "gemonitor: the conversion has no monitor programmes at %s", path);
		sysMemFree(d);
		return 0;
	}

	numprogs = (d[4] << 8) | d[5];
	numimages = (d[6] << 8) | d[7];
	numwords = monBe32(d + 8);

	if (numprogs > GEMON_MAX_PROGRAMS || numimages > GEMON_MAX_IMAGES
			|| len < 12 + 4 * numprogs + 12 * numimages + 4 * (u64)numwords) {
		sysMemFree(d);
		return 0;
	}

	p = d + 12;

	for (u32 i = 0; i < numprogs; i++, p += 4) {
		g_GeMonPrograms[i] = monBe32(p) < numwords ? monBe32(p) : 0;
	}

	for (u32 i = 0; i < numimages; i++, p += 12) {
		g_GeMonImageNums[i] = monBe32(p);
		g_GeMonImages[i].width = p[4];
		g_GeMonImages[i].height = p[5];
		// Five levels at most, as texLoad() keeps: GoldenEye's loader keeps a
		// picture's six and Perfect Dark's stops at five, while texSelect()
		// looks for the palette past as many levels as the config names. A
		// six read it 8 bytes late - four colours off along the ramp and the
		// last four from past its end - and put a blue smear across the
		// keyboard key every door console's lamp is made of
		g_GeMonImages[i].level = p[6] > 5 ? 5 : p[6];
		g_GeMonImages[i].format = p[7];
		g_GeMonImages[i].depth = p[8];
		g_GeMonImages[i].s = p[9];
		g_GeMonImages[i].t = p[10];
	}

	g_GeMonWords = sysMemZeroAlloc(sizeof(u32) * (numwords + 1));

	if (!g_GeMonWords) {
		sysMemFree(d);
		return 0;
	}

	for (u32 i = 0; i < numwords; i++, p += 4) {
		g_GeMonWords[i] = monBe32(p);
	}

	// a block that ran off its end would run into a restart
	g_GeMonWords[numwords] = TVCMD_RESTART;

	g_GeMonNumPrograms = numprogs;
	g_GeMonNumImages = numimages;
	g_GeMonNumWords = numwords;
	sysMemFree(d);

	sysLogPrintf(LOG_NOTE, "gemonitor: %d of GoldenEye's monitor programmes over %d pictures", (s32)numprogs, (s32)numimages);

	return 1;
}

void geMonitorStageStart(s32 stagenum)
{
	g_GeMonFolder = 0;

	g_GeMonOn = modloaderStageIsMission(stagenum) && geMonitorLoad(stagenum);
	geMonitorForgetPictures();

	if (g_GeMonOn) {
		// last level's pictures were pointers into last level's pool
		for (s32 i = 0; i < g_GeMonNumImages; i++) {
			g_GeMonImages[i].texturenum = g_GeMonImageNums[i];
			g_GeMonImages[i].unk0b = 0;
		}
	}
}

u32 *geMonitorProgram(s32 imagenum)
{
	if (!g_GeMonOn) {
		return NULL;
	}

	if (imagenum < 0 || imagenum >= g_GeMonNumPrograms) {
		imagenum = 0;   // GoldenEye's default: the Bond logo
	}

	return g_GeMonWords + g_GeMonPrograms[imagenum];
}

/**
 * For GE Plus's folder, which shows the programmes on a page of their own
 * (gexfront.c) and is not on a remake stage when it does: the tables out of the
 * conversion in that mod directory, and how many programmes there are.
 */
s32 geMonitorOpen(s32 moddir, const char *dir)
{
	if (!geMonitorLoadFrom(moddir, dir)) {
		return 0;
	}

	// The page draws them on GoldenEye's own TV props through
	// tvscreenRender(), which asks here for its pictures - over the Institute,
	// which is no remake stage. Any the last visit loaded were pointers into
	// the pool of whatever stage that was.
	g_GeMonFolder = 1;
	geMonitorForgetPictures();

	for (s32 i = 0; i < g_GeMonNumImages; i++) {
		g_GeMonImages[i].texturenum = g_GeMonImageNums[i];
		g_GeMonImages[i].unk0b = 0;
	}

	return g_GeMonNumPrograms;
}

void geMonitorClose(void)
{
	g_GeMonFolder = 0;
}

u32 *geMonitorProgramAt(s32 n)
{
	if (!g_GeMonWords || n < 0 || n >= g_GeMonNumPrograms) {
		return NULL;
	}

	return g_GeMonWords + g_GeMonPrograms[n];
}

u32 *geMonitorJump(u32 *cmdlist, u32 arg)
{
	// not asked of the stage: the folder runs these lists over the Institute
	if (!g_GeMonWords || cmdlist < g_GeMonWords || cmdlist >= g_GeMonWords + g_GeMonNumWords) {
		return NULL;
	}

	return g_GeMonWords + (arg < g_GeMonNumWords ? arg : 0);
}

/**
 * Asked of the list the screen is running, as a jump is, and not only of where
 * the game is: GE Plus's folder shows these over the Institute, whose own
 * screens are still drawn under it running Perfect Dark's programmes. Handed
 * this table they loaded Perfect Dark's texture of a GoldenEye picture's number
 * into it - whichever asked first - and the page's sets then drew that: the
 * scrolling text was one of the Institute's walls, tinted green.
 */
/**
 * The picture at index loaded, and the release's own bound over it in the HD
 * look (g_GeMonBean) or the ROM's put back in the N64 look; F6 flips the look
 * under a screen already showing.
 */
static void geMonitorBindPicture(s32 i)
{
	struct textureconfig *tc = &g_GeMonImages[i];
	const s32 hd = gebeanGetEnabled() && xblaMeshGetEnabled();
	const char *bean = NULL;

	if ((u32)tc->texturenum < NUM_TEXTURES) {
		texLoadFromConfigs(tc, 1, NULL, 0);
	}

	if (tc->unk0b != 1 || hd == g_GeMonBoundHd[i]) {
		return;
	}

	g_GeMonBoundHd[i] = hd;

	if (g_GeMonBound[i]) {
		xblaTexForgetPicture(g_GeMonBound[i]);
		g_GeMonBound[i] = NULL;
	}

	for (s32 k = 0; k < (s32)ARRAYCOUNT(g_GeMonBean); k++) {
		if (g_GeMonBean[k].image == g_GeMonImageNums[i]) {
			bean = g_GeMonBean[k].bean;
			break;
		}
	}

	if (hd && bean) {
		char source[64];
		s32 w = 0, h = 0;
		u8 *rgba;

		snprintf(source, sizeof(source), "new/texture/monitors/%s", bean);
		rgba = gebeanDecodePictureFile(source, &w, &h);

		// taken over by the registry, or freed there
		if (rgba && xblaTexBindPictureAt(tc->textureptr, rgba, w, h)) {
			g_GeMonBound[i] = tc->textureptr;
		}
	}

	// the renderer keeps a texture by the address it was uploaded from
	videoFreeCachedTexture(tc->textureptr);
}

struct textureconfig *geMonitorImage(u32 *cmdlist, u32 index)
{
	if ((!g_GeMonOn && !g_GeMonFolder) || index >= (u32)g_GeMonNumImages) {
		return NULL;
	}

	if (!g_GeMonWords || cmdlist < g_GeMonWords || cmdlist >= g_GeMonWords + g_GeMonNumWords) {
		return NULL;
	}

	geMonitorBindPicture((s32)index);

	return &g_GeMonImages[index];
}

#endif
