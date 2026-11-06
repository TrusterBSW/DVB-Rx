"""Serveur HTTP de dvbrx-web (bibliothèque standard uniquement + numpy)."""

import argparse
import json
import mimetypes
import os
import re
import threading
import time
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

from . import VERSION, export
from .client import Busy, DvbrxClient, DvbrxError
from .config import Config
from .scan import Scanner, combo_of, pol_of

STATIC_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "static")


class App:
    def __init__(self, cfg):
        self.cfg = cfg
        self.sockets = []
        for i, s in enumerate(cfg.sockets):
            key = re.sub(r"[^A-Za-z0-9_-]+", "_", os.path.basename(s["path"]).rsplit(".", 1)[0]) or f"s{i}"
            client = DvbrxClient(s["path"])
            self.sockets.append({"name": s["name"], "path": s["path"], "client": client,
                                 "scanner": Scanner(cfg, client, key), "monitor": None})
        self.lock = threading.Lock()

    def cards(self):
        """Carte (modèle, numéro de série) derrière chaque socket, pour le sélecteur."""
        out = []
        for sk in self.sockets:
            try:
                r = sk["client"].request({"cmd": "status"}, timeout=1.0).get("radio") or {}
                out.append({"model": r.get("model") or "", "serial": r.get("serial") or ""})
            except (DvbrxError, OSError, ValueError):
                out.append({"model": "", "serial": ""})
        return out

    def sock(self, q):
        try:
            i = int(q.get("s", 0))
        except (TypeError, ValueError):
            i = 0
        if not 0 <= i < len(self.sockets):
            raise DvbrxError(f"récepteur {i} inconnu")
        return self.sockets[i]

    # --- enrichissement du statut ------------------------------------------------------

    def enrich_status(self, st, sk):
        cfg = self.cfg
        radio = st.get("radio") or {}
        lnb = radio.get("lnb") or {}
        lo = None
        combo = None
        if lnb.get("present") and str(lnb.get("volt")) in ("13", "18"):
            lo = cfg.lo_high if lnb.get("tone") else cfg.lo_low
            combo = combo_of(lnb.get("volt"), lnb.get("tone"))
        radio["lnb_lo"] = lo
        radio["lnb_combo"] = combo
        if cfg.input_offset_db is not None and _num(radio.get("level_dbfs")) and _num(radio.get("gain")):
            radio["input_dbm_dvbrx"] = radio.get("input_dbm")
            radio["input_dbm"] = round(radio["level_dbfs"] - radio["gain"] + cfg.input_offset_db, 1)
        for ch in st.get("channels") or []:
            ds = str(ch.get("delsys") or "")
            if ds.startswith("DVB-S") and _num(ch.get("freq")):
                ch["if_freq"] = ch["freq"]
                if lo is not None:
                    ch["rf_freq"] = ch["freq"] + lo
                    ch["polarization"] = pol_of(combo) if combo else None
            if cfg.input_offset_db is not None and _num(ch.get("level_dbfs")) and _num(ch.get("gain")):
                ch["input_dbm"] = round(ch["level_dbfs"] - ch["gain"] + cfg.input_offset_db, 1)
        mon = sk.get("monitor")
        if mon and time.time() - mon["started"] > 5 and not any(
                c.get("state") == "monitor" for c in st.get("channels") or []):
            sk["monitor"] = mon = None  # arrêté par dvbrx (sécurité des 30 s)
        st["web_monitor"] = mon
        job = sk["scanner"].job
        st["web_scan"] = job.info() if job and job.state == "running" else None
        return st


def _num(v):
    return isinstance(v, (int, float)) and not isinstance(v, bool)


