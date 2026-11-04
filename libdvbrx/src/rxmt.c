/*
 * Multithreaded DVB-S2 receiver, see rxmt.h.
 */
#include "dvbrx/rxmt.h"
#include "dvbrx/bb.h"
#include "dvbrx/demap.h"
#include "dvbrx/dvbc.h"
#include "dvbrx/dvbs.h"
#include "dvbrx/dvbt.h"
#include "dvbrx/dvbt2.h"
#include "dvbrx/ldpc.h"
#include "dvbrx/pl.h"
#include "tap.h"
#include "queue.h"

#include <stdio.h>
#include <math.h>
#include <string.h>

#define SBLK 65536     /* samples (or symbols) per block */
#define NSBLK 128      /* sample blocks in flight: ~140 ms at 60 Msps */
#define NYBLK 16       /* symbol blocks in flight */
#define MAX_LANES 64
#define MAX_WORKERS 16

struct sblock {
    int n;
    int gap;      /* input samples were dropped just before this block */
    double shift; /* front-end frequency correction, cycles/symbol */
    float data[2 * SBLK];
};

#define AFC_THRESHOLD 0.01 /* cycles/symbol before correcting upstream */

#define MAX_XFEC 32400 /* symbols in the longest XFECFRAME (QPSK, normal) */
#define MAX_OPEN 4     /* batches being filled at once: MODCODs in a VCM mix */

/* Frames of one MODCOD: the PL thread fills in the symbols, an LDPC worker
 * demaps and decodes them (demapping dense APSK costs about as much as the
 * PL sync, so it is spread over the workers). */
struct batch {
    drx_modcod mc;
    struct timespec start; /* first frame in */
    uint64_t fseq[MAX_LANES]; /* frame sequence numbers, increasing */
    int emit;                 /* output thread: next frame to output */
    const drx_demap* demap;
    int n;
    drx_fec_params fec;
    int iter;
    int demapped; /* by a demapper thread, else by the LDPC worker */
    float* sym[MAX_LANES];
    float n0[MAX_LANES];
    int8_t* llr[MAX_LANES];
    uint8_t* cw[MAX_LANES];
};

struct worker {
    drx_mt* mt;
    pthread_t thread;
    drx_ldpc* ldpc[128]; /* by PLS code >> 1 */
};

struct drx_mt {
    drx_mt_cfg cfg;
    drx_ts_cb cb;
    void* ctx;
    int nbatch, batch_max;

    struct queue free_s, samp_q, filt_q, free_y, sym_q, free_b, demap_q, ldpc_q, done_q;
    pthread_t t_mf, t_timing, t_pl, t_out;
    struct worker workers[MAX_WORKERS];
    int nworkers, workers_left;
    pthread_t t_demap[MAX_WORKERS];
    int ndemap, demap_left;
    pthread_mutex_t workers_lock;
    int started, finished;

    /* producer side */
    struct sblock* fill;
    uint64_t dropped;
    int gap;

    /* front-end thread */
    drx_frontend* fe;          /* filter stage and timing stage */
    _Atomic double afc_target; /* requested correction, cycles/symbol */
    double fe_shift;           /* correction in use by the filter stage */

    /* PL thread (or the DVB-S chain) */
    drx_pl* pl;
    drx_dvbs* dvbs;
    drx_dvbc* dvbc;
    drx_dvbt* dvbt;
    drx_dvbt2* dvbt2;
    double pl_shift;   /* front-end correction of the symbols fed so far */
    int afc_pending;   /* a correction was requested and not seen yet */
    drx_demap* demap[128];
    struct batch* open[MAX_OPEN]; /* by age, oldest first */
    int nopen;
    uint64_t next_fseq;

    /* output thread */
    drx_bch* bch[128];
    drx_bbdec* bb;
    uint8_t* ts;

    pthread_mutex_t stats_lock;
    drx_rx_stats st;
    struct drx_tap tap; /* constellation, see drx_mt_constellation() */
    uint64_t iter_sum;
};

void drx_mt_cfg_default(drx_mt_cfg* cfg, float sps, float rolloff)
{
    drx_frontend_cfg_default(&cfg->fe, sps, rolloff);
    drx_rx_cfg_default(&cfg->rx);
    cfg->dvbs = 0;
    cfg->dvbs_rate = 0;
    cfg->dvbc = 0;
    cfg->dvbc_qam = 0;
    cfg->dvbc_inversion = 2;
    cfg->dvbt = 0;
    cfg->sample_rate = 0;
    cfg->bandwidth = 0;
    cfg->dvbt2 = 0;
    cfg->ldpc_threads = 2;
    cfg->demap_threads = -1;
    cfg->max_latency_ms = 100;
}

