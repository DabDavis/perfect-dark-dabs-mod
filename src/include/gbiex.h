#ifndef _IN_GBIEX_H
#define _IN_GBIEX_H

/**
 * 07 gSPColor - copy colors from segment + offset into DMEM
 *
 * upper word
 * 00FF0000	- number of bytes to copy minus 4 (eg. 4 colours = 0x0c)
 * 0000FFFF	- nubmer of bytes to copy
 *
 * lower word
 * 0F000000	- segment
 * 00FFFFFF	- offset in color table
 */
#define gSPColor(pkt, v, n)                           \
    gDma1p(pkt, G_COL, v, sizeof(Col)*(n),((n)-1)<<2)

#define gsSPColor(v, n, v0)                        \
    gsDma1p(G_COL, v, sizeof(Col)*(n), ((n)-1)<<2)

/**
 * B1	rsp_tri4
 * Draws up to four triangles at a time.
 * Expects values from 0-F, corresponding with # points declared by vertex command.
 * Triangles with all points set to 0 are not drawn.
 *
 * upper word
 * 0000F000	z4
 * 00000F00	z3
 * 000000F0	z2
 * 0000000F	z1
 *
 * lower word
 * F0000000	y4
 * 0F000000	x4
 * 00F00000	y3
 * 000F0000	x3
 * 0000F000	y2
 * 00000F00	x2
 * 000000F0	y1
 * 0000000F	x1
 */
#define	gSPTri4(pkt, x1, y1, z1, x2, y2, z2, x3, y3, z3, x4, y4, z4) \
{                                                                    \
    Gfx *_g = (Gfx *)(pkt);                                          \
    _g->words.w0 = (_SHIFTL(G_TRI4, 24, 8)                           \
            | _SHIFTL(z4, 12, 4)                                     \
            | _SHIFTL(z3, 8, 4)                                      \
            | _SHIFTL(z2, 4, 4)                                      \
            | _SHIFTL(z1, 0, 4));                                    \
    _g->words.w1 = (_SHIFTL(y4, 28, 4)                               \
            | _SHIFTL(x4, 24, 4)                                     \
            | _SHIFTL(y3, 20, 4)                                     \
            | _SHIFTL(x3, 16, 4)                                     \
            | _SHIFTL(y2, 12, 4)                                     \
            | _SHIFTL(x2, 8, 4)                                      \
            | _SHIFTL(y1, 4, 4)                                      \
            | _SHIFTL(x1, 0, 4));                                    \
}

#define gSPTri3(pkt, x1, y1, z1, x2, y2, z2, x3, y3, z3)      \
    gSPTri4(pkt, x1, y1, z1, x2, y2, z2, x3, y3, z3, 0, 0, 0)

#define gSPTri2(pkt, x1, y1, z1, x2, y2, z2)               \
    gSPTri4(pkt, x1, y1, z1, x2, y2, z2, 0, 0, 0, 0, 0, 0)

#define gSPTri1(pkt, x1, y1, z1)                        \
    gSPTri4(pkt, x1, y1, z1, 0, 0, 0, 0, 0, 0, 0, 0, 0)

#define	gDPLoadTLUT06(pkt, a, b, c, d)				                                        \
{                                                                                           \
	Gfx *_g = (Gfx *)pkt;                                                                   \
	_g->words.w0 = _SHIFTL(G_LOADTLUT, 24, 8) | _SHIFTL((a), 14, 10) | _SHIFTL((b), 2, 10); \
	_g->words.w1 = _SHIFTL(0x06, 24, 8) | _SHIFTL((c), 14, 10) | _SHIFTL((d), 2, 10);       \
}

/**
 * Like gDPSetPrimColor, but is useful when the input colour is already in
 * RGBA format. It avoids unnecessary bitshifting and masking.
 */
