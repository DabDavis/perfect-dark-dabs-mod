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
#include "gexfront.h"
#include "game/music.h"
#include "game/mplayer/mplayer.h"
#include "lib/rng.h"
#include "lib/audiodma.h"
#include "lib/snd.h"

#define MAX_SEQUENCES 128
#define MAX_RANDOM    64
// the converted mods a bank is held for: GoldenEye's, and a ROM hack's or two
#define MAX_BANKS     4

/**
 * A conversion's music: GoldenEye's, or a GoldenEye ROM hack's own (Goldfinger
 * 64 has its own instruments and sequences, on GoldenEye's numbering). Each is
 * read once, out of its own mod dir's menu/, and what it appends stays: an
 * appended sequence is never taken back.
 */
struct gemusicbank {
	s32 moddir;
	u8 *seqs; // the sequence table as the ROM has it
	u32 seqlen;
	s32 count;
	ALBank *bank;
	s16 seqnums[MAX_SEQUENCES]; // 0 not asked for yet, -1 cannot be had
	s16 volumes[MAX_SEQUENCES];
	s16 random[MAX_RANDOM];
	s32 numrandom;
};

static struct {
	struct gemusicbank banks[MAX_BANKS];
	s32 numbanks;
	// the dirs looked in and found without a bank, a bit a dir
	u32 nobank;
	s32 numdirs; // fsGetNumModDirs() when nobank was last cleared
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

// mod dir `moddir`'s bank, read the first time it is asked for; NULL where it has none
static struct gemusicbank *musicBankAt(s32 moddir)
{
	const char *dir = moddir >= 0 ? fsGetModDirAt(moddir) : NULL;
	struct gemusicbank *b;
	u32 len = 0;
	u32 ctllen = 0;
	u32 seqlen = 0;
	u8 *raw;
	u8 *ctl;
	u8 *tbl;
	u8 *seqs;
	u8 *list;

	if (!dir) {
		return NULL;
	}

	for (s32 i = 0; i < g_GeMusic.numbanks; i++) {
		if (g_GeMusic.banks[i].moddir == moddir) {
			return &g_GeMusic.banks[i];
		}
	}

	// mods mounted since: look again
	if (g_GeMusic.numdirs != fsGetNumModDirs()) {
		g_GeMusic.numdirs = fsGetNumModDirs();
		g_GeMusic.nobank = 0;
	}

	if (moddir < 32 && (g_GeMusic.nobank & (1u << moddir))) {
		return NULL;
	}

	if (g_GeMusic.numbanks >= MAX_BANKS || !(raw = musicLoadFile(dir, "instrumentsctl", &len, 0))) {
		if (moddir < 32) {
			g_GeMusic.nobank |= 1u << moddir;
		}

		return NULL;
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

		if (moddir < 32) {
			g_GeMusic.nobank |= 1u << moddir;
		}

		return NULL;
	}

	b = &g_GeMusic.banks[g_GeMusic.numbanks++];
	memset(b, 0, sizeof(*b));
	b->moddir = moddir;

	alBnkfNew((ALBankFile *)ctl, tbl);
	b->bank = ((ALBankFile *)ctl)->bankArray[0];
	b->seqs = seqs;
	b->seqlen = seqlen;
	b->count = (seqs[0] << 8) | seqs[1];

	if (b->count > MAX_SEQUENCES) {
		b->count = MAX_SEQUENCES;
	}

	if (seqlen < 4 + (u32)b->count * 8) {
		b->count = (seqlen - 4) / 8;
	}

	// a conversion from before these were written plays every sequence
	// at the folders theme's volume, and draws nothing
	if ((list = musicLoadFile(dir, "musicvolumes.bin", &len, 0))) {
		for (u32 n = 0; n < len / 2 && n < MAX_SEQUENCES; n++) {
			b->volumes[n] = (s16)((list[n * 2] << 8) | list[n * 2 + 1]);
		}

		sysMemFree(list);
	}

	if ((list = musicLoadFile(dir, "musicrandom.bin", &len, 0))) {
		for (u32 n = 0; n < len / 2 && b->numrandom < MAX_RANDOM; n++) {
			const s16 geseq = (s16)((list[n * 2] << 8) | list[n * 2 + 1]);

			if (geseq <= 0) {
				break;
			}

			b->random[b->numrandom++] = geseq;
		}

		sysMemFree(list);
	}

	sysLogPrintf(LOG_NOTE, "gemusic: %d sequences from %s", b->count, dir);