static double elapsed_ms(const struct timespec* t0)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (t.tv_sec - t0->tv_sec) * 1e3 + (t.tv_nsec - t0->tv_nsec) * 1e-6;
}

/* ---- front-end threads: matched filter, then timing ----------------------- */

static void* mf_thread(void* arg)
{
    drx_mt* mt = arg;
    struct sblock* in;
    while ((in = queue_pop(&mt->samp_q, -1))) {
        if (mt->dvbt) {
            /* DVB-T: resampling to the elementary rate (in place: the
             * resampler keeps its own copy of the input), straight to the
             * OFDM demodulator */
            in->n = drx_dvbt_resample(mt->dvbt, in->data, in->n, in->data, SBLK, &in->shift);
            queue_push(&mt->sym_q, in);
            continue;
        }
        if (mt->dvbt2) {
            in->n = drx_dvbt2_resample(mt->dvbt2, in->data, in->n, in->data, SBLK, &in->shift);
            queue_push(&mt->sym_q, in);
            continue;
        }
        const double target = mt->afc_target;
        if (target != mt->fe_shift) {
            mt->fe_shift = target;
            drx_frontend_set_freq(mt->fe, target / mt->cfg.fe.sps);
        }
        drx_frontend_filter(mt->fe, in->data, in->n, in->data); /* in place */
        in->shift = mt->fe_shift;
        queue_push(&mt->filt_q, in);
    }
    queue_close(&mt->filt_q);
    return NULL;
}

static void* timing_thread(void* arg)
{
    drx_mt* mt = arg;
    struct sblock* in;
    while ((in = queue_pop(&mt->filt_q, -1))) {
        int pos = 0, gap = in->gap;
        while (pos < in->n) {
            struct sblock* out = queue_pop(&mt->free_y, -1);
            out->gap = gap;
            gap = 0;
            int used;
            out->n = drx_frontend_timing(mt->fe, in->data + 2 * pos, in->n - pos, out->data,
                                         SBLK, &used, NULL);
            pos += used;
            out->shift = in->shift;
            queue_push(&mt->sym_q, out);
        }
        queue_push(&mt->free_s, in);
    }
    queue_close(&mt->sym_q);
    return NULL;
}

/* ---- PL + demapper thread ------------------------------------------------ */

/* Where the PL thread sends its batches: demapper threads, or straight to
 * the LDPC workers. */
static struct queue* feed(drx_mt* mt) { return mt->ndemap ? &mt->demap_q : &mt->ldpc_q; }

static void send_batch(drx_mt* mt, int i)
{
    queue_push(feed(mt), mt->open[i]);
    memmove(mt->open + i, mt->open + i + 1, sizeof(*mt->open) * (mt->nopen - i - 1));
    mt->nopen--;
}

static void send_all(drx_mt* mt)
{
    while (mt->nopen)
        send_batch(mt, 0);
}

/* The batch collecting frames of MODCOD m: one batch per MODCOD, so that a
 * VCM/ACM mix still fills the LDPC lanes. */
static struct batch* batch_for(drx_mt* mt, const drx_modcod* m, const drx_fec_params* fec,
                               const drx_demap* dm)
{
    for (int i = 0; i < mt->nopen; i++)
        if (mt->open[i]->mc.pls == m->pls)
            return mt->open[i];
    if (mt->nopen == MAX_OPEN)
        send_batch(mt, 0);
    struct batch* b = queue_pop(&mt->free_b, 0);
    if (!b) {
        /* The output thread may be waiting for a frame held in an open
         * batch: send them before blocking. */
        send_all(mt);
        b = queue_pop(&mt->free_b, -1);
    }
    b->mc = *m;
    b->fec = *fec;
    b->demap = dm;
    b->n = b->emit = 0;
    b->demapped = 0;
    clock_gettime(CLOCK_MONOTONIC, &b->start);
    mt->open[mt->nopen++] = b;
    return b;
}

/* drx_pl_out_cb: the PL sync writes the frame straight into the open batch
 * of its MODCOD when there is one (the usual case), saving a copy. */
static float* plframe_buffer(void* ctx, const drx_modcod* m, int nsym)
{
    drx_mt* mt = ctx;
    if (nsym > MAX_XFEC)
        return NULL;
    for (int i = 0; i < mt->nopen; i++)
        if (mt->open[i]->mc.pls == m->pls) /* not full: sent when full */
            return mt->open[i]->sym[mt->open[i]->n];
    return NULL;
}

