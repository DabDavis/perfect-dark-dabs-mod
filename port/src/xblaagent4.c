#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ultra64.h>
#include <PR/ultratypes.h>
#include "constants.h"
#include "types.h"
#include "data.h"
#include "system.h"
#include "romdata.h"
#include "gebean.h"
#include "xblaimport.h"
#include "xblamesh.h"
#include "xblaagent4.h"

/**
 * Agent 4: the one character 4J added to the XBLA release, and not an unlock -
 * the release lists him in the Combat Simulator straight after the ROM's
 * characters, with no feature to earn (its g_MpBodies row 61: body 0x98, head
 * 0x97, requirefeature 0, named "Agent 4" in every language).
 *
 * His three files are the release's own, past the ROM's file table: Cagent4Z
 * 0x7e0, Cheadagent4Z 0x7e1 and Ghand_agent4Z 0x7e2, whole Perfect Dark model
 * files (SKEL_CHR, SKEL_HEAD and the hands' skeleton) wearing the ROM's own
 * textures - Carrington's - with 4J's meshes named in their nodes as every
 * release model file has. So they are served from the package as they are
 * (romdataRegisterXblaFile()), and the rows are the release's: his table rows
 * 0x97 and 0x98 read out of the release's image (xbla-xex-image-dump), which
 * are Carrington's body to the digit.
 *
 * Only with the package unpacked and readable: the files are read out of it.
 */

#define AGENT4_BODY_FILE 0x7e0
#define AGENT4_HEAD_FILE 0x7e1
#define AGENT4_HAND_FILE 0x7e2

// The MP save holds a list index in 7 bits (gebean.c's GEBEAN_MAX_MPINDEX)
#define AGENT4_MAX_MPINDEX 126

/**
 * Whether the release's file is a model with the skeleton the row needs.
 * Asked once a refresh: a package that is some other release's, or damaged,
 * must not leave a row pointing at something the game will pose as a body.
 */
static s32 xblaAgent4FileIs(s32 xblaid, u32 skel)
{
	u32 len = 0;
	u8 *file = xblaMeshReadFile((u16)xblaid, &len);
	s32 ok;

	if (!file) {
		return 0;
	}

	ok = len > 0x20 && file[0] == 0x05
			&& (((u32)file[4] << 24) | ((u32)file[5] << 16) | ((u32)file[6] << 8) | file[7]) == skel;

	free(file);

	return ok;
}

static s32 listed;

// His rows in each look: [0] the N64 look's stand-in, [1] his own. 4J's files
// carry N64 lists too, but they are a placeholder - Carrington's head and suit
// in Carrington's textures, nothing like the armour and helmet 4J's meshes
// are - so the N64 look wears dataDyne's Shock Trooper, the ROM's nearest
// (user's choice, 2026-09-23)
static struct headorbody lookHead[2];
static struct headorbody lookBody[2];

// Each look's models as this stage loaded them, so F6 back and forth hands a
// respawn the model it loaded before instead of loading another copy into the
// stage's pool. Dropped with every row's by bodiesReset().
static struct modeldef *keptHead[2];
static struct modeldef *keptBody[2];
static s32 appliedLook = -1;

const char *xblaAgent4BodyName(s32 bodynum)
{
	return listed && bodynum == XBLA_AGENT4_BODYROW ? "Agent 4\n" : NULL;
}

void xblaAgent4StageReset(void)
{
	memset(keptHead, 0, sizeof(keptHead));
	memset(keptBody, 0, sizeof(keptBody));
}

/**
 * Put the rows of the look being drawn in place. A chr already wearing the
 * other look's model keeps it until its next spawn: a model is chosen as a chr
 * is made.
 */
void xblaAgent4MeshesSwitched(void)
{
	struct headorbody *head = &g_HeadsAndBodies[XBLA_AGENT4_HEADROW];
	struct headorbody *body = &g_HeadsAndBodies[XBLA_AGENT4_BODYROW];
	const s32 look = xblaMeshGetEnabled() ? 1 : 0;

	if (!listed) {
		return;
	}

	if (appliedLook == 0 || appliedLook == 1) {
		if (head->filenum == lookHead[appliedLook].filenum && head->modeldef) {
			keptHead[appliedLook] = head->modeldef;
		}

		if (body->filenum == lookBody[appliedLook].filenum && body->modeldef) {
			keptBody[appliedLook] = body->modeldef;
		}
	}

	*head = lookHead[look];
	*body = lookBody[look];
	head->modeldef = keptHead[look];
	body->modeldef = keptBody[look];
	appliedLook = look;
}

