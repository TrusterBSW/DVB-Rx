/* dvbrx-web : page de supervision (JavaScript sans dépendance). */
'use strict';

const $ = (s, r = document) => r.querySelector(s);
const $$ = (s, r = document) => Array.from(r.querySelectorAll(s));

const S = {
  cfg: null,
  sock: 0,
  tab: 'status',
  status: null,
  job: null,
  results: { quick: null, full: null },
  jobReq: null,          // dernière demande de scan faite depuis cette page
  busyAsked: false,
  mon: null,             // alignement en cours {params, force, started}
  hist: [],
  monLostAt: 0,
};

/* ------------------------------------------------------------------ utilitaires */

const NF = {};
function num(v, d = 1) {
  if (v === null || v === undefined || Number.isNaN(v)) return '—';
  if (!NF[d]) NF[d] = new Intl.NumberFormat('fr-FR', { minimumFractionDigits: d, maximumFractionDigits: d });
  return NF[d].format(v);
}
const isNum = (v) => typeof v === 'number' && Number.isFinite(v);
const mhz = (hz, d = 3) => (isNum(hz) ? `${num(hz / 1e6, d)} MHz` : '—');
const ghz = (hz, d = 4) => (isNum(hz) ? `${num(hz / 1e9, d)} GHz` : '—');
const kbd = (rs) => (isNum(rs) && rs > 0 ? `${num(rs / 1e3, 0)} kBd` : '—');
function esc(s) {
  return String(s ?? '').replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
}
function dur(s) {
  if (!isNum(s)) return '—';
  s = Math.floor(s);
  const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), x = s % 60;
  return (h ? `${h} h ` : '') + `${String(m).padStart(h ? 2 : 1, '0')} min ${String(x).padStart(2, '0')} s`;
}
function when(t) {
  return t ? new Date(t * 1000).toLocaleString('fr-FR') : '—';
}
let toastTimer = null;
function toast(msg, bad = false) {
  const t = $('#toast');
  t.textContent = msg;
  t.className = 'show' + (bad ? ' bad' : '');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { t.className = ''; }, bad ? 7000 : 3500);
}

class ApiError extends Error {
  constructor(msg, status, data) { super(msg); this.status = status; this.data = data || {}; }
  get busy() { return this.data.error === 'busy'; }
}

async function api(path, body) {
  const opts = body === undefined ? {} : {
    method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body),
  };
  let r;
  try {
    r = await fetch(path, opts);
  } catch (e) {
    throw new ApiError('dvbrx-web injoignable', 0, { unreachable: true, web: true });
  }
  let data;
  try { data = await r.json(); } catch (e) { data = { ok: false, error: `réponse HTTP ${r.status} illisible` }; }
  if (!r.ok || data.ok === false) throw new ApiError(data.message || data.error || `HTTP ${r.status}`, r.status, data);
  return data;
}
const q = (path, extra = {}) => path + '?' + new URLSearchParams({ s: S.sock, ...extra });

/* boucles de rafraîchissement (setTimeout chaîné, arrêt propre) */
const loops = {};
function startLoop(name, fn, ms) {
  if (loops[name]) return;
  const L = { stop: false, timer: null };
  loops[name] = L;
  const tick = async () => {
    if (L.stop) return;
    try { await fn(); } catch (e) { /* affiché par fn */ }
    if (!L.stop) L.timer = setTimeout(tick, typeof ms === 'function' ? ms() : ms);
  };
  tick();
}
function stopLoop(name) {
  const L = loops[name];
  if (!L) return;
  L.stop = true;
  clearTimeout(L.timer);
  delete loops[name];
}

function setConn(state, text) {
  const c = $('#conn');
  c.firstElementChild.className = 'dot ' + state;
  c.lastElementChild.textContent = text;
}

/* boîte « récepteur occupé » : résout true si l'utilisateur force */
function askForce(what) {
  const dlg = $('#busyDlg');
  $('h3', dlg).textContent = what ? `Le récepteur est occupé (${what})` : 'Le récepteur est occupé';
  return new Promise((resolve) => {
    const done = (v) => {
      $('#busyForce').onclick = $('#busyCancel').onclick = null;
      dlg.onclose = null;
      if (dlg.open) dlg.close();
      resolve(v);
    };
    $('#busyForce').onclick = () => done(true);
    $('#busyCancel').onclick = () => done(false);
    dlg.onclose = () => done(false);
    dlg.showModal();
  });
}

/* ------------------------------------------------------------------ onglets */

const TAB_HOOKS = {};
function visible() { return document.visibilityState === 'visible'; }

function showTab(name) {
  if (name === S.tab) return;
  const old = S.tab;
  S.tab = name;
  $$('#tabs button').forEach((b) => b.classList.toggle('active', b.dataset.tab === name));
  $$('section.tab').forEach((s) => s.classList.toggle('active', s.id === 'tab-' + name));
  if (TAB_HOOKS[old] && TAB_HOOKS[old].leave) TAB_HOOKS[old].leave();
  if (TAB_HOOKS[name] && TAB_HOOKS[name].enter && visible()) TAB_HOOKS[name].enter();
  try { history.replaceState(null, '', '#' + name); } catch (e) { /* rien */ }
}

document.addEventListener('visibilitychange', () => {
  const h = TAB_HOOKS[S.tab];
  if (!h) return;
  if (visible()) { if (h.enter) h.enter(); } else if (h.hidden) h.hidden(); else if (h.leave) h.leave();
});

/* ------------------------------------------------------------------ libellés */

const STATE_FR = { idle: 'inactif', tuned: 'accordé', probe: 'sondage', monitor: 'alignement' };
const AGC_FR = {
  level: 'niveau (vise −24 dBFS)', search: 'recherche du meilleur SNR', best: 'meilleur SNR trouvé', manual: 'manuel',
};
const SVC_TYPE = {
  1: 'TV SD', 2: 'radio', 3: 'télétexte', 0x0a: 'radio AAC', 0x0c: 'données', 0x11: 'TV HD MPEG-2',
  0x16: 'TV SD H.264', 0x19: 'TV HD H.264', 0x1c: 'TV 3D', 0x1f: 'TV HEVC', 0x20: 'TV UHD HEVC',
};
const DS_COLOR = {
  'DVB-C': '#4ea1ff', 'DVB-T': '#3ccf7a', 'DVB-T2': '#2ec4b6', 'DVB-S': '#f2b134', 'DVB-S2': '#c38bff',
  DOCSIS: '#7d8794', wide: '#5b6470', none: '#ff5d5d',
};

function lnbText(lnb, radio) {
  if (!lnb) return '—';
  if (!lnb.present) return 'pas de contrôleur (dvbrx sans -L)';
  const v = String(lnb.volt);
  if (v === 'off') return 'non alimenté';
  const pol = v === '13' ? 'V' : v === '18' ? 'H' : '?';
  const band = lnb.tone ? 'bande haute' : 'bande basse';
  let s = `${v} V${lnb.tone ? ' + 22 kHz' : ', sans 22 kHz'} → ${pol}, ${band}`;
  if (radio && isNum(radio.lnb_lo)) s += ` (OL ${num(radio.lnb_lo / 1e6, 0)} MHz)`;
  return s;
}

function chanFreqText(ch) {
  const ds = String(ch.delsys || '');
  if (ds.startsWith('DVB-S')) {
    if (isNum(ch.rf_freq)) return `${mhz(ch.rf_freq)} ${ch.polarization || ''} (FI ${mhz(ch.if_freq ?? ch.freq)})`;
    return `FI ${mhz(ch.freq)} (oscillateur inconnu)`;
  }
  return mhz(ch.freq);
}

