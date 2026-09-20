/**
 * GoldenEye's monitor programmes: what its TVs and its big projection screens show.
 *
 * A GoldenEye monitor runs a little programme - use this image, scroll it, hold,
 * zoom, tint, jump to another programme one time in ten - and Perfect Dark's
 * tvscreen is the same machine: the sixteen commands have the same numbers, the
 * same arguments and the same widths in both games (chrai.h's MON* macros against
 * tvcmds.h). What differs is the data. A record's ImageNum picks a programme out
 * of GoldenEye's own fifty-two (monitorSetImageByNum(), propobj.c) and a
 * programme's images are an index into GoldenEye's own fifty (s_monitorimages,
 * oddtextures.c), and Perfect Dark has tables of its own for both - so a converted
 * level's screens ran Perfect Dark's programmes over Perfect Dark's pictures.
 *
 * The programmes are one block of words in the ROM's data segment, MON_BLOCK_AT
 * for MON_BLOCK_WORDS, which decodes end to end (build/gexrom/monscan.py); a
 * jump's target is an address inside it. PROGRAMS is the word each ImageNum starts
 * at and IMAGES is s_monitorimages: GoldenEye's image id, width, height, level,
 * format, depth and the two wrap modes, which is Perfect Dark's textureconfig
 * field for field.
 *
 * Generated from the GoldenEye decompilation (src/game/propobj.c for the
 * programmes' addresses and the selector, assets/oddtextures.c and
 * assets/image_externs.h for the images); the three programmes the decompilation
 * gives no address for were placed from the block itself and checked against the
 * jumps that name them. The Python copy is tools/geconvert/gemonitortable.py and the two
 * must agree.
 */
#ifndef GEMONITORTABLE_H
#define GEMONITORTABLE_H

#define GEMON_BLOCK_AT 0x80030b74
#define GEMON_BLOCK_WORDS 1334
#define GEMON_NUM_PROGRAMS 52
#define GEMON_NUM_IMAGES 50

// the word of the block each ImageNum starts at
static const uint16_t g_GeMonPrograms[GEMON_NUM_PROGRAMS] = {
	   0,  //  0 monAnim00Bond
	  35,  //  1 monAnim01DesktopsSatellite
	 172,  //  2 monAnim02Astrological
	 244,  //  3 monAnim03ThreeWavePattern
	 297,  //  4 monAnim04WavePattern
	 320,  //  5 monAnim05GreenTextUp
	 351,  //  6 monAnim06RedTextDown
	 390,  //  7 monAnim07GreenTextDown
	 426,  //  8 monAnim08RedBarGraph
	 437,  //  9 monAnim09BlueBarGraph
	 448,  // 10 monAnim0AGreenBarGraph
	 480,  // 11 monAnim0BRadar
	 487,  // 12 monAnim0CSpinningCube
	 583,  // 13 monAnim0DLocWeapArmed
	 609,  // 14 monAnim0ERedTarget
	 213,  // 15 monAnim0FSatelliteTargeting
	 507,  // 16 monAnim10GlobalMap
	 638,  // 17 monAnim11KarlYelling
	 662,  // 18 monAnim12Skateboard
	 821,  // 19 monAnim13PoliceGuy
	 841,  // 20 monAnim14Off
	 849,  // 21 monAnim15RandomSeven
	 874,  // 22 monAnim16RandomFour
	 887,  // 23 monAnim17RandImageEffect
	 920,  // 24 monRandEffectChanceSHUTTLE1
	 927,  // 25 monRandEffectChanceSHUTTLE2
	 934,  // 26 monRandEffectChanceEARTHFULL1
	 941,  // 27 monRandEffectChanceEARTHFULL2
	 948,  // 28 monRandEffectChanceBLUESTARS
	 955,  // 29 monRandEffectChanceGALAXY1
	 962,  // 30 monRandEffectChanceGALAXY2
	 969,  // 31 monRandEffectChanceEARTHTEXT
	 976,  // 32 monRandEffectChanceTARGETEARTH
	 983,  // 33 monRandEffectChanceGALAXY3
	 990,  // 34 monRandChanceScrollOrZoomRandRGBN
	1004,  // 35 monRandChanceScrollOrZoomRed
	1009,  // 36 monRandChanceScrollOrZoomGreen
	1014,  // 37 monRandChanceScrollOrZoomBlue
	1019,  // 38 monRandChanceScrollOrZoom
	1040,  // 39 monAnim27RandomEffectScrollRight
	1047,  // 40 monAnim28RandomEffectScrollUpFast
	1054,  // 41 monAnim29RandomEffectScrollUp
	1061,  // 42 monAnim2ARandEffectScrollZoom1
	1091,  // 43 monAnim2ARandEffectScrollZoom2
	1135,  // 44 monAnim2CRandEffectWaitRoute
	1145,  // 45 monAnim2DRandEffectFlash
	1165,  // 46 monAnim2ERedBrightening
	1184,  // 47 monAnim2FGreenBrightening
	1203,  // 48 monAnim30GreySolid
	1217,  // 49 monAnim31RedSolid
	1231,  // 50 monAnim32GreenSolid
	1245,  // 51 monAnim33BlackSolid
};

