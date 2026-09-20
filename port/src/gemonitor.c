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
		g_GeMonImages[i].level = p[6];
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

/** A picture as the file has it: its texture's number and its config's fields. */
const struct textureconfig *geMonitorImageInfo(u32 index, u32 *texturenum)
{
	if (!g_GeMonWords || index >= (u32)g_GeMonNumImages) {
		return NULL;
	}

	*texturenum = g_GeMonImageNums[index];

	return &g_GeMonImages[index];
}

u32 *geMonitorJump(u32 *cmdlist, u32 arg)
{
	// not asked of the stage: the folder runs these lists over the Institute
	if (!g_GeMonWords || cmdlist < g_GeMonWords || cmdlist >= g_GeMonWords + g_GeMonNumWords) {
		return NULL;
	}

	return g_GeMonWords + (arg < g_GeMonNumWords ? arg : 0);
}

struct textureconfig *geMonitorImage(u32 index)
{
	if ((!g_GeMonOn && !g_GeMonFolder) || index >= (u32)g_GeMonNumImages) {
		return NULL;
	}

	return &g_GeMonImages[index];
}

#endif
