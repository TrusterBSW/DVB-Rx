# DVB-S2 receive CPU optimisation, agent D: notes

Scope: libdvbrx/src/{pl,demap,ldpc,bch,bb,rxmt}.c and include/dvbrx/pl.h.
Target: i5-9500 (AVX2/FMA, no AVX-512), GCC 14 -O3 -march=native, no -ffast-math.
Nothing committed. Baseline build is ~/dvbrx-agentD-base on the host (unmodified copy).

## Results (host 10.5.1.2, noisy: a VM used up to 240% CPU during the runs)

| Measure | Baseline | Modified |
|---|---|---|
| dvbrx -t 3, s2_8psk34 (8PSK 3/4 pilots, 27.5 Msym/s), cores = (user+sys)/0.4007 s | 3.16-3.58 (min 3.16) | 2.40-2.59 (min 2.40) |
| same, user CPU | 1.13-1.24 s | 0.81-0.89 s (-28%) |
| same, sys CPU (file read + first-touch page faults, unchanged) | 0.13-0.23 s | 0.13-0.18 s |
| dvbrx_file single thread, user cycles, s2_8psk34 | 3.92 G | 2.68 G (-32%) |
| dvbrx_file, np_8psk3_4 (pilotless, 2 sps) | 0.97 G | 0.50 G (-49%) |
| dvbrx_file, 16apsk3_4 | 0.67 G | 0.45 G |
| dvbrx_file, 32apsk4_5 | 0.57 G | 0.38 G |
| perf samples per thread in the pipeline (1999 Hz): LDPC workers / plsync / output | 883 / 658 / 88 | 599 / 332 / 41 |
| pilotless 8PSK pipeline, plsync samples (4999 Hz) | 885 (thread limit ~20 Msym/s, NOT real time) | 241 (~74 Msym/s) |

Output: s2_8psk34 TS byte-identical to baseline (31810 packets, 0 errors, LOCK 8PSK 3_4 pilots).
run_backend.sh and run_pl.sh 0.01 0.5 outputs identical to baseline; run_s2x.py 3.0 -> 55 ok;
run_s2x.py 3.0 64APSK nopilots ok (1492/1595 vs 1522 baseline in that single run; over 48 runs
at 15.4-15.9 dB x 8 offsets: 72348 vs 72443 baseline, noise level).

## Changes

### ldpc.c (decode ~1.75x faster, bit-identical output)
- Serial-to-parallel input (llr[f][j] -> data[j][f]) and hard-decision output were byte loops,
  ~25% of decode time at 4 iterations. Now 16x16 byte transposes with SSE2 unpacks
  (load_frames/store_frames, tr16); hard decisions packed 8 bits per lane with
  v_shift_in_sign then transposed. Unused lanes still get 127.
- Check node update rewritten as two passes per node (CN_IN/CN_OUT) without the
  inp/out/mags VLAs and the pointer array; magnitude recomputed in pass 2.
- First iteration (update_first): all messages zero, so no memset of bnl (7.2 MB for 3/4
  normal at 32 lanes) and no bnl reads.
- ldpcbench (8PSK 3/4 vectors, EsN0 6 dB, 3.75 it, 32 lanes, taskset -c 5): decode cycles for
  80 batches 2.24 G -> 1.26 G; ~4300 -> ~7500 frames/s/core (host noise +-10%).

### pl.c
- rotate(): out = g y e^{j(ph0 + dph i)} (x descr): 8 phasors stepped by e^{j 8 dph}, seeded in
  double, AVX2/FMA cmul8 (scalar fallback). Replaces the serial per-symbol phasor in derotate
  (latency bound) and the cexpf calls.
- Pilot frames: only header, pilot blocks and next SOF are derotated by omega (for phase
  estimation, track_freq and the known-symbol error). Data runs are taken straight from y with
  omega ramp + interpolated pilot phase as one linear phase per run (a run sits inside one
  interpolation segment), written directly to the output. Removed is_pilot() (2 divisions per
  symbol) and the per-symbol segment search.
- decode_pls: 256 x 64 correlations replaced by 4 32-point fast Hadamard transforms (derivation
  in the comment). Checked against brute force with plstest.c: 0 mismatches over 170k noisy
  headers. Header derotation via rotate() instead of 90 cexpf.
- lr_residual: O(n L) double loop -> prefix sums, O(n).
- Pilotless frame_pll:
  - hard decisions via a 128x128 int8 grid (cell -> nearest point when the four corners agree,
    exact by convexity; fallback to full search / drx_demap_nearest); 1/|p|^2 precomputed.
  - loop updated every PLL_BLOCK = 8 symbols. A plain block update cost ~0.65% packets on
    pilotless 16APSK near threshold (48 runs: 191061 vs 192304). Fix: to first order the block
    errors are e = L e' (L unit lower triangular, L_it = K1 + K2 (i-1-t)); the block's phase
    advance and frequency step are linear in e', hence in e: precomputed weights
    w_phase/w_freq (pll_weights()). Restores per-symbol loop dynamics: 16APSK 192875 vs base
    192304; 8PSK 135302 vs 135234; QPSK 122179 vs 122014; 128APSK 33271 vs 32857;
    4_8_4_16APSK 30321 vs 30189; 64APSK 72348 vs 72443 (sums of packets over many runs).
  - Cost (single thread np_8psk3_4, cycles): per-symbol loop with LUT 724 M; block 4: 547 M;
    block 8 plain: 507-515 M; block 8 weighted: 487 M; block 16 weighted: 478 M (kept 8).
