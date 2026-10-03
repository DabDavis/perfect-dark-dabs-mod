/**
 * GoldenEye's own mip levels, for a texture converted from its ROM.
 *
 * A texture whose header asks for levels of detail its data does not carry has
 * them made at load, each half the size of the one before, by averaging every
 * 2x2 block (texInflateZlib(), texInflateNonZlib()). Perfect Dark's engine
 * changed how: its paletted shrink averages 8-bit channels and finds the
 * nearest palette entry by trying every one, where GoldenEye averages the
 * 5-bit channels, rounds alpha to one bit (opaque when two of the four are)
 * and finds the entry with a binary search over the palette's brightness that
 * assumes the palette is sorted by it, then looks four entries either side.
 * On a palette that is not sorted it lands somewhere else entirely, and the
 * cartridge draws what it lands on: Crab Key's wall grille (an 8x8 CI8 cell,
 * a white frame round transparent black) has white levels from 4x4 down on
 * the cartridge and dark grey ones from Perfect Dark's search - a white mesh
 * there, a black one here, the cartridge's own RDRAM bytes compared. The
 * non-paletted shrink rounds differently too (and Perfect Dark's IA16 alpha
 * is the sum unshifted).
 *
 * These are GoldenEye's functions as the decomp has them (src/game/image.c),
 * the IA16 CI4 case's misplaced brackets included: the cartridge runs them.
 * texLoad() picks them for a texture read out of a ROM conversion's folder
 * (modloaderDirIndexIsConversion()).
 */
#include <ultra64.h>
#include "constants.h"
#include "types.h"
#include "getexshrink.h"

#define TEX_ALPHA_WEIGHT 961

// The texels are big-endian bytes, as on the cartridge, and GoldenEye read a
// 16 or 32-bit texel as one word: so do these, whatever the host's order
#define GE_LD16(p) ((u16)((((const u8 *)(p))[0] << 8) | ((const u8 *)(p))[1]))
#define GE_LD32(p) ((u32)(((const u8 *)(p))[0] << 24 | ((const u8 *)(p))[1] << 16 | ((const u8 *)(p))[2] << 8 | ((const u8 *)(p))[3]))
#define GE_ST16(p, v) do { u16 v_ = (v); ((u8 *)(p))[0] = v_ >> 8; ((u8 *)(p))[1] = v_; } while (0)
#define GE_ST32(p, v) do { u32 v_ = (v); ((u8 *)(p))[0] = v_ >> 24; ((u8 *)(p))[1] = v_ >> 16; ((u8 *)(p))[2] = v_ >> 8; ((u8 *)(p))[3] = v_; } while (0)

static s32 geTexFindClosestColourIndexRGBA(u16 *palette, s32 numcolours, s32 r, s32 g, s32 b, s32 a)
{
    s32 low;
    s32 high;
    s32 i;
    u16 targetcolour;
    s32 targetmagnitude;
 
    // Cursor into the palette: the midpoint in stage 2, the scan position in stage 3.
    s32 paletteidx;
 
    u16 colour;
    s32 red;
    s32 green;
    s32 blue;
    s32 alpha;
    s32 magnitude;
 
    s32 diffr;
    s32 diffg;
    s32 diffb;
    s32 alphapenalty;
    s32 distance;
 
    s32 bestindex;
    s32 bestvalue;
 
    // Stage 1: scan the whole palette for a matching colour and return its index if one is found.
    targetcolour = ((r << 11) | (g << 6) | (b << 1) | a);
 
    for (i = 0; i < numcolours; i++)
    {
        if (targetcolour == palette[i])
        {
            return i;
        }
    }
 
    /** 
     * Stage 2: Find a promising palette neighborhood.
     */
    low = 0;
    high = numcolours - 1;
    // TEX_ALPHA_WEIGHT = 31^2, the maximum possible squared difference in one five-bit color channel.
    targetmagnitude = (r * r) + (g * g) + (b * b) + (a * TEX_ALPHA_WEIGHT);
 
    while (high - low >= 2)
    {
        paletteidx = (high + low) >> 1;
 
        colour = palette[paletteidx];
        red = (colour >> 11) & 0x1F;
        green = (colour >> 6) & 0x1F;
        blue = (colour >> 1) & 0x1F;
        alpha = colour & 1;
 
        magnitude = (red * red) + (green * green) + (blue * blue) + (alpha * TEX_ALPHA_WEIGHT);
 
        if (magnitude < targetmagnitude)
        {
            low = paletteidx;
            continue;
        }
 
        if (targetmagnitude < magnitude)
        {
            high = paletteidx;
        }
        else
        {
            high = paletteidx;
            low = paletteidx;
        }
    }
 
    // Stage 3: Search the nearby palette entries accurately. 
    low = high - 4;
 
    if (low < 0)
    {
        low = 0;
    }
 
    high += 4;
 
    if (high >= numcolours)
    {
        high = numcolours - 1;
    }
 
    bestindex = 0;
    bestvalue = 999999;
 
    for (paletteidx = low; paletteidx <= high; paletteidx++)
    {
        colour = palette[paletteidx];
        diffr = ((colour >> 11) & 0x1F) - r;
        diffg = ((colour >> 6) & 0x1F) - g;
        diffb = ((colour >> 1) & 0x1F) - b;
 
        // An alpha mismatch costs 961, strongly discouraging a palette entry with the wrong transparency.
        alphapenalty = (a == (colour & 1)) ? 0 : TEX_ALPHA_WEIGHT;
 
        distance = alphapenalty;
        distance += diffr * diffr;
        distance += diffg * diffg;
        distance += diffb * diffb;
 
        if (distance < bestvalue)
        {
            bestindex = paletteidx;
            bestvalue = distance;
        }
    }
 
    return bestindex;
}

