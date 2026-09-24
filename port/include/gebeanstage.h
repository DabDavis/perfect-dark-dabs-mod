#ifndef _IN_GEBEANSTAGE_H
#define _IN_GEBEANSTAGE_H

#include <stdio.h>
#include <PR/ultratypes.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * GoldenEye XBLA's HD levels drawn as the rooms of the levels GE Plus
 * converts from the player's GoldenEye ROM.
 *
 * Project Bean (gebean.h) keeps each level's drawn mesh in
 * files/new/background/<name>: one batch sorted by material, no rooms, no
 * portals. It is GoldenEye's own N64 geometry remade at a scale of its own -
 * 4J built the release on the cartridge's levels - and the conversion
 * (geconvert.c) is that same geometry in Perfect Dark's room format, so the
 * two are paired by GoldenEye's own name for the level and nothing else
 * (gebeanstagetable.h, from .xbla-work/ge-bean/gen_stagetable.py).
 *
 * When a paired level is running, the XBLA meshes and stages switches are on
 * and the release is here (gebeanGetEnabled()), every Bean triangle is dealt
 * to the room whose own triangle it lies on, and each room Bean's mesh reaches
 * is written in the ROM's room format and served to bgLoadRoom() through
 * xblastage.c's hooks, the way the Perfect Dark release's rooms are. A room
 * Bean has nothing in keeps the converted file's own geometry. The portals,
 * lights, collision (tiles) and props stay the conversion's.
 */

/** Whether the running level has Bean rooms to serve; builds them the first time. */
u32 gebeanStageRoomSize(s32 roomnum);
uintptr_t gebeanStageRoomRead(s32 roomnum, u8 *dst, u32 len);

/**
 * A level converted from GoldenEye's own data is served whole: its portals
 * are the N64 level's and do not see what the HD mesh opens up, so its rooms
 * are drawn by distance instead (xblaStageDrawsEveryRoom()).
 */
s32 gebeanStageDrawsEveryRoom(void);

/** GoldenEye's own key for the level being served in HD ("dam"), or NULL. */
const char *gebeanStageLevelKey(void);

/**
 * Once a frame, after the camera is placed: whether one of GoldenEye's own
 * cameras (authored - an opening shot, the swirl, a cutscene) stands outside
 * the level, where the HD rooms are drawn with their back faces culled
 * (gebeanStageCullsBackFaces(), read by bgRenderRoomOpaque()).
 */
void gebeanStageTickCamera(s32 authored);
s32 gebeanStageCullsBackFaces(void);

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
