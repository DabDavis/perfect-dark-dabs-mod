/**
 * GoldenEye's credits, played when the Cradle is finished.
 *
 * GoldenEye does it with a level: finishing the Cradle's report runs
 * LEVELID_CUBA (front.c: `mission_num == SP_LEVEL_CRADLE` ->
 * `selected_stage = LEVELID_CUBA`), where Bond and Natalya stand in a jungle
 * clearing, a camera circles them, three lines of dialogue come up and the
 * credits roll. When the roll has run off the top the list fades out and
 * leaves, and the front end plays the cast reel in its long version before
 * putting the player back on the mission grid with the Cradle under the cursor.
 *
 * All of it is the ROM's. The conversion makes Cuba a mission like the
 * twenty (geconvert.c, GEMISSION_CUBA, never shown in the folder); its list
 * 0x1000 is GoldenEye's own and runs as it is, and the three commands it needs
 * that Perfect Dark has no word for are the port's own:
 *
 * - **CameraOrbitPad** (0x01e8, chrai.c's AI_CameraOrbitPad and bondview2.c's
 *   CAMERAMODE_POSEND): the camera stands `distance` out from a pad and
 *   `height` over the point `lookheight` above it, looks at that point and
 *   goes round it `speed` 65536ths of a turn a tick. The player's tick mode is
 *   TICKMODE_WARP, which builds and ticks Bond's body and leaves the camera to
 *   whoever asks - the opening swirl's frozen camera (gecinema.c) - and the
 *   body is then Bond's list's to move;
 * - **CreditsRoll** (0x01e9) starts the roll, and **IFCreditsHasCompleted**
 *   (0x01ea) asks whether it is over, as GoldenEye's credits_state 1 and 2.
 *
 * The roll is bondviewRenderCredits(): rows 16 units apart on GoldenEye's
 * 440x330 frame (Cuba is its one level drawn at the front end's resolution -
 * bondview.c allocates it a 440x330 buffer), each with two columns of Zurich
 * Bold, a position and an alignment carrying over from the row before when a
 * row gives -1, and 0x5011 standing for "nothing in this column". The rows are
 * menu/credits.bin, the ones Cuba's INTROTYPE_CREDITS record points at, and the
 * text is Cuba's own bank (LlenE), which the stage has loaded as its mission's.
 *
 * GoldenEye moves the roll one unit a *frame drawn*, not a tick. This keeps it
 * to the clock at GoldenEye's twenty frames a second there - the native port
 * runs lockstep, a frame a tick, and cannot say what the console managed - so
 * a hitch here does not slow it and a fast machine does not hurry it.
 */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <ultra64.h>
#include "constants.h"
#include "types.h"
#include "bss.h"
#include "data.h"
#include "lib/vars.h"
#include "fs.h"
#include "modloader.h"
#include "system.h"
#include "game/lang.h"
#include "game/pad.h"
#include "game/player.h"
#include "gecredits.h"
#include "geroom.h"
#include "gexfront.h"

// GoldenEye's frame the roll is laid out on, and its line
#define CREDITS_W       440
#define CREDITS_H       330
#define CREDITS_LINE    16
// a unit a frame at GoldenEye's twenty frames a second, in 60ths
#define CREDITS_PER_60  (1.0f / 3.0f)

#define CREDITS_BLANK   0x5011

// CREDITS_ALIGNMENT: right means the text *starts* at the position
#define ALIGN_RIGHT  0
#define ALIGN_LEFT   1
#define ALIGN_CENTER 2

struct creditrow {
	u16 text1, text2;
	s16 pos1;
	s16 align1;
	s16 pos2;
	s16 align2;
};

static struct {
	s32 on;
	struct creditrow *rows;
	s32 numrows;

	s32 orbit;
	f32 angle;       // radians
	f32 speed;       // radians a 60th
	f32 distance;
	f32 height;
	f32 lookheight;
	s32 padnum;

	s32 state;       // GoldenEye's credits_state: 0, 1 rolling, 2 over
	f32 frame;       // camera_80036438
} g_Credits;

static u16 be16(const u8 *p)
{
	return (u16)(p[0] << 8 | p[1]);
}