static s32 geTexFindClosestColourIndexIA(u16 *palette, s32 numcolours, s32 intensity, s32 alpha)
{
    s32 scanstart;
    s32 high;
    s32 i;
    s32 scanidx;
    s32 bestindex;
    s32 bestvalue;
    s32 low;
    s32 targetcolour;
    s32 targetmagnitude;
    s32 colour;
    s32 entryintensity;
    s32 entryalpha;
    s32 magnitude;
    s32 diffi;
    s32 diffa;
    s32 distance;
 
    // Stage 1: scan the whole palette for a matching intensity and return its index if one is found.
    targetcolour = (intensity << 8) | alpha;
 
    for (i = 0; i < numcolours; i++)
    {
        if ((u16)targetcolour == palette[i])
        {
            return i;
        }
    }
 
    // Stage 2: binary search by squared magnitude.
    low = 0;
    high = numcolours - 1;
    targetmagnitude = (intensity * intensity) + (alpha * alpha);
 
    while (high - low >= 2)
    {
        s32 mid;
 
        mid = (high + low) >> 1;
        colour = palette[mid];
 
        entryintensity = (colour >> 8) & 0xFF;
        entryalpha = colour & 0xFF;
        magnitude = (entryintensity * entryintensity) + (entryalpha * entryalpha);
 
        if (magnitude < targetmagnitude)
        {
            low = mid;
            continue;
        }
 
        if (targetmagnitude < magnitude)
        {
            high = mid;
        }
        else
        {
            // Equal magnitude. Collapse the window to end the search here.
            low = mid;
            high = mid;
        }
    }
 
    // Stage 3: widen the window by four entries either side and clamp it.
    scanstart = high - 4;
    scanidx = scanstart;
    high += 4;

    if (scanidx < 0)
    {
        scanstart = 0;
    }
 
    if (high >= numcolours)
    {
        high = numcolours - 1;
    }

    /* 999999 is an unreachable sentinel: the worst possible distance is
     * 2 * 255 * 255 = 130050. */
    bestindex = 0;
    bestvalue = 999999;

    scanidx = scanstart;
 
    if (scanidx <= high)
    {
        s32 scanend;

        for (;;)
        {
            colour = palette[scanidx];
 
            diffi = ((colour >> 8) & 0xff) - intensity;
            diffa = (colour & 0xff) - alpha;
            distance = (diffi * diffi) + (diffa * diffa);
            scanend = high + 1;
 
            if (distance < bestvalue)
            {
                bestindex = scanidx;
                bestvalue = distance;
            }

            scanidx++;

            if (scanend == scanidx)
            {
                break;
            }
        }
    }
 
    return bestindex;
}