static void on_plframe(void* ctx, const drx_plframe* info, const float* xfec)
{
    drx_mt* mt = ctx;
    const drx_modcod* m = &info->modcod;
    drx_demap** dm = &mt->demap[m->pls >> 1];
    if (!*dm && !(*dm = drx_demap_new(m->constellation, m->rate, m->short_frame)))
        return;
    drx_fec_params fec;
    if (drx_fec_lookup(m->short_frame, m->rate, &fec))
        return;

    struct batch* b = batch_for(mt, m, &fec, *dm);
    b->fseq[b->n] = mt->next_fseq++;
    b->n0[b->n] = info->n0;
    if (xfec != b->sym[b->n]) /* else already in place, see plframe_buffer() */
        memcpy(b->sym[b->n], xfec, sizeof(float) * 2 * info->nsym);
    if (drx_tap_on(&mt->tap))
        drx_tap_put(&mt->tap, xfec, info->nsym, info->nsym / 512 + 1);
    b->n++;
    if (b->n == mt->batch_max)
        for (int i = 0; i < mt->nopen; i++)
            if (mt->open[i] == b)
                send_batch(mt, i);

    /* AFC: move large carrier offsets ahead of the matched filter. A new
     * request waits until the previous one reached the frames being
     * decoded, i.e. the front-end applied it and so did the PL sync. */
    if (mt->afc_pending && mt->pl_shift == mt->afc_target && !drx_pl_shift_pending(mt->pl))
        mt->afc_pending = 0;
    if (!mt->afc_pending && info->settled && info->snr_db > 0 &&
        (info->freq > AFC_THRESHOLD || info->freq < -AFC_THRESHOLD)) {
        mt->afc_target = mt->pl_shift + info->freq;
        mt->afc_pending = 1;
    }
    pthread_mutex_lock(&mt->stats_lock);
    mt->st.plframes++;
    mt->st.pls = m->pls;
    mt->st.constellation = m->constellation;
    mt->st.rate = m->rate;
    mt->st.short_frame = m->short_frame;
    mt->st.pilots = info->pilots;
    mt->st.snr_db = info->snr_db;
    mt->st.freq = (float)(info->freq + mt->pl_shift);
    pthread_mutex_unlock(&mt->stats_lock);
}

static void on_ts_dvbs(void* ctx, const uint8_t* ts, int len)
{
    drx_mt* mt = ctx;
    mt->cb(mt->ctx, 0, ts, len);
}

/* DVB-S: the whole chain (carrier, Viterbi, RS) runs on this thread. */
static void* dvbs_thread(void* arg)
{
    static const char* const rates[] = { NULL, "1_2", "2_3", "3_4", "5_6", "7_8" };
    drx_mt* mt = arg;
    struct sblock* y;
    while ((y = queue_pop(&mt->sym_q, -1))) {
        if (y->shift != mt->pl_shift) {
            drx_dvbs_shift(mt->dvbs, y->shift - mt->pl_shift);
            mt->pl_shift = y->shift;
        }
        drx_dvbs_process(mt->dvbs, y->data, y->n, on_ts_dvbs, mt);
        queue_push(&mt->free_y, y);

        drx_dvbs_stats ds;
        drx_dvbs_get_stats(mt->dvbs, &ds);
        /* AFC, as for DVB-S2: total correction = upstream + residual */
        if (mt->afc_pending && mt->pl_shift == mt->afc_target)
            mt->afc_pending = 0;
        const double residual = ds.freq - mt->pl_shift;
        if (!mt->afc_pending && ds.locked && (residual > AFC_THRESHOLD || residual < -AFC_THRESHOLD)) {
            mt->afc_target = ds.freq;
            mt->afc_pending = 1;
        }
        pthread_mutex_lock(&mt->stats_lock);
        mt->st.locked = ds.locked;
        mt->st.constellation = "QPSK";
        mt->st.rate = ds.rate ? rates[ds.rate] : NULL;
        mt->st.snr_db = ds.snr_db;
        mt->st.freq = ds.freq;
        mt->st.plframes = ds.packets;
        mt->st.ts_packets = ds.packets;
        mt->st.ts_crc_errors = ds.rs_failed;
        mt->st.bch_failed = ds.rs_failed;
        pthread_mutex_unlock(&mt->stats_lock);
    }
    drx_dvbs_flush(mt->dvbs);
    drx_dvbs_stats ds;
    drx_dvbs_get_stats(mt->dvbs, &ds);
    pthread_mutex_lock(&mt->stats_lock);
    mt->st.ts_packets = ds.packets;
    mt->st.ts_crc_errors = mt->st.bch_failed = ds.rs_failed;
    pthread_mutex_unlock(&mt->stats_lock);
    queue_close(feed(mt));
    return NULL;
}

