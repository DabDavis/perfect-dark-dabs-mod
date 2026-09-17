#ifndef _IN_GEBEANSTAGE_H
#define _IN_GEBEANSTAGE_H

#include <stdio.h>
#include <PR/ultratypes.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * GoldenEye XBLA's HD levels drawn as the rooms of GoldenEye X's.
 *
 * Project Bean (gebean.h) keeps each level's drawn mesh in
 * files/new/background/<name>: one batch sorted by material, no rooms, no
 * portals. GoldenEye X kept GoldenEye's N64 geometry in Perfect Dark's room
 * format, and the Bean file holds every one of those N64 vertices at a scale
 * of its own, so each GE-X level file is paired with its Bean level offline
 * (gebeanstagetable.h, from .xbla-work/ge-bean/gen_stagetable.py) by its room
 * count and room positions.
 *
 * When a paired level is running, the XBLA meshes and stages switches are on
 * and Mod.XblaGoldenEye is on, every Bean triangle is dealt to the GE-X room
 * whose own triangle it lies on, and each room whose surface Bean's mesh
 * covers is written in the ROM's room format and served to bgLoadRoom()
 * through xblastage.c's hooks, the way the Perfect Dark release's rooms are.
 * A room GE-X changed from GoldenEye's keeps GE-X's own geometry. The
 * portals, lights, collision (tiles) and props stay GE-X's.
 */

/** Whether the running level has Bean rooms to serve; builds them the first time. */
u32 gebeanStageRoomSize(s32 roomnum);
uintptr_t gebeanStageRoomRead(s32 roomnum, u8 *dst, u32 len);

/** A new level: the last one's rooms and mesh go. */
void gebeanStageLevelReset(void);

/**
 * Texture numbers from GEBEANSTAGE_TEXBASE up in a served room are the Bean
 * level's own pictures, drawn through xblaStageWriteTexture() on a 32x32
 * stand-in tile.
 */
#define GEBEANSTAGE_TEXBASE 0xc000
#define GEBEANSTAGE_TEXNONE 0xcfff

s32 gebeanStageOwnsRecord(u32 record);
const void *gebeanStageTile(u32 record);

void gebeanStageTrace(FILE *f);

#ifdef __cplusplus
}
#endif

#endif
