# Agent C optimisation notes: DVB-T inner/outer layers (conv.c, outer.c, dvbt.c)

Host i5-9500 (GCC 14), single thread `dvbrx_file` on CPU 4, min of 3, (user+sys)/signal seconds.

| | baseline | final |
|---|---|---|
| DVB-T 8K 64-QAM 5/6, 10.24 Msps, 3.0 s | 2.619 s (0.873 cores) | 1.309 s (0.436 cores), -50% |
| DVB-C 256-QAM 6.9 Msym/s, 2.0 s | 1.175 s (0.588 cores) | 0.948 s (0.474 cores), -19% |

All outputs are bit-exact with the unmodified code: sn.fc32 (`-l 3/4` and `-l auto`) ba1eb039...,
tn.fc32 bf495960... on the laptop with GCC 16 and c8483ff9... on the host with GCC 14 (each the
baseline md5 for that compiler). The tn decode still gives 310 uncorrectable packets on GCC 16 and
327 on GCC 14. dvbt_8k64 and dvbt_2k16 decode with 0 uncorrectable and match the reference .ts. The
dvbc256 output .ts is also unchanged. The only output that changes is the MER statistic, now summed
in 8 lanes; it prints the same to 2 decimals on every file I tried.

## Changes, in the order made, with host DVB-T totals

1. **Viterbi (conv.c), 2.61 → 2.19 s.** This uses AVX2 intrinsics, and the GCC
   vector-extension code is kept as the portable fallback.
   - Two metric layouts alternate. In "Y", Q is natural and goes to P; in "X", P goes back to Q.
     The deinterleave (packus of the even and odd int16) only needs the lane fix (`vpermq`) every
     other step; before, GCC used 2 `vpermq` per vector pair every step.
   - The decision words come out in natural order: 2 `packsswb` and 2 `movemask`, plus a `vpermq`
     on the X steps. GCC's old code used `packuswb` + `vpermq` ×4.
   - Branch metrics: a_j = SGX_j·(sx+sy) when bits 0 and 4 of j are equal, else SGX_j·(sx−sy). A
     prepass writes (u,w) and (w,u) dwords. The loop then uses a load-only `vpbroadcastd` and
     `vpsignw`, where it used to do 2 `vpbroadcastw` (p5) + 4 `vpmullw`.
   - `__attribute__((optimize("no-tree-slp-vectorize")))` on `trellis()` is needed. Without it,
     GCC 14 merges the two dec-word stores with `vpinsrq` on p5, which cost about 10%.
   - The traceback keeps the state unmasked and uses `bt` + `adc`, a 2-cycle dependency chain
     (it was about 4 cycles). The ring is walked in contiguous segments with no per-step wrap
     test.
   - Renormalisation timing is free: decisions only depend on metric differences.
   - Micro-benchmark (`bconv`, 8192-pair calls): 8.44 → 5.0 ns/bit. The trellis alone is about
     16-17 cycles per step; the port model's floor for this scheme is about 12.5.
2. **demap_symbol (dvbt.c), 2.19 → 1.66 s.**
   - `interp_channel()` interpolates the channel on every carrier, with no `/3` and no branches.
   - Cells are stored as int16 soft rows of 8 (conv_soft applied early, which is exact).
   - The bit deinterleaver and demux are a precomputed gather table.
   - The AVX2 cell loop handles 8 cells with `vgatherdpd` and a branch-free conv_soft (clamps in
     the same NaN order). The old scalar conv_soft branches mispredicted a lot.
3. **Outer layer (outer.c), 1.66 → 1.55 s for DVB-T and 1.17 → 1.01 s for DVB-C.**
   - The synced path packs 8 bits per multiply: `(x * 0x8040201008040201) >> 56`.
   - The Forney deinterleaver is one 4 KB ring: output q = input q − (11 − q%12)·204. Bytes are
     processed in packet chunks, and the `out_q` and `fifo` fields are gone.
4. **RS (outer.c), 1.55 → 1.46 s for DVB-T and 1.01 → 0.95 s for DVB-C.**
   - The syndrome Horner (3264 table lookups per packet) is replaced by r = c·x^16 mod g,
     slicing-by-4 in two u64 (51 steps × 4 lookups).
   - When r is not zero, the syndromes are computed as S_i = r(α^i)·α^(−16i).
5. **conv_depuncture (conv.c), 1.2 → 0.2 ns per soft bit.** Four puncturing periods are done as
   k `pshufb` (SSSE3). The slow path still handles the phase and the tail.
6. **demod_window (dvbt.c), 7.3% → 2.8%.**
   - The carrier reorder used an `idiv` per carrier (`% n`); it is now 2 contiguous loops.
   - The NCO chunks (each restarts from cexp every 256 samples) run as 4 independent chains side
     by side.
7. **shift_refs (dvbt.c), 4.5% → 3.9%.** The h3 and prev phasor chains run interleaved in a
   `noinline turn_chains()`.
8. **Demap gather and MER, 1.48 → 1.30 s.** The gather now uses `vpgatherdd`, with even/odd index
   halves and one `vpblendw`. MER is accumulated in 8 lanes.

Final host profile for DVB-T: trellis about 29%, fft_run 19% (not mine), resamp_run 15% (not mine),
demap about 8%, traceback about 4.4%, turn_chains 3.9%, depuncture 2.8%, demod_window 2.8%.

## Bit-exactness traps (read before touching the DVB-T float code)

- The tn md5 depends on how GCC contracts FMAs, and therefore on inlining and SLP decisions anywhere
  in `run()` and `track_symbol`. Inlining demap_symbol into run() changed track_symbol's rounding,
  so demap_symbol must stay `noinline`.