static void creditsLoad(s32 stagenum)
{
	const char *dir = fsGetModDirAt(modloaderGetStageModDirIndex(stagenum));
	char path[FS_MAXPATH + 1];
	u32 len = 0;
	u8 *data;
	s32 n;

	if (!dir) {
		return;
	}

	snprintf(path, sizeof(path), "%s/menu/credits.bin", dir);
	data = fsFileLoad(path, &len);

	if (!data) {
		sysLogPrintf(LOG_WARNING, "gecredits: no %s; the conversion is older than the credits", path);
		return;
	}

	n = len >= 8 && !memcmp(data, "GEC1", 4) ? (s32)((data[4] << 24) | (data[5] << 16) | (data[6] << 8) | data[7]) : 0;

	if (n <= 0 || 8 + 12 * (u32)n > len) {
		sysLogPrintf(LOG_WARNING, "gecredits: %s is not a credits file", path);
		sysMemFree(data);
		return;
	}

	g_Credits.rows = sysMemZeroAlloc(sizeof(*g_Credits.rows) * n);

	for (s32 i = 0; i < n; i++) {
		const u8 *r = data + 8 + 12 * i;

		g_Credits.rows[i].text1 = be16(r + 0);
		g_Credits.rows[i].text2 = be16(r + 2);
		g_Credits.rows[i].pos1 = (s16)be16(r + 4);
		g_Credits.rows[i].align1 = (s16)be16(r + 6);
		g_Credits.rows[i].pos2 = (s16)be16(r + 8);
		g_Credits.rows[i].align2 = (s16)be16(r + 10);
	}

	g_Credits.numrows = n;
	sysMemFree(data);
}

void gecreditsStageStart(s32 stagenum)
{
	if (g_Credits.rows) {
		sysMemFree(g_Credits.rows);
	}

	memset(&g_Credits, 0, sizeof(g_Credits));

	if (modloaderStageMission(stagenum) != GEMISSION_CUBA) {
		return;
	}

	g_Credits.on = 1;
	creditsLoad(stagenum);

	// Zurich Bold, which the folder's own screens load
	gexFrontLoadText();
}

s32 gecreditsIsOn(void)
{
	return g_Credits.on;
}

void gecreditsOrbit(s32 distance, s32 height, s32 speed60, s32 padnum, s32 lookheight, s32 start)
{
	if (!g_Credits.on) {
		return;
	}

	g_Credits.orbit = 1;
	g_Credits.angle = start * (M_BADTAU / 65535.0f);
	g_Credits.speed = speed60 * (M_BADTAU / 65535.0f);
	g_Credits.distance = distance;
	g_Credits.height = height;
	g_Credits.lookheight = lookheight;
	g_Credits.padnum = padnum;

	// the opening swirl's frozen camera: the body is made and ticked, the walk
	// is not, and the camera is gecreditsCameraTick()'s
	playerSetTickMode(TICKMODE_WARP);
}

/**
 * bondviewSetCurrentPlayerPosition()'s CAMERAMODE_POSEND: round the pad at
 * the orbit's distance, `height` over the point it looks at.
 */
s32 gecreditsCameraTick(void)
{
	struct player *pl = g_Vars.currentplayer;
	struct coord pos, look;
	struct coord up = {0, 1, 0};
	struct pad pad;
	s32 room;

	if (!g_Credits.orbit || !pl || !pl->prop) {
		return 0;
	}

	padUnpack(g_Credits.padnum, PADFIELD_POS | PADFIELD_ROOM, &pad);

	pos.x = pad.pos.x + sinf(g_Credits.angle) * g_Credits.distance;
	pos.y = pad.pos.y + g_Credits.lookheight + g_Credits.height;
	pos.z = pad.pos.z + cosf(g_Credits.angle) * g_Credits.distance;

	look.x = pad.pos.x - pos.x;
	look.y = pad.pos.y + g_Credits.lookheight - pos.y;
	look.z = pad.pos.z - pos.z;

	g_Credits.angle += g_Credits.speed * g_Vars.lvupdate60freal;

	while (g_Credits.angle >= M_BADTAU) {
		g_Credits.angle -= M_BADTAU;
	}

	while (g_Credits.angle < 0.0f) {
		g_Credits.angle += M_BADTAU;
	}

	playerSetCameraMode(CAMERAMODE_THIRDPERSON);

	// the room of the tile the camera is over, as a cutscene camera's
	room = pad.room > 0 && pad.room < g_Vars.roomcount ? geRoomCutsceneCamera(&pos, &pad.pos, pad.room) : -1;

	if (room > 0) {
		player0f0c1ba4(&pos, &up, &look, &pos, room);
	} else {
		player0f0c1840(&pos, &up, &look, &pl->prop->pos, pl->prop->rooms);
	}

	return 1;
}

void gecreditsRoll(void)
{
	if (g_Credits.state == 0) {
		g_Credits.state = 1;
		g_Credits.frame = 0;
	}

	// a conversion with no rows has nothing to roll
	if (!g_Credits.numrows) {
		g_Credits.state = 2;
	}
}

