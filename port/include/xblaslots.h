#ifndef _IN_XBLASLOTS_H
#define _IN_XBLASLOTS_H

/**
 * Texture slots the XBLA release reused for new pictures.
 *
 * Record N of the release's Textures.raw is texture N of the ROM for the
 * first NUM_TEXTURES records - except for these, where 4J put a different
 * picture in the slot and pointed its rewritten rooms at it: 0222 is a
 * Chicago dataDyne sign in the ROM and a Villa cliff in the release, 08a2 is
 * Area 51's "51" wall and a grass bank. So a ROM room that binds one of them
 * must not be given the release's picture (the pack leaves them out, and a
 * pack already on disk is cleaned of them at startup), and a release room
 * that binds one must get the release's picture whatever pack is on (the
 * level loader draws it through the meshes' stand-in tile, the way it draws
 * the records past NUM_TEXTURES).
 *
 * Found by loading every room of every level twice, from the ROM's copy and
 * from the release's, with a gdb breakpoint on texLoadFromGdl() printing
 * each texture number: a slot the release's rooms bind that the ROM's rooms
 * of the same level do not is one 4J reused. See CLAUDE-notes/xbla.md,
 * "The slots 4J reused". tools/texpack/xblaconvert.py reads this file, so
 * the list lives here and nowhere else. Keep it sorted.
 */
#define XBLA_REUSED_SLOTS \
	0x0222, 0x0224, 0x0227, 0x022f, 0x08a2, 0x08a3

#endif
