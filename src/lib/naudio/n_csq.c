#include <libaudio.h>
#include "n_libaudio.h"
#include <os_internal.h>
#include <ultraerror.h>
#include "types.h"
#ifndef PLATFORM_N64
#include "bss.h"
#include "system.h"

/**
 * How many times each of the three music players' sequences has come back to
 * its start, for GE Plus's looping Cinema, which breaks between levels on one
 * so the music ends cleanly (gecinema.c).
 *
 * A track's own jump back is not the song's: Runway's drum track loops a
 * 1536-tick pattern for ever, and other tracks repeat a phrase a set number of
 * times. The song has come round once every track still playing has made its
 * loop-for-ever jump - or when the sequence ends, for one with no loop.
 */
volatile u32 g_SeqLoopPoints[3];
static u16 g_SeqLoopTracks[3];

static s32 seqInstanceIndex(ALCSeq *seq)
{
	for (s32 i = 0; i < 3; i++) {
		if (seq == &g_SeqInstances[i].seq) {
			return i;
		}
	}

	return -1;
}

static void seqNoteLoopJump(ALCSeq *seq, u32 track)
{
	const s32 i = seqInstanceIndex(seq);

	if (i >= 0) {
		g_SeqLoopTracks[i] |= 1 << track;

		if ((g_SeqLoopTracks[i] & seq->validTracks) == seq->validTracks) {
			g_SeqLoopTracks[i] = 0;
			g_SeqLoopPoints[i]++;
		}
	}
}

static void seqNoteEnd(ALCSeq *seq)
{
	const s32 i = seqInstanceIndex(seq);

	if (i >= 0) {
		g_SeqLoopTracks[i] = 0;
		g_SeqLoopPoints[i]++;
	}
}
#endif

u32 __n_alCSeqGetTrackEvent(ALCSeq *seq, u32 track, N_ALEvent *event, s32 arg3);
u8 __getTrackByte(ALCSeq *seq, u32 track);
u32 __readVarLen(ALCSeq *seq,u32 track);

#ifndef PLATFORM_N64
static void seqNew(ALCSeq *seq, u8 *ptr, u8 *end);

void n_alCSeqNew(ALCSeq *seq, u8 *ptr)
{
	seqNew(seq, ptr, NULL);
}

static void seqNew(ALCSeq *seq, u8 *ptr, u8 *end)
#else
void n_alCSeqNew(ALCSeq *seq, u8 *ptr)
#endif
{
	u32 i, tmpOff, flagTmp;

	/* load the seqence pointed to by ptr   */
	seq->base = (ALCMidiHdr*)ptr;
#ifndef PLATFORM_N64
	seq->end = end;
	seq->bad = 0;
#endif
	seq->validTracks = 0;
	seq->lastDeltaTicks = 0;
	seq->lastTicks = 0;
	seq->deltaFlag = 1;

	for (i = 0; i < 16; i++) {
		seq->lastStatus[i] = 0;
		seq->curBUPtr[i] = 0;
		seq->curBULen[i] = 0;
		tmpOff = seq->base->trackOffset[i];

		if (tmpOff) {
			flagTmp = 1 << i;
			seq->validTracks |= flagTmp;
			seq->curLoc[i] = (u8*)((uintptr_t)ptr + tmpOff);
			seq->evtDeltaTicks[i] = __readVarLen(seq,i);
		} else {
			seq->curLoc[i] = 0;
		}
	}

	seq->qnpt = 1.0f / (f32)seq->base->division;

#ifndef PLATFORM_N64
	if (seqInstanceIndex(seq) >= 0) {
		g_SeqLoopTracks[seqInstanceIndex(seq)] = 0;
	}
#endif
}

#ifndef PLATFORM_N64
/**
 * n_alCSeqNew() for a sequence of len bytes in a buffer with 16 bytes to spare
 * past them, every read of which then stays inside that: a read that would
 * leave it ends the sequence (AL_SEQ_END_EVT)
 * instead. False, and nothing to play, when its header already points past
 * its end.
 *
 * The cartridge reads whatever its pointers say: a loop's jump back is a
 * 32-bit offset from where it stands, and a bad one (or bad data) lands on
 * the N64 in other RAM. Here it lands outside the process: a crash in
 * __getTrackByte() reading 0xffffffffffffffff on a GoldenEye Arenas match's
 * death tune (F3 20261010-224719, sequence 513).
 */