void xblaAgent4Refresh(void)
{
	struct headorbody *head = &g_HeadsAndBodies[XBLA_AGENT4_HEADROW];
	struct headorbody *body = &g_HeadsAndBodies[XBLA_AGENT4_BODYROW];
	s32 numbodies = g_MpListCounts.bodies;
	s32 numheads = g_MpListCounts.heads;
	s32 headfile, bodyfile, handfile;

	// The rows as they stand hold a model a chr may be wearing, and this can
	// run from the pause menu: file it under its look before they are rebuilt
	if (listed && appliedLook >= 0) {
		if (head->filenum == lookHead[appliedLook].filenum && head->modeldef) {
			keptHead[appliedLook] = head->modeldef;
		}

		if (body->filenum == lookBody[appliedLook].filenum && body->modeldef) {
			keptBody[appliedLook] = body->modeldef;
		}
	}

	listed = 0;
	appliedLook = -1;

	if (!xblaImportGetReadyStfsPath()) {
		return;
	}

	if (!xblaAgent4FileIs(AGENT4_BODY_FILE, SKEL_CHR) || !xblaAgent4FileIs(AGENT4_HEAD_FILE, SKEL_HEAD)) {
		sysLogPrintf(LOG_WARNING, "xblaagent4: the release's Agent 4 files are not the models expected");
		return;
	}

	bodyfile = romdataRegisterXblaFile("Cagent4Z", AGENT4_BODY_FILE);
	headfile = romdataRegisterXblaFile("Cheadagent4Z", AGENT4_HEAD_FILE);
	handfile = romdataRegisterXblaFile("Ghand_agent4Z", AGENT4_HAND_FILE);

	if (!bodyfile || !headfile || !handfile) {
		return;
	}

	if (numbodies >= AGENT4_MAX_MPINDEX || numbodies >= ARRAYCOUNT(g_MpBodies)
			|| numheads > AGENT4_MAX_MPINDEX || numheads >= ARRAYCOUNT(g_MpHeads)) {
		return;
	}

	// The release's own rows 0x97 and 0x98
	memset(&lookHead[1], 0, sizeof(lookHead[1]));
	lookHead[1].ismale = 1;
	lookHead[1].unk00_01 = 1;
	lookHead[1].type = HEADBODYTYPE_DEFAULT;
	lookHead[1].height = 13;
	lookHead[1].filenum = headfile;
	lookHead[1].scale = 1;
	lookHead[1].animscale = 1;

	memset(&lookBody[1], 0, sizeof(lookBody[1]));
	lookBody[1].ismale = 1;
	lookBody[1].type = HEADBODYTYPE_DEFAULT;
	lookBody[1].height = 154;
	lookBody[1].filenum = bodyfile;
	lookBody[1].scale = 1;
	lookBody[1].animscale = 0.85915493965149f;
	lookBody[1].handfilenum = handfile;

	// The Shock Trooper's, with a model of their own: one head modeldef
	// cannot sit on two bodies (chrs-and-memory.md)
	lookHead[0] = g_HeadsAndBodies[HEAD_DDSHOCK];
	lookHead[0].modeldef = NULL;
	lookBody[0] = g_HeadsAndBodies[BODY_DDSHOCK];
	lookBody[0].modeldef = NULL;

	g_MpBodies[numbodies].bodynum = XBLA_AGENT4_BODYROW;
	g_MpBodies[numbodies].name = 0; // xblaAgent4BodyName(): a port text id is past an s16
	g_MpBodies[numbodies].headnum = XBLA_AGENT4_HEADROW;
	g_MpBodies[numbodies].requirefeature = 0;
	g_MpListCounts.bodies = numbodies + 1;

	g_MpHeads[numheads].headnum = XBLA_AGENT4_HEADROW;
	g_MpHeads[numheads].requirefeature = 0;
	g_MpListCounts.heads = numheads + 1;
	listed = 1;

	xblaAgent4MeshesSwitched();

	sysLogPrintf(LOG_NOTE, "xblaagent4: Agent 4 in the Combat Simulator's lists (files %d %d %d)",
			bodyfile, headfile, handfile);
}
