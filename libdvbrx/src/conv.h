/*
 * Inner code of DVB-S and DVB-T (internal): K = 7 convolutional code,
 * G1 = 171, G2 = 133 (octal), punctured to 1/2 .. 7/8, soft Viterbi decoder.
 */
#ifndef DVBRX_CONV_H
#define DVBRX_CONV_H

#include <math.h>
#include <stdint.h>

#define CONV_TB 128    /* traceback depth */
#define CONV_CHUNK 4096 /* bits decided per traceback */
#define CONV_RING (CONV_CHUNK + CONV_TB)

/* Soft values: int16, positive for a 0 bit, |v| <= CONV_SOFT_MAX, 0 erased. */
#define CONV_SOFT_SCALE 32.0f
#define CONV_SOFT_MAX 127

/* Code rates as in enum drx_dvbs_rate: 1 = 1/2, 2 = 2/3, 3 = 3/4, 4 = 5/6,
 * 5 = 7/8. */
struct conv_punct {
    int k, n;       /* k input bits give n kept coded bits */
    uint8_t isy[8]; /* kept bit is Y (else X) */
    uint8_t idx[8]; /* input bit of the period it belongs to */
};
extern const struct conv_punct CONV_PUNCT[6];

struct conv_depunct {
    const struct conv_punct* p;
    int pos;           /* next kept bit of the period */
    int16_t cur[8][2]; /* soft (X, Y) of the period's input bits, 0 = erased */
};

/* Soft bits in, (X, Y) pairs out; returns the number of pairs. */
int conv_depuncture(struct conv_depunct* dp, const int16_t* soft, int n, int16_t (*pairs)[2]);

static inline int16_t conv_soft(float v)
{
    v *= CONV_SOFT_SCALE;
    return (int16_t)(v > CONV_SOFT_MAX ? CONV_SOFT_MAX : v < -CONV_SOFT_MAX ? -CONV_SOFT_MAX : lrintf(v));
}

/* Viterbi over a whole block; bits gets n decisions, dec n words (bit s:
 * the survivor into state s comes from the odd state). */
void conv_viterbi_block(const int16_t (*pairs)[2], int n, uint8_t* bits, uint64_t* dec);

/* Fraction of the kept coded bits of a block whose hard decision disagrees
 * with the re-encoded Viterbi output, for a puncturing phase (0 .. n - 1)
 * of the code rate; soft holds n_soft punctured soft bits. 1.0 when the
 * block is too short. */
double conv_mismatch(const int16_t* soft, int n_soft, int rate, int phase);

/* Streaming decoder: decisions come out CONV_CHUNK bits at a time. */
struct conv_viterbi {
    int16_t m[64];
    uint64_t* dec; /* CONV_RING steps of decisions, bit s for state s */
    int head, pending;
    uint8_t* out;
};

int conv_viterbi_init(struct conv_viterbi* v);
void conv_viterbi_free(struct conv_viterbi* v);
void conv_viterbi_reset(struct conv_viterbi* v);
/* Feeds n pairs; bits_cb gets the decided bits (one per byte). Stops early
 * when *run becomes zero. */
void conv_viterbi_decode(struct conv_viterbi* v, const int16_t (*pairs)[2], int n,
                         void (*bits_cb)(void* ctx, const uint8_t* bits, int n), void* ctx,
                         const int* run);

#endif
