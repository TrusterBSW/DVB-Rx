<h1 align="center">dvbrx</h1>

<p align="center">
  <b>A software radio seen as a real TV card.</b><br>
  Digital TV received in real time by a USRP B210, a bladeRF or a HackRF, and served
  through <code>/dev/dvb</code> to tvheadend, VLC or dvbv5.
</p>

<p align="center">
  <a href="README.fr.md">Version française</a> ·
  <a href="#install">Install</a> ·
  <a href="#quick-start">Quick start</a> ·
  <a href="#documentation">Documentation</a>
</p>

<p align="center">
  <img alt="license" src="https://img.shields.io/badge/license-GPL--3.0--or--later-blue">
  <img alt="platform" src="https://img.shields.io/badge/platform-Debian%2012%20%7C%2013%20%7C%20Ubuntu%2024.04%20%7C%20Proxmox%20VE-informational">
  <img alt="standards" src="https://img.shields.io/badge/DVB-S%20%7C%20S2%20%7C%20S2X%20%7C%20C%20%7C%20T%20%7C%20T2-success">
  <img alt="radios" src="https://img.shields.io/badge/radios-USRP%20B210%20%7C%20bladeRF%20%7C%20HackRF-orange">
</p>

---

dvbrx decodes, in real time, digital TV received by a software radio and exposes it through the
standard Linux DVB API (DVBv5): a kernel module creates an ordinary `/dev/dvb/adapterN`, with
frontend, demux and dvr. tvheadend, VLC, Kaffeine or dvbv5-zap see it as a hardware tuner.

- **Every standard**: DVB-S, DVB-S2 and DVB-S2X up to 256APSK (multistream, roll-off 0.05 to
  0.35), DVB-C 16- to 256-QAM, DVB-T 2K/8K (hierarchical included), DVB-T2 1K to 32K (PLP,
  rotated constellations).
- **Demodulators in C**: AVX2 / AVX-512 vectorised LDPC, pilot-based equaliser, Viterbi,
  Reed-Solomon; a 52 Msym/s DVB-S2 is received live on a B210.
- **A real DVB card**: frontend named after the radio and its serial number, LNB control
  (13/18 V, 22 kHz, DiSEqC) through an ESP32, an impossible tuning refused at once so that
  tvheadend moves to another tuner.
- **Several radios at once**, or several multiplexes from a single wideband capture; the USB
  bandwidth is shared between the radios on one bus.
- **dvbrx-web**: a monitoring page, cable and satellite scan, constellation, alignment of a dish
  or a DVB-T antenna, export for tvheadend.
- **Ready to run**: Debian packages, DKMS module, one systemd service per adapter.

> [!WARNING]
> **LNB voltage.** With an LNB controller, the cable carries 13 or 18 V: connect it only to a
> protected radio input (bias tee, DC blocker). dvbrx only receives.

## Contents

