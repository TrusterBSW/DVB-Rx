"""Client du socket de contrôle de dvbrx (une requête JSON par ligne)."""

import base64
import json
import socket

import numpy as np


class DvbrxError(Exception):
    """Erreur renvoyée par dvbrx ("ok": false) ou socket injoignable."""

    def __init__(self, message, busy=False, unreachable=False):
        super().__init__(message)
        self.busy = busy
        self.unreachable = unreachable


class Busy(DvbrxError):
    def __init__(self):
        super().__init__("busy", busy=True)


class DvbrxClient:
    def __init__(self, path):
        self.path = path

    def request(self, req, timeout=5.0):
        """Envoie une requête sur une connexion neuve et renvoie la réponse (dict)."""
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.settimeout(timeout)
            s.connect(self.path)
        except OSError as e:
            raise DvbrxError(f"dvbrx injoignable ({self.path}) : {e.strerror or e}",
                             unreachable=True) from None
        try:
            s.sendall((json.dumps(req) + "\n").encode())
            f = s.makefile("rb")
            line = f.readline()
        except socket.timeout:
            raise DvbrxError(f"pas de réponse de dvbrx en {timeout:g} s à « {req.get('cmd')} »") from None
        except OSError as e:
            raise DvbrxError(f"erreur de communication avec dvbrx : {e}") from None
        finally:
            s.close()
        if not line:
            raise DvbrxError("dvbrx a fermé la connexion sans répondre")
        try:
            rep = json.loads(line)
        except ValueError:
            raise DvbrxError("réponse de dvbrx illisible (JSON invalide)") from None
        return rep

    def call(self, req, timeout=5.0):
        """Comme request(), mais lève DvbrxError / Busy si "ok" est faux."""
        rep = self.request(req, timeout)
        if not rep.get("ok"):
            err = str(rep.get("error", "erreur inconnue"))
            if err == "busy":
                raise Busy()
            raise DvbrxError(err)
        return rep

    # --- commandes -----------------------------------------------------------------

    def status(self):
        return self.call({"cmd": "status"}, timeout=3.0)

    def constellation(self, adapter, points=2048):
        return self.call({"cmd": "constellation", "adapter": adapter, "points": points}, timeout=3.0)

    def capture(self, freq, rate, samples, gain="auto", bandwidth=0, antenna=None, force=False):
        req = {"cmd": "capture", "freq": int(round(freq)), "rate": int(round(rate)),
               "samples": int(samples), "gain": gain, "bandwidth": int(bandwidth)}
        if antenna:
            req["antenna"] = antenna
        if force:
            req["force"] = True
        rep = self.call(req, timeout=30.0 + samples / max(rate, 1.0))
        raw = base64.b64decode(rep.get("iq", ""))
        iq = np.frombuffer(raw, dtype="<f4")
        iq = iq[: iq.size // 2 * 2].astype(np.float32).view(np.complex64)
        rep["iq"] = iq
        return rep

    def probe(self, delsys, freq, symbol_rate=0, bandwidth=0, rolloff=0.0, modulation="auto",
              plp=-1, seconds=6, antenna=None, force=False):
        req = {"cmd": "probe", "delsys": delsys, "freq": int(round(freq)),
               "symbol_rate": int(round(symbol_rate)), "bandwidth": int(bandwidth),
               "rolloff": rolloff, "modulation": modulation, "plp": int(plp),
               "seconds": seconds}
        if antenna:
            req["antenna"] = antenna
        if force:
            req["force"] = True
        return self.call(req, timeout=seconds + 30.0)

    def lnb(self, volt, tone, force=False):
        req = {"cmd": "lnb", "volt": str(volt), "tone": bool(tone)}
        if force:
            req["force"] = True
        return self.call(req, timeout=10.0)

    def monitor_start(self, delsys, freq, symbol_rate, rolloff, antenna=None, force=False):
        req = {"cmd": "monitor", "action": "start", "delsys": delsys, "freq": int(round(freq)),
               "symbol_rate": int(round(symbol_rate)), "rolloff": rolloff}
        if antenna:
            req["antenna"] = antenna
        if force:
            req["force"] = True
        return self.call(req, timeout=15.0)

    def monitor_stop(self):
        return self.call({"cmd": "monitor", "action": "stop"}, timeout=10.0)
