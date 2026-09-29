#ifndef _IN_GECONVERT_H
#define _IN_GECONVERT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Raise when a change alters what a conversion writes: arenas converted by an
// older one are converted again at the next start (gexplusrom.c)
#define GECONVERT_VERSION_STR "91"

#define GECONVERT_ROM_SIZE 0xc00000

/**
 * Whether the first 0x40 bytes of a file, in any dump byte order, are
 * GoldenEye 007 (US)'s header.
 */
int geconvertHeaderIsGoldenEyeUs(const uint8_t *head, size_t len);

/**
 * Whether rom (romlen bytes, any of the three dump byte orders) is GoldenEye
 * 007 (US). The ROM is put into .z64 order in place either way.
 */
int geconvertIsGoldenEyeUs(uint8_t *rom, size_t romlen);

/**
 * Converts GoldenEye's levels and props out of the ROM into the maps-only mod
 * directory outdir (port/src/geconvert.c). Returns 1 on success, else 0 with
 * the reason in err. Safe to run on a thread of its own: progress is read with
 * geconvertProgress() out of geconvertTotal().
 */
int geconvertRun(uint8_t *rom, size_t romlen, const char *outdir, char *err, size_t errlen);

int geconvertProgress(void);
int geconvertTotal(void);

/**
 * The file name of mission n's own text bank, under the converted mod's
 * menu/ - the bank every text id in that mission indexes, and the one the
 * conversion wrote its objectives' and its radio messages' ids against
 * (LANGBANK_GEMISSION). NULL when n is not one of GoldenEye's twenty.
 */
const char *geconvertMissionLangFile(int mission);

/**
 * The texture number the conversion gives GoldenEye's image number image:
 * its own, unless Perfect Dark's texture config tables load that number
 * (texRemap(), tools/geconvert/texremap.py).
 */
uint32_t geconvertTexRemap(uint32_t image);

// GoldenEye's own image number for a texture number the conversion wrote (the
// inverse of geconvertTexRemap())
uint32_t geconvertTexUnremap(uint32_t num);

// The ROM's file names for the props (kind 0), characters (1) and hand items
// (2) the conversion writes as Pgx/Cgx/Igx%03uZ, for the asset dump; 0 when
// the ROM is not GoldenEye 007 (US)
int geconvertReadNames(uint8_t *rom, size_t romlen, void (*fn)(void *arg, int kind, int num, const char *file), void *arg);

// where its per-level report lines go (stderr until set)
void geconvertSetLog(void (*fn)(const char *msg));

#ifdef __cplusplus
}
#endif

#endif
