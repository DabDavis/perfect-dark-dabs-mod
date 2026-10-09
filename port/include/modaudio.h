#ifndef _IN_MODAUDIO_H
#define _IN_MODAUDIO_H

#include <PR/ultratypes.h>
#include <PR/libaudio.h>

/**
 * A PD mod's audio segments (sfxctl, sfxtbl, seqctl, seqtbl, sequences) swapped
 * in and out between stages, without a restart (modaudio.c).
 *
 * modAudioQuiesce() stops the music and every sound and returns 1 once nothing
 * is playing; call it once a frame until it does (the polite path).
 * modAudioEnter() and modAudioLeave() swap the sound table, the music bank and
 * the sequence table and flush every cache that holds a sample; called at a
 * stage boundary without that quiet (online, at the host's stage load) they
 * end every sound and sequence outright first, and keep a left mod's buffers
 * until the stops have reached the voices. A segment
 * the mod does not ship is the game's own. Entering a mod while another is
 * entered leaves that one first. Leave returns to the banks the game booted
 * with.
 */
void modAudioEnter(const char *moddir);
void modAudioLeave(void);
s32 modAudioQuiesce(void);

/**
 * The sound ids the boot bank's slots take: the longest of the boot bank
 * (bootcount sounds) and every installed mod's, so a mod entered live never
 * runs into the ids appended after them. Called once by sndLoadSfxCtl().
 */
s32 modAudioReserveSounds(s32 bootcount);

/** The mod directory entered, or NULL. */
const char *modAudioCurrent(void);

/** A text dump of every sound, the music bank and the sequence table, hashed (tests). */
void modAudioDump(const char *path);

/** --mod-audio-enter / --mod-dump-audio, after the boot's banks are built. */
void modAudioTestSwitches(void);

/**
 * Rebasing a mod's converted ALSounds onto the game's own bank starts, which
 * every loader in snd.c adds to what it reads (sndGetCtlStart(),
 * sndGetTblStart()). Each object is moved once: sounds share envelopes, key
 * maps and wave tables. tbl NULL is a bank whose samples are the game's own
 * sfxtbl.
 */
struct sndrebase {
	u8 *ctl;
	u8 *tbl;
	uintptr_t *seen;
	s32 numseen;
	s32 maxseen;
};

/** Rebases the ALSound at rb->ctl + off; returns its offset from the game's ctl start (for sndAppendSound()). */
uintptr_t sndRebaseSound(struct sndrebase *rb, uintptr_t off);
void sndRebaseFree(struct sndrebase *rb);

#endif
