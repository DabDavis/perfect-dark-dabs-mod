#ifndef _IN_GEBEAN_H
#define _IN_GEBEAN_H

#include <PR/ultratypes.h>

#ifdef __cplusplus
extern "C" {
#endif

struct modeldef;
struct modelnode;

/**
 * GoldenEye 007 for the Xbox 360 - Rare's 2007 "Project Bean" build - as a
 * source of characters for GoldenEye X.
 *
 * The player drops their copy in xbla/ beside the Perfect Dark release: the
 * archive it came as, or the folder it unpacks to (the one holding default.xex
 * and files/). An archive's characters and heads, and nothing else of it, are
 * unpacked once into cache/xbla/goldeneye/.
 *
 * GoldenEye X keeps GoldenEye's N64 models under Perfect Dark's file names, so
 * a model is paired with Bean's by a table made offline (gebeantable.h, from
 * .xbla-work/ge-bean/gen_beantable.py): the file's name, and what its list
 * nodes and vertices add up to, so a different mod's file of the same name is
 * left alone. What the pairing gives back is a mesh in 4J's own layout, which
 * xblamesh.c draws like one of the Perfect Dark release's: Bean's vertices
 * re-posed onto the model's rest skeleton and skinned to the model's own
 * matrices, a group per list node. CLAUDE-notes/ge-bean.md has the formats and
 * the reasons.
 *
 * Mod.XblaGoldenEye switches it on.
 */

/**
 * g_HeadsAndBodies past the stock table, whose terminator is row 151 - the last
 * a mod's own table can import (moddata.c) - so everything from 152 is ours.
 *
 * A converted mission's bodies come first and have rows **kept for them**
 * (gexplus.c): a body's row has to fit the byte a packedchr and aiSpawnChrAtPad
 * hold it in, and the Combat Simulator's pool below - GoldenEye X's borrowed
 * characters are 106 rows of it - used to start at 152 too and left a mission
 * none under 256. Its spawn commands then kept GoldenEye's own numbers, and
 * Egyptian's Baron Samedi, who is GoldenEye's 12, spawned as Perfect Dark's 12:
 * Joanna's head, worn as a body, with no root matrix for a shot to be tested
 * against (chrTestHit()).
 *
 * The pool's rows are never written into a byte - a Combat Simulator body is
 * g_MpBodies[].bodynum, an s16 - so it is the one that moves up.
 */
#define GEROM_BODY_FIRST  152
#define GEROM_BODY_ROWS   24    // GEROM_MAX_ROWS: every row a mission may take
#define GEROM_BODY_LAST   (GEROM_BODY_FIRST + GEROM_BODY_ROWS - 1)
#define GEBEAN_POOL_BASE  (GEROM_BODY_LAST + 1)

/** Mod.XblaGoldenEye. */
s32 gebeanGetEnabled(void);
void gebeanSetEnabled(s32 enabled);

/** Whether a copy was found in xbla/, unpacked or not. Never unpacks. */
s32 gebeanIsAvailable(void);

/**
 * The table row for a model file as it loads, or -1: a mod's file (never a
 * stock one) whose name and shape are in the table.
 */
s32 gebeanFindRow(u16 fileid, struct modeldef *modeldef);

/** The row's file name, for the log. */
const char *gebeanRowName(s32 row);

/**
 * Whether the row is one of the Combat Simulator pool's (gebeanPoolRefresh())
 * rather than GoldenEye X's. A pool row stands on a Perfect Dark model, so
 * with the release's meshes off (F6) it takes Bean's N64-look original; a
 * GoldenEye X model is GoldenEye's N64 one already and draws itself.
 */
s32 gebeanRowIsPool(s32 row);

/**
 * Whether the row is a GoldenEye gun's first-person model. Its mesh is built
 * in each list node's own space, one group a node with no palette, so it is
 * drawn the way a model pack's is - under the node's own matrix - rather than
 * posed like a character.
 */
s32 gebeanRowIsFirstPerson(s32 row);

/**
 * Where the first-person gun drawn for this weapon ends, as an offset from its
 * host's MODELPART_GUN_MUZZLEPOS node in the model's own space, or 0 if the
 * host's own model is the one in the hand.
 *
 * Perfect Dark fires everything from that node - the bullet stream, a beam,
 * the smoke, a projectile - and it belongs to the host. A release gun of
 * another shape drawn on it ends somewhere else.
 *
 * `outpart` is the model part the offset is measured from, which is the
 * muzzle node where the host has one and its muzzle flash where it does not -
 * Perfect Dark's conversions of GoldenEye's submachine guns and rifles carry
 * no muzzle node at all.
 */
s32 gebeanFirstPersonMuzzleOffset(s32 weaponnum, s32 *outpart, f32 *out);

/**
 * GoldenEye's characters and heads in the Combat Simulator's own lists, for
 * Perfect Dark rather than for GoldenEye X: each one a row of g_HeadsAndBodies
 * past the stock table, whose file is an alias of a Perfect Dark body or head
 * (romdataRegisterAliasFile()) that the Bean mesh is skinned onto. Appended
 * when the switch is on, a copy is in xbla/ and the lists are the game's own -
 * a mod's lists (GoldenEye X's have GoldenEye's characters already) keep them
 * out - and taken off again otherwise. Called at boot, after a mod swap and
 * when the switch changes.
 */
void gebeanPoolRefresh(void);

/**
 * The release's meshes moved (F6). GoldenEye's guns follow that switch as its
 * characters do - the release's gun with the meshes on, GoldenEye's own N64
 * one with them off - and what has to move with it rather than at the draw is
 * whether Perfect Dark's hands are drawn, since the N64 gun carries
 * GoldenEye's own.
 */
void gebeanMeshesSwitched(void);

/** The Combat Simulator name of a pool body's g_HeadsAndBodies row, or NULL. */
const char *gebeanPoolBodyName(s32 bodynum);

/**
 * GoldenEye's name for a g_HeadsAndBodies head row that is one of its faces -
 * the release's pool, or GoldenEye X's own, borrowed or loaded - or NULL.
 */
const char *gebeanHeadName(s32 headnum);

/**
 * Makes sure the copy is on disk, unpacking it if it has to - which is a level
 * load's business, not a frame's, so a model load that finds a row calls this.
 * 1 when the files are there.
 */
s32 gebeanPrepare(void);
// The first unpack at startup, with a notice on the window while it works.
void gebeanUnpackAtStartup(void);

/**
 * Which of the model's matrices a list node is drawn under: the nearest
 * position or chrinfo node above it, and 0 for a head file's lists, which hang
 * under the body's neck (matrix 0) once grafted and under the head's one
 * matrix when the head is drawn on its own.
 */
s32 gebeanListNodeMatrix(const struct modelnode *node);

/**
 * The matrix a list node's own display list loads first (its first G_MTX), or
 * -1 when it loads none. A list is drawn under that one, whatever position
 * node it hangs under: the PP9i's gun list is under the root and loads 33.
 */
s32 gebeanListLoadedMatrix(const struct modelnode *node);
// The matrix each of a list node's vertices is loaded under, -1 before any;
// filebase is the model file's start, or NULL for a loaded model
s32 gebeanListVertexMatrices(const struct modelnode *node, const u8 *filebase, s16 *vtxmtx, s32 numvertices);
s32 gebeanIsPoolRow(s32 headorbodynum);

// The pool's body or head for one of GoldenEye's characters by its Bean source
// ("char/oliveguard"), or -1 when the pool is not Bean's.
s32 gebeanPoolNumBySource(const char *source);

#define GEBEAN_MAXMATS 256

/**
 * A GoldenEye XBLA level's HD mesh - files/new/background/<name> - for
 * gebeanstage.c, which serves it as the rooms of GoldenEye X's copy of the
 * level.
 */
struct gebeanlevel;

struct gebeanlevelvtx {
	f32 pos[3];
	f32 uv[2];
	u32 argb;
};

struct gebeanlevel *gebeanLevelOpen(const char *name);
void gebeanLevelClose(struct gebeanlevel *level);
s32 gebeanLevelTriangles(struct gebeanlevel *level,
		void (*fn)(void *arg, s32 tex, const struct gebeanlevelvtx *v), void *arg);
s32 gebeanLevelNumTextures(struct gebeanlevel *level);
const void *gebeanLevelTexture(struct gebeanlevel *level, s32 tex, u8 *alpha, u8 *soft);

/** The pictures a built mesh's material words index (XBLAMESH_MAT_TABLE). */
struct gebeanmats {
	u16 fileid;  // in: the model file the mesh is built for
	s32 num;
	const void *tile[GEBEAN_MAXMATS];
	u8 alpha[GEBEAN_MAXMATS];
	u8 soft[GEBEAN_MAXMATS];
	// Groups blanked because the head file carries this body's neck
	u64 neckblank;
	// For a neck node, the group holding the body's own neck to draw instead
	// under a head that is not its own; -1 for none
	s8 neckfill[64];
};

/**
 * The mesh for a row, in 4J's layout: group k is list node k of nodes, and a
 * bit of *outAbsent is set for a node that keeps its own geometry. The palette
 * is the model's matrices, holding each joint's inverse rest. NULL when the
 * copy is missing or the model is not the shape the row expects. The file is
 * malloc'd and the caller's. original takes the character from Bean's
 * files/original/ (the N64 look) instead of files/new/ (HD).
 */
u8 *gebeanBuild(s32 row, s32 original, struct modeldef *modeldef, struct modelnode **nodes, s32 numnodes,
		struct gebeanmats *mats, u64 *outAbsent, u32 *outLen);

#ifdef __cplusplus
}
#endif

#endif
