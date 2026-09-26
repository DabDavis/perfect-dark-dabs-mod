/**
 * GoldenEye's own music, out of the player's ROM: the levels of the remake
 * play what GoldenEye plays on them.
 *
 * GoldenEye picks a level's music from one table, `music_setup_entries` - a
 * main theme, a background and an X theme a level - and a level the table does
 * not name (the multiplayer-only ones) draws its theme from `random_tracks`
 * (getmusictrack_or_randomtrack()). The conversion puts each level's row on its
 * line of the maps and missions blocks, and Perfect Dark's own three questions
 * (stagemusic.c) are answered from it: the main theme is its primary track, the
 * background its ambient one, and the X theme what a mission's MusicPlaySlot
 * commands bring in, which already convert to Perfect Dark's X reasons.
 *
 * Each sequence plays at GoldenEye's own volume for it
 * (`g_musicDefaultTrackVolume`), on the scale the folders theme always has
 * here: its 0x6665 is Perfect Dark's menu scale, 0x4ccc, so every sequence is
 * three quarters of GoldenEye's - which is what the sound effects are played
 * at beside it (GESFX_VOLUME).
 */
#include <stdio.h>
#include <string.h>
#include <ultra64.h>
#include <PR/libaudio.h>
#include "constants.h"
#include "types.h"
#include "bss.h"
#include "data.h"
#include "fs.h"
#include "system.h"
#include "modloader.h"
#include "preprocess.h"
#include "gemusic.h"
#include "game/music.h"
#include "game/mplayer/mplayer.h"
#include "lib/rng.h"
#include "lib/audiodma.h"
#include "lib/snd.h"

#define MAX_SEQUENCES 128
#define MAX_RANDOM    64

static struct {
	s32 searcheddirs;
	u8 *seqs; // the sequence table as the ROM has it, and NULL until it is found
	u32 seqlen;
	s32 count;
	ALBank *bank;
	s16 seqnums[MAX_SEQUENCES]; // 0 not asked for yet, -1 cannot be had
	s16 volumes[MAX_SEQUENCES];
	s16 random[MAX_RANDOM];
	s32 numrandom;
	s32 drawnstage;
	s32 drawn;
} g_GeMusic = { .drawnstage = -1 };