s32 geTexShrinkPaletted(u8 *src, u8 *dst, s32 srcwidth, s32 srcheight, s32 format, u16 *palette, s32 numcolours)
{
    s32 j;
    s32 i;
    s32 alignedsrcwidth;
    s32 aligneddstwidth;
    s32 dstheight;
    s16 colour1;
    s16 colour2;
    s16 colour3;
    s16 colour4;
    s32 r;
    s32 g;
    s32 b;
    s32 a;
    s32 nextrow;
    u8 *dst8;
    s32 nextcol;
    s32 c;
    u8 *src8;

    dst8 = dst;
    src8 = src;
    dstheight = (srcheight + 1) >> 1;

    switch (format)
    {
        case TEXFORMAT_RGBA16_CI8:
        case TEXFORMAT_IA16_CI8:
            aligneddstwidth = (((srcwidth + 1) >> 1) + 7) & 0xff8;
            alignedsrcwidth = (srcwidth + 7) & 0xff8;
            break;

        case TEXFORMAT_RGBA16_CI4:
        case TEXFORMAT_IA16_CI4:
            aligneddstwidth = (((srcwidth + 1) >> 1) + 15) & 0xff0;
            alignedsrcwidth = (srcwidth + 15) & 0xff0;
            break;
    }


    switch (format)
    {
        case TEXFORMAT_RGBA16_CI8:
            for (i = 0; i < srcheight; i += 2)
            {
                nextrow = i + 1 < srcheight ? alignedsrcwidth : 0;

                for (j = 0; j < alignedsrcwidth; j += 2)
                {
                    nextcol = j + 1 < srcwidth ? j + 1 : j;

                    colour1 = palette[src8[j]];
                    colour2 = palette[src8[nextcol]];
                    colour3 = palette[src8[nextrow + j]];
                    colour4 = palette[src8[nextrow + nextcol]];

                    r = ((((colour1 >> 0xB) & 0x1F) + ((colour2 >> 0xB) & 0x1F) + ((colour3 >> 0xB) & 0x1F) + ((colour4 >> 0xB) & 0x1F)) >> 2) & 0x1F;
                    g = ((((colour1 >> 6) & 0x1F) + ((colour2 >> 6) & 0x1F) + ((colour3 >> 6) & 0x1F) + ((colour4 >> 6) & 0x1F)) >> 2) & 0x1F;
                    b = ((((colour1 >> 1) & 0x1F) + ((colour2 >> 1) & 0x1F) + ((colour3 >> 1) & 0x1F) + ((colour4 >> 1) & 0x1F)) >> 2) & 0x1F;
                    a = (((colour1 & 1) + (colour2 & 1) + (colour3 & 1) + (colour4 & 1) + 2) >> 2) & 1;

                    dst8[j >> 1] = geTexFindClosestColourIndexRGBA(palette, numcolours, r, g, b, a);
                }

                dst8 += aligneddstwidth;
                src8 += alignedsrcwidth * 2;
            }

            return dstheight * aligneddstwidth;

        case TEXFORMAT_IA16_CI8:
            for (i = 0; i < srcheight; i += 2)
            {
                nextrow = i + 1 < srcheight ? alignedsrcwidth : 0;

                for (j = 0; j < alignedsrcwidth; j += 2)
                {
                    nextcol = j + 1 < srcwidth ? j + 1 : j;

                    colour1 = palette[src8[j]];
                    colour2 = palette[src8[nextcol]];
                    colour3 = palette[src8[nextrow + j]];
                    colour4 = palette[src8[nextrow + nextcol]];

                    c = ((((colour1 >> 8) & 0xff) + ((colour2 >> 8) & 0xff) + ((colour3 >> 8) & 0xff) + ((colour4 >> 8) & 0xff)) >> 2) & 0xff;
                    a = ((((colour1 >> 0) & 0xff) + ((colour2 >> 0) & 0xff) + ((colour3 >> 0) & 0xff) + ((colour4 >> 0) & 0xff) + 1) >> 2) & 0xff;

                    dst8[j >> 1] = geTexFindClosestColourIndexIA(palette, numcolours, c, a);
                }

                dst8 += aligneddstwidth;
                src8 += alignedsrcwidth * 2;
            }

            return dstheight * aligneddstwidth;

        case TEXFORMAT_RGBA16_CI4:
            for (i = 0; i < srcheight; i += 2)
            {
                nextrow = i + 1 < srcheight ? alignedsrcwidth >> 1 : 0;

                for (j = 0; j < alignedsrcwidth; j += 4)
                {
                    colour1 = palette[(src8[j >> 1] >> 4) & 0xf];
                    colour2 = palette[src8[j >> 1] >> ((j + 1 < srcwidth ? 0 : 4)) & 0xf];
                    colour3 = palette[(src8[nextrow + (j >> 1)] >> 4) & 0xf];
                    colour4 = palette[src8[nextrow + (j >> 1)] >> ((j + 1 < srcwidth ? 0 : 4)) & 0xf];

                    r = ((((colour1 >> 0xB) & 0x1F) + ((colour2 >> 0xB) & 0x1F) + ((colour3 >> 0xB) & 0x1F) + ((colour4 >> 0xB) & 0x1F)) >> 2) & 0x1F;
                    g = ((((colour1 >> 6) & 0x1F) + ((colour2 >> 6) & 0x1F) + ((colour3 >> 6) & 0x1F) + ((colour4 >> 6) & 0x1F)) >> 2) & 0x1F;
                    b = ((((colour1 >> 1) & 0x1F) + ((colour2 >> 1) & 0x1F) + ((colour3 >> 1) & 0x1F) + ((colour4 >> 1) & 0x1F)) >> 2) & 0x1F;
                    a = ((((colour1 & 1) + (colour2 & 1) + (colour3 & 1) + (colour4 & 1) + 2) >> 2) & 1);

                    dst8[j >> 2] = (geTexFindClosestColourIndexRGBA(palette, numcolours, r, g, b, a) * 0x10) & 0xFFFF;

                    colour1 = palette[(src8[(j + 2) >> 1] >> 4) & 0xf];
                    colour2 = palette[(src8[(j + 2) >> 1] >> (j + 3 < srcwidth ? 0 : 4)) & 0xf];
                    colour3 = palette[(src8[nextrow + ((j + 2) >> 1)] >> 4) & 0xf];
                    colour4 = palette[(src8[nextrow + ((j + 2) >> 1)] >> (j + 3 < srcwidth ? 0 : 4)) & 0xf];

                    r = ((((colour1 >> 0xB) & 0x1F) + ((colour2 >> 0xB) & 0x1F) + ((colour3 >> 0xB) & 0x1F) + ((colour4 >> 0xB) & 0x1F)) >> 2) & 0x1F;
                    g = ((((colour1 >> 6) & 0x1F) + ((colour2 >> 6) & 0x1F) + ((colour3 >> 6) & 0x1F) + ((colour4 >> 6) & 0x1F)) >> 2) & 0x1F;
                    b = ((((colour1 >> 1) & 0x1F) + ((colour2 >> 1) & 0x1F) + ((colour3 >> 1) & 0x1F) + ((colour4 >> 1) & 0x1F)) >> 2) & 0x1F;
                    a = ((((colour1 & 1) + (colour2 & 1) + (colour3 & 1) + (colour4 & 1) + 2) >> 2) & 1);

                    dst8[j >> 2] |= geTexFindClosestColourIndexRGBA(palette, numcolours, r, g, b, a) & 0xff;
                }

                dst8 += aligneddstwidth >> 1;
                src8 += alignedsrcwidth;
            }

            return (aligneddstwidth >> 1) * dstheight;

        case TEXFORMAT_IA16_CI4:
            for (i = 0; i < srcheight; i += 2)
            {
                nextrow = i + 1 < srcheight ? alignedsrcwidth >> 1 : 0;

                for (j = 0; j < alignedsrcwidth; j += 4)
                {
                    // @bug: The brackets are wrong in colour2 and colour4 which
                    // causes the index shift to be part of the ternary condition.
                    // It's done correctly in TEXFORMAT_RGBA16_CI4 (above).
                    // This buggy calculation is repeated further below.
                    colour1 = palette[(src8[j >> 1] >> 4) & 0xf];
                    colour2 = palette[(src8[j >> 1] >> (j + 1 < srcwidth) ? 0 : 4) & 0xf];
                    colour3 = palette[(src8[nextrow + (j >> 1)] >> 4) & 0xf];
                    colour4 = palette[(src8[nextrow + (j >> 1)] >> (j + 1 < srcwidth) ? 0 : 4) & 0xf];

                    c = ((((colour1 >> 8) & 0xff) + ((colour2 >> 8) & 0xff) + ((colour3 >> 8) & 0xff) + ((colour4 >> 8) & 0xff)) >> 2) & 0xff;
                    a = ((((colour1 >> 0) & 0xff) + ((colour2 >> 0) & 0xff) + ((colour3 >> 0) & 0xff) + ((colour4 >> 0) & 0xff) + 1) >> 2) & 0xff;

                    dst8[j >> 2] = (geTexFindClosestColourIndexIA(palette, numcolours, c, a) * 0x10) & 0xFFFF;

                    colour1 = palette[(src8[(j + 2) >> 1] >> 4) & 0xf];
                    colour2 = palette[(src8[(j + 2) >> 1] >> (j + 3 < srcwidth) ? 0 : 4) & 0xf];
                    colour3 = palette[(src8[nextrow + ((j + 2) >> 1)] >> 4) & 0xf];
                    colour4 = palette[(src8[nextrow + ((j + 2) >> 1)] >> (j + 3 < srcwidth) ? 0 : 4) & 0xf];

                    c = ((((colour1 >> 8) & 0xff) + ((colour2 >> 8) & 0xff) + ((colour3 >> 8) & 0xff) + ((colour4 >> 8) & 0xff)) >> 2) & 0xff;
                    a = ((((colour1 >> 0) & 0xff) + ((colour2 >> 0) & 0xff) + ((colour3 >> 0) & 0xff) + ((colour4 >> 0) & 0xff) + 1) >> 2) & 0xff;

                    dst8[j >> 2] |= geTexFindClosestColourIndexIA(palette, numcolours, c, a) & 0xff;
                }

                dst8 += aligneddstwidth >> 1;
                src8 += alignedsrcwidth;
            }

            return (aligneddstwidth >> 1) * dstheight;
    }

    return 0;
}