/* DVB-C: equaliser, carrier, outer layer, all on this thread. */
static void* dvbc_thread(void* arg)
{
    static const char* const names[] = { "QAM16", "QAM32", "QAM64", "QAM128", "QAM256" };
    drx_mt* mt = arg;
    struct sblock* y;
    while ((y = queue_pop(&mt->sym_q, -1))) {
        if (y->shift != mt->pl_shift) {
            drx_dvbc_shift(mt->dvbc, y->shift - mt->pl_shift);
            mt->pl_shift = y->shift;
        }
        drx_dvbc_process(mt->dvbc, y->data, y->n, on_ts_dvbs, mt);
        queue_push(&mt->free_y, y);

        drx_dvbc_stats cs;
        drx_dvbc_get_stats(mt->dvbc, &cs);
        if (mt->afc_pending && mt->pl_shift == mt->afc_target)
            mt->afc_pending = 0;
        const double residual = cs.freq - mt->pl_shift;
        if (!mt->afc_pending && cs.locked && (residual > AFC_THRESHOLD || residual < -AFC_THRESHOLD)) {
            mt->afc_target = cs.freq;
            mt->afc_pending = 1;
        }
        pthread_mutex_lock(&mt->stats_lock);
        mt->st.locked = cs.locked;
        mt->st.constellation = cs.qam == 16 ? names[0] : cs.qam == 32 ? names[1] : cs.qam == 64 ? names[2]
                             : cs.qam == 128 ? names[3] : cs.qam == 256 ? names[4] : NULL;
        mt->st.rate = NULL;
        mt->st.snr_db = cs.snr_db;
        mt->st.freq = cs.freq;
        mt->st.plframes = cs.packets;
        mt->st.ts_packets = cs.packets;
        mt->st.ts_crc_errors = cs.rs_failed;
        mt->st.bch_failed = cs.rs_failed;
        pthread_mutex_unlock(&mt->stats_lock);
    }
    queue_close(feed(mt));
    return NULL;
}

/* DVB-T: OFDM demodulation on this thread, Viterbi on the receiver's own. */
static void* dvbt_thread(void* arg)
{
    static const char* const qams[] = { "QPSK", "QAM16", "QAM64" };
    static const char* const rates[] = { NULL, "1_2", "2_3", "3_4", "5_6", "7_8" };
    drx_mt* mt = arg;
    struct sblock* y;
    while ((y = queue_pop(&mt->sym_q, -1))) {
        if (y->gap)
            drx_dvbt_gap(mt->dvbt);
        drx_dvbt_demod(mt->dvbt, y->data, y->n, y->shift, on_ts_dvbs, mt);
        queue_push(&mt->free_s, y);
        drx_dvbt_stats ts;
        drx_dvbt_get_stats(mt->dvbt, &ts);
        pthread_mutex_lock(&mt->stats_lock);
        mt->st.locked = ts.locked;
        mt->st.constellation = ts.qam == 4 ? qams[0] : ts.qam == 16 ? qams[1] : ts.qam == 64 ? qams[2] : NULL;
        mt->st.rate = ts.rate ? rates[ts.rate] : NULL;
        mt->st.snr_db = ts.snr_db;
        mt->st.freq = ts.freq;
        mt->st.plframes = ts.symbols;
        mt->st.ts_packets = ts.packets;
        mt->st.ts_crc_errors = ts.rs_failed;
        mt->st.bch_failed = ts.rs_failed;
        pthread_mutex_unlock(&mt->stats_lock);
    }
    drx_dvbt_flush(mt->dvbt);
    queue_close(feed(mt));
    return NULL;
}

/* DVB-T2: OFDM demodulation, L1, PLP decoding on this thread. */
static void dvbt2_stats(drx_mt* mt)
{
    drx_dvbt2_stats ts;
    drx_dvbt2_get_stats(mt->dvbt2, &ts);
    pthread_mutex_lock(&mt->stats_lock);
    mt->st.locked = ts.locked;
    mt->st.constellation = ts.qam == 4 ? "QPSK" : ts.qam == 16 ? "QAM16" : ts.qam == 64 ? "QAM64"
                         : ts.qam == 256 ? "QAM256" : NULL;
    mt->st.rate = ts.rate;
    mt->st.snr_db = ts.snr_db;
    mt->st.freq = ts.freq;
    mt->st.plframes = ts.fec_blocks;
    mt->st.ldpc_failed_batches = ts.fec_failed;
    mt->st.ts_packets = ts.packets;
    mt->st.ts_crc_errors = ts.crc_errors;
    mt->st.bch_failed = ts.fec_failed;
    pthread_mutex_unlock(&mt->stats_lock);
}