class Handler(BaseHTTPRequestHandler):
    server_version = f"dvbrx-web/{VERSION}"
    app: App = None

    def log_request(self, code="-", size="-"):  # journal discret : seulement les erreurs
        if str(getattr(code, "value", code)).startswith(("4", "5")):
            super().log_request(code, size)

    # --- réponses ----------------------------------------------------------------------

    def send_json(self, obj, code=200):
        body = json.dumps(obj, ensure_ascii=False).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def send_err(self, e, code=None):
        if isinstance(e, Busy):
            return self.send_json({"ok": False, "error": "busy",
                                   "message": "tvheadend (ou une autre application) utilise le récepteur"},
                                  code or 409)
        if isinstance(e, DvbrxError) and e.unreachable:
            return self.send_json({"ok": False, "error": str(e), "unreachable": True}, code or 502)
        return self.send_json({"ok": False, "error": str(e)}, code or 400)

    def send_text(self, text, ctype="text/plain; charset=utf-8", filename=None):
        body = text.encode()
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Cache-Control", "no-store")
        if filename:
            self.send_header("Content-Disposition", f'attachment; filename="{filename}"')
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def body_json(self):
        n = int(self.headers.get("Content-Length") or 0)
        if n <= 0:
            return {}
        if n > 1 << 20:
            raise DvbrxError("requête trop grande")
        raw = self.rfile.read(n)
        try:
            v = json.loads(raw or b"{}")
        except ValueError:
            raise DvbrxError("JSON invalide") from None
        return v if isinstance(v, dict) else {}

    def serve_static(self, path):
        if path in ("", "/"):
            path = "/index.html"
        rel = os.path.normpath(path.lstrip("/"))
        if rel.startswith("..") or os.path.isabs(rel):
            return self.send_error(HTTPStatus.NOT_FOUND)
        full = os.path.join(STATIC_DIR, rel)
        if not os.path.isfile(full):
            return self.send_error(HTTPStatus.NOT_FOUND)
        ctype = mimetypes.guess_type(full)[0] or "application/octet-stream"
        if ctype.startswith("text/") or ctype in ("application/javascript",):
            ctype += "; charset=utf-8"
        with open(full, "rb") as fh:
            data = fh.read()
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    # --- routes ------------------------------------------------------------------------

    def do_GET(self):
        u = urlparse(self.path)
        q = {k: v[-1] for k, v in parse_qs(u.query).items()}
        app = self.app
        try:
            if not u.path.startswith("/api/"):
                return self.serve_static(u.path)
            if u.path == "/api/config":
                return self.send_json(dict(app.cfg.public(), ok=True, version=VERSION,
                                           cards=app.cards()))
            sk = app.sock(q)
            if u.path == "/api/status":
                st = sk["client"].status()
                return self.send_json(app.enrich_status(st, sk))
            if u.path == "/api/constellation":
                pts = max(64, min(8192, int(q.get("points", 2048))))
                rep = sk["client"].constellation(int(q.get("adapter", 0)), pts)
                return self.send_json(rep)
            if u.path == "/api/scan/progress":
                job = sk["scanner"].job
                return self.send_json({"ok": True, "job": job.info() if job else None})
            if u.path == "/api/scan/result":
                mode = q.get("mode", "full")
                res = sk["scanner"].results.get(mode)
                return self.send_json({"ok": True, "result": res})
            if u.path == "/api/export":
                kind = "sat" if q.get("kind") == "sat" else "cable"
                res = sk["scanner"].results.get("full")
                if not res:
                    raise DvbrxError("pas de scan complet : lancez-en un d'abord")
                text = export.build(res, kind, f"{sk['name']} ({sk['path']})")
                fname = f"dvbrx-{'sat' if kind == 'sat' else 'cable'}-{time.strftime('%Y%m%d')}"
                return self.send_text(text, filename=fname)
            return self.send_json({"ok": False, "error": "inconnu"}, 404)
        except (DvbrxError, ValueError) as e:
            return self.send_err(e if isinstance(e, DvbrxError) else DvbrxError(str(e)))

    def do_POST(self):
        u = urlparse(self.path)
        app = self.app
        try:
            body = self.body_json()
            sk = app.sock(body)
            sc = sk["scanner"]
            if u.path == "/api/scan/start":
                mode = "full" if body.get("mode") == "full" else "quick"
                if sk.get("monitor"):
                    try:
                        app.enrich_status(sk["client"].status(), sk)
                    except DvbrxError:
                        pass
                if sk.get("monitor"):
                    raise DvbrxError("l'alignement satellite est en cours : arrêtez-le d'abord")
                params = {"sat": bool(body.get("sat")), "force": bool(body.get("force")),
                          "rescan": bool(body.get("rescan"))}
                for k in ("cable_start", "cable_stop"):
                    if body.get(k):
                        params[k] = float(body[k])
                job = sc.start(mode, params)
                return self.send_json({"ok": True, "job": job.info()})
            if u.path == "/api/scan/cancel":
                if sc.job:
                    sc.job.cancel.set()
                return self.send_json({"ok": True})
            if u.path == "/api/lnb":
                if sc.running():
                    raise DvbrxError("un scan est en cours")
                volt = str(body.get("volt", "off"))
                if volt not in ("13", "18", "off"):
                    raise DvbrxError("tension invalide")
                sk["client"].lnb(volt, bool(body.get("tone")), force=bool(body.get("force")))
                if sk.get("monitor"):
                    sk["monitor"].update(volt=volt, tone=bool(body.get("tone")))
                return self.send_json({"ok": True})
            if u.path == "/api/monitor/start":
                if sc.running():
                    raise DvbrxError("un scan est en cours sur ce récepteur : attendez sa fin ou annulez-le")
                ds = body.get("delsys") or "DVB-S2"
                gain = body.get("gain")  # gain fixe (dB) ; absent ou vide : automatique
                gain = None if gain in (None, "", "auto") else float(gain)
                if gain is not None and not (0 <= gain <= 120):
                    raise DvbrxError(f"gain invalide : {gain}")
                if ds in ("DVB-T", "DVB-T2"):  # antenne terrestre : pas de LNB
                    freq = float(body.get("freq") or 0)
                    if not (47e6 <= freq <= 1000e6):
                        raise DvbrxError(f"fréquence hors bande terrestre : {freq / 1e6:.1f} MHz")
                    bw = int(body.get("bandwidth") or 8000000)
                    if bw not in (6000000, 7000000, 8000000):
                        raise DvbrxError(f"largeur invalide : {bw}")
                    plp = body.get("plp")
                    plp = int(plp) if plp not in (None, "") and int(plp) >= 0 else -1
                    sk["client"].monitor_start(ds, freq, 0, 0, bandwidth=bw, plp=plp, gain=gain,
                                               antenna=app.cfg.antenna_cable, force=bool(body.get("force")))
                    sk["monitor"] = {"delsys": ds, "freq": freq, "bandwidth": bw, "plp": plp, "gain": gain,
                                     "volt": None, "tone": None, "started": time.time()}
                    return self.send_json({"ok": True, "monitor": sk["monitor"], "lnb_error": None})
                if ds not in ("DVB-S", "DVB-S2"):
                    raise DvbrxError("norme invalide pour l'alignement")
                freq = float(body.get("freq") or 0)
                if not (app.cfg.sat_if_start - 50e6 <= freq <= app.cfg.sat_if_stop + 50e6):
                    raise DvbrxError(f"fréquence FI hors bande : {freq / 1e6:.1f} MHz")
                rs = float(body.get("symbol_rate") or 0)
                ro = float(body.get("rolloff") or (app.cfg.rolloff_s2 if ds == "DVB-S2" else app.cfg.rolloff_s))
                sk["client"].monitor_start(ds, freq, rs, ro, antenna=app.cfg.antenna_sat, gain=gain,
                                           force=bool(body.get("force")))
                volt = str(body.get("volt", "13"))
                tone = bool(body.get("tone"))
                lnb_err = None
                if volt in ("13", "18", "off"):
                    try:
                        sk["client"].lnb(volt, tone, force=True)  # le moniteur vient de prendre la radio
                    except DvbrxError as e:
                        lnb_err = str(e)
                        if "no LNB" in lnb_err:
                            lnb_err = "pas de contrôleur de LNB (dvbrx lancé sans -L)"
                sk["monitor"] = {"delsys": ds, "freq": freq, "symbol_rate": rs, "rolloff": ro, "gain": gain,
                                 "volt": volt, "tone": tone, "started": time.time()}
                return self.send_json({"ok": True, "monitor": sk["monitor"], "lnb_error": lnb_err})
            if u.path == "/api/monitor/stop":
                sk["monitor"] = None
                try:
                    sk["client"].monitor_stop()
                except DvbrxError as e:
                    return self.send_json({"ok": True, "warning": str(e)})
                return self.send_json({"ok": True})
            return self.send_json({"ok": False, "error": "inconnu"}, 404)
        except (DvbrxError, ValueError, TypeError) as e:
            return self.send_err(e if isinstance(e, DvbrxError) else DvbrxError(str(e)))