s32 n_alCSeqNewBounded(ALCSeq *seq, u8 *ptr, u32 len)
{
	const ALCMidiHdr *hdr = (const ALCMidiHdr *)ptr;
	s32 i;

	if (len < sizeof(ALCMidiHdr) || hdr->division == 0) {
		return false;
	}

	for (i = 0; i < 16; i++) {
		if (hdr->trackOffset[i] && (hdr->trackOffset[i] < sizeof(ALCMidiHdr) || hdr->trackOffset[i] >= len)) {
			return false;
		}
	}

	// the stock reader takes one more delta after the last track's end, past
	// the data: a few bytes of the buffer's slack are fair game (seqPlay()'s
	// buffer has 0x40 past the data at least)
	seqNew(seq, ptr, ptr + len + 16);

	return !seq->bad;
}

/** Whether len bytes from p lie inside the sequence (always, unchecked). */
static bool seqInside(ALCSeq *seq, const u8 *p, u32 len)
{
	if (seq->end == NULL) {
		return true;
	}

	if (p >= (const u8 *)seq->base && p <= seq->end && len <= (u32)(seq->end - p)) {
		return true;
	}

	if (!seq->bad) {
		seq->bad = 1;
		sysLogPrintf(LOG_WARNING, "music: player %d's sequence read %td bytes from its start, outside its %td; it ends there",
				seqInstanceIndex(seq), p - (const u8 *)seq->base, seq->end - (const u8 *)seq->base);
	}

	return false;
}
#endif

void n_alCSeqNextEvent(ALCSeq *seq, N_ALEvent *evt, s32 arg2)
{
	u32 i;
	u32 firstTime = 0xffffffff;
	u32 firstTrack;
	u32 lastTicks = seq->lastDeltaTicks;

	for (i = 0; i < 16; i++) {
		if ((seq->validTracks >> i) & 1) {
			if (seq->deltaFlag) {
				seq->evtDeltaTicks[i] -= lastTicks;
			}

			if (seq->evtDeltaTicks[i] < firstTime) {
				firstTime = seq->evtDeltaTicks[i];
				firstTrack = i;
			}
		}
	}

#ifndef PLATFORM_N64
	// no track left to read (firstTrack is then never set), or a read that
	// left the sequence: it ends
	if (seq->bad || firstTime == 0xffffffff) {
		seq->validTracks = 0;
		evt->type = AL_SEQ_END_EVT;
		evt->msg.midi.ticks = 0;
		seq->lastDeltaTicks = 0;
		seq->deltaFlag = 1;
		return;
	}
#endif

	__n_alCSeqGetTrackEvent(seq, firstTrack, evt, arg2);

#ifndef PLATFORM_N64
	if (seq->bad) {
		seq->validTracks = 0;
		evt->type = AL_SEQ_END_EVT;
	}
#endif

	evt->msg.midi.ticks = firstTime;
	seq->lastTicks += firstTime;
	seq->lastDeltaTicks = firstTime;

	if (evt->type != AL_TRACK_END) {
		seq->evtDeltaTicks[firstTrack] += __readVarLen(seq, firstTrack);
	}

	seq->deltaFlag = 1;
}

