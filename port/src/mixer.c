#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#include <ultra64.h>

#include "mixer.h"
#include "platform.h"

#define MINIMP3_IMPLEMENTATION
#include "external/minimp3.h"

#ifdef __SSE4_1__
#include <immintrin.h>
#define HAS_SSE41 1
#define HAS_NEON 0
#elif __ARM_NEON
#include <arm_neon.h>
#define HAS_SSE41 0
#define HAS_NEON 1
#else
#define HAS_SSE41 0
#define HAS_NEON 0
#endif

#pragma GCC optimize ("unroll-loops")

#if HAS_SSE41
#define LOADLH(l, h) _mm_castpd_si128(_mm_loadh_pd(_mm_load_sd((const double *)(l)), (const double *)(h)))
#endif

#define ROUND_UP_64(v) (((v) + 63) & ~63)
#define ROUND_UP_32(v) (((v) + 31) & ~31)
#define ROUND_UP_16(v) (((v) + 15) & ~15)
#define ROUND_UP_8(v) (((v) + 7) & ~7)
#define ROUND_DOWN_16(v) ((v) & ~0xf)

#define BUF_SIZE 3072 // 2720 + slack
#define BUF_U8(a) (rspa.buf.as_u8 + (a))
#define BUF_S16(a) (rspa.buf.as_s16 + (a) / sizeof(int16_t))

#define NUM_SAMPLES 0xB8 // MIXER_CHUNK_FRAMES
#define NUM_BYTES 0x170

#define OFS_BASE 0
#define OFS_TEMP1 0x170
#define OFS_MAIN_L 0x4E0
#define OFS_MAIN_R 0x650
#define OFS_AUX_L 0x7C0
#define OFS_AUX_R 0x930

static struct {
    int16_t vol[2];

    int16_t target[2];
    int32_t rate[2];

    int16_t vol_dry;
    int16_t vol_wet;

    ADPCM_STATE *adpcm_loop_state;

    int16_t adpcm_table[8][2][8];

    union {
        int16_t as_s16[BUF_SIZE / sizeof(int16_t)];
        uint8_t as_u8[BUF_SIZE];
    } buf;
} rspa;

/**
 * 5.1 output (mixerSetSurround(), port/src/audio.c decides when), and the
 * headphone surround made from it.
 *
 * In Surround mode the game marks a sound behind the listener with the phase
 * bits aEnvMixerImpl() reads. With six speakers that mark routes the voice
 * instead: its dry share goes to the rear pair, or to the front pair with what
 * both front speakers share taken out for the centre. None of it goes to MAIN,
 * which is left holding only what the reverb returns (n_reverb.c, n_mainbus.c
 * mix it there), and aInterleaveImpl() spreads that to the front and the rear.
 *
 * Every buffer is indexed as DMEM is (i^XOR in aEnvMixerImpl()), so slot k
 * lines up with MAIN's slot k when the chunk is interleaved.
 */
#define SURR_REAR_VALID 0x5e51 // in a voice's saved state: its rearmix is its own
#define SURR_FADE_STEP 37 // 0x7fff over 40 ms at 22020 Hz: front to rear without a click
#define SURR_REVERB_FRONT 0.8f // 0.8^2 + 0.6^2 = 1: the reverb keeps its power
#define SURR_REVERB_REAR 0.6f
#define SURR_SLOTS 16 // chunks mixed and not yet queued: 3 a frame, 3 buffers deep

_Static_assert(NUM_SAMPLES == MIXER_CHUNK_FRAMES, "audio.c walks the output in mixer chunks");

static struct {
    int mode; // MIXER_SURROUND_*
    float lfegain;
    float hproom; // the headphone room's reflections, 0.7 by default (Audio.HeadphoneRoom)

    float front[2][NUM_SAMPLES];
    float centre[NUM_SAMPLES];
    float rear[2][NUM_SAMPLES];
    float stereo[2][NUM_SAMPLES]; // dry as Stereo would mix it, for the recorder

    float lfe[5]; // biquad: b0 b1 b2 a1 a2
    float lfez[2];

    // the chunk aInterleaveImpl() just finished, until aSaveBufferImpl() says where it went
    int pending;
    int16_t chunk[NUM_SAMPLES][MIXER_SURR_COUNT];
} surr;

// The game hands a frame's buffer to the device a frame after mixing it
// (amgrFrame()), so a chunk's other four channels wait here under the address
// its stereo was saved to.
static struct {
    const int16_t *key;
    uint32_t age;
    int16_t data[NUM_SAMPLES][MIXER_SURR_COUNT];
} surrslots[SURR_SLOTS];
static uint32_t surrage;

/**
 * Headphone surround (MIXER_SURROUND_HEADPHONES): the 5.1 mix's five speakers
 * heard through a head, as a stereo pair for a headset.
 *
 * Brown and Duda's structural model (A Structural Model for Binaural Sound
 * Synthesis, 1998), built from its equations, so no measured HRTF ships with
 * the game: each speaker reaches each ear through a head shadow - a one pole,
 * one zero filter that lifts the treble by up to 6 dB on the near side and
 * cuts it by up to 20 dB on the far side, by the angle between the speaker and
 * that ear - and at the delay a sphere the size of a head puts between them
 * (0.26 ms across the head for the front pair at 30 degrees, 0.55 ms for the
 * rear at 110). That places a speaker left or right but cannot tell front from
 * back, which a real ear does with its pinna at frequencies 22 kHz sampling
 * has no room for, so the rear pair also gets what an ear hears from behind:
 * duller treble and a lift around 1 kHz. A small room's first reflections take
 * the sound out of the head; how loud they are is the player's (hproom).
 */
#define HP_SPEAKERS 5
#define HP_HIST 32 // the longest delay to an ear is (1 + pi/2) head radii: 15 samples
#define HP_ER_LEN 512 // 23 ms of reflections
#define HP_ER_TAPS 6
#define HP_GAIN 0.78f // pink noise ahead, to the side and behind within 0.5 dB of Stereo
#define HP_REAR 0.72f // the sphere puts the rear pair 20 degrees off the ear's axis, 3 dB too loud

static const float hpazimuth[HP_SPEAKERS] = { -30.f, 30.f, 0.f, -110.f, 110.f }; // FL FR C RL RR, ITU 5.1
static const int hperdelay[2][HP_ER_TAPS] = {
    { 68, 117, 174, 247, 324, 425 }, // 3.1 to 19.3 ms
    { 81, 134, 189, 273, 350, 458 }, // 3.7 to 20.8 ms: the ears hear different walls
};
static const float hpergain[HP_ER_TAPS] = { 0.22f, -0.18f, 0.15f, 0.12f, -0.10f, 0.08f };

struct biquad {
    float b0, b1, b2, a1, a2;
    float z1, z2;
};

static struct {
    // the head shadow from each speaker to each ear (0 left, 1 right): y = b0 x + b1 x1 - a1 y1
    float b0[HP_SPEAKERS][2], b1[HP_SPEAKERS][2], a1[HP_SPEAKERS][2];
    float x1[HP_SPEAKERS][2], y1[HP_SPEAKERS][2];
    float delay[HP_SPEAKERS][2]; // in samples
    float hist[HP_SPEAKERS][HP_HIST];
    int pos;

    struct biquad rearshelf[2], rearpeak[2]; // RL, RR

    float er[2][HP_ER_LEN];
    int erpos;
    float erlp[2], erlpcoef;
} hp;