#define	gDPSetPrimColorViaWord(pkt, m, l, rgba)     \
{                                                   \
	Gfx *_g = (Gfx *)(pkt);                         \
	_g->words.w0 =	(_SHIFTL(G_SETPRIMCOLOR, 24, 8) \
			| _SHIFTL(m, 8, 8)                      \
			| _SHIFTL(l, 0, 8));                    \
	_g->words.w1 =  (rgba);                         \
}

#define	gDPSetEnvColorViaWord(pkt, rgba) gDPSetColor(pkt, G_SETENVCOLOR, rgba)
#define	gDPSetFogColorViaWord(pkt, rgba) gDPSetColor(pkt, G_SETFOGCOLOR, rgba)

/**
 * gDPFillRectangleScaled - a wrapper around gDPFillRectangle which applies
 * g_ScaleX to the X coordinates.
 *
 * g_ScaleX is normally 1, but 2 when using hi-res.
 */
#define gDPFillRectangleScaled(pkt, x1, y1, x2, y2) gDPFillRectangle(pkt, (x1) * g_ScaleX, y1, (x2) * g_ScaleX, y2)

#define gDPHudRectangle(pkt, x1, y1, x2, y2) gDPFillRectangle(pkt, (x1) * g_ScaleX, y1, ((x2 + 1)) * g_ScaleX, (y2) + 1)

/**
 * Custom combiner modes.
 *
 * Modes 6-10 are copies of the following but they replace
 * SHADE with ENVIRONMENT in the alpha half:
 *
 * 06: G_CC_MODULATEIA2
 * 07: G_CC_MODULATEIA
 * 08: G_CC_MODULATEI2
 * 09: G_CC_MODULATEI
 * 10: G_CC_SHADE
 *
 * Modes 12-16 are copies of the following but they replace
 * SHADE with SCALE in the colour half:
 *
 * 12: G_CC_MODULATEI
 * 13: G_CC_MODULATEIA
 * 14: G_CC_SHADE
 * 15: G_CC_MODULATEI2
 * 16: G_CC_MODULATEIA2
 *
 * Summary of modes:
 * 00:  T0*EV          T0*EV
 * 01:  CM             (1-CM)*PR+CM
 * 02:  PR             T0*PR
 * 03:  CM*EV          CM*EV
 * 04:  SH             T0*SH
 * 05:  T1*SC          T1*PL
 * 06:  CM*SH          CM*EV
 * 07:  T0*SH          T0*EV
 * 08:  CM*SH          EV
 * 09:  T0*SH          EV
 * 10:  SH             EV
 * 11:  (T1-T0)*LF+T0  T1
 * 12:  T0*SC          SH
 * 13:  T0*SC          T0*SH
 * 14:  SC             SH
 * 15:  CM*SC          SH
 * 16:  CM*SC          CM*SH
 * 17:  (T0-EV)*SA+EV  (T0-EV)*SH+EV
 * 18:  CM*SH          CM
 * 19:  (T0-EV)*SA+EV  T0*EV
 * 20:  CM*SH          CM*SH+PR
 * 21:  (T1-T0)*LF+T0  SH+EV
 * 22:  (T1-T0)*LF+T0  (SH-EV)*T0
 * 23:  CM*SH          PR+CM
 * 24:  (T1-T0)*LF+T0  (1-SH)*EV
 * 25:  (T1-T0)*LF+T0  EV
 * 26:  (T1-T0)*LF+T0  T0*EV
 * 27:  (PR-EV)*T0+EV  (PR-EV)*T0+EV
 */
