#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <ultra64.h>
#include <PR/ultratypes.h>
#include "constants.h"
#include "types.h"
#include "data.h"
#include "bss.h"
#include "lib/main.h"
#include "lib/memp.h"
#include "game/bg.h"
#include "game/gfxmemory.h"
#include "platform.h"
#include "config.h"
#include "fs.h"
#include "input.h"
#include "system.h"
#include "video.h"
#include "screenshot.h"
#include "texpack.h"
#include "xblamesh.h"
#include "xblatex.h"
#include "xblastage.h"
#include "trace.h"
#include "../fast3d/gfx_api.h"

#define TRACE_DIR_NAME "traces"
#define TRACE_KEYNAME_LEN 32
#define TRACE_DEFAULT_KEY "F3"
#define TRACE_MAX_DUPES 100

static char keyName[TRACE_KEYNAME_LEN] = TRACE_DEFAULT_KEY;
static s32 keyVk = -1;
static bool pending;

s32 traceGetKey(void)
{
	if (keyVk < 0) {
		if (!keyName[0] || !strcmp(keyName, "NONE")) {
			keyVk = 0;
		} else {
			keyVk = inputGetKeyByName(keyName);

			if (keyVk < 0) {
				keyVk = 0;
			}
		}
	}

	return keyVk;
}

void traceRequest(void)
{
	pending = true;

	// The picture of the same frame, under the same stamp.
	screenshotRequest();
}

void traceTick(void)
{
	const s32 vk = traceGetKey();

	if (vk > 0 && inputKeyJustPressed(vk)) {
		traceRequest();
	}
}

void traceChrNote(struct chrdata *chr, u8 bit)
{
	if (!chr) {
		return;
	}

	if (chr->tracedrawframe != (u32)g_Vars.lvframenum) {
		chr->tracedrawframe = (u32)g_Vars.lvframenum;
		chr->tracedrawbits = 0;
		chr->tracedrawalpha = 0;
	}

	chr->tracedrawbits |= bit;
}

static bool tracePickFilename(char *out, u32 outSize)
{
	static char dir[FS_MAXPATH + 1];
	const time_t now = time(NULL);
	const struct tm *lt = localtime(&now);
	char stamp[32];
	s32 dupe;

	if (!dir[0] && fsChooseOutputDir(TRACE_DIR_NAME, dir, sizeof(dir)) != 0) {
		sysLogPrintf(LOG_ERROR, "trace: nowhere to put %s that can be written", TRACE_DIR_NAME);
		return false;
	}

	if (lt) {
		strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", lt);
	} else {
		snprintf(stamp, sizeof(stamp), "%llu", (unsigned long long)now);
	}

	for (dupe = 1; dupe <= TRACE_MAX_DUPES; dupe++) {
		char rel[FS_MAXPATH + 1];

		if (dupe == 1) {
			snprintf(rel, sizeof(rel), "%s/pd-%s.txt", dir, stamp);
		} else {
			snprintf(rel, sizeof(rel), "%s/pd-%s-%d.txt", dir, stamp, dupe);
		}

		if (fsFileSize(rel) < 0) {
			strncpy(out, fsFullPath(rel), outSize - 1);
			out[outSize - 1] = '\0';
			return true;
		}
	}

	return false;
}

static const char *traceDrawBits(u8 bits, char *buf, u32 size)
{
	buf[0] = '\0';

	if (bits == 0) {
		strncpy(buf, "not reached by chrRender", size - 1);
		buf[size - 1] = '\0';
		return buf;
	}

	if (bits & TRACECHR_DREW) strncat(buf, "DREW ", size - strlen(buf) - 1);
	if (bits & TRACECHR_CALLED_OPA) strncat(buf, "opa ", size - strlen(buf) - 1);
	if (bits & TRACECHR_CALLED_XLU) strncat(buf, "xlu ", size - strlen(buf) - 1);
	if (bits & TRACECHR_DEFERRED) strncat(buf, "deferred-to-xlu ", size - strlen(buf) - 1);
	if (bits & TRACECHR_NODRAW) strncat(buf, "NODRAW-alpha0 ", size - strlen(buf) - 1);
	if (bits & TRACECHR_BODYNODRAW) strncat(buf, "body-budget ", size - strlen(buf) - 1);
	if (bits & TRACECHR_EYESPY) strncat(buf, "eyespy ", size - strlen(buf) - 1);
	if (bits & TRACECHR_XRAYFAR) strncat(buf, "xray-far ", size - strlen(buf) - 1);

	return buf;
}