// Robert Bristow-Johnson's cookbook, at the mixer's rate
static void biquadPeak(struct biquad *q, float fc, float gaindb, float bw) {
    const float a = powf(10.f, gaindb / 40.f);
    const float w0 = 2.f * (float)M_PI * fc / 22020.f;
    const float alpha = sinf(w0) / (2.f * bw);
    const float a0 = 1.f + alpha / a;

    q->b0 = (1.f + alpha * a) / a0;
    q->b1 = -2.f * cosf(w0) / a0;
    q->b2 = (1.f - alpha * a) / a0;
    q->a1 = q->b1;
    q->a2 = (1.f - alpha / a) / a0;
    q->z1 = q->z2 = 0.f;
}

static void biquadHighShelf(struct biquad *q, float fc, float gaindb) {
    const float a = powf(10.f, gaindb / 40.f);
    const float w0 = 2.f * (float)M_PI * fc / 22020.f;
    const float cosw = cosf(w0);
    const float alpha = sinf(w0) / 2.f * 1.41421356f; // shelf slope 1
    const float sq = 2.f * sqrtf(a) * alpha;
    const float a0 = (a + 1.f) - (a - 1.f) * cosw + sq;

    q->b0 = a * ((a + 1.f) + (a - 1.f) * cosw + sq) / a0;
    q->b1 = -2.f * a * ((a - 1.f) + (a + 1.f) * cosw) / a0;
    q->b2 = a * ((a + 1.f) + (a - 1.f) * cosw - sq) / a0;
    q->a1 = 2.f * ((a - 1.f) - (a + 1.f) * cosw) / a0;
    q->a2 = ((a + 1.f) - (a - 1.f) * cosw - sq) / a0;
    q->z1 = q->z2 = 0.f;
}

static inline float biquadRun(struct biquad *q, float x) {
    const float y = q->b0 * x + q->z1;
    q->z1 = q->b1 * x - q->a1 * y + q->z2;
    q->z2 = q->b2 * x - q->a2 * y;
    return y;
}

static inline float flushDenormal(float v) {
    return fabsf(v) < 1e-15f ? 0.f : v;
}

static void hpInit(void) {
    const float headtime = 0.0875f / 343.f; // head radius over the speed of sound
    const float k = 1.f / tanf((343.f / 0.0875f) / 22020.f); // bilinear, prewarped at the shadow's corner

    memset(&hp, 0, sizeof(hp));

    for (int s = 0; s < HP_SPEAKERS; ++s) {
        for (int e = 0; e < 2; ++e) {
            // the angle between the speaker and this ear, 0 to 180 degrees
            float theta = fabsf(hpazimuth[s] - (e == 0 ? -90.f : 90.f));
            if (theta > 180.f) {
                theta = 360.f - theta;
            }

            // the treble's gain: 2 facing the ear, 0.1 at 150 degrees, a bright spot behind
            const float alpha = 1.05f + 0.95f * cosf(theta / 150.f * (float)M_PI);
            const float rad = theta * (float)M_PI / 180.f;
            const float tau = theta < 90.f ? -headtime * cosf(rad) : headtime * (rad - (float)M_PI / 2.f);

            hp.b0[s][e] = (1.f + alpha * k) / (1.f + k);
            hp.b1[s][e] = (1.f - alpha * k) / (1.f + k);
            hp.a1[s][e] = (1.f - k) / (1.f + k);
            hp.delay[s][e] = (tau + headtime) * 22020.f; // nearest ear facing it: no delay
        }
    }

    for (int i = 0; i < 2; ++i) {
        biquadHighShelf(&hp.rearshelf[i], 2500.f, -5.f);
        biquadPeak(&hp.rearpeak[i], 1000.f, 2.f, 1.f);
    }

    hp.erlpcoef = expf(-2.f * (float)M_PI * 4000.f / 22020.f); // walls soak up the treble
}

// one sample of the five speakers (and the subwoofer) to the two ears
static inline void hpRender(const float in[HP_SPEAKERS], float lfe, float *outl, float *outr) {
    float ear[2] = { 0.f, 0.f };
    float x[HP_SPEAKERS];

    for (int s = 0; s < HP_SPEAKERS; ++s) {
        x[s] = in[s];
    }

    x[3] = biquadRun(&hp.rearpeak[0], biquadRun(&hp.rearshelf[0], x[3] * HP_REAR));
    x[4] = biquadRun(&hp.rearpeak[1], biquadRun(&hp.rearshelf[1], x[4] * HP_REAR));

    hp.pos = (hp.pos + 1) & (HP_HIST - 1);

    for (int s = 0; s < HP_SPEAKERS; ++s) {
        hp.hist[s][hp.pos] = x[s];

        for (int e = 0; e < 2; ++e) {
            const float d = hp.delay[s][e];
            const int di = (int)d;
            const float frac = d - di;
            const float v = hp.hist[s][(hp.pos - di) & (HP_HIST - 1)] * (1.f - frac)
                + hp.hist[s][(hp.pos - di - 1) & (HP_HIST - 1)] * frac;
            const float y = hp.b0[s][e] * v + hp.b1[s][e] * hp.x1[s][e] - hp.a1[s][e] * hp.y1[s][e];

            hp.x1[s][e] = v;
            hp.y1[s][e] = y;
            ear[e] += y;
        }
    }

    // the room: each ear mostly hears its own side's walls
    hp.erpos = (hp.erpos + 1) & (HP_ER_LEN - 1);
    hp.er[0][hp.erpos] = x[0] + x[3] + 0.7f * x[2] + 0.5f * (x[1] + x[4]);
    hp.er[1][hp.erpos] = x[1] + x[4] + 0.7f * x[2] + 0.5f * (x[0] + x[3]);

    for (int e = 0; e < 2; ++e) {
        float r = 0.f;

        for (int t = 0; t < HP_ER_TAPS; ++t) {
            r += hp.er[e][(hp.erpos - hperdelay[e][t]) & (HP_ER_LEN - 1)] * hpergain[t];
        }

        hp.erlp[e] = r * (1.f - hp.erlpcoef) + hp.erlp[e] * hp.erlpcoef;
        ear[e] += hp.erlp[e] * surr.hproom;
    }

    *outl = (ear[0] + lfe) * HP_GAIN;
    *outr = (ear[1] + lfe) * HP_GAIN;
}

static void hpFlushDenormals(void) {
    for (int s = 0; s < HP_SPEAKERS; ++s) {
        for (int e = 0; e < 2; ++e) {
            hp.x1[s][e] = flushDenormal(hp.x1[s][e]);
            hp.y1[s][e] = flushDenormal(hp.y1[s][e]);
        }
    }

    for (int i = 0; i < 2; ++i) {
        hp.rearshelf[i].z1 = flushDenormal(hp.rearshelf[i].z1);
        hp.rearshelf[i].z2 = flushDenormal(hp.rearshelf[i].z2);
        hp.rearpeak[i].z1 = flushDenormal(hp.rearpeak[i].z1);
        hp.rearpeak[i].z2 = flushDenormal(hp.rearpeak[i].z2);
        hp.erlp[i] = flushDenormal(hp.erlp[i]);
    }
}