function lockBadge(locked, state) {
  if (state === 'idle') return '<span class="badge">—</span>';
  return locked ? '<span class="badge ok">verrouillé</span>' : '<span class="badge bad">non verrouillé</span>';
}

function ratio(err, tot) {
  if (!isNum(err)) return '—';
  let s = num(err, 0);
  if (isNum(tot) && tot > 0) s += ` / ${num(tot, 0)} (${(err / tot).toExponential(1)})`;
  return s;
}

/* ------------------------------------------------------------------ Statut */

function kv(rows) {
  return rows.filter((r) => r).map(([k, v]) => `<dt>${esc(k)}</dt><dd>${v}</dd>`).join('');
}

/* « Salon — B210 31A5B7C » : le nom du socket et la carte qui le sert */
function sockLabel(name, c) {
  const card = c ? [c.model, c.serial].filter(Boolean).join(' ') : '';
  return card ? `${name} — ${card}` : name;
}

function renderStatus(st) {
  const r = st.radio || {};
  const offNote = S.cfg && S.cfg.input_offset_db !== null && S.cfg.input_offset_db !== undefined
    ? ` (offset web ${num(S.cfg.input_offset_db, 1)} dB)` : '';
  $('#radioKv').innerHTML = kv([
    ['Source', esc(r.source ?? '—')],
    ['Carte', esc(r.model || '—')],
    ['N° de série', r.serial ? `<code>${esc(r.serial)}</code>` : '—'],
    ['Fréquence', mhz(r.freq)],
    ['Filtre analogique', isNum(r.bandwidth) && r.bandwidth > 0 ? mhz(r.bandwidth, 2) : 'ouvert / —'],
    ['Échantillonnage', isNum(r.rate) ? `${num(r.rate / 1e6, 3)} MS/s` : '—'],
    ['Gain', isNum(r.gain) ? `${num(r.gain, 1)} dB` : '—'],
    ['AGC', esc(AGC_FR[r.agc] || r.agc || '—')],
    ['Niveau', isNum(r.level_dbfs) ? `${num(r.level_dbfs, 1)} dBFS` : '—'],
    ['Puissance d\'entrée', isNum(r.input_dbm)
      ? `${num(r.input_dbm, 1)} dBm<span class="est">estimation, avant ampli${esc(offNote)}</span>` : '—'],
    ['Débordements', isNum(r.overflows) ? num(r.overflows, 0) : '—'],
    ['Échantillons comblés', isNum(r.filled) ? num(r.filled, 0) : '—'],
    ['LNB', esc(lnbText(r.lnb, r))],
  ]);
  $('#radioUpd').textContent = 'mis à jour ' + new Date().toLocaleTimeString('fr-FR');
  const opt = $('#sock').selectedOptions[0];
  if (opt && (r.model || r.serial)) opt.textContent = sockLabel(opt.dataset.name, r);

  const cards = (st.channels || []).map((ch) => {
    const ds = String(ch.delsys || '');
    const isT = ds === 'DVB-T' || ds === 'DVB-T2';
    const ofdm = [ch.fft ? `${ch.fft >= 1024 ? ch.fft / 1024 + 'K' : ch.fft}` : null, ch.guard, ds === 'DVB-T2' && isNum(ch.plp) ? `PLP ${ch.plp}` : null]
      .filter((x) => x).join(' · ');
    const rows = [
      ['État', `<span class="badge ${ch.state === 'idle' ? '' : 'info'}">${esc(STATE_FR[ch.state] || ch.state || '—')}</span>`],
      ['Norme', esc(ds || '—')],
      ['Fréquence', chanFreqText(ch)],
      isT ? ['Largeur de canal', isNum(ch.bandwidth) ? mhz(ch.bandwidth, 0) : '—']
        : ['Débit symbole', kbd(ch.symbol_rate)],
      !isT && isNum(ch.rolloff) && ch.rolloff > 0 ? ['Roll-off', num(ch.rolloff, 2)] : null,
      ['Modulation', esc(ch.modulation || '—')],
      ['Rendement', esc(ch.code_rate || '—')],
      ch.pilots !== null && ch.pilots !== undefined ? ['Pilotes', ch.pilots ? 'oui' : 'non'] : null,
      isT ? ['FFT / intervalle de garde / PLP', esc(ofdm || '—')] : null,
      ['Verrouillage', lockBadge(ch.locked, ch.state)],
      ['SNR', isNum(ch.snr_db) ? `${num(ch.snr_db, 1)} dB` : '—'],
      ['Décalage de fréquence', isNum(ch.freq_offset_hz) ? `${num(ch.freq_offset_hz / 1e3, 1)} kHz` : '—'],
      ['Débit TS récupéré', isNum(ch.ts_mbps) ? `<b>${num(ch.ts_mbps, 2)} Mbit/s</b>` : '—'],
      ['Erreurs TS', ratio(ch.ts_errors, ch.ts_packets)],
      ['Erreurs FEC', ratio(ch.fec_errors, ch.fec_blocks)],
      isNum(ch.level_dbfs) ? ['Niveau du canal', `${num(ch.level_dbfs, 1)} dBFS`] : null,
      ['Durée', dur(ch.uptime)],
    ];
    return `<div class="card"><div class="card-head"><h2>Adaptateur ${esc(ch.adapter)}</h2>
      <span class="muted">/dev/dvb/adapter${esc(ch.adapter)}</span></div><dl class="kv">${kv(rows)}</dl></div>`;
  });
  $('#chanCards').innerHTML = cards.join('') || '<p class="muted">Aucun canal.</p>';

  const ban = [];
  if (st.web_scan) ban.push(`<div class="banner warn">Scan ${st.web_scan.mode === 'full' ? 'complet' : 'rapide'} en cours : ${esc(st.web_scan.message)}</div>`);
  if (st.web_monitor) ban.push(`<div class="banner warn">Alignement ${['DVB-T', 'DVB-T2'].includes(st.web_monitor.delsys) ? 'terrestre' : 'satellite'} en cours (le récepteur est réservé).</div>`);
  $('#statusBanner').innerHTML = ban.join('');
}

async function pollStatus() {
  try {
    const st = await api(q('/api/status'));
    S.status = st;
    setConn('ok', 'dvbrx connecté');
    renderStatus(st);
    return st;
  } catch (e) {
    setConn('bad', e.message);
    $('#statusBanner').innerHTML = `<div class="banner bad">${esc(e.message)}</div>`;
    throw e;
  }
}

TAB_HOOKS.status = {
  enter() { startLoop('status', pollStatus, 1000); },
  leave() { stopLoop('status'); },
};

/* ------------------------------------------------------------------ Constellation */

const CONST = { acc: null, size: 0, range: 1.8 };

function constReset() {
  const c = $('#constCanvas');
  CONST.size = c.width;
  CONST.acc = new Float32Array(c.width * c.height);
  constDraw();
}

function constAccumulate(points) {
  const c = $('#constCanvas');
  const n = c.width;
  if (!CONST.acc || CONST.size !== n) constReset();
  const acc = CONST.acc;
  const decay = parseFloat($('#constPersist').value);
  for (let i = 0; i < acc.length; i++) acc[i] *= decay;
  const k = n / (2 * CONST.range);
  for (const p of points) {
    const x = Math.round((p[0] + CONST.range) * k);
    const y = Math.round((CONST.range - p[1]) * k);
    if (x < 1 || y < 1 || x >= n - 1 || y >= n - 1) continue;
    const o = y * n + x;
    acc[o] += 1;
    acc[o - 1] += 0.35; acc[o + 1] += 0.35; acc[o - n] += 0.35; acc[o + n] += 0.35;
  }
  constDraw();
}