u32 __n_alCSeqGetTrackEvent(ALCSeq *seq, u32 track, N_ALEvent *event, s32 arg3)
{
	u32 offset;
	u8 status, loopCt, curLpCt, *tmpPtr;

	status = __getTrackByte(seq, track);

	if (status == AL_MIDI_Meta) {
		u8 type = __getTrackByte(seq, track);

		if (type == AL_MIDI_META_TEMPO) {
			event->type = AL_TEMPO_EVT;
			event->msg.tempo.status = status;
			event->msg.tempo.type = type;
			event->msg.tempo.byte1 = __getTrackByte(seq, track);
			event->msg.tempo.byte2 = __getTrackByte(seq, track);
			event->msg.tempo.byte3 = __getTrackByte(seq, track);
			seq->lastStatus[track] = 0;  /* lastStatus not supported after meta */
		} else if (type == AL_MIDI_META_EOT) {
			u32 flagMask;

			flagMask = 1 << track;
			seq->validTracks = seq->validTracks ^ flagMask;

			if (seq->validTracks) { /* there is music left don't end */
				event->type = AL_TRACK_END;
			} else {       /* no more music send AL_SEQ_END_EVT msg */
				event->type = AL_SEQ_END_EVT;
#ifndef PLATFORM_N64
				if (arg3) {
					seqNoteEnd(seq);
				}
#endif
			}
		} else if (type == AL_CMIDI_LOOPSTART_CODE) {
			status = __getTrackByte(seq, track);
			event->msg.loop.count = status << 8;

			status = __getTrackByte(seq, track);
			event->msg.loop.count += status;

			seq->lastStatus[track] = 0;
			event->type = AL_CSP_LOOPSTART;
		} else if (type == AL_CMIDI_LOOPEND_CODE) {
			tmpPtr = seq->curLoc[track];
#ifndef PLATFORM_N64
			if (!seqInside(seq, tmpPtr, 6)) {
				event->type = AL_SEQ_END_EVT;
				return 0;
			}
#endif
			loopCt = *tmpPtr++;
			curLpCt = *tmpPtr;

			if (curLpCt == 0 || !arg3) {
				*tmpPtr = loopCt; /* reset current loop count */
				seq->curLoc[track] = tmpPtr + 5; /* move pointer to end of event */
			} else {
				if (curLpCt != 0xff) { /* not a loop forever */
					*tmpPtr = curLpCt - 1;   /* decrement current loop count */
				}

				tmpPtr++;                    /* get offset from end of event */
				offset = (*tmpPtr++) << 24;
				offset += (*tmpPtr++) << 16;
				offset += (*tmpPtr++) << 8;
				offset += *tmpPtr++;
#ifndef PLATFORM_N64
				if (seq->end && offset > (u32)(tmpPtr - (u8 *)seq->base)) {
					seqInside(seq, (u8 *)seq->base - 1, 1);
					event->type = AL_SEQ_END_EVT;
					return 0;
				}
#endif
				seq->curLoc[track] = tmpPtr - offset;
#ifndef PLATFORM_N64
				if (curLpCt == 0xff) {
					seqNoteLoopJump(seq, track);
				}
#endif
			}

			seq->lastStatus[track] = 0;
			event->type = AL_CSP_LOOPEND;
		}
	} else {
		event->type = AL_SEQ_MIDI_EVT;

		if (status & 0x80) {
			event->msg.midi.status = (status & 0xf0) | track;
			event->msg.midi.byte1 = __getTrackByte(seq,track);
			seq->lastStatus[track] = event->msg.midi.status;
		} else {    /* running status */
			event->msg.midi.status = seq->lastStatus[track];
			event->msg.midi.byte1 = status;
		}

		if ((event->msg.midi.status & 0xf0) != AL_MIDI_ProgramChange
				&& (event->msg.midi.status & 0xf0) != AL_MIDI_ChannelPressure) {
			event->msg.midi.byte2 = __getTrackByte(seq,track);

			if ((event->msg.midi.status & 0xf0) == AL_MIDI_NoteOn) {
				event->msg.midi.duration = __readVarLen(seq,track);
			}
		} else {
			event->msg.midi.byte2 = 0;
		}
	}

	return TRUE;
}

s32 alCSeqGetTicks(ALCSeq *seq)
{
	return seq->lastTicks;
}

void alCSeqSetLoc(ALCSeq *seq, ALCSeqMarker *m)
{
	s32 i;

	seq->validTracks    = m->validTracks;
	seq->lastTicks      = m->lastTicks;
	seq->lastDeltaTicks = m->lastDeltaTicks;

	for (i = 0; i < 16; i++) {
		seq->curLoc[i]        = m->curLoc[i];
		seq->curBUPtr[i]      = m->curBUPtr[i];
		seq->curBULen[i]      = m->curBULen[i];
		seq->lastStatus[i]    = m->lastStatus[i];
		seq->evtDeltaTicks[i] = m->evtDeltaTicks[i];
	}
}

void alCSeqGetLoc(ALCSeq *seq, ALCSeqMarker *m)
{
	s32 i;

	m->validTracks    = seq->validTracks;
	m->lastTicks      = seq->lastTicks;
	m->lastDeltaTicks = seq->lastDeltaTicks;

	for (i = 0; i < 16; i++) {
		m->curLoc[i]        = seq->curLoc[i];
		m->curBUPtr[i]      = seq->curBUPtr[i];
		m->curBULen[i]      = seq->curBULen[i];
		m->lastStatus[i]    = seq->lastStatus[i];
		m->evtDeltaTicks[i] = seq->evtDeltaTicks[i];
	}
}

void n_alCSeqNewMarker(ALCSeq *seq, ALCSeqMarker *m, u32 ticks)
{
	N_ALEvent evt;
	ALCSeq tempSeq;
	s32 i;

	n_alCSeqNew(&tempSeq, (u8*)seq->base);
#ifndef PLATFORM_N64
	tempSeq.end = seq->end;
#endif

	do {
		m->validTracks    = tempSeq.validTracks;
		m->lastTicks      = tempSeq.lastTicks;
		m->lastDeltaTicks = tempSeq.lastDeltaTicks;

		for (i = 0; i < 16; i++) {
			m->curLoc[i]        = tempSeq.curLoc[i];
			m->curBUPtr[i]      = tempSeq.curBUPtr[i];
			m->curBULen[i]      = tempSeq.curBULen[i];
			m->lastStatus[i]    = tempSeq.lastStatus[i];
			m->evtDeltaTicks[i] = tempSeq.evtDeltaTicks[i];
		}

		n_alCSeqNextEvent(&tempSeq, &evt, 0);

		if (evt.type == AL_SEQ_END_EVT) {
			break;
		}
	} while (tempSeq.lastTicks < ticks);
}