- drx_pl_set_out_cb() (new, optional; pl.h): caller may supply the output buffer per frame.

### rxmt.c
- plframe_buffer(): PL writes each XFECFRAME straight into the open batch of its MODCOD; the
  173 KB memcpy per frame in on_plframe is skipped when already in place. rx.c unchanged
  (internal buffer).

### demap.c (bit-identical LLRs)
- quantise in AVX2 (clamp, cvtps_epi32 = lrintf rounding, packs); GCC would not vectorise the
  scalar version (float compare/convert may trap without -fno-trapping-math).
- Distance as |p|^2 - 2 Re(y p*) (|y|^2 cancels): 2 FMAs per point.
- minima_avx: per-bit minima tree in registers for 3-5 bits (8PSK..32APSK), table version
  kept for others.
- demapbench, 16 warm frames: 8PSK 4.93 -> 1.68 ns/sym, 16APSK 7.08 -> 2.80, 32APSK 10.82 -> 5.03.

### bch.c / bb.c
- BCH remainder by slicing-by-4 (4 tables of 256 x 256 bits); CRC-8 by slicing-by-8.
  test_backend on host: bch 49k -> 114k frames/s, bb 74k -> 180k frames/s. test_bch passes
  (1-12 errors corrected, 13 detected).

## Dead ends (measured, not kept)
- LDPC message compression (min0/min1/argmin + sign masks, ~3x less bnl traffic): bounded
  first by shrinking bnl to an L1/L2-resident region (wrong results, timing only): ~0.855 ms vs
  ~0.95 ms per update pass, i.e. at most ~10% single thread, while reconstruction adds ~11
  uops/link. Not implemented. (Short frames, fully cached, are ~50% faster per link, so memory
  matters more with 3 workers in parallel: 3 parallel ldpcbench runs dropped to ~3100 frames/s
  each vs ~6600 alone, partly host contention.)
- LDPC software prefetch of data[] PF_DIST check nodes ahead (1, 2, 4, 8, 16): no gain (noise).
- LDPC storing magnitudes instead of recomputing: 1.28 G vs 1.26 G cycles (slightly worse).
- LDPC dropping min_epu8 in v_mag (changes the -128 edge case): no measurable gain.
- LDPC specialising the layer for constant cnt (12 for 3/4): -12% instructions, -2% cycles;
  dropped for code size (S2X has cnt 1..28).
- Streaming stores (_mm256_stream_ps + sfence, 64-byte aligned batch buffers) for the PL data
  output: plsync samples 647-736 vs 675-692, no gain.

## Remaining hot spots (pipeline, s2_8psk34, -t 3, modified build)
Shares of perf samples: drx-timing ~39% (not mine), drx-filter ~14% (not mine), main thread
~13% (file read ~4%, drx_mt_push memcpy ~3.4%, page faults ~3%), LDPC workers ~23% (update
~14% of which update_first ~3.3%, demap ~4.2% with quantise, bad()/transposes ~2%), plsync ~9%
(rotate ~2.5% (memory bound, ~4 ns/sym in the pipeline vs ~1 in isolation), drx_pl_process
input memcpy ~1.5-2%, first-touch faults on batch buffers), output ~1.6% (bch ~1%).

## Ideas not tried (estimated gains)
- Remove the drx_mt_push input copy: zero-copy API (get a free sblock, fill it, commit) used
  by tools/dvbrx.c (not my file) for file and UHD input: ~3% of samples, ~0.08 cores.
- Memory footprint / page faults: 128 x 512 KB sblocks + 12 batches x 32 lanes x ~244 KB
  (~160 MB touched): sys 0.13-0.18 s per run = ~0.35 cores of this metric, mostly startup.
  LIFO free lists for batches/sblocks (fewer buffers touched, warmer reuse), or
  MADV_HUGEPAGE on the big buffers (THP is "madvise" on the host): est. 0.05-0.15 cores on the
  metric, little in steady state.
- drx_pl_process copies every input block into pl->buf (~1.5-2% of samples): process frames
  in place from the sblock, copying only frames straddling blocks: maybe half of that.
- 32APSK+ demap still ~5 ns/sym: exploit APSK ring structure or the existing candidate LUT
  for max-log minima (risky for exactness): maybe -30% of demap on APSK.
- LDPC early termination: final bad() is a full pass (~0.2-0.25 of an update); no exact cheap
  fusion found.
- LDPC lanes: with the faster decoder, -t 2 may be enough at 27.5 Msym/s; test real-time
  margin with fewer workers (less L3/DRAM contention).