#define G_CC_CUSTOM_00  TEXEL0,    0,           ENVIRONMENT,  0,           TEXEL0,    0,           ENVIRONMENT,   0
#define G_CC_CUSTOM_01  0,         0,           0,            COMBINED,    1,         COMBINED,    PRIMITIVE,     COMBINED
#define G_CC_CUSTOM_02  0,         0,           0,            PRIMITIVE,   TEXEL0,    0,           PRIMITIVE,     0
#define G_CC_CUSTOM_03  COMBINED,  0,           ENVIRONMENT,  0,           COMBINED,  0,           ENVIRONMENT,   0
#define G_CC_CUSTOM_04  0,         0,           0,            SHADE,       TEXEL0,    0,           SHADE,         0
#define G_CC_CUSTOM_05  TEXEL1,    0,           SCALE,        0,           TEXEL1,    0,           PRIM_LOD_FRAC, 0
#define G_CC_CUSTOM_06  COMBINED,  0,           SHADE,        0,           COMBINED,  0,           ENVIRONMENT,   0
#define G_CC_CUSTOM_07  TEXEL0,    0,           SHADE,        0,           TEXEL0,    0,           ENVIRONMENT,   0
#define G_CC_CUSTOM_08  COMBINED,  0,           SHADE,        0,           0,         0,           0,             ENVIRONMENT
#define G_CC_CUSTOM_09  TEXEL0,    0,           SHADE,        0,           0,         0,           0,             ENVIRONMENT
#define G_CC_CUSTOM_10  0,         0,           0,            SHADE,       0,         0,           0,             ENVIRONMENT
#define G_CC_CUSTOM_11  TEXEL1,    TEXEL0,      LOD_FRACTION, TEXEL0,      1,         0,           TEXEL1,        0
#define G_CC_CUSTOM_12  TEXEL0,    0,           SCALE,        0,           0,         0,           0,             SHADE
#define G_CC_CUSTOM_13  TEXEL0,    0,           SCALE,        0,           TEXEL0,    0,           SHADE,         0
#define G_CC_CUSTOM_14  1,         0,           SCALE,        0,           0,         0,           0,             SHADE
#define G_CC_CUSTOM_15  COMBINED,  0,           SCALE,        0,           0,         0,           0,             SHADE
#define G_CC_CUSTOM_16  COMBINED,  0,           SCALE,        0,           COMBINED,  0,           SHADE,         0
#define G_CC_CUSTOM_17  TEXEL0,    ENVIRONMENT, SHADE_ALPHA,  ENVIRONMENT, TEXEL0,    ENVIRONMENT, SHADE,         ENVIRONMENT
#define G_CC_CUSTOM_18  COMBINED,  0,           SHADE,        0,           0,         0,           0,             COMBINED
#define G_CC_CUSTOM_19  TEXEL0,    ENVIRONMENT, SHADE_ALPHA,  ENVIRONMENT, TEXEL0,    0,           ENVIRONMENT,   0
#define G_CC_CUSTOM_20  COMBINED,  0,           SHADE,        0,           COMBINED,  0,           SHADE,         PRIMITIVE
#define G_CC_CUSTOM_21  TEXEL1,    TEXEL0,      LOD_FRACTION, TEXEL0,      1,         0,           SHADE,         ENVIRONMENT
#define G_CC_CUSTOM_22  TEXEL1,    TEXEL0,      LOD_FRACTION, TEXEL0,      SHADE,     ENVIRONMENT, TEXEL0,        0
#define G_CC_CUSTOM_23  COMBINED,  0,           SHADE,        0,           1,         0,           PRIMITIVE,     COMBINED
// Glass See-Through (modGetGlassSeeThrough()): the second cycle of 20 and 23
// with the primitive alpha laid over the texel's as a blend, (1 - a) * prim + a,
// where theirs adds it. A pane then keeps (1 - prim) of its own see-through
// rather than going opaque as soon as prim reaches 1 - a. Identical at prim 0.
#define G_CC_CUSTOM_GLASS_LERP  COMBINED,  0,     SHADE,        0,           1,         COMBINED,    PRIMITIVE,     COMBINED
#define G_CC_CUSTOM_24  TEXEL1,    TEXEL0,      LOD_FRACTION, TEXEL0,      1,         SHADE,       ENVIRONMENT,   0
#define G_CC_CUSTOM_25  TEXEL1,    TEXEL0,      LOD_FRACTION, TEXEL0,      1,         0,           ENVIRONMENT,   0
#define G_CC_CUSTOM_26  TEXEL1,    TEXEL0,      LOD_FRACTION, TEXEL0,      TEXEL0,    0,           ENVIRONMENT,   0
#define G_CC_CUSTOM_27  PRIMITIVE, ENVIRONMENT, TEXEL0,       ENVIRONMENT, PRIMITIVE, ENVIRONMENT, TEXEL0,        ENVIRONMENT