s32 gecreditsHaveRolled(void)
{
	return g_Credits.state == 2;
}

void gecreditsTick(void)
{
	if (g_Credits.on && g_Credits.state == 1) {
		g_Credits.frame += g_Vars.lvupdate60freal * CREDITS_PER_60;
	}
}

static const char *creditsText(u16 id, char *buf, size_t len)
{
	const char *text = langGet(LANGBANK_GEMISSION << 9 | (id & 0x1ff));
	char *nl;

	snprintf(buf, len, "%s", text ? text : "");

	if ((nl = strchr(buf, '\n'))) {
		*nl = '\0';
	}

	return buf;
}

static Gfx *creditsPrint(Gfx *gdl, u16 id, s32 pos, s32 align, s32 y)
{
	char buf[128];
	const char *text = creditsText(id, buf, sizeof(buf));
	s32 w = 0;
	s32 h = 0;
	s32 x;

	if (!text[0]) {
		return gdl;
	}

	gexFrontTextMeasure(0, text, &w, &h);

	x = align == ALIGN_LEFT ? pos - w : align == ALIGN_CENTER ? pos - (w >> 1) : pos;

	return gexFrontTextPrint(gdl, 0, x, y, text, 0xffffffff);
}

/**
 * bondviewRenderCredits(), row for row: the rows on the screen are the ones
 * between a screen's height below the frame count and the frame count, and
 * the position and alignment of each column carry over from the rows above,
 * which are walked first for them. The row whose two ids are 0 ends it.
 */
Gfx *gecreditsRender(Gfx *gdl)
{
	const s32 frame = (s32)g_Credits.frame;
	s32 xpos1 = 220;
	s32 xpos2 = 220;
	s32 align1 = ALIGN_RIGHT;
	s32 align2 = ALIGN_RIGHT;
	s32 start;
	s32 end;
	s32 i;

	if (!g_Credits.on || g_Credits.state != 1 || !g_Credits.numrows) {
		return gdl;
	}

	start = (frame - CREDITS_H) / CREDITS_LINE;
	end = frame / CREDITS_LINE + 1;

	if (start < 0) {
		start = 0;
	}

	if (end > g_Credits.numrows) {
		end = g_Credits.numrows;
	}

	for (i = 0; i < start && i < g_Credits.numrows; i++) {
		const struct creditrow *r = &g_Credits.rows[i];

		if (r->text1 == 0 && r->text2 == 0) {
			g_Credits.state = 2;
			return gdl;
		}

		if (r->text1 != CREDITS_BLANK) {
			if (r->pos1 >= 0) {
				xpos1 = r->pos1;
			}

			if (r->align1 >= ALIGN_RIGHT) {
				align1 = r->align1;
			}
		}

		if (r->text2 != CREDITS_BLANK) {
			if (r->pos2 >= 0) {
				xpos2 = r->pos2;
			}

			if (r->align2 >= ALIGN_RIGHT) {
				align2 = r->align2;
			}
		}
	}

	if (start >= g_Credits.numrows) {
		g_Credits.state = 2;
		return gdl;
	}

	gSPSetExtraGeometryModeEXT(gdl++, G_ASPECT_CENTER_EXT);
	gexFrontTextFrameDefault();
	// the rows are laid out on whole units as GoldenEye's were, and the part
	// of one the frame count is past moves them the rest of the way: stepped
	// a unit every third frame, the roll juddered (F3 20260928-002510)
	gexFrontTextNudgeY(-(g_Credits.frame - (f32)frame));
	gdl = gexFrontTextSetup(gdl);

	for (i = start; i < end && (g_Credits.rows[i].text1 || g_Credits.rows[i].text2); i++) {
		const struct creditrow *r = &g_Credits.rows[i];
		const s32 y = i * CREDITS_LINE - frame + CREDITS_H;

		if (r->text1 != CREDITS_BLANK) {
			if (r->pos1 >= 0) {
				xpos1 = r->pos1;
			}

			if (r->align1 >= ALIGN_RIGHT) {
				align1 = r->align1;
			}

			gdl = creditsPrint(gdl, r->text1, xpos1, align1, y);
		}

		if (r->text2 != CREDITS_BLANK) {
			if (r->pos2 >= 0) {
				xpos2 = r->pos2;
			}

			if (r->align2 >= ALIGN_RIGHT) {
				align2 = r->align2;
			}

			gdl = creditsPrint(gdl, r->text2, xpos2, align2, y);
		}
	}

	gexFrontTextNudgeY(0);
	gDPPipeSync(gdl++);
	gSPClearExtraGeometryModeEXT(gdl++, G_ASPECT_CENTER_EXT);

	return gdl;
}
