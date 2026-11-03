/*
 * DVB-S2 / S2X LDPC decoder: layered offset min-sum on 8-bit LLRs, decoding
 * several frames at once (one per SIMD byte lane).
 */
#ifndef DVBRX_LDPC_H
#define DVBRX_LDPC_H

#include "dvbrx/fec.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct drx_ldpc drx_ldpc;

/* Number of frames decoded together (32 with AVX2, 64 with AVX-512BW). */
int drx_ldpc_lanes(void);

/* rate uses the same naming as drx_fec_lookup(). */
drx_ldpc* drx_ldpc_new(const drx_fec_params* p, const char* rate);
void drx_ldpc_free(drx_ldpc* d);

/*
 * Decode nframes (<= drx_ldpc_lanes()) codewords.
 *
 * llr[f] holds the nldpc LLRs of frame f in transmission order, positive
 * for a 0 bit. out[f] receives the kldpc information bits (the BCH
 * codeword), packed MSB first. Returns the number of iterations run, or a
 * negative value if some frame still violated a parity check after
 * max_iter iterations.
 */
int drx_ldpc_decode(drx_ldpc* d,
                    const int8_t* const* llr,
                    uint8_t* const* out,
                    int nframes,
                    int max_iter);

#ifdef __cplusplus
}
#endif

#endif /* DVBRX_LDPC_H */
