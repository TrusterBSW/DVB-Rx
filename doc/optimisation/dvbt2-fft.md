# DVB-T2 CPU optimisation notes (agent B)

Scope: `libdvbrx/src/dvbt2.c`, `libdvbrx/src/fft.c`, `libdvbrx/src/fft.h` only.
Target: i5-9500 (AVX2/FMA, no AVX-512), GCC 14 `-O3 -march=native`, no `-ffast-math`.
Original sources and reference outputs: `work/refB/` (`dvbt2.c`, `fft.c`, `fft.h`, `*.ts`, `*.log`, `run.sh`).
Intermediate snapshot (before structured bit deinterleaving): `work/refB/dvbt2_stage3.c`.

## Result (host 10.5.1.2, `taskset -c 2,3`, min of 5 runs, run alternately with the baseline)

| Signal | Baseline cores | Final cores |
|---|---|---|
| 8K 64QAM 3/4, `~/dvbrx/work/bench/dvbt2_8k.fc32` @ 10.24 Msps | 0.743 (median 0.746) | 0.292 (median 0.296) |
| 32K 256QAM rot 2/3, `dvbt2_def` @ 9142857.15 | 0.932 (median 0.964) | 0.349 (median 0.370) |

Per thread (perf `--sort comm` share × total cores):

| Thread | 8K base | 8K final | 32K base | 32K final |
|---|---|---|---|---|
| drx-t2fec0 + t2fec1 | 0.32 | 0.08 | 0.47 | 0.12 |
| drx-plsync (demod) | 0.30 | 0.10 | 0.34 | 0.13 |
| drx-filter (resampler, not ours) | 0.08 | 0.075 | 0.08 | 0.067 |
| dvbrx main (file read, bench artifact) | 0.04 | 0.036 | 0.04 | 0.03 |

Host load was about 3.5 from the other agents, so single runs vary by a few percent.

## Steps and measured gains (host, cores, 8K / 32K)

| Step | 8K | 32K |
|---|---|---|
| Baseline | 0.744 | 0.939 |
| 1. New FFT, demod_symbol, store_cells dense pass, data_symbol modulos, shift_refs, vectorized demapper | 0.397 | 0.470 |
| 2. AVX2 pack, find_p1 (table, bounded scan, persistent buffers), store_cells with inverse FI tables | 0.304 | 0.367 |
| 3. Time deinterleaver blocked by 8 rows, memcpy runs | 0.299 | ~0.36 (noise) |
| 4. Structured bit deinterleaving (column runs + parity transpose) | 0.292 | 0.349 |

### What was changed (details)

**fft.c: own radix-4 FFT (API unchanged)**
- **Structure:** decimation in time. A first pass (radix-8 if log2 n is odd, else radix-4, no twiddles) reads x in bit-reversed order into a work buffer. Then radix-4 passes of span R, 4R .. n/4 run, and the last pass writes back to x.
- **Vectorization:** 4 complex values per `v8f` (GCC vector extensions, `__builtin_shufflevector`, so clang builds it too).
  - The first pass processes 4 blocks at once (consecutive sources r..r+3) and then does a 4×4 complex transpose.
  - Twiddles are stored expanded as (wr, wr, ...) and (-wi, wi, ...), so a complex multiply is one swap, one mul and one FMA.
- **Other sizes:** the inverse transform is conj(FFT(conj x)) (no caller uses it). n < 64 keeps the old radix-2 code.
- **New constraint:** `struct fft` now owns a work buffer, so one call at a time per struct (every current user owns its own).
- **Laptop timing, per FFT:**

  | n | Old (µs) | New (µs) | Speedup |
  |---|---|---|---|
  | 1K | 4.5 | 0.80 | 5.7× |
  | 2K | 9.9 | 2.3 | 4.3× |
  | 4K | 20.3 | 6.7 | 3.0× |
  | 8K | 50.5 | 13.8 | 3.65× |
  | 16K | 127 | 32.7 | 3.9× |
  | 32K | 279 | 65.3 | 4.3× |

- **Accuracy:** relative error vs a double-precision DFT is 1.05e-7 (1K) to 1.22e-7 (8K); the old FFT had 1.14e-7 to 1.31e-7.
- **Test harness:** `ffttest/t.c` in the session scratchpad. It is not kept in the repo; it can be rebuilt from the old `fft.c` in `work/refB` by renaming its symbols.

