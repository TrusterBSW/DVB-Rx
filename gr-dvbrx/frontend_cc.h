#ifndef GR_DVBRX_FRONTEND_CC_H
#define GR_DVBRX_FRONTEND_CC_H

#include <gnuradio/block.h>
#include <dvbrx/frontend.h>
#include <vector>

namespace gr {
namespace dvbrx {

class frontend_cc : public gr::block
{
public:
    typedef std::shared_ptr<frontend_cc> sptr;
    static sptr make(float sps,
                     float rolloff,
                     int rrc_delay = 10,
                     float loop_bw = 1e-3,
                     float damping = 1.0);

    frontend_cc(float sps, float rolloff, int rrc_delay, float loop_bw, float damping);
    ~frontend_cc() override;

    float gain() const;

    void forecast(int noutput_items, gr_vector_int& ninput_items_required) override;
    int general_work(int noutput_items,
                     gr_vector_int& ninput_items,
                     gr_vector_const_void_star& input_items,
                     gr_vector_void_star& output_items) override;

private:
    drx_frontend* d_fe;
    float d_sps;
    std::vector<uint64_t> d_strobe;
    std::vector<tag_t> d_pending;
};

} // namespace dvbrx
} // namespace gr

#endif
