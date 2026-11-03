/*
 * GNU Radio block around the libdvbrx front-end (matched filter, timing
 * recovery, AGC): samples in, symbols out.
 *
 * Stream tags move to the first symbol whose strobe is at or after the
 * tagged sample, as gr-dvbs2rx's PL Sync expects for the rotator tags.
 */
#include "frontend_cc.h"

#include <gnuradio/io_signature.h>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace gr {
namespace dvbrx {

frontend_cc::sptr frontend_cc::make(
    float sps, float rolloff, int rrc_delay, float loop_bw, float damping)
{
    return gnuradio::make_block_sptr<frontend_cc>(sps, rolloff, rrc_delay, loop_bw, damping);
}

frontend_cc::frontend_cc(
    float sps, float rolloff, int rrc_delay, float loop_bw, float damping)
    : gr::block("dvbrx_frontend_cc",
                gr::io_signature::make(1, 1, sizeof(gr_complex)),
                gr::io_signature::make(1, 1, sizeof(gr_complex))),
      d_sps(sps)
{
    drx_frontend_cfg cfg;
    drx_frontend_cfg_default(&cfg, sps, rolloff);
    cfg.rrc_delay = rrc_delay;
    cfg.loop_bw = loop_bw;
    cfg.damping = damping;
    d_fe = drx_frontend_new(&cfg);
    if (!d_fe)
        throw std::invalid_argument("dvbrx frontend_cc: invalid configuration");
    set_relative_rate(1.0 / sps);
    set_tag_propagation_policy(TPP_DONT);
}

frontend_cc::~frontend_cc() { drx_frontend_free(d_fe); }

float frontend_cc::gain() const { return drx_frontend_gain(d_fe); }

void frontend_cc::forecast(int noutput_items, gr_vector_int& ninput_items_required)
{
    ninput_items_required[0] = static_cast<int>(std::ceil(noutput_items * d_sps));
}

int frontend_cc::general_work(int noutput_items,
                              gr_vector_int& ninput_items,
                              gr_vector_const_void_star& input_items,
                              gr_vector_void_star& output_items)
{
    auto in = static_cast<const float*>(input_items[0]);
    auto out = static_cast<float*>(output_items[0]);
    if (d_strobe.size() < static_cast<size_t>(noutput_items))
        d_strobe.resize(noutput_items);

    int used = 0;
    const int n = drx_frontend_process(
        d_fe, in, ninput_items[0], out, noutput_items, &used, d_strobe.data());

    std::vector<tag_t> tags;
    get_tags_in_range(tags, 0, nitems_read(0), nitems_read(0) + used);
    tags.insert(tags.begin(), d_pending.begin(), d_pending.end());
    d_pending.clear();
    const auto first = d_strobe.begin(), last = d_strobe.begin() + n;
    for (auto& tag : tags) {
        const auto it = std::lower_bound(first, last, tag.offset);
        if (it != last) {
            tag.offset = nitems_written(0) + (it - first);
            add_item_tag(0, tag);
        } else {
            d_pending.push_back(tag);
        }
    }

    consume_each(used);
    return n;
}

} // namespace dvbrx
} // namespace gr
