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
 * Makes sure the copy is on disk, unpacking it if it has to - which is a level
 * load's business, not a frame's, so a model load that finds a row calls this.
 * 1 when the files are there.
 */
s32 gebeanPrepare(void);

/**
 * Which of the model's matrices a list node is drawn under: the nearest
 * position or chrinfo node above it, and 0 for a head file's lists, which hang
 * under the body's neck (matrix 0) once grafted and under the head's one
 * matrix when the head is drawn on its own.
 */
s32 gebeanListNodeMatrix(const struct modelnode *node);

#define GEBEAN_MAXMATS 256

/** The pictures a built mesh's material words index (XBLAMESH_MAT_TABLE). */
struct gebeanmats {
	s32 num;
	const void *tile[GEBEAN_MAXMATS];
	u8 alpha[GEBEAN_MAXMATS];
	u8 soft[GEBEAN_MAXMATS];
};

/**
 * The mesh for a row, in 4J's layout: group k is list node k of nodes, and a
 * bit of *outAbsent is set for a node that keeps its own geometry. The palette
 * is the model's matrices, holding each joint's inverse rest. NULL when the
 * copy is missing or the model is not the shape the row expects. The file is
 * malloc'd and the caller's.
 */
u8 *gebeanBuild(s32 row, struct modeldef *modeldef, struct modelnode **nodes, s32 numnodes,
		struct gebeanmats *mats, u64 *outAbsent, u32 *outLen);

#ifdef __cplusplus
}
#endif

#endif
