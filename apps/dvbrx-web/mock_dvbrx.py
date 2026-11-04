#!/usr/bin/env python3
"""Faux dvbrx pour développer et tester dvbrx-web sans matériel.

Implémente le socket de contrôle décrit dans doc/dvbrx-web-api.md avec des données
synthétiques :
- câble : DVB-C QAM64/256 (6,875 et 6,9 MBd), DOCSIS 3.0 (6,952 MBd, PID 0x1FFE seul),
  DVB-T, DVB-T2 (deux PLP), un bloc OFDM DOCSIS 3.1 de 96 MHz, un multiplex faible
  annoncé seulement par la NIT ;
- satellite : porteuses différentes selon la combinaison du LNB (13/18 V, 22 kHz) ;
- « tvheadend » regarde une chaîne (--tune) : capture, probe et monitor répondent
  "busy" sauf avec "force": true.

Les spectres sont synthétisés dans le domaine fréquentiel ; les porteuses uniques sont de
vraies modulations linéaires périodiques (filtre en racine de cosinus surélevé), donc la
raie cyclique de |x|² au débit symbole est présente.

Usage : ./mock_dvbrx.py --socket /tmp/dvbrx-0.sock [--tune dvbt2|dvbc|dvbs2|none]
        [--no-lnb] [--strict] [--speed 0.15]
"""

import argparse
import base64
import json
import math
import os
import socketserver
import sys
import threading
import time

import numpy as np

OFFSET = -50.0          # input_dbm = level_dbfs - gain + OFFSET (comme dvbrx -P par défaut)
REF = 15.0              # « vraie » pleine échelle du B210 simulé à gain 0 : -15 dBm
LO_LOW, LO_HIGH = 9750e6, 10600e6
LO_ERROR = 0.35e6       # dérive de l'oscillateur du LNB simulé

# --- monde simulé -------------------------------------------------------------------------

TV_NAMES = [
    "France 2 HD", "France 3", "France 5", "Arte HD", "LCP", "France 4", "franceinfo",
    "TV5MONDE", "Euronews", "Gulli", "TMC", "W9", "6ter", "RMC Story", "Paris Première",
    "Planète+", "Canal+ Cinéma", "Ciné+ Frisson", "Eurosport 1", "beIN Sports 1",
    "Mezzo", "Nickelodeon", "Disney Channel", "Discovery", "Ushuaïa TV", "Histoire TV",
    "Trace Urban", "MCM", "BFM Business", "CNews", "LCI", "Bloomberg", "CNN Int.",
    "BBC World News", "Al Jazeera", "RTL9", "AB1", "Toute l'Histoire", "Science&Vie TV",
    "Animaux", "Equidia", "Golf+", "Automoto", "Mangas", "Game One", "J-One",
]
RADIO_NAMES = ["France Inter", "France Culture", "FIP", "Mouv'", "France Musique", "RFI"]
PROVIDERS = ["Numericable", "SFR", "CanalSat", "GlobeCast", "SES", "Eutelsat", "TDF"]