#ifndef PLATFORM_N64

/* Extended commands */

#define G_SETFB_EXT                  0x21
#define G_SETTIMG_FB_EXT             0x23
#define G_INVALTEXCACHE_EXT          0x34
#define G_TEXRECT_WIDE_EXT           0x37
#define G_FILLRECT_WIDE_EXT          0x38
#define G_SETGRAYSCALE_EXT           0x39
#define G_EXTRAGEOMETRYMODE_EXT      0x3a
#define G_SETINTENSITY_EXT           0x40
#define G_COPYFB_EXT                 0x41
#define G_IMAGERECT_EXT              0x42
#define G_RDPFLUSH_EXT               0x43
#define G_CLEAR_DEPTH_EXT            0x44
#define G_SETSUBPIXELOFFSET_EXT      0x45
#define G_SETFONTGLYPH_EXT           0x46
#define G_SETTEXGENSHIFT_EXT         0x47
#define G_SETRECTDEPTH_EXT           0x48
#define G_SETDEPTHBIAS_EXT           0x49
#define G_TAA_EXT                    0x4a
#define G_SETFOGLINE_EXT             0x4b

/* G_EXTRAGEOMETRYMODE flags */

#define G_INVERT_CULLING_EXT     0x00000001
#define G_ASPECT_LEFT_EXT        0x00000010
#define G_ASPECT_RIGHT_EXT       0x00000020
#define G_ASPECT_WIDE_EXT        0x00000040
#define G_ASPECT_CENTER_EXT      (G_ASPECT_LEFT_EXT | G_ASPECT_RIGHT_EXT)
#define G_ASPECT_MODE_EXT        (G_ASPECT_CENTER_EXT | G_ASPECT_WIDE_EXT)
#define G_NO_CLIPPING_EXT        0x00000100
#define G_MODULATE_EXT           0x00000200 // this should really go into OTHERMODE_H, but for some reason I can't get it to work
#define G_ADDITIVE_EXT           0x00000400 // with a blending render mode: source times its alpha added to what is there
#define G_ENVMAP_EXT             0x00000800 // texel 0 is a sphere-map atlas looked up per pixel: see gfx_pc.cpp
#define G_TEXGEN_TURN_EXT        0x00001000 // with G_TEXGEN_EYE_EXT: G_SETTEXGENSHIFT_EXT's shift turns the LookAt (a fraction of a turn) instead of being added (the levels' reflections)
#define G_TEXGEN_FACE_EXT        0x00004000 // with G_LIGHTING: each triangle is lit and texgenned from its own face normal, turned to the eye, not from its vertices' colours (a flat room panel with no normals of its own)
#define G_TEXGEN_EYE_EXT         0x00002000 // with G_TEXTURE_GEN: the lookup follows the eye ray and adds G_SETTEXGENSHIFT_EXT's shift (the K7 sheen)
#define G_DECAL_EXT              0x00008000 // what follows is drawn in the decal z mode (ZMODE_DEC) whatever the render mode says: a mesh's overlay flat on its own surface (xblamesh.c)

/* Extra texture filtering mode */

#define G_TF_BLUR_EXT (1 << G_MDSFT_TEXTFILT)

/* Extended command macros */

#define gDPSetFramebufferTargetEXT(pkt, f, s, w, i) \
    gSetImage(pkt, G_SETFB_EXT, f, s, w, i)

#define gDPSetFramebufferTextureEXT(pkt, f, s, w, i) \
    gSetImage(pkt, G_SETTIMG_FB_EXT, f, s, w, i)

