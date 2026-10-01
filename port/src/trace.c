#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>
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
#include "xblafont.h"
#include "xblastage.h"
#include "gebeanstage.h"
#include "wallhitclip.h"
#include "game/modoptions.h"
#include "game/chrai.h"
#include "game/objectives.h"
#include "game/game_0b0fd0.h"
#include "modloader.h"
#include "trace.h"
#include "pngwrite.h"
#include "crashreport.h"
#include "../fast3d/gfx_api.h"
#include <SDL.h>

extern s32 g_TickRateDiv;

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

void traceSetKey(s32 vk)
{
	if (vk <= 0 || vk >= VK_TOTAL_COUNT) {
		strcpy(keyName, "NONE");
		keyVk = 0;
		return;
	}

	strncpy(keyName, inputGetKeyName(vk), sizeof(keyName) - 1);
	keyName[sizeof(keyName) - 1] = '\0';
	keyVk = vk;
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

	// Not over its own Report a Problem dialog: the new dump would take the
	// dialog's paths and clear the note being typed, and its picture would be
	// of the dialog rather than of the problem.
	if (vk > 0 && inputKeyJustPressed(vk) && !traceReportIsOpen()) {
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
	// Which AI list a chr is on and where in it: a stuck set piece (Statue
	// Park's Trevelyan circling pad 1 on list 0x413) reads straight off it
	if (prop->type != PROPTYPE_PLAYER && chr->ailist) {
		s32 global = 0; // the game's bool is an s32; this file's is stdbool's
		fprintf(f, " ailist %#x+%d", chraiGetListIdByList(chr->ailist, (void *)&global), chr->aioffset);
	}
	if (chr->tracedrawframe == (u32)g_Vars.lvframenum) {
		fprintf(f, "\n    draw: this frame, alpha %d: %s\n", chr->tracedrawalpha,
				traceDrawBits(chr->tracedrawbits, bits, sizeof(bits)));
	} else if (chr->tracedrawframe > 0 && chr->tracedrawframe < (u32)g_Vars.lvframenum) {
		fprintf(f, "\n    draw: not this frame (last reached by chrRender at frame %u, now %d)\n",
				chr->tracedrawframe, g_Vars.lvframenum);
	} else {
		fprintf(f, "\n    draw: never reached by chrRender\n");
	}

	if (chr->model) {
		xblaMeshTraceModel(f, chr->model, "    ");
	}
}


static const char *traceModeString(const SDL_DisplayMode *m, char *buf, u32 size)
{
	snprintf(buf, size, "%dx%d@%dHz", m->w, m->h, m->refresh_rate);
	return buf;
}

/**
 * What a stutter report needs about the screen: the monitor's refresh, the
 * desktop and window modes, and the renderer, vsync, MSAA and frame limit
 * actually in effect (pd.ini below says what was asked for).
 */
static void traceDisplay(FILE *f)
{
	SDL_Window *wnd = (SDL_Window *)videoGetWindowHandle();
	const char *driver = SDL_GetCurrentVideoDriver();
	const char *gpu = videoGetGpuName();
	char buf[64];

	fprintf(f, "\n[display]\n");

	if (wnd) {
		const s32 idx = SDL_GetWindowDisplayIndex(wnd);
		const u32 flags = SDL_GetWindowFlags(wnd);
		const char *name = idx >= 0 ? SDL_GetDisplayName(idx) : NULL;
		SDL_DisplayMode mode;
		s32 w = 0, h = 0;

		fprintf(f, "video driver %s, display %d \"%s\" of %d\n",
				driver ? driver : "-", idx, name ? name : "-", SDL_GetNumVideoDisplays());

		if (idx >= 0 && SDL_GetCurrentDisplayMode(idx, &mode) == 0) {
			fprintf(f, "display mode now %s", traceModeString(&mode, buf, sizeof(buf)));
		} else {
			fprintf(f, "display mode now unknown (%s)", SDL_GetError());
		}

		if (idx >= 0 && SDL_GetDesktopDisplayMode(idx, &mode) == 0) {
			fprintf(f, ", desktop %s", traceModeString(&mode, buf, sizeof(buf)));
		}

		if ((flags & SDL_WINDOW_FULLSCREEN) && SDL_GetWindowDisplayMode(wnd, &mode) == 0) {
			fprintf(f, ", window's fullscreen mode %s", traceModeString(&mode, buf, sizeof(buf)));
		}

		SDL_GetWindowSize(wnd, &w, &h);
		fprintf(f, "\nwindow %dx%d %s%s%s\n", w, h,
				(flags & SDL_WINDOW_FULLSCREEN_DESKTOP) == SDL_WINDOW_FULLSCREEN_DESKTOP ? "borderless fullscreen"
				: (flags & SDL_WINDOW_FULLSCREEN) ? "exclusive fullscreen" : "windowed",
				(flags & SDL_WINDOW_MAXIMIZED) ? ", maximized" : "",
				(flags & SDL_WINDOW_INPUT_FOCUS) ? ", focused" : ", not focused");
	} else {
		fprintf(f, "video driver %s, no window\n", driver ? driver : "-");
	}

	fprintf(f, "renderer %s, gpu %s\n", videoGetRendererName(), gpu && gpu[0] ? gpu : "-");

	{
		const s32 limit = videoGetFramerateLimit();

		if (limit > 0) {
			snprintf(buf, sizeof(buf), "%d fps", limit);
		} else {
			snprintf(buf, sizeof(buf), "none");
		}

		fprintf(f, "active: vsync %d, msaa %dx, frame limit %s, refresh %d Hz, game tick divisor %d\n",
				videoGetVsync(), videoGetMSAA(), buf, videoGetRefreshRate(), g_TickRateDiv);
	}
}

static int traceCompareFloatDesc(const void *a, const void *b)
{
	const f32 x = *(const f32 *)a;
	const f32 y = *(const f32 *)b;

	return x < y ? 1 : x > y ? -1 : 0;
}

/**
 * The last few seconds of frames: how even the presentation was, and whether
 * the long frames were the game and renderer running over (work) or the
 * present being held (vsync, the limiter or the driver).
 */
static void traceFrames(FILE *f)
{
	static f32 frameMs[VIDEO_FRAME_HISTORY];
	static f32 workMs[VIDEO_FRAME_HISTORY];
	static f32 sorted[VIDEO_FRAME_HISTORY];
	static u8 ticks[VIDEO_FRAME_HISTORY];
	const u32 n = videoGetFrameHistory(frameMs, workMs, ticks, VIDEO_FRAME_HISTORY);
	const s32 hz = videoGetRefreshRate();
	const f32 refreshMs = hz > 0 ? 1000.f / hz : 1000.f / 60.f;
	u32 over15 = 0, over2 = 0, slowwork = 0, heldpresent = 0;
	u32 tickhist[10] = {0};
	f64 total = 0.0, worktotal = 0.0;
	u32 onepct, i;
	f32 worst = 0.f, worstwork = 0.f;

	fprintf(f, "\n[frames]\n");

	if (n == 0) {
		fprintf(f, "no frames recorded yet\n");
		return;
	}

	for (i = 0; i < n; i++) {
		total += frameMs[i];
		worktotal += workMs[i];
		sorted[i] = frameMs[i];

		if (frameMs[i] > worst) {
			worst = frameMs[i];
			worstwork = workMs[i];
		}

		if (frameMs[i] > refreshMs * 1.5f) {
			over15++;

			if (workMs[i] > refreshMs) {
				slowwork++;
			} else {
				heldpresent++;
			}
		}

		if (frameMs[i] > refreshMs * 2.f) {
			over2++;
		}

		tickhist[ticks[i] < 9 ? ticks[i] : 9]++;
	}

	qsort(sorted, n, sizeof(sorted[0]), traceCompareFloatDesc);
	onepct = n / 100 ? n / 100 : 1;

	{
		f64 lowsum = 0.0;

		for (i = 0; i < onepct; i++) {
			lowsum += sorted[i];
		}

		fprintf(f, "last %u frames over %.1f s: avg %.1f fps, avg %.2f ms, 1%% low %.2f ms (%.1f fps), worst %.2f ms (work %.2f ms)\n",
				n, total / 1000.0, total > 0.0 ? n * 1000.0 / total : 0.0, total / n,
				lowsum / onepct, lowsum > 0.0 ? onepct * 1000.0 / lowsum : 0.0, worst, worstwork);
	}

	fprintf(f, "refresh interval %.2f ms%s: %u frames over 1.5x, %u over 2x; of the 1.5x ones %u ran over in the game and renderer, %u waited at the present\n",
			refreshMs, hz > 0 ? "" : " (refresh unknown, 60 Hz assumed)", over15, over2, slowwork, heldpresent);
	fprintf(f, "work before the swap avg %.2f ms; game step in 240ths:", worktotal / n);

	for (i = 0; i < 10; i++) {
		if (tickhist[i]) {
			fprintf(f, " %s%u x%u", i == 9 ? ">=" : "", i, tickhist[i]);
		}
	}

	fprintf(f, "\n");
}


/* -------------------------------------------------------------------------
 * [recent switches]
 * ------------------------------------------------------------------------- */

#define TRACE_EVENTS 24
#define TRACE_EVENT_LEN 112

struct traceevent {
	s32 stagenum;
	s32 lvframenum;
	char text[TRACE_EVENT_LEN];
};

static struct traceevent g_TraceEvents[TRACE_EVENTS];
static u32 g_TraceEventCount;

void traceNoteEvent(const char *fmt, ...)
{
	struct traceevent *e = &g_TraceEvents[g_TraceEventCount % TRACE_EVENTS];
	va_list ap;

	e->stagenum = g_Vars.stagenum;
	e->lvframenum = g_Vars.lvframenum;
	va_start(ap, fmt);
	vsnprintf(e->text, sizeof(e->text), fmt, ap);
	va_end(ap);
	g_TraceEventCount++;
}

static void traceEvents(FILE *f)
{
	const u32 first = g_TraceEventCount > TRACE_EVENTS ? g_TraceEventCount - TRACE_EVENTS : 0;

	fprintf(f, "\n[recent switches] the last %u of %u: packs, the release's art, stage loads, menu previews\n",
			g_TraceEventCount - first, g_TraceEventCount);

	for (u32 i = first; i < g_TraceEventCount; i++) {
		const struct traceevent *e = &g_TraceEvents[i % TRACE_EVENTS];

		fprintf(f, "stage 0x%02x frame %d: %s\n", e->stagenum, e->lvframenum, e->text);
	}
}

/* -------------------------------------------------------------------------
 * [room textures]
 *
 * What each on-screen room's display lists bind, followed to everything that
 * decides the picture drawn for it: the texture number the pack registry has
 * for the address and the shared pool's entry at it (number, which mod), where
 * the texels came from (texpackTextureArt()), checksums of the texels and of
 * the TLUT as the list loads them, the release's record or a picture bound at
 * the address (xblatex.c), and every renderer cache entry for the address -
 * what it was keyed on, what went up and where from, and when. A picture
 * drawn wrong for a whole session (the Institute's paintings, F3
 * 20260930-213440) is one of these disagreeing with the others.
 * ------------------------------------------------------------------------- */

#define TRACE_TEX_PER_ROOM 12
#define TRACE_TEX_ROOMS 10
#define TRACE_TEX_LINES 160
#define TRACE_TEX_GDL_CMDS 20000
#define TRACE_TEX_GDL_DEPTH 6

struct tracetexuse {
	const u8 *addr;
	const u8 *tlut;
	u32 bytes;    // as the load names them
	u32 tlutn;    // colours in the TLUT load
	u8 fmt, siz;
};

struct tracetexlist {
	struct tracetexuse uses[TRACE_TEX_PER_ROOM];
	s32 count;
	s32 skipped;
	const u8 *timg;
	u32 timgwidth;
	u8 timgfmt, timgsiz;
	const u8 *pendingtlut;
	u32 pendingtlutn;
	s32 cmds;
};

static u32 traceFnv(const u8 *p, u32 len)
{
	u32 h = 0x811c9dc5u;

	for (u32 i = 0; i < len; i++) {
		h = (h ^ p[i]) * 0x01000193u;
	}

	return h;
}

static struct tracetexuse *traceTexAdd(struct tracetexlist *l, const u8 *addr)
{
	for (s32 i = 0; i < l->count; i++) {
		if (l->uses[i].addr == addr) {
			return &l->uses[i];
		}
	}

	if (l->count >= TRACE_TEX_PER_ROOM) {
		l->skipped++;
		return NULL;
	}

	memset(&l->uses[l->count], 0, sizeof(l->uses[0]));
	l->uses[l->count].addr = addr;

	return &l->uses[l->count++];
}

static void traceTexWalkGdl(struct tracetexlist *l, Gfx *gdl, s32 depth)
{
	static struct tracetexuse *last;

	if (!gdl || depth > TRACE_TEX_GDL_DEPTH) {
		return;
	}

	if (depth == 0) {
		last = NULL;
	}

	for (; l->cmds < TRACE_TEX_GDL_CMDS; gdl++) {
		const u32 op = (u32)(gdl->words.w0 >> 24) & 0xff;
		const uintptr_t w0 = gdl->words.w0;
		const uintptr_t w1 = gdl->words.w1;

		l->cmds++;

		if (op == (u8)G_ENDDL) {
			return;
		}

		if (op == G_DL) {
			traceTexWalkGdl(l, (Gfx *)gfx_trace_seg_addr(w1), depth + 1);

			if ((w0 >> 16) & 1) {
				return; // a branch, not a call
			}

			continue;
		}

		if (op == G_SETTIMG) {
			l->timg = (const u8 *)gfx_trace_seg_addr(w1);
			l->timgfmt = (w0 >> 21) & 7;
			l->timgsiz = (w0 >> 19) & 3;
			l->timgwidth = (w0 & 0x3ff) + 1;
			continue;
		}

		if (op == G_LOADTLUT && l->timg) {
			const u32 uls = (w0 >> 14) & 0x3ff, ult = (w0 >> 2) & 0x3ff;
			const u32 lrs = (w1 >> 14) & 0x3ff, lrt = (w1 >> 2) & 0x3ff;
			const u32 n = (lrs >= uls && lrt >= ult) ? (lrs - uls + 1) * (lrt - ult + 1) : 0;
			// Where the renderer reads it from (gfx_dp_load_tlut()): the
			// palette is loaded out of the texture's own image, past its texels
			const u8 *tlut = l->timg + ((size_t)l->timgwidth * ult + uls) * 2;

			// Onto the texture just loaded if it has none yet, else the next
			if (last && !last->tlut) {
				last->tlut = tlut;
				last->tlutn = n;
			} else {
				l->pendingtlut = tlut;
				l->pendingtlutn = n;
			}

			continue;
		}

		if ((op == G_LOADBLOCK || op == G_LOADTILE) && l->timg) {
			struct tracetexuse *u = traceTexAdd(l, l->timg);

			last = u;

			if (u && !u->bytes) {
				const u32 lrs = (w1 >> 12) & 0xfff;

				u->fmt = l->timgfmt;
				u->siz = l->timgsiz;
				u->bytes = op == G_LOADBLOCK ? ((lrs + 1) << l->timgsiz) >> 1 : 0;

				if (l->pendingtlut && !u->tlut) {
					u->tlut = l->pendingtlut;
					u->tlutn = l->pendingtlutn;
					l->pendingtlut = NULL;
				}
			}
		}
	}
}

static void traceTexWalkBlocks(struct tracetexlist *l, struct roomblock *block, s32 depth)
{
	s32 guard = 0;

	while (block && depth < 32 && guard++ < 4096 && l->cmds < TRACE_TEX_GDL_CMDS) {
		if (block->type == 0) {
			traceTexWalkGdl(l, block->gdl, 0);
		} else if (block->type == 1) {
			traceTexWalkBlocks(l, block->child, depth + 1);
		}

		block = block->next;
	}
}

static struct tex *traceTexPoolAt(const u8 *addr)
{
	struct tex *cur = g_TexSharedPool.head;
	s32 guard = 0;

	while (cur && guard++ < 8192) {
		if (cur->data == addr) {
			return cur;
		}

		if (!cur->next) {
			break;
		}

		cur = (struct tex *)PHYS_TO_K0(cur->next);
	}

	return NULL;
}

static struct tex *traceTexPoolNum(s32 num)
{
	struct tex *cur = g_TexSharedPool.head;
	s32 guard = 0;

	while (cur && guard++ < 8192) {
		if (cur->texturenum == num) {
			return cur;
		}

		if (!cur->next) {
			break;
		}

		cur = (struct tex *)PHYS_TO_K0(cur->next);
	}

	return NULL;
}

static const char *traceTexFmtName(u32 fmt, u32 siz, char *buf, u32 size)
{
	static const char *const fmts[] = { "rgba", "yuv", "ci", "ia", "i", "?5", "?6", "?7" };
	static const char *const sizes[] = { "4", "8", "16", "32" };

	snprintf(buf, size, "%s%s", fmts[fmt & 7], sizes[siz & 3]);

	return buf;
}

static void traceTexLine(FILE *f, const struct tracetexuse *u, const char *indent)
{
	static const char *const arts[] = { "rom", "mod", "modstage" };
	struct GfxTraceTexEntry entries[4];
	const u32 now = gfx_trace_frame();
	const s32 regnum = texpackGetTextureNum(u->addr);
	const s32 art = texpackTextureArt(u->addr);
	struct tex *pool = traceTexPoolAt(u->addr);
	s32 alpha = 0, soft = 0;
	const s32 picture = xblaTexImageInfo(u->addr, &alpha, &soft);
	const s32 record = xblaTexRecordOf(u->addr);
	s32 n;
	char fmtbuf[16];

	fprintf(f, "%stex %04x", indent, regnum >= 0 ? regnum : 0xffff);

	if (pool) {
		fprintf(f, " pool %04x mod %u %ux%u", pool->texturenum, (u32)pool->srcmod, pool->width, pool->height);
	} else {
		fprintf(f, " pool -");
	}

	fprintf(f, " art %s load %s %u bytes data %08x",
			art >= 0 && art <= 2 ? arts[art] : "-",
			traceTexFmtName(u->fmt, u->siz, fmtbuf, sizeof(fmtbuf)),
			u->bytes, u->bytes && u->bytes <= 0x10000 ? traceFnv(u->addr, u->bytes) : 0);

	if (u->tlut) {
		fprintf(f, " tlut %u at %+ld pal %08x", u->tlutn, (long)(u->tlut - u->addr),
				u->tlutn <= 256 ? traceFnv(u->tlut, u->tlutn * 2) : 0);
	}

	if (record >= 0) {
		fprintf(f, " xbla record %04x", record);
	}

	if (picture) {
		fprintf(f, " BOUND PICTURE alpha %d soft %d", alpha, soft);
	}

	if (regnum >= 0 && texpackHaveReplacements()) {
		fprintf(f, " pack %d", texpackHaveReplacementFor(regnum));
	}

	n = gfx_trace_texture_entries(u->addr, entries, ARRAYCOUNT(entries));
	fprintf(f, "; cache %d", n);

	for (s32 i = 0; i < n && i < (s32)ARRAYCOUNT(entries); i++) {
		const struct GfxTraceTexEntry *e = &entries[i];

		fprintf(f, " [%s pal %s%+ld/%u",
				traceTexFmtName(e->fmt, e->siz, fmtbuf, sizeof(fmtbuf)),
				e->palette ? "" : "none", e->palette ? (long)((const u8 *)e->palette - u->addr) : 0L,
				e->palindex);

		// The key's other half, which nothing resets between loads: two
		// entries for one address differ here if anywhere
		if (e->palette1) {
			fprintf(f, " pal1 %+ld", (long)((const u8 *)e->palette1 - u->addr));
		}

		if (e->glyph) {
			fprintf(f, " glyph %08x", e->glyph);
		}

		fprintf(f, " src %c %ux%u up %u ago drawn %u ago]", e->source, e->width, e->height,
				now - e->upload_frame, now - e->last_frame);
	}

	fprintf(f, "\n");
}

static void traceRoomTextures(FILE *f)
{
	struct tracetexlist *l = malloc(sizeof(*l));
	s32 rooms = 0;
	s32 lines = 0;

	fprintf(f, "\n[room textures] per on-screen room (at most %d rooms, %d each): registry number, pool entry"
			" (number, mod+1, size), art, load format, checksums of texels and TLUT, release record or bound"
			" picture, pack has one; then each renderer cache entry: key format, TLUT offset/palette, source"
			" (g game p pack X release x bound f font m menu), uploaded size, frames since upload and last draw\n",
			TRACE_TEX_ROOMS, TRACE_TEX_PER_ROOM);

	if (!l) {
		return;
	}

	for (s32 i = 1; i < g_Vars.roomcount && rooms < TRACE_TEX_ROOMS && lines < TRACE_TEX_LINES; i++) {
		struct roomgfxdata *gfx;

		if (!(g_Rooms[i].flags & ROOMFLAG_ONSCREEN) || !(gfx = g_Rooms[i].gfxdata)) {
			continue;
		}

		memset(l, 0, sizeof(*l));
		traceTexWalkBlocks(l, gfx->opablocks, 0);
		traceTexWalkBlocks(l, gfx->xlublocks, 0);
		rooms++;

		fprintf(f, "room %d: %d textures%s\n", i, l->count + l->skipped,
				l->skipped ? " (the first listed)" : "");

		for (s32 t = 0; t < l->count && lines < TRACE_TEX_LINES; t++, lines++) {
			traceTexLine(f, &l->uses[t], "  ");
		}
	}

	// The Institute's paintings, wherever the camera is: the pictures one
	// tester saw drawn wrong in every look and pack for a whole session
	if (g_Vars.stagenum == STAGE_CITRAINING) {
		static const s32 watched[] = { 0x0255, 0x0267, 0x0269, 0x026b };

		fprintf(f, "the Institute's paintings:\n");

		for (s32 i = 0; i < (s32)ARRAYCOUNT(watched); i++) {
			struct tex *tex = traceTexPoolNum(watched[i]);
			struct tracetexuse u;

			if (!tex) {
				fprintf(f, "  %04x not in the pool\n", watched[i]);
				continue;
			}

			memset(&u, 0, sizeof(u));
			u.addr = tex->data;
			u.fmt = tex->gbiformat;
			u.siz = tex->depth;
			// the pixels with their mipmaps, then the palette straight after
			// them (texInflate*()): with an upload to say where the palette
			// is, the same span and TLUT the room's own load names
			{
				struct GfxTraceTexEntry e;

				if (gfx_trace_texture_entries(tex->data, &e, 1) > 0 && e.palette > (const void *)tex->data) {
					u.tlut = (const u8 *)e.palette;
					u.tlutn = tex->unk0a + 1;
					u.bytes = (u32)(u.tlut - u.addr);
					u.siz = G_IM_SIZ_16b;
				} else {
					u.bytes = (((u32)tex->width * tex->height) << tex->depth) >> 1;
				}
			}

			traceTexLine(f, &u, "  ");
		}
	}

	free(l);
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

	// A stage id above the table says nothing about which map it is: the
	// Stage Loader hands ids out in the order the mods register, so the same
	// id is a different map on the reporter's machine than on ours. A report
	// that only says "stage 0x55" costs a session working the map out from
	// its room count.
	{
		const char *mapname = modloaderGetStageMapName(mainGetStageNum());
		const char *moddir = modloaderGetStageModDir(mainGetStageNum());

		if (mapname || moddir) {
			fprintf(f, "stage map \"%s\" of mod \"%s\"\n",
					mapname ? mapname : "-", moddir ? moddir : "-");
		}
	}
	// A solo mission's progress: its stage flags and each objective's status
	// (0 incomplete, 1 complete, 2 failed), which say how far its set pieces
	// have got when a report says one never happened
	if (!g_Vars.normmplayerisrunning && objectiveGetCount() > 0) {
		fprintf(f, "mission: stage flags %08x, objectives", g_StageFlags);
		for (s32 i = 0; i < objectiveGetCount(); i++) {
			fprintf(f, " %d", objectiveCheck(i));
		}
		fprintf(f, "\n");
	}
	fprintf(f, "xbla: meshes %d stages %d meshtextures %d font %d, rooms from release %d; texture pack replacements %d\n",
			xblaMeshGetEnabled(), xblaStageGetEnabled(), xblaTexGetEnabled(),
			xblaFontGetEnabled(), xblaStageIsRelease(), texpackHaveReplacements());

	traceDisplay(f);
	traceFrames(f);
	traceEvents(f);

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
	fprintf(f, "renderer (last frame): %u draws, %u tris, %u verts, %u texture uploads, %u evictions, cache %u of %u, %u buffer-full flushes;"
			" texture cache peak %u, grew %u times, %u evicted in all, %u MB of %u, up to %u%s\n",
			gs.drawcalls, gs.tris, gs.verts, gs.texuploads,
			gs.texevictions, gs.cacheentries, gs.cachesize, gs.bufferfullflushes,
			gs.cachepeak, gs.cachegrows, gs.cacheevictions, gs.cachemb, gs.cachebudgetmb, gs.cachemax,
			gs.cachefixed ? " (fixed by --gfxtexcache)" : "");
	xblaMeshTrace(f);
	xblaTexTrace(f);
	xblaFontTrace(f);
	xblaStageTrace(f);
	gebeanStageTrace(f);
	texpackTrace(f);

	// The title's logos, the credits and the boot menus are no level: the
	// rooms, props and player left in g_Vars belong to a stage that is gone
	// (or that never was, at the first title), and F3 on the title read
	// g_Rooms through a stale room count and crashed.
	if (!STAGE_IS_LEVEL(g_Vars.stagenum)) {
		fprintf(f, "\n[level]\nstage 0x%02x is not a level (the title, the credits or a boot menu): no camera, rooms or characters\n",
				g_Vars.stagenum);
		return;
	}

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

		// The agent's sight options live in the save, not pd.ini, so a report
		// of "no crosshair" says nothing of them without this line (F3
		// 20260930-152306: Sight on Screen off reads exactly like a bug)
		if (g_Vars.currentplayerstats) {
			const u32 options = g_PlayerConfigsArray[g_Vars.currentplayerstats->mpindex].options;

			fprintf(f, "hand: weapon 0x%02x func %d sight %u gunsightoff 0x%x (0 aiming); options %04x: sight on screen %d,"
					" always show target %d, zoom range %d, ammo on screen %d\n",
					pl->hands[HAND_RIGHT].gset.weaponnum, pl->hands[HAND_RIGHT].gset.weaponfunc,
					currentPlayerGetSight(), pl->gunsightoff, options,
					(options & OPTION_SIGHTONSCREEN) != 0, (options & OPTION_ALWAYSSHOWTARGET) != 0,
					(options & OPTION_SHOWZOOMRANGE) != 0, (options & OPTION_AMMOONSCREEN) != 0);
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

	traceRoomTextures(f);

	for (ptr = g_Vars.onscreenprops; ptr && ptr < g_Vars.endonscreenprops; ptr++) {
		if (*ptr && (*ptr)->type < 8) {
			counts[(*ptr)->type]++;
		}
	}

	fprintf(f, "\n[onscreen props] %d: obj %d door %d chr %d weapon %d explosion %d player %d smoke %d\n",
			g_Vars.numonscreenprops, counts[PROPTYPE_OBJ], counts[PROPTYPE_DOOR],
			counts[PROPTYPE_CHR], counts[PROPTYPE_WEAPON], counts[5],
			counts[PROPTYPE_PLAYER], counts[7]);

	{
		s32 clipped;
		s32 whole;
		s32 waiting;

		wallhitClipCounts(&clipped, &whole, &waiting);
		fprintf(f, "wallhits: %d in use of %d; clip decals at edges %d: %d clipped, %d whole, %d not tried\n",
				clipped + whole + waiting, g_WallhitsMax, modIsDecalClipOn(), clipped, whole, waiting);
	}

	fprintf(f, "\n[chrs] every character in the level; draw bits say what chrRender() did with it this frame\n");

	// The live props only, active then paused (the active list's tail leads on
	// into the paused one). A slot in g_Vars.props that has been freed keeps
	// its type and a pointer to a chr that is gone - a corpse the vertex
	// store's reaper faded out - and F3 on Chicago crashed reading one.
	{
		struct prop *prop = g_Vars.activeprops;
		s32 guard = 0;

		while (prop && guard++ < g_Vars.maxprops) {
			if ((prop->type == PROPTYPE_CHR || prop->type == PROPTYPE_PLAYER) && prop->chr
					&& prop->chr->prop == prop) {
				traceChr(f, pl, prop, prop->chr);
			}

			prop = prop->next;
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

// The widest picture a report carries. A 4K frame is tens of megabytes as a
// PNG with no filtering; at this width it is around one, and still shows
// which pixel is wrong.
#define TRACE_REPORT_SHOT_WIDTH 1280

/**
 * The frame, box filtered down by a whole factor to fit the report's width,
 * as a PNG beside the dump. The full size picture is screenshots/' own.
 */
static bool traceWriteReportShot(const char *path)
{
	const s32 width = videoGetWindowWidth();
	const s32 height = videoGetWindowHeight();
	s32 factor, outw, outh, x, y;
	u8 *rgb, *small, *png;
	u32 pngsize = 0;
	FILE *f;
	bool ok;

	if (width <= 0 || height <= 0) {
		return false;
	}

	factor = (width + TRACE_REPORT_SHOT_WIDTH - 1) / TRACE_REPORT_SHOT_WIDTH;
	outw = width / factor;
	outh = height / factor;
	rgb = malloc((size_t)width * height * 3);
	small = malloc((size_t)outw * outh * 3);

	if (!rgb || !small || !videoReadScreenPixels(rgb, width, height)) {
		free(rgb);
		free(small);
		return false;
	}

	for (y = 0; y < outh; y++) {
		for (x = 0; x < outw; x++) {
			u32 sum[3] = {0, 0, 0};
			s32 dx, dy, c;

			for (dy = 0; dy < factor; dy++) {
				const u8 *row = rgb + ((size_t)(y * factor + dy) * width + x * factor) * 3;

				for (dx = 0; dx < factor; dx++) {
					sum[0] += row[dx * 3];
					sum[1] += row[dx * 3 + 1];
					sum[2] += row[dx * 3 + 2];
				}
			}

			for (c = 0; c < 3; c++) {
				small[((size_t)y * outw + x) * 3 + c] = sum[c] / (factor * factor);
			}
		}
	}

	free(rgb);
	png = pngEncode(small, outw, outh, 3, true, &pngsize);
	free(small);

	if (!png) {
		return false;
	}

	f = fopen(path, "wb");
	ok = f && fwrite(png, 1, pngsize, f) == pngsize;

	if (f && fclose(f) != 0) {
		ok = false;
	}

	free(png);

	return ok;
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
	// What a crash report carries around its stack goes around the dump too:
	// the settings and the log say how the player got to this frame.
	crashReportWriteContext(f);
	fclose(f);
	sysLogPrintf(LOG_NOTE, "trace: %s", filename);

	if (traceReportEnabled()) {
		char shot[FS_MAXPATH + 1];
		const u32 len = strlen(filename);

		snprintf(shot, sizeof(shot), "%s", filename);

		if (len > 4 && len < sizeof(shot)) {
			strcpy(shot + len - 4, ".png");
		}

		traceReportOffer(filename, traceWriteReportShot(shot) ? shot : NULL);
	}
}

void traceInit(void)
{
	videoAddPreSwapCallback(tracePreSwap);
}

PD_CONSTRUCTOR static void traceConfigInit(void)
{
	configRegisterString("Mod.TraceKey", keyName, sizeof(keyName));
}
