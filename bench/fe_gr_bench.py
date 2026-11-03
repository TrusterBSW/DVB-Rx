#!/usr/bin/env python3
"""Throughput of the dvbrx front-end GNU Radio block alone (file -> block ->
null sink), optionally behind a tagging rotator as in the receiver."""
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "python"))
from gnuradio import blocks, dvbs2rx, gr  # noqa: E402
from dvbrx.gr_blocks import frontend_cc  # noqa: E402

path = sys.argv[1]
tb = gr.top_block()
src = blocks.file_source(gr.sizeof_gr_complex, path, False)
fe = frontend_cc(2.0, 0.2)
chain = [src]
if "--rotator" in sys.argv:
    chain.append(dvbs2rx.rotator_cc(0, True))
chain += [fe, blocks.null_sink(gr.sizeof_gr_complex)]
tb.connect(*chain)
t0 = time.monotonic()
tb.run()
dt = time.monotonic() - t0
print(f"{fe.nitems_written(0) / dt / 1e6:.1f} Msym/s ({dt:.2f} s)")
