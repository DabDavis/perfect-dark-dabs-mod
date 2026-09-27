#include "n_synthInternals.h"
#include <os.h>
#include <R4300.h>

#define ADPCMFBYTES      9
#define LFSAMPLES        4

Acmd *_decodeChunk(Acmd *ptr, N_PVoice *f, s32 tsam, s32 nbytes, s16 outp, s16 inp, u32 flags);

#ifndef PLATFORM_N64
/**
 * GoldenEye's alRaw16Pull() for the port: outCount samples of a big-endian
 * 16-bit wave into DMEM at *outp, looping between the wave's loop points as
 * often as its count says (-1 for ever) and zeros past its end. The mixer's
 * DMEM holds host-order samples, so each chunk is swapped on the way in.
 */
static Acmd *n_alRaw16Pull(N_PVoice *f, s16 *outp, s32 outCount, Acmd *ptr)
{
	// admaExec() serves at most ADMA_ITEM_SIZE bytes a call
	static s16 chunk[0x100];
	intptr_t base = (intptr_t) f->dc_table->base;
	s32 total = f->dc_table->len >> 1;
	s32 done = 0;

	while (done < outCount) {
		s32 nSam = outCount - done;
		s32 i;

		if (f->dc_loop.count != 0 && f->dc_loop.end > f->dc_loop.start
				&& f->dc_sample >= f->dc_loop.end) {
			if (f->dc_loop.count != -1) {
				f->dc_loop.count--;
			}

			f->dc_sample = f->dc_loop.start;
			f->dc_memin = base + (f->dc_loop.start << 1);
		}

		if (f->dc_loop.count != 0 && f->dc_loop.end > f->dc_sample) {
			nSam = MIN(nSam, f->dc_loop.end - f->dc_sample);
		}

		if (f->dc_sample >= total) {
			// past the end of a wave that no longer loops
			aClearBuffer(ptr++, *outp + (done << 1), nSam << 1);
			f->dc_sample += nSam;
			done += nSam;
			continue;
		}

		nSam = MIN(nSam, total - f->dc_sample);
		nSam = MIN(nSam, (s32) (sizeof(chunk) / sizeof(chunk[0])));

		{
			u8 *src = (u8 *) (f->dc_dma)(f->dc_memin, nSam << 1, f->dc_dmaState);

			for (i = 0; i < nSam; i++) {
				chunk[i] = (s16) ((src[i * 2] << 8) | src[i * 2 + 1]);
			}
		}

		aLoadBuffer(ptr++, nSam << 1, *outp + (done << 1), chunk);

		f->dc_sample += nSam;
		f->dc_memin += nSam << 1;
		done += nSam;
	}

	return ptr;
}
#endif

