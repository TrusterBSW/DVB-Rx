"""Traitements du signal du scan : spectre moyenné, détection des canaux, raie cyclique."""

import numpy as np

COMMON_SYMBOL_RATES = [
    # câble
    6875000, 6900000, 6952000, 6111000, 5217000, 5000000, 5360537, 6000000,
    # satellite
    27500000, 22000000, 29500000, 29700000, 30000000, 30000000, 27000000, 23000000,
    28000000, 28800000, 29900000, 31000000, 31200000, 25000000, 20000000, 21500000,
    15000000, 14400000, 12000000, 13333000, 10000000, 7200000, 7500000, 4800000,
    4500000, 3333000, 3000000, 2200000, 2000000,
]


def psd(iq, nfft=2048):
    """Densité spectrale moyennée (Welch, Hann, sans recouvrement), en puissance par case.

    La somme des cases vaut la puissance moyenne de iq (pleine échelle = 1 -> 0 dBFS).
    Renvoie les cases centrées (fftshift), de -fs/2 à +fs/2.
    """
    iq = np.asarray(iq, dtype=np.complex64)
    nseg = iq.size // nfft
    if nseg < 1:
        nfft = max(16, 1 << int(np.log2(max(iq.size, 16))))
        iq = np.resize(iq, nfft)
        nseg = 1
    w = np.hanning(nfft).astype(np.float32)
    seg = iq[: nseg * nfft].reshape(nseg, nfft) * w
    spec = np.fft.fft(seg, axis=1)
    p = np.mean(np.abs(spec) ** 2, axis=0) / (np.sum(w ** 2) * nfft)
    p = np.fft.fftshift(p)
    # résidu de composante continue / fuite d'OL : on interpole les cases centrales
    c = nfft // 2
    k = max(2, nfft // 1024)
    lo, hi = p[c - 3 * k:c - k], p[c + k + 1:c + 3 * k + 1]
    if lo.size and hi.size:
        p[c - k:c + k + 1] = 0.5 * (np.median(lo) + np.median(hi))
    return p


def db(x):
    return 10.0 * np.log10(np.maximum(x, 1e-30))


def smooth(x, n):
    if n <= 1:
        return x
    k = np.ones(int(n)) / int(n)
    return np.convolve(x, k, mode="same")


def decimate_for_display(f0, df, p_dbm, maxpts=4000):
    """Réduit un spectre (dBm par case) à <= maxpts points (moyenne en linéaire)."""
    n = p_dbm.size
    g = max(1, int(np.ceil(n / maxpts)))
    m = n // g
    if m == 0:
        return {"f0": f0, "df": df, "db": []}
    lin = 10 ** (p_dbm[: m * g] / 10.0)
    lin = lin.reshape(m, g).mean(axis=1)
    # correction pour garder l'échelle « dBm par case d'origine »
    out = db(lin)
    return {"f0": float(f0 + (g - 1) * df / 2), "df": float(df * g),
            "db": [round(float(v), 2) for v in out]}


# --- câble / terrestre -----------------------------------------------------------------

def _band_mean_db(f, lin, a, b):
    i0, i1 = np.searchsorted(f, [a, b])
    if i1 <= i0:
        return None
    return float(db(np.mean(lin[i0:i1])))


def _band_sum_dbm(f, lin, a, b):
    i0, i1 = np.searchsorted(f, [a, b])
    if i1 <= i0:
        return None
    return float(db(np.sum(lin[i0:i1])))


def detect_cable(f, p_dbm, grid=8e6, grid_offset=None, threshold_db=6.0, merge_dip_db=1.0):
    """Détection des canaux sur une grille fixe (8 MHz).

    f : fréquences des cases (Hz, croissantes, pas régulier), p_dbm : niveau par case.
    Renvoie (canaux, plancher en dBm/case, décalage de grille retenu).
    """
    if f.size < 16:
        return [], None, grid_offset
    df = f[1] - f[0]
    lin = 10 ** (p_dbm / 10.0)
    ps = db(smooth(lin, max(1, int(150e3 / df))))
    floor = float(np.percentile(ps, 5))
    thr = floor + threshold_db
    half = grid / 2

    def cells_for(off):
        first = np.ceil((f[0] + half - off) / grid) * grid + off
        return np.arange(first, f[-1] - half + 1, grid)

    if grid_offset is None:
        best, best_off = -np.inf, 0.0
        for off in np.arange(0, grid, 125e3):
            score = 0.0
            for c in cells_for(off):
                mid = _band_mean_db(f, lin, c - 0.375 * grid, c + 0.375 * grid)
                if mid is None or mid < thr:
                    continue
                el = _band_mean_db(f, lin, c - half - 150e3, c - half + 150e3)
                er = _band_mean_db(f, lin, c + half - 150e3, c + half + 150e3)
                edges = [e for e in (el, er) if e is not None]
                if edges:
                    score += mid - max(edges)
            if score > best:
                best, best_off = score, off
        grid_offset = best_off

    cells = []
    for c in cells_for(grid_offset):
        mid = _band_mean_db(f, lin, c - 0.375 * grid, c + 0.375 * grid)
        if mid is None or mid < thr:
            continue
        right = _band_mean_db(f, lin, c + half - 150e3, c + half + 150e3)
        cells.append({"c": float(c), "mid": mid, "edge_right": right})

    # canaux contigus sans creux à la frontière : un seul bloc large (OFDM DOCSIS 3.1…)
    groups = []
    for cell in cells:
        g = groups[-1] if groups else None
        if (g and abs(cell["c"] - g[-1]["c"] - grid) < 1 and g[-1]["edge_right"] is not None
                and min(g[-1]["mid"], cell["mid"]) - g[-1]["edge_right"] < merge_dip_db):
            g.append(cell)
        else:
            groups.append([cell])

    chans = []
    for g in groups:
        if len(g) >= 3:
            a, b = g[0]["c"] - half, g[-1]["c"] + half
            chans.append({"freq": (a + b) / 2, "width": b - a, "kind": "wide",
                          "level_dbm": _band_sum_dbm(f, lin, a, b),
                          "margin_db": round(float(np.mean([x["mid"] for x in g]) - floor), 1)})
        else:
            for x in g:
                c = x["c"]
                chans.append({"freq": c, "width": grid, "kind": "cell",
                              "level_dbm": _band_sum_dbm(f, lin, c - half, c + half),
                              "margin_db": round(x["mid"] - floor, 1)})
    for ch in chans:
        if ch["level_dbm"] is not None:
            ch["level_dbm"] = round(ch["level_dbm"], 1)
    return chans, floor, grid_offset


# --- satellite -------------------------------------------------------------------------

def sliding_floor(f, ps, chunk=20e6, span=3):
    """Plancher de bruit lentement variable : 10e centile par tranche, minimum glissant."""
    df = f[1] - f[0]
    n = max(8, int(chunk / df))
    m = int(np.ceil(ps.size / n))
    q = np.array([np.percentile(ps[i * n:(i + 1) * n], 10) for i in range(m)])
    qm = np.array([q[max(0, i - span):i + span + 1].min() for i in range(m)])
    centers = np.array([f[min(ps.size - 1, i * n + n // 2)] for i in range(m)])
    return np.interp(f, centers, qm)


def detect_sat(f, p_dbm, threshold_db=4.0, min_rs=1e6):
    """Porteuses satellite : régions au-dessus du plancher, largeur à -3 dB -> débit symbole."""
    if f.size < 16:
        return [], None
    df = f[1] - f[0]
    lin = 10 ** (p_dbm / 10.0)
    ps = db(smooth(lin, max(1, int(250e3 / df))))
    floor = sliding_floor(f, ps)
    above = ps > floor + threshold_db
    chans = []
    edges = np.flatnonzero(np.diff(np.concatenate(([0], above.astype(np.int8), [0]))))
    for a, b in zip(edges[0::2], edges[1::2]):
        if (b - a) * df < 0.6 * min_rs:
            continue
        seg = ps[a:b]
        level = float(np.percentile(seg, 80))
        half = seg > level - 3.0
        # morceaux au-dessus de -3 dB, réunis si le trou est petit (ondulation)
        runs = []
        e2 = np.flatnonzero(np.diff(np.concatenate(([0], half.astype(np.int8), [0]))))
        for x, y in zip(e2[0::2], e2[1::2]):
            if runs and (x - runs[-1][1]) * df < max(0.3e6, 0.05 * (b - a) * df):
                runs[-1][1] = y
            else:
                runs.append([x, y])
        for x, y in runs:
            width = (y - x) * df
            if width < 0.8 * min_rs:
                continue
            lo, hi = f[a + x], f[a + y - 1] + df
            c = (lo + hi) / 2
            fl = float(np.mean(floor[a + x:a + y]))
            margin = float(np.mean(ps[a + x:a + y])) - fl
            # bribes étroites : queue de roll-off d'une voisine, bord de bande, ondulation
            if width < 3e6 and (margin < threshold_db + 3 or a + x < 4 or a + y > f.size - 4):
                continue
            chans.append({"freq": float(c), "width": float(width), "kind": "carrier",
                          "symbol_rate_est": float(round(width / 1e4) * 1e4),
                          "level_dbm": round(float(db(np.sum(lin[a + x:a + y]))), 1),
                          "margin_db": round(margin, 1)})
    return chans, float(np.median(floor))


# --- raie cyclique de |x|² -------------------------------------------------------------

def cyclic_line(iq, fs, band, rs_min, rs_max, threshold_db=15.0):
    """Cherche la raie de |x|² à la fréquence symbole (porteuse unique filtrée en racine de
    cosinus surélevé). Le signal est d'abord filtré à ±band/2 autour du centre.

    Renvoie {"found", "rs", "contrast_db"}. Les fréquences au-delà de fs/2 sont repliées :
    on choisit le candidat (f, fs-f, fs+f) qui tombe dans [rs_min, rs_max].
    """
    x = np.asarray(iq, dtype=np.complex64)
    n = x.size
    if n < 4096:
        return {"found": False, "rs": None, "contrast_db": 0.0}
    X = np.fft.fft(x)
    fr = np.fft.fftfreq(n, 1.0 / fs)
    X[np.abs(fr) > band / 2] = 0
    y = np.fft.ifft(X)
    p = np.abs(y) ** 2
    p = p - p.mean()
    P = np.abs(np.fft.rfft(p * np.hanning(n))) ** 2
    fo = np.fft.rfftfreq(n, 1.0 / fs)
    cand = np.stack([fo, fs - fo, fs + fo])
    ok = (cand >= rs_min) & (cand <= rs_max)
    valid = ok.any(axis=0) & (fo > 50e3)
    if not valid.any():
        return {"found": False, "rs": None, "contrast_db": 0.0}
    # référence locale : le spectre de |x|² n'est pas plat. Médiane par blocs de 512 cases
    # (insensible à la raie), ramenée à la moyenne d'une loi exponentielle (/ ln 2)
    nb = max(1, P.size // 512)
    blk = P[: nb * 512].reshape(nb, -1) if P.size >= 512 else P[None, :]
    med = np.median(blk, axis=1) / np.log(2)
    centers = (np.arange(med.size) + 0.5) * (P.size / med.size)
    base = np.interp(np.arange(P.size), centers, med) + 1e-30
    ratio = P / base
    idx = np.flatnonzero(valid)
    k = idx[np.argmax(ratio[idx])]
    contrast = float(10 * np.log10(ratio[k]))
    # interpolation parabolique (en log) pour la fréquence fine
    if 0 < k < P.size - 1:
        a, b, c = np.log(P[k - 1] + 1e-30), np.log(P[k] + 1e-30), np.log(P[k + 1] + 1e-30)
        d = a - 2 * b + c
        delta = 0.5 * (a - c) / d if d != 0 else 0.0
    else:
        delta = 0.0
    fobs = (k + float(np.clip(delta, -0.5, 0.5))) * fs / n
    choices = [v for v in (fobs, fs - fobs, fs + fobs) if rs_min <= v <= rs_max]
    rs = choices[0] if choices else fobs
    return {"found": contrast >= threshold_db, "rs": float(rs), "contrast_db": round(contrast, 1)}


def snap_symbol_rate(rs, tol=0.002):
    """Arrondit à un débit symbole courant (±0,2 %), sinon au kBd."""
    if not rs:
        return 0
    best = min(COMMON_SYMBOL_RATES, key=lambda v: abs(v - rs))
    if abs(best - rs) <= tol * best:
        return int(best)
    return int(round(rs / 1000.0) * 1000)