static int16_t resample_table[64][4] = {
    {0x0c39, 0x66ad, 0x0d46, 0xffdf}, {0x0b39, 0x6696, 0x0e5f, 0xffd8},
    {0x0a44, 0x6669, 0x0f83, 0xffd0}, {0x095a, 0x6626, 0x10b4, 0xffc8},
    {0x087d, 0x65cd, 0x11f0, 0xffbf}, {0x07ab, 0x655e, 0x1338, 0xffb6},
    {0x06e4, 0x64d9, 0x148c, 0xffac}, {0x0628, 0x643f, 0x15eb, 0xffa1},
    {0x0577, 0x638f, 0x1756, 0xff96}, {0x04d1, 0x62cb, 0x18cb, 0xff8a},
    {0x0435, 0x61f3, 0x1a4c, 0xff7e}, {0x03a4, 0x6106, 0x1bd7, 0xff71},
    {0x031c, 0x6007, 0x1d6c, 0xff64}, {0x029f, 0x5ef5, 0x1f0b, 0xff56},
    {0x022a, 0x5dd0, 0x20b3, 0xff48}, {0x01be, 0x5c9a, 0x2264, 0xff3a},
    {0x015b, 0x5b53, 0x241e, 0xff2c}, {0x0101, 0x59fc, 0x25e0, 0xff1e},
    {0x00ae, 0x5896, 0x27a9, 0xff10}, {0x0063, 0x5720, 0x297a, 0xff02},
    {0x001f, 0x559d, 0x2b50, 0xfef4}, {0xffe2, 0x540d, 0x2d2c, 0xfee8},
    {0xffac, 0x5270, 0x2f0d, 0xfedb}, {0xff7c, 0x50c7, 0x30f3, 0xfed0},
    {0xff53, 0x4f14, 0x32dc, 0xfec6}, {0xff2e, 0x4d57, 0x34c8, 0xfebd},
    {0xff0f, 0x4b91, 0x36b6, 0xfeb6}, {0xfef5, 0x49c2, 0x38a5, 0xfeb0},
    {0xfedf, 0x47ed, 0x3a95, 0xfeac}, {0xfece, 0x4611, 0x3c85, 0xfeab},
    {0xfec0, 0x4430, 0x3e74, 0xfeac}, {0xfeb6, 0x424a, 0x4060, 0xfeaf},
    {0xfeaf, 0x4060, 0x424a, 0xfeb6}, {0xfeac, 0x3e74, 0x4430, 0xfec0},
    {0xfeab, 0x3c85, 0x4611, 0xfece}, {0xfeac, 0x3a95, 0x47ed, 0xfedf},
    {0xfeb0, 0x38a5, 0x49c2, 0xfef5}, {0xfeb6, 0x36b6, 0x4b91, 0xff0f},
    {0xfebd, 0x34c8, 0x4d57, 0xff2e}, {0xfec6, 0x32dc, 0x4f14, 0xff53},
    {0xfed0, 0x30f3, 0x50c7, 0xff7c}, {0xfedb, 0x2f0d, 0x5270, 0xffac},
    {0xfee8, 0x2d2c, 0x540d, 0xffe2}, {0xfef4, 0x2b50, 0x559d, 0x001f},
    {0xff02, 0x297a, 0x5720, 0x0063}, {0xff10, 0x27a9, 0x5896, 0x00ae},
    {0xff1e, 0x25e0, 0x59fc, 0x0101}, {0xff2c, 0x241e, 0x5b53, 0x015b},
    {0xff3a, 0x2264, 0x5c9a, 0x01be}, {0xff48, 0x20b3, 0x5dd0, 0x022a},
    {0xff56, 0x1f0b, 0x5ef5, 0x029f}, {0xff64, 0x1d6c, 0x6007, 0x031c},
    {0xff71, 0x1bd7, 0x6106, 0x03a4}, {0xff7e, 0x1a4c, 0x61f3, 0x0435},
    {0xff8a, 0x18cb, 0x62cb, 0x04d1}, {0xff96, 0x1756, 0x638f, 0x0577},
    {0xffa1, 0x15eb, 0x643f, 0x0628}, {0xffac, 0x148c, 0x64d9, 0x06e4},
    {0xffb6, 0x1338, 0x655e, 0x07ab}, {0xffbf, 0x11f0, 0x65cd, 0x087d},
    {0xffc8, 0x10b4, 0x6626, 0x095a}, {0xffd0, 0x0f83, 0x6669, 0x0a44},
    {0xffd8, 0x0e5f, 0x6696, 0x0b39}, {0xffdf, 0x0d46, 0x66ad, 0x0c39}
};

static inline int16_t clamp16(int32_t v) {
    if (v < -0x8000) {
        return -0x8000;
    } else if (v > 0x7fff) {
        return 0x7fff;
    }
    return (int16_t)v;
}

static inline int16_t clampf16(float v) {
    if (v <= -32768.f) {
        return -0x8000;
    } else if (v >= 32767.f) {
        return 0x7fff;
    }
    return (int16_t)lrintf(v);
}

static inline int32_t clamp32(int64_t v) {
    if (v < -0x7fffffff - 1) {
        return -0x7fffffff - 1;
    } else if (v > 0x7fffffff) {
        return 0x7fffffff;
    }
    return (int32_t)v;
}

void aClearBufferImpl(uint16_t addr, int nbytes) {
    nbytes = ROUND_UP_16(nbytes);
    memset(BUF_U8(addr), 0, nbytes);
}

void aLoadBufferImpl(const void *source_addr, uint16_t dest_addr, uint16_t nbytes) {
    memcpy(BUF_U8(dest_addr), source_addr, ROUND_UP_8(nbytes));
}

void aSaveBufferImpl(uint16_t source_addr, int16_t *dest_addr, uint16_t nbytes) {
    memcpy(dest_addr, BUF_S16(source_addr), ROUND_UP_8(nbytes));

    // n_alSavePull(): the chunk just interleaved going to the output buffer
    if (surr.pending && source_addr == OFS_BASE) {
        int slot = 0;

        for (int i = 0; i < SURR_SLOTS; ++i) {
            if (surrslots[i].key == dest_addr) {
                slot = i;
                break;
            }
            if (surrslots[i].age < surrslots[slot].age) {
                slot = i;
            }
        }

        surrslots[slot].key = dest_addr;
        surrslots[slot].age = ++surrage;
        memcpy(surrslots[slot].data, surr.chunk, sizeof(surr.chunk));
        surr.pending = 0;
    }
}

const int16_t *mixerSurroundTake(const int16_t *chunk) {
    for (int i = 0; i < SURR_SLOTS; ++i) {
        if (surrslots[i].key == chunk) {
            surrslots[i].key = NULL;
            surrslots[i].age = 0;
            return &surrslots[i].data[0][0];
        }
    }

    return NULL;
}

void mixerSetSurround(int mode, float lfegain, float hproom) {
    if (mode == MIXER_SURROUND_HEADPHONES && surr.mode != MIXER_SURROUND_HEADPHONES) {
        hpInit();
    }

    if (mode != MIXER_SURROUND_OFF && surr.mode == MIXER_SURROUND_OFF) {
        // a 2nd order Butterworth low pass at 120 Hz for the subwoofer
        const float w0 = 2.f * (float)M_PI * 120.f / 22020.f;
        const float alpha = sinf(w0) / (2.f * 0.70710678f);
        const float cosw = cosf(w0);
        const float a0 = 1.f + alpha;

        surr.lfe[0] = (1.f - cosw) * 0.5f / a0;
        surr.lfe[1] = (1.f - cosw) / a0;
        surr.lfe[2] = surr.lfe[0];
        surr.lfe[3] = -2.f * cosw / a0;
        surr.lfe[4] = (1.f - alpha) / a0;
        surr.lfez[0] = surr.lfez[1] = 0.f;

        memset(surr.front, 0, sizeof(surr.front));
        memset(surr.centre, 0, sizeof(surr.centre));
        memset(surr.rear, 0, sizeof(surr.rear));
        memset(surr.stereo, 0, sizeof(surr.stereo));
    }

    if (mode != MIXER_SURROUND_51) {
        surr.pending = 0;
        memset(surrslots, 0, sizeof(surrslots));
    }

    surr.mode = mode;
    surr.lfegain = lfegain;
    surr.hproom = hproom;
}

void aLoadADPCMImpl(int num_entries_times_16, const int16_t *book_source_addr) {
    // The RSP would read whatever address it was handed; here a null one is a
    // segfault that takes the game with it, and a bank whose wave is not ADPCM
    // has no book to hand over (n_alAdpcmPull() keeps those out, and this is
    // the backstop for any bank that reaches here another way).
    if (!book_source_addr) {
        return;
    }
    memcpy(rspa.adpcm_table, book_source_addr, num_entries_times_16);
}