**dvbt2.c: FEC thread (`decode_job`, `setup_plp_fec`)**
- **Cell deinterleave:** `(cellperm[i]+P) % ncells` became a conditional subtract. Cells are gathered into separate float arrays ci/cq/cc, padded so whole vectors can be read; the rotated Q of the last point comes from cell 0.
- **Pass 1 (8 cells per vector, in place):**
  - Derotation.
  - kr = c1 c2 / (ct² c2 + sn² c1) and ki likewise: 2 divisions instead of 6; non-rotated takes kr = ki = c1.
  - MER with truncation (`vtrunc` of min(|w|/2, na-1)) and lane masks; err/sig accumulated per lane, so the order differs from the original sum.
- **Pass 2 (32 cells per step):**
  - The PAM levels w, |w|-t, ... are multiplied by (weight × k) in the same rounding order as before.
  - Clamp ±127, `cvtps2dq` (round to nearest even, same as lrintf), `packs` to bytes.
  - Output goes into one plane per bit. With 2 cells per demux word the plane is split into even cells then odd cells (`to_llr32_split`).
- **Bit deinterleaving:**
  - Interleaver column c is the plane of bit `mux[c] % m`, cell parity `mux[c] / m`, rotated by `tw[c]` → 2 memcpy per column.
  - llr[0..K) is the column order directly; parity: `llr[K + Q s + t] = il[K + 360 t + s]`.
  - QPSK keeps a uint16 lookup table.
- **Checks:** LLRs vs the original float demapper on 8 noisy captures: ~0.004% of LLRs differ, all by ±1, no sign flips. err matches to 6 digits.

**dvbt2.c: demod thread**
- **`demod_symbol`:** the rotation is vectorized (4 lanes, re-anchored with an exact `cexpf` every 256 samples); the carrier copy-out uses two segments instead of `% n` (that was an idiv per carrier).
- **`store_cells`:**
  - Interpolated channel for all carriers with no `k/dx` division.
  - Dense vector loop computing x·conj(h)/(|h|²+1e-12) and |h|².
  - Compaction of the data carriers.
  - Cells are then written in order through inverted frequency-interleaver tables (`invert_perm` in `make_layout`; the layout `h*` arrays now hold the inverse).
- **`data_symbol`:** continual-pilot residues are precomputed (`cp_res`); the grid residue is advanced incrementally.
- **`shift_refs`:** the grid rotation is a double-precision recurrence, re-anchored every 64 points.
- **`interp_dense`:** rotations per r are computed once.
- **`find_p1`:**
  - Prefix-sum buffers are persistent, the e^(-j2πi/1024) table replaces a `cexpf` per sample, and `sqrt` replaces `cabs`.
  - The end-of-frame check scans 4096 samples instead of 65536. The result is identical: the scan stops 1024 samples past the peak, and any s > save+32 → restart in both versions.
- **Time deinterleaver (`decode_plp`):** rows are processed 8 at a time when each row is a run of consecutive cells (`plp_run`), with a fallback per element otherwise. Without TI it copies `memcpy` runs. The job buffers are byte-identical to the old loop (nti 1..193, nc 5..180).
- **`bits()`:** unsigned shifts (UBSan reported signed overflow on 35-bit fields; behaviour unchanged).

## What did not help, or needed rework (with numbers)

- **`__builtin_convertvector(v8i → int8 x8)`:** on AVX2 it compiles to 16 `vpextrd` plus scalar shifts/ors, because there is no `vpmovdb` without AVX-512 (the laptop has AVX-512, so this only showed up in host asm). Decode_job was 5.2% of total at that line alone; it was replaced by intrinsics `packs` + `permutevar`.
- **Vector min/max written with masks** (`(a&m)|(b&~m)`): GCC emits `vpblendvb` (2 uops), not `vminps`. Replaced by `_mm256_min_ps` under `#ifdef __AVX2__`; C has no vector `?:`.
- **Inverse-permutation (sequential write) store_cells:** about 0 gain alone. store_cells was 6.5% of 0.40 cores and became 8.7% of 0.30 cores, i.e. 0.026 cores either way. Random writes became random reads plus a compaction pass. Kept because it is not worse and simplifies the scatter; the cost is memory-bound (frame buffer 4.8 MB per 8K frame, first touch).
- **TI blocking (step 3):** 0.304 → 0.299 cores, within noise. The copy is still ~4% of samples: it reads the frame buffer, which is already out of cache (L3/DRAM).
- **Rejected without trying:**
  - FFTW in fft.c: the planner is not thread-safe, `chan.c` creates plans in other threads, the fix needs `fftwf_make_planner_thread_safe` (libfftw3f_threads, Makefile not ours), and results for DVB-T/C would change.
  - AVX2 gathers (`vgatherdps`): slow on Coffee Lake with the GDS/Downfall microcode.