static void* dvbt2_thread(void* arg)
{
    drx_mt* mt = arg;
    struct sblock* y;
    while ((y = queue_pop(&mt->sym_q, -1))) {
        if (y->gap)
            drx_dvbt2_gap(mt->dvbt2);
        drx_dvbt2_demod(mt->dvbt2, y->data, y->n, y->shift, on_ts_dvbs, mt);
        queue_push(&mt->free_s, y);
        dvbt2_stats(mt);
    }
    drx_dvbt2_flush(mt->dvbt2); /* the last frames' TS */
    dvbt2_stats(mt);
    queue_close(feed(mt));
    return NULL;
}

static void* pl_thread(void* arg)
{
    drx_mt* mt = arg;
    for (;;) {
        struct sblock* y = queue_pop(&mt->sym_q, 10);
        if (y) {
            if (y->gap)
                drx_pl_reset(mt->pl);
            if (y->shift != mt->pl_shift) {
                drx_pl_shift(mt->pl, y->shift - mt->pl_shift);
                mt->pl_shift = y->shift;
            }
            drx_pl_process(mt->pl, y->data, y->n, on_plframe, mt);
            queue_push(&mt->free_y, y);
        } else if (queue_closed(&mt->sym_q)) {
            break;
        }
        for (int i = 0; i < mt->nopen;)
            if (elapsed_ms(&mt->open[i]->start) > mt->cfg.max_latency_ms)
                send_batch(mt, i);
            else
                i++;
        drx_pl_stats ps;
        drx_pl_get_stats(mt->pl, &ps);
        pthread_mutex_lock(&mt->stats_lock);
        mt->st.locked = ps.locked;
        pthread_mutex_unlock(&mt->stats_lock);
    }
    send_all(mt);
    queue_close(feed(mt));
    return NULL;
}

/* ---- LDPC workers --------------------------------------------------------- */

/* Demapping in threads of its own (dense APSK): with it, the LDPC workers
 * were 5 decoders whose working sets (2.5 MB each at 32 lanes) overflowed
 * the L3 cache; 3 decoders alone decode more (memory bound beyond). */
static void* demap_thread(void* arg)
{
    drx_mt* mt = arg;
    struct batch* b;
    while ((b = queue_pop(&mt->demap_q, -1))) {
        for (int f = 0; f < b->n; f++)
            drx_demap_frame(b->demap, b->sym[f], b->n0[f], b->llr[f]);
        b->demapped = 1;
        queue_push(&mt->ldpc_q, b);
    }
    pthread_mutex_lock(&mt->workers_lock);
    if (--mt->demap_left == 0)
        queue_close(&mt->ldpc_q);
    pthread_mutex_unlock(&mt->workers_lock);
    return NULL;
}

static void* ldpc_thread(void* arg)
{
    struct worker* w = arg;
    drx_mt* mt = w->mt;
    struct batch* b;
    while ((b = queue_pop(&mt->ldpc_q, -1))) {
        drx_ldpc** ld = &w->ldpc[b->mc.pls >> 1];
        if (!*ld)
            *ld = drx_ldpc_new(&b->fec, b->mc.rate);
        if (!b->demapped)
            for (int f = 0; f < b->n; f++)
                drx_demap_frame(b->demap, b->sym[f], b->n0[f], b->llr[f]);
        b->iter = *ld ? drx_ldpc_decode(*ld, (const int8_t* const*)b->llr, b->cw, b->n,
                                        mt->cfg.rx.ldpc_iter)
                      : -1;
        queue_push(&mt->done_q, b);
    }
    pthread_mutex_lock(&mt->workers_lock);
    if (--mt->workers_left == 0)
        queue_close(&mt->done_q);
    pthread_mutex_unlock(&mt->workers_lock);
    return NULL;
}

/* ---- output thread -------------------------------------------------------- */

/* BCH, BB layer and TS output of frame f of batch b. */
static void output_frame(drx_mt* mt, struct batch* b, int f)
{
    drx_bch** bc = &mt->bch[b->mc.pls >> 1];
    if (!*bc)
        *bc = drx_bch_new(&b->fec);
    int r = *bc ? drx_bch_decode(*bc, b->cw[f]) : -1;
    if (r >= 0) {
        const int kbytes = b->fec.kbch / 8;
        drx_bb_descramble(b->cw[f], kbytes);
        drx_bbheader h;
        int len = drx_bbdec_frame(mt->bb, b->cw[f], kbytes, mt->ts, &h);
        if (len > 0 && mt->cfg.rx.isi == DRX_ISI_FIRST)
            mt->cfg.rx.isi = h.isi; /* lock on the first stream */
        if (len > 0 && (mt->cfg.rx.isi == DRX_ISI_ALL || mt->cfg.rx.isi == h.isi))
            mt->cb(mt->ctx, h.isi, mt->ts, len);
    }
    drx_bbdec_stats bs;
    drx_bbdec_get_stats(mt->bb, &bs);
    pthread_mutex_lock(&mt->stats_lock);
    if (r < 0)
        mt->st.bch_failed++;
    else
        mt->st.bch_corrected_bits += r;
    mt->st.ts_packets = bs.packets;
    mt->st.ts_crc_errors = bs.crc_errors;
    mt->st.bb_bad_headers = bs.bad_headers;
    pthread_mutex_unlock(&mt->stats_lock);
}