s32 geTexShrinkNonPaletted(u8 *src, u8 *dst, s32 srcwidth, s32 srcheight, s32 format)
{
    s32 i;
    s32 j;
    s32 alignedsrcwidth;
    s32 aligneddstwidth;
    u32 *dst32 = (u32 *) dst;
    u16 *dst16 = (u16 *) dst;
    u8 *dst8 = dst;
    u32 *src32 = (u32 *) src;
    u16 *src16 = (u16 *) src;
    u8 *src8 = src;
    s32 dstheight = (srcheight + 1) >> 1;
    s32 r;
    s32 g;
    s32 b;
    s32 a;
    s32 c;
    u32 tl32;
    u32 tr32;
    u32 bl32;
    u32 br32;
    u16 tl16;
    u16 tr16;
    u16 bl16;
    u16 br16;
    u8 tl8;
    u8 tr8;
    u8 bl8;
    u8 br8;
    s32 nextrow;
    s32 nextcol;

    switch (format)
    {
        case TEXFORMAT_RGBA32:
        case TEXFORMAT_RGB24:
            aligneddstwidth = (((srcwidth + 1) >> 1) + 3) & 0xffc;
            alignedsrcwidth = (srcwidth + 3) & 0xffc;
            break;
        case TEXFORMAT_RGBA16:
        case TEXFORMAT_RGB15:
        case TEXFORMAT_IA16:
            aligneddstwidth = (((srcwidth + 1) >> 1) + 3) & 0xffc;
            alignedsrcwidth = (srcwidth + 3) & 0xffc;
            break;
        case TEXFORMAT_IA8:
        case TEXFORMAT_I8:
            aligneddstwidth = (((srcwidth + 1) >> 1) + 7) & 0xff8;
            alignedsrcwidth = (srcwidth + 7) & 0xff8;
            break;
        case TEXFORMAT_IA4:
        case TEXFORMAT_I4:
            aligneddstwidth = (((srcwidth + 1) >> 1) + 15) & 0xff0;
            alignedsrcwidth = (srcwidth + 15) & 0xff0;
            break;
    }

    switch (format)
    {
        case TEXFORMAT_RGBA32:
        case TEXFORMAT_RGB24:
            for (i = 0; i < srcheight; i += 2)
            {
                nextrow = i + 1 < srcheight ? alignedsrcwidth : 0;

                for (j = 0; j < alignedsrcwidth; j += 2)
                {
                    nextcol = j + 1 < srcwidth ? j + 1 : j;

                    tl32 = GE_LD32(&src32[j]);
                    tr32 = GE_LD32(&src32[nextcol]);
                    bl32 = GE_LD32(&src32[nextrow + j]);
                    br32 = GE_LD32(&src32[nextrow + nextcol]);

                    r = ((((tl32 >> 24) & 0xff) + ((tr32 >> 24) & 0xff) + ((bl32 >> 24) & 0xff) + ((br32 >> 24) & 0xff)) >> 2) & 0xff;
                    g = ((((tl32 >> 16) & 0xff) + ((tr32 >> 16) & 0xff) + ((bl32 >> 16) & 0xff) + ((br32 >> 16) & 0xff)) >> 2) & 0xff;
                    b = ((((tl32 >>  8) & 0xff) + ((tr32 >>  8) & 0xff) + ((bl32 >>  8) & 0xff) + ((br32 >>  8) & 0xff)) >> 2) & 0xff;
                    a = ((((tl32 >>  0) & 0xff) + ((tr32 >>  0) & 0xff) + ((bl32 >>  0) & 0xff) + ((br32 >>  0) & 0xff) + 1) >> 2) & 0xff;

                    GE_ST32(&dst32[j >> 1], r << 24 | g << 16 | b << 8 | a);
                }

                dst32 += aligneddstwidth;
                src32 += alignedsrcwidth * 2;
            }

            return dstheight * aligneddstwidth * 4;

        case TEXFORMAT_RGBA16:
        case TEXFORMAT_RGB15:
            for (i = 0; i < srcheight; i += 2)
            {
                nextrow = i + 1 < srcheight ? alignedsrcwidth : 0;

                for (j = 0; j < alignedsrcwidth; j += 2)
                {
                    nextcol = j + 1 < srcwidth ? j + 1 : j;

                    tl16 = GE_LD16(&src16[j]);
                    tr16 = GE_LD16(&src16[nextcol]);
                    bl16 = GE_LD16(&src16[nextrow + j]);
                    br16 = GE_LD16(&src16[nextrow + nextcol]);

                    r = ((((tl16 >> 11) & 0x1f) + ((tr16 >> 11) & 0x1f) + ((bl16 >> 11) & 0x1f) + ((br16 >> 11) & 0x1f)) >> 2) & 0x1f;
                    g = ((((tl16 >>  6) & 0x1f) + ((tr16 >>  6) & 0x1f) + ((bl16 >>  6) & 0x1f) + ((br16 >>  6) & 0x1f)) >> 2) & 0x1f;
                    b = ((((tl16 >>  1) & 0x1f) + ((tr16 >>  1) & 0x1f) + ((bl16 >>  1) & 0x1f) + ((br16 >>  1) & 0x1f)) >> 2) & 0x1f;
                    a = ((((tl16 >>  0) & 0x01) + ((tr16 >>  0) & 0x01) + ((bl16 >>  0) & 0x01) + ((br16 >>  0) & 0x01) + 2) >> 2) & 0x01;

                    GE_ST16(&dst16[j >> 1], r << 11 | g << 6 | b << 1 | a);
                }

                dst16 += aligneddstwidth;
                src16 += alignedsrcwidth * 2;
            }

            return dstheight * aligneddstwidth * 2;

        case TEXFORMAT_IA16:
            for (i = 0; i < srcheight; i += 2)
            {
                nextrow = i + 1 < srcheight ? alignedsrcwidth : 0;

                for (j = 0; j < alignedsrcwidth; j += 2)
                {
                    nextcol = j + 1 < srcwidth ? j + 1 : j;

                    tl16 = GE_LD16(&src16[j]);
                    tr16 = GE_LD16(&src16[nextcol]);
                    bl16 = GE_LD16(&src16[nextrow + j]);
                    br16 = GE_LD16(&src16[nextrow + nextcol]);

                    c = (((tl16 >> 8) & 0xff) + ((tr16 >> 8) & 0xff) + ((bl16 >> 8) & 0xff) + ((br16 >> 8) & 0xff)) >> 2;
                    a = ((tl16 & 0xff) + (tr16 & 0xff) + (bl16 & 0xff) + (br16 & 0xff) + 1) >> 2;

                    GE_ST16(&dst16[j >> 1], ((u8)c << 8) | (a & 0xFF));
                }

                dst16 += aligneddstwidth;
                src16 += alignedsrcwidth * 2;
            }

            return dstheight * aligneddstwidth * 2;

        case TEXFORMAT_IA8:
            for (i = 0; i < srcheight; i += 2)
            {
                nextrow = i + 1 < srcheight ? alignedsrcwidth : 0;

                for (j = 0; j < alignedsrcwidth; j += 2)
                {
                    nextcol = j + 1 < srcwidth ? j + 1 : j;

                    tl8 = src8[j];
                    tr8 = src8[nextcol];
                    bl8 = src8[nextrow + j];
                    br8 = src8[nextrow + nextcol];

                    c = ((((tl8 >> 4) & 0xf) + ((tr8 >> 4) & 0xf) + ((bl8 >> 4) & 0xf) + ((br8 >> 4) & 0xf)) << 2) & 0xF0;
                    a = (((tl8 & 0xf) + (tr8 & 0xf) + (bl8 & 0xf) + (br8 & 0xf) + 1) >> 2) & 0xF;

                    dst8[j >> 1] = c | a;
                }

                dst8 += aligneddstwidth;
                src8 += alignedsrcwidth * 2;
            }

            return dstheight * aligneddstwidth;

        case TEXFORMAT_I8:
            for (i = 0; i < srcheight; i += 2)
            {
                nextrow = i + 1 < srcheight ? alignedsrcwidth : 0;

                for (j = 0; j < alignedsrcwidth; j += 2)
                {
                    nextcol = j + 1 < srcwidth ? j + 1 : j;

                    tl8 = src8[j];
                    tr8 = src8[nextcol];
                    bl8 = src8[nextrow + j];
                    br8 = src8[nextrow + nextcol];

                    c = (u16)((tl8 + tr8 + bl8 + br8 + 1) >> 2);

                    dst8[j >> 1] = c;
                }

                dst8 += aligneddstwidth;
                src8 += alignedsrcwidth * 2;
            }

            return dstheight * aligneddstwidth;

        case TEXFORMAT_IA4:
            for (i = 0; i < srcheight; i += 2)
            {
                nextcol = i + 1;

                for (j = 0; j < alignedsrcwidth; j += 4)
                {
                    tl8 = src8[j >> 1];
                    tr8 = src8[(nextcol < srcheight ? (alignedsrcwidth >> 1) : 0) + (j >> 1)];
                    bl8 = src8[(j >> 1) + 1];
                    br8 = src8[(nextcol < srcheight ? (alignedsrcwidth >> 1) : 0) + (j >> 1) + 1];

                    c = (((((tl8 >> 5) & 7) + ((tl8 >> 1) & 7) + ((tr8 >> 5) & 7) + ((tr8 >> 1) & 7)) << 3) & 0xe0)
                        | (((((bl8 >> 5) & 7) + ((bl8 >> 1) & 7) + ((br8 >> 5) & 7) + ((br8 >> 1) & 7)) >> 1) & 0xe);

                    a = (((((tl8 >> 4) & 1) + (tl8 & 1) + ((tr8 >> 4) & 1) + (tr8 & 1) + 1) << 2) & 0x10)
                        | (((((bl8 >> 4) & 1) + (bl8 & 1) + ((br8 >> 4) & 1) + (br8 & 1) + 1) >> 2) & 1);

                    dst8[j >> 2] = c | a;
                }

                dst8 += aligneddstwidth >> 1;
                src8 += alignedsrcwidth;
            }

            return (aligneddstwidth >> 1) * dstheight;

        case TEXFORMAT_I4:
            for (i = 0; i < srcheight; i += 2)
            {
                for (j = 0; j < alignedsrcwidth; j += 4)
                {
                    tl8 = src8[j >> 1];
                    tr8 = src8[(i + 1 < srcheight ? (alignedsrcwidth >> 1) : 0) + (j >> 1)];
                    bl8 = src8[(j >> 1) + 1];
                    br8 = src8[(i + 1 < srcheight ? (alignedsrcwidth >> 1) : 0) + (j >> 1) + 1];

                    c = ((((tl8 >> 4) & 0xf) + (tl8 & 0xf) + ((tr8 >> 4) & 0xf) + (tr8 & 0xf)) << 2) & 0xf0;
                    a = ((((bl8 >> 4) & 0xf) + (bl8 & 0xf) + ((br8 >> 4) & 0xf) + (br8 & 0xf)) >> 2) & 0xf;

                    dst8[j >> 2] = c | a;
                }

                dst8 += aligneddstwidth >> 1;
                src8 += alignedsrcwidth;
            }

            return (aligneddstwidth >> 1) * dstheight;
    }

    return 0;
}