/*
 * Names the glyph a font texture is about to draw, for the texture pack.
 *
 * A glyph is uploaded from font->chars[n].pixeldata, so its address does
 * identify it - but the fill and the outline of the same glyph are drawn from
 * the same address by two different renderers, and a pack ships a different
 * image for each. The display list is built before it is run, so which one this
 * is cannot be known later from anything else; it has to be said here.
 *
 * Applies to the next gDPSetTextureImage and nothing after it. Nothing about
 * the glyph's size travels with it: a replacement image is of the whole tile
 * the glyph is loaded as - 16 texels wide, the character in its top-left
 * corner - so the renderer maps it exactly as it maps the game's own texels.
 */
#define gDPSetFontGlyphEXT(pkt, font, glyph, outline)          \
{                                                              \
    Gfx *_g = (Gfx *)(pkt);                                    \
                                                               \
    _g->words.w0 = _SHIFTL(G_SETFONTGLYPH_EXT, 24, 8);         \
    _g->words.w1 = _SHIFTL(outline, 24, 8)                     \
        | _SHIFTL(font, 16, 8) | _SHIFTL(glyph, 0, 16);        \
}

#define gDPCopyFramebufferEXT(pkt, dst, src, uls, ult, back)   \
{                                                              \
    Gfx *_g = (Gfx *)(pkt);                                    \
                                                               \
    _g->words.w0 = _SHIFTL(G_COPYFB_EXT, 24, 8)                \
        | _SHIFTL(dst, 11, 11) | _SHIFTL(src, 0, 11) |         \
          _SHIFTL(back, 22, 1);                                \
    _g->words.w1 = _SHIFTL(uls, 16, 16) | _SHIFTL(ult, 0, 16); \
}

#define gDPInvalTexCacheEXT(pkt, addr)                 \
{                                                      \
    Gfx *_g = (Gfx *)(pkt);                            \
                                                       \
    _g->words.w0 = _SHIFTL(G_INVALTEXCACHE_EXT, 24, 8);\
    _g->words.w1 = (uintptr_t)(addr);                  \
}

#define gDPGrayscaleEXT(pkt, state)                    \
{                                                      \
    Gfx* _g = (Gfx*)(pkt);                             \
                                                       \
    _g->words.w0 = _SHIFTL(G_SETGRAYSCALE_EXT, 24, 8); \
    _g->words.w1 = state;                              \
}

#define gDPSetGrayscaleColorEXT(pkt, r, g, b, lerp) DPRGBColor(pkt, G_SETINTENSITY_EXT, r, g, b, lerp)

// NOTE: these will function correctly only if you pass `gdl++` as `pkt`

#define gDPFillRectangleWideEXT(pkt, ulx, uly, lrx, lry)                         \
{                                                                                \
    Gfx *_g0 = (Gfx*)(pkt), *_g1 = (Gfx*)(pkt);                                  \
                                                                                 \
    _g0->words.w0 = _SHIFTL(G_FILLRECT_WIDE_EXT, 24, 8) | _SHIFTL((lrx), 2, 22); \
    _g0->words.w1 = _SHIFTL((lry), 2, 22);                                       \
    _g1->words.w0 = _SHIFTL((ulx), 2, 22);                                       \
    _g1->words.w1 = _SHIFTL((uly), 2, 22);                                       \
}

#define gSPTextureRectangleWideEXT(pkt, xl, yl, xh, yh, tile, s, t, dsdx, dtdy, flip)         \
{                                                                                             \
    Gfx *_g0 = (Gfx*)(pkt), *_g1 = (Gfx*)(pkt), *_g2 = (Gfx*)(pkt);                           \
                                                                                              \
    _g0->words.w0 = _SHIFTL(G_TEXRECT_WIDE_EXT, 24, 8) | _SHIFTL((xh), 0, 24);                \
    _g0->words.w1 = (_SHIFTL((yh), 0, 24) | _SHIFTL((tile), 24, 3) | _SHIFTL((flip), 27, 1)); \
    _g1->words.w0 = _SHIFTL((xl), 0, 24);                                                     \
    _g1->words.w1 = _SHIFTL((yl), 0, 24);                                                     \
    _g2->words.w0 = (_SHIFTL(s, 16, 16) | _SHIFTL(t, 0, 16));                                 \
    _g2->words.w1 = (_SHIFTL(dsdx, 16, 16) | _SHIFTL(dtdy, 0, 16));                           \
}

