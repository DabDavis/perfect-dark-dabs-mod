"""GoldenEye's monitor programmes: what its TVs and its big projection screens show.

A GoldenEye monitor runs a little programme - use this image, scroll it, hold,
zoom, tint, jump to another programme one time in ten - and Perfect Dark's
tvscreen is the same machine: the sixteen commands have the same numbers, the
same arguments and the same widths in both games (chrai.h's MON* macros against
tvcmds.h). What differs is the data. A record's ImageNum picks a programme out
of GoldenEye's own fifty-two (monitorSetImageByNum(), propobj.c) and a
programme's images are an index into GoldenEye's own fifty (s_monitorimages,
oddtextures.c), and Perfect Dark has tables of its own for both - so a converted
level's screens ran Perfect Dark's programmes over Perfect Dark's pictures.

The programmes are one block of words in the ROM's data segment, MON_BLOCK_AT
for MON_BLOCK_WORDS, which decodes end to end (build/gexrom/monscan.py); a
jump's target is an address inside it. PROGRAMS is the word each ImageNum starts
at and IMAGES is s_monitorimages: GoldenEye's image id, width, height, level,
format, depth and the two wrap modes, which is Perfect Dark's textureconfig
field for field.

Generated from the GoldenEye decompilation (src/game/propobj.c for the
programmes' addresses and the selector, assets/oddtextures.c and
assets/image_externs.h for the images); the three programmes the decompilation
gives no address for were placed from the block itself and checked against the
jumps that name them. The C copy is port/include/gemonitortable.h and the two
must agree.
"""

MON_BLOCK_AT = 0x80030b74
MON_BLOCK_WORDS = 1334

# (word offset into the block, GoldenEye's name), by ImageNum
PROGRAMS = (
    (   0, 'monAnim00Bond'),  # 0
    (  35, 'monAnim01DesktopsSatellite'),  # 1
    ( 172, 'monAnim02Astrological'),  # 2
    ( 244, 'monAnim03ThreeWavePattern'),  # 3
    ( 297, 'monAnim04WavePattern'),  # 4
    ( 320, 'monAnim05GreenTextUp'),  # 5
    ( 351, 'monAnim06RedTextDown'),  # 6
    ( 390, 'monAnim07GreenTextDown'),  # 7
    ( 426, 'monAnim08RedBarGraph'),  # 8
    ( 437, 'monAnim09BlueBarGraph'),  # 9
    ( 448, 'monAnim0AGreenBarGraph'),  # 10
    ( 480, 'monAnim0BRadar'),  # 11
    ( 487, 'monAnim0CSpinningCube'),  # 12
    ( 583, 'monAnim0DLocWeapArmed'),  # 13
    ( 609, 'monAnim0ERedTarget'),  # 14
    ( 213, 'monAnim0FSatelliteTargeting'),  # 15
    ( 507, 'monAnim10GlobalMap'),  # 16
    ( 638, 'monAnim11KarlYelling'),  # 17
    ( 662, 'monAnim12Skateboard'),  # 18
    ( 821, 'monAnim13PoliceGuy'),  # 19
    ( 841, 'monAnim14Off'),  # 20
    ( 849, 'monAnim15RandomSeven'),  # 21
    ( 874, 'monAnim16RandomFour'),  # 22
    ( 887, 'monAnim17RandImageEffect'),  # 23
    ( 920, 'monRandEffectChanceSHUTTLE1'),  # 24
    ( 927, 'monRandEffectChanceSHUTTLE2'),  # 25
    ( 934, 'monRandEffectChanceEARTHFULL1'),  # 26
    ( 941, 'monRandEffectChanceEARTHFULL2'),  # 27
    ( 948, 'monRandEffectChanceBLUESTARS'),  # 28
    ( 955, 'monRandEffectChanceGALAXY1'),  # 29
    ( 962, 'monRandEffectChanceGALAXY2'),  # 30
    ( 969, 'monRandEffectChanceEARTHTEXT'),  # 31
    ( 976, 'monRandEffectChanceTARGETEARTH'),  # 32
    ( 983, 'monRandEffectChanceGALAXY3'),  # 33
    ( 990, 'monRandChanceScrollOrZoomRandRGBN'),  # 34
    (1004, 'monRandChanceScrollOrZoomRed'),  # 35
    (1009, 'monRandChanceScrollOrZoomGreen'),  # 36
    (1014, 'monRandChanceScrollOrZoomBlue'),  # 37
    (1019, 'monRandChanceScrollOrZoom'),  # 38
    (1040, 'monAnim27RandomEffectScrollRight'),  # 39
    (1047, 'monAnim28RandomEffectScrollUpFast'),  # 40
    (1054, 'monAnim29RandomEffectScrollUp'),  # 41
    (1061, 'monAnim2ARandEffectScrollZoom1'),  # 42
    (1091, 'monAnim2ARandEffectScrollZoom2'),  # 43
    (1135, 'monAnim2CRandEffectWaitRoute'),  # 44
    (1145, 'monAnim2DRandEffectFlash'),  # 45
    (1165, 'monAnim2ERedBrightening'),  # 46
    (1184, 'monAnim2FGreenBrightening'),  # 47
    (1203, 'monAnim30GreySolid'),  # 48
    (1217, 'monAnim31RedSolid'),  # 49
    (1231, 'monAnim32GreenSolid'),  # 50
    (1245, 'monAnim33BlackSolid'),  # 51
)