class World:
    def __init__(self, seed=1):
        self.rng = np.random.default_rng(seed)
        r = self.rng
        self.cable = []
        ts = [1000]

        def services(n, scrambled_ratio=0.5, radio=0, prov=None):
            out = []
            sid = ts[0] * 10
            for i in range(n):
                sid += 1
                out.append({"sid": sid, "name": TV_NAMES[int(r.integers(len(TV_NAMES)))],
                            "provider": prov or PROVIDERS[int(r.integers(len(PROVIDERS)))],
                            "type": int(r.choice([1, 0x16, 0x19, 0x19, 0x1F])),
                            "scrambled": bool(r.random() < scrambled_ratio)})
            for i in range(radio):
                sid += 1
                out.append({"sid": sid, "name": RADIO_NAMES[i % len(RADIO_NAMES)],
                            "provider": "Radio France", "type": 2, "scrambled": False})
            return out

        def add_c(f, rs, mod, p, n=6, docsis=False, nit=True, fec=None):
            ts[0] += 1
            self.cable.append({"freq": f * 1e6, "kind": "sc", "delsys": "DVB-C", "rs": rs,
                               "beta": 0.15, "mod": mod, "power": p, "docsis": docsis,
                               "tsid": None if docsis else ts[0], "onid": None if docsis else 64810,
                               "services": [] if docsis else services(n, 0.6, radio=1 if n > 6 else 0),
                               "nit": nit and not docsis, "snr": 34 + r.normal(0, 1.5),
                               "mbps": rs * {"QAM64": 6, "QAM256": 8, "QAM128": 7}[mod] * 188 / 204 / 1e6})

        for f in (123, 131, 139, 147):
            add_c(f, 6875000, "QAM64", -60 + r.normal(0, 1))
        for f in (307, 315, 323, 331, 339, 347):
            add_c(f, 6875000, "QAM256", -59 + r.normal(0, 1), n=9)
        add_c(115, 6875000, "QAM64", -93.0, n=4)              # trop faible : vu seulement par la NIT
        for f in range(571, 628, 8):
            add_c(f, 6952000, "QAM256", -58 + r.normal(0, 0.5), docsis=True)
        add_c(699, 6900000, "QAM256", -61.0, n=8)
        ts[0] += 1
        self.cable.append({"freq": 651e6, "kind": "ofdm", "delsys": "DVB-T", "bw": 8e6, "occ": 7.61e6,
                           "power": -63.0, "mod": "QAM64", "code_rate": "2/3", "fft": 8192,
                           "guard": "1/4", "tsid": ts[0], "onid": 8442, "services": services(5, 0.0),
                           "snr": 27.5, "mbps": 19.9, "nit": True})
        ts[0] += 1
        self.cable.append({"freq": 659e6, "kind": "ofdm", "delsys": "DVB-T2", "bw": 8e6, "occ": 7.71e6,
                           "power": -62.0, "mod": "QAM256", "code_rate": "2/3", "fft": 32768,
                           "guard": "1/128", "plps": {0: (ts[0], services(6, 0.3)),
                                                      1: (ts[0] + 1, services(3, 1.0))},
                           "tsid": ts[0], "onid": 8442, "services": None, "snr": 25.6, "mbps": 40.2,
                           "nit": True})
        ts[0] += 2
        # bloc OFDM DOCSIS 3.1 de 96 MHz
        self.cable.append({"freq": 799e6, "kind": "ofdm", "delsys": None, "bw": 96e6, "occ": 95.2e6,
                           "power": -61 + 10 * math.log10(12), "snr": 0, "nit": False})
        self.cable_floor_8m = -95.0    # dBm dans 8 MHz

        # satellite : (MHz réels, polarisation, débit symbole, norme, modulation, rendement)
        tp = [
            (10773, "V", 22000000, "DVB-S2", "8PSK", "2/3"), (10862, "V", 27500000, "DVB-S", "QPSK", "3/4"),
            (10979, "V", 22000000, "DVB-S2", "8PSK", "2/3"), (11097, "V", 27500000, "DVB-S", "QPSK", "3/4"),
            (11244, "V", 4500000, "DVB-S2", "QPSK", "3/4"), (11362, "V", 22000000, "DVB-S2", "8PSK", "2/3"),
            (11523, "V", 22000000, "DVB-S2", "16APSK", "3/4"), (11778, "V", 27500000, "DVB-S", "QPSK", "3/4"),
            (11914, "V", 27500000, "DVB-S2", "8PSK", "3/4"), (12051, "V", 27500000, "DVB-S", "QPSK", "3/4"),
            (12188, "V", 27500000, "DVB-S2", "8PSK", "9/10"), (12515, "V", 22000000, "DVB-S2", "QPSK", "3/4"),
            (12663, "V", 22000000, "DVB-S2", "8PSK", "2/3"),
            (10744, "H", 22000000, "DVB-S2", "8PSK", "2/3"), (10832, "H", 22000000, "DVB-S", "QPSK", "5/6"),
            (10891, "H", 22000000, "DVB-S2", "32APSK", "4/5"), (11023, "H", 23500000, "DVB-S2", "8PSK", "3/5"),
            (11141, "H", 22000000, "DVB-S", "QPSK", "5/6"), (11303, "H", 22000000, "DVB-S2", "8PSK", "2/3"),
            (11493, "H", 22000000, "DVB-S2", "8PSK", "2/3"), (11627, "H", 22000000, "DVB-S", "QPSK", "5/6"),
            (11719, "H", 27500000, "DVB-S2", "8PSK", "3/4"), (11836, "H", 27500000, "DVB-S", "QPSK", "3/4"),
            (11953, "H", 27500000, "DVB-S", "QPSK", "3/4"), (12110, "H", 27500000, "DVB-S2", "8PSK", "3/4"),
            (12226, "H", 27500000, "DVB-S", "QPSK", "3/4"), (12363, "H", 27500000, "DVB-S", "QPSK", "3/4"),
            (12480, "H", 27500000, "DVB-S2", "8PSK", "3/4"), (12603, "H", 22000000, "DVB-S", "QPSK", "5/6"),
            (12699, "H", 22000000, "DVB-S2", "8PSK", "2/3"),
        ]
        self.sat = []
        for rf, pol, rs, ds, mod, fec in tp:
            ts[0] += 1
            bits = {"QPSK": 2, "8PSK": 3, "16APSK": 4, "32APSK": 5}[mod]
            num, den = (int(x) for x in fec.split("/"))
            self.sat.append({"rf": rf * 1e6, "pol": pol, "rs": rs, "delsys": ds, "mod": mod,
                             "code_rate": fec, "beta": 0.35 if ds == "DVB-S" else 0.20,
                             "tsid": ts[0], "onid": 1, "snr": 12.5 + r.normal(0, 1.2),
                             "services": services(int(r.integers(4, 12)), 0.7, prov="SES"),
                             "mbps": rs * bits * num / den * 0.97 / 1e6})

    def sat_nit(self):
        return [{"delsys": t["delsys"], "freq": int(t["rf"]), "polarization": t["pol"],
                 "symbol_rate": t["rs"], "modulation": t["mod"], "tsid": t["tsid"]} for t in self.sat]

    def cable_nit(self):
        out = []
        for c in self.cable:
            if not c.get("nit"):
                continue
            if c["delsys"] == "DVB-C":
                out.append({"delsys": "DVB-C", "freq": int(c["freq"]), "symbol_rate": c["rs"],
                            "modulation": c["mod"], "tsid": c["tsid"]})
        return out