function heat(v) { // 0..1 -> RGB (noir, bleu, cyan, vert, jaune, blanc)
  const stops = [[0, 0, 0], [20, 40, 140], [30, 150, 220], [60, 220, 120], [250, 230, 60], [255, 255, 255]];
  const x = Math.min(0.9999, Math.max(0, v)) * (stops.length - 1);
  const i = Math.floor(x), f = x - i;
  const a = stops[i], b = stops[i + 1];
  return [a[0] + (b[0] - a[0]) * f, a[1] + (b[1] - a[1]) * f, a[2] + (b[2] - a[2]) * f];
}

function constDraw() {
  const c = $('#constCanvas');
  const ctx = c.getContext('2d');
  const n = c.width;
  const img = ctx.createImageData(n, n);
  const acc = CONST.acc;
  let mx = 1e-9;
  if (acc) for (let i = 0; i < acc.length; i++) if (acc[i] > mx) mx = acc[i];
  const lm = Math.log1p(mx);
  const d = img.data;
  for (let i = 0; i < n * n; i++) {
    const v = acc ? acc[i] : 0;
    let r = 11, g = 14, b = 18;
    if (v > 0.02) [r, g, b] = heat(0.12 + 0.88 * Math.log1p(v) / lm);
    d[4 * i] = r; d[4 * i + 1] = g; d[4 * i + 2] = b; d[4 * i + 3] = 255;
  }
  ctx.putImageData(img, 0, 0);
  ctx.strokeStyle = '#ffffff22';
  ctx.lineWidth = 1;
  ctx.beginPath();
  ctx.moveTo(n / 2 + 0.5, 0); ctx.lineTo(n / 2 + 0.5, n);
  ctx.moveTo(0, n / 2 + 0.5); ctx.lineTo(n, n / 2 + 0.5);
  ctx.stroke();
  ctx.beginPath();
  ctx.arc(n / 2, n / 2, n / (2 * CONST.range), 0, 2 * Math.PI);
  ctx.stroke();
}

function fillAdapters(sel, st, preferLocked) {
  const chans = (st && st.channels) || [];
  const cur = sel.value;
  sel.innerHTML = chans.map((c) => `<option value="${esc(c.adapter)}">${esc(c.adapter)} : ${esc(c.delsys || STATE_FR[c.state] || '')} ${isNum(c.freq) ? esc(mhz(c.rf_freq ?? c.freq, 1)) : ''}</option>`).join('');
  if (chans.some((c) => String(c.adapter) === cur)) sel.value = cur;
  else if (preferLocked) {
    const l = chans.find((c) => c.locked);
    if (l) sel.value = String(l.adapter);
  }
}

async function constStatusTick() {
  const st = await pollStatusQuiet();
  if (!st) return;
  fillAdapters($('#constAdapter'), st, true);
  const ch = (st.channels || []).find((c) => String(c.adapter) === $('#constAdapter').value);
  $('#constSnr').textContent = ch && isNum(ch.snr_db) ? `${num(ch.snr_db, 1)} dB` : '—';
  $('#constLock').innerHTML = ch ? lockBadge(ch.locked, ch.state) : '—';
}

async function pollStatusQuiet() {
  try {
    const st = await api(q('/api/status'));
    S.status = st;
    setConn('ok', 'dvbrx connecté');
    return st;
  } catch (e) {
    setConn('bad', e.message);
    return null;
  }
}

async function constTick() {
  const ad = $('#constAdapter').value;
  if (ad === '') return;
  try {
    const r = await api(q('/api/constellation', { adapter: ad, points: $('#constPoints').value }));
    $('#constMod').textContent = r.modulation || '—';
    const pts = r.points || [];
    $('#constInfo').textContent = pts.length ? `${pts.length} points reçus` : 'pas de symboles (canal non verrouillé ou inactif)';
    constAccumulate(pts);
  } catch (e) {
    $('#constInfo').textContent = e.message;
  }
}

TAB_HOOKS.const = {
  async enter() {
    if (!CONST.acc) constReset();
    startLoop('constStatus', constStatusTick, 2000);
    startLoop('const', constTick, 500);
  },
  leave() { stopLoop('const'); stopLoop('constStatus'); },
};

/* ------------------------------------------------------------------ dessin de spectre */

function setupCanvas(c) {
  const dpr = window.devicePixelRatio || 1;
  const w = Math.max(200, c.clientWidth), h = Math.max(50, c.clientHeight);
  if (c.width !== Math.round(w * dpr) || c.height !== Math.round(h * dpr)) {
    c.width = Math.round(w * dpr);
    c.height = Math.round(h * dpr);
  }
  const ctx = c.getContext('2d');
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  return { ctx, w, h };
}

function niceStep(range, target) {
  const raw = range / target;
  const p = Math.pow(10, Math.floor(Math.log10(raw)));
  for (const m of [1, 2, 2.5, 5, 10]) if (raw <= m * p) return m * p;
  return 10 * p;
}

function chanColor(ch) {
  if (ch.docsis) return DS_COLOR.DOCSIS;
  if (ch.kind === 'wide' || ch.class === 'wide') return DS_COLOR.wide;
  if (ch.skip) return DS_COLOR.wide;
  if (ch.delsys && DS_COLOR[ch.delsys]) return DS_COLOR[ch.delsys];
  return DS_COLOR.none;
}

/* line : {spectrum:{f0,df,db}, lo, kind, start, stop, channels}; mode : 'quick' | 'full' */
function drawSpectrum(c, line, mode) {
  const { ctx, w, h } = setupCanvas(c);
  ctx.clearRect(0, 0, w, h);
  const sp = line.spectrum;
  const lo = line.kind === 'sat' ? line.lo || 0 : 0;
  const ml = 46, mr = 8, mt = 8, mb = 20;
  const fa = (sp ? sp.f0 : line.start) + lo, fb = (sp ? sp.f0 + sp.df * (sp.db.length - 1) : line.stop) + lo;
  const fmin = Math.min(fa, line.start + lo), fmax = Math.max(fb, line.stop + lo);
  const X = (f) => ml + ((f - fmin) / (fmax - fmin || 1)) * (w - ml - mr);
  let ymin = -120, ymax = -40;
  if (sp && sp.db.length) {
    const sorted = sp.db.slice().sort((a, b) => a - b);
    ymin = Math.floor((sorted[Math.floor(sorted.length * 0.01)] - 5) / 5) * 5;
    ymax = Math.ceil((sorted[sorted.length - 1] + 4) / 5) * 5;
    if (ymax - ymin < 20) ymax = ymin + 20;
  }
  const Y = (v) => mt + (1 - (v - ymin) / (ymax - ymin)) * (h - mt - mb);
  c._map = { X, fmin, fmax, ml, mr, w, lo, line, mode, Y };

  // grille
  ctx.font = '11px system-ui, sans-serif';
  ctx.fillStyle = '#8d97a6';
  ctx.strokeStyle = '#ffffff12';
  ctx.lineWidth = 1;
  const ys = niceStep(ymax - ymin, 4);
  for (let v = Math.ceil(ymin / ys) * ys; v <= ymax; v += ys) {
    ctx.beginPath(); ctx.moveTo(ml, Y(v) + 0.5); ctx.lineTo(w - mr, Y(v) + 0.5); ctx.stroke();
    ctx.fillText(String(v), 4, Y(v) + 4);
  }
  const unit = fmax > 3e9 ? 1e9 : 1e6;
  const xs = niceStep((fmax - fmin) / unit, Math.max(4, Math.floor(w / 80))) * unit;
  for (let f = Math.ceil(fmin / xs) * xs; f <= fmax; f += xs) {
    ctx.beginPath(); ctx.moveTo(X(f) + 0.5, mt); ctx.lineTo(X(f) + 0.5, h - mb); ctx.stroke();
    const lab = unit === 1e9 ? num(f / 1e9, xs < 1e8 ? 2 : 1) : num(f / 1e6, 0);
    ctx.fillText(lab, X(f) - ctx.measureText(lab).width / 2, h - 5);
  }
  ctx.fillText(unit === 1e9 ? 'GHz' : 'MHz', w - mr - 24, h - 5);

  // canaux
  for (const ch of line.channels || []) {
    const f0 = (ch.freq + lo) - ch.width / 2, f1 = (ch.freq + lo) + ch.width / 2;
    const x0 = X(f0), x1 = Math.max(X(f1), x0 + 2);
    if (mode === 'quick') {
      ctx.fillStyle = ch.kind === 'wide' ? '#7d879433' : '#4ea1ff2e';
      ctx.fillRect(x0, mt, x1 - x0, h - mt - mb);
    } else {
      const col = chanColor(ch);
      const solid = ch.locked || ch.docsis;
      ctx.fillStyle = col + (solid ? '55' : '22');
      ctx.fillRect(x0, mt, x1 - x0, h - mt - mb);
      const neutral = ch.kind === 'wide' || ch.class === 'wide' || ch.skip;
      ctx.strokeStyle = solid || neutral ? col : DS_COLOR.none;
      ctx.setLineDash(solid ? [] : [4, 3]);
      ctx.strokeRect(x0 + 0.5, mt + 0.5, x1 - x0 - 1, h - mt - mb - 1);
      ctx.setLineDash([]);
    }
  }

  // trace
  if (sp && sp.db.length) {
    ctx.strokeStyle = '#e3e7ee';
    ctx.lineWidth = 1;
    ctx.beginPath();
    sp.db.forEach((v, i) => {
      const x = X(sp.f0 + i * sp.df + lo), y = Y(v);
      if (i) ctx.lineTo(x, y); else ctx.moveTo(x, y);
    });
    ctx.stroke();
  } else {
    ctx.fillStyle = '#8d97a6';
    ctx.font = '14px system-ui, sans-serif';
    ctx.fillText(line.error || 'pas de spectre', ml + 10, h / 2);
  }
  ctx.fillStyle = '#8d97a6';
  ctx.font = '10px system-ui, sans-serif';
  ctx.fillText('dBm/case (est.)', ml + 4, mt + 10);
}