# (image id, width, height, level, format, depth, s, t, GoldenEye's name), by the index MONUSEIMAGE names
IMAGES = (
    (2187, 32, 32, 0, 4, 1, 0, 0, 'IMAGE_MONITOR_BOND'),
    (2188, 128, 16, 0, 4, 1, 0, 0, 'IMAGE_MONITOR_LOCATION'),
    (2189, 128, 16, 0, 4, 1, 0, 0, 'IMAGE_MONITOR_BEGINARMING'),
    (2190, 128, 16, 0, 4, 1, 0, 0, 'IMAGE_MONITOR_TARGET'),
    (2191, 128, 16, 0, 4, 1, 0, 0, 'IMAGE_MONITOR_SEVERNAYA'),
    (2192, 128, 16, 0, 4, 1, 0, 0, 'IMAGE_MONITOR_BREAKTARGET'),
    (2193, 128, 16, 0, 4, 1, 0, 0, 'IMAGE_MONITOR_AIMER'),
    (2194, 32, 32, 6, 0, 2, 0, 0, 'IMAGE_MONITOR_EARTH'),
    (2195, 32, 32, 6, 0, 2, 0, 0, 'IMAGE_MONITOR_DESKTOPBANG'),
    (2196, 32, 32, 6, 0, 2, 0, 0, 'IMAGE_MONITOR_HEATMAP'),
    (2197, 32, 32, 6, 0, 2, 0, 0, 'IMAGE_MONITOR_3DMATH'),
    (1185, 32, 32, 6, 0, 2, 0, 0, 'IMAGE_MONITOR_DESKTOPBARS'),
    (2198, 32, 32, 6, 0, 2, 0, 0, 'IMAGE_MONITOR_2DMATH'),
    (2199, 32, 32, 6, 0, 2, 0, 0, 'IMAGE_MONITOR_SATELLITE'),
    (1186, 32, 32, 6, 0, 2, 0, 0, 'IMAGE_MONITOR_DESKTOP'),
    (1187, 32, 32, 6, 0, 2, 0, 0, 'IMAGE_MONITOR_DESKTOPSTAGGERED'),
    (2200, 16, 16, 5, 4, 1, 0, 0, 'IMAGE_MONITOR_CUBE1'),
    (582, 32, 32, 6, 0, 2, 0, 0, 'IMAGE_MONITOR_SHUTTLE1'),
    (583, 32, 32, 6, 0, 2, 0, 0, 'IMAGE_MONITOR_SHUTTLE2'),
    (584, 32, 32, 6, 0, 2, 0, 0, 'IMAGE_MONITOR_EARTHFULL1'),
    (2201, 32, 32, 6, 0, 2, 0, 0, 'IMAGE_MONITOR_EARTHFULL2'),
    (2202, 32, 32, 6, 0, 2, 0, 0, 'IMAGE_MONITOR_BLUESTARS'),
    (2203, 32, 32, 6, 0, 2, 0, 0, 'IMAGE_MONITOR_GALAXY1'),
    (2204, 32, 32, 6, 0, 2, 0, 0, 'IMAGE_MONITOR_GALAXY2'),
    (581, 32, 32, 6, 0, 2, 0, 0, 'IMAGE_MONITOR_EARTHTEXT'),
    (2205, 32, 32, 6, 0, 2, 0, 0, 'IMAGE_MONITOR_TARGETEARTH'),
    (2206, 32, 32, 6, 0, 2, 0, 0, 'IMAGE_MONITOR_GALAXY3'),
    (2227, 64, 64, 0, 4, 1, 0, 0, 'IMAGE_MONITOR_STATIC'),
    (2223, 32, 32, 0, 4, 1, 0, 0, 'IMAGE_MONITOR_SINE'),
    (2224, 32, 32, 0, 4, 1, 0, 0, 'IMAGE_MONITOR_TEXT'),
    (2225, 32, 32, 0, 4, 1, 0, 0, 'IMAGE_MONITOR_BARS'),
    (2226, 32, 32, 0, 4, 1, 0, 0, 'IMAGE_MONITOR_SQUARES'),
    (2219, 32, 32, 0, 4, 1, 0, 0, 'IMAGE_MONITOR_FIST1'),
    (2220, 32, 32, 0, 4, 1, 0, 0, 'IMAGE_MONITOR_FIST2'),
    (2221, 32, 32, 0, 4, 1, 0, 0, 'IMAGE_MONITOR_FIST3'),
    (2222, 32, 32, 0, 4, 1, 0, 0, 'IMAGE_MONITOR_FIST4'),
    (2218, 32, 32, 0, 4, 1, 0, 0, 'IMAGE_MONITOR_SKATEBOARD4'),
    (2207, 32, 32, 0, 4, 1, 0, 0, 'IMAGE_MONITOR_SKATEBOARD1'),
    (2208, 32, 32, 0, 4, 1, 0, 0, 'IMAGE_MONITOR_SKATEBOARD2'),
    (2209, 32, 32, 0, 4, 1, 0, 0, 'IMAGE_MONITOR_SKATEBOARD3'),
    (2210, 32, 32, 6, 4, 1, 0, 0, 'IMAGE_MONITOR_TALK1'),
    (2211, 32, 32, 6, 4, 1, 0, 0, 'IMAGE_MONITOR_TALK2'),
    (2212, 32, 32, 6, 4, 1, 0, 0, 'IMAGE_MONITOR_TALK3'),
    (2213, 32, 32, 6, 4, 1, 0, 0, 'IMAGE_MONITOR_TALK4'),
    (2214, 128, 48, 0, 4, 1, 0, 2, 'IMAGE_MONITOR_WORLDMAP'),
    (2215, 16, 16, 5, 4, 1, 0, 0, 'IMAGE_MONITOR_CUBE2'),
    (2216, 16, 16, 5, 4, 1, 0, 0, 'IMAGE_MONITOR_CUBE3'),
    (2217, 16, 16, 5, 4, 1, 0, 0, 'IMAGE_MONITOR_CUBE4'),
    (2263, 54, 54, 0, 3, 1, 2, 2, 'IMAGE_MONITOR_TRIANGLE'),
    (837, 32, 32, 6, 0, 2, 0, 0, 'IMAGE_MONITOR_KEYBOARDKEY'),
)