# --- constellations ------------------------------------------------------------------------

def constellation_points(mod):
    m = mod.upper()
    if m.startswith("QAM"):
        n = int(m[3:])
        k = int(round(math.sqrt(n)))
        if k * k != n:  # QAM128 : on se contente d'une croix approximative
            k = 12
        ax = np.arange(k) * 2 - (k - 1)
        pts = (ax[:, None] + 1j * ax[None, :]).ravel()
    elif m == "QPSK":
        pts = np.exp(1j * (np.pi / 4 + np.pi / 2 * np.arange(4)))
    elif m == "8PSK":
        pts = np.exp(1j * (np.pi / 4 * np.arange(8)))
    elif m == "16APSK":
        pts = np.concatenate([np.exp(1j * (np.pi / 4 + np.pi / 2 * np.arange(4))),
                              2.85 * np.exp(1j * (np.pi / 12 + np.pi / 6 * np.arange(12)))])
    elif m == "32APSK":
        pts = np.concatenate([np.exp(1j * (np.pi / 4 + np.pi / 2 * np.arange(4))),
                              2.84 * np.exp(1j * (np.pi / 12 + np.pi / 6 * np.arange(12))),
                              5.27 * np.exp(1j * (np.pi / 16 * np.arange(16)))])
    else:
        pts = np.exp(1j * (np.pi / 4 + np.pi / 2 * np.arange(4)))
    return pts / np.sqrt(np.mean(np.abs(pts) ** 2))


def rrc_amp(f, rs, beta):
    """Amplitude du filtre en racine de cosinus surélevé (1 au centre)."""
    af = np.abs(f)
    f1, f2 = rs * (1 - beta) / 2, rs * (1 + beta) / 2
    h = np.zeros_like(af)
    h[af <= f1] = 1.0
    mid = (af > f1) & (af < f2)
    h[mid] = np.sqrt(0.5 * (1 + np.cos(np.pi / (beta * rs) * (af[mid] - f1))))
    return h


# --- état du faux récepteur -----------------------------------------------------------------