function drawSnrBars(c, line) {
  const { ctx, w, h } = setupCanvas(c);
  ctx.clearRect(0, 0, w, h);
  const spec = c.previousElementSibling;
  const m = spec && spec._map;
  if (!m) return;
  const lo = m.lo;
  const smax = 40;
  ctx.font = '10px system-ui, sans-serif';
  ctx.fillStyle = '#8d97a6';
  ctx.fillText('SNR', 4, 12);
  ctx.fillText(`${smax} dB`, 4, 24);
  for (const ch of line.channels || []) {
    if (!isNum(ch.snr_db)) continue;
    const x0 = m.X(ch.freq + lo - ch.width / 2), x1 = Math.max(m.X(ch.freq + lo + ch.width / 2), x0 + 3);
    const bh = Math.max(2, Math.min(1, ch.snr_db / smax) * (h - 14));
    ctx.fillStyle = chanColor(ch) + (ch.locked ? 'dd' : '55');
    ctx.fillRect(x0 + 1, h - bh, x1 - x0 - 2, bh);
    if (x1 - x0 > 22) {
      ctx.fillStyle = '#e3e7ee';
      const t = num(ch.snr_db, 0);
      ctx.fillText(t, (x0 + x1) / 2 - ctx.measureText(t).width / 2, h - bh - 2);
    }
  }
}

function hoverSetup(c, readout) {
  c.addEventListener('mousemove', (ev) => {
    const m = c._map;
    if (!m) return;
    const rect = c.getBoundingClientRect();
    const x = ev.clientX - rect.left;
    const f = m.fmin + ((x - m.ml) / (m.w - m.ml - m.mr)) * (m.fmax - m.fmin);
    const sp = m.line.spectrum;
    let lvl = '';
    if (sp && sp.db.length) {
      const i = Math.round((f - m.lo - sp.f0) / sp.df);
      if (i >= 0 && i < sp.db.length) lvl = ` · ${num(sp.db[i], 1)} dBm/case`;
    }
    const ch = (m.line.channels || []).find((k) => Math.abs(f - (k.freq + m.lo)) <= k.width / 2);
    let t = (m.lo ? `${mhz(f)} (FI ${mhz(f - m.lo)})` : mhz(f)) + lvl;
    if (ch) t += ' · ' + chanSummary(ch, m.lo);
    readout.textContent = t;
  });
  c.addEventListener('mouseleave', () => { readout.textContent = ''; });
}

function chanSummary(ch) {
  const parts = [];
  if (ch.docsis) parts.push('DOCSIS');
  else if (ch.delsys) parts.push(ch.delsys);
  if (ch.modulation) parts.push(ch.modulation);
  if (isNum(ch.symbol_rate) && ch.symbol_rate) parts.push(kbd(ch.symbol_rate));
  else if (isNum(ch.symbol_rate_est)) parts.push(`≈ ${kbd(ch.symbol_rate_est)}`);
  if (isNum(ch.snr_db)) parts.push(`SNR ${num(ch.snr_db, 1)} dB`);
  if (ch.services && ch.services.length) parts.push(`${ch.services.length} services`);
  if (ch.status) parts.push(ch.status);
  return parts.join(', ');
}

/* ------------------------------------------------------------------ scans : progression */

function renderJob(job) {
  for (const box of $$('[data-jobbox]')) {
    if (!job) { box.classList.remove('show'); continue; }
    box.classList.add('show');
    const running = job.state === 'running';
    const stateFr = { running: 'en cours', done: 'terminé', cancelled: 'annulé', busy: 'récepteur occupé', error: 'erreur' }[job.state] || job.state;
    const cls = { done: 'ok', running: 'info', busy: 'warn', error: 'bad', cancelled: '' }[job.state] || '';
    box.innerHTML = `<div class="card-head"><h3>Scan ${job.mode === 'full' ? 'complet' : 'rapide'}${job.params && job.params.sat ? ' (Sat ON)' : ''}</h3>
      <span class="badge ${cls}">${esc(stateFr)}</span><span class="spacer"></span>
      ${running ? '<button class="btn" data-cancel>Annuler</button>' : ''}</div>
      <div class="progress"><div style="width:${(job.progress * 100).toFixed(1)}%"></div></div>
      <div class="jobmsg">${esc(job.message)} ${running ? `<span class="muted">(${Math.round(job.progress * 100)} %)</span>` : ''}</div>
      ${job.state === 'busy' ? '<div class="banner warn">tvheadend utilise le récepteur : relancez en forçant si vous acceptez d\'interrompre la chaîne en cours.</div>' : ''}
      ${job.state === 'error' ? `<div class="banner bad">${esc(job.error)}</div>` : ''}
      <details class="log"><summary class="muted">journal</summary><pre>${esc((job.log || []).join('\n'))}</pre></details>`;
    const cb = $('[data-cancel]', box);
    if (cb) cb.onclick = () => api('/api/scan/cancel', { s: S.sock }).catch((e) => toast(e.message, true));
  }
  const running = job && job.state === 'running';
  $('#quickGo').disabled = running;
  $('#fullGo').disabled = running;
}