Acmd *n_alAdpcmPull(N_PVoice *filter, s16 *outp, s32 outCount, Acmd *p)
{
	Acmd *ptr = p;
	s16 inp;
	s32 tsam;
	s32 nframes;
	s32 nbytes;
	s32 overFlow;
	s32 startZero;
	s32 nOver;
	s32 nSam;
	s32 op;
	s32 nLeft;
	s32 bEnd;
	s32 decoded = 0;
	s32 looped = 0;

	N_PVoice *f = filter;

	if (outCount == 0) {
		return ptr;
	}

	inp = N_AL_DECODER_IN;

#ifndef PLATFORM_N64
	// Perfect Dark's own banks hold nothing but ADPCM, so its synthesiser
	// dropped libultra's raw branch; GoldenEye's music bank has two raw waves
	// (instruments 2 and 51 - 51 is the synth bass, a looped single cycle).
	// They play through n_alRaw16Pull(), GoldenEye's alRaw16Pull() in the
	// port's terms. Anything else without a book is silent rather than fatal.
	if (f->dc_table && f->dc_table->type == AL_RAW16_WAVE) {
		return n_alRaw16Pull(f, outp, outCount, ptr);
	}

	if (!f->dc_table || f->dc_table->type != AL_ADPCM_WAVE
			|| !f->dc_table->waveInfo.adpcmWave.book) {
		aClearBuffer(ptr++, *outp, outCount << 1);
		f->dc_lastsam = 0;
		f->dc_sample += outCount;

		return ptr;
	}
#endif

	aLoadADPCM(ptr++, f->dc_bookSize, K0_TO_PHYS(f->dc_table->waveInfo.adpcmWave.book->book));

	looped = (outCount + f->dc_sample > f->dc_loop.end) && (f->dc_loop.count != 0);

	if (looped) {
		nSam = f->dc_loop.end - f->dc_sample;
	} else {
		nSam = outCount;
	}

	if (f->dc_lastsam) {
		nLeft = ADPCMFSIZE - f->dc_lastsam;
	} else {
		nLeft = 0;
	}

	tsam = nSam - nLeft;

	if (tsam<0) {
		tsam = 0;
	}

	nframes = (tsam + ADPCMFSIZE - 1) >> LFSAMPLES;
	nbytes =  nframes*ADPCMFBYTES;

	if (looped) {
		ptr = _decodeChunk(ptr, f, tsam, nbytes, *outp, inp, f->dc_first);

		/*
		 * Fix up output pointer, which will be used as the input pointer
		 * by the following module.
		 */
		if (f->dc_lastsam) {
			*outp += (f->dc_lastsam<<1);
		} else {
			*outp += (ADPCMFSIZE<<1);
		}

		/*
		 * Now fix up state info to reflect the loop start point
		 */
		f->dc_lastsam = f->dc_loop.start &0xf;
		f->dc_memin = (intptr_t) f->dc_table->base + ADPCMFBYTES * ((s32) (f->dc_loop.start>>LFSAMPLES) + 1);
		f->dc_sample = f->dc_loop.start;

		bEnd = *outp;

		while (outCount > nSam) {
			outCount -= nSam;

			/*
			 * Put next one after the end of the last lot - on the
			 * frame boundary (32 byte) after the end.
			 */
			op = (bEnd + ((nframes+1)<<(LFSAMPLES+1)) + 16) & ~0x1f;

			/*
			 * The actual end of data
			 */
			bEnd += (nSam<<1);

			/*
			 * -1 is loop forever - the loop count is not exact now
			 * for small loops!
			 */
			if (f->dc_loop.count != -1 && f->dc_loop.count != 0) {
				f->dc_loop.count--;
			}

			/*
			 * What's left to compute.
			 */
			nSam = MIN(outCount, f->dc_loop.end - f->dc_loop.start);
			tsam = nSam - ADPCMFSIZE + f->dc_lastsam;

			if (tsam < 0) {
				tsam = 0;
			}

			nframes = (tsam+ADPCMFSIZE - 1) >> LFSAMPLES;
			nbytes =  nframes*ADPCMFBYTES;
			ptr = _decodeChunk(ptr, f, tsam, nbytes, op, inp, f->dc_first | A_LOOP);

			/*
			 * Merge the two sections in DMEM.
			 */
			aDMEMMove(ptr++, op + (f->dc_lastsam << 1), bEnd, nSam << 1);
		}

		f->dc_lastsam = (outCount + f->dc_lastsam) & 0xf;
		f->dc_sample += outCount;
		f->dc_memin += ADPCMFBYTES*nframes;

		return ptr;
	}

	/*
	 * The unlooped case, which is executed most of the time
	 */

	nSam = nframes << LFSAMPLES;

	/*
	 * overFlow is the number of bytes past the end
	 * of the bitstream I try to generate
	 */
	overFlow = f->dc_memin + nbytes - ((intptr_t) f->dc_table->base + f->dc_table->len);

	if (overFlow < 0) {
		overFlow = 0;
	}

	nOver = (overFlow / ADPCMFBYTES) << LFSAMPLES;

	if (nOver > nSam + nLeft) {
		nOver = nSam + nLeft;
	}

	nbytes -= overFlow;

	if (nOver - (nOver & 0xf) < outCount) {
		decoded = 1;
		ptr = _decodeChunk(ptr, f, nSam - nOver, nbytes, *outp, inp, f->dc_first);

		if (f->dc_lastsam) {
			*outp += f->dc_lastsam << 1;
		} else {
			*outp += ADPCMFSIZE << 1;
		}

		f->dc_lastsam = (outCount + f->dc_lastsam) & 0xf;
		f->dc_sample += outCount;
		f->dc_memin += ADPCMFBYTES * nframes;
	} else {
		f->dc_lastsam = 0;
		f->dc_memin += ADPCMFBYTES * nframes;
	}

	/*
	 * Put zeros in if necessary
	 */
	if (nOver) {
		f->dc_lastsam = 0;

		if (decoded) {
			startZero = (nLeft + nSam - nOver) << 1;
		} else {
			startZero = 0;
		}

		aClearBuffer(ptr++, startZero + *outp, nOver << 1);
	}

	return ptr;
}

