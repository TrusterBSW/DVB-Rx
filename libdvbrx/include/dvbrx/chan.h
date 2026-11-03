/*
 * Channelizer: several channels picked out of one wideband capture, so
 * that the multiplexes inside the SDR's bandwidth are received at once.
 *
 * Fast-convolution filter bank (overlap-save), the receiving counterpart of
 * dvbtx's synthesis bank: one FFT of the capture per block, shared by all
 * the channels; for each channel, the bins around its frequency are
 * weighted (flat over the occupied band, cosine taper up to the output
 * Nyquist frequency) and one small inverse FFT gives its samples at its own
 * rate, Fs * L / N.
 *
 * Thread safety: channels may be added, moved and removed from one thread
 * while another one runs drx_chan_process.
 */
#ifndef DVBRX_CHAN_H
#define DVBRX_CHAN_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct drx_chan drx_chan;

/* Receives n samples (interleaved I/Q) of one channel, on the thread
 * running drx_chan_process. */
typedef void (*drx_chan_cb)(void* ctx, const float* iq, int n);

drx_chan* drx_chan_new(double fs);
void drx_chan_free(drx_chan* c);

/*
 * Adds a channel centred at offset (Hz from the capture centre), occupying
 * occ Hz (two-sided), wanted at about rate_want samples per second (the
 * rate obtained, a little above, goes to *rate). Returns its id, or -1 if
 * it does not fit (outside the capture band, too wide).
 */
int drx_chan_add(drx_chan* c, double offset, double occ, double rate_want, double* rate,
                 drx_chan_cb cb, void* ctx);
void drx_chan_remove(drx_chan* c, int id);
/* Moves a channel (the capture centre was retuned). */
void drx_chan_set_offset(drx_chan* c, int id, double offset);

/* Consumes n interleaved I/Q samples of the capture. */
void drx_chan_process(drx_chan* c, const float* iq, int n);

#ifdef __cplusplus
}
#endif

#endif /* DVBRX_CHAN_H */