- **Pipeline SNR display** (`dvbrx -F ... -S 9142857.15`): varies run to run in the baseline too (111/84/62 dB). It depends on timing (the resampler picks up the clock step asynchronously); not a regression.

## Remaining hot spots (final build, 8K bench, % of all samples; total 0.292 cores)

| Function | % | ≈ cores | Notes |
|---|---|---|---|
| resamp_run (drx-filter) | 21–25 | 0.07 | not ours |
| drx_ldpc_decode (2 threads) | ~10 | 0.03 | not ours |
| decode_job | ~11 | 0.03 | cell-deinterleave gather from job buffer ~4.5%; demap passes ~1.5%; column copies + parity transpose now small |
| store_cells | 7.5 | 0.022 | interpolation 1.4%, compaction 1.4%, ordered gather 1.7%, dense equalise ~1% |
| fft_run | 6 | 0.018 | ~19 µs per 8K FFT on host (14 µs laptop) |
| run (decode_plp TI copy, data_symbol) | 5.6 | 0.016 | TI copy ~4% |
| demod_symbol | 3.4 | 0.010 | reads fresh input samples (L3) |
| find_p1 | 2.7 | 0.008 | mostly the initial acquisition scan; one-time in real use |
| libc memcpy in plsync | 2.4 | 0.007 | `drx_dvbt2_demod` copies input into `d->buf`, plus `compact()` memmove |
| bch / crc8 | ~3 | 0.009 | not ours |
| main thread file read (kernel memcpy) | ~8 | | bench artifact (`-F`) |

32K is similar: store_cells 9.4%, fft 5.6%, decode_job 11.7%, ldpc 12.4%, resamp 17.7%.

## Ideas not yet tried (estimated gain, cores at 8K)

1. **Fuse time + cell deinterleaving into one gather in the FEC thread** (job = PLP cells in address order via memcpy runs; FEC thread computes the TI+L0 source index per cell). Removes the strided TI copy in plsync, roughly −0.012 plsync, slightly more gather work in FEC. Needs care: the TI address arithmetic per block.
2. **Avoid the input copy in `drx_dvbt2_demod`** (resampler output written straight into `d->buf`, as `drx_dvbt2_process` already does), plus a cheaper `compact()` (ring buffer): ~0.005–0.007. Touches the API path used by rxmt.c (not ours).
3. **store_cells:**
   - Vectorize the grid interpolation (process 4 grid segments per vector for dx ≥ 4, or precompute (1-t)S_r, tS_r): ~0.003.
   - Precompute q→k tables per (l mod Dy, parity) for Dy ≤ 4 to skip compaction (memory: 2·Dy·cdata ints): ~0.003.
4. **FFT:**
   - Compact twiddles for the last pass (8K: 128 KB of expanded twiddles; 32K: 512 KB).
   - Radix-8 passes (fewer passes over 256 KB for 32K).
   - Fuse the carrier-offset rotation into the first pass and the scale/fftshift into the last.
   - Estimated 20–30% of fft_run, ~0.004–0.006.
5. **find_p1 acquisition:** float accumulators per 1024-sample block (doubles only across blocks), vectorized lag products: ~0.005 on the 3 s bench, ~0 in steady state.
6. **NFEC:** each FEC thread is now ~0.04 cores at 8K (32 Mbit/s); NFEC = 1 may suffice with less wakeup/sync overhead. Not measured; check 32K 256QAM worst case first.
7. **Not ours, for the record:**
   - Resampler (0.07, largest item).
   - LDPC (0.03; e.g. early termination, lanes).
   - BCH + crc8 (~0.009; table-driven or CLMUL CRC, syndrome only when LDPC parity check fails).

## Verification procedure (laptop)

```sh
cd /home/rbilly/claude/DVB-S_RX-agentB && make -C libdvbrx -j4 && . ./env.sh
work/refB/run.sh work/outB      # runs the 7 reference files (dvbrx_file -2 8), prints stats + time
for f in dvbt2_8k dvbt2_def t2_256_34 t2_q4_23 t2_norot x16p1 x32p8; do cmp work/refB/$f.ts work/outB/$f.ts && echo "$f identical"; done
libdvbrx/build/dvbrx -F work/real/dvbt2_8k.fc32 -S 9142857.15 -2 8 -o work/outB/pipe8k.ts; cmp work/outB/pipe8k.ts work/outB/dvbt2_8k.ts
python3 tests/matrix.py dvbt2 --quick      # 41 PASS + 1 SKIP
python3 tests/matrix.py dvbt2              # 98 PASS + 1 SKIP (copy: work/matrixB_dvbt2_results.txt)
python3 tests/matrix.py dvbt dvbc --quick  # 17 PASS (fft.c is shared)
```