static void traceRooms(FILE *f, const RoomNum *rooms)
{
	s32 i;

	for (i = 0; i < 8 && rooms[i] != -1; i++) {
		const s32 r = rooms[i];

		if (r >= 0 && r < g_Vars.roomcount && (g_Rooms[r].flags & ROOMFLAG_ONSCREEN)) {
			struct drawslot *slot = bgGetRoomDrawSlot(r);

			fprintf(f, " %d*[%d %d %d %d]", r,
					slot ? slot->box.xmin : -1, slot ? slot->box.ymin : -1,
					slot ? slot->box.xmax : -1, slot ? slot->box.ymax : -1);
		} else {
			fprintf(f, " %d", r);
		}
	}
}

static f32 traceWrapDegrees(f32 deg)
{
	while (deg > 180.0f) deg -= 360.0f;
	while (deg < -180.0f) deg += 360.0f;
	return deg;
}

/**
 * Where a position sits relative to the camera: distance, and the angles left
 * of and above the look vector, in degrees. At FovY 60 the picture edge is
 * about 37 degrees to the side at either aspect ratio, 30 above and below.
 */
static void traceRelative(const struct player *pl, const struct coord *pos,
		f32 *dist, f32 *yaw, f32 *pitch)
{
	const f32 dx = pos->x - pl->cam_pos.x;
	const f32 dy = pos->y - pl->cam_pos.y;
	const f32 dz = pos->z - pl->cam_pos.z;
	const f32 lx = pl->cam_look.x;
	const f32 ly = pl->cam_look.y;
	const f32 lz = pl->cam_look.z;
	const f32 horiz = sqrtf(dx * dx + dz * dz);
	const f32 lhoriz = sqrtf(lx * lx + lz * lz);

	*dist = sqrtf(dx * dx + dy * dy + dz * dz);
	// The game's atan2f() answers in 0 to tau, so both are brought to +-180.
	*yaw = traceWrapDegrees(atan2f(dx * lz - dz * lx, dx * lx + dz * lz) * (180.0f / M_PI));
	*pitch = traceWrapDegrees((atan2f(dy, horiz) - atan2f(ly, lhoriz)) * (180.0f / M_PI));
}

static void traceChr(FILE *f, const struct player *pl, struct prop *prop, struct chrdata *chr)
{
	char bits[128];
	f32 dist = 0, yaw = 0, pitch = 0;

	if (pl) {
		traceRelative(pl, &prop->pos, &dist, &yaw, &pitch);
	}

	fprintf(f, "%s %3d body %3d act %2d pos (%.0f %.0f %.0f) dist %5.0f yaw %+6.1f pitch %+5.1f",
			prop->type == PROPTYPE_PLAYER ? "player" : "chr",
			chr->chrnum, chr->bodynum, chr->actiontype,
			prop->pos.x, prop->pos.y, prop->pos.z, dist, yaw, pitch);
	fprintf(f, " propflags %08x onthis %d onany %d rooms",
			prop->flags,
			(prop->flags & PROPFLAG_ONTHISSCREENTHISTICK) ? 1 : 0,
			(prop->flags & PROPFLAG_ONANYSCREENTHISTICK) ? 1 : 0);
	traceRooms(f, prop->rooms);
	fprintf(f, "\n    chrflags %08x hidden %08x fadealpha %d model %p def %p scale %.4f",
			chr->chrflags, chr->hidden, chr->fadealpha,
			(void *)chr->model, chr->model ? (void *)chr->model->definition : NULL,
			chr->model ? chr->model->scale : 0.0f);
	fprintf(f, "\n    draw: frame %u (now %d) alpha %d %s\n",
			chr->tracedrawframe, g_Vars.lvframenum, chr->tracedrawalpha,
			chr->tracedrawframe == (u32)g_Vars.lvframenum
				? traceDrawBits(chr->tracedrawbits, bits, sizeof(bits))
				: "not rendered this frame");

	if (chr->model) {
		xblaMeshTraceModel(f, chr->model, "    ");
	}
}