// 5.1: the front pair goes out the way stereo always has, the other four wait
// in surr.chunk for aSaveBufferImpl() to file them. Headphone surround: all
// six are heard through a head and the ears go out as the stereo pair.
static void surrInterleave(void) {
    const int16_t *revl = BUF_S16(OFS_MAIN_L); // only the reverb is in MAIN in 5.1
    const int16_t *revr = BUF_S16(OFS_MAIN_R);
    int16_t *d = BUF_S16(OFS_BASE);
    const float *b = surr.lfe;
    float z0 = surr.lfez[0], z1 = surr.lfez[1];

    for (int k = 0; k < NUM_SAMPLES; ++k) {
        const float fl = surr.front[0][k] + revl[k] * SURR_REVERB_FRONT;
        const float fr = surr.front[1][k] + revr[k] * SURR_REVERB_FRONT;
        const float c = surr.centre[k];
        const float rl = surr.rear[0][k] + revl[k] * SURR_REVERB_REAR;
        const float rr = surr.rear[1][k] + revr[k] * SURR_REVERB_REAR;

        // transposed direct form II
        const float x = fl + fr + c + rl + rr;
        const float y = b[0] * x + z0;
        z0 = b[1] * x - b[3] * y + z1;
        z1 = b[2] * x - b[4] * y;

        if (surr.mode == MIXER_SURROUND_HEADPHONES) {
            const float speakers[HP_SPEAKERS] = { fl, fr, c, rl, rr };
            float l, r;

            hpRender(speakers, y * surr.lfegain, &l, &r);
            d[k * 2] = clampf16(l);
            d[k * 2 + 1] = clampf16(r);
            continue;
        }

        d[k * 2] = clampf16(fl);
        d[k * 2 + 1] = clampf16(fr);
        surr.chunk[k][MIXER_SURR_C] = clampf16(c);
        surr.chunk[k][MIXER_SURR_LFE] = clampf16(y * surr.lfegain);
        surr.chunk[k][MIXER_SURR_RL] = clampf16(rl);
        surr.chunk[k][MIXER_SURR_RR] = clampf16(rr);
        surr.chunk[k][MIXER_SURR_STEREO_L] = clampf16(surr.stereo[0][k] + revl[k]);
        surr.chunk[k][MIXER_SURR_STEREO_R] = clampf16(surr.stereo[1][k] + revr[k]);
    }

    // a denormal here would cost every sample after it
    surr.lfez[0] = fabsf(z0) < 1e-12f ? 0.f : z0;
    surr.lfez[1] = fabsf(z1) < 1e-12f ? 0.f : z1;

    memset(surr.front, 0, sizeof(surr.front));
    memset(surr.centre, 0, sizeof(surr.centre));
    memset(surr.rear, 0, sizeof(surr.rear));
    memset(surr.stereo, 0, sizeof(surr.stereo));

    if (surr.mode == MIXER_SURROUND_HEADPHONES) {
        hpFlushDenormals();
    } else {
        surr.pending = 1;
    }
}

void aInterleaveImpl(void) {
    if (surr.mode != MIXER_SURROUND_OFF) {
        surrInterleave();
        return;
    }

    const int16_t *l = BUF_S16(OFS_MAIN_L);
    const int16_t *r = BUF_S16(OFS_MAIN_R);
    int count = ROUND_UP_16(NUM_SAMPLES) / 8;
    int16_t *d = BUF_S16(OFS_BASE);
    while (count > 0) {
        int16_t l0 = *l++;
        int16_t l1 = *l++;
        int16_t l2 = *l++;
        int16_t l3 = *l++;
        int16_t l4 = *l++;
        int16_t l5 = *l++;
        int16_t l6 = *l++;
        int16_t l7 = *l++;
        int16_t r0 = *r++;
        int16_t r1 = *r++;
        int16_t r2 = *r++;
        int16_t r3 = *r++;
        int16_t r4 = *r++;
        int16_t r5 = *r++;
        int16_t r6 = *r++;
        int16_t r7 = *r++;
        *d++ = l0;
        *d++ = r0;
        *d++ = l1;
        *d++ = r1;
        *d++ = l2;
        *d++ = r2;
        *d++ = l3;
        *d++ = r3;
        *d++ = l4;
        *d++ = r4;
        *d++ = l5;
        *d++ = r5;
        *d++ = l6;
        *d++ = r6;
        *d++ = l7;
        *d++ = r7;
        --count;
    }
}

void aDMEMMoveImpl(uint16_t in_addr, uint16_t out_addr, int nbytes) {
    memmove(BUF_U8(out_addr), BUF_U8(in_addr), ROUND_UP_16(nbytes));
}

void aSetLoopImpl(ADPCM_STATE *adpcm_loop_state) {
    rspa.adpcm_loop_state = adpcm_loop_state;
}

