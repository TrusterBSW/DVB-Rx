"""Scans rapide (FFT par fenêtres) et complet (identification de la norme + probe)."""

import json
import os
import threading
import time

import numpy as np

from . import dsp
from .client import Busy, DvbrxError

DOCSIS_RS = 6952000

# combinaisons du LNB universel : (identifiant, libellé, tension, 22 kHz)
SAT_COMBOS = [
    ("VL", "Satellite VL : verticale, bande basse (13 V, sans 22 kHz)", "13", False),
    ("VH", "Satellite VH : verticale, bande haute (13 V + 22 kHz)", "13", True),
    ("HL", "Satellite HL : horizontale, bande basse (18 V, sans 22 kHz)", "18", False),
    ("HH", "Satellite HH : horizontale, bande haute (18 V + 22 kHz)", "18", True),
]
SAT_IDS = [c[0] for c in SAT_COMBOS]


class Cancelled(Exception):
    pass


def combo_of(volt, tone):
    for cid, _, v, t in SAT_COMBOS:
        if str(v) == str(volt) and bool(t) == bool(tone):
            return cid
    return None


def pol_of(cid):
    return "V" if cid[0] == "V" else "H"


class Job:
    """Un scan en tâche de fond sur une instance dvbrx."""

    def __init__(self, mode, params):
        self.mode = mode
        self.params = params
        self.state = "running"          # running | done | cancelled | busy | error
        self.progress = 0.0
        self.message = "démarrage…"
        self.error = None
        self.cancel = threading.Event()
        self.started = time.time()
        self.finished = None
        self.thread = None
        self.log = []

    def info(self):
        return {"mode": self.mode, "state": self.state, "progress": round(self.progress, 4),
                "message": self.message, "error": self.error,
                "params": {k: v for k, v in self.params.items() if not k.startswith("_")},
                "started": self.started, "finished": self.finished, "log": self.log[-30:]}

    def say(self, msg, progress=None):
        self.message = msg
        if progress is not None:
            self.progress = max(0.0, min(1.0, progress))
        self.log.append(time.strftime("%H:%M:%S ") + msg)

    def check(self):
        if self.cancel.is_set():
            raise Cancelled()