async function jobTick() {
  let r;
  try { r = await api(q('/api/scan/progress')); } catch (e) { return; }
  const job = r.job;
  const prev = S.job;
  S.job = job;
  renderJob(job);
  if (job && job.state === 'running') startLoop('job', jobTick, 800);
  if (prev && prev.state === 'running' && job && job.state !== 'running') {
    await loadResults();
    if (job.state === 'busy' && S.jobReq && !S.busyAsked) {
      S.busyAsked = true;
      if (await askForce('scan')) startScan(S.jobReq.mode, true);
    } else if (job.state === 'done') {
      toast(`Scan ${job.mode === 'full' ? 'complet' : 'rapide'} terminé`);
    }
  }
  if (!job || job.state !== 'running') {
    if (S.tab !== 'quick' && S.tab !== 'full') stopLoop('job');
  }
}

async function startScan(mode, force = false) {
  const sat = mode === 'full' ? $('#fullSat').checked : $('#quickSat').checked;
  const body = { s: S.sock, mode, sat, force, rescan: mode === 'full' && $('#fullRescan').checked };
  if (mode === 'quick') {
    const a = parseFloat($('#cableStart').value), b = parseFloat($('#cableStop').value);
    if (!(a > 0 && b > a + 8)) { toast('Plage câble invalide', true); return; }
    body.cable_start = a * 1e6;
    body.cable_stop = b * 1e6;
  }
  S.jobReq = { mode };
  S.busyAsked = force;
  try {
    const r = await api('/api/scan/start', body);
    S.job = r.job;
    renderJob(r.job);
    stopLoop('job');
    startLoop('job', jobTick, 800);
  } catch (e) {
    toast(e.message, true);
  }
}

async function loadResults() {
  for (const mode of ['quick', 'full']) {
    try {
      const r = await api(q('/api/scan/result', { mode }));
      S.results[mode] = r.result;
    } catch (e) {
      S.results[mode] = null;
    }
  }
  renderQuick();
  renderFull();
}

/* ------------------------------------------------------------------ Scan rapide */

function lineTitle(ln) {
  if (ln.kind === 'sat') {
    const a = ln.start + ln.lo, b = ln.stop + ln.lo;
    return `${esc(ln.label)} <span class="muted">${num(a / 1e9, 3)}–${num(b / 1e9, 3)} GHz · OL ${num(ln.lo / 1e6, 0)} MHz</span>`;
  }
  return `${esc(ln.label)} <span class="muted">${num(ln.start / 1e6, 0)}–${num(ln.stop / 1e6, 0)} MHz</span>`;
}

function renderLines(container, res, mode, rowsFn, headFn) {
  if (!res) {
    container.innerHTML = '<p class="muted">Aucun résultat enregistré.</p>';
    return;
  }
  let html = `<p class="muted">Résultat du ${esc(when(res.time))}${res.sat ? ' (Sat ON)' : ''}.</p>`;
  if (mode === 'full' && res.summary) {
    const s = res.summary;
    html += `<div class="card"><b>${s.locked}</b> multiplex verrouillés, <b>${s.services}</b> services,
      ${s.docsis} canaux DOCSIS, ${s.failed} non décodés.
      <div class="legend" style="margin-top:6px">${Object.entries({ 'DVB-C': 'DVB-C', 'DVB-T': 'DVB-T', 'DVB-T2': 'DVB-T2', 'DVB-S': 'DVB-S', 'DVB-S2': 'DVB-S2', DOCSIS: 'DOCSIS (internet)', wide: 'bloc large / doublon', none: 'non décodé (pointillés)' })
    .map(([k, v]) => `<span><i style="background:${DS_COLOR[k]}"></i>${v}</span>`).join('')}</div></div>`;
  }
  res.lines.forEach((ln, i) => {
    const n = (ln.channels || []).length;
    html += `<div class="card"><div class="card-head"><h3>${lineTitle(ln)}</h3>
      <span class="badge">${n} ${mode === 'quick' ? (ln.kind === 'sat' ? 'porteuse(s)' : 'canal(aux)') : 'canal(aux)'}</span>
      ${isNum(ln.floor_dbm) ? `<span class="muted">plancher ${num(ln.floor_dbm, 1)} dBm/case</span>` : ''}</div>
      ${ln.error ? `<div class="banner bad">${esc(ln.error)}</div>` : ''}
      <canvas class="spec" data-line="${i}"></canvas>
      ${mode === 'full' ? `<canvas class="snrbars" data-snr="${i}"></canvas>` : ''}
      <div class="readout" data-readout="${i}"></div>
      ${n ? `<div class="tablewrap"><table><thead>${headFn(ln)}</thead><tbody>${rowsFn(ln, i)}</tbody></table></div>` : ''}
      </div>`;
  });
  container.innerHTML = html;
  drawLines(container, res, mode);
  $$('canvas.spec', container).forEach((c) => hoverSetup(c, $(`[data-readout="${c.dataset.line}"]`, container)));
}

function drawLines(container, res, mode) {
  if (!res || !container.offsetParent) return;
  $$('canvas.spec', container).forEach((c) => drawSpectrum(c, res.lines[+c.dataset.line], mode));
  $$('canvas.snrbars', container).forEach((c) => drawSnrBars(c, res.lines[+c.dataset.snr]));
}

function quickHead(ln) {
  return ln.kind === 'sat'
    ? '<tr><th class="num">#</th><th class="num">Fréquence réelle</th><th class="num">FI</th><th class="num">Largeur −3 dB</th><th class="num">Débit symbole estimé</th><th class="num">Niveau (est.)</th><th class="num">Marge</th></tr>'
    : '<tr><th class="num">#</th><th class="num">Fréquence</th><th class="num">Largeur</th><th class="num">Niveau (est.)</th><th class="num">Marge</th><th>Type</th></tr>';
}

function quickRows(ln) {
  return (ln.channels || []).map((ch, i) => (ln.kind === 'sat'
    ? `<tr><td class="num">${i + 1}</td><td class="num">${mhz(ch.rf, 1)}</td><td class="num">${mhz(ch.freq, 1)}</td>
       <td class="num">${mhz(ch.width, 2)}</td><td class="num">${kbd(ch.symbol_rate_est)}</td>
       <td class="num">${num(ch.level_dbm, 1)} dBm</td><td class="num">${num(ch.margin_db, 1)} dB</td></tr>`
    : `<tr><td class="num">${i + 1}</td><td class="num">${mhz(ch.freq, 1)}</td><td class="num">${mhz(ch.width, 0)}</td>
       <td class="num">${num(ch.level_dbm, 1)} dBm</td><td class="num">${num(ch.margin_db, 1)} dB</td>
       <td>${ch.kind === 'wide' ? 'bloc large (OFDM DOCSIS 3.1 ?)' : 'canal 8 MHz'}</td></tr>`)).join('');
}

function renderQuick() {
  renderLines($('#quickResult'), S.results.quick, 'quick', quickRows, quickHead);
}

TAB_HOOKS.quick = {
  async enter() {
    startLoop('job', jobTick, 800);
    if (!S.results.quick && !S.results.full) await loadResults();
    else drawLines($('#quickResult'), S.results.quick, 'quick');
  },
  leave() { if (!(S.job && S.job.state === 'running')) stopLoop('job'); },
};

/* ------------------------------------------------------------------ Scan complet */

function fullHead(ln) {
  return `<tr><th class="num">Fréquence${ln.kind === 'sat' ? ' réelle' : ''}</th><th>Norme</th><th class="num">Débit symb. / largeur</th>
    <th>Modulation</th><th>Rendement</th><th>FFT · IG · PLP</th><th>Verrou</th><th class="num">SNR</th>
    <th class="num">Décalage</th><th class="num">Débit TS</th><th class="num">Erreurs TS</th><th class="num">Services</th><th>État</th></tr>`;
}