void func00039718(ALCSeq *seq, ALCSeqMarker *m, u32 ticks, u32 arg3)
{
	N_ALEvent evt;
	ALCSeq tempSeq;
	s32 i;
	s32 j;
	ALCSeqMarker m2;

	n_alCSeqNew(&tempSeq, (u8*)seq->base);
#ifndef PLATFORM_N64
	tempSeq.end = seq->end;
#endif

	for (j = 0; j < ticks; j++) {
		m[j].lastTicks = 0;
	}

	do {
		m2.validTracks    = tempSeq.validTracks;
		m2.lastTicks      = tempSeq.lastTicks;
		m2.lastDeltaTicks = tempSeq.lastDeltaTicks;

		for (i = 0; i < 16; i++) {
			m2.curLoc[i]        = tempSeq.curLoc[i];
			m2.curBUPtr[i]      = tempSeq.curBUPtr[i];
			m2.curBULen[i]      = tempSeq.curBULen[i];
			m2.lastStatus[i]    = tempSeq.lastStatus[i];
			m2.evtDeltaTicks[i] = tempSeq.evtDeltaTicks[i];
		}

		n_alCSeqNextEvent(&tempSeq, &evt, 0);

		if (evt.type == AL_CSP_LOOPSTART) {
			if ((evt.msg.loop.count >> 8) >= arg3 && (evt.msg.loop.count >> 8) < arg3 + ticks) {
				if (m[(evt.msg.loop.count >> 8) - arg3].lastTicks == 0) {
					m[(evt.msg.loop.count >> 8) - arg3] = m2;

					if (--j <= 0) {
						return;
					}
				}
			}
		}
	} while (evt.type != AL_SEQ_END_EVT);
}

u8 __getTrackByte(ALCSeq *seq, u32 track)
{
	u8 theByte;

#ifndef PLATFORM_N64
	// a read outside a bounded sequence (n_alCSeqNewBounded()) reads a zero,
	// and the event it was part of becomes the sequence's end
	if (seq->bad) {
		return 0;
	}

	if (seq->end) {
		if (seq->curBULen[track]) {
			if (!seqInside(seq, seq->curBUPtr[track], 1)) {
				return 0;
			}
		} else if (!seqInside(seq, seq->curLoc[track], 1)) {
			return 0;
		} else if (*seq->curLoc[track] == AL_CMIDI_BLOCK_CODE) {
			const u8 *code = seq->curLoc[track];

			if (!seqInside(seq, code, 2)) {
				return 0;
			}

			if (code[1] != AL_CMIDI_BLOCK_CODE) {
				// a backup: a copy of len bytes from further back
				const u32 backup = ((u32)code[1] << 8) + code[2] + 4;

				if (!seqInside(seq, code, 4)) {
					return 0;
				}

				if (backup > (u32)(code + 4 - (const u8 *)seq->base)) {
					seqInside(seq, (const u8 *)seq->base - 1, 1);
					return 0;
				}
			}
		}
	}
#endif

	if (seq->curBULen[track]) {
		theByte = *seq->curBUPtr[track];
		seq->curBUPtr[track]++;
		seq->curBULen[track]--;
	} else /* need to handle backup mode */ {
		theByte = *seq->curLoc[track];
		seq->curLoc[track]++;

		if (theByte == AL_CMIDI_BLOCK_CODE) {
			u8 loBackUp, hiBackUp, theLen, nextByte;
			u32 backup;

			nextByte = *seq->curLoc[track];
			seq->curLoc[track]++;

			if (nextByte != AL_CMIDI_BLOCK_CODE) {
				/**
				 * if here, then got a backup section. get the amount of
				 * backup, and the len of the section. Subtract the amount of
				 * backup from the curLoc ptr, and subtract four more, since
				 * curLoc has been advanced by four while reading the codes.
				 */
				hiBackUp = nextByte;
				loBackUp = *seq->curLoc[track];
				seq->curLoc[track]++;
				theLen = *seq->curLoc[track];
				seq->curLoc[track]++;
				backup = (u32)hiBackUp;
				backup = backup << 8;
				backup += loBackUp;
				seq->curBUPtr[track] = seq->curLoc[track] - (backup + 4);
				seq->curBULen[track] = (u32)theLen;

				/* now get the byte */
				theByte = *seq->curBUPtr[track];
				seq->curBUPtr[track]++;
				seq->curBULen[track]--;
			}
		}
	}

	return theByte;
}

u32 __readVarLen(ALCSeq *seq, u32 track)
{
	u32 value;
	u32 c;

	value = (u32)__getTrackByte(seq, track);

	if (value & 0x80) {
		value &= 0x7f;

		do {
			c = (u32)__getTrackByte(seq, track);
			value = (value << 7) + (c & 0x7f);
		} while (c & 0x80);
	}

	return value;
}