- **Noisy test captures:** `work/noisyB/*.fc32`, decoded with `dvbrx_file -2 8 -S 10240000 <f> /dev/null`.
  - Generated with `tests/dvbt2_gen.py` + `tests/channel.py --in-rate 9142857.142857143 --out-rate 10240000 --ppm 15 --freq 3000 --snr N`.
  - Captures: q4n, q16s, q64n, q64norot, q64_32k, q256n, q256s, tiauto.
- **Sanitizers** (binaries in `work/tsanB/`):
  - TSan: `gcc -O1 -g -fsanitize=thread ... $(ls src/*.c) tools/dvbrx_file.c`.
  - ASan + UBSan: `-fsanitize=address,undefined -fno-sanitize-recover=all`, run with `ASAN_OPTIONS=detect_leaks=0`; the leaks are in dvbrx_file.c main.
- **Bit-exact demapper / TI comparison:** debug copies of dvbt2.c with the original code inlined next to the new code (`LLRCMP` / `TICMP` lines on stderr). The scripts were in the session scratchpad (`llrcmp/mkdbg.py`, `llrblock.txt`); rebuild them from `work/refB/dvbt2.c` if needed.
- **Baseline binaries** for comparison: `work/baseB_build/`.

## Benchmark and profiling (host 10.5.1.2)

```sh
rsync -a --exclude build --exclude work --exclude third_party --exclude prefix --exclude .git \
    /home/rbilly/claude/DVB-S_RX-agentB/ rbilly@10.5.1.2:~/dvbrx-agentB/
ssh rbilly@10.5.1.2 'make -C ~/dvbrx-agentB/libdvbrx -j2 CFLAGS="-O3 -march=native -fno-math-errno -Wall -Wextra \
    -std=gnu11 -ffp-contract=fast -fcx-limited-range -D_GNU_SOURCE -g -fno-omit-frame-pointer" build/dvbrx'
# baseline tree (unmodified copy): ~/dvbrx-agentB-base (same build command)
# 32K file copied to ~/dvbrx-agentB-def.fc32 (from work/real/dvbt2_def.fc32), rate 9142857.15
ssh rbilly@10.5.1.2 '~/benchB2.sh'   # base/new alternated, 5 runs each, 8K + 32K, prints min/median cores
```

`~/benchB2.sh` measures `(user+sys)/signal seconds` with `TIMEFORMAT="%U %S %R"` and runs:

```sh
time taskset -c 2,3 <tree>/libdvbrx/build/dvbrx -F <file> -S <rate> -2 8 -o /dev/null
```

- 8K: `~/dvbrx/work/bench/dvbt2_8k.fc32`, 10.24e6, 3.0 s.
- 32K: `~/dvbrx-agentB-def.fc32`, 9142857.15, duration = size/8/rate.
- `~/benchB.sh <tree> <tag>` does 3 runs of one tree.

Profiling:

```sh
sudo rm -f /tmp/perfB.data
sudo perf record -F 999 -g -o /tmp/perfB.data -- taskset -c 2,3 ~/dvbrx-agentB/libdvbrx/build/dvbrx \
    -F ~/dvbrx/work/bench/dvbt2_8k.fc32 -S 10.24e6 -2 8 -o /dev/null 2>&1 | tail -1
sudo perf report -i /tmp/perfB.data --no-children --sort comm,symbol --stdio | grep -E '^ +[0-9.]+%' | head -25
sudo perf report -i /tmp/perfB.data --no-children --sort comm --stdio | grep -E '^ +[0-9.]+%'   # per thread
# per source line (perf annotate of the inlined fec_thread printed nothing; this works):
sudo perf report -i /tmp/perfB.data --no-children --comm drx-plsync --sort symbol,srcline --stdio | grep -E '^ +[0-9.]+%' | head -40
sudo perf report -i /tmp/perfB.data --no-children --comm drx-t2fec0 --sort symbol,srcline --stdio | grep -E '^ +[0-9.]+%' | head -40
sudo perf annotate -i /tmp/perfB.data --stdio -s store_cells | grep -E '^\s+[0-9]+\.[0-9]+' | sort -rn | head -30
# host asm (the laptop has AVX-512 and generates different code!):
objdump -d --no-show-raw-insn -l ~/dvbrx-agentB/libdvbrx/build/dvbt2.o > ~/asmB.txt
sudo rm -f /tmp/perfB.data
```
