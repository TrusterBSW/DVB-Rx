/*
 * DVB-S2 receiver back half, see rx.h. Single-threaded: every call runs
 * the whole chain on the caller's thread.
 */
#include "dvbrx/rx.h"
#include "dvbrx/bb.h"
#include "dvbrx/demap.h"
#include "dvbrx/ldpc.h"
#include "dvbrx/pl.h"

#include <stdlib.h>
#include <string.h>

#define MAX_LANES 64

/* Decoders for one MODCOD and frame size, created on first use. */
struct codec {
    drx_fec_params fec;
    drx_demap* demap;
    drx_ldpc* ldpc;
    drx_bch* bch;
};

struct drx_rx {
    drx_rx_cfg cfg;
    drx_ts_cb cb;
    void* ctx;
    drx_pl* pl;
    drx_bbdec* bb;
    struct codec* codecs[128]; /* by PLS code >> 1 */

    /* current batch */
    struct codec* batch_codec;
    int batch_n, batch_max;
    int8_t* llr[MAX_LANES];
    uint8_t* cw[MAX_LANES];
    uint8_t* ts;

    drx_rx_stats st;
    uint64_t iter_sum;
};

void drx_rx_cfg_default(drx_rx_cfg* cfg)
{
    cfg->gold_code = 0;
    cfg->max_batch = 0;
    cfg->ldpc_iter = 25;
    cfg->isi = -1;
}

drx_rx* drx_rx_new(const drx_rx_cfg* cfg, drx_ts_cb cb, void* ctx)
{
    drx_rx* rx = calloc(1, sizeof(*rx));
    if (!rx)
        return NULL;
    rx->cfg = *cfg;
    rx->cb = cb;
    rx->ctx = ctx;
    rx->pl = drx_pl_new(cfg->gold_code);
    rx->bb = drx_bbdec_new();
    rx->batch_max = drx_ldpc_lanes();
    if (cfg->max_batch > 0 && cfg->max_batch < rx->batch_max)
        rx->batch_max = cfg->max_batch;
    rx->ts = malloc(58192 / 8 + 188);
    int ok = rx->pl && rx->bb && rx->ts;
    for (int i = 0; i < rx->batch_max && ok; i++) {
        rx->llr[i] = malloc(64800);
        rx->cw[i] = malloc(64800 / 8);
        ok = rx->llr[i] && rx->cw[i];
    }
    if (!ok) {
        drx_rx_free(rx);
        return NULL;
    }
    return rx;
}

static void free_codec(struct codec* c)
{
    if (!c)
        return;
    drx_demap_free(c->demap);
    drx_ldpc_free(c->ldpc);
    drx_bch_free(c->bch);
    free(c);
}

void drx_rx_free(drx_rx* rx)
{
    if (!rx)
        return;
    drx_pl_free(rx->pl);
    drx_bbdec_free(rx->bb);
    for (int m = 0; m < 128; m++)
        free_codec(rx->codecs[m]);
    for (int i = 0; i < MAX_LANES; i++) {
        free(rx->llr[i]);
        free(rx->cw[i]);
    }
    free(rx->ts);
    free(rx);
}

static struct codec* get_codec(drx_rx* rx, const drx_modcod* m)
{
    struct codec** c = &rx->codecs[m->pls >> 1];
    if (*c)
        return *c;
    struct codec* n = calloc(1, sizeof(*n));
    if (!n || drx_fec_lookup(m->short_frame, m->rate, &n->fec)) {
        free(n);
        return NULL;
    }
    n->demap = drx_demap_new(m->constellation, m->rate, m->short_frame);
    n->ldpc = drx_ldpc_new(&n->fec, m->rate);
    n->bch = drx_bch_new(&n->fec);
    if (!n->demap || !n->ldpc || !n->bch) {
        free_codec(n);
        return NULL;
    }
    return *c = n;
}

void drx_rx_flush(drx_rx* rx)
{
    const int n = rx->batch_n;
    struct codec* c = rx->batch_codec;
    if (!n)
        return;
    rx->batch_n = 0;

    int it = drx_ldpc_decode(c->ldpc, (const int8_t* const*)rx->llr, rx->cw, n,
                             rx->cfg.ldpc_iter);
    rx->st.ldpc_batches++;
    if (it < 0) {
        rx->st.ldpc_failed_batches++;
        it = rx->cfg.ldpc_iter;
    }
    rx->iter_sum += it;

    const int kbytes = c->fec.kbch / 8;
    for (int f = 0; f < n; f++) {
        int r = drx_bch_decode(c->bch, rx->cw[f]);
        if (r < 0) {
            rx->st.bch_failed++;
            continue; /* the BBHEADER CRC would most likely reject it anyway */
        }
        rx->st.bch_corrected_bits += r;
        drx_bb_descramble(rx->cw[f], kbytes);
        drx_bbheader h;
        int len = drx_bbdec_frame(rx->bb, rx->cw[f], kbytes, rx->ts, &h);
        if (len > 0 && rx->cfg.isi == DRX_ISI_FIRST)
            rx->cfg.isi = h.isi; /* lock on the first stream */
        if (len > 0 && (rx->cfg.isi == DRX_ISI_ALL || rx->cfg.isi == h.isi))
            rx->cb(rx->ctx, h.isi, rx->ts, len);
    }
}

static void on_plframe(void* ctx, const drx_plframe* info, const float* xfec)
{
    drx_rx* rx = ctx;
    rx->st.plframes++;
    rx->st.pls = info->modcod.pls;
    rx->st.constellation = info->modcod.constellation;
    rx->st.rate = info->modcod.rate;
    rx->st.short_frame = info->modcod.short_frame;
    rx->st.pilots = info->pilots;
    rx->st.snr_db = info->snr_db;
    rx->st.freq = info->freq;

    struct codec* c = get_codec(rx, &info->modcod);
    if (!c)
        return;
    if (rx->batch_n && rx->batch_codec != c)
        drx_rx_flush(rx); /* MODCOD change (VCM/ACM) */
    rx->batch_codec = c;
    drx_demap_frame(c->demap, xfec, info->n0, rx->llr[rx->batch_n++]);
    if (rx->batch_n == rx->batch_max)
        drx_rx_flush(rx);
}

void drx_rx_symbols(drx_rx* rx, const float* sym, int n)
{
    drx_pl_process(rx->pl, sym, n, on_plframe, rx);
}

void drx_rx_get_stats(const drx_rx* rx, drx_rx_stats* s)
{
    *s = rx->st;
    drx_pl_stats ps;
    drx_pl_get_stats(rx->pl, &ps);
    s->locked = ps.locked;
    drx_bbdec_stats bs;
    drx_bbdec_get_stats(rx->bb, &bs);
    s->ts_packets = bs.packets;
    s->ts_crc_errors = bs.crc_errors;
    s->bb_bad_headers = bs.bad_headers;
    s->ldpc_avg_iter = rx->st.ldpc_batches ? (float)rx->iter_sum / rx->st.ldpc_batches : 0;
}