void aADPCMdecImpl(uint8_t flags, ADPCM_STATE state, int nbytes, uint16_t inofs, uint16_t outofs) {
#if HAS_SSE41
    const __m128i tblrev = _mm_setr_epi8(12, 13, 10, 11, 8, 9, 6, 7, 4, 5, 2, 3, 0, 1, -1, -1);
    const __m128i pos0 = _mm_set_epi8(3, -1, 3, -1, 2, -1, 2, -1, 1, -1, 1, -1, 0, -1, 0, -1);
    const __m128i pos1 = _mm_set_epi8(7, -1, 7, -1, 6, -1, 6, -1, 5, -1, 5, -1, 4, -1, 4, -1);
    const __m128i mult = _mm_set_epi16(0x10, 0x01, 0x10, 0x01, 0x10, 0x01, 0x10, 0x01);
    const __m128i mask = _mm_set1_epi16((int16_t)0xf000);
#elif HAS_NEON
    static const int8_t pos0_data[] = {-1, 0, -1, 0, -1, 1, -1, 1, -1, 2, -1, 2, -1, 3, -1, 3};
    static const int8_t pos1_data[] = {-1, 4, -1, 4, -1, 5, -1, 5, -1, 6, -1, 6, -1, 7, -1, 7};
    static const int16_t mult_data[] = {0x01, 0x10, 0x01, 0x10, 0x01, 0x10, 0x01, 0x10};
    static const int16_t table_prefix_data[] = {0, 0, 0, 0, 0, 0, 0, 1 << 11};
    const int8x16_t pos0 = vld1q_s8(pos0_data);
    const int8x16_t pos1 = vld1q_s8(pos1_data);
    const int16x8_t mult = vld1q_s16(mult_data);
    const int16x8_t mask = vdupq_n_s16((int16_t)0xf000);
    const int16x8_t table_prefix = vld1q_s16(table_prefix_data);
#endif
    uint8_t *in = BUF_U8(inofs);
    int16_t *out = BUF_S16(outofs);
    nbytes = ROUND_UP_32(nbytes);
    if (flags & A_INIT) {
        memset(out, 0, 16 * sizeof(int16_t));
    } else if (flags & A_LOOP) {
        memcpy(out, rspa.adpcm_loop_state, 16 * sizeof(int16_t));
    } else {
        memcpy(out, state, 16 * sizeof(int16_t));
    }
    out += 16;
#if HAS_SSE41
    __m128i prev_interleaved = _mm_set1_epi32((uint16_t)out[-2] | ((uint16_t)out[-1] << 16));
    //__m128i prev_interleaved = _mm_shuffle_epi32(_mm_loadu_si32(out - 2), 0); // GCC misses this?
#elif HAS_NEON
    int16x8_t result = vld1q_s16(out - 8);
#endif
    while (nbytes > 0) {
        int shift = *in >> 4; // should be in 0..12
        int table_index = *in++ & 0xf; // should be in 0..7
        int16_t (*tbl)[8] = rspa.adpcm_table[table_index];
        int i;
#if HAS_SSE41
        // The _mm_loadu_si64 instruction was added in GCC 9, and results in the same
        // asm as the following instructions, so better be compatible with old GCC.
        //__m128i inv = _mm_loadu_si64(in);
        uint64_t v; memcpy(&v, in, 8);
        __m128i inv = _mm_set_epi64x(0, v);
        __m128i invec[2] = {_mm_shuffle_epi8(inv, pos0), _mm_shuffle_epi8(inv, pos1)};
        __m128i tblvec0 = _mm_loadu_si128((const __m128i *)tbl[0]);
        __m128i tblvec1 = _mm_loadu_si128((const __m128i *)(tbl[1]));
        __m128i tbllo = _mm_unpacklo_epi16(tblvec0, tblvec1);
        __m128i tblhi = _mm_unpackhi_epi16(tblvec0, tblvec1);
        __m128i shiftcount = _mm_set_epi64x(0, 12 - shift); // _mm_cvtsi64_si128 does not exist on 32-bit x86
        __m128i tblvec1_rev[8];

        tblvec1_rev[0] = _mm_insert_epi16(_mm_shuffle_epi8(tblvec1, tblrev), 1 << 11, 7);
        tblvec1_rev[1] = _mm_bsrli_si128(tblvec1_rev[0], 2);
        tblvec1_rev[2] = _mm_bsrli_si128(tblvec1_rev[0], 4);
        tblvec1_rev[3] = _mm_bsrli_si128(tblvec1_rev[0], 6);
        tblvec1_rev[4] = _mm_bsrli_si128(tblvec1_rev[0], 8);
        tblvec1_rev[5] = _mm_bsrli_si128(tblvec1_rev[0], 10);
        tblvec1_rev[6] = _mm_bsrli_si128(tblvec1_rev[0], 12);
        tblvec1_rev[7] = _mm_bsrli_si128(tblvec1_rev[0], 14);
        in += 8;
        for (i = 0; i < 2; i++) {
            __m128i acc0 = _mm_madd_epi16(prev_interleaved, tbllo);
            __m128i acc1 = _mm_madd_epi16(prev_interleaved, tblhi);
            __m128i muls[8];
            __m128i result;
            invec[i] = _mm_sra_epi16(_mm_and_si128(_mm_mullo_epi16(invec[i], mult), mask), shiftcount);

            muls[7] = _mm_madd_epi16(tblvec1_rev[0], invec[i]);
            muls[6] = _mm_madd_epi16(tblvec1_rev[1], invec[i]);
            muls[5] = _mm_madd_epi16(tblvec1_rev[2], invec[i]);
            muls[4] = _mm_madd_epi16(tblvec1_rev[3], invec[i]);
            muls[3] = _mm_madd_epi16(tblvec1_rev[4], invec[i]);
            muls[2] = _mm_madd_epi16(tblvec1_rev[5], invec[i]);
            muls[1] = _mm_madd_epi16(tblvec1_rev[6], invec[i]);
            muls[0] = _mm_madd_epi16(tblvec1_rev[7], invec[i]);

            acc0 = _mm_add_epi32(acc0, _mm_hadd_epi32(_mm_hadd_epi32(muls[0], muls[1]), _mm_hadd_epi32(muls[2], muls[3])));
            acc1 = _mm_add_epi32(acc1, _mm_hadd_epi32(_mm_hadd_epi32(muls[4], muls[5]), _mm_hadd_epi32(muls[6], muls[7])));

            acc0 = _mm_srai_epi32(acc0, 11);
            acc1 = _mm_srai_epi32(acc1, 11);

            result = _mm_packs_epi32(acc0, acc1);
            _mm_storeu_si128((__m128i *)out, result);
            out += 8;

            prev_interleaved = _mm_shuffle_epi32(result, _MM_SHUFFLE(3, 3, 3, 3));
        }
#elif HAS_NEON
        int8x8_t inv = vld1_s8((int8_t *)in);
        int16x8_t tblvec[2] = {vld1q_s16(tbl[0]), vld1q_s16(tbl[1])};
        int16x8_t invec[2] = {vreinterpretq_s16_s8(vcombine_s8(vtbl1_s8(inv, vget_low_s8(pos0)),
                                                               vtbl1_s8(inv, vget_high_s8(pos0)))),
                              vreinterpretq_s16_s8(vcombine_s8(vtbl1_s8(inv, vget_low_s8(pos1)),
                                                               vtbl1_s8(inv, vget_high_s8(pos1))))};
        int16x8_t shiftcount = vdupq_n_s16(shift - 12); // negative means right shift
        int16x8_t tblvec1[8];

        in += 8;
        tblvec1[0] = vextq_s16(table_prefix, tblvec[1], 7);
        invec[0] = vmulq_s16(invec[0], mult);
        tblvec1[1] = vextq_s16(table_prefix, tblvec[1], 6);
        invec[1] = vmulq_s16(invec[1], mult);
        tblvec1[2] = vextq_s16(table_prefix, tblvec[1], 5);
        tblvec1[3] = vextq_s16(table_prefix, tblvec[1], 4);
        invec[0] = vandq_s16(invec[0], mask);
        tblvec1[4] = vextq_s16(table_prefix, tblvec[1], 3);
        invec[1] = vandq_s16(invec[1], mask);
        tblvec1[5] = vextq_s16(table_prefix, tblvec[1], 2);
        tblvec1[6] = vextq_s16(table_prefix, tblvec[1], 1);
        invec[0] = vqshlq_s16(invec[0], shiftcount);
        invec[1] = vqshlq_s16(invec[1], shiftcount);
        tblvec1[7] = table_prefix;
        for (i = 0; i < 2; i++) {
            int32x4_t acc0;
            int32x4_t acc1;

            acc1 = vmull_lane_s16(vget_high_s16(tblvec[0]), vget_high_s16(result), 2);
            acc1 = vmlal_lane_s16(acc1, vget_high_s16(tblvec[1]), vget_high_s16(result), 3);
            acc0 = vmull_lane_s16(vget_low_s16(tblvec[0]), vget_high_s16(result), 2);
            acc0 = vmlal_lane_s16(acc0, vget_low_s16(tblvec[1]), vget_high_s16(result), 3);

            acc0 = vmlal_lane_s16(acc0, vget_low_s16(tblvec1[0]), vget_low_s16(invec[i]), 0);
            acc0 = vmlal_lane_s16(acc0, vget_low_s16(tblvec1[1]), vget_low_s16(invec[i]), 1);
            acc0 = vmlal_lane_s16(acc0, vget_low_s16(tblvec1[2]), vget_low_s16(invec[i]), 2);
            acc0 = vmlal_lane_s16(acc0, vget_low_s16(tblvec1[3]), vget_low_s16(invec[i]), 3);

            acc1 = vmlal_lane_s16(acc1, vget_high_s16(tblvec1[0]), vget_low_s16(invec[i]), 0);
            acc1 = vmlal_lane_s16(acc1, vget_high_s16(tblvec1[1]), vget_low_s16(invec[i]), 1);
            acc1 = vmlal_lane_s16(acc1, vget_high_s16(tblvec1[2]), vget_low_s16(invec[i]), 2);
            acc1 = vmlal_lane_s16(acc1, vget_high_s16(tblvec1[3]), vget_low_s16(invec[i]), 3);
            acc1 = vmlal_lane_s16(acc1, vget_high_s16(tblvec1[4]), vget_high_s16(invec[i]), 0);
            acc1 = vmlal_lane_s16(acc1, vget_high_s16(tblvec1[5]), vget_high_s16(invec[i]), 1);
            acc1 = vmlal_lane_s16(acc1, vget_high_s16(tblvec1[6]), vget_high_s16(invec[i]), 2);
            acc1 = vmlal_lane_s16(acc1, vget_high_s16(tblvec1[7]), vget_high_s16(invec[i]), 3);

            result = vcombine_s16(vqshrn_n_s32(acc0, 11), vqshrn_n_s32(acc1, 11));
            vst1q_s16(out, result);
            out += 8;
        }
#else
        for (i = 0; i < 2; i++) {
            int16_t ins[8];
            int16_t prev1 = out[-1];
            int16_t prev2 = out[-2];
            int j, k;
            for (j = 0; j < 4; j++) {
                ins[j * 2] = (((*in >> 4) << 28) >> 28) << shift;
                ins[j * 2 + 1] = (((*in++ & 0xf) << 28) >> 28) << shift;
            }
            for (j = 0; j < 8; j++) {
                int32_t acc = tbl[0][j] * prev2 + tbl[1][j] * prev1 + (ins[j] << 11);
                for (k = 0; k < j; k++) {
                    acc += tbl[1][((j - k) - 1)] * ins[k];
                }
                acc >>= 11;
                *out++ = clamp16(acc);
            }
        }
#endif
        nbytes -= 16 * sizeof(int16_t);
    }
    memcpy(state, out - 16, 16 * sizeof(int16_t));
}