static void traceWrite(FILE *f)
{
	const struct player *pl = g_Vars.currentplayer;
	const time_t now = time(NULL);
	char stamp[64];
	u32 gfxused = 0, gfxsize = 0, vtxused = 0, vtxsize = 0;
	struct GfxTraceStats gs;
	s32 counts[8] = {0};
	struct prop **ptr;
	s32 i;

	if (!strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", localtime(&now))) {
		stamp[0] = '\0';
	}

	fprintf(f, "pd trace %s\nversion: %s\n", stamp, sysGetVersionString());
	fprintf(f, "stage 0x%02x lvframenum %d tickmode %d players %d window %dx%d\n",
			mainGetStageNum(), g_Vars.lvframenum, g_Vars.tickmode, PLAYERCOUNT(),
			videoGetWindowWidth(), videoGetWindowHeight());
	fprintf(f, "xbla: meshes %d stages %d meshtextures %d, rooms from release %d; texture pack replacements %d\n",
			xblaMeshGetEnabled(), xblaStageGetEnabled(), xblaTexGetEnabled(),
			xblaStageIsRelease(), texpackHaveReplacements());

	fprintf(f, "\n[memory]\n");
	fprintf(f, "memp: stage pool free onboard %u expansion %u (total %u); permanent free onboard %u expansion %u\n",
			mempGetPoolFree(MEMPOOL_STAGE, MEMBANK_ONBOARD),
			mempGetPoolFree(MEMPOOL_STAGE, MEMBANK_EXPANSION),
			mempGetStageFreeTotal(),
			mempGetPoolFree(MEMPOOL_PERMANENT, MEMBANK_ONBOARD),
			mempGetPoolFree(MEMPOOL_PERMANENT, MEMBANK_EXPANSION));
	gfxTraceGetPools(&gfxused, &gfxsize, &vtxused, &vtxsize);
	fprintf(f, "gfx pools (last frame): master list %u of %u commands, vtx pool %u of %u bytes\n",
			gfxused, gfxsize, vtxused, vtxsize);
	gfx_trace_stats(&gs);
	fprintf(f, "renderer (last frame): %u draws, %u tris, %u verts, %u distinct textures, %u uploads, %u evictions, cache %u of %u, %u buffer-full flushes\n",
			gs.drawcalls, gs.tris, gs.verts, gs.distincttextures, gs.texuploads,
			gs.texevictions, gs.cacheentries, gs.cachesize, gs.bufferfullflushes);
	xblaMeshTrace(f);
	xblaTexTrace(f);
	xblaStageTrace(f);
	texpackTrace(f);

	fprintf(f, "\n[camera]\n");

	if (pl) {
		fprintf(f, "cam pos (%.1f %.1f %.1f) look (%.3f %.3f %.3f) theta %.1f verta %.1f fovy %.1f aspect %.3f screen %.0fx%.0f lodscalez %.4f\n",
				pl->cam_pos.x, pl->cam_pos.y, pl->cam_pos.z,
				pl->cam_look.x, pl->cam_look.y, pl->cam_look.z,
				pl->vv_theta, pl->vv_verta, pl->c_perspfovy, pl->c_perspaspect,
				pl->c_screenwidth, pl->c_screenheight, pl->c_lodscalez);

		if (pl->prop) {
			fprintf(f, "player prop pos (%.1f %.1f %.1f) rooms", pl->prop->pos.x, pl->prop->pos.y, pl->prop->pos.z);
			traceRooms(f, pl->prop->rooms);
			fprintf(f, "\n");
		}
	} else {
		fprintf(f, "no current player\n");
	}

	fprintf(f, "\n[rooms on screen] of %d\n", g_Vars.roomcount);

	for (i = 1; i < g_Vars.roomcount; i++) {
		if (g_Rooms[i].flags & ROOMFLAG_ONSCREEN) {
			struct drawslot *slot = bgGetRoomDrawSlot(i);

			fprintf(f, "room %d flags %04x loaded240 %d box %d %d %d %d\n", i,
					g_Rooms[i].flags, g_Rooms[i].loaded240,
					slot ? slot->box.xmin : -1, slot ? slot->box.ymin : -1,
					slot ? slot->box.xmax : -1, slot ? slot->box.ymax : -1);
		}
	}

	for (ptr = g_Vars.onscreenprops; ptr && ptr < g_Vars.endonscreenprops; ptr++) {
		if (*ptr && (*ptr)->type < 8) {
			counts[(*ptr)->type]++;
		}
	}

	fprintf(f, "\n[onscreen props] %d: obj %d door %d chr %d weapon %d explosion %d player %d smoke %d\n",
			g_Vars.numonscreenprops, counts[PROPTYPE_OBJ], counts[PROPTYPE_DOOR],
			counts[PROPTYPE_CHR], counts[PROPTYPE_WEAPON], counts[5],
			counts[PROPTYPE_PLAYER], counts[7]);

	fprintf(f, "\n[chrs] every character in the level; draw bits say what chrRender() did with it this frame\n");

	for (i = 0; i < g_Vars.maxprops; i++) {
		struct prop *prop = &g_Vars.props[i];

		if ((prop->type == PROPTYPE_CHR || prop->type == PROPTYPE_PLAYER) && prop->chr) {
			traceChr(f, pl, prop, prop->chr);
		}
	}

	fprintf(f, "\n[objects on screen with a release mesh]\n");

	for (ptr = g_Vars.onscreenprops; ptr && ptr < g_Vars.endonscreenprops; ptr++) {
		struct prop *prop = *ptr;

		if (prop && (prop->type == PROPTYPE_OBJ || prop->type == PROPTYPE_DOOR || prop->type == PROPTYPE_WEAPON)
				&& prop->obj && prop->obj->model) {
			f32 dist = 0, yaw = 0, pitch = 0;

			if (pl) {
				traceRelative(pl, &prop->pos, &dist, &yaw, &pitch);
			}

			if (xblaMeshTraceModel(NULL, prop->obj->model, NULL) > 0) {
				fprintf(f, "obj modelnum 0x%x pos (%.0f %.0f %.0f) dist %.0f yaw %+.1f propflags %08x rooms",
						prop->obj->modelnum, prop->pos.x, prop->pos.y, prop->pos.z, dist, yaw, prop->flags);
				traceRooms(f, prop->rooms);
				fprintf(f, "\n");
				xblaMeshTraceModel(f, prop->obj->model, "    ");
			}
		}
	}
}

/**
 * Runs with the frame drawn and not yet presented, alongside the screenshot's
 * own callback, so the two describe the same frame.
 */
static void tracePreSwap(void)
{
	char filename[FS_MAXPATH + 1];
	FILE *f;

	if (!pending) {
		return;
	}

	pending = false;

	if (!tracePickFilename(filename, sizeof(filename))) {
		return;
	}

	f = fopen(filename, "w");

	if (!f) {
		sysLogPrintf(LOG_ERROR, "trace: could not write %s", filename);
		return;
	}

	traceWrite(f);
	fclose(f);
	sysLogPrintf(LOG_NOTE, "trace: %s", filename);
}

void traceInit(void)
{
	videoAddPreSwapCallback(tracePreSwap);
}

PD_CONSTRUCTOR static void traceConfigInit(void)
{
	configRegisterString("Mod.TraceKey", keyName, sizeof(keyName));
}