class Mock:
    def __init__(self, args):
        self.args = args
        self.world = World(args.seed)
        self.rng = np.random.default_rng(args.seed + 7)
        self.lock = threading.Lock()          # une seule commande exclusive à la fois
        self.t0 = time.time()
        self.has_lnb = not args.no_lnb
        self.lnb = {"volt": "off", "tone": False}
        self.app = None                      # accord de « tvheadend »
        self.suspended_until = 0.0
        self.last_status = time.time()
        self.monitor = None
        self.exclusive = None                # {"state", "freq", "delsys"} pendant capture/probe
        self.overflows = 0
        self.const_until = 0.0
        if args.tune == "dvbt2":
            self.app = {"chan": self.world.cable[-2], "freq": 659e6, "plp": 0}
        elif args.tune == "dvbc":
            self.app = {"chan": next(c for c in self.world.cable if c["freq"] == 323e6), "freq": 323e6}
        elif args.tune == "dvbs2":
            self.lnb = {"volt": "13", "tone": False}
            t = next(t for t in self.world.sat if t["rf"] == 11362e6)
            self.app = {"chan": t, "freq": t["rf"] - LO_LOW, "sat": True}
        if self.app:
            self.app["since"] = time.time()

    # -- utilitaires --

    def app_active(self):
        return self.app is not None and time.time() >= self.suspended_until

    def take_radio(self, req):
        if self.app_active() and not req.get("force"):
            return False
        if self.app is not None:
            self.suspended_until = time.time() + 20.0
            self.app["since"] = time.time() + 20.0
        if self.monitor and req.get("cmd") != "monitor":
            self.monitor = None
        return True

    def lo(self):
        return LO_HIGH if self.lnb["tone"] else LO_LOW

    def pol(self):
        return {"13": "V", "18": "H"}.get(str(self.lnb["volt"]))

    def sat_visible(self):
        """Porteuses satellite visibles en FI avec l'état actuel du LNB : (FI réelle, tp)."""
        pol = self.pol()
        if not self.has_lnb or pol is None:
            return []
        lo = self.lo() + LO_ERROR
        out = []
        for t in self.world.sat:
            if t["pol"] == pol:
                fi = t["rf"] - lo
                if 900e6 < fi < 2200e6:
                    out.append((fi, t))
        return out

    # -- synthèse d'un signal --

    def synth(self, fc, fs, n, gain):
        rng = self.rng
        df = fs / n
        f = fc + (np.arange(n) - n // 2) * df
        X = np.zeros(n, dtype=np.complex128)
        sat = fc >= 900e6
        if sat:
            if self.pol() is None or not self.has_lnb:
                dens = -100.0 - 60.0            # LNB non alimenté : bruit du B210 seul (dBm/Hz)
                dens = np.full(n, dens)
            else:
                dens = -64.0 - 4.0 * (f - 950e6) / 1200e6 - 10 * np.log10(27.5e6)
        else:
            dens = np.full(n, self.world.cable_floor_8m - 10 * np.log10(8e6))
        pn = 10 ** (dens / 10.0) * df
        X += np.sqrt(pn / 2) * (rng.standard_normal(n) + 1j * rng.standard_normal(n))

        def add_sc(fch, rs, beta, mod, p_dbm):
            K = int(round(n * rs / fs))
            if K < 8:
                return
            pts = constellation_points(mod)
            a = pts[rng.integers(len(pts), size=K)]
            A = np.fft.fft(a)
            M = int(rs * (1 + beta) / 2 / df) + 1
            m = np.arange(-M, M + 1)
            c0 = int(round((fch - fc) / df)) + n // 2
            bins = c0 + m
            ok = (bins >= 0) & (bins < n)
            if not ok.any():
                return
            g = math.sqrt(10 ** (p_dbm / 10.0)) / K
            X[bins[ok]] += g * rrc_amp(m[ok] * df, K * df, beta) * A[m[ok] % K]

        def add_ofdm(fch, occ, p_dbm):
            sel = np.abs(f - fch) < occ / 2
            nb = int(sel.sum())
            if nb == 0:
                return
            tot = int(round(occ / df))
            amp = math.sqrt(10 ** (p_dbm / 10.0) / max(tot, 1) / 2)
            X[sel] += amp * (rng.standard_normal(nb) + 1j * rng.standard_normal(nb))

        lo_edge, hi_edge = fc - fs / 2, fc + fs / 2
        if sat:
            tilt = lambda fi: -52.0 - 4.0 * (fi - 950e6) / 1200e6  # noqa: E731
            for fi, t in self.sat_visible():
                if fi + t["rs"] > lo_edge and fi - t["rs"] < hi_edge:
                    add_sc(fi, t["rs"], t["beta"], t["mod"], tilt(fi) + (t["snr"] - 12.5))
        else:
            for c in self.world.cable:
                half = c.get("bw", 8e6) / 2
                if c["freq"] + half < lo_edge or c["freq"] - half > hi_edge:
                    continue
                if c["kind"] == "sc":
                    add_sc(c["freq"], c["rs"], c["beta"], c["mod"], c["power"])
                else:
                    add_ofdm(c["freq"], c["occ"], c["power"])

        # filtre analogique / numérique : coupe au-delà de 0,45 fs
        rel = np.abs(f - fc) / fs
        h = np.ones(n)
        edge = rel > 0.44
        h[edge] = 10 ** (-40 * (rel[edge] - 0.44) / 0.06 / 20)
        X *= h
        total_dbm = 10 * math.log10(max(np.sum(np.abs(X) ** 2), 1e-30))
        if gain == "auto":
            gain = int(np.clip(round(-24.0 - total_dbm - REF), 0, 76))
        else:
            gain = int(gain)
        x = np.fft.ifft(np.fft.ifftshift(X)) * n
        x *= 10 ** ((gain + REF) / 20.0)
        x += 0.003 * (1 + 1j)                # résidu de composante continue
        x = np.clip(x.real, -1, 1) + 1j * np.clip(x.imag, -1, 1)
        level = 10 * math.log10(max(np.mean(np.abs(x) ** 2), 1e-30))
        return x.astype(np.complex64), gain, level

    # -- recherche d'un multiplex pour probe/monitor --

    def find(self, delsys, freq, rs, bw):
        if delsys in ("DVB-S", "DVB-S2"):
            for fi, t in self.sat_visible():
                tol = min(0.1 * t["rs"], 3e6)
                if (abs(fi - freq) < tol and t["delsys"] == delsys
                        and (rs == 0 or abs(rs - t["rs"]) < 0.005 * t["rs"])):
                    return t, fi
            return None, None
        for c in self.world.cable:
            if c.get("delsys") != delsys or abs(c["freq"] - freq) > 0.6e6:
                continue
            if delsys == "DVB-C" and abs(rs - c["rs"]) > 0.005 * c["rs"]:
                continue
            if delsys in ("DVB-T", "DVB-T2") and bw and abs(bw - c["bw"]) > 1:
                continue
            return c, c["freq"]
        return None, None

    # -- commandes --

    def cmd_status(self, req):
        self.last_status = time.time()
        now = time.time()
        if self.monitor and now - self.monitor["last_status"] > 30:
            self.monitor = None
        if self.monitor:
            self.monitor["last_status"] = now
        chans = []
        radio = {"source": "uhd", "freq": 0, "rate": 0, "bandwidth": 0, "gain": 0, "agc": "level",
                 "level_dbfs": None, "input_dbm": None, "overflows": self.overflows, "filled": 0,
                 "lnb": {"present": self.has_lnb, "volt": self.lnb["volt"], "tone": self.lnb["tone"]}}
        a0 = {"adapter": 0, "state": "idle", "delsys": None, "freq": None, "symbol_rate": None,
              "bandwidth": None, "rolloff": None, "locked": False, "snr_db": None,
              "freq_offset_hz": None, "modulation": None, "code_rate": None, "pilots": None,
              "fft": None, "guard": None, "plp": None, "ts_packets": 0, "ts_errors": 0,
              "ts_mbps": None, "fec_blocks": 0, "fec_errors": 0, "uptime": 0}
        a1 = dict(a0, adapter=1)
        if self.exclusive:
            e = self.exclusive
            radio.update(freq=e["freq"], rate=e.get("rate", 0), gain=e.get("gain", 40),
                         agc="search", level_dbfs=-24.0)
            a0.update(state=e["state"], delsys=e.get("delsys"), freq=e["freq"])
        elif self.monitor:
            m = self.monitor
            if self.app is not None:
                self.suspended_until = now + 20.0
            t, fi = m["t"], m["fi"]
            wob = 3.0 * math.sin(now / 7.0) + 1.5 * math.sin(now / 2.3)
            gain = 38
            lvl = -27.0 + wob + self.rng.normal(0, 0.3) if t else -41.0 + self.rng.normal(0, 0.5)
            radio.update(freq=m["freq"], rate=max(2 * m["rs"], 4e6), bandwidth=m["rs"] * 1.35 or 4e6,
                         gain=gain, agc="level", level_dbfs=round(lvl, 1),
                         input_dbm=round(lvl - gain + OFFSET, 1))
            locked = bool(t) and m["rs"] > 0 and (t["snr"] + wob) > 4
            a0.update(state="monitor", delsys=m["delsys"], freq=m["freq"], symbol_rate=m["rs"],
                      rolloff=m["rolloff"], locked=locked,
                      snr_db=round(t["snr"] + wob + self.rng.normal(0, 0.2), 1) if locked else None,
                      freq_offset_hz=int(fi - m["freq"]) if t else None,
                      modulation=t["mod"] if locked else None,
                      code_rate=t["code_rate"] if locked else None,
                      pilots=True if locked and m["delsys"] == "DVB-S2" else None,
                      uptime=round(now - m["since"], 1))
            if not self.args.strict:
                a0.update(level_dbfs=round(lvl, 1), gain=gain, input_dbm=round(lvl - gain + OFFSET, 1))
        elif self.app_active():
            c = self.app["chan"]
            up = now - self.app["since"]
            is_sat = self.app.get("sat")
            gain = 31 if not is_sat else 44
            lvl = -24.0 + self.rng.normal(0, 0.4)
            radio.update(freq=self.app["freq"], rate=10240000 if not is_sat else 56000000,
                         bandwidth=9020000 if not is_sat else 30000000, gain=gain,
                         agc="best", level_dbfs=round(lvl, 1), input_dbm=round(lvl - gain + OFFSET, 1))
            self.overflows += int(self.rng.random() < 0.02)
            radio["overflows"] = self.overflows
            pk = int(up * c["mbps"] * 1e6 / 1504)
            a0.update(state="tuned", delsys=c["delsys"], freq=self.app["freq"],
                      locked=True, snr_db=round(c["snr"] + self.rng.normal(0, 0.15), 1),
                      freq_offset_hz=int(13100 + self.rng.normal(0, 40)),
                      modulation=c["mod"], code_rate=c.get("code_rate"),
                      ts_packets=pk, ts_errors=int(pk * 1.5e-5),
                      ts_mbps=round(c["mbps"] + self.rng.normal(0, 0.05), 2),
                      fec_blocks=int(up * 900), fec_errors=int(up * 6), uptime=round(up, 1))
            if c["delsys"] in ("DVB-T", "DVB-T2"):
                a0.update(bandwidth=8000000, fft=c["fft"], guard=c["guard"],
                          plp=self.app.get("plp") if c["delsys"] == "DVB-T2" else None)
            elif c["delsys"] == "DVB-C":
                a0.update(symbol_rate=c["rs"], rolloff=0.15)
            else:
                a0.update(symbol_rate=c["rs"], rolloff=c["beta"], pilots=True,
                          freq_offset_hz=int(-LO_ERROR + self.rng.normal(0, 400)))
        chans = [a0, a1]
        return {"ok": True, "radio": radio, "channels": chans}

    def cmd_constellation(self, req):
        ad = int(req.get("adapter", 0))
        n = max(16, min(8192, int(req.get("points", 2048))))
        st = self.cmd_status_peek()
        ch = st["channels"][ad] if 0 <= ad < len(st["channels"]) else None
        if not ch or not ch.get("locked") or not ch.get("modulation"):
            return {"ok": True, "modulation": ch.get("modulation") if ch else None, "points": []}
        pts = constellation_points(ch["modulation"])
        s = pts[self.rng.integers(len(pts), size=n)]
        snr = ch.get("snr_db") or 20
        sig = math.sqrt(10 ** (-snr / 10) / 2)
        s = s * np.exp(1j * self.rng.normal(0, 0.01, n))
        s = s + sig * (self.rng.standard_normal(n) + 1j * self.rng.standard_normal(n))
        return {"ok": True, "modulation": ch["modulation"],
                "points": [[round(float(v.real), 4), round(float(v.imag), 4)] for v in s]}

    def cmd_status_peek(self):
        saved = self.last_status
        mon = dict(self.monitor) if self.monitor else None
        st = self.cmd_status({})
        self.last_status = saved
        if mon:
            self.monitor["last_status"] = mon["last_status"]
        return st

    def cmd_capture(self, req):
        fs = float(req.get("rate", 10e6))
        n = int(req.get("samples", 65536))
        if not 1 <= n <= 1048576:
            return {"ok": False, "error": "samples must be in 1..1048576"}
        if not 200e3 <= fs <= 61.44e6:
            return {"ok": False, "error": "unsupported rate"}
        fc = float(req.get("freq", 0))
        if not 70e6 <= fc <= 6e9:
            return {"ok": False, "error": "frequency out of range"}
        if not self.lock.acquire(timeout=60):
            return {"ok": False, "error": "busy"}
        try:
            if not self.take_radio(req):
                return {"ok": False, "error": "busy"}
            self.exclusive = {"state": "probe", "freq": fc, "rate": fs}
            x, gain, level = self.synth(fc, fs, n, req.get("gain", "auto"))
            time.sleep(0.02 + n / fs)
            iq = np.empty(2 * n, dtype="<f4")
            iq[0::2], iq[1::2] = x.real, x.imag
            return {"ok": True, "freq": int(fc), "rate": int(fs), "gain": gain,
                    "level_dbfs": round(level, 1), "iq": base64.b64encode(iq.tobytes()).decode()}
        finally:
            self.exclusive = None
            self.lock.release()

    def cmd_probe(self, req):
        ds = req.get("delsys")
        if ds not in ("DVB-S", "DVB-S2", "DVB-C", "DVB-T", "DVB-T2"):
            return {"ok": False, "error": "unknown delsys"}
        if not self.lock.acquire(timeout=120):
            return {"ok": False, "error": "busy"}
        try:
            if not self.take_radio(req):
                return {"ok": False, "error": "busy"}
            freq = float(req.get("freq", 0))
            rs = float(req.get("symbol_rate", 0) or 0)
            bw = float(req.get("bandwidth", 0) or 0)
            self.exclusive = {"state": "probe", "freq": freq, "delsys": ds}
            secs = float(req.get("seconds", 6))
            time.sleep(min(secs, 30) * self.args.speed)
            c, real_f = self.find(ds, freq, rs, bw)
            gain = int(self.rng.integers(30, 45))
            base = {"ok": True, "locked": False, "snr_db": None, "freq_offset_hz": None,
                    "modulation": None, "code_rate": None, "ts_mbps": None, "ts_errors": 0,
                    "gain": gain, "level_dbfs": round(-24.0 + self.rng.normal(0, 0.5), 1),
                    "tsid": None, "onid": None, "network": "", "services": [], "nit": []}
            if c is None:
                if not self.args.strict:
                    base["pids"] = []
                return base
            plp = int(req.get("plp", -1))
            services, tsid = c.get("services"), c.get("tsid")
            if ds == "DVB-T2":
                plps = c["plps"]
                if plp < 0:
                    plp = min(plps)
                if plp not in plps:
                    return base
                tsid, services = plps[plp]
            base.update(locked=True, snr_db=round(c["snr"] + self.rng.normal(0, 0.3), 1),
                        freq_offset_hz=int(real_f - freq + self.rng.normal(0, 300)),
                        modulation=c["mod"], code_rate=c.get("code_rate"),
                        ts_mbps=round(c["mbps"], 1), ts_errors=int(self.rng.integers(0, 30)),
                        tsid=tsid, onid=c.get("onid"),
                        network="Réseau simulé" if not c.get("docsis") else "",
                        services=services or [])
            if not c.get("docsis"):
                base["nit"] = self.world.sat_nit() if ds.startswith("DVB-S") else self.world.cable_nit()
            if not self.args.strict:
                base["pids"] = [0x1FFE] if c.get("docsis") else [0, 0x10, 0x11, 0x12, 0x100, 0x101]
                if ds in ("DVB-T", "DVB-T2"):
                    base.update(fft=c["fft"], guard=c["guard"], bandwidth=int(c["bw"]))
                if ds == "DVB-T2":
                    base.update(plp=plp, plps=sorted(c["plps"]))
                if ds.startswith("DVB-S"):
                    base.update(rolloff=c["beta"], pilots=ds == "DVB-S2", symbol_rate=c["rs"])
                if ds == "DVB-C":
                    base.update(symbol_rate=c["rs"])
            return base
        finally:
            self.exclusive = None
            self.lock.release()

    def cmd_lnb(self, req):
        if not self.has_lnb:
            return {"ok": False, "error": "no LNB controller"}
        volt = str(req.get("volt", "off"))
        if volt not in ("13", "18", "off"):
            return {"ok": False, "error": "bad volt"}
        self.lnb = {"volt": volt, "tone": bool(req.get("tone"))}
        return {"ok": True}

    def cmd_monitor(self, req):
        if req.get("action") == "stop":
            self.monitor = None
            return {"ok": True}
        if req.get("action") != "start":
            return {"ok": False, "error": "bad action"}
        if not self.take_radio(req):
            return {"ok": False, "error": "busy"}
        freq = float(req.get("freq", 0))
        rs = float(req.get("symbol_rate", 0) or 0)
        ds = req.get("delsys", "DVB-S2")
        now = time.time()
        self.monitor = {"delsys": ds, "freq": freq, "rs": rs, "rolloff": req.get("rolloff", 0.2),
                        "since": now, "last_status": now, "t": None, "fi": None}
        # la porteuse est recherchée à chaque statut (le LNB peut changer après le start)
        return {"ok": True}

    def refresh_monitor(self):
        m = self.monitor
        if not m:
            return
        t, fi = None, None
        for f_i, tp in self.sat_visible():
            if abs(f_i - m["freq"]) < max(2e6, 0.25 * tp["rs"]):
                if m["rs"] == 0 or (abs(m["rs"] - tp["rs"]) < 0.01 * tp["rs"] and
                                    (tp["delsys"] == m["delsys"] or m["delsys"] == "DVB-S2")):
                    t, fi = tp, f_i
                elif m["rs"] == 0:
                    t, fi = tp, f_i
        m["t"], m["fi"] = t, fi

    def handle(self, req):
        cmd = req.get("cmd")
        if self.monitor and time.time() - self.monitor["last_status"] > 30:
            self.monitor = None
        if cmd == "status":
            self.refresh_monitor()
            return self.cmd_status(req)
        if cmd == "constellation":
            self.refresh_monitor()
            return self.cmd_constellation(req)
        if cmd == "capture":
            return self.cmd_capture(req)
        if cmd == "probe":
            return self.cmd_probe(req)
        if cmd == "lnb":
            return self.cmd_lnb(req)
        if cmd == "monitor":
            return self.cmd_monitor(req)
        return {"ok": False, "error": f"unknown command: {cmd}"}


class ReqHandler(socketserver.StreamRequestHandler):
    def handle(self):
        mock = self.server.mock
        for line in self.rfile:
            line = line.strip()
            if not line:
                continue
            try:
                req = json.loads(line)
                rep = mock.handle(req) if isinstance(req, dict) else {"ok": False, "error": "bad request"}
            except ValueError:
                rep = {"ok": False, "error": "bad json"}
            except Exception as e:  # noqa: BLE001
                rep = {"ok": False, "error": f"internal: {e}"}
            if mock.args.verbose:
                short = {k: v for k, v in rep.items() if k not in ("iq", "points", "services", "nit")}
                print(f"<- {json.dumps(req)[:160]}\n-> {json.dumps(short)[:200]}", file=sys.stderr)
            try:
                self.wfile.write((json.dumps(rep) + "\n").encode())
                self.wfile.flush()
            except OSError:
                return


class Server(socketserver.ThreadingMixIn, socketserver.UnixStreamServer):
    daemon_threads = True


def main():
    ap = argparse.ArgumentParser(description="Faux dvbrx (socket de contrôle) pour dvbrx-web.")
    ap.add_argument("--socket", "-C", default="/tmp/dvbrx-mock-0.sock", help="chemin du socket")
    ap.add_argument("--tune", choices=["dvbt2", "dvbc", "dvbs2", "none"], default="dvbt2",
                    help="chaîne regardée par « tvheadend » (busy sans force)")
    ap.add_argument("--no-lnb", action="store_true", help="pas de contrôleur de LNB (-L absent)")
    ap.add_argument("--strict", action="store_true",
                    help="s'en tenir au contrat (pas de pids/plps/fft/guard dans probe, "
                         "pas de niveau dans le canal monitor)")
    ap.add_argument("--speed", type=float, default=0.15,
                    help="facteur appliqué à la durée des probe (1 = temps réel)")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--verbose", "-v", action="store_true")
    args = ap.parse_args()
    if os.path.exists(args.socket):
        os.unlink(args.socket)
    srv = Server(args.socket, ReqHandler)
    os.chmod(args.socket, 0o660)
    srv.mock = Mock(args)
    print(f"mock dvbrx sur {args.socket} (tune={args.tune}, lnb={'non' if args.no_lnb else 'oui'}, "
          f"strict={'oui' if args.strict else 'non'})", flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        srv.server_close()
        try:
            os.unlink(args.socket)
        except OSError:
            pass


if __name__ == "__main__":
    main()