void aResampleImpl(uint8_t flags, uint16_t pitch, RESAMPLE_STATE state, uint16_t inofs, uint8_t outflag) {
    int16_t tmp[16];
    int16_t *in_initial = BUF_S16(inofs);
    int16_t *in = in_initial;
    int16_t *out = BUF_S16(OFS_BASE + ((outflag & 3) != 0) * NUM_BYTES);
    int nbytes = ROUND_UP_16(NUM_BYTES);
    uint32_t pitch_accumulator;
    int i;
#if !HAS_SSE41 && !HAS_NEON
    int16_t *tbl;
    int32_t sample;
#endif
    if (flags & A_INIT) {
        memset(tmp, 0, 5 * sizeof(int16_t));
    } else {
        memcpy(tmp, state, 16 * sizeof(int16_t));
    }
    if (flags & 2) {
        memcpy(in - 8, tmp + 8, 8 * sizeof(int16_t));
        in -= tmp[5] / sizeof(int16_t);
    }
    in -= 4;
    pitch_accumulator = (uint16_t)tmp[4];
    memcpy(in, tmp, 4 * sizeof(int16_t));

#if HAS_SSE41
    __m128i multiples = _mm_setr_epi16(0, 2, 4, 6, 8, 10, 12, 14);
    __m128i pitchvec = _mm_set1_epi16((int16_t)pitch);
    __m128i pitchvec_8_steps = _mm_set1_epi32((pitch << 1) * 8);
    __m128i pitchacclo_vec = _mm_set1_epi32((uint16_t)pitch_accumulator);
    __m128i pl = _mm_mullo_epi16(multiples, pitchvec);
    __m128i ph = _mm_mulhi_epu16(multiples, pitchvec);
    __m128i acc_a = _mm_add_epi32(_mm_unpacklo_epi16(pl, ph), pitchacclo_vec);
    __m128i acc_b = _mm_add_epi32(_mm_unpackhi_epi16(pl, ph), pitchacclo_vec);

    do {
        __m128i tbl_positions = _mm_srli_epi16(_mm_packus_epi32(
            _mm_and_si128(acc_a, _mm_set1_epi32(0xffff)),
            _mm_and_si128(acc_b, _mm_set1_epi32(0xffff))), 10);

        __m128i in_positions = _mm_packus_epi32(_mm_srli_epi32(acc_a, 16), _mm_srli_epi32(acc_b, 16));
        __m128i tbl_entries[4];
        __m128i samples[4];

        /*for (i = 0; i < 4; i++) {
            tbl_entries[i] = _mm_castpd_si128(_mm_loadh_pd(_mm_load_sd(
                (const double *)resample_table[_mm_extract_epi16(tbl_positions, 2 * i)]),
                (const double *)resample_table[_mm_extract_epi16(tbl_positions, 2 * i + 1)]));
            samples[i] = _mm_castpd_si128(_mm_loadh_pd(_mm_load_sd(
                (const double *)&in[_mm_extract_epi16(in_positions, 2 * i)]),
                (const double *)&in[_mm_extract_epi16(in_positions, 2 * i + 1)]));
            samples[i] = _mm_mulhrs_epi16(samples[i], tbl_entries[i]);
        }*/
        tbl_entries[0] = LOADLH(resample_table[_mm_extract_epi16(tbl_positions, 0)], resample_table[_mm_extract_epi16(tbl_positions, 1)]);
        tbl_entries[1] = LOADLH(resample_table[_mm_extract_epi16(tbl_positions, 2)], resample_table[_mm_extract_epi16(tbl_positions, 3)]);
        tbl_entries[2] = LOADLH(resample_table[_mm_extract_epi16(tbl_positions, 4)], resample_table[_mm_extract_epi16(tbl_positions, 5)]);
        tbl_entries[3] = LOADLH(resample_table[_mm_extract_epi16(tbl_positions, 6)], resample_table[_mm_extract_epi16(tbl_positions, 7)]);
        samples[0] = LOADLH(&in[_mm_extract_epi16(in_positions, 0)], &in[_mm_extract_epi16(in_positions, 1)]);
        samples[1] = LOADLH(&in[_mm_extract_epi16(in_positions, 2)], &in[_mm_extract_epi16(in_positions, 3)]);
        samples[2] = LOADLH(&in[_mm_extract_epi16(in_positions, 4)], &in[_mm_extract_epi16(in_positions, 5)]);
        samples[3] = LOADLH(&in[_mm_extract_epi16(in_positions, 6)], &in[_mm_extract_epi16(in_positions, 7)]);
        samples[0] = _mm_mulhrs_epi16(samples[0], tbl_entries[0]);
        samples[1] = _mm_mulhrs_epi16(samples[1], tbl_entries[1]);
        samples[2] = _mm_mulhrs_epi16(samples[2], tbl_entries[2]);
        samples[3] = _mm_mulhrs_epi16(samples[3], tbl_entries[3]);

        _mm_storeu_si128((__m128i *)out, _mm_hadds_epi16(_mm_hadds_epi16(samples[0], samples[1]), _mm_hadds_epi16(samples[2], samples[3])));

        acc_a = _mm_add_epi32(acc_a, pitchvec_8_steps);
        acc_b = _mm_add_epi32(acc_b, pitchvec_8_steps);
        out += 8;
        nbytes -= 8 * sizeof(int16_t);
    } while (nbytes > 0);
    in += (uint16_t)_mm_extract_epi16(acc_a, 1);
    pitch_accumulator = (uint16_t)_mm_extract_epi16(acc_a, 0);
#elif HAS_NEON
    static const uint16_t multiples_data[8] = {0, 2, 4, 6, 8, 10, 12, 14};
    uint16x8_t multiples = vld1q_u16(multiples_data);
    uint32x4_t pitchvec_8_steps = vdupq_n_u32((pitch << 1) * 8);
    uint32x4_t pitchacclo_vec = vdupq_n_u32((uint16_t)pitch_accumulator);
    uint32x4_t acc_a = vmlal_n_u16(pitchacclo_vec, vget_low_u16(multiples), pitch);
    uint32x4_t acc_b = vmlal_n_u16(pitchacclo_vec, vget_high_u16(multiples), pitch);

    do {
        uint16x8x2_t unzipped = vuzpq_u16(vreinterpretq_u16_u32(acc_a), vreinterpretq_u16_u32(acc_b));
        uint16x8_t tbl_positions = vshrq_n_u16(unzipped.val[0], 10);
        uint16x8_t in_positions = unzipped.val[1];
        int16x8_t tbl_entries[4];
        int16x8_t samples[4];
        int16x8x2_t unzipped1;
        int16x8x2_t unzipped2;

        tbl_entries[0] = vcombine_s16(vld1_s16(resample_table[vgetq_lane_u16(tbl_positions, 0)]), vld1_s16(resample_table[vgetq_lane_u16(tbl_positions, 1)]));
        tbl_entries[1] = vcombine_s16(vld1_s16(resample_table[vgetq_lane_u16(tbl_positions, 2)]), vld1_s16(resample_table[vgetq_lane_u16(tbl_positions, 3)]));
        tbl_entries[2] = vcombine_s16(vld1_s16(resample_table[vgetq_lane_u16(tbl_positions, 4)]), vld1_s16(resample_table[vgetq_lane_u16(tbl_positions, 5)]));
        tbl_entries[3] = vcombine_s16(vld1_s16(resample_table[vgetq_lane_u16(tbl_positions, 6)]), vld1_s16(resample_table[vgetq_lane_u16(tbl_positions, 7)]));
        samples[0] = vcombine_s16(vld1_s16(&in[vgetq_lane_u16(in_positions, 0)]), vld1_s16(&in[vgetq_lane_u16(in_positions, 1)]));
        samples[1] = vcombine_s16(vld1_s16(&in[vgetq_lane_u16(in_positions, 2)]), vld1_s16(&in[vgetq_lane_u16(in_positions, 3)]));
        samples[2] = vcombine_s16(vld1_s16(&in[vgetq_lane_u16(in_positions, 4)]), vld1_s16(&in[vgetq_lane_u16(in_positions, 5)]));
        samples[3] = vcombine_s16(vld1_s16(&in[vgetq_lane_u16(in_positions, 6)]), vld1_s16(&in[vgetq_lane_u16(in_positions, 7)]));
        samples[0] = vqrdmulhq_s16(samples[0], tbl_entries[0]);
        samples[1] = vqrdmulhq_s16(samples[1], tbl_entries[1]);
        samples[2] = vqrdmulhq_s16(samples[2], tbl_entries[2]);
        samples[3] = vqrdmulhq_s16(samples[3], tbl_entries[3]);

        unzipped1 = vuzpq_s16(samples[0], samples[1]);
        unzipped2 = vuzpq_s16(samples[2], samples[3]);
        samples[0] = vqaddq_s16(unzipped1.val[0], unzipped1.val[1]);
        samples[1] = vqaddq_s16(unzipped2.val[0], unzipped2.val[1]);
        unzipped1 = vuzpq_s16(samples[0], samples[1]);
        samples[0] = vqaddq_s16(unzipped1.val[0], unzipped1.val[1]);

        vst1q_s16(out, samples[0]);

        acc_a = vaddq_u32(acc_a, pitchvec_8_steps);
        acc_b = vaddq_u32(acc_b, pitchvec_8_steps);
        out += 8;
        nbytes -= 8 * sizeof(int16_t);
    } while (nbytes > 0);
    in += vgetq_lane_u16(vreinterpretq_u16_u32(acc_a), 1);
    pitch_accumulator = vgetq_lane_u16(vreinterpretq_u16_u32(acc_a), 0);
#else
    do {
        for (i = 0; i < 8; i++) {
            tbl = resample_table[pitch_accumulator * 64 >> 16];
            sample = ((in[0] * tbl[0] + 0x4000) >> 15) +
                     ((in[1] * tbl[1] + 0x4000) >> 15) +
                     ((in[2] * tbl[2] + 0x4000) >> 15) +
                     ((in[3] * tbl[3] + 0x4000) >> 15);
            *out++ = clamp16(sample);

            pitch_accumulator += (pitch << 1);
            in += pitch_accumulator >> 16;
            pitch_accumulator %= 0x10000;
        }
        nbytes -= 8 * sizeof(int16_t);
    } while (nbytes > 0);
#endif

    state[4] = (int16_t)pitch_accumulator;
    memcpy(state, in, 4 * sizeof(int16_t));
    i = (in - in_initial + 4) & 7;
    in -= i;
    if (i != 0) {
        i = -8 - i;
    }
    state[5] = i;
    memcpy(state + 8, in, 8 * sizeof(int16_t));
}