function fullRows(ln, li) {
  return (ln.channels || []).map((ch, i) => {
    const sat = ln.kind === 'sat';
    const isT = ch.delsys === 'DVB-T' || ch.delsys === 'DVB-T2';
    const svc = ch.services || [];
    const scr = svc.filter((s) => s.scrambled).length;
    const f = sat ? (ch.rf ?? ch.freq + ln.lo) : ch.freq;
    const ofdm = isT ? [ch.fft ? `${ch.fft / 1024}K` : null, ch.guard, ch.delsys === 'DVB-T2' && isNum(ch.plp) ? `PLP ${ch.plp}` : null].filter((x) => x).join(' · ') : '';
    const ds = ch.docsis ? 'DOCSIS' : (ch.delsys || (ch.class === 'wide' ? 'OFDM large' : '—'));
    const lock = ch.skip || ch.class === 'wide' ? '<span class="badge">—</span>'
      : ch.docsis ? '<span class="badge">internet</span>' : lockBadge(ch.locked, 'x');
    const rate = isT ? mhz(ch.bandwidth || 8e6, 0) : (ch.symbol_rate ? kbd(ch.symbol_rate) : (ch.width ? mhz(ch.width, 1) : '—'));
    let row = `<tr class="${svc.length ? 'click' : ''}" data-svc="${li}-${i}">
      <td class="num">${mhz(f, sat ? 1 : 3)}${sat ? ` ${esc(ch.polarization || ln.id[0])}` : ''}</td>
      <td><i style="display:inline-block;width:9px;height:9px;border-radius:2px;background:${chanColor(ch)}"></i> ${esc(ds)}${ch.source === 'nit' ? ' <span class="badge">NIT</span>' : ''}</td>
      <td class="num">${rate}</td><td>${esc(ch.modulation || '—')}</td><td>${esc(ch.code_rate || '—')}</td>
      <td>${esc(ofdm || '—')}</td><td>${lock}</td>
      <td class="num">${isNum(ch.snr_db) ? num(ch.snr_db, 1) + ' dB' : '—'}</td>
      <td class="num">${isNum(ch.freq_offset_hz) ? num(ch.freq_offset_hz / 1e3, 1) + ' kHz' : '—'}</td>
      <td class="num">${isNum(ch.ts_mbps) ? num(ch.ts_mbps, 2) + ' Mbit/s' : '—'}</td>
      <td class="num">${isNum(ch.ts_errors) ? num(ch.ts_errors, 0) : '—'}</td>
      <td class="num">${svc.length ? `${svc.length}${scr ? ` (${scr} chiffrés)` : ''} ▸` : '—'}</td>
      <td class="muted">${esc(ch.status || '')}</td></tr>`;
    if (svc.length) {
      const items = svc.map((s) => `<li>${esc(s.name || `service ${s.sid}`)} <span class="muted">— ${esc(s.provider || '?')},
        ${esc(SVC_TYPE[s.type] || `type 0x${Number(s.type || 0).toString(16)}`)}, sid ${esc(s.sid)}</span>
        ${s.scrambled ? '<span class="badge warn">chiffré</span>' : '<span class="badge ok">en clair</span>'}</li>`).join('');
      const ids = [isNum(ch.tsid) ? `TSID ${ch.tsid}` : null, isNum(ch.onid) ? `ONID ${ch.onid}` : null, ch.network ? `réseau « ${esc(ch.network)} »` : null].filter((x) => x).join(' · ');
      row += `<tr class="svc" data-svcrow="${li}-${i}" hidden><td colspan="13"><div class="muted">${ids}</div><ul class="services">${items}</ul></td></tr>`;
    }
    return row;
  }).join('');
}

function renderFull() {
  const box = $('#fullResult');
  renderLines(box, S.results.full, 'full', fullRows, fullHead);
  $$('tr.click', box).forEach((tr) => {
    tr.onclick = () => {
      const r = $(`tr[data-svcrow="${tr.dataset.svc}"]`, box);
      if (r) r.hidden = !r.hidden;
    };
  });
  const res = S.results.full;
  const has = (kind) => res && res.lines.some((l) => (l.kind === 'sat') === (kind === 'sat')
    && l.channels.some((c) => c.locked && !c.docsis && c.delsys));
  for (const [id, kind] of [['#expCable', 'cable'], ['#expSat', 'sat']]) {
    const a = $(id);
    if (has(kind)) {
      a.href = q('/api/export', { kind });
      a.removeAttribute('aria-disabled');
      a.style.opacity = '';
      a.style.pointerEvents = '';
    } else {
      a.removeAttribute('href');
      a.setAttribute('aria-disabled', 'true');
      a.style.opacity = '.45';
      a.style.pointerEvents = 'none';
    }
  }
}

TAB_HOOKS.full = {
  async enter() {
    startLoop('job', jobTick, 800);
    if (!S.results.quick && !S.results.full) await loadResults();
    else drawLines($('#fullResult'), S.results.full, 'full');
  },
  leave() { if (!(S.job && S.job.state === 'running')) stopLoop('job'); },
};

/* ------------------------------------------------------------------ Alignement (satellite ou terrestre) */

const UHF = { first: 21, last: 69 }; // canal N : 306 + 8 N MHz (Europe, 8 MHz)
const uhfChannel = (f) => { const n = Math.round((f / 1e6 - 306) / 8); return n >= UHF.first && n <= UHF.last && Math.abs(f / 1e6 - 306 - 8 * n) < 1 ? n : null; };

function alParams() {
  if ($('#alMode').value === 'ter') {
    const f = parseFloat($('#alTFreq').value) * 1e6;
    const plp = parseInt($('#alTPlp').value, 10);
    return { mode: 'ter', rf: f, delsys: $('#alTDelsys').value, bw: parseInt($('#alTBw').value, 10),
             plp: isNum(plp) && plp >= 0 ? plp : -1, ok: isNum(f) && f >= 47e6 && f <= 1000e6 };
  }
  const unit = $('#alUnit').value;
  const v = parseFloat($('#alFreq').value);
  const tone = $('#alTone').checked;
  const volt = $('input[name=alVolt]:checked').value;
  const L = S.cfg.lnb;
  const lo = tone ? L.lo_high : L.lo_low;
  let rf, iff;
  if (unit === 'rf') { rf = v * 1e9; iff = rf - lo; } else { iff = v * 1e6; rf = iff + lo; }
  const rsk = parseFloat($('#alRs').value);
  return { mode: 'sat', rf, iff, lo, tone, volt, rs: isNum(rsk) && rsk > 0 ? rsk * 1e3 : 0, delsys: $('#alDelsys').value, ok: isNum(v) };
}

function alCalc() {
  if (!S.cfg) return;
  const p = alParams();
  const ter = p.mode === 'ter';
  $('#alTerCtl').hidden = !ter;
  $('#alSatCtl').hidden = ter;
  $('#alLnbRow').hidden = ter;
  if (ter) {
    const n = p.ok ? uhfChannel(p.rf) : null;
    $('#alTCalc').textContent = !p.ok ? 'fréquence ? (47–1000 MHz)' : n ? `canal UHF ${n}` : 'hors grille UHF (VHF ou décalage)';
    return;
  }
  const L = S.cfg.lnb;
  let t = `réelle ${ghz(p.rf, 4)} · FI ${mhz(p.iff, 1)} · OL ${num(p.lo / 1e6, 0)} MHz`;
  const sat = S.cfg.sat;
  if (!p.ok) t = 'fréquence ?';
  else if (p.iff < sat.if_start || p.iff > sat.if_stop) t += ' · ⚠ FI hors bande 950–2150 MHz';
  else if ((p.rf >= L.switch) !== p.tone) t += ` · ⚠ ${p.tone ? 'sous' : 'au-dessus du'} seuil ${num(L.switch / 1e6, 0)} MHz`;
  $('#alCalc').textContent = t;
}