class Scanner:
    """Gère les scans d'une instance dvbrx (un seul à la fois) et leurs résultats."""

    def __init__(self, cfg, client, key):
        self.cfg = cfg
        self.client = client
        self.key = key
        self.lock = threading.Lock()
        self.job = None
        self.results = {"quick": None, "full": None}
        self.dvbrx_offset = None
        for mode in ("quick", "full"):
            try:
                with open(self._path(mode), encoding="utf-8") as fh:
                    self.results[mode] = json.load(fh)
            except (OSError, ValueError):
                pass

    # --- persistance -------------------------------------------------------------------

    def _path(self, mode):
        return os.path.join(self.cfg.data_dir, f"{self.key}-{mode}.json")

    def _save(self, mode, result):
        self.results[mode] = result
        try:
            os.makedirs(self.cfg.data_dir, exist_ok=True)
            tmp = self._path(mode) + ".tmp"
            with open(tmp, "w", encoding="utf-8") as fh:
                json.dump(result, fh, ensure_ascii=False)
            os.replace(tmp, self._path(mode))
        except OSError as e:
            result["save_error"] = f"résultat non enregistré : {e}"

    # --- tâches ------------------------------------------------------------------------

    def running(self):
        return self.job is not None and self.job.state == "running"

    def start(self, mode, params):
        with self.lock:
            if self.running():
                raise DvbrxError("un scan est déjà en cours sur ce récepteur")
            job = Job(mode, params)
            self.job = job
            job.thread = threading.Thread(target=self._run, args=(job,), daemon=True,
                                          name=f"scan-{self.key}")
            job.thread.start()
            return job

    def _run(self, job):
        try:
            self._learn_offset()
            if job.mode == "quick":
                res = self.quick_scan(job, 0.0, 1.0)
                self._save("quick", res)
            else:
                res = self.full_scan(job)
                self._save("full", res)
            job.state = "done"
            job.say("terminé", 1.0)
        except Cancelled:
            job.state = "cancelled"
            job.say("scan annulé")
        except Busy:
            job.state = "busy"
            job.error = "busy"
            job.say("le récepteur est occupé (tvheadend l'utilise)")
        except DvbrxError as e:
            job.state = "error"
            job.error = str(e)
            job.say(f"erreur : {e}")
        except Exception as e:  # noqa: BLE001 (on veut un message dans la page)
            job.state = "error"
            job.error = f"{type(e).__name__}: {e}"
            job.say(f"erreur interne : {job.error}")
        finally:
            job.finished = time.time()

    # --- briques -----------------------------------------------------------------------

    def _offset(self):
        """Offset de input_dbm = level_dbfs - gain + offset : celui de la configuration, sinon
        celui de dvbrx (déduit de son statut), sinon 0 dB (défaut de dvbrx)."""
        if self.cfg.input_offset_db is not None:
            return self.cfg.input_offset_db
        if self.dvbrx_offset is not None:
            return self.dvbrx_offset
        return 0.0

    def _learn_offset(self):
        try:
            r = self.client.status().get("radio") or {}
            vals = [r.get(k) for k in ("input_dbm", "level_dbfs", "gain")]
            if all(isinstance(v, (int, float)) and not isinstance(v, bool) for v in vals):
                self.dvbrx_offset = round(vals[0] - vals[1] + vals[2], 1)
        except DvbrxError:
            pass

    def _input_dbm(self, level_dbfs, gain):
        return level_dbfs - gain + self._offset()

    def _gain_offset(self, rep):
        """dBFS -> dBm estimé : - gain + offset."""
        g = rep.get("gain")
        g = float(g) if isinstance(g, (int, float)) else 0.0
        return -g + self._offset()

    def _sweep(self, job, start, stop, antenna, p0, p1, label):
        """Spectre assemblé de start à stop (Hz) par fenêtres de cfg.scan_useful."""
        cfg = self.cfg
        rate, useful = cfg.scan_rate, min(cfg.scan_useful, cfg.scan_rate * 0.95)
        centers = np.arange(start + useful / 2, stop + useful / 2 - 1, useful)
        fs_all, ps_all = [], []
        for i, fc in enumerate(centers):
            job.check()
            job.say(f"{label} : fenêtre {i + 1}/{len(centers)} ({fc / 1e6:.0f} MHz)",
                    p0 + (p1 - p0) * i / len(centers))
            rep = self.client.capture(fc, rate, cfg.scan_samples, gain="auto", bandwidth=0,
                                      antenna=antenna, force=job.params.get("force", False))
            fs = float(rep.get("rate", rate))
            fcr = float(rep.get("freq", fc))
            p = dsp.psd(rep["iq"], cfg.scan_fft)
            n = p.size
            f = fcr + (np.arange(n) - n // 2) * fs / n
            keep = (f >= fc - useful / 2) & (f < fc + useful / 2) & (f >= start) & (f < stop)
            fs_all.append(f[keep])
            ps_all.append(dsp.db(p[keep]) + self._gain_offset(rep))
        if not fs_all:
            return np.array([]), np.array([])
        f = np.concatenate(fs_all)
        p = np.concatenate(ps_all)
        order = np.argsort(f)
        return f[order], p[order]

    def _acquire_radio(self, job):
        """Petite capture : vérifie qu'on a la radio avant de toucher au LNB."""
        if job.params.get("_radio_ok"):
            return
        self.client.capture(self.cfg.sat_if_start + 25e6, 10e6, 4096, gain="auto",
                            antenna=self.cfg.antenna_sat, force=job.params.get("force", False))
        job.params["_radio_ok"] = True

    def _lnb_state(self):
        try:
            st = self.client.status()
            lnb = (st.get("radio") or {}).get("lnb") or {}
            return lnb.get("volt"), lnb.get("tone")
        except DvbrxError:
            return None, None

    def _set_lnb(self, job, volt, tone):
        self.client.lnb(volt, tone, force=job.params.get("force", False))
        job.params["_lnb"] = (str(volt), bool(tone))
        time.sleep(self.cfg.lnb_settle)

    def _restore_lnb(self, job, saved):
        volt, tone = saved
        if volt is None or job.params.get("_lnb") is None:
            return
        try:
            self.client.lnb(volt, bool(tone), force=True)  # la radio est encore à nous
            job.params["_lnb"] = (str(volt), bool(tone))
        except DvbrxError:
            job.params["_lnb"] = None

    def lo_for(self, cid):
        return self.cfg.lo_high if cid[1] == "H" else self.cfg.lo_low

    # --- scan rapide -------------------------------------------------------------------

    def quick_scan(self, job, p0, p1):
        cfg, prm = self.cfg, job.params
        sat = bool(prm.get("sat"))
        cstart = float(prm.get("cable_start") or cfg.cable_start)
        cstop = float(prm.get("cable_stop") or cfg.cable_stop)
        if cstop <= cstart + 8e6:
            raise DvbrxError("plage câble invalide")
        nwin_c = int(np.ceil((cstop - cstart) / cfg.scan_useful))
        nwin_s = int(np.ceil((cfg.sat_if_stop - cfg.sat_if_start) / cfg.scan_useful))
        total = nwin_c + (4 * nwin_s if sat else 0)
        result = {"mode": "quick", "time": time.time(), "sat": sat, "lines": [],
                  "cable_range": [cstart, cstop]}

        # ligne câble / terrestre
        pc = p0 + (p1 - p0) * nwin_c / total
        f, p = self._sweep(job, cstart, cstop, cfg.antenna_cable, p0, pc, "câble")
        chans, floor, off = dsp.detect_cable(f, p, cfg.cable_grid, cfg.cable_grid_offset,
                                             cfg.cable_threshold, cfg.cable_merge_dip)
        result["lines"].append({
            "id": "cable", "label": "Câble / terrestre", "kind": "cable",
            "start": cstart, "stop": cstop, "lo": 0, "volt": None, "tone": None,
            "spectrum": dsp.decimate_for_display(f[0], f[1] - f[0], p) if f.size > 1 else None,
            "floor_dbm": None if floor is None else round(floor, 1),
            "grid_offset": off, "channels": chans})
        job.say(f"câble : {len(chans)} canal(aux) occupé(s)", pc)

        if sat:
            saved = self._lnb_state()
            try:
                self._acquire_radio(job)
                for i, (cid, label, volt, tone) in enumerate(SAT_COMBOS):
                    job.check()
                    a = pc + (p1 - pc) * i / 4
                    b = pc + (p1 - pc) * (i + 1) / 4
                    line = {"id": cid, "label": label, "kind": "sat",
                            "start": cfg.sat_if_start, "stop": cfg.sat_if_stop,
                            "lo": self.lo_for(cid), "volt": volt, "tone": tone,
                            "spectrum": None, "floor_dbm": None, "channels": []}
                    result["lines"].append(line)
                    try:
                        self._set_lnb(job, volt, tone)
                    except Busy:
                        raise
                    except DvbrxError as e:
                        msg = str(e)
                        if "no LNB" in msg:
                            msg = "pas de contrôleur de LNB (dvbrx lancé sans -L)"
                        for rest in SAT_COMBOS[i:]:
                            if rest[0] != cid:
                                result["lines"].append({
                                    "id": rest[0], "label": rest[1], "kind": "sat",
                                    "start": cfg.sat_if_start, "stop": cfg.sat_if_stop,
                                    "lo": self.lo_for(rest[0]), "volt": rest[2],
                                    "tone": rest[3], "spectrum": None, "channels": [],
                                    "error": msg})
                        line["error"] = msg
                        job.say(f"satellite : {msg}")
                        break
                    f, p = self._sweep(job, cfg.sat_if_start, cfg.sat_if_stop, cfg.antenna_sat,
                                       a, b, cid)
                    chans, floor = dsp.detect_sat(f, p, cfg.sat_threshold, cfg.sat_min_rs)
                    lo = line["lo"]
                    for ch in chans:
                        ch["rf"] = ch["freq"] + lo
                    line["spectrum"] = dsp.decimate_for_display(f[0], f[1] - f[0], p) if f.size > 1 else None
                    line["floor_dbm"] = None if floor is None else round(floor, 1)
                    line["channels"] = chans
                    job.say(f"{cid} : {len(chans)} porteuse(s)", b)
            finally:
                self._restore_lnb(job, saved)
        return result

    # --- scan complet ------------------------------------------------------------------

    def full_scan(self, job):
        cfg, prm = self.cfg, job.params
        sat = bool(prm.get("sat"))
        quick = self.results.get("quick")
        need_ids = ["cable"] + (SAT_IDS if sat else [])
        have = {ln["id"] for ln in (quick or {}).get("lines", []) if not ln.get("error")}
        if prm.get("rescan") or not quick or not all(i in have for i in need_ids):
            job.say("pas de scan rapide utilisable : on le refait d'abord", 0.0)
            quick = self.quick_scan(job, 0.0, 0.3)
            self._save("quick", quick)
            base = 0.3
        else:
            base = 0.0

        lines = []
        for ln in quick["lines"]:
            if ln["id"] not in need_ids:
                continue
            nl = {k: v for k, v in ln.items() if k != "channels"}
            nl["channels"] = []
            for ch in ln.get("channels", []):
                c = dict(ch)
                c["source"] = "scan"
                c["line"] = ln["id"]
                nl["channels"].append(c)
            lines.append(nl)
        by_id = {ln["id"]: ln for ln in lines}

        # doublons satellite : une porteuse vue dans les deux bandes n'est sondée que dans
        # sa bande nominale (seuil de commutation)
        for ln in lines:
            if ln["kind"] != "sat":
                continue
            cid = ln["id"]
            sib = by_id.get(cid[0] + ("L" if cid[1] == "H" else "H"))
            for ch in ln["channels"]:
                nominal = (ch["rf"] >= cfg.lnb_switch) == (cid[1] == "H")
                if not nominal and sib:
                    if any(abs(o["rf"] - ch["rf"]) < max(2e6, ch["width"] / 4)
                           for o in sib["channels"]):
                        ch["skip"] = "doublon (vu dans la bande nominale)"

        work = [(ln, ch) for ln in lines for ch in ln["channels"]]
        saved = self._lnb_state() if sat else (None, None)
        done = 0
        try:
            while work:
                ln, ch = work.pop(0)
                job.check()
                total = done + len(work) + 1
                prog = base + (1 - base) * done / total
                name = f"{ln['id']} {(ch.get('rf') or ch['freq']) / 1e6:.1f} MHz"
                if ch.get("skip") or ch.get("kind") == "wide":
                    if ch.get("kind") == "wide":
                        ch["status"] = "bloc large non sondé (OFDM DOCSIS 3.1 ?)"
                        ch["class"] = "wide"
                    else:
                        ch["status"] = ch["skip"]
                    done += 1
                    continue
                job.say(f"canal {done + 1}/{total} : {name}", prog)
                if ln["kind"] == "sat":
                    if job.params.get("_lnb") != (str(ln["volt"]), bool(ln["tone"])):
                        self._acquire_radio(job)
                        self._set_lnb(job, ln["volt"], ln["tone"])
                    self._probe_sat(job, ln, ch)
                else:
                    self._probe_cable(job, ln, ch)
                for entry in ch.get("probe", {}).get("nit", []) or []:
                    self._nit_add(entry, ln, by_id, work)
                for extra in ch.pop("_extra", []):
                    ln["channels"].append(extra)
                done += 1
        finally:
            if sat:
                self._restore_lnb(job, saved)

        for ln in lines:
            ln["channels"].sort(key=lambda c: (c["freq"], c.get("plp") or 0))
        summary = {"locked": 0, "docsis": 0, "services": 0, "failed": 0}
        for ln in lines:
            for ch in ln["channels"]:
                if ch.get("docsis"):
                    summary["docsis"] += 1
                elif ch.get("locked"):
                    summary["locked"] += 1
                    summary["services"] += len(ch.get("services", []))
                elif not ch.get("skip") and ch.get("kind") != "wide":
                    summary["failed"] += 1
        return {"mode": "full", "time": time.time(), "sat": sat, "lines": lines,
                "quick_time": quick.get("time"), "summary": summary}

    def _probe(self, job, ch, delsys, freq, **kw):
        antenna = self.cfg.antenna_sat if delsys.startswith("DVB-S") else self.cfg.antenna_cable
        seconds = self.cfg.probe_seconds_t2 if delsys == "DVB-T2" else self.cfg.probe_seconds
        rep = self.client.probe(delsys, freq, seconds=seconds, antenna=antenna,
                                force=job.params.get("force", False), **kw)
        ch.setdefault("tries", []).append({"delsys": delsys, "locked": bool(rep.get("locked")),
                                           **{k: v for k, v in kw.items() if v}})
        return rep

    def _apply(self, ch, delsys, rep, **params):
        ch["delsys"] = delsys
        ch["locked"] = bool(rep.get("locked"))
        ch.update({k: v for k, v in params.items()})
        info = {k: v for k, v in rep.items() if k not in ("ok", "services", "nit")}
        ch["probe"] = dict(info, nit=rep.get("nit") or [])
        ch["services"] = rep.get("services") or []
        for k in ("snr_db", "freq_offset_hz", "modulation", "code_rate", "ts_mbps", "ts_packets", "ts_errors",
                  "tsid", "onid", "network", "fft", "guard", "pilots", "rolloff",
                  "fec_blocks", "fec_errors"):
            if k in rep:
                ch[k] = rep[k]
        # plus de 1 % de paquets perdus : signal trop faible pour sa modulation, que la
        # recherche automatique de QAM a pu mal identifier
        npk, nerr = rep.get("ts_packets") or 0, rep.get("ts_errors") or 0
        ch["weak"] = bool(ch["locked"] and npk and nerr > 0.01 * npk)
        if rep.get("plp") is not None and delsys == "DVB-T2":
            ch["plp"] = rep["plp"]
        if "level_dbfs" in rep and "gain" in rep:
            try:
                ch["input_dbm"] = round(self._input_dbm(float(rep["level_dbfs"]), float(rep["gain"])), 1)
            except (TypeError, ValueError):
                pass

    def _is_docsis(self, ch, rep):
        pids = rep.get("pids")
        if isinstance(pids, list) and pids:
            # dvbrx : [pid, paquets, paquets brouillés] ; DOCSIS = PID 0x1FFE (+ bourrage 0x1FFF)
            seen = {int(p[0] if isinstance(p, list) else p) for p in pids}
            return 0x1FFE in seen and seen <= {0x1FFE, 0x1FFF}
        # sans liste de PID (contrat actuel) : 6,952 MBd verrouillé, ni PAT ni service
        rs = ch.get("symbol_rate") or 0
        return (abs(rs - DOCSIS_RS) < 0.003 * DOCSIS_RS and not rep.get("services")
                and rep.get("tsid") in (None, 0) and not rep.get("nit"))

    def _probe_cable(self, job, ln, ch):
        cfg = self.cfg
        ant = cfg.antenna_cable
        if ch.get("source") == "nit":
            return self._probe_known(job, ln, ch, ant)
        rep = self.client.capture(ch["freq"], 16e6, 262144, gain="auto", bandwidth=0,
                                  antenna=ant, force=job.params.get("force", False))
        cyc = dsp.cyclic_line(rep["iq"], float(rep.get("rate", 16e6)), 8e6, 3.0e6, 7.6e6,
                              cfg.cyclic_threshold)
        ch["cyclic"] = cyc
        if cyc["found"]:
            ch["class"] = "single"
            rs = dsp.snap_symbol_rate(cyc["rs"])
            r = self._probe(job, ch, "DVB-C", ch["freq"], symbol_rate=rs, rolloff=0.15)
            self._apply(ch, "DVB-C", r, symbol_rate=rs, bandwidth=None)
            if ch["locked"] and self._is_docsis(ch, r):
                ch["docsis"] = True
                ch["status"] = "DOCSIS (internet), non exporté"
            elif ch["locked"]:
                ch["status"] = "verrouillé, nombreuses erreurs (signal faible)" if ch["weak"] else "verrouillé"
            else:
                ch["status"] = "porteuse unique non décodée"
            return
        ch["class"] = "ofdm"
        for ds in ("DVB-T", "DVB-T2"):
            job.check()
            kw = {"bandwidth": 8000000}
            if ds == "DVB-T2":
                kw["plp"] = -1
            r = self._probe(job, ch, ds, ch["freq"], **kw)
            if r.get("locked"):
                self._apply(ch, ds, r, bandwidth=8000000, symbol_rate=None)
                ch["status"] = "verrouillé"
                if ds == "DVB-T2":
                    self._t2_other_plps(job, ln, ch, r)
                return
        # dernier essai : un DVB-C dont la raie cyclique était trop faible
        r = self._probe(job, ch, "DVB-C", ch["freq"], symbol_rate=6875000, rolloff=0.15)
        if r.get("locked"):
            ch["class"] = "single"
            self._apply(ch, "DVB-C", r, symbol_rate=6875000, bandwidth=None)
            ch["status"] = "verrouillé"
            return
        ch["locked"] = False
        ch["status"] = "OFDM non identifié (ni DVB-T ni DVB-T2)"

    def _t2_other_plps(self, job, ln, ch, rep):
        """Multi-PLP : si dvbrx annonce la liste des PLP ("plps"), on sonde les autres."""
        plps = rep.get("plps")
        if not isinstance(plps, list):
            return
        cur = rep.get("plp", 0) or 0
        for plp in plps:
            if plp == cur:
                continue
            job.check()
            extra = {k: ch[k] for k in ("freq", "width", "kind", "level_dbm", "margin_db", "line")
                     if k in ch}
            extra["source"] = "plp"
            r = self._probe(job, extra, "DVB-T2", ch["freq"], bandwidth=8000000, plp=plp)
            self._apply(extra, "DVB-T2", r, bandwidth=8000000, symbol_rate=None, plp=plp)
            extra["class"] = "ofdm"
            extra["status"] = "verrouillé" if extra["locked"] else "PLP non décodé"
            ch.setdefault("_extra", []).append(extra)

    def _probe_sat(self, job, ln, ch):
        cfg = self.cfg
        ant = cfg.antenna_sat
        if ch.get("source") == "nit":
            return self._probe_known(job, ln, ch, ant)
        est = ch.get("symbol_rate_est") or ch["width"]
        fs = float(min(cfg.scan_rate, max(4e6, 2.5 * est)))
        rep = self.client.capture(ch["freq"], fs, 262144, gain="auto", bandwidth=0,
                                  antenna=ant, force=job.params.get("force", False))
        cyc = dsp.cyclic_line(rep["iq"], float(rep.get("rate", fs)), est * 1.45,
                              0.7 * est, 1.3 * est, cfg.cyclic_threshold)
        ch["cyclic"] = cyc
        rs = dsp.snap_symbol_rate(cyc["rs"] if cyc["found"] else est,
                                  tol=0.002 if cyc["found"] else 0.03)
        ch["class"] = "single"
        for ds, ro in (("DVB-S2", cfg.rolloff_s2), ("DVB-S", cfg.rolloff_s)):
            job.check()
            r = self._probe(job, ch, ds, ch["freq"], symbol_rate=rs, rolloff=ro)
            if r.get("locked"):
                self._apply(ch, ds, r, symbol_rate=rs, bandwidth=None)
                ch.setdefault("rolloff", ro)
                ch["status"] = "verrouillé"
                self._sat_rf(ln, ch)
                return
        ch["locked"] = False
        ch["symbol_rate"] = rs
        ch["status"] = "porteuse non décodée (ni DVB-S2 ni DVB-S)"
        self._sat_rf(ln, ch)

    def _sat_rf(self, ln, ch):
        off = ch.get("freq_offset_hz") or 0
        ch["if_freq"] = ch["freq"] + off
        ch["rf"] = ch["if_freq"] + ln["lo"]
        ch["polarization"] = pol_of(ln["id"])

    def _probe_known(self, job, ln, ch, ant):
        """Multiplex annoncé par la NIT : on sonde directement avec ses paramètres."""
        ds = ch.get("delsys_hint") or "DVB-C"
        kw = {}
        if ds in ("DVB-T", "DVB-T2"):
            kw["bandwidth"] = int(ch.get("bandwidth") or 8000000)
            if ds == "DVB-T2":
                kw["plp"] = int(ch.get("plp", -1) if ch.get("plp") is not None else -1)
        else:
            kw["symbol_rate"] = int(ch.get("symbol_rate") or 0)
            kw["rolloff"] = {"DVB-C": 0.15, "DVB-S": cfg_ro(self.cfg, "s"),
                             "DVB-S2": cfg_ro(self.cfg, "s2")}.get(ds, 0.2)
            if ds == "DVB-C" and ch.get("modulation_hint"):
                kw["modulation"] = ch["modulation_hint"]
        order = [ds]
        if ds == "DVB-S2":
            order.append("DVB-S")
        elif ds == "DVB-S":
            order.insert(0, "DVB-S2")
        for d in order:
            if d != ds and "rolloff" in kw:
                kw["rolloff"] = cfg_ro(self.cfg, "s2" if d == "DVB-S2" else "s")
            r = self._probe(job, ch, d, ch["freq"], **kw)
            if r.get("locked"):
                self._apply(ch, d, r, **{k: v for k, v in kw.items()
                                         if k in ("symbol_rate", "bandwidth", "plp")})
                ch["status"] = "verrouillé (annoncé par la NIT)"
                if ch["delsys"] == "DVB-C" and self._is_docsis(ch, r):
                    ch["docsis"] = True
                    ch["status"] = "DOCSIS (internet), non exporté"
                break
        else:
            ch["locked"] = False
            ch["delsys"] = ds
            ch["status"] = "annoncé par la NIT, non reçu"
        ch["class"] = "ofdm" if ch.get("delsys") in ("DVB-T", "DVB-T2") else "single"
        if ln["kind"] == "sat":
            self._sat_rf(ln, ch)

    def _nit_add(self, entry, src_line, by_id, work):
        """Ajoute un multiplex annoncé par la NIT s'il n'est pas déjà dans la liste."""
        try:
            ds = str(entry.get("delsys") or "")
            freq = float(entry.get("freq") or 0)
        except (TypeError, ValueError):
            return
        if freq <= 0:
            return
        cfg = self.cfg
        if ds.startswith("DVB-S"):
            if freq > 3e9:  # fréquence réelle (descripteur satellite) -> FI
                pol = str(entry.get("polarization") or entry.get("pol") or "").upper()[:1]
                if pol not in ("H", "V"):
                    if src_line["kind"] != "sat":
                        return
                    pol = pol_of(src_line["id"])
                band = "H" if freq >= cfg.lnb_switch else "L"
                cid = pol + band
                ifreq = freq - (cfg.lo_high if band == "H" else cfg.lo_low)
            else:
                if src_line["kind"] != "sat":
                    return
                cid, ifreq = src_line["id"], freq
            line = by_id.get(cid)
            if line is None or not (cfg.sat_if_start <= ifreq <= cfg.sat_if_stop):
                return
            rs = float(entry.get("symbol_rate") or 0)
            tol = max(2e6, rs / 4)
        else:
            line = by_id.get("cable")
            if line is None:
                return
            ifreq = freq
            rs = float(entry.get("symbol_rate") or 0)
            tol = 1.5e6
        plp = entry.get("plp")
        for ch in line["channels"]:
            f0 = ch.get("if_freq") or ch["freq"]
            if abs(f0 - ifreq) < tol and (ch.get("kind") != "wide") and (
                    plp is None or ch.get("plp") in (None, plp)):
                return
            if ch.get("kind") == "wide" and abs(ch["freq"] - ifreq) < ch["width"] / 2:
                return
        for ln2, ch in work:
            if ln2 is line and abs(ch["freq"] - ifreq) < tol:
                return
        ch = {"freq": ifreq, "width": rs * 1.2 if rs else float(entry.get("bandwidth") or 8e6),
              "kind": "nit", "source": "nit", "line": line["id"], "delsys_hint": ds,
              "symbol_rate": int(rs) or None, "bandwidth": entry.get("bandwidth"),
              "modulation_hint": entry.get("modulation"), "plp": plp,
              "nit_tsid": entry.get("tsid")}
        if line["kind"] == "sat":
            ch["rf"] = ifreq + line["lo"]
        line["channels"].append(ch)
        work.append((line, ch))


def cfg_ro(cfg, which):
    return cfg.rolloff_s2 if which == "s2" else cfg.rolloff_s