- GCC 14 and GCC 16 contract some loops differently. In shift_refs' cp_prev loop, GCC 16 fuses and
  GCC 14 does not, so that loop was left in its original source form.
- The explicit `fmaf()` code in interp_channel, the cell loop, demod_window and turn_chains copies
  the pattern that both compilers produce. I read it from `-fdump-tree-widening_mul`.
- Always check tn on both the laptop and the host. Each compiler has its own baseline md5.
- Compare to see where values diverge: dump `d->soft` and per-cell values from an original build
  and a new build, then `cmp`.

## Dead ends and measurements

- The first demap restructure with scalar math (H array, int16 rows, gather table) gained about 0%
  on the host. The cost was the branchy scalar conv_soft and the division per cell; vectorising
  was what helped.
- demod_window with all 32 NCO chains interleaved went from 7.3% to 10.7%, slower. The chunks are
  2 KB apart and land in the same L1 sets. Four chains are fine.
- Viterbi `asm("" : "+r"(w))` did not stop the dec-word SLP merge; the optimize attribute did.
- The serial prev-phasor chain in shift_refs (6817 steps × about 8 cycles) can't be shortened
  bit-exactly. Only the TPS carriers of `prev` are used, but they need the chain up to k = 6799.

## Ideas not tried, with estimated gains

- **Viterbi layout schedule, about 5% of DVB-T.** Use `vpunpck*` (u16/u32) + `vperm2i128`, 4
  phases with phase-specific sign constants and a decision `pshufb`. A port-model search
  (scratchpad sched3.py, my own model) gives about 10.1 cycles per step against 12.5 for the
  current X/Y scheme. The best schedule found is
  `['p128v1','u16v0','p128v0+u32v0','u16v0']`, start layout (Va=0,Vb=5,l=3,e2=1,e1=2,e0=4).
  Verify with the harness that compares decisions against the original conv.c.
- **Packed traceback output, about 1-1.5%.** After the unmasked bt/adc traceback, bits 5..63 of
  the state register are the decided bits in time order. Emit 56 bits per 56 steps instead of a
  store per bit. This needs an `outer_bytes()` API, with the bit-offset shift on the synced path
  and the sync search unpacking.
- **Sync search in outer_bits while unsynced, acquisition time only.** It is about 9 ops per bit
  with sbyte() at 1632 offsets. It could use packed bits and 16-bit compares.
- **Demap.** The `vgatherdpd` of x and hk could become contiguous loads over all carriers, with
  the rows copied by dk. The two-chain MER is no longer needed. Rest of demap about 2-3%.
- **turn_chains.** Skip the prev multiply for non-TPS carriers; this gives nothing on latency.
- **RS slicing-by-8.** About 0.5% for DVB-T and 1% for DVB-C. It needs 32 KB of tables, which is
  too much L1.

## Commands

```
# build (host)
rsync -a --exclude build --exclude work --exclude third_party --exclude prefix --exclude .git \
  /home/rbilly/claude/DVB-S_RX-agentC/ rbilly@10.5.1.2:~/dvbrx-agentC/
ssh rbilly@10.5.1.2 'make -C ~/dvbrx-agentC/libdvbrx -j2 CFLAGS="-O3 -march=native -fno-math-errno -Wall -Wextra -std=gnu11 -ffp-contract=fast -fcx-limited-range -D_GNU_SOURCE -g -fno-omit-frame-pointer" build/dvbrx build/dvbrx_file'
# bench (host ~/benchC.sh <dir> <label>): min of 3, taskset -c 4
dvbrx_file -S 10.24e6 -T 8 ~/dvbrx/work/bench/dvbt_8k64.fc32 /dev/null   # 3.0 s of signal
dvbrx_file -s 2 -c 256 ~/dvbrx/work/bench/dvbc256.fc32 /dev/null         # 2.0 s of signal
# md5 on host (~/hostcheckC.sh <dir>; sn/tn copies in ~/agentC-ref), baseline in ~/dvbrx-agentC-base
# profile
sudo rm -f /tmp/perfC.data; sudo perf record -F 4999 -g -o /tmp/perfC.data -- taskset -c 4 <cmd>
sudo perf report -i /tmp/perfC.data --no-children --sort symbol --stdio | grep -E '^ +[0-9.]+%'
sudo perf annotate -i /tmp/perfC.data <sym> --stdio
sudo perf stat -e cycles,instructions,uops_dispatched_port.port_5,uops_issued.any -- <cmd>
# contraction patterns
gcc <CFLAGS> -c dvbt.c -fdump-tree-widening_mul -o /dev/null   # look for .FMA/.FMS/.VEC_FMADDSUB
```
Unit harnesses (scratchpad, they compile the original conv.c/outer.c under renamed symbols):
- `tconv.c` checks depuncture for all rates and phases, block Viterbi and streaming Viterbi
  against the original, and times them.
- `bconv.c` is the Viterbi micro-benchmark.
- `touter.c` holds a TS encoder (PRBS, RS, Forney) with noise, an inverted stretch and garbage
  bursts that cause losses and resets, and compares the old and new outer layers byte for byte.

## State at stop

No optimisation is in progress. The last change, the vectorised demap gather and the 8-lane MER
(item 8), is finished and md5-checked on both compilers. The quick matrix
(`python3 tests/matrix.py dvbt dvbs dvbc --quick`) on the final build passed all 27 cases: 12 DVB-T,
10 DVB-S, 5 DVB-C, 0 FAIL. The full 85-case matrix has not been run. To re-validate, rebuild,
re-run that command and the md5 set above, on both the laptop and the host.

The next candidate, not started, is the Viterbi 4-phase unpack schedule; see "Ideas not tried".
Validate it with tconv.c (decisions identical to the original conv.c) and then the md5 set.
