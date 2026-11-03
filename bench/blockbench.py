#!/usr/bin/env python3
"""Throughput of individual receiver blocks: IQ file -> block -> null sink."""
import sys, time
from gnuradio import gr, blocks
from gnuradio import dvbs2rx

def bench(path, make_block, label, sps=2.0):
    tb = gr.top_block()
    src = blocks.file_source(gr.sizeof_gr_complex, path, False)
    blk = make_block()
    tb.connect(src, blk, blocks.null_sink(gr.sizeof_gr_complex))
    t0 = time.monotonic(); tb.run(); dt = time.monotonic() - t0
    nsamp = src.nitems_written(0) if hasattr(src, 'nitems_written') else 0
    import os; nsamp = os.path.getsize(path) // 8
    print(f"{label:40s} {nsamp/sps/dt/1e6:7.2f} Msym/s  ({dt:.2f} s)")

path = sys.argv[1]
bench(path, lambda: blocks.copy(gr.sizeof_gr_complex), "copy (reference)")
for name, m in (("polyphase", 0), ("linear", 1), ("quadratic", 2), ("cubic", 3)):
    for d in (50, 10):
        bench(path, lambda: dvbs2rx.symbol_sync_cc(2.0, 1e-3, 1.0, 0.2, d, 128, m),
              f"symbol_sync {name} rrc_delay={d}")
