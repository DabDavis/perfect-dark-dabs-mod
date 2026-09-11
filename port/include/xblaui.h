#ifndef _IN_XBLAUI_H
#define _IN_XBLAUI_H

#include <PR/ultratypes.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The XBLA release's own interface art.
 *
 * Past the textures that carry a number and the font atlases, the package
 * holds the art 4J drew for the console's own frontend: the logo at record
 * 0dd9, the Xbox button and d-pad glyphs at 0dbf to 0dd8, the achievement
 * icons and 37 country flags at 0dda to 0e1d. None of it has a texture number
 * and none of it stands in for anything the ROM draws, so it can only be used
 * where this port puts it deliberately.
 *
 * The logo is the one piece that has somewhere to go: the page where the
 * release's art is switched on, as its banner. The rest is written down in
 * xbla.md and drawn by nothing - the port's menus name a button nowhere, and
 * the glyphs are the Xbox's own controller rather than the one a player of
 * this port is holding.
 */

struct menuimage;

/**
 * The logo, as a picture the menus can draw, or NULL when there is no package
 * on disk to read it out of.
 *
 * Registered with menuimage.c at startup either way, since that has to happen
 * before there is a second thread; what decides is whether the record is
 * there when the renderer first asks for the picture.
 */
struct menuimage *xblaUiGetLogo(void);

/** The logo's aspect, for a row working out the rectangle to draw it in. */
#define XBLAUI_LOGO_ASPECT(h) ((h) * 420 / 255)

#ifdef __cplusplus
}
#endif

#endif