#define gSPImageRectangleEXT(pkt, x0, y0, s0, t0, x1, y1, s1, t1, tile, iw, ih) \
{                                                                               \
    Gfx *_g0 = (Gfx*)(pkt), *_g1 = (Gfx*)(pkt), *_g2 = (Gfx*)(pkt);             \
                                                                                \
    _g0->words.w0 = _SHIFTL(G_IMAGERECT_EXT, 24, 8) | _SHIFTL((tile), 0, 3);    \
    _g0->words.w1 = _SHIFTL((iw), 16, 16) | _SHIFTL((ih), 0, 16);               \
    _g1->words.w0 = _SHIFTL((x0), 16, 16) | _SHIFTL((y0), 0, 16);               \
    _g1->words.w1 = _SHIFTL((s0), 16, 16) | _SHIFTL((t0), 0, 16);               \
    _g2->words.w0 = _SHIFTL((x1), 16, 16) | _SHIFTL((y1), 0, 16);               \
    _g2->words.w1 = _SHIFTL((s1), 16, 16) | _SHIFTL((t1), 0, 16);               \
}

#define gSPExtraGeometryModeEXT(pkt, c, s)                                              \
{                                                                                       \
    Gfx* _g = (Gfx*)(pkt);                                                              \
                                                                                        \
    _g->words.w0 = _SHIFTL(G_EXTRAGEOMETRYMODE_EXT, 24, 8) | _SHIFTL(~(u32)(c), 0, 24); \
    _g->words.w1 = (u32)(s);                                                            \
}

#define gDPSetSubpixelOffsetEXT(pkt, x, y)                                             \
{                                                                                      \
    Gfx *_g = (Gfx*)(pkt);                                                             \
                                                                                       \
    _g->words.w0 = _SHIFTL(G_SETSUBPIXELOFFSET_EXT, 24, 8) | _SHIFTL((s16)(x), 0, 16); \
    _g->words.w1 = _SHIFTL((s16)(y), 0, 16);                                           \
}

/*
 * Added to the texgen's s and t under G_TEXGEN_EYE_EXT, in 1/16384ths of the
 * span a normal's whole range covers (16384 is a whole span).
 */
#define gDPSetTexgenShiftEXT(pkt, s, t)                                                \
{                                                                                      \
    Gfx *_g = (Gfx*)(pkt);                                                             \
                                                                                       \
    _g->words.w0 = _SHIFTL(G_SETTEXGENSHIFT_EXT, 24, 8) | _SHIFTL((s16)(s), 0, 16);    \
    _g->words.w1 = _SHIFTL((s16)(t), 0, 16);                                           \
}

/*
 * Rectangles after this are drawn at normalised depth z (-1 near, 1 far) and
 * depth tested against the scene without writing (on), or in front of
 * everything as stock (off). z goes in w1 as a signed fraction of 2^30.
 */
#define gDPSetRectDepthEXT(pkt, on, z)                                                 \
{                                                                                      \
    Gfx *_g = (Gfx*)(pkt);                                                             \
                                                                                       \
    _g->words.w0 = _SHIFTL(G_SETRECTDEPTH_EXT, 24, 8) | _SHIFTL((on) ? 1 : 0, 0, 1);   \
    _g->words.w1 = (u32)(s32)((z) * 1073741824.0f);                                    \
}

/*
 * Push what follows away from the eye by this many of the depth buffer's
 * smallest steps (0 for none), on top of whatever its z mode offsets by.
 */
#define gDPSetDepthBiasEXT(pkt, units)                                                 \
{                                                                                      \
    Gfx *_g = (Gfx*)(pkt);                                                             \
                                                                                       \
    _g->words.w0 = _SHIFTL(G_SETDEPTHBIAS_EXT, 24, 8);                                 \
    _g->words.w1 = (u32)(s32)(units);                                                  \
}