void aEnvMixerImpl(uint8_t flags, ENVMIX_STATE state, int16_t rvol) {
    int16_t *in = BUF_S16(OFS_BASE);
    int16_t *dry[2] = {BUF_S16(OFS_MAIN_L), BUF_S16(OFS_MAIN_R)};
    int16_t *wet[2] = {BUF_S16(OFS_AUX_L), BUF_S16(OFS_AUX_R)};
    int nsamples = NUM_SAMPLES;

    struct {
        int32_t t[2];
        int32_t rate[2];
        int16_t tgt[2];
        int16_t voldry;
        int16_t volwet;
        // port, 5.1 only: how far the voice is through its move between the
        // front pair and the rear, 0 to 0x7fff, while rearvalid is
        // SURR_REAR_VALID (n_env.c clears the state when a voice starts a sound)
        int16_t rearmix;
        uint16_t rearvalid;
    } *savedstate = (void *)state;

    rspa.vol[1] = rvol; // why the fuck is this here?

    // naudio uses a linear envelope
    // TODO: sse/neon

    int32_t t[2], tgt[2], rate[2];
    int16_t voldry, volwet;

    if (flags & A_INIT) {
        for (int i = 0; i < 2; ++i) {
           t[i] = rspa.vol[i] << 16;
            rate[i] = rspa.rate[i] >> 3;
            tgt[i] = rspa.target[i] << 16;
        }
        voldry = rspa.vol_dry;
        volwet = rspa.vol_wet;
    } else {
        for (int i = 0; i < 2; ++i) {
            t[i] = savedstate->t[i];
            rate[i] = savedstate->rate[i];
            tgt[i] = savedstate->tgt[i] << 16;
        }
        voldry = savedstate->voldry;
        volwet = savedstate->volwet;
    }

    // The low bits of the dry and wet amounts are the microcode's phase flags
    // (asp.s, cmd_ENVMIXER): dry's inverts everything the voice puts on the
    // left, wet's everything it puts on the right. Only Surround mode sets
    // them (n_env.c, mp3.c), for a sound behind the listener, so that sound
    // leaves in antiphase: Dolby Surround's matrix, which a Pro Logic decoder
    // steers to the rear speakers. The RSP inverts with an xor, not a negate.
    const int16_t invl = -(voldry & 1);
    const int16_t invr = -(volwet & 1);

    // In 5.1 the same mark sends the voice to the rear pair, and a voice that
    // changes sides fades across rather than jumping
    const int32_t reartgt = ((voldry ^ volwet) & 1) ? 0x7fff : 0;
    int32_t rearmix = savedstate->rearvalid == SURR_REAR_VALID ? savedstate->rearmix : reartgt;

    #ifdef PLATFORM_BIG_ENDIAN
    #define XOR 0
    #else
    #define XOR 1
    #endif

    for (int i = 0; i < nsamples; ++i) {
        int16_t gain[4];
        int16_t vol[2];
        int16_t *outptr[4];

        for (int j = 0; j < 2; ++j) {
            t[j] += rate[j];
            if ((rate[j] <= 0 && t[j] <= tgt[j]) || (rate[j] > 0 && t[j] >= tgt[j])) {
                t[j] = tgt[j];
                rate[j] = 0;
            }
            vol[j] = t[j] >> 16;
        }

        outptr[0] = dry[0] + (i^XOR);
        outptr[1] = dry[1] + (i^XOR);
        outptr[2] = wet[0] + (i^XOR);
        outptr[3] = wet[1] + (i^XOR);

        gain[0] = clamp16((vol[0] * voldry + 0x4000) >> 15);
        gain[1] = clamp16((vol[1] * voldry + 0x4000) >> 15);
        gain[2] = clamp16((vol[0] * volwet + 0x4000) >> 15);
        gain[3] = clamp16((vol[1] * volwet + 0x4000) >> 15);

        const int16_t insamp = in[i^XOR];

        if (surr.mode == MIXER_SURROUND_OFF) {
            const int16_t inl = insamp ^ invl;
            const int16_t inr = insamp ^ invr;
            *outptr[0] = clamp16(*outptr[0] + ((inl * gain[0]) >> 15));
            *outptr[1] = clamp16(*outptr[1] + ((inr * gain[1]) >> 15));
            *outptr[2] = clamp16(*outptr[2] + ((inl * gain[2]) >> 15));
            *outptr[3] = clamp16(*outptr[3] + ((inr * gain[3]) >> 15));
            continue;
        }

        // 5.1: the reverb send as Stereo would make it, in phase
        *outptr[2] = clamp16(*outptr[2] + ((insamp * gain[2]) >> 15));
        *outptr[3] = clamp16(*outptr[3] + ((insamp * gain[3]) >> 15));

        if (rearmix < reartgt) {
            rearmix = rearmix + SURR_FADE_STEP < reartgt ? rearmix + SURR_FADE_STEP : reartgt;
        } else if (rearmix > reartgt) {
            rearmix = rearmix - SURR_FADE_STEP > reartgt ? rearmix - SURR_FADE_STEP : reartgt;
        }

        const int k = i ^ XOR;
        const float x = insamp * (1.f / 32768.f);
        const float gl = gain[0] > 0 ? gain[0] : 0;
        const float gr = gain[1] > 0 ? gain[1] : 0;

        surr.stereo[0][k] += x * gl;
        surr.stereo[1][k] += x * gr;

        // equal power across the fade
        const float wr = rearmix == 0 ? 0.f : rearmix == 0x7fff ? 1.f : sqrtf(rearmix * (1.f / 0x7fff));
        const float wf = rearmix == 0 ? 1.f : rearmix == 0x7fff ? 0.f : sqrtf(1.f - rearmix * (1.f / 0x7fff));

        if (wr > 0.f) {
            surr.rear[0][k] += x * gl * wr;
            surr.rear[1][k] += x * gr * wr;
        }

        if (wf > 0.f) {
            // The game pans with an equal power pair (n_eqpower), so what both
            // speakers share is a phantom centre. That share goes to the
            // centre speaker, and the three are scaled back up to the pair's
            // power: dead ahead is all centre, hard left all left.
            const float m = gl < gr ? gl : gr;
            const float v2 = gl * gl + gr * gr;
            const float p = 2.f * v2 - 2.f * gl * gr - fabsf(gl * gl - gr * gr);
            const float n = p > 0.f ? sqrtf(v2 / p) * wf : wf;

            surr.front[0][k] += x * (gl - m) * n;
            surr.front[1][k] += x * (gr - m) * n;
            surr.centre[k] += x * m * n * 1.41421356f;
        }
    }

    #undef XOR

    for (int i = 0; i < 2; ++i) {
        savedstate->t[i] = t[i];
        savedstate->rate[i] = rate[i];
        savedstate->tgt[i] = tgt[i] >> 16;
    }
    savedstate->voldry = voldry;
    savedstate->volwet = volwet;
    savedstate->rearmix = rearmix;
    savedstate->rearvalid = surr.mode != MIXER_SURROUND_OFF ? SURR_REAR_VALID : 0;
}

