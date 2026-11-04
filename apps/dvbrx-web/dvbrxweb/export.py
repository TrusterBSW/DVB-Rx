"""Export des multiplex trouvés au format dvbv5 (fichiers dvb-scan de tvheadend / dvbv5-scan).

Fréquences : en Hz pour le câble et le terrestre, en kHz pour le satellite (usage dvbv5,
cf. /usr/share/tvheadend/data/dvb-scan/dvb-s/Astra-19.2E : FREQUENCY = 11362000).
"""

import re
import time

S_FEC = {"1/2", "2/3", "3/4", "5/6", "7/8", "3/5", "4/5", "8/9", "9/10", "1/3", "1/4", "2/5"}
T_FEC = {"1/2", "2/3", "3/4", "5/6", "7/8", "3/5", "4/5"}
GUARDS = {"1/4", "1/8", "1/16", "1/32", "1/128", "19/128", "19/256"}
FFT_MODES = {1024: "1K", 2048: "2K", 4096: "4K", 8192: "8K", 16384: "16K", 32768: "32K"}


def modulation_v5(mod, delsys):
    m = re.sub(r"[^A-Z0-9]", "", str(mod or "").upper())
    if not m:
        return "QAM/AUTO" if delsys in ("DVB-C", "DVB-T", "DVB-T2") else None
    if m.startswith("QAM"):
        return "QAM/" + m[3:]
    if m.endswith("QAM"):
        return "QAM/" + m[:-3]
    if m == "QPSK":
        return "QPSK"
    if m in ("8PSK", "PSK8"):
        return "PSK/8"
    mm = re.match(r"(\d+)APSK", m) or re.match(r"APSK(\d+)", m)
    if mm:
        return "APSK/" + mm.group(1)
    return None


def _fec(rate, allowed):
    r = str(rate or "").strip()
    return r if r in allowed else "AUTO"


def channel_block(ch):
    ds = ch["delsys"]
    out = ["[CHANNEL]"]
    add = lambda k, v: out.append(f"\t{k} = {v}")  # noqa: E731
    if ds == "DVB-C":
        add("DELIVERY_SYSTEM", "DVBC/ANNEX_A")
        add("FREQUENCY", int(round(ch["freq"] / 1e5) * 1e5))
        add("SYMBOL_RATE", int(ch.get("symbol_rate") or 6875000))
        add("INNER_FEC", "NONE")
        add("MODULATION", modulation_v5(ch.get("modulation"), ds) or "QAM/AUTO")
        add("INVERSION", "AUTO")
    elif ds in ("DVB-T", "DVB-T2"):
        add("DELIVERY_SYSTEM", "DVBT" if ds == "DVB-T" else "DVBT2")
        add("FREQUENCY", int(round(ch["freq"] / 1e5) * 1e5))
        add("BANDWIDTH_HZ", int(ch.get("bandwidth") or 8000000))
        add("CODE_RATE_HP", _fec(ch.get("code_rate"), T_FEC) if ds == "DVB-T" else "AUTO")
        add("CODE_RATE_LP", "NONE" if ds == "DVB-T" else "AUTO")
        add("MODULATION", modulation_v5(ch.get("modulation"), ds) if ds == "DVB-T" else "QAM/AUTO")
        add("TRANSMISSION_MODE", FFT_MODES.get(ch.get("fft"), "AUTO"))
        add("GUARD_INTERVAL", ch.get("guard") if ch.get("guard") in GUARDS else "AUTO")
        add("HIERARCHY", "NONE" if ds == "DVB-T" else "AUTO")
        if ds == "DVB-T2":
            plp = ch.get("plp")
            add("STREAM_ID", int(plp) if isinstance(plp, int) and plp >= 0 else 0)
        add("INVERSION", "AUTO")
    else:  # DVB-S / DVB-S2
        add("DELIVERY_SYSTEM", "DVBS" if ds == "DVB-S" else "DVBS2")
        add("FREQUENCY", int(round(ch["rf"] / 1e6) * 1000))  # kHz, arrondi au MHz
        add("POLARIZATION", "HORIZONTAL" if ch.get("polarization") == "H" else "VERTICAL")
        add("SYMBOL_RATE", int(ch.get("symbol_rate") or 0))
        add("INNER_FEC", _fec(ch.get("code_rate"), S_FEC))
        add("MODULATION", modulation_v5(ch.get("modulation"), ds) or "QPSK")
        if ds == "DVB-S2":
            add("ROLLOFF", "AUTO")
        add("INVERSION", "AUTO")
    return "\n".join(out)


def exportable(ch, kind):
    if not ch.get("locked") or ch.get("docsis") or not ch.get("delsys"):
        return False
    if kind == "sat":
        return ch["delsys"] in ("DVB-S", "DVB-S2") and ch.get("rf")
    return ch["delsys"] in ("DVB-C", "DVB-T", "DVB-T2")


def build(result, kind, source_name=""):
    """Texte du fichier dvbv5 pour kind = "cable" (câble/terrestre) ou "sat"."""
    seen = set()
    blocks = []
    for ln in (result or {}).get("lines", []):
        if (kind == "sat") != (ln.get("kind") == "sat"):
            continue
        for ch in ln.get("channels", []):
            if not exportable(ch, kind):
                continue
            block = channel_block(ch)
            key = block.replace("\n", "")
            if key in seen:
                continue
            seen.add(key)
            names = ", ".join(s.get("name", "") for s in ch.get("services", [])[:6])
            comment = f"# {ch['delsys']}"
            if names:
                comment += f" : {names}"
            blocks.append(comment + "\n" + block)
    when = time.strftime("%Y-%m-%d %H:%M", time.localtime((result or {}).get("time", time.time())))
    title = "câble / terrestre" if kind == "cable" else "satellite (LNB universel)"
    head = (f"# dvbrx-web : multiplex {title} trouvés par le scan complet du {when}\n"
            f"# récepteur : {source_name}\n"
            "# format dvbv5 (dvbv5-scan, tvheadend data/dvb-scan)\n")
    if kind == "sat":
        head += "# FREQUENCY en kHz (fréquence réelle, arrondie au MHz)\n"
    return head + "\n" + "\n\n".join(blocks) + ("\n" if blocks else "")