/* Batches of different MODCODs complete in any order; frames are output in
 * their original order (each input stream must stay in sequence). */
static void* out_thread(void* arg)
{
    drx_mt* mt = arg;
    struct batch* pending[64];
    int npending = 0;
    uint64_t next = 0;
    struct batch* b;
    while ((b = queue_pop(&mt->done_q, -1))) {
        pthread_mutex_lock(&mt->stats_lock);
        mt->st.ldpc_batches++;
        if (b->iter < 0)
            mt->st.ldpc_failed_batches++;
        mt->iter_sum += b->iter < 0 ? (uint64_t)mt->cfg.rx.ldpc_iter : (uint64_t)b->iter;
        pthread_mutex_unlock(&mt->stats_lock);
        pending[npending++] = b;
        for (int progress = 1; progress;) {
            progress = 0;
            for (int i = 0; i < npending;) {
                struct batch* p = pending[i];
                while (p->emit < p->n && p->fseq[p->emit] == next) {
                    output_frame(mt, p, p->emit++);
                    next++;
                    progress = 1;
                }
                if (p->emit == p->n) {
                    queue_push(&mt->free_b, p);
                    pending[i] = pending[--npending];
                } else {
                    i++;
                }
            }
        }
    }
    return NULL;
}

/* ---- setup ------------------------------------------------------------------ */