/*
 * The fog line gSPFogFactor() sets - factor = z/w * mul + offset, 0 to 255 -
 * as a float, one half a command: G_FOGLINE_OFFSET_EXT in flags for the
 * offset, the multiplier without. For fog further out than an s16 multiplier
 * reaches under a near plane of a few units, where the z range from fog to
 * full fog is a few ten-thousandths. With G_FOGLINE_LINEAR_EXT (on both
 * halves) the line is evaluated at the eye depth instead, factor = w * mul +
 * offset: fog linear in distance. gSPFogFactor()/gSPFogPosition() put the
 * line back to z/w.
 */
#define G_FOGLINE_OFFSET_EXT 0x1
#define G_FOGLINE_LINEAR_EXT 0x2

#define gSPFogLineEXT(pkt, flags, value)                                               \
{                                                                                      \
    Gfx *_g = (Gfx *)(pkt);                                                            \
    union { f32 f; u32 u; } _l;                                                        \
                                                                                       \
    _l.f = (value);                                                                    \
    _g->words.w0 = _SHIFTL(G_SETFOGLINE_EXT, 24, 8) | _SHIFTL((flags), 0, 2);          \
    _g->words.w1 = _l.u;                                                               \
}

/*
 * TAA (Video.TAA): brackets a player's world - sky, rooms, props, beams,
 * sparks - and leaves out the gun, the HUD and the glares after it. BEGIN
 * starts the sub-pixel jitter; END stops it and resolves the world against
 * the frame before it, reprojected through the depth buffer by the camera.
 * mtx is 16 floats, row-vector world -> clip (the draw space's offset and
 * scale folded in), in memory that lives until the frame is drawn; slot is
 * the player, whose history it is.
 */
#define gSPTaaEXT(pkt, begin, slot, mtx)                               \
{                                                                      \
    Gfx *_g = (Gfx *)(pkt);                                            \
                                                                       \
    _g->words.w0 = _SHIFTL(G_TAA_EXT, 24, 8) | _SHIFTL((begin), 8, 1) \
        | _SHIFTL((slot), 0, 8);                                       \
    _g->words.w1 = (uintptr_t)(mtx);                                   \
}

#define gSPSetExtraGeometryModeEXT(pkt, word) gSPExtraGeometryModeEXT((pkt), 0, word)
#define gSPClearExtraGeometryModeEXT(pkt, word) gSPExtraGeometryModeEXT((pkt), word, 0)

#define gDPFillRectangleEXT gDPFillRectangleWideEXT
#define gSPTextureRectangleEXT(p, xl, yl, xh, yh, tile, s, t, ds, dt) gSPTextureRectangleWideEXT(p, xl, yl, xh, yh, tile, s, t, ds, dt, G_OFF)
#define gSPTextureRectangleFlipEXT(p, xl, yl, xh, yh, tile, s, t, ds, dt) gSPTextureRectangleWideEXT(p, xl, yl, xh, yh, tile, s, t, ds, dt, G_ON)

#define gDPFlushEXT(pkt) gDPNoParam(pkt, G_RDPFLUSH_EXT)

#define gDPClearDepthEXT(pkt) gDPNoParam(pkt, G_CLEAR_DEPTH_EXT)

#undef gDPFillRectangleScaled
#define gDPFillRectangleScaled(pkt, x1, y1, x2, y2) gDPFillRectangleEXT(pkt, (x1) * g_ScaleX, y1, (x2) * g_ScaleX, y2)

#undef gDPHudRectangle
#define gDPHudRectangle(pkt, x1, y1, x2, y2) gDPFillRectangleEXT(pkt, (x1) * g_ScaleX, y1, ((x2 + 1)) * g_ScaleX, (y2) + 1)

#else // PLATFORM_N64

#define gDPFillRectangleEXT gDPFillRectangle
#define gSPTextureRectangleEXT gSPTextureRectangle

#endif // PLATFORM_N64

#endif
