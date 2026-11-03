# Agent A: CPU optimisation notes (front-end + DVB-C demodulator)

Files changed: `libdvbrx/src/frontend.c`, `libdvbrx/src/dvbc.c`. `frontend.h` is unchanged.
The original sources are in `work/agentA_tools/orig_sources/`, and the helper scripts are in `work/agentA_tools/`.

## Results (host 10.5.1.2, i5-9500 at 4.1 GHz, `taskset -c 0,1`, min of 3, base = unmodified copy run alongside)
Cores = (user+sys)/duration. The S2 numbers use the 0.4007 s divisor that was given. **The S2 file is in fact 0.8014 s long**
(44.08 M samples at 55 Msps = 22.0 M symbols), so the true core counts are half of these.

| bench | base | now (TDELAY 2) |
|---|---|---|
| DVB-S2 8PSK 3/4 27.5 Msym | 3.00 - 3.20 | 2.47 - 2.71 (about -17 %) |
| DVB-C 256QAM 6.9 Msym | 0.632 - 0.639 | 0.48 - 0.51 (about -22 %) |

Single-thread stage benches (host, warm buffers):
- timing: 16.0 -> 7.7 ns/symbol (TDELAY 2); 7.35 with TDELAY 4; 10.5 with TDELAY 1.
- filter (mf + NCO): 5.5 -> 2.9 ns/sample.
- dvbc process (with outer): 61 -> 51.6 ns/symbol. The DD part alone: 42.7 -> 33.7 ns. outer.c takes about 17 ns/symbol (not my file).

## Changes
### frontend.c
1. **mf**: explicit v8f, 8 accumulators (64 floats, RB 32 -> NACC 8). The old 4 accumulators were bound by FMA latency.
2. **nco**: there is no per-sample recurrence any more. A 64-entry phasor table exp(-j2πfk) is rebuilt when the frequency changes,
   and an exact phasor (double) is computed per 64-sample block. The loop is vectorised, and it is fused with the copy into `x`
   (one pass instead of memcpy + in-place pass).
3. **timing()**:
   - fminf/fmaxf were **library calls** (not inlined without -ffast-math). They cost about 5 % of all S2 CPU and spilled every register.
     The bounds are now branches, and NaN still ends at the lower bound.
   - The position is in 32.32 fixed point (`tpos`, `thalf`, `tsps`). There is no floorf in the chain, and it is exact.
   - mu comes from the fraction bits as a float in [1,2), and the Lagrange polynomial is rewritten in v = 1 + mu (Estrin).
   - Both interpolants are vectorised (v8f coefficients with the AGC gain folded in, then a 4-way reduction).
   - The loop works in fixed-point units: K1, K2, vi and the bounds are scaled by sps·2^32. `vi` is stored scaled, and `timing_drift`
     divides it back.
   - The AGC power is computed after the loop, from `out` (one register less).
   - The advances ring (`tadv[TDELAY]`, int64) is in GPRs, and the loop is unrolled by TDELAY.
   - Verified: with TDELAY 4, the strobes are **identical** to base over 1 M symbols, and the symbols differ by -118 dB
     (`strobe_test.c`, which also exercises the max_out path).
### dvbc.c
1. DD/CMA loops are in `run_dd()` and `run_cma()`, with the scalar state in locals (`struct eq`).
   Before, stores to the `uint8_t` bit buffer aliased the receiver struct, so the loop state was reloaded each time.
2. **Exact LMS reformulation**: the previous symbol's tap update is pending (`ep`), and
   y = Σ(w+ep·conj(x))x' = T + ep·U. T and U are summed over the old line and do not wait for ep. The taps are then updated.
3. A square-QAM decision per axis (`lvl[]` table) replaces decide() rotations and lookups. Cross constellations still use decide().
   The bits are written with one 8-byte store (`lsb_bytes`), and the bits buffer has +8 bytes of slack.
4. The taps and delay line stay in the struct, 32-aligned (`aligned_alloc(64)`). This was measured faster than holding them in
   registers (51.6 vs 53.5 ns).
   - Verified: base and new give the same packets and RS counts on the bench symbols (67314 packets, 19 bytes fixed).

## Dead ends (measured)
- PLL split phasor (expj(phase+omega)·expj_small(correction), exact): slower on the host (55.7 vs 51.6 ns/sym, more uops).
- Registers for the taps/line in dvbc: 53.5 vs 51.6 ns.
- Unrolled vs shifting advance ring in timing: no change (10.2 vs 10.25 cold).
- TDELAY 8: no faster than 4. The loop is bounded by the RS/ROB window (about 70 uops/symbol, chain about 60 cycles),
  not by the delay.