async function alStart(force = false) {
  const p = alParams();
  if (!p.ok) { toast('Fréquence invalide', true); return; }
  const force_ = force || (S.mon && S.mon.force) || false;
  const body = p.mode === 'ter'
    ? { s: S.sock, delsys: p.delsys, freq: Math.round(p.rf), bandwidth: p.bw, plp: p.plp, force: force_ }
    : { s: S.sock, delsys: p.delsys, freq: Math.round(p.iff), symbol_rate: Math.round(p.rs), volt: p.volt, tone: p.tone, force: force_ };
  try {
    const r = await api('/api/monitor/start', body);
    const fresh = !S.mon;
    S.mon = { params: p, force: body.force, started: Date.now() };
    S.monLostAt = 0;
    if (fresh) S.hist = [];
    $('#alStart').textContent = 'Appliquer';
    $('#alStop').disabled = false;
    if (p.mode === 'ter') {
      const n = uhfChannel(p.rf);
      $('#alState').textContent = `en cours : ${mhz(p.rf, 3)}${n ? ` (canal ${n})` : ''}, ${p.delsys} ${num(p.bw / 1e6, 0)} MHz`;
    } else {
      $('#alState').textContent = `en cours : FI ${mhz(p.iff, 1)}, ${p.rs ? kbd(p.rs) : 'niveau seul (débit inconnu)'}`;
    }
    if (r.lnb_error) toast(`LNB : ${r.lnb_error}`, true);
    startLoop('align', alTick, 500);
  } catch (e) {
    if (e.busy) {
      if (await askForce('alignement')) return alStart(true);
      return;
    }
    toast(e.message, true);
  }
}

async function alStop(silent = false) {
  stopLoop('align');
  soundStop();
  const was = S.mon;
  S.mon = null;
  $('#alStart').textContent = 'Démarrer';
  $('#alStop').disabled = true;
  $('#alState').textContent = was ? 'arrêté' : '';
  if (was) {
    try { await api('/api/monitor/stop', { s: S.sock }); } catch (e) { if (!silent) toast(e.message, true); }
  }
}

function stopBeacon() {
  if (!S.mon) return;
  try {
    navigator.sendBeacon('/api/monitor/stop', new Blob([JSON.stringify({ s: S.sock })], { type: 'application/json' }));
  } catch (e) { /* rien */ }
}

function setMeter(el, frac) {
  $('.meter > div', el).style.width = `${(Math.max(0, Math.min(1, frac)) * 100).toFixed(1)}%`;
}

async function alTick() {
  let st;
  try {
    st = await api(q('/api/status'));
    setConn('ok', 'dvbrx connecté');
  } catch (e) {
    setConn('bad', e.message);
    $('#alState').textContent = e.message;
    return;
  }
  const r = st.radio || {};
  const ch = (st.channels || []).find((c) => c.state === 'monitor');
  if (!ch) {
    if (!S.monLostAt) S.monLostAt = Date.now();
    if (Date.now() - S.mon.started > 5000 && Date.now() - S.monLostAt > 2000) {
      $('#alState').textContent = 'le moniteur n\'est plus actif dans dvbrx (arrêté ou repris par une application)';
      stopLoop('align');
      soundStop();
      S.mon = null;
      $('#alStart').textContent = 'Démarrer';
      $('#alStop').disabled = true;
    }
    return;
  }
  S.monLostAt = 0;
  const lvl = isNum(ch.level_dbfs) ? ch.level_dbfs : r.level_dbfs;
  const dbm = isNum(ch.input_dbm) ? ch.input_dbm : r.input_dbm;
  const gain = isNum(ch.gain) ? ch.gain : r.gain;
  const snr = ch.locked && isNum(ch.snr_db) ? ch.snr_db : null;
  const gl = $('#gLevel'), gs = $('#gSnr'), gk = $('#gLock');
  $('[data-v]', gl).textContent = isNum(lvl) ? num(lvl, 1) : '—';
  $('[data-dbm]', gl).textContent = isNum(dbm) ? num(dbm, 1) : '—';
  $('[data-gain]', gl).textContent = isNum(gain) ? num(gain, 0) : '—';
  setMeter(gl, isNum(lvl) ? (lvl + 60) / 60 : 0);
  $('[data-v]', gs).textContent = snr !== null ? num(snr, 1) : '—';
  $('[data-mod]', gs).textContent = [ch.modulation, ch.code_rate, ch.delsys].filter((x) => x).join(' · ') || (S.mon && S.mon.params.mode === 'sat' && !S.mon.params.rs ? 'débit symbole non donné : niveau seul' : '—');
  setMeter(gs, snr !== null ? snr / 20 : 0);
  gk.classList.toggle('ok', !!ch.locked);
  gk.classList.toggle('bad', !ch.locked);
  $('[data-v]', gk).textContent = ch.locked ? 'VERROUILLÉ' : 'NON VERROUILLÉ';
  $('[data-off]', gk).textContent = isNum(ch.freq_offset_hz) ? `décalage ${num(ch.freq_offset_hz / 1e3, 0)} kHz` : '';
  const now = Date.now() / 1000;
  S.hist.push({ t: now, lvl, snr, locked: !!ch.locked });
  while (S.hist.length && S.hist[0].t < now - 660) S.hist.shift();
  drawHist();
  soundUpdate(snr, lvl);
}

function drawHist() {
  const c = $('#alignHist');
  if (!c.offsetParent) return;
  const { ctx, w, h } = setupCanvas(c);
  ctx.clearRect(0, 0, w, h);
  const span = parseFloat($('#alSpan').value);
  const now = Date.now() / 1000;
  const ml = 40, mr = 40, mt = 8, mb = 20;
  const X = (t) => ml + (1 - (now - t) / span) * (w - ml - mr);
  const pts = S.hist.filter((p) => p.t >= now - span);
  let smax = 20;
  for (const p of pts) if (isNum(p.snr) && p.snr > smax - 2) smax = Math.ceil((p.snr + 3) / 5) * 5;
  const YL = (v) => mt + (1 - (v + 60) / 60) * (h - mt - mb);
  const YS = (v) => mt + (1 - v / smax) * (h - mt - mb);
  ctx.font = '11px system-ui, sans-serif';
  ctx.strokeStyle = '#ffffff12';
  for (let v = -60; v <= 0; v += 10) {
    ctx.beginPath(); ctx.moveTo(ml, YL(v) + 0.5); ctx.lineTo(w - mr, YL(v) + 0.5); ctx.stroke();
    ctx.fillStyle = '#4ea1ff'; ctx.fillText(String(v), 6, YL(v) + 4);
    const sv = smax * (v + 60) / 60;
    ctx.fillStyle = '#3ccf7a'; ctx.fillText(num(sv, 0), w - mr + 6, YL(v) + 4);
  }
  ctx.fillStyle = '#8d97a6';
  const step = span <= 60 ? 10 : span <= 300 ? 60 : 120;
  for (let s = 0; s <= span; s += step) {
    const x = X(now - s);
    const lab = s ? `−${s >= 60 ? s / 60 + ' min' : s + ' s'}` : 'maintenant';
    ctx.fillText(lab, Math.min(x - ctx.measureText(lab).width / 2, w - mr - ctx.measureText(lab).width), h - 5);
  }
  for (const p of pts) {
    if (!p.locked) { ctx.fillStyle = '#ff5d5d55'; ctx.fillRect(X(p.t) - 1, h - mb - 4, 3, 4); }
  }
  const trace = (key, Y, col) => {
    ctx.strokeStyle = col; ctx.lineWidth = 2; ctx.beginPath();
    let pen = false;
    for (const p of pts) {
      if (!isNum(p[key])) { pen = false; continue; }
      const x = X(p.t), y = Y(Math.max(key === 'snr' ? 0 : -60, Math.min(key === 'snr' ? smax : 0, p[key])));
      if (pen) ctx.lineTo(x, y); else ctx.moveTo(x, y);
      pen = true;
    }
    ctx.stroke();
  };
  trace('lvl', YL, '#4ea1ff');
  trace('snr', YS, '#3ccf7a');
}