drx_mt* drx_mt_new(const drx_mt_cfg* cfg, drx_ts_cb cb, void* ctx)
{
    drx_mt* mt = calloc(1, sizeof(*mt));
    if (!mt)
        return NULL;
    mt->cfg = *cfg;
    mt->cb = cb;
    mt->ctx = ctx;
    int nthreads = cfg->ldpc_threads < 1 ? 1
                   : cfg->ldpc_threads > MAX_WORKERS ? MAX_WORKERS
                                                     : cfg->ldpc_threads;
    /* auto: beyond 3 decoder threads, 1 or 2 of them demap */
    mt->ndemap = cfg->demap_threads >= 0 ? cfg->demap_threads
                 : nthreads >= 5         ? 2
                 : nthreads == 4         ? 1
                                         : 0;
    if (cfg->demap_threads < 0 && !(cfg->dvbs || cfg->dvbc || cfg->dvbt || cfg->dvbt2))
        nthreads -= mt->ndemap;
    if (cfg->dvbs || cfg->dvbc || cfg->dvbt || cfg->dvbt2)
        mt->ndemap = 0;
    mt->ndemap = mt->ndemap > MAX_WORKERS ? MAX_WORKERS : mt->ndemap;
    mt->nworkers = nthreads < 1 ? 1 : nthreads;
    mt->nbatch = 2 * mt->nworkers + 2 * mt->ndemap + 2 + MAX_OPEN;
    mt->batch_max = drx_ldpc_lanes();
    if (cfg->rx.max_batch > 0 && cfg->rx.max_batch < mt->batch_max)
        mt->batch_max = cfg->rx.max_batch;
    pthread_mutex_init(&mt->stats_lock, NULL);
    pthread_mutex_init(&mt->workers_lock, NULL);

    drx_frontend_cfg fc = cfg->fe;
    if (cfg->dvbt || cfg->dvbt2) /* not used: any valid settings */
        drx_frontend_cfg_default(&fc, 2, 0.2f);
    mt->fe = drx_frontend_new(&fc);
    mt->pl = drx_pl_new(cfg->rx.gold_code);
    if (mt->pl)
        drx_pl_set_out_cb(mt->pl, plframe_buffer);
    if (cfg->dvbs)
        mt->dvbs = drx_dvbs_new((enum drx_dvbs_rate)cfg->dvbs_rate, 1);
    if (cfg->dvbc)
        mt->dvbc = drx_dvbc_new((enum drx_qam)cfg->dvbc_qam);
    if (mt->dvbc)
        drx_dvbc_set_inversion(mt->dvbc, cfg->dvbc_inversion);
    /* in-place resampling needs at most one output per input */
    if (cfg->dvbt && cfg->sample_rate >= cfg->bandwidth * 8 / 7)
        mt->dvbt = drx_dvbt_new(cfg->sample_rate, cfg->bandwidth, 1);
    if (cfg->dvbt2 && cfg->sample_rate >= cfg->bandwidth * 8 / 7)
        mt->dvbt2 = drx_dvbt2_new(cfg->sample_rate, cfg->bandwidth, cfg->rx.isi >= 0 ? cfg->rx.isi : -1);
    if (mt->dvbs)
        drx_dvbs_set_tap(mt->dvbs, &mt->tap);
    if (mt->dvbc)
        drx_dvbc_set_tap(mt->dvbc, &mt->tap);
    if (mt->dvbt)
        drx_dvbt_set_tap(mt->dvbt, &mt->tap);
    if (mt->dvbt2)
        drx_dvbt2_set_tap(mt->dvbt2, &mt->tap);
    mt->bb = drx_bbdec_new();
    mt->ts = malloc(58192 / 8 + 188);
    int ok = mt->fe && mt->pl && mt->bb && mt->ts && (!cfg->dvbs || mt->dvbs) &&
             (!cfg->dvbc || mt->dvbc) && (!cfg->dvbt || mt->dvbt) &&
             (!cfg->dvbt2 || mt->dvbt2);
    ok = ok && !queue_init(&mt->free_s, NSBLK) && !queue_init(&mt->samp_q, NSBLK) &&
         !queue_init(&mt->filt_q, NSBLK) && !queue_init(&mt->free_y, NYBLK) && !queue_init(&mt->sym_q, NYBLK) &&
         !queue_init(&mt->free_b, mt->nbatch) && !queue_init(&mt->ldpc_q, mt->nbatch) &&
         !queue_init(&mt->demap_q, mt->nbatch) &&
         !queue_init(&mt->done_q, mt->nbatch);
    for (int i = 0; ok && i < NSBLK; i++) {
        struct sblock* s = malloc(sizeof(*s));
        ok = s != NULL;
        if (ok)
            queue_push(&mt->free_s, s);
    }
    for (int i = 0; ok && i < NYBLK; i++) {
        struct sblock* s = malloc(sizeof(*s));
        ok = s != NULL;
        if (ok)
            queue_push(&mt->free_y, s);
    }
    for (int i = 0; ok && i < mt->nbatch; i++) {
        struct batch* b = calloc(1, sizeof(*b));
        ok = b != NULL;
        for (int l = 0; ok && l < mt->batch_max; l++) {
            b->llr[l] = malloc(64800);
            b->sym[l] = malloc(sizeof(float) * 2 * MAX_XFEC);
            b->cw[l] = malloc(64800 / 8);
            ok = b->llr[l] && b->cw[l] && b->sym[l];
        }
        if (b)
            queue_push(&mt->free_b, b);
    }
    if (!ok) {
        drx_mt_free(mt);
        return NULL;
    }

    mt->workers_left = mt->nworkers;
    mt->demap_left = mt->ndemap;
    pthread_create(&mt->t_mf, NULL, mf_thread, mt);
    pthread_setname_np(mt->t_mf, "drx-filter");
    pthread_create(&mt->t_timing, NULL, timing_thread, mt);
    pthread_setname_np(mt->t_timing, "drx-timing");
    pthread_create(&mt->t_pl, NULL, cfg->dvbs ? dvbs_thread : cfg->dvbc ? dvbc_thread : cfg->dvbt ? dvbt_thread
                         : cfg->dvbt2 ? dvbt2_thread : pl_thread, mt);
    pthread_setname_np(mt->t_pl, "drx-plsync");
    for (int i = 0; i < mt->nworkers; i++) {
        char name[16];
        snprintf(name, sizeof(name), "drx-ldpc%d", i);
        mt->workers[i].mt = mt;
        pthread_create(&mt->workers[i].thread, NULL, ldpc_thread, &mt->workers[i]);
        pthread_setname_np(mt->workers[i].thread, name);
    }
    for (int i = 0; i < mt->ndemap; i++) {
        char name[16];
        snprintf(name, sizeof(name), "drx-demap%d", i);
        pthread_create(&mt->t_demap[i], NULL, demap_thread, mt);
        pthread_setname_np(mt->t_demap[i], name);
    }
    pthread_create(&mt->t_out, NULL, out_thread, mt);
    pthread_setname_np(mt->t_out, "drx-output");
    mt->started = 1;
    return mt;
}