s32 n_alLoadParam(N_PVoice *filter, s32 paramID, void *param)
{
	N_PVoice *a = filter;

	switch (paramID) {
	case (AL_FILTER_SET_WAVETABLE):
		a->dc_table = (ALWaveTable *) param;
		a->dc_memin = (intptr_t) a->dc_table->base;
		a->dc_sample = 0;

		switch (a->dc_table->type) {
		case (AL_ADPCM_WAVE):
			a->dc_table->len = ADPCMFBYTES * ((s32) (a->dc_table->len/ADPCMFBYTES));

			a->dc_bookSize = 2*a->dc_table->waveInfo.adpcmWave.book->order*
				a->dc_table->waveInfo.adpcmWave.book->npredictors*ADPCMVSIZE;

			if (a->dc_table->waveInfo.adpcmWave.loop) {
				a->dc_loop.start = a->dc_table->waveInfo.adpcmWave.loop->start;
				a->dc_loop.end = a->dc_table->waveInfo.adpcmWave.loop->end;
				a->dc_loop.count = a->dc_table->waveInfo.adpcmWave.loop->count;

				bcopy(a->dc_table->waveInfo.adpcmWave.loop->state, a->dc_lstate, sizeof(ADPCM_STATE));
			} else {
				a->dc_loop.start = a->dc_loop.end = a->dc_loop.count = 0;
			}
			break;
		case (AL_RAW16_WAVE):
			if (a->dc_table->waveInfo.rawWave.loop) {
				a->dc_loop.start = a->dc_table->waveInfo.rawWave.loop->start;
				a->dc_loop.end = a->dc_table->waveInfo.rawWave.loop->end;
				a->dc_loop.count = a->dc_table->waveInfo.rawWave.loop->count;
			} else {
				a->dc_loop.start = a->dc_loop.end = a->dc_loop.count = 0;
			}
			break;
		default:
			break;
		}
		break;
	case (AL_FILTER_RESET):
		a->dc_lastsam = 0;
		a->dc_first   = 1;
		a->dc_sample = 0;

		/* sct 2/14/96 - Check table since it is initialized to null and */
		/* Get loop info according to table type. */
		if (a->dc_table) {
			a->dc_memin  = (intptr_t) a->dc_table->base;

			if (a->dc_table->type == AL_ADPCM_WAVE) {
				if (a->dc_table->waveInfo.adpcmWave.loop) {
					a->dc_loop.count = a->dc_table->waveInfo.adpcmWave.loop->count;
				}
			} else if (a->dc_table->type == AL_RAW16_WAVE) {
				if (a->dc_table->waveInfo.rawWave.loop) {
					a->dc_loop.count = a->dc_table->waveInfo.rawWave.loop->count;
				}
			}
		}
		break;
	default:
		break;
	}

	return 0;
}

Acmd *_decodeChunk(Acmd *ptr, N_PVoice *f, s32 tsam, s32 nbytes, s16 outp, s16 inp, u32 flags)
{
	intptr_t dramAlign, dramLoc;

	if (nbytes > 0) {
		dramLoc = (f->dc_dma)(f->dc_memin, nbytes, f->dc_dmaState);
		/*
		 * Make sure enough is loaded into DMEM to take care
		 * of 8 byte alignment
		 */
		dramAlign = dramLoc & 0x7;
		nbytes += dramAlign;

		n_aLoadBuffer(ptr++, nbytes + 8 - (nbytes & 0x7), inp, dramLoc - dramAlign);
	} else {
		dramAlign = 0;
	}

	if (flags & A_LOOP) {
		aSetLoop(ptr++, K0_TO_PHYS(f->dc_lstate));
	}

	n_aADPCMdec(ptr++, K0_TO_PHYS(f->dc_state), flags, tsam << 1, dramAlign, outp);

	f->dc_first = 0;

	return ptr;
}