// s_monitorimages: image id, width, height, level, format, depth, s, t
static const struct { uint16_t image; uint8_t w, h, level, format, depth, s, t; } g_GeMonImages[GEMON_NUM_IMAGES] = {
	{ 2187,  32,  32, 0, 4, 1, 0, 0 },  // IMAGE_MONITOR_BOND
	{ 2188, 128,  16, 0, 4, 1, 0, 0 },  // IMAGE_MONITOR_LOCATION
	{ 2189, 128,  16, 0, 4, 1, 0, 0 },  // IMAGE_MONITOR_BEGINARMING
	{ 2190, 128,  16, 0, 4, 1, 0, 0 },  // IMAGE_MONITOR_TARGET
	{ 2191, 128,  16, 0, 4, 1, 0, 0 },  // IMAGE_MONITOR_SEVERNAYA
	{ 2192, 128,  16, 0, 4, 1, 0, 0 },  // IMAGE_MONITOR_BREAKTARGET
	{ 2193, 128,  16, 0, 4, 1, 0, 0 },  // IMAGE_MONITOR_AIMER
	{ 2194,  32,  32, 6, 0, 2, 0, 0 },  // IMAGE_MONITOR_EARTH
	{ 2195,  32,  32, 6, 0, 2, 0, 0 },  // IMAGE_MONITOR_DESKTOPBANG
	{ 2196,  32,  32, 6, 0, 2, 0, 0 },  // IMAGE_MONITOR_HEATMAP
	{ 2197,  32,  32, 6, 0, 2, 0, 0 },  // IMAGE_MONITOR_3DMATH
	{ 1185,  32,  32, 6, 0, 2, 0, 0 },  // IMAGE_MONITOR_DESKTOPBARS
	{ 2198,  32,  32, 6, 0, 2, 0, 0 },  // IMAGE_MONITOR_2DMATH
	{ 2199,  32,  32, 6, 0, 2, 0, 0 },  // IMAGE_MONITOR_SATELLITE
	{ 1186,  32,  32, 6, 0, 2, 0, 0 },  // IMAGE_MONITOR_DESKTOP
	{ 1187,  32,  32, 6, 0, 2, 0, 0 },  // IMAGE_MONITOR_DESKTOPSTAGGERED
	{ 2200,  16,  16, 5, 4, 1, 0, 0 },  // IMAGE_MONITOR_CUBE1
	{  582,  32,  32, 6, 0, 2, 0, 0 },  // IMAGE_MONITOR_SHUTTLE1
	{  583,  32,  32, 6, 0, 2, 0, 0 },  // IMAGE_MONITOR_SHUTTLE2
	{  584,  32,  32, 6, 0, 2, 0, 0 },  // IMAGE_MONITOR_EARTHFULL1
	{ 2201,  32,  32, 6, 0, 2, 0, 0 },  // IMAGE_MONITOR_EARTHFULL2
	{ 2202,  32,  32, 6, 0, 2, 0, 0 },  // IMAGE_MONITOR_BLUESTARS
	{ 2203,  32,  32, 6, 0, 2, 0, 0 },  // IMAGE_MONITOR_GALAXY1
	{ 2204,  32,  32, 6, 0, 2, 0, 0 },  // IMAGE_MONITOR_GALAXY2
	{  581,  32,  32, 6, 0, 2, 0, 0 },  // IMAGE_MONITOR_EARTHTEXT
	{ 2205,  32,  32, 6, 0, 2, 0, 0 },  // IMAGE_MONITOR_TARGETEARTH
	{ 2206,  32,  32, 6, 0, 2, 0, 0 },  // IMAGE_MONITOR_GALAXY3
	{ 2227,  64,  64, 0, 4, 1, 0, 0 },  // IMAGE_MONITOR_STATIC
	{ 2223,  32,  32, 0, 4, 1, 0, 0 },  // IMAGE_MONITOR_SINE
	{ 2224,  32,  32, 0, 4, 1, 0, 0 },  // IMAGE_MONITOR_TEXT
	{ 2225,  32,  32, 0, 4, 1, 0, 0 },  // IMAGE_MONITOR_BARS
	{ 2226,  32,  32, 0, 4, 1, 0, 0 },  // IMAGE_MONITOR_SQUARES
	{ 2219,  32,  32, 0, 4, 1, 0, 0 },  // IMAGE_MONITOR_FIST1
	{ 2220,  32,  32, 0, 4, 1, 0, 0 },  // IMAGE_MONITOR_FIST2
	{ 2221,  32,  32, 0, 4, 1, 0, 0 },  // IMAGE_MONITOR_FIST3
	{ 2222,  32,  32, 0, 4, 1, 0, 0 },  // IMAGE_MONITOR_FIST4
	{ 2218,  32,  32, 0, 4, 1, 0, 0 },  // IMAGE_MONITOR_SKATEBOARD4
	{ 2207,  32,  32, 0, 4, 1, 0, 0 },  // IMAGE_MONITOR_SKATEBOARD1
	{ 2208,  32,  32, 0, 4, 1, 0, 0 },  // IMAGE_MONITOR_SKATEBOARD2
	{ 2209,  32,  32, 0, 4, 1, 0, 0 },  // IMAGE_MONITOR_SKATEBOARD3
	{ 2210,  32,  32, 6, 4, 1, 0, 0 },  // IMAGE_MONITOR_TALK1
	{ 2211,  32,  32, 6, 4, 1, 0, 0 },  // IMAGE_MONITOR_TALK2
	{ 2212,  32,  32, 6, 4, 1, 0, 0 },  // IMAGE_MONITOR_TALK3
	{ 2213,  32,  32, 6, 4, 1, 0, 0 },  // IMAGE_MONITOR_TALK4
	{ 2214, 128,  48, 0, 4, 1, 0, 2 },  // IMAGE_MONITOR_WORLDMAP
	{ 2215,  16,  16, 5, 4, 1, 0, 0 },  // IMAGE_MONITOR_CUBE2
	{ 2216,  16,  16, 5, 4, 1, 0, 0 },  // IMAGE_MONITOR_CUBE3
	{ 2217,  16,  16, 5, 4, 1, 0, 0 },  // IMAGE_MONITOR_CUBE4
	{ 2263,  54,  54, 0, 3, 1, 2, 2 },  // IMAGE_MONITOR_TRIANGLE
	{  837,  32,  32, 6, 0, 2, 0, 0 },  // IMAGE_MONITOR_KEYBOARDKEY
};

#endif
