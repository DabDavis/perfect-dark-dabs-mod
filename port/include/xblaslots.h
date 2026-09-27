#ifndef _IN_XBLASLOTS_H
#define _IN_XBLASLOTS_H

/**
 * Texture slots the XBLA release reused for new pictures.
 *
 * Record N of the release's Textures.raw is texture N of the ROM for the
 * first NUM_TEXTURES records - except for these, where 4J put a different
 * picture in the slot and pointed its rewritten rooms at it: 0222 is a
 * police car's light bar in the ROM and a Villa cliff in the release, 08a2
 * is Area 51's "51" wall and a grass bank, 0216 is the taxi's "CAB" sign
 * and a Defection console panel. So a ROM room or a ROM model that binds one
 * of them must not be given the release's picture (the pack leaves them out,
 * and a pack already on disk is cleaned of them at startup), and a release
 * room that binds one must get the release's picture whatever pack is on
 * (the level loader draws it through the meshes' stand-in tile, the way it
 * draws the records past NUM_TEXTURES).
 *
 * Found by reading the rooms of all 60 bg files twice, from the ROM's copy
 * and from the release's, and collecting every 0xc0 texture command:
 * tools/texpack/bgtexscan.py. A slot the release's rooms of a level bind
 * that the ROM's rooms of that same level do not is a candidate - 44 of
 * them - and the two pictures then settle it, because 4J also retextured
 * surfaces with slots the level had simply not used before (0962 is sand in
 * both copies, 0a4b a blue swirl in both). The twenty-two below that a
 * release room binds are the ones where the picture changed subject.
 *
 * The rest are the slots no room binds because a *model* binds them, which
 * the room diff cannot see. Every reused slot the diff did find belongs to
 * one of five ROM props - taxicab, policecar, hovbike, a51interceptor,
 * dd_hovercopter, the Chicago and Area 51 vehicles - so all 79 slots those
 * models bind were compared picture by picture as well (--models). 0217,
 * 0230 and 08ac are the three more that came out of it; they are in the
 * pack's half of this list alone, since no room of either copy binds them.
 *
 * Slots a redraw rather than a reuse, left out deliberately: 0215 is the rope
 * prop's, purple in the ROM and planks in the release, and both copies' Air
 * Base rooms bind it; 08ad is the rubber plant's leaf in both; 08b8 is the
 * hovercopter's rotor, still a rotor. A false entry here costs an upscale
 * and mis-scales the release room that binds the slot, so the picture has to
 * say so.
 *
 * Two slots kept their subject and changed their layout, which the diff
 * cannot see because both copies' rooms bind them: 00dd and 00c2,
 * Investigation's wall terminal screens (steel frame and wood frame), are
 * one framed screen each in the ROM and two stacked in the release, and the
 * release rooms map one of the two. A pack's single screen on those
 * coordinates spilled its black over the terminal's bezel. The release's
 * numbered records whose shape is not the ROM tile's were put side by side
 * with a pack's to find 00c2 (tools/texpack/xblaconvert.py's read_records,
 * record size against source size).
 *
 * 007b was once listed above as a redraw. It is a re-laid slot like those
 * two, only more so: the ROM's is Defection's 48x32 "BIG-BOY" billboard
 * poster, the release's a 1024x1024 atlas of the skyline towers round the
 * rooftop - facades, window strips, roofs - and 4J's rooms of bg_ame map
 * whole towers onto it. Any 007b that is not the release's own then paints
 * the poster over the towers in red and orange streaks: a pack's (PD
 * Forever Plus HD ships the poster), or the ROM's whenever the numbered
 * record is not asked for (F3 20260927-083325).
 *
 * See CLAUDE-notes/xbla.md, "The slots 4J reused".
 * tools/texpack/xblaconvert.py reads this file, so the list lives here and
 * nowhere else. Keep it sorted.
 */
#define XBLA_REUSED_SLOTS \
	0x007b, 0x00a5, 0x00a9, 0x00c2, 0x00dd, 0x0216, 0x0217, 0x0219, \
	0x021a, 0x021b, 0x0221, 0x0222, 0x0223, 0x0224, 0x0226, 0x0227, \
	0x022f, 0x0230, 0x089e, 0x089f, 0x08a2, 0x08a3, 0x08ac, 0x08bb, \
	0x08bf, 0x08c0, 0x08c1, 0x08c3

#endif