- Timing loop (drx-timing, ~0.95 cores) is the largest consumer: fminf/fmaxf are library calls
  (fmaxf32/fminf32 + PLT ~9% of samples in that thread) because of no -ffast-math; use
  ternaries or intrinsics in frontend.c (not my file): likely the biggest single remaining win.

## Commands and scripts

Local build and checks:
```
cd /home/rbilly/claude/DVB-S_RX-agentD && make -C libdvbrx -j4 && . ./env.sh
tests/run_backend.sh; tests/run_pl.sh 0.01 0.5
python3 tests/run_s2x.py 3.0 | grep -c '^ok'; python3 tests/run_s2x.py 3.0 64APSK nopilots
libdvbrx/build/dvbrx -F work/bench/s2_8psk34.fc32 -S 55e6 -s 27.5e6 -r 0.2 -t 3 -o out.ts
```
Extra test files (2 sps, in work/benchD/ locally and ~/dvbrx-agentD-work/ on the host):
`python3 tests/txshape.py work/vec/<v>/plframe.fc32 work/benchD/<v>.fc32 --rolloff 0.2 --esn0 15 --freq 0.003 --sps 2`
for v = np_8psk3_4, 16apsk3_4, 32apsk4_5, 8psk3_4.

Host sync/build (scratch sync.sh did this):
```
rsync -a --exclude build --exclude work --exclude third_party --exclude prefix --exclude .git \
  /home/rbilly/claude/DVB-S_RX-agentD/ rbilly@10.5.1.2:~/dvbrx-agentD/
ssh rbilly@10.5.1.2 'make -C ~/dvbrx-agentD/libdvbrx -j2 CFLAGS="-O3 -march=native -fno-math-errno \
  -Wall -Wextra -std=gnu11 -ffp-contract=fast -fcx-limited-range -D_GNU_SOURCE -g \
  -fno-omit-frame-pointer" build/dvbrx build/dvbrx_file'
```
Host scripts (in ~ on 10.5.1.2; the remote login shell is zsh, so they are bash scripts):
- `~/benchD.sh <dir> <label> [n]`: dvbrx -t 3 on ~/dvbrx/work/bench/s2_8psk34.fc32, prints
  user/sys/real and cores = (user+sys)/0.4007. Usage: `~/benchD.sh dvbrx-agentD new 3`.
- `~/fileD.sh <dir> [n]`: dvbrx_file -s 2 -r 0.2 pinned to CPU 5 on the bench file and the
  three ~/dvbrx-agentD-work files, best Msps + packet counts.
- `~/abD.sh N "cmdA" "cmdB" ...`: interleaved runs pinned to CPU 5, minimum user cycles
  (perf stat -e cycles:u) per command. Used for all A/B comparisons because wall time was too
  noisy.
- `~/profD.sh <dir> <file|name> [n]`: perf record -F 2999 of dvbrx_file on CPU 5, top symbols
  and source lines in my files.
- `~/beD.sh <dir>`: builds test_backend and prints demap/ldpc/bch/bb frames/s on the xfecframe
  vectors (demap numbers include first-touch page faults; use demapbench instead).
- ~/dvbrx-agentD-work/ldpcbench.c: `ldpcbench ldpc.u8 bch.u8 3_4 6 20` (or `zero x 3_4 6 10
  [short]` for all-zero codewords); prints frames/s, iterations and an output checksum.
- ~/dvbrx-agentD-work/demapbench.c: `demapbench x8psk34.fc32 8PSK 3_4 200`; ns/symbol + LLR checksum.
Pipeline per-thread profile:
```
sudo rm -f /tmp/perfD.data; sudo perf record -F 1999 -o /tmp/perfD.data -- \
  ~/dvbrx-agentD/libdvbrx/build/dvbrx -F ~/dvbrx/work/bench/s2_8psk34.fc32 -S 55e6 -s 27.5e6 \
  -r 0.2 -t 3 -o /dev/null 2>&1 | tail -1
sudo perf report -i /tmp/perfD.data --no-children --sort comm --stdio -F sample,comm
sudo perf report -i /tmp/perfD.data --no-children --comm drx-plsync --sort dso,sym --stdio
```
Pilotless near-threshold comparisons (laptop scratch, now lost with /tmp): dvbrx_file
`-n <EsN0> -f <freq>` on work/vec/np_*/plframe.fc32 and work/vec/s2x/*_np/plframe.fc32 over
several EsN0 and 8 offsets (0.01 0.003 -0.02 0.0071 -0.0043 0.0151 -0.011 0.0003), summing TS
packets; compare against a baseline build (originals rebuilt in a scratch copy). The
PLL_BLOCK define can be overridden with -DPLL_BLOCK=n for such tests.
Thread safety: libdvbrx built with -fsanitize=thread, dvbrx -t 3 on work/benchD/8psk3_4.fc32:
only the 2 existing races in tools/dvbrx.c (print_stats/file_thread), none in libdvbrx.