static u32 be32(const u8 *p)
{
	return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static u8 *musicLoadFile(const char *dir, const char *name, u32 *len, u32 pad)
{
	char path[FS_MAXPATH + 1];

	snprintf(path, sizeof(path), "%s/menu/%s", dir, name);
	*len = 0;

	// asked for first: a load of a file that is not there is an error line of
	// its own, and every mod dir but one has no menu/
	return fsFileSize(path) > 0 ? fsFileLoadPadded(path, len, pad) : NULL;
}

static s32 musicLoad(void)
{
	if (g_GeMusic.seqs) {
		return 1;
	}

	// nothing found, and nothing new to look in
	if (g_GeMusic.searcheddirs == fsGetNumModDirs()) {
		return 0;
	}

	g_GeMusic.searcheddirs = fsGetNumModDirs();

	for (s32 i = 0; i < g_GeMusic.searcheddirs; i++) {
		const char *dir = fsGetModDirAt(i);
		u32 len = 0;
		u32 ctllen = 0;
		u32 seqlen = 0;
		u8 *raw;
		u8 *ctl;
		u8 *tbl;
		u8 *seqs;
		u8 *list;

		if (!dir || !(raw = musicLoadFile(dir, "instrumentsctl", &len, 0))) {
			continue;
		}

		ctl = preprocessALBankFile(raw, len, &ctllen);
		sysMemFree(raw);
		// padded: the sound DMA reads a whole item from where a sample starts
		tbl = musicLoadFile(dir, "instrumentstbl", &len, ADMA_ITEM_SIZE);
		seqs = musicLoadFile(dir, "sequences", &seqlen, 0);

		if (!ctl || !tbl || !seqs || seqlen < 4) {
			sysMemFree(ctl);
			sysMemFree(tbl);
			sysMemFree(seqs);
			continue;
		}

		// what a sequence plays from stays loaded: an appended sequence is
		// never taken back
		alBnkfNew((ALBankFile *)ctl, tbl);
		g_GeMusic.bank = ((ALBankFile *)ctl)->bankArray[0];
		g_GeMusic.seqs = seqs;
		g_GeMusic.seqlen = seqlen;
		g_GeMusic.count = (seqs[0] << 8) | seqs[1];

		if (g_GeMusic.count > MAX_SEQUENCES) {
			g_GeMusic.count = MAX_SEQUENCES;
		}

		if (seqlen < 4 + (u32)g_GeMusic.count * 8) {
			g_GeMusic.count = (seqlen - 4) / 8;
		}

		// a conversion from before these were written plays every sequence
		// at the folders theme's volume, and draws nothing
		if ((list = musicLoadFile(dir, "musicvolumes.bin", &len, 0))) {
			for (u32 n = 0; n < len / 2 && n < MAX_SEQUENCES; n++) {
				g_GeMusic.volumes[n] = (s16)((list[n * 2] << 8) | list[n * 2 + 1]);
			}

			sysMemFree(list);
		}

		if ((list = musicLoadFile(dir, "musicrandom.bin", &len, 0))) {
			for (u32 n = 0; n < len / 2 && g_GeMusic.numrandom < MAX_RANDOM; n++) {
				const s16 geseq = (s16)((list[n * 2] << 8) | list[n * 2 + 1]);

				if (geseq <= 0) {
					break;
				}

				g_GeMusic.random[g_GeMusic.numrandom++] = geseq;
			}

			sysMemFree(list);
		}

		return 1;
	}

	return 0;
}

s32 geMusicSequence(s32 geseq)
{
	const u8 *e;
	s32 seqnum;

	if (geseq <= 0 || geseq >= MAX_SEQUENCES || !musicLoad() || geseq >= g_GeMusic.count || !g_GeMusic.bank) {
		return -1;
	}

	if (g_GeMusic.seqnums[geseq]) {
		return g_GeMusic.seqnums[geseq];
	}

	e = g_GeMusic.seqs + 4 + geseq * 8;
	seqnum = -1;

	if (be32(e) < g_GeMusic.seqlen && (u32)((e[6] << 8) | e[7]) <= g_GeMusic.seqlen - be32(e)) {
		seqnum = seqAppend(g_GeMusic.seqs + be32(e), (e[4] << 8) | e[5], (e[6] << 8) | e[7], g_GeMusic.bank);
	}

	if (seqnum >= 0 && g_GeMusic.volumes[geseq] > 0) {
		seqAppendSetScale(seqnum, g_GeMusic.volumes[geseq] * 3 / 4);
	}

	g_GeMusic.seqnums[geseq] = seqnum >= 0 ? seqnum : -1;

	return g_GeMusic.seqnums[geseq];
}

void geMusicStageReset(void)
{
	g_GeMusic.drawnstage = -1;
}

s32 geMusicIsStage(s32 stagenum)
{
	s32 tracks[3];

	return (modloaderStageIsMission(stagenum) || modloaderStageIsRemake(stagenum))
		&& modloaderGetStageMusic(stagenum, tracks) && musicLoad();
}

s32 geMusicStageTrack(s32 stagenum, s32 which)
{
	s32 tracks[3];
	s32 seqnum;

	if (which < 0 || which > GEMUSIC_X || !geMusicIsStage(stagenum) || !modloaderGetStageMusic(stagenum, tracks)) {
		return GEMUSIC_NOTOURS;
	}

	// an arena's music is the player's to pick, and GoldenEye's is what Random
	// plays (as a borrowed mod's arena plays its own, modborrow.c)
	if (g_Vars.normmplayerisrunning && (mpGetUsingMultipleTunes() || mpGetCurrentTrackSlotNum() >= 0)) {
		return GEMUSIC_NOTOURS;
	}

	if (which == GEMUSIC_MAIN && tracks[GEMUSIC_MAIN] < 0) {
		// asked more than once as a level starts, and again whenever the
		// theme comes back from under something: the draw is the level's
		if (g_GeMusic.drawnstage != stagenum) {
			if (g_GeMusic.numrandom <= 0) {
				return GEMUSIC_NOTOURS;
			}

			g_GeMusic.drawn = g_GeMusic.random[rngRandom() % (u32)g_GeMusic.numrandom];
			g_GeMusic.drawnstage = stagenum;
		}

		tracks[GEMUSIC_MAIN] = g_GeMusic.drawn;
	}

	if (tracks[which] < 0) {
		return -1;
	}

	seqnum = geMusicSequence(tracks[which]);

	if (seqnum < 0) {
		return which == GEMUSIC_MAIN ? GEMUSIC_NOTOURS : -1;
	}

	if (which == GEMUSIC_MAIN && g_Vars.normmplayerisrunning) {
		// GoldenEye's themes loop, and one plays a match through
		g_MusicLife60 = 0x7fffffff;
	}

	return seqnum;
}