- [Install](#install)
- [Quick start](#quick-start)
- [Command line](#command-line)
- [A standard DVB card](#a-standard-dvb-card-devdvbadaptern)
- [Source layout](#source-layout)
- [Test bench without a signal](#test-bench-without-a-signal)
- [Measurements](#cpu-optimisation-2026-11-03)
- [Future work](#future-work)
- [Building from source](#building-from-source)
- [Documentation](#documentation)
- [License](#license)

## Install

One package for Debian 12 and 13, Proxmox VE 8 and 9, and Ubuntu 24.04 or later (amd64).
In one line:

```bash
curl -fsSL https://raw.githubusercontent.com/TrusterBSW/DVB-Rx/main/install.sh | sudo sh
curl -fsSL https://raw.githubusercontent.com/TrusterBSW/DVB-Rx/main/install.sh | sudo sh -s -- --web
```

[`install.sh`](install.sh):
- checks the system;
- downloads the latest package from [`releases/`](releases/) and checks its
  [`SHA256SUMS`](releases/SHA256SUMS);
- installs the kernel headers (for the DKMS module) and the package with apt, which pulls in the
  radio libraries;
- lists the radios plugged in.

It never starts a service. Its options:
- `--web` also installs dvbrx-web, the monitoring page;
- `--adapters N` creates N adapters (one radio each);
- `--deb FILE` installs a package built locally (repeatable).

Or by hand:

```bash
wget https://github.com/TrusterBSW/DVB-Rx/raw/main/releases/dvbrx_0.1.0_amd64.deb
sudo apt install ./dvbrx_0.1.0_amd64.deb      # + linux-headers-… for the module (DKMS)
```

## Quick start

```bash
sudoedit /etc/dvbrxd/0.conf          # the radio of adapter 0: DVBRXD_ARGS="-u type=b200 -G auto"
sudo systemctl enable --now dvbrxd@0 # /dev/dvb/adapter0, also at boot
journalctl -u dvbrxd@0 -f            # 'frontend named "dvbrx B210 31A5B7C"'
dvbv5-zap -a 0 -c channels.conf "France 2"      # or tvheadend: Configuration > DVB Inputs
```

Several radios: `/etc/modprobe.d/dvbrx.conf` (`options dvbrx adapters=2`), one
`/etc/dvbrxd/N.conf` file per radio, designated by its serial number (see [Radios](#radios)).

## Command line

    make -C libdvbrx
    # B210, L-band intermediate frequency (after the LNB), 27.5 Msym/s:
    libdvbrx/build/dvbrx -u type=b200 -f 1210e6 -s 27.5e6 -o udp://127.0.0.1:1234
    mpv udp://127.0.0.1:1234            # or: dvbrx ... -o - | mpv -

Useful options: `-S` sample rate (default 2 × symbol rate; a non-integer
ratio ≥ 1.6 works), `-G` gain in dB or `-G auto` (see below), `-g` PLS gold
code, `-i` ISI (multistream; without `-i`, over UDP each ISI goes to
PORT + ISI), `-t` LDPC threads, `-l` DVB-S, `-c QAM` DVB-C (roll-off 0.15),
`-T MHZ` DVB-T, `-2 MHZ` DVB-T2 (channel width; `-S` rate, default 1.12 ×
the OFDM rate, i.e. 10.24 Msps at 8 MHz; `-i` PLP). From a file:
`-F file.fc32`, `-R` to pace it in real time (looped).

Several multiplexes from one capture (see "Multichannel"): `-m` repeated,
one channel per option, `-S` capture rate, `-f` centre (default: middle of
the channels):

    dvbrx -u type=b200 -G 25 -S 30.72e6 \
        -m f=646e6,sys=t2,bw=8,o=udp://127.0.0.1:1234 \
        -m f=655e6,sys=c,sr=6.9e6,qam=64,o=udp://127.0.0.1:1235

Keys: `f` (Hz), `sys` (`s`, `s2`, `c`, `t`, `t2`), `sr` (sym/s), `bw` (MHz),
`ro`, `qam`, `fec` (DVB-S), `isi` / `plp`, `gold`, `o` (output). `-x HZ`:
a single channel offset by HZ in a capture at `-S`.

## A standard DVB card: `/dev/dvb/adapterN`

The kernel module `kernel/dvbrx` creates classic DVB-S/S2 adapters
(frontend, demux, dvr, net) served by `dvbrx -d N`; DVB applications
(dvbv5-zap, tvheadend, VLC, Kaffeine…) use them like a card.

    # module (tested on kernels 6.12 and 7.0)
    make -C kernel && sudo modprobe dvb-core && sudo insmod kernel/dvbrx.ko
    # or a lasting install: sudo dkms install ./kernel  (dkms.conf provided)

    # daemon: B210, LNB controller on the ESP32
    sudo libdvbrx/build/dvbrx -d 0 -u type=b200 -L serial:/dev/ttyACM0
    # without hardware: an IQ file replayed in a loop at the real rate
    sudo libdvbrx/build/dvbrx -d 0 -F signal.fc32

    dvbv5-zap -a 0 -l UNIVERSAL -c channels.conf -r MyChannel

- The frequency requested by the application is the IF after the LNB
  (applications compute it from their LNB configuration); symbol rate, ISI
  (`STREAM_ID`) and PLS gold code (`SCRAMBLING_SEQUENCE_INDEX`) are taken
  from the tuning request. Without an ISI, the first stream received is
  delivered.
- LNB commands (13/18 V, 22 kHz, burst, DiSEqC) are passed to the
  controller synchronously: see `doc/lnb-protocol.md`.
- Reported status: lock (`FE_HAS_LOCK` as soon as the TS comes out), C/N
  (DVBv5 statistics), frames received / frames in error.
- DVB-S (first generation): the application requests `SYS_DVBS`, the code
  rate comes from `INNER_FEC` (or automatic detection).
- tvheadend sees three frontends on the adapter: DVB-S (S/S2/S2X), DVB-C
  and DVB-T (T and T2), usable one at a time; enable the ones you want and
  assign them a network (Configuration → DVB Inputs → TV adapters).
- The frontend carries the name of the radio and its serial number
  ("dvbrx B210 31A5B7C", "dvbrx bladeRF 2.0 xA4 0a0896e9…"): the daemon
  opens the radio when it starts and passes the name to the module
  (`DVBRX_IOC_NAME`), before tvheadend starts (`Type=notify`,
  `Before=tvheadend.service`). tvheadend reads this name when it discovers
  the adapter, and derives its configuration identifier from it: each radio
  keeps its own. Swapping radios while tvheadend is running requires
  restarting tvheadend, then enabling the new radio's frontends and
  assigning them the networks.
  Tested with a real radio link through the HackRF: scan, then playback of
  channels in DVB-S, DVB-S2, DVB-T, DVB-T2 and DVB-C; the parameters
  (constellation, code rate, guard interval, FFT) are reported to
  tvheadend. DVB-S2X: with a real radio link (dvbtx with S2X added, 16APSK
  26/45, roll-off 0.10, pilots: scan, playback of Arte without errors;
  64APSK 128/180 needs ~15 dB, beyond what the HackRF → B210 link gives at
  1.25 GHz, ~12-16 dB), and from a file for 64APSK 128/180; the 55 S2X
  MODCODs sent by dvbtx (null radio) are decoded without errors.

### Radios

| | USRP B210 | bladeRF 2.0 micro | HackRF One |
|---|---|---|---|
| `-u` | `uhd:serial=…` (or `type=b200`) | `bladerf:*:serial=…` | `hackrf:SERIAL` |
| Max sample rate | 61.44 Msps | 61.44 Msps | 20 Msps |
| Max symbol rate, DVB-S (roll-off 0.35) | 41 Msym/s | 41 Msym/s | 13.3 Msym/s |
| DVB-S2 roll-off 0.20 / 0.05 | 46 / 52.7 Msym/s | 46 / 52.7 Msym/s | 15 / 17.1 Msym/s |
| DVB-C (roll-off 0.15) | 48 Msym/s | 48 Msym/s | 15.6 Msym/s |
| Sensitivity (SFR cable, 2026-11-05) | reference | ≈ B210 (±1.5 dB) | 6-7 dB lower (measured behind a USB hub that disturbed it, see "Shared USB bandwidth") |
| Timestamps (gaps filled) | yes | yes (FPGA ≤ 0.15) | no |
| Multichannel (`-n`, `-m`) | yes | yes | no |

- bladeRF: the FPGA must match libbladeRF (0.15.3 with libbladeRF
  2024.05; a 0.16 FPGA delivers only a quarter of the samples); the version
  is shown at startup. udev rule in `packaging/60-dvbrx.rules`.
- B210 and bladeRF (AD9361): between 138 and 250 MHz, signals at three
  times the tuned frequency fold back into the band (a ~300 MHz low-pass
  filter is needed).
- Sample rate (bridge mode): 2 samples per symbol if the radio can do it,
  otherwise at least symbol rate × (1 + roll-off) / 0.9: the signal must
  stay within 90% of the band (at the edge, the AD9361 filters cost ~3 dB,
  dvbtx measurement). That is 1.5 samples/symbol in DVB-S, 1.28 in DVB-C,
  1.33 to 1.17 in DVB-S2 (roll-off 0.20 to 0.05; automatic roll-off:
  counted as 0.35). From a file (2026-11-06), no SNR loss at these rates
  compared to 2 samples/symbol (but on a real signal, see "Shared USB
  bandwidth": ~2.4 dB less in DVB-C on the cable): DVB-S2 QPSK to 32APSK,
  with and without pilots, 18/18; DVB-C 16- to 256-QAM (256-QAM locks a
  little more slowly at 1.28: ~20 packets lost at tuning); DVB-S at 1.5.
  On a real signal (2026-11-06, dvbtx → coupler on a closed cable, IF
  1200 MHz, TX gain 20 dB, probes in bridge mode):

  | Signal | Radio | Sample rate | Samples/symbol | SNR | Steady-state errors |
  |---|---|---|---|---|---|
  | DVB-S2 QPSK 3/4, 46 Msym/s, roll-off 0.20 | B210 | 61.44 Msps | 1.34 | 17.8 dB | 0 (TS 66.8 Mbit/s) |
  | DVB-S2 QPSK 3/4, 50 Msym/s, 0.10 | B210 | 61.44 Msps | 1.23 | 17.2 dB | 0 (72.4 Mbit/s) |
  | DVB-S2 QPSK 3/4, 52 Msym/s, 0.05 | B210 | 61.44 Msps | 1.18 | 16.6 dB | 0 (75.6 Mbit/s) |
  | DVB-S2 QPSK 3/4, 15 Msym/s, 0.20 | HackRF | 20 Msps | 1.33 | ~20 dB | USB losses (hub) |
  | DVB-S2 QPSK 3/4, 17 Msym/s, 0.05 | HackRF | 20 Msps | 1.18 | ~19 dB | USB losses (hub) |
  | DVB-S QPSK 3/4, 13.3 Msym/s, 0.35 | B210 / HackRF | 30.72 / 20 Msps | 2.3 / 1.5 | 21.0 / 16.3 dB | 0 / 2.6% |

  Before this rule, bridge mode refused anything above 37.3 Msym/s on the
  B210. FEC failures during lock acquisition (a few seconds, automatic
  gain) are not counted. The HackRF has 2-7% of packets in error in DVB-S
  even at 2 samples/symbol and without USB loss: a separate fault, to be
  investigated.
  Oversampling further brings nothing: HackRF on the 602 MHz DVB-C
  (64-QAM, 6.875 Mbd, gain 70, 4 × 30 s alternated), 1.8-2.0% of packets in
  error at 13.75 Msps (2 samples/symbol) against 3.9-4.5% at 20 Msps.
- Maximum symbol rate: the daemon reports to the frontend (`FE_GET_INFO`,
  `DVBRX_IOC_LIMITS`) the one for the lowest roll-off (0.05) or, in
  multichannel mode, the one for the usable band of the capture. dvb-core
  then refuses at once (`EINVAL`) a DVB-S/S2/C tuning above it, for example
  a 22000 kBd transponder on a HackRF; tvheadend shows "failed to tune" and
  moves to another free tuner (test of 2026-11-06: switched to the bladeRF,
  stream in 2.9 s). With no other tuner, the channel does not start.
  Between the two limits (a 15 Msym/s DVB-S on a HackRF), the tuning
  passes the kernel but the daemon refuses it ("no usable sample rate"):
  the application sees no signal.

### Several radios at once

One daemon per radio, each on its own adapter, each radio designated by
its serial number (the plug-in order does not matter):

    # /etc/modprobe.d/dvbrx.conf: options dvbrx adapters=2
    # /etc/dvbrxd/0.conf: DVBRXD_ARGS="-u bladerf:*:serial=0a0896e9… -G auto -t 3"
    # /etc/dvbrxd/1.conf: DVBRXD_ARGS="-u hackrf:334c64dc39395367 -G auto -t 2"
    sudo systemctl enable --now dvbrxd@0 dvbrxd@1

tvheadend sees two adapters named after their radio and can receive two
multiplexes at once; dvbrx-web finds both sockets and shows them with
their radio. Test of 2026-11-06 (bladeRF + HackRF, SFR cable, through
tvheadend): DVB-T 554 MHz on the bladeRF without errors while the HackRF
received the 602 MHz DVB-C (QAM64, SNR 24.8 dB, 3% of blocks in error: the
HackRF is at its limit on this cable); no overflow.

### Shared USB bandwidth

USB is the bottleneck: an AD9361 radio (B210, bladeRF) sends 4 bytes per
sample, i.e. 246 MB/s at 61.44 Msps and 220 MB/s for a 27500 kBd
transponder (55 Msps); USB 3 (5 Gbit/s) carries only about 350 MB/s of
payload per bus. One radio alone fits; two radios on wideband satellite on
the same bus do not. The HackRF (2 bytes per sample, 40 MB/s at 20 Msps)
is already at the USB 2 ceiling (~42 MB/s).

**Different ports do not mean different controllers**: all the ports of a
controller (and those of the hubs plugged into it) share its bandwidth. An
xHCI controller has two buses, one USB 2 and the other USB 3, on separate
lines: a USB 2 radio (HackRF) takes nothing from a USB 3 radio on the same
controller. `lsusb -t` shows the buses (`Bus 002 … 5000M`) and what is
plugged into them; here the HackRF (usb1, 480M) and the bladeRF (usb2,
5000M) are on the same controller `0000:00:14.0`.

The daemons share the bandwidth of a bus. Each one finds its radio's bus
in `/sys/bus/usb` (by its serial number) and writes what it uses to
`/run/dvbrx/usb-N`:

- on tuning, it takes its full rate (2 samples/symbol) if it is alone on
  the bus, otherwise what the others leave, without going below the
  channel minimum (symbol rate × (1 + roll-off) / 0.9);
- if it lacks room, it asks for some: the other radios on the bus step down
  to what they can leave, through a new tuning (about 1 s without TS for
  them), then it tunes; with no room after 2.5 s, it takes its minimum
  anyway (overflows likely);
- a radio that stepped down keeps that rate until its next tuning (channel
  change): stepping back up would cut the stream, for a few dB at most;
- budget per bus: 350 MB/s on USB 3, 42 MB/s on USB 2; `-U MB/S` changes
  it (set the same value on all the radios of a bus), `-U 0` disables
  sharing; the short captures of dvbrx-web scans are not counted, nor are
  the other devices on the bus (DVB-T stick, disk…); the status
  (`radio.usb`) and dvbrx-web show the usage.
- Seen on 2026-11-06: a HackRF at 20 Msps behind a small USB 2 hub, shared
  with an RTL2832 stick and a B210 (both idle), lost samples ~14 times per
  second (the HackRF's "shortfalls" counter, `hackrf_debug --state`, and
  `hackrf_transfer` alone did the same), against no loss once alone on the
  hub (stick removed).
- Even without loss, the HackRF behind this small unpowered hub saw the
  phase of its oscillator jump by about 90°, twice every 102.4 ms (USB
  power): 2-7% of packets in error in DVB-S and in DVB-C, whatever the
  gain, rate and tuning. Plugged directly into another PC, the same board
  has no jumps. A HackRF wants a direct port or a powered hub.
- Radio unplugged: the daemon stops ("the SDR does not answer any more")
  instead of insisting (libbladeRF ended up crashing, SIGFPE); systemd
  restarts it and it reopens the radio when it comes back.

Stepping down has a cost in real conditions: on the SFR cable, bladeRF on
the 658 MHz DVB-C (64-QAM), SNR 26.7 dB at 13.75 Msps (2 samples/symbol)
against 24.3 dB at 8.78 Msps (1.28), 0 errors in both cases (neighbouring
multiplexes at ±8 MHz). This is why the daemon only steps down if the
radio or the bus requires it. Test of the mechanism on 2026-11-06 with a
simulated neighbouring radio on the bladeRF's bus: tuned at 12.5 Msps
instead of 13.75 when it occupied 300 MB/s, and stepped down from 13.75 to
12.5 Msps during tvheadend playback when it requested 300.

### Multichannel: several adapters on one B210

    sudo modprobe dvbrx adapters=2
    sudo dvbrx -d 0 -n 2 -u type=b200 -G 25 -W 30.72e6   # /dev/dvb/adapter0 and 1

A single fixed-rate capture (`-W`, default 30.72 Msps) serves `-n`
adapters: the channeliser (`chan.c`) extracts each multiplex at its own
rate and each adapter has its own receiver. The centre of the capture
follows the channels in use: it is set on the first one, then recentres
(B210 retune; the channels in progress follow without losing lock) when a
new channel does not fit; a tuning that does not fit with the channels of
the other adapters (90% of the usable band) is refused (no lock). `-f`
fixes the centre. Satellite: the adapters share the LNB; a command
(polarisation, band, DiSEqC) that would change what another adapter in use
receives is refused (`EBUSY`); repeating the current state is accepted.

- Gain: what counts is the power of all the multiplexes in the capture;
  the gain set for a single channel saturates (B210, two HackRF
  multiplexes: errors at 35 dB, 0 errors at 20-28 dB): use `-G auto`.
- Cost: the channeliser costs ~0.15 core at 30.72 Msps (a 16384-point FFT
  every 14336 samples, then one IFFT per channel); each multiplex costs its
  demodulation (i5-9500: DVB-T 8K 64-QAM 5/6 ~1.3 cores, DVB-C 256-QAM
  ~1.1, DVB-T2 8K 64-QAM ~1.2, DVB-S2 depending on the rate).
- Measured with a real radio link (HackRF dvbtx: DVB-T2 8K 64-QAM 3/4 at
  646 MHz + DVB-C 64-QAM 6.9 Mbd at 655 MHz, B210 at 30.72 Msps) on the
  host without virtualisation: 57 s, 0 failed FEC blocks, 0 packets in
  error on both (31.3 + 37.5 Mbit/s), 0 overflows, 2.5 cores. Three
  multiplexes (T2 + T + C) from a file in 30.72 Msps: 0 errors outside lock
  acquisition.
- In the VM (QEMU USB redirection), the B210 overflows above ~15 Msps
  (2 to 3 times/s at 30.72 Msps, still ~1/s at 20.48 Msps), whatever the
  buffer: multichannel loses frames there. For wideband, give the VM a USB
  controller through PCIe passthrough, or run dvbrx on the host machine.

### Installing from source (tested on Debian 13)

The package (see [Install](#install)) is the normal way. From a copy of
the sources:

    sudo packaging/install-service.sh 1     # DKMS module, udev, dvbrxd user, service
    sudoedit /etc/dvbrxd/0.conf             # DVBRXD_ARGS="-u type=b200 -L serial:/dev/ttyACM0 -t 3"
    sudo systemctl enable --now dvbrxd@0

The module is loaded at boot, `/dev/dvbrxN` and `/dev/dvb` belong to the
`video` group, and the `dvbrxd@N` service runs `/usr/local/bin/dvbrx`
(package: `/usr/bin/dvbrx`) as the `dvbrxd` user. Tested with
dvbv5-scan / dvbv5-zap and VLC (`cvlc dvb-s2://frequency=11778000000
--dvb-srate=...`). For the USRP, add UHD's udev rule (uhd-host package) if
`dvbrx` does not see the B210.

### The B210 in practice (LibreSDR clone)

Since 2026-11-03, the receiver runs directly on the Proxmox host (dvbrx
module through dkms, `dvbrxd@0`, tvheadend): 0 overflows at 30.72 Msps,
DVB-S2 played through tvheadend without errors. The test VM was backed up
(vzdump) then deleted. The notes below about the VM still apply if we go
back to it.

- LibreSDR clone: copy its FPGA image and its `/etc/uhd/uhd.conf` (fixed
  61.44 MHz master clock: the possible rates are 61.44/N Msps; dvbrx takes
  the rate actually obtained, 30.72 Msps for 15.5 Msym/s).
- Automatic gain `-G auto` (recommended): dvbrx measures the mean level
  and the ADC clipping over 0.5 s windows and sets the B210 gain for a
  level of -24 dBFS (±4 dB), lowering it at once (6 to 10 dB) if the ADC
  clips. Measured in DVB-T 64-QAM: flat MER (27.1-27.5 dB) from -30 to
  -15 dBFS, clipping from -10 dBFS, MER 22 dB at -5 dBFS, no lock beyond;
  starting from 45 dB, the gain settles in ~4 s. The level and the gain are
  shown in the statistics (`-22.2 dBFS gain 30`, `CLIP` when clipping).
- Choose the input with `-A TX/RX` or `-A RX2` (on this setup, the signal
  arrives on TX/RX).
- Larger USB buffers, essential in a VM:
  `-u type=b200,num_recv_frames=512,recv_frame_size=16360` (losses from 42%
  down to 0.5% at 30.72 Msps).
- The signal must stay within 90% of the sampled band: at 20.48 Msps for
  15.5 Msym/s (roll-off 0.2, 91%), before the equaliser, the SNR collapsed.
  The bridge mode rule takes this into account (see "Radios").
- The interrupts of the USB controller (several thousand per second) slow
  down the thread that shares their CPU: `packaging/dvbrx-irq-pin` moves
  them to the last CPU and `dvbrxd-cpu.conf.example` (to copy to
  `/etc/systemd/system/dvbrxd@.service.d/cpu.conf`) keeps dvbrx on the
  others. Without this, dvbrx loses samples in the VM.
- A USB overflow: the lost samples (counted from the B210 timestamps) are
  replaced by as many zeros, and the receivers keep their timing
  synchronisation (DVB-T2: a few symbols erased instead of a
  reacquisition); `ovf N (x ms)` in the statistics. The LOCK status
  reported to `/dev/dvb` is held for 1 s so that applications do not retune
  at every micro-dropout.

First real test (HackRF on a direct cable, DVB-S2 8PSK 9/10 pilots,
15.5 Msym/s at 1200 MHz, 10950 H multiplex through tvheadend): lock,
SNR ~14 dB, 0 CRC errors, 6 services found by the scan.

## Source layout

- `libdvbrx/`: complete DVB receivers in C (GPL-3.0-or-later, part of it
  ported from gr-dvbs2rx):
  - `frontend.c`: frequency correction (AFC), matched filter, symbol
    timing sync (Gardner + cubic interpolation), AGC;
  - `pl.c`: PL frame sync, PLS decoding, descrambling, carrier recovery
    (pilots + interpolation, or decision-directed PLL without pilots);
  - `demap.c`: max-log demapper + deinterleaving, all the DVB-S2 and
    DVB-S2X MODCODs up to 256APSK (except VL-SNR);
  - `ldpc.c`: layered min-sum LDPC, 32 frames with AVX2 / 64 with AVX-512;
  - `bch.c`, `bb.c`: BCH, BB descrambling, TS reconstruction per ISI;
  - `dvbs.c`: DVB-S (1st generation): carrier, automatic detection of the
    code rate / phase ambiguity / puncturing, Viterbi K=7, deinterleaver,
    Reed-Solomon (204,188), energy dispersal;
  - `dvbc.c`: DVB-C (EN 300 429): frequency from the fourth-power spectral
    line (FFT), CMA then decision-directed equaliser, PLL, differential
    quadrant decoding, 16- to 256-QAM, automatic constellation detection;
  - `dvbt.c`: DVB-T (EN 300 744): 2K/8K mode and guard interval detection
    on the cyclic prefix, fractional and integer frequency, channel
    estimation on the scattered pilots (interpolation with compensated
    phase slope), common phase and sampling clock on the continual pilots,
    TPS, symbol and bit deinterleaving, demapping weighted by |H|²,
    Viterbi; HP stream in hierarchical modulation;
  - `dvbt2.c`: DVB-T2 (EN 302 755, base profile, SISO): P1 (S1/S2,
    frequency), guard interval, P2 symbols and L1-pre / L1-post (shortened
    and punctured 16K LDPC), all FFT sizes 1K to 32K, extended mode,
    patterns PP1 to PP8, frame closing symbol, frequency, time and cell
    deinterleaving, rotated constellations (cyclic Q delay), 16K/64K LDPC
    (including the T2-specific codes: 2/3 normal, 3/5 short), BCH, BB layer
    (FEC decoding on 2 dedicated threads, frames decoded in parallel and
    returned in order); one PLP (`-i`); tables generated by
    `gen/gen_t2_tables.py` from gr-dtv;
  - `chan.c`: multichannel channeliser (fast-convolution filter bank,
    overlap-save: one FFT of the capture shared by all the channels, then
    per channel a small IFFT at its own rate; the inverse of the dvbtx
    synthesis bank);
  - `resamp.c`: polyphase resampler (SDR rate → OFDM rate, ratio tracking
    the transmitter clock);
  - `conv.c` (Viterbi, puncturing) and `outer.c` (packet sync,
    deinterleaver, RS, energy dispersal): layers shared by DVB-S, DVB-C
    and DVB-T; `fft.c`: FFT (OFDM);
  - `rx.c` (single-threaded) and `rxmt.c` (multi-threaded pipeline);
  - `gen/`: table generators (FEC, LDPC, and constellations /
    interleaving / PLS codes measured on the gr-dtv transmitter, sources in
    `third_party/gnuradio-dtv`);
  - `tools/dvbrx.c`: the receiver (`dvbrxd@N` daemon in bridge mode);
    `tools/sdr_uhd.c`, `sdr_bladerf.c`, `sdr_hackrf.c`: the radio drivers
    (USRP through the UHD C API, libbladeRF, libhackrf), or IQ file.
- `gr-dvbrx/`, `apps/dvbrx-dvbs2-rx`: intermediate step (C front end in
  the GNU Radio gr-dvbs2rx chain).
- `tests/`: reference vectors (gr-dtv transmitter) and per-stage tests.
- `bench/`: throughput and MER measurements.

## Test bench without a signal

    # Per-stage reference vectors, then tests
    python3 tests/txvectors.py work/ts1m.ts work/vec/qpsk34 -m QPSK3/4 --pilots
    tests/run_backend.sh            # demapper -> LDPC -> BCH -> BB
    tests/run_pl.sh 0.01 0.5        # PL sync -> TS (offset, SNR margin)
    tests/run_s2x.py 3.0            # the 55 DVB-S2X MODCODs, Shannon + 3 dB
    # multistream / VCM: one TS stream per ISI, MODCOD of choice, gold code
    python3 tests/mis_gen.py mis.fc32 --stream 1:a.ts:QPSK3/4 \
        --stream 2:b.ts:16APSK3/4 --gold 131073 --pilots
    libdvbrx/build/dvbrx_file -i 2 -g 131073 -n 12 mis.fc32 isi2.ts
    # DVB-S: gr-dtv channel coding (DVB-T, identical), automatic decoding
    python3 tests/dvbs_gen.py in.ts dvbs.fc32 --rate 3/4
    libdvbrx/build/dvbrx_file -l auto -n 7.4 dvbs.fc32 out.ts
    tests/run_s2x.py 3.0 64APSK nopilots
    # any roll-off (S2X: 0.15/0.10/0.05) from PL frames
    python3 tests/txshape.py work/vec/.../plframe.fc32 iq.fc32 --rolloff 0.05 --esn0 8


    # Test TS stream (test pattern + 1 kHz tone)
    mkdir -p work && ffmpeg -f lavfi -i testsrc2=size=1280x720:rate=25 \
        -f lavfi -i sine=frequency=1000:sample_rate=48000 -t 30 \
        -c:v libx264 -b:v 4M -c:a mp2 -f mpegts -muxrate 5M work/test.ts

    # TS -> DVB-S2 modulator -> noise/offset -> demodulator -> TS, + throughput
    bench/loopback.py -m 8PSK3/4 --snr 9 --ts-bytes 8000000 --keep
    bench/loopback.py --rx-app dvbs2-rx ...          # original gr-dvbs2rx chain

    # Steady-state throughput (looped file); 22194 = symbols/frame
    # for 8PSK, normal frame, pilots
    bench/steady.sh apps/dvbrx-dvbs2-rx work/loopback/<iq>.fc32 8PSK3/4 22194

    # Front end only: throughput (pure C, then GNU Radio block) and MER
    libdvbrx/build/fe_bench <iq.fc32> 2 0.2 <symboles.fc32>
    python3 bench/fe_gr_bench.py <iq.fc32> --rotator
    python3 bench/mer.py <symboles.fc32>

    # Test matrix: all the standards and their parameters (generated,
    # resampled to an SDR rate, +15 ppm, frequency offset, noise)
    python3 tests/matrix.py [dvbt] [dvbt2] [dvbc] [dvbs] [--quick]
    # several multiplexes side by side in a wideband capture
    python3 tests/wideband.py wide.fc32 --rate 30.72e6 --seconds 1.2 --snr 32 \
        t2.fc32:9142857.14:-9.5e6 t.fc32:9142857.14:0.5e6 c.fc32:13.8e6:9.5e6

Matrix (2026-11-03): DVB-T 60/60 (2K/8K, QPSK to 64-QAM, 1/2 to 7/8, GI 1/32
and 1/4, 5 to 8 MHz), DVB-C 15/15 (16- to 256-QAM, 2 to 3.07 samples/symbol),
DVB-S 10/10, DVB-T2 98/98 (4 constellations × 6 code rates × 16K/64K, and
all FFT sizes × guard intervals × pilot patterns × extended mode, widths
1.7 to 10 MHz). The only case that cannot be tested with gr-dtv: 16K PP1
extended (its pilot generator has 3636 instead of 13636, table G.2, while
its mapper counts the right cells: inconsistent signal); checked with
dvbtx, as well as 32K PP8 19/128 and 16K PP2 extended: 0 errors.

## CPU optimisation (2026-11-03)

On an i5-9500, cores needed in real time (before → after): DVB-T 8K
64-QAM 5/6 1.25 → 0.44; DVB-T2 8K 64-QAM 3/4 0.94 → 0.29; DVB-C 256-QAM
0.98 → 0.41; DVB-S2 8PSK 3/4 at 27.5 Mbd 1.86 → 1.23. Details, dead ends
and remaining leads: `doc/optimisation/`.

## dvbrx results (i5-9500, 6 cores, 8PSK 3/4 at 8.5 dB, pilots)

- 36.7 Msym/s maximum (full C chain); real time sustained at 27.5 and
  30 Msym/s without loss.
- Error-free decoding 0.5 dB from the QEF threshold for QPSK, 8PSK, 16APSK
  and 32APSK, carrier offset up to ±0.1 symbol⁻¹ (± 2.75 MHz at
  27.5 Msym/s) and beyond with the AFC.
- DVB-S2X: the 47 MODCODs (QPSK to 64APSK, normal and short frames)
  decode at Shannon + 3 dB with pilots; thresholds measured at about
  Shannon + 1.75 to 2.25 dB (8PSK 13/18 ~7.7 dB, 64APSK 4/5 ~16.0 dB).
  64APSK: 36.7 Msym/s max, real time 30 Msym/s without loss.
- Roll-off 0.20 to 0.05 (timing loop bandwidth reduced for low
  roll-offs); at 0.05, allow ~1 dB of margin above the threshold.
- 128APSK and 256APSK (professional contribution): decoded at Shannon +
  3 dB; 256APSK 3/4 in real time up to 27.5 Msym/s; phase noise tolerance
  ~0.2°/symbol (random walk), a quality LNB is required.
- Multistream (MIS) and VCM: ISI selection, PLS gold code or root code
  (`-g 131073` or `-g r2052`), several MODCODs interleaved frame by frame
  (LDPC batches per MODCOD, output put back in frame order).
- DVB-S: the 5 code rates detected automatically and decoded 0.5 dB from
  the threshold of the standard (1/2 down to Es/N0 3.5 dB); 38 Msym/s max,
  real time 30 Msym/s without loss (Viterbi on a separate thread).
- DVB-C: 16- to 256-QAM decoded without a bad packet at the Es/N0 of the
  standard (16-QAM 17 dB, 32-QAM 20 dB, 64-QAM 23 dB, 128-QAM 26 dB;
  256-QAM at the threshold at 29 dB), carrier offset ±0.04 symbol⁻¹,
  constellation detected automatically; receiver MER floor 46 dB;
  6.9 Msym/s 64-QAM sustained in real time (multi-threaded pipeline).
- DVB-T: real signals from dvbtx (2K 16-QAM 2/3 GI 1/8, 8K 64-QAM 5/6
  GI 1/32) and from gr-dtv decoded without errors (MER > 100 dB on a pure
  digital signal); with resampling from 9.14 to 10.24 Msps, clock −60 to
  +40 ppm, carrier ±20 kHz, −6 dB echo at 5 µs and SNR 25 dB: 0 packets
  lost; 8K 64-QAM 5/6 in real time (3 threads: resampling, OFDM,
  Viterbi), tuning through `/dev/dvb` (`dvbv5-zap`) checked.
- DVB-T2: real signals from dvbtx decoded without a single failed FEC
  block: 32K extended PP7 GI 1/128 256-QAM rotated 2/3 (the default dvbtx
  profile), 8K PP2 GI 1/8 in QPSK, 16-QAM and 64-QAM, code rates 1/2 to
  5/6, rotated or not; with resampling to 10.24 Msps, clock ±60 ppm and
  carrier ±20 kHz: no errors; −6 dB echo alone or noise alone (64-QAM 3/4
  down to 22 dB) without errors, both combined at 25 dB: ~7% of failed
  blocks (decoupled rotated demapping, to be improved); real time 8K (OFDM
  thread at ~70%), `/dev/dvb` checked (`dvbv5-zap`, PLP through
  `STREAM_ID`).
- gr-dtv note: its T2 cell interleaver computes the offset P(r) wrongly
  for FEC blocks r ≥ 1 of a TI block; `tests/dvbt2_gen.py --ti fec`
  (one FEC block per TI block) works around the problem.
- Reference signals: `dvbtx-engine` (10.5.1.23) with the `null` radio
  (`args = file=...`) produces real DVB-S/S2/C/T/T2 IQ without touching
  the transmission in progress.
- Note: until 2026-11-03, `tests/txshape.py --esn0` added 3 dB less noise
  than requested (fixed); symbol-level measurements (`dvbrx_file -n`)
  were not affected.
- Limits: without pilots, dense S2X APSK (8APSK, 16APSK 26/45, 32APSK)
  does not decode reliably; VL-SNR is not supported.

## GNU Radio chain results (i5-9500, 8PSK 3/4, normal frame, pilots)

| Chain | Steady-state throughput |
|---|---|
| original gr-dvbs2rx | 11.9 Msym/s |
| dvbrx-dvbs2-rx, SNR 11 dB | 33.7 Msym/s |
| dvbrx-dvbs2-rx, SNR 8.5 dB | 30.4 Msym/s |
| dvbrx-dvbs2-rx, SNR 8.1 dB (threshold) | 19 Msym/s (limited by the LDPC) |
## Future work

See `doc/A_FAIRE.md`: robustness (DVB-S resynchronisation, equaliser
without pilots, DVB-T lock acquisition), DVB-S on bladeRF and HackRF still
to be measured, and the optimisation leads in `doc/optimisation/`.

## Building from source


    git clone --recursive https://github.com/igorauad/gr-dvbs2rx third_party/gr-dvbs2rx
    cmake -S third_party/gr-dvbs2rx -B third_party/gr-dvbs2rx/build \
          -DCMAKE_INSTALL_PREFIX=$PWD/prefix -DCMAKE_BUILD_TYPE=Release \
          -DNATIVE_OPTIMIZATIONS=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5
    cmake --build third_party/gr-dvbs2rx/build -j8
    cmake --install third_party/gr-dvbs2rx/build
    . ./env.sh

    make -C libdvbrx                                  # C library + fe_bench
    cmake -S gr-dvbrx -B gr-dvbrx/build && cmake --build gr-dvbrx/build   # GNU Radio block

System dependencies: gnuradio (+ gnuradio-dev on Debian), libuhd, FFTW
(libfftw3-dev, single precision), pybind11, tsduck, ffmpeg. For `dvbrx`
alone: FFTW and, depending on the radios wanted, libuhd-dev,
libbladerf-dev, libhackrf-dev (each driver is built if its header is
present).

The receiver alone: FFTW and, depending on the radios, the headers of libuhd, libbladeRF and
libhackrf (the radio libraries are loaded at run time):

```bash
sudo apt install build-essential libfftw3-dev libuhd-dev libbladerf-dev libhackrf-dev
make -C libdvbrx                                   # libdvbrx/build/dvbrx
sudo packaging/install-service.sh 1                # install from here, with the DKMS module
./packaging/build-deb.sh dist/                     # the .debs for this system
./packaging/build-all-debs.sh                      # the .debs for all releases (docker)
```

## Documentation

- `dvbrx --help`, `man dvbrx` (`man -L fr dvbrx` in French) and `man dvbrx-web`;
- [`doc/dvbrx-web-api.md`](doc/dvbrx-web-api.md): the dvbrx control socket;
- [`doc/lnb-protocol.md`](doc/lnb-protocol.md): the LNB controller protocol;
- [`apps/dvbrx-web/README.md`](apps/dvbrx-web/README.md): the web page;
- [`doc/A_FAIRE.md`](doc/A_FAIRE.md) and [`doc/optimisation/`](doc/optimisation/): work in
  progress and measurements.

## License

GPL-3.0-or-later; the kernel module (`kernel/`) is under GPL-2.0. © 2026 BILLY Rémi.
