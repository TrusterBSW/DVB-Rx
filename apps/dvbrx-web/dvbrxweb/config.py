"""Lecture de la configuration (/etc/dvbrxd/web.conf, format INI)."""

import configparser
import glob
import os
import re

DEFAULTS = {
    "web": {
        "listen": "0.0.0.0",
        "port": "8099",
        "data_dir": "/var/lib/dvbrx-web",
    },
    "dvbrx": {
        # vide : tous les /run/dvbrx/dvbrx-*.sock présents au démarrage
        "sockets": "",
        # vide : on garde input_dbm calculé par dvbrx (option -P)
        "input_offset_db": "",
    },
    "lnb": {
        "lo_low_mhz": "9750",
        "lo_high_mhz": "10600",
        "switch_mhz": "11700",
        "settle_s": "0.5",
    },
    "inputs": {
        "antenna_sat": "",
        "antenna_cable": "",
    },
    "cable": {
        "start_mhz": "100",
        "stop_mhz": "870",
        "threshold_db": "6",
        "grid_mhz": "8",
        "grid_offset_mhz": "auto",
        "merge_dip_db": "1.0",
    },
    "sat": {
        "if_start_mhz": "950",
        "if_stop_mhz": "2150",
        "threshold_db": "4",
        "min_symbol_rate": "1000000",
        "rolloff_s2": "0.20",
        "rolloff_s": "0.35",
    },
    "scan": {
        "rate": "56000000",
        "useful_mhz": "48",
        "samples": "262144",
        "fft": "2048",
        "probe_seconds": "6",
        "probe_seconds_t2": "8",
        "cyclic_threshold_db": "15",
    },
}

DEFAULT_SOCKET_GLOB = "/run/dvbrx/dvbrx-*.sock"


class Config:
    def __init__(self, path=None):
        cp = configparser.ConfigParser(interpolation=None, inline_comment_prefixes=("#", ";"))
        cp.read_dict(DEFAULTS)
        self.path = path
        if path:
            if not os.path.exists(path):
                raise SystemExit(f"dvbrx-web : fichier de configuration introuvable : {path}")
            cp.read(path, encoding="utf-8")
        self.cp = cp
        g = cp.get
        self.listen = g("web", "listen")
        self.port = cp.getint("web", "port")
        self.data_dir = g("web", "data_dir")
        off = g("dvbrx", "input_offset_db").strip()
        self.input_offset_db = float(off) if off else None
        self.lo_low = cp.getfloat("lnb", "lo_low_mhz") * 1e6
        self.lo_high = cp.getfloat("lnb", "lo_high_mhz") * 1e6
        self.lnb_switch = cp.getfloat("lnb", "switch_mhz") * 1e6
        self.lnb_settle = cp.getfloat("lnb", "settle_s")
        self.antenna_sat = g("inputs", "antenna_sat").strip() or None
        self.antenna_cable = g("inputs", "antenna_cable").strip() or None
        self.cable_start = cp.getfloat("cable", "start_mhz") * 1e6
        self.cable_stop = cp.getfloat("cable", "stop_mhz") * 1e6
        self.cable_threshold = cp.getfloat("cable", "threshold_db")
        self.cable_grid = cp.getfloat("cable", "grid_mhz") * 1e6
        go = g("cable", "grid_offset_mhz").strip().lower()
        self.cable_grid_offset = None if go in ("", "auto") else float(go) * 1e6
        self.cable_merge_dip = cp.getfloat("cable", "merge_dip_db")
        self.sat_if_start = cp.getfloat("sat", "if_start_mhz") * 1e6
        self.sat_if_stop = cp.getfloat("sat", "if_stop_mhz") * 1e6
        self.sat_threshold = cp.getfloat("sat", "threshold_db")
        self.sat_min_rs = cp.getfloat("sat", "min_symbol_rate")
        self.rolloff_s2 = cp.getfloat("sat", "rolloff_s2")
        self.rolloff_s = cp.getfloat("sat", "rolloff_s")
        self.scan_rate = cp.getfloat("scan", "rate")
        self.scan_useful = cp.getfloat("scan", "useful_mhz") * 1e6
        self.scan_samples = cp.getint("scan", "samples")
        self.scan_fft = cp.getint("scan", "fft")
        self.probe_seconds = cp.getfloat("scan", "probe_seconds")
        self.probe_seconds_t2 = cp.getfloat("scan", "probe_seconds_t2")
        self.cyclic_threshold = cp.getfloat("scan", "cyclic_threshold_db")
        self.sockets = parse_sockets(g("dvbrx", "sockets"))

    def set_sockets(self, items):
        self.sockets = parse_sockets(",".join(items))

    def public(self):
        """Ce que la page a besoin de connaître."""
        return {
            "sockets": [{"id": i, "name": s["name"], "path": s["path"]}
                        for i, s in enumerate(self.sockets)],
            "lnb": {"lo_low": self.lo_low, "lo_high": self.lo_high, "switch": self.lnb_switch},
            "cable": {"start": self.cable_start, "stop": self.cable_stop},
            "sat": {"if_start": self.sat_if_start, "if_stop": self.sat_if_stop},
            "input_offset_db": self.input_offset_db,
        }


def parse_sockets(text):
    """« chemin » ou « nom=chemin », séparés par des virgules ou des retours à la ligne."""
    out = []
    for item in re.split(r"[,\n]+", text or ""):
        item = item.strip()
        if not item:
            continue
        if "=" in item:
            name, path = (x.strip() for x in item.split("=", 1))
        else:
            path = item
            m = re.search(r"dvbrx-(\d+)\.sock$", path)
            name = f"dvbrx {m.group(1)}" if m else os.path.basename(path)
        out.append({"name": name, "path": path})
    if not out:
        for path in sorted(glob.glob(DEFAULT_SOCKET_GLOB)) or ["/run/dvbrx/dvbrx-0.sock"]:
            m = re.search(r"dvbrx-(\d+)\.sock$", path)
            out.append({"name": f"dvbrx {m.group(1)}" if m else path, "path": path})
    return out