- DD_SETTLE 8192/16384/24576 in dvbc: did not remove the acquisition errors.

## Open question 1: timing-loop delay (decision: TDELAY 2)
- The matrix test generators are **unseeded** (`np.random.randn` in channel.py and txshape.py), so every run uses new noise.
- DVB-C 256QAM 2.226 sps over 4 realisations, uncorrectable counts (threshold 20):
  - base (= TDELAY 4, bit-identical results): 33 22 51 10
  - TDELAY 1: 0 21 36 12
  - TDELAY 2: 9 24 52 12
  - The case fails about 50 % of the time **whatever the delay**. All other DVB-C cases give 0 to 8 errors.
  - The bad packets are always the first N after outer sync, so this is a DD convergence / deinterleaver start problem.
- The real dvbc256 file (dvbrx mt) gives err 32 (TDELAY 1), 45 (TDELAY 2), 44 (TDELAY 4 = base). With shifted file starts
  (skip k·7919 samples) the counts vary chaotically for every variant:
  - td1: 7 131 24 21 41
  - td2: 5 **23995** 17 13 42
  - td4: 4 635 22 10 33
  - base at k=2: 438
  - k=6..8, base/td1/td2/td4: 34/27/14/34, 66/62/64/65, 4/4/4/4
  - **Not resolved**: the 23995 case (td2, skip 15838 samples) is deterministic and very bad (24k packets at the start).
    It should be investigated before integrating TDELAY 2. TDELAY 4 (= base behaviour) avoids that particular trajectory
    but gives 635 errors there.
- A DD step-size gear shift (MU_DD 1e-3 while count < 20000) reduced the 256QAM 2.226 errors to 0 1 0 0 (td4) or 32 0 0 50 (td2).
  It is a behaviour change and is **not applied**.

## Ideas not tried (estimated gain)
- timing: drop the `append` memcpy for filtered input by reading the caller's buffer directly, with only a small junction copy.
  memcpy is about 14 % of the timing stage (also 4K-aliasing prone). Estimate -0.04 cores at S2 27.5 Msym.
- timing: batch processing (SoA) of TDELAY symbols. Estimate up to 2x on the timing stage.
- mf: 4 shifted aligned copies of x to avoid split loads. Estimate +25 % on mf (about -0.04 cores).
- dvbc: rot by recurrence with renormalisation (saves the expj, about 25 uops/symbol). Small numerical change.
- outer.c per-bit processing is 17 ns/symbol for DVB-C (other agent's file).

## Validation status (local laptop, TDELAY 2)
- `tests/run_pl.sh 0.01 0.5`: 5 lines with 0 bad. PASS
- `run_s2x.py 3.0`: 55 ok. PASS
- `matrix.py dvbs dvbc`: 24/25 PASS. The only failure is DVB-C 256-QAM 2.226 sps with 33 uncorrectable (the flaky case above).
- `sn.fc32`: ts 5151 packets, RS fixed 1632 bytes, 0 uncorrectable (was 1675/1666).
- real dvbc256 (dvbrx): LOCK QAM256, ts 83666 err 45 (was 36 / 44).

## Commands
- Build on the host: `work/agentA_tools/syncA.sh [-DTDELAY=n]`
  (rsync + `make CFLAGS="-O3 -march=native ... -g -fno-omit-frame-pointer" build/dvbrx`)
- Bench: `~/benchA.sh <dir> 3` on the host (it is `benchA.sh`: S2 and DVB-C commands, `taskset -c 0,1`, min of 3, logs in `/tmp/benchA_*`).
- Profile: `~/perfA.sh <dir> s2|c [N]`. Then `sudo perf annotate -i /tmp/perfA.data --stdio | awk '/Percent/{n++} n==1'`.
  `-s sym` was unreliable.
- Stage benches:
  - timing / filter, warm: `split_bench.c` with REP=2, via `sbhost.sh [-DTDELAY=n]`.
  - DVB-C single thread: `dvbc_bench.c`, symbols at `~/benchA_work/dvbc_sym.fc32`; variants via `varbench.sh <names>`
    (files `scratch var/dvbc_<name>.c`).
- Delay statistics: `work/tdtest/gen.sh R` (realisations of every matrix DVB-C case; about 1.8 GB, delete when done),
  `work/tdtest/run.sh "<glob>" <variants>`, variant binaries in `work/tdbin` (`mktd.sh`, `mkvar.sh`).