	return b;
}

/**
 * GoldenEye's own bank: the conversion's dir (GEXPLUSROM_DIR), else the first
 * mounted dir that has one, as it always was.
 */
static struct gemusicbank *musicBankGoldenEye(void)
{
	struct gemusicbank *b = musicBankAt(modloaderGexPlusDirIndex());

	for (s32 i = 0; !b && i < fsGetNumModDirs(); i++) {
		b = musicBankAt(i);
	}

	return b;
}

/**
 * The bank for mod dir `moddir`'s music: its own where it has one, else
 * GoldenEye's. -1 asks for the one playing now: a remake level's own mod's in
 * a level, the folder's (GoldenEye's, or a ROM hack's) in its menus.
 */
static struct gemusicbank *musicBank(s32 moddir)
{
	struct gemusicbank *b = NULL;

	if (moddir < 0) {
		if (modloaderStageIsRemake(g_Vars.stagenum)) {
			moddir = modloaderGetStageModDirIndex(g_Vars.stagenum);
		} else {
			moddir = gexFrontTextModDir();
		}
	}

	if (moddir >= 0) {
		b = musicBankAt(moddir);
	}

	return b ? b : musicBankGoldenEye();
}

static s32 musicSequenceIn(struct gemusicbank *b, s32 geseq)
{
	const u8 *e;
	s32 seqnum;

	if (!b || geseq <= 0 || geseq >= MAX_SEQUENCES || geseq >= b->count || !b->bank) {
		return -1;
	}

	if (b->seqnums[geseq]) {
		return b->seqnums[geseq];
	}

	e = b->seqs + 4 + geseq * 8;
	seqnum = -1;

	if (be32(e) < b->seqlen && (u32)((e[6] << 8) | e[7]) <= b->seqlen - be32(e)) {
		seqnum = seqAppend(b->seqs + be32(e), (e[4] << 8) | e[5], (e[6] << 8) | e[7], b->bank);
	}

	if (seqnum >= 0 && b->volumes[geseq] > 0) {
		seqAppendSetScale(seqnum, b->volumes[geseq] * 3 / 4);
	}

	b->seqnums[geseq] = seqnum >= 0 ? seqnum : -1;

	return b->seqnums[geseq];
}

s32 geMusicSequence(s32 geseq)
{
	return musicSequenceIn(musicBank(-1), geseq);
}

void geMusicStageReset(void)
{
	g_GeMusic.drawnstage = -1;
}

s32 geMusicIsStage(s32 stagenum)
{
	s32 tracks[3];

	return (modloaderStageIsMission(stagenum) || modloaderStageIsRemake(stagenum))
		&& modloaderGetStageMusic(stagenum, tracks) && musicBank(modloaderGetStageModDirIndex(stagenum));
}

s32 geMusicStageTrack(s32 stagenum, s32 which)
{
	s32 tracks[3];
	s32 seqnum;
	struct gemusicbank *b;

	if (which < 0 || which > GEMUSIC_X || !geMusicIsStage(stagenum) || !modloaderGetStageMusic(stagenum, tracks)) {
		return GEMUSIC_NOTOURS;
	}

	// the stage's own mod's music: GoldenEye's, or a ROM hack's
	b = musicBank(modloaderGetStageModDirIndex(stagenum));

	// an arena's music is the player's to pick, and GoldenEye's is what Random
	// plays (as a borrowed mod's arena plays its own, modborrow.c)
	if (g_Vars.normmplayerisrunning && (mpGetUsingMultipleTunes() || mpGetCurrentTrackSlotNum() >= 0)) {
		return GEMUSIC_NOTOURS;
	}

	if (which == GEMUSIC_MAIN && tracks[GEMUSIC_MAIN] < 0) {
		// asked more than once as a level starts, and again whenever the
		// theme comes back from under something: the draw is the level's
		if (g_GeMusic.drawnstage != stagenum) {
			if (b->numrandom <= 0) {
				return GEMUSIC_NOTOURS;
			}

			g_GeMusic.drawn = b->random[rngRandom() % (u32)b->numrandom];
			g_GeMusic.drawnstage = stagenum;
		}

		tracks[GEMUSIC_MAIN] = g_GeMusic.drawn;
	}

	if (tracks[which] < 0) {
		return -1;
	}

	seqnum = musicSequenceIn(b, tracks[which]);

	if (seqnum < 0) {
		return which == GEMUSIC_MAIN ? GEMUSIC_NOTOURS : -1;
	}

	if (which == GEMUSIC_MAIN && g_Vars.normmplayerisrunning) {
		// GoldenEye's themes loop, and one plays a match through
		g_MusicLife60 = 0x7fffffff;
	}

	return seqnum;
}