int drx_mt_push(drx_mt* mt, const float* iq, int n, int nonblock)
{
    int done = 0;
    while (done < n) {
        if (!mt->fill) {
            mt->fill = nonblock ? queue_pop(&mt->free_s, 0) : queue_pop(&mt->free_s, -1);
            if (!mt->fill) {
                mt->dropped += n - done;
                mt->gap = 1;
                return done;
            }
            mt->fill->n = 0;
            mt->fill->gap = mt->gap;
            mt->gap = 0;
        }
        int m = SBLK - mt->fill->n < n - done ? SBLK - mt->fill->n : n - done;
        memcpy(mt->fill->data + 2 * mt->fill->n, iq + 2 * done, sizeof(float) * 2 * m);
        mt->fill->n += m;
        done += m;
        if (mt->fill->n == SBLK) {
            queue_push(&mt->samp_q, mt->fill);
            mt->fill = NULL;
        }
    }
    return done;
}

void drx_mt_finish(drx_mt* mt)
{
    if (!mt->started || mt->finished)
        return;
    if (mt->fill && mt->fill->n)
        queue_push(&mt->samp_q, mt->fill);
    else if (mt->fill)
        queue_push(&mt->free_s, mt->fill);
    mt->fill = NULL;
    queue_close(&mt->samp_q);
    pthread_join(mt->t_mf, NULL);
    pthread_join(mt->t_timing, NULL);
    pthread_join(mt->t_pl, NULL);
    for (int i = 0; i < mt->ndemap; i++)
        pthread_join(mt->t_demap[i], NULL);
    for (int i = 0; i < mt->nworkers; i++)
        pthread_join(mt->workers[i].thread, NULL);
    pthread_join(mt->t_out, NULL);
    mt->finished = 1;
}

static void free_queue_items(struct queue* q, int batches)
{
    void* p;
    while (q->items && (p = queue_pop(q, 0))) {
        if (batches) {
            struct batch* b = p;
            for (int l = 0; l < MAX_LANES; l++) {
                free(b->llr[l]);
                free(b->sym[l]);
                free(b->cw[l]);
            }
        }
        free(p);
    }
    if (q->items)
        queue_destroy(q);
}

void drx_mt_free(drx_mt* mt)
{
    if (!mt)
        return;
    drx_mt_finish(mt);
    free_queue_items(&mt->free_s, 0);
    free_queue_items(&mt->samp_q, 0);
    free_queue_items(&mt->filt_q, 0);
    free_queue_items(&mt->free_y, 0);
    free_queue_items(&mt->sym_q, 0);
    free_queue_items(&mt->free_b, 1);
    free_queue_items(&mt->demap_q, 1);
    free_queue_items(&mt->ldpc_q, 1);
    free_queue_items(&mt->done_q, 1);
    for (int m = 0; m < 128; m++) {
        drx_demap_free(mt->demap[m]);
        drx_bch_free(mt->bch[m]);
        for (int w = 0; w < mt->nworkers; w++)
            drx_ldpc_free(mt->workers[w].ldpc[m]);
    }
    drx_frontend_free(mt->fe);
    drx_pl_free(mt->pl);
    drx_dvbs_free(mt->dvbs);
    drx_dvbc_free(mt->dvbc);
    drx_dvbt_free(mt->dvbt);
    drx_dvbt2_free(mt->dvbt2);
    drx_bbdec_free(mt->bb);
    free(mt->ts);
    free(mt);
}

void drx_mt_get_stats(drx_mt* mt, drx_rx_stats* s)
{
    pthread_mutex_lock(&mt->stats_lock);
    *s = mt->st;
    s->ldpc_avg_iter = mt->st.ldpc_batches ? (float)mt->iter_sum / mt->st.ldpc_batches : 0;
    pthread_mutex_unlock(&mt->stats_lock);
}

uint64_t drx_mt_dropped(drx_mt* mt) { return mt->dropped; }

int drx_mt_constellation(drx_mt* mt, float* iq, int max)
{
    atomic_store(&mt->tap.until, drx_tap_now() + 5.0);
    const unsigned w = atomic_load_explicit(&mt->tap.w, memory_order_acquire);
    int n = max < DRX_TAP_N ? max : DRX_TAP_N;
    if ((unsigned)n > w)
        n = (int)w;
    double p = 0;
    for (int i = 0; i < n; i++) {
        const unsigned k = (w - n + i) % DRX_TAP_N;
        iq[2 * i] = mt->tap.buf[2 * k];
        iq[2 * i + 1] = mt->tap.buf[2 * k + 1];
        p += (double)iq[2 * i] * iq[2 * i] + (double)iq[2 * i + 1] * iq[2 * i + 1];
    }
    if (n && p > 0) {
        const float g = (float)(1.0 / sqrt(p / n));
        for (int i = 0; i < 2 * n; i++)
            iq[i] *= g;
    }
    return n;
}