def main(argv=None):
    ap = argparse.ArgumentParser(prog="dvbrx-web",
                                 description="Web supervision page of the dvbrx receiver.")
    ap.add_argument("--config", "-c", help="configuration file (INI), e.g. /etc/dvbrxd/web.conf")
    ap.add_argument("--listen", help="listening address (default 0.0.0.0)")
    ap.add_argument("--port", "-p", type=int, help="HTTP port (default 8099)")
    ap.add_argument("--socket", "-s", action="append",
                    help="dvbrx control socket, PATH or NAME=PATH (repeatable)")
    ap.add_argument("--data-dir", help="directory of the scan results")
    ap.add_argument("--version", action="version", version=f"dvbrx-web {VERSION}")
    a = ap.parse_args(argv)

    cfg = Config(a.config)
    if a.listen:
        cfg.listen = a.listen
    if a.port:
        cfg.port = a.port
    if a.socket:
        cfg.set_sockets(a.socket)
    if a.data_dir:
        cfg.data_dir = a.data_dir

    Handler.app = App(cfg)
    mimetypes.add_type("application/javascript", ".js")
    mimetypes.add_type("text/css", ".css")
    srv = ThreadingHTTPServer((cfg.listen, cfg.port), Handler)
    srv.daemon_threads = True
    socks = ", ".join(f"{s['name']} = {s['path']}" for s in cfg.sockets)
    print(f"dvbrx-web {VERSION} : http://{cfg.listen}:{cfg.port}/  (sockets : {socks} ; "
          f"données : {cfg.data_dir})", flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        srv.server_close()
    return 0