/* son : hauteur selon le SNR (verrouillé) ou le niveau */
const SND = { ctx: null, osc: null, gain: null };
function soundUpdate(snr, lvl) {
  if (!$('#alSound').checked) return;
  try {
    if (!SND.ctx) SND.ctx = new (window.AudioContext || window.webkitAudioContext)();
    if (!SND.osc) {
      SND.osc = SND.ctx.createOscillator();
      SND.gain = SND.ctx.createGain();
      SND.gain.gain.value = 0.04;
      SND.osc.connect(SND.gain).connect(SND.ctx.destination);
      SND.osc.start();
    }
    const f = snr !== null ? 400 + snr * 45 : 150 + Math.max(0, (isNum(lvl) ? lvl : -60) + 60) * 4;
    SND.osc.frequency.setTargetAtTime(f, SND.ctx.currentTime, 0.1);
  } catch (e) { /* pas de son */ }
}
function soundStop() {
  if (SND.osc) { try { SND.osc.stop(); } catch (e) { /* rien */ } SND.osc = null; }
}

TAB_HOOKS.align = {
  enter() { alCalc(); drawHist(); if (S.mon) startLoop('align', alTick, 500); },
  // l'alignement continue sur les autres onglets (la constellation, par exemple) : seuls le
  // bouton Arrêter et la fermeture de la page l'arrêtent ; alTick garde dvbrx informé
  leave() { /* rien */ },
  hidden() { /* on continue (la sécurité de 30 s de dvbrx s'applique si le navigateur ralentit) */ },
};

/* ------------------------------------------------------------------ démarrage */

function satToggleSync(inp) {
  const st = $('.state', inp.closest('.switch'));
  if (st) st.textContent = inp.checked ? 'ON' : 'OFF';
  try { localStorage.setItem('dvbrxweb.sat', inp.checked ? '1' : '0'); } catch (e) { /* rien */ }
  $$('.satToggle').forEach((o) => {
    if (o !== inp) { o.checked = inp.checked; const s2 = $('.state', o.closest('.switch')); if (s2) s2.textContent = inp.checked ? 'ON' : 'OFF'; }
  });
}

function bind() {
  $$('#tabs button').forEach((b) => { b.onclick = () => showTab(b.dataset.tab); });
  $('#sock').onchange = async () => {
    if (S.mon) await alStop(true);
    S.sock = parseInt($('#sock').value, 10) || 0;
    S.results = { quick: null, full: null };
    S.job = null;
    CONST.acc = null;
    const h = TAB_HOOKS[S.tab];
    if (h && h.leave) h.leave();
    if (h && h.enter) h.enter();
    loadResults();
  };
  $('#constClear').onclick = constReset;
  $('#constAdapter').onchange = constReset;
  $$('.satToggle').forEach((i) => { i.onchange = () => satToggleSync(i); });
  $('#quickGo').onclick = () => startScan('quick');
  $('#fullGo').onclick = () => startScan('full');
  ['#alFreq', '#alRs', '#alDelsys', '#alTone'].forEach((s) => $(s).addEventListener('input', alCalc));
  $$('input[name=alVolt]').forEach((i) => i.addEventListener('change', alCalc));
  $('#alUnit').onchange = () => {
    // conversion de la valeur affichée
    const L = S.cfg.lnb;
    const lo = $('#alTone').checked ? L.lo_high : L.lo_low;
    const v = parseFloat($('#alFreq').value);
    if (isNum(v)) {
      if ($('#alUnit').value === 'if') { $('#alFreq').value = ((v * 1e9 - lo) / 1e6).toFixed(1); $('#alFreq').step = '0.1'; } else { $('#alFreq').value = ((v * 1e6 + lo) / 1e9).toFixed(4); $('#alFreq').step = '0.001'; }
    }
    $('#alAuto').disabled = $('#alUnit').value !== 'rf';
    alCalc();
  };
  $('#alAuto').onclick = () => {
    const p = alParams();
    $('#alTone').checked = p.rf >= S.cfg.lnb.switch;
    alCalc();
    if (S.mon) alStart();
  };
  // contrôle manuel de la combinaison : appliqué tout de suite pendant l'alignement
  $('#alTone').addEventListener('change', () => { if (S.mon) alStart(); });
  $$('input[name=alVolt]').forEach((i) => i.addEventListener('change', () => { if (S.mon) alStart(); }));
  // terrestre : fréquence <-> canal UHF
  $('#alMode').onchange = () => {
    try { localStorage.setItem('dvbrxweb.alMode', $('#alMode').value); } catch (e) { /* rien */ }
    alCalc();
    if (S.mon) alStart();
  };
  $('#alTFreq').addEventListener('input', () => {
    const n = uhfChannel(parseFloat($('#alTFreq').value) * 1e6);
    $('#alTCh').value = n || '';
    alCalc();
  });
  $('#alTCh').addEventListener('input', () => {
    const n = parseInt($('#alTCh').value, 10);
    if (n >= UHF.first && n <= UHF.last) $('#alTFreq').value = 306 + 8 * n;
    alCalc();
  });
  ['#alTDelsys', '#alTBw', '#alTPlp'].forEach((s) => $(s).addEventListener('change', alCalc));
  $('#alStart').onclick = () => alStart();
  $('#alStop').onclick = () => alStop();
  $('#alSpan').onchange = drawHist;
  $('#alSound').onchange = () => { if (!$('#alSound').checked) soundStop(); };
  window.addEventListener('pagehide', stopBeacon);
  window.addEventListener('beforeunload', stopBeacon);
  let rt = null;
  window.addEventListener('resize', () => {
    clearTimeout(rt);
    rt = setTimeout(() => {
      drawLines($('#quickResult'), S.results.quick, 'quick');
      drawLines($('#fullResult'), S.results.full, 'full');
      drawHist();
    }, 150);
  });
}

async function init() {
  bind();
  try {
    S.cfg = await api('/api/config');
  } catch (e) {
    setConn('bad', e.message);
    $('#statusBanner').innerHTML = `<div class="banner bad">${esc(e.message)}</div>`;
    setTimeout(init, 5000);
    return;
  }
  const socks = S.cfg.sockets || [];
  const cards = S.cfg.cards || [];
  $('#sock').innerHTML = socks.map((s, i) => `<option value="${s.id}" title="${esc(s.path)}" data-name="${esc(s.name)}">${esc(sockLabel(s.name, cards[i]))}</option>`).join('');
  $('#sockSel').hidden = socks.length < 2;
  $('#cableStart').value = Math.round(S.cfg.cable.start / 1e6);
  $('#cableStop').value = Math.round(S.cfg.cable.stop / 1e6);
  let sat = false;
  try { sat = localStorage.getItem('dvbrxweb.sat') === '1'; } catch (e) { /* rien */ }
  $$('.satToggle').forEach((i) => { i.checked = sat; });
  satToggleSync($('#quickSat'));
  try { if (localStorage.getItem('dvbrxweb.alMode') === 'ter') $('#alMode').value = 'ter'; } catch (e) { /* rien */ }
  alCalc();
  const want = (location.hash || '').slice(1);
  S.tab = '';
  showTab(TAB_HOOKS[want] ? want : 'status');
  loadResults();
  pollStatusQuiet();
  // un scan lancé par un autre navigateur est aussi suivi
  jobTick();
}

init();