// flags is always 0 in PD
void aMixImpl(uint8_t flags, int16_t gain, uint16_t in_addr, uint16_t out_addr) {
    int nbytes = ROUND_UP_16(NUM_BYTES); // can't round up to 32 here because 0x170 ain't gonna do that
    int16_t *in = BUF_S16(in_addr);
    int16_t *out = BUF_S16(out_addr);
#if HAS_SSE41
    __m128i gain_vec = _mm_set1_epi16(gain);
#elif !HAS_NEON
    int i;
    int32_t sample;
#endif

#if !HAS_NEON
    if (gain == -0x8000) {
        while (nbytes > 0) {
#if HAS_SSE41
            __m128i out1, in1;
            out1 = _mm_loadu_si128((const __m128i *)out);
            in1 = _mm_loadu_si128((const __m128i *)in);

            out1 = _mm_subs_epi16(out1, in1);

            _mm_storeu_si128((__m128i *)out, out1);

            out += 8;
            in += 8;
#else
            for (i = 0; i < 8; i++) {
                sample = *out - *in++;
                *out++ = clamp16(sample);
            }
#endif

            nbytes -= 8 * sizeof(int16_t);
        }
    }
#endif

    while (nbytes > 0) {
#if HAS_SSE41
        __m128i out1, in1;
        out1 = _mm_loadu_si128((const __m128i *)out);
        in1 = _mm_loadu_si128((const __m128i *)in);

        out1 = _mm_adds_epi16(out1, _mm_mulhrs_epi16(in1, gain_vec));

        _mm_storeu_si128((__m128i *)out, out1);

        out += 8;
        in += 8;
#elif HAS_NEON
        int16x8_t out1, in1;
        out1 = vld1q_s16(out);
        in1 = vld1q_s16(in);

        out1 = vqaddq_s16(out1, vqrdmulhq_n_s16(in1, gain));

        vst1q_s16(out, out1);

        out += 8;
        in += 8;
#else
        for (i = 0; i < 8; i++) {
            sample = ((*out * 0x7fff + *in++ * gain) + 0x4000) >> 15;
            *out++ = clamp16(sample);
        }
#endif

        nbytes -= 8 * sizeof(int16_t);
    }
}

void aSetVolumeImpl(uint8_t flags, int16_t v, int16_t t, int16_t r) {
    // the flags are not really very intuitive here
    if (flags & A_VOL) {
        if (flags & A_LEFT) {
            rspa.vol[0] = v;
            rspa.vol_dry = t;
            rspa.vol_wet = r;
        } else {
            rspa.target[1] = v;
            rspa.rate[1] = (int32_t)((uint16_t)t << 16 | ((uint16_t)r));
        }
    } else /* A_RATE */ {
        rspa.target[0] = v;
        rspa.rate[0] = (int32_t)((uint16_t)t << 16 | ((uint16_t)r));
    }
}

void aPlayMP3Impl(const void *mp3file, u32 mp3size, void *out, int reset) {
    static mp3dec_t mp3d;
    static const u8 *curdata = NULL; // pointer to the mp3 we're currently processing
    static s32 dataptr = 0; // byte index into curdata

    if (mp3file != curdata || reset) {
        // new mp3, reinit decoder
        mp3dec_init(&mp3d);
        curdata = mp3file;
        dataptr = 0;
    }

    // this command is supposed to write one full frame to out
    // but which frame? we'll just decode sequentially, it'll probably work
    if (dataptr < mp3size) {
        // FIXME: decoding straight to out might bite us in the ass because it's only 1160 bytes
        mp3dec_frame_info_t info;
        const s32 samples = mp3dec_decode_frame(&mp3d, curdata + dataptr, mp3size - dataptr, out, &info);
        // fill in the rest of the buffer if frame is smaller
        const s32 diff = 580 - samples;
        if (diff > 0) {
            memset((s16 *)out + samples, 0, diff * 2);
        } else {
            assert(diff == 0);
        }
        dataptr += info.frame_bytes;
    } else {
        // empty frame
        memset(out, 0, 580 * 2);
    }
}

void aPoleFilterImpl(uint8_t flags, int16_t gain, uint32_t t, uint32_t addr) {
    // this never gets called?
}

void aDisableImpl(uint16_t outp, uint32_t b, uint32_t c) {
    // this never gets called?
}
