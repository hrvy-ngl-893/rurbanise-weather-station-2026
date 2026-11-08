'use strict';
const $ = s => document.querySelector(s);
const esc = s => String(s).replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const num = (v, d = 1) => (v == null || isNaN(v)) ? '—' : (+v).toFixed(d);
const dirName = d => (d == null || d < 0) ? '—' : ['N','NE','E','SE','S','SW','W','NW'][Math.round(d / 45) % 8] + ' ' + Math.round(d) + '°';
let tab = 'live', isAdmin = false;
let riskListAt = 0;

// Every request has a time limit, so a slow or lost station never leaves the page waiting silently.
async function api(url, data, timeoutMs = 15000) {
  const o = data ? {method: 'POST', body: new URLSearchParams(data)} : (data === null ? {method: 'POST'} : {});
  const ac = new AbortController();
  const timer = setTimeout(() => ac.abort(), timeoutMs);
  o.signal = ac.signal;
  try {
    const r = await fetch(url, o);
    let j = {};
    try { j = await r.json(); } catch (e) {}
    j._ok = r.ok; j._status = r.status;
    return j;
  } catch (e) {
    return {_ok: false, _status: 0, error: e.name === 'AbortError'
      ? 'No answer from the station after ' + Math.round(timeoutMs / 1000) + ' s. Check that this device is still on its Wi-Fi.'
      : 'Station not reachable (its Wi-Fi may have turned off).'};
  } finally { clearTimeout(timer); }
}

function toast(m) {
  const t = $('#toast');
  t.textContent = m; t.hidden = false;
  clearTimeout(toast.h);
  toast.h = setTimeout(() => t.hidden = true, 6000);
}

// Shows what is happening while the station works: the button is disabled and says so, a status line next to it
// (and the Admin status bar) reads "Saving…", then the result.  Returns the station's answer.
async function act(btn, statusEl, busyText, fn, okText) {
  const label = btn ? btn.textContent : '';
  if (btn) { btn.disabled = true; btn.textContent = busyText; }
  setStatus(statusEl, busyText + ' The station can take a few seconds.', 'busy');
  const r = await fn();
  if (btn) { btn.disabled = false; btn.textContent = label; }
  const time = new Date().toLocaleTimeString([], {hour: '2-digit', minute: '2-digit'});
  setStatus(statusEl, r._ok ? (okText || 'Done') + ' · ' + time : 'Failed: ' + (r.error || 'HTTP ' + r._status), r._ok ? 'ok' : 'err');
  return r;
}
function setStatus(el, text, kind) {
  for (const e of [el, $('#adminbar .status')]) {
    if (!e) continue;
    e.textContent = text; e.className = 'status' + (kind ? ' ' + kind : '');
  }
}

/* ------------------------------------------------------------ tabs */
document.querySelectorAll('nav button').forEach(b => b.onclick = () => show(b.dataset.t));
function show(t) {
  tab = t;
  document.querySelectorAll('main>section').forEach(s => s.hidden = s.id !== t);
  document.querySelectorAll('nav button').forEach(b => b.classList.toggle('on', b.dataset.t === t));
  if (t === 'live') pollLive();
  if (t === 'hist') loadHist();
  if (t === 'data') loadFiles();
  if (t === 'admin') loadAdmin();
}
$('#ext').onclick = async () => { await api('/api/keepalive', null); pollLive(); };

/* ------------------------------------------------------------ live */
async function pollLive() {
  if (document.hidden) return;
  const j = await api('/api/live', undefined, 8000);
  if (!j._ok) { $('#sess').textContent = 'offline'; return; }
  const s = j.session_left_s;
  $('#sess').textContent = 'Wi-Fi off in ' + Math.floor(s / 60) + ':' + String(s % 60).padStart(2, '0');
  $('#clock').textContent = new Date(j.time * 1000).toLocaleString();
  if (tab === 'live') {
    renderLive(j);
    if (Date.now() - riskListAt >= 15000) loadRiskEvents();
  }
}
setInterval(pollLive, 3000);

const mapsUrl = (lat, lon) => 'https://www.google.com/maps?q=' + lat + ',' + lon;

function renderLive(j) {
  const n = j.now || {}, l = j.last || {};
  const w = [];
  let riskMessage = '';
  if (j.rtc_ok === false) w.push('Clock module (DS3231) not found: time may be wrong and wake-ups use a timer.');
  if (!j.sd_ok) w.push('SD card not available: data is not being logged.');
  if (!j.bmp_ok) w.push(j.bmp_error || 'Pressure/temperature sensor not responding.');
  if (!j.time_trusted) w.push('Clock not set. Open Admin and press "Set time from this device".');
  const usb = j.power_mode !== 1, pv = n.batt ?? l.batt;
  if (usb && pv != null && pv < 4.4) w.push('USB power is low (' + num(pv, 2) + ' V). Use a 5 V 2 A adapter and a short, thick USB cable.');
  if (!usb && pv != null && pv < 3.5) w.push('Battery is low (' + num(pv, 2) + ' V).');
  if (j.risk_level && j.risk_level !== 'none') {
    riskMessage = j.risk_level + ': the weather looks ' + Math.round((j.risk_score || 0) * 100) + '% like the hours before a reported "' +
      (j.risk_tag || 'hazard') + '" (last ' + j.risk_window_hours + ' h). Check outside. Not an official warning.';
  }
  if (j.risk_error && j.sd_ok) w.push('Hazard comparison unavailable: ' + j.risk_error + '.');
  const severity = {Advisory: 'risk-advisory', Warning: 'risk-warning', Critical: 'risk-critical'}[j.risk_level] || '';
  $('#warn').innerHTML = (riskMessage ? '<div class="banner ' + severity + '">' + esc(riskMessage) + '</div>' : '') +
    w.map(x => '<div class="banner">' + esc(x) + '</div>').join('');
  const kmh = v => v == null ? '—' : num(v * 3.6, 0);
  const c = [
    ['Temperature', num(n.temp ?? l.temp), '°C'],
    ['Humidity', num(n.humidity ?? l.humidity), '%'],
    ['Pressure', num(n.pressure ?? l.pressure, 1), 'hPa'],
    ['Rain today', num(j.rain_today, 1), 'mm'],
    ['Wind now', num(n.wind, 1), 'm/s (' + kmh(n.wind) + ' km/h)'],
    ['Wind direction', dirName(n.dir ?? l.wind_dir), ''],
    ['Wind avg / gust', num(l.wind_avg) + ' / ' + num(l.wind_gust), 'm/s'],
    [usb ? 'USB power' : 'Battery', num(pv, 2), 'V']
  ];
  $('#cards').innerHTML = c.map(x => '<div class="card"><small>' + x[0] + '</small><b>' + x[1] + '</b><u>' + x[2] + '</u></div>').join('');
  const age = l.ts ? Math.max(0, j.time - l.ts) : null;
  const facts = [
    ['Last saved', l.ts ? Math.round(age / 60) + ' min ago (every ' + Math.round(j.interval / 60) + ' min)' : 'nothing saved yet'],
    ['Running since', j.boot_ts ? new Date(j.boot_ts * 1000).toLocaleString() + ' (' + esc(j.boot_reason) + ')' : '—'],
    ['Location', j.lat != null && j.lon != null
      ? num(j.lat, 5) + ', ' + num(j.lon, 5) + ' · <a href="' + mapsUrl(j.lat, j.lon) + '" target="_blank" rel="noopener">map</a>'
      : 'not set (Admin › Station location)']
  ];
  $('#liveinfo').innerHTML = facts.map(f => '<dt>' + f[0] + '</dt><dd>' + f[1] + '</dd>').join('');
}

async function loadRiskEvents() {
  riskListAt = Date.now();
  const box = $('#riskEvents');
  if (!box) return;
  const j = await api('/api/risks');
  if (!j._ok) { box.textContent = j.error || 'Could not load reports'; return; }
  const events = (j.events || []).slice().reverse(), admin = !!j.admin;
  box.className = events.length ? '' : 'muted';
  box.innerHTML = events.length
    ? '<table><tr><th>When</th><th>Hazard</th><th>Rain 3 h</th>' + (admin ? '<th></th>' : '') + '</tr>' + events.map(e => {
        const w3 = (e.windows || [])[0] || {};
        return '<tr><td>' + esc(new Date(e.ts * 1000).toLocaleString([], {month: 'short', day: 'numeric', hour: '2-digit', minute: '2-digit'})) +
          '</td><td><b>' + esc(e.tag) + '</b>' + (e.notes ? '<br><span class="muted">' + esc(e.notes) + '</span>' : '') +
          '</td><td>' + num(w3.rain_mm, 1) + ' mm</td>' +
          (admin ? '<td><button type="button" class="del" data-ts="' + e.ts + '" data-tag="' + esc(e.tag) + '">Delete</button></td>' : '') + '</tr>';
      }).join('') + '</table>'
    : 'No reports yet.';
  box.querySelectorAll('button.del').forEach(b => b.onclick = async () => {
    if (!confirm('Delete the "' + b.dataset.tag + '" report? The station stops comparing the weather with it. This cannot be undone.')) return;
    const r = await act(b, $('#riskDelStatus'), 'Deleting…', () => api('/api/risks/delete', {ts: b.dataset.ts, tag: b.dataset.tag}), 'Report deleted');
    if (r._ok) { await loadRiskEvents(); pollLive(); }
  });
}

$('#riskForm').onsubmit = async e => {
  e.preventDefault();
  const r = await act($('#riskSubmit'), $('#riskStatus'), 'Saving…',
    () => api('/api/risks/log', {tag: $('#riskTag').value, notes: $('#riskNotes').value}), 'Report saved with the weather of the last 12 hours');
  if (!r._ok) return;
  $('#riskTag').value = ''; $('#riskNotes').value = '';
  riskListAt = 0;
  await loadRiskEvents();
  pollLive();
};

/* ------------------------------------------------------------ charts */
// One reading per chart, stacked, all on the same time axis.  A pointer on any chart shows the same moment on all.
const css = n => getComputedStyle(document.documentElement).getPropertyValue(n).trim();
function chart(cv, series, opt) {
  const dpr = devicePixelRatio || 1, W = cv.clientWidth, H = cv.clientHeight;
  cv.width = W * dpr; cv.height = H * dpr;
  const c = cv.getContext('2d');
  c.scale(dpr, dpr); c.clearRect(0, 0, W, H);
  const fg = css('--fg'), mut = css('--mut'), line = css('--line');
  c.font = '11px system-ui,sans-serif';
  const pts = series.flatMap(s => s.d.filter(p => p[1] != null));
  const L = 46, R = 10, T = 8, B = opt.xlabels ? 20 : 6;
  if (!pts.length) { c.fillStyle = mut; c.fillText('No data in this range', L, 24); return null; }
  const x0 = opt.x0, x1 = opt.x1;
  let y0 = Infinity, y1 = -Infinity;
  for (const p of pts) { y0 = Math.min(y0, p[1]); y1 = Math.max(y1, p[1]); }
  if (opt.bars) y0 = Math.min(0, y0);
  if (y0 === y1) { y0 -= 1; y1 += 1; }
  const pad = (y1 - y0) * 0.08;
  if (!opt.bars) y0 -= pad;
  y1 += pad;
  const X = t => L + (t - x0) / (x1 - x0 || 1) * (W - L - R);
  const Y = v => H - B - (v - y0) / (y1 - y0) * (H - T - B);
  c.strokeStyle = line; c.fillStyle = mut; c.lineWidth = 1;
  for (let i = 0; i <= 3; i++) {
    const v = y0 + (y1 - y0) * i / 3, y = Y(v);
    c.beginPath(); c.moveTo(L, y); c.lineTo(W - R, y); c.stroke();
    c.fillText(v.toFixed(Math.abs(y1 - y0) < 1 ? 2 : Math.abs(y1 - y0) < 10 ? 1 : 0), 4, y + 4);
  }
  const span = x1 - x0;
  for (let i = 0; i <= 4; i++) {
    const t = x0 + span * i / 4, x = X(t);
    c.strokeStyle = line; c.beginPath(); c.moveTo(x, T); c.lineTo(x, H - B); c.stroke();
    if (!opt.xlabels) continue;
    const d = new Date(t * 1000);
    const s = span > 60 * 86400 ? d.toLocaleDateString([], {month: 'short', year: '2-digit'})
      : span > 172800 ? d.toLocaleDateString([], {month: 'short', day: 'numeric'}) : d.toLocaleTimeString([], {hour: '2-digit', minute: '2-digit'});
    c.fillStyle = mut; c.textAlign = i === 0 ? 'left' : i === 4 ? 'right' : 'center';
    c.fillText(s, Math.min(Math.max(x, L), W - R), H - 5);
  }
  c.textAlign = 'left';
  series.forEach(s => {
    c.strokeStyle = c.fillStyle = s.c; c.lineWidth = 2;
    if (opt.bars) {
      const bw = Math.max(1, (W - L - R) / Math.max(1, s.d.length) * 0.75);
      s.d.forEach(p => { if (p[1] != null && p[1] > 0) c.fillRect(X(p[0]) - bw / 2, Y(p[1]), bw, Y(0) - Y(p[1])); });
    } else {
      c.beginPath();
      let pen = false;
      s.d.forEach(p => {
        if (p[1] == null) { pen = false; return; }
        pen ? c.lineTo(X(p[0]), Y(p[1])) : c.moveTo(X(p[0]), Y(p[1]));
        pen = true;
      });
      c.stroke();
    }
  });
  if (opt.cursor != null) {
    const x = X(opt.cursor);
    c.strokeStyle = fg; c.lineWidth = 1; c.setLineDash([3, 3]);
    c.beginPath(); c.moveTo(x, T); c.lineTo(x, H - B); c.stroke(); c.setLineDash([]);
  }
  return {X, L, R, W};
}

/* ------------------------------------------------------------ history */
// row layout: 0 ts, 1 temp, 2 pres, 3 wavg, 4 wgust, 5 wdir, 6 rain, 7 batt, 8 humidity
const METRICS = {
  temp: {name: 'Temperature', cols: [1], unit: '°C', d: 1},
  humidity: {name: 'Humidity', cols: [8], unit: '%', d: 0},
  pres: {name: 'Air pressure', cols: [2], unit: 'hPa', d: 1},
  wind: {name: 'Wind', cols: [3, 4], keys: ['average', 'gust'], unit: 'm/s', d: 1},
  rain: {name: 'Rain', cols: [6], unit: 'mm', d: 1, bars: true},
  batt: {name: 'Power (USB volts)', cols: [7], unit: 'V', d: 2}
};
let H = [], hStride = 1, hInterval = 300, hFrom = 0, hTo = 0, hCursor = null;
const picked = () => [...document.querySelectorAll('#metrics input:checked')].map(i => i.value);
const bucketName = s => s < 3600 ? (s / 60) + ' min' : s < 86400 ? Math.round(s / 3600) + ' h' : Math.round(s / 86400) + ' day' + (s >= 172800 ? 's' : '');

async function loadHist() {
  const to = Math.floor(Date.now() / 1000), range = +$('#range').value;
  const long = range > 31 * 86400;
  const st = $('#hsum');
  st.textContent = long ? 'Reading ' + (range > 100 * 86400 ? 'a year' : '90 days') + ' of files from the SD card… this can take up to 30 seconds.' : 'Loading…';
  st.className = 'status busy';
  const j = await api('/api/history?from=' + (to - range) + '&to=' + to + '&max=' + (long ? 365 : 400), undefined, long ? 60000 : 20000);
  if (!j._ok) { H = []; st.textContent = j.error || 'Could not load history'; st.className = 'status err'; drawHist(); return; }
  H = j.rows || []; hStride = j.stride || 1; hInterval = j.interval || 300; hFrom = to - range; hTo = to;
  st.className = 'status';
  st.textContent = H.length ? 'Each point is ' + (hStride > 1 ? 'the average of ' + bucketName(hStride * hInterval) : 'one ' + bucketName(hInterval) + ' reading') +
    '. Move over a chart to read every reading at that moment.' : '';
  drawHist();
}

function statsText(m, rows) {
  const v = rows.map(r => r[m.cols[0]]).filter(x => x != null);
  if (!v.length) return '';
  const sum = v.reduce((a, b) => a + b, 0);
  return m.bars ? 'total ' + sum.toFixed(1) + ' ' + m.unit
    : 'min ' + Math.min(...v).toFixed(m.d) + ' · max ' + Math.max(...v).toFixed(m.d) + ' · avg ' + (sum / v.length).toFixed(m.d) + ' ' + m.unit;
}
function readout(m, t) {
  if (!H.length) return '';
  let best = H[0];
  for (const r of H) if (Math.abs(r[0] - t) < Math.abs(best[0] - t)) best = r;
  const when = new Date(best[0] * 1000).toLocaleString([], {month: 'short', day: 'numeric', hour: '2-digit', minute: '2-digit'});
  return when + ': ' + m.cols.map((c, i) => (m.keys ? m.keys[i] + ' ' : '') + num(best[c], m.d)).join(', ') + ' ' + m.unit;
}

function drawHist() {
  const box = $('#charts'), keys = picked();
  const colors = [css('--s1'), css('--s2')];
  if (box.dataset.keys !== keys.join()) {
    box.dataset.keys = keys.join();
    box.innerHTML = keys.map(k => {
      const m = METRICS[k];
      const key = m.keys ? '<span class="key">' + m.keys.map((n, j) => '<i style="background:' + colors[j] + '"></i>' + n).join('') + '</span>' : '';
      return '<div class="chart" data-k="' + k + '"><div class="ch"><b><em></em>' + key + '</b><span></span></div><canvas></canvas></div>';
    }).join('') || '<p class="muted">Tick at least one reading above.</p>';
    box.querySelectorAll('canvas').forEach(cv => {
      cv.onpointermove = e => { const g = cv._geo; if (!g) return; const fx = (e.offsetX - g.L) / (g.W - g.L - g.R); hCursor = hFrom + Math.min(1, Math.max(0, fx)) * (hTo - hFrom); drawHist(); };
      cv.onpointerleave = () => { hCursor = null; drawHist(); };
    });
  }
  box.querySelectorAll('.chart').forEach((el, i, all) => {
    const m = METRICS[el.dataset.k];
    const series = m.cols.map((col, j) => ({c: colors[j], d: H.map(r => [r[0], r[col]])}));
    const cv = el.querySelector('canvas');
    cv._geo = chart(cv, series, {bars: m.bars, x0: hFrom, x1: hTo, xlabels: i === all.length - 1, cursor: hCursor});
    el.querySelector('.ch em').textContent = m.name + (m.bars ? ' per ' + bucketName(hStride * hInterval) : '') + ' (' + m.unit + ')';
    el.querySelector('.ch > span').textContent = hCursor != null ? readout(m, hCursor) : statsText(m, H);
  });
}
$('#metrics').onchange = drawHist;
$('#range').onchange = loadHist;
addEventListener('resize', () => { if (tab === 'hist') drawHist(); });

const stamp = () => new Date().toISOString().slice(0, 16).replace(/[T:]/g, '-');
function saveBlob(name, blob) {
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob); a.download = name;
  document.body.append(a); a.click(); a.remove();
  setTimeout(() => URL.revokeObjectURL(a.href), 5000);
}
$('#exp_png').onclick = () => {
  const charts = [...document.querySelectorAll('#charts .chart')];
  if (!charts.length || !H.length) return toast('Nothing to save yet');
  const dpr = devicePixelRatio || 1, w = charts[0].querySelector('canvas').width, head = 26 * dpr, title = 40 * dpr;
  const out = document.createElement('canvas');
  out.width = w; out.height = title + charts.reduce((s, el) => s + head + el.querySelector('canvas').height, 0);
  const c = out.getContext('2d');
  c.fillStyle = css('--bg'); c.fillRect(0, 0, out.width, out.height);
  c.fillStyle = css('--fg'); c.font = 'bold ' + 15 * dpr + 'px system-ui,sans-serif';
  c.fillText('Weather Station · ' + $('#range').selectedOptions[0].text + ' · to ' + new Date(hTo * 1000).toLocaleString(), 6 * dpr, 24 * dpr);
  let y = title;
  for (const el of charts) {
    c.font = 'bold ' + 12 * dpr + 'px system-ui,sans-serif'; c.fillStyle = css('--fg');
    c.fillText(el.querySelector('.ch em').textContent + '   ' + el.querySelector('.ch > span').textContent, 6 * dpr, y + 17 * dpr);
    y += head;
    const cv = el.querySelector('canvas');
    c.drawImage(cv, 0, y); y += cv.height;
  }
  out.toBlob(b => saveBlob('weather_chart_' + stamp() + '.png', b));
};
$('#exp_csv').onclick = () => {
  if (!H.length) return toast('Nothing to save yet');
  const keys = picked(), cols = ['time'], idx = [];
  for (const k of keys) METRICS[k].cols.forEach((c, i) => { cols.push(k + (METRICS[k].keys ? '_' + METRICS[k].keys[i] : '') + '_' + METRICS[k].unit.replace('°', '')); idx.push(c); });
  const lines = [cols.join(',')].concat(H.map(r => [new Date(r[0] * 1000).toISOString()].concat(idx.map(c => r[c] ?? '')).join(',')));
  saveBlob('weather_chart_' + stamp() + '.csv', new Blob([lines.join('\n') + '\n'], {type: 'text/csv'}));
};

/* ------------------------------------------------------------ data / downloads */
const day = d => d.toISOString().slice(0, 10);
$('#d_to').value = day(new Date());
$('#d_from').value = day(new Date(Date.now() - 6 * 864e5));
$('#d_go').onclick = () => {
  const f = $('#d_from').value, t = $('#d_to').value;
  if (!f || !t) return toast('Pick both dates');
  const from = Math.floor(new Date(f + 'T00:00:00').getTime() / 1000);
  const to = Math.floor(new Date(t + 'T23:59:59').getTime() / 1000);
  toast('Preparing the download… the station reads one file per day.');
  location.href = '/api/range.csv?from=' + from + '&to=' + to;
};
async function loadFiles() {
  $('#sdinfo').textContent = 'Loading…';
  const j = await api('/api/files');
  if (!j._ok) { $('#sdinfo').textContent = j.error || 'Could not list files'; return; }
  const mb = b => (b / 1048576).toFixed(b > 1e8 ? 0 : 1);
  $('#sdinfo').textContent = j.sd_ok ? (j.total ? 'SD card: ' + mb(j.used) + ' MB used of ' + mb(j.total) + ' MB' : 'SD card OK') : 'SD card not available';
  const rows = (j.files || []).slice().reverse();
  $('#files').innerHTML = rows.length
    ? rows.map(f => '<tr><td><a href="/api/file?name=' + encodeURIComponent(f.name) + '" download>' + esc(f.name) + '</a></td><td>' + (f.size / 1024).toFixed(1) + ' KB</td></tr>').join('')
    : '<tr><td class="muted">No files yet.</td></tr>';
}

/* ------------------------------------------------------------ station location */
// Accepts "14.5995, 120.9842", "14.5995 120.9842", a Google Maps link (…@14.59,120.98,17z or ?q=14.59,120.98)
// and degrees-minutes-seconds such as 14°35'58.2"N 120°59'03.1"E. Returns [lat, lon] or null.
function parseLatLon(t) {
  t = String(t || '').trim();
  if (!t) return null;
  const dms = /(\d+(?:\.\d+)?)\s*°\s*(?:(\d+(?:\.\d+)?)\s*['′]\s*)?(?:(\d+(?:\.\d+)?)\s*["″]\s*)?([NSEW])/gi;
  const parts = [...t.matchAll(dms)];
  if (parts.length === 2) {
    const v = parts.map(m => {
      const d = (+m[1]) + (+(m[2] || 0)) / 60 + (+(m[3] || 0)) / 3600;
      return {d: /[SW]/i.test(m[4]) ? -d : d, ns: /[NS]/i.test(m[4])};
    });
    const lat = v.find(x => x.ns), lon = v.find(x => !x.ns);
    return lat && lon ? [lat.d, lon.d] : null;
  }
  const m = t.match(/@(-?\d+(?:\.\d+)?),\s*(-?\d+(?:\.\d+)?)/) || t.match(/[?&](?:q|query|ll)=(-?\d+(?:\.\d+)?),\s*(-?\d+(?:\.\d+)?)/) ||
            t.match(/^\s*(-?\d+(?:\.\d+)?)\s*[,;\s]\s*(-?\d+(?:\.\d+)?)\s*$/);
  if (!m) return null;
  const lat = +m[1], lon = +m[2];
  return Math.abs(lat) <= 90 && Math.abs(lon) <= 180 ? [lat, lon] : null;
}
function bindLocation() {
  const showLink = () => {
    const la = $('#loc_lat').value, lo = $('#loc_lon').value;
    $('#loc_link').innerHTML = la && lo ? '<a href="' + mapsUrl(la, lo) + '" target="_blank" rel="noopener">Check on the map</a>' : '';
  };
  $('#loc_paste').oninput = () => {
    const v = parseLatLon($('#loc_paste').value);
    if (v) { $('#loc_lat').value = v[0].toFixed(6); $('#loc_lon').value = v[1].toFixed(6); showLink(); }
  };
  $('#loc_lat').oninput = $('#loc_lon').oninput = showLink;
  showLink();
  $('#loc_gps').onclick = () => {
    const hint = 'This device will not share its location with the station page (it is not a secure https page). ' +
      'Open Google Maps on any phone or computer, find the station, copy its coordinates and paste them in the box above.';
    if (!navigator.geolocation || !window.isSecureContext) { setStatus($('#loc_status'), hint, 'err'); return; }
    setStatus($('#loc_status'), 'Asking this device for its location…', 'busy');
    navigator.geolocation.getCurrentPosition(p => {
      $('#loc_lat').value = p.coords.latitude.toFixed(6); $('#loc_lon').value = p.coords.longitude.toFixed(6); showLink();
      setStatus($('#loc_status'), 'Filled in (accuracy about ' + Math.round(p.coords.accuracy) + ' m). Press Save location.', 'ok');
    }, () => setStatus($('#loc_status'), hint, 'err'), {enableHighAccuracy: true, timeout: 15000, maximumAge: 0});
  };
  $('#loc_save').onclick = async () => {
    const la = $('#loc_lat').value.trim(), lo = $('#loc_lon').value.trim();
    if (!la || !lo) return setStatus($('#loc_status'), 'Enter both latitude and longitude', 'err');
    await act($('#loc_save'), $('#loc_status'), 'Saving…', () => api('/api/location', {lat: la, lon: lo}), 'Location saved');
  };
  $('#loc_clear').onclick = async () => {
    const r = await act($('#loc_clear'), $('#loc_status'), 'Clearing…', () => api('/api/location', {clear: '1'}), 'Location cleared');
    if (r._ok) { $('#loc_lat').value = ''; $('#loc_lon').value = ''; showLink(); }
  };
}

/* ------------------------------------------------------------ admin */
let reminderTimer = null;
const REMIND = 'Keep your admin password written down somewhere safe. If you lose it, hold the station button for 10 seconds (allowed once every 30 days).';

async function loadAdmin() {
  const A = $('#admin');
  A.innerHTML = '<p class="status busy">Asking the station…</p>';
  const s = await api('/api/session');
  if (!s._ok) { A.innerHTML = '<p class="status err">' + esc(s.error || 'Could not reach station') + '</p>'; return; }
  isAdmin = !!s.admin; riskListAt = 0;
  clearInterval(reminderTimer);
  if (s.admin) { renderAdmin(); reminderTimer = setInterval(() => toast(REMIND), 600000); return; }
  const msg = s.reset_msg ? '<div class="banner">' + esc(s.reset_msg) + '</div>' : '';
  if (s.setup_open) {
    A.innerHTML = msg + '<h2>' + (s.password_set ? 'Set a new admin password' : 'First-time setup') + '</h2><div class="panel"><h3>Choose an admin password</h3>' +
      '<p class="note" style="margin-top:0">Write it down and keep it safe. It cannot be shown again.</p>' +
      '<label>New password (8–64 characters)<input type="password" id="pw1" autocomplete="new-password"></label>' +
      '<label>Repeat password<input type="password" id="pw2" autocomplete="new-password"></label>' +
      '<div class="actions"><button id="setup" class="primary">Save password</button><span class="status" id="setup_st"></span></div></div>';
    $('#setup').onclick = async () => {
      if ($('#pw1').value !== $('#pw2').value) return setStatus($('#setup_st'), 'The two passwords do not match', 'err');
      const r = await act($('#setup'), $('#setup_st'), 'Saving…', () => api('/api/setup', {password: $('#pw1').value}), 'Saved');
      if (r._ok) { toast(REMIND); loadAdmin(); }
    };
    return;
  }
  A.innerHTML = msg + '<h2>Admin</h2><div class="panel"><h3>Log in</h3>' +
    (s.locked ? '<div class="banner">Too many wrong attempts. Try again in a few minutes.</div>' : '') +
    '<label>Password<input type="password" id="pw" autocomplete="current-password"></label>' +
    '<div class="actions"><button id="login" class="primary">Log in</button><span class="status" id="login_st"></span></div>' +
    '<p class="note">Forgot it? Hold the station button for 10 seconds. This is allowed once every 30 days.</p></div>';
  $('#login').onclick = async () => {
    const r = await act($('#login'), $('#login_st'), 'Checking…', () => api('/api/login', {password: $('#pw').value}), 'Logged in');
    if (r._ok) { toast(REMIND); loadAdmin(); }
  };
  $('#pw').onkeydown = e => { if (e.key === 'Enter') $('#login').click(); };
}

const CF = [
  ['interval_s', 'Log interval (seconds, 60–86400)'],
  ['window_s', 'Wind measurement window (seconds, 3–60)'],
  ['wind_k', 'Anemometer: m/s per pulse per second (calibrate)'],
  ['rain_mm_tip', 'Rain gauge: mm per bucket tip (calibrate)'],
  ['vane_off', 'Wind vane offset (degrees)'],
  ['altitude_m', 'Altitude in metres (0 = log station pressure)'],
  ['tz_min', 'UTC offset in minutes (Philippines = 480)'],
  ['session_s', 'Wi-Fi turns off after (seconds idle)'],
  ['ap_ssid', 'Station Wi-Fi name', 'text']
];
const panel = (id, n, title, body) => '<div class="panel" id="' + id + '"><h3><span class="n">' + n + '</span>' + title + '</h3>' + body + '</div>';

async function renderAdmin() {
  const A = $('#admin');
  A.innerHTML = '<p class="status busy">Loading settings…</p>';
  const c = await api('/api/config');
  if (!c._ok) { A.innerHTML = '<p class="status err">' + esc(c.error || 'Could not load settings') + '</p>'; return; }
  const batt = c.power_mode === 1;
  A.innerHTML =
    '<div id="adminbar"><b>Admin</b><span class="status">Ready</span></div>' +
    '<nav class="toc"><a href="#a_wifi">1 Wi-Fi</a><a href="#a_clock">2 Clock</a><a href="#a_loc">3 Location</a><a href="#a_set">4 Settings</a><a href="#a_up">5 Upload</a><a href="#a_pw">6 Password</a></nav>' +
    panel('a_wifi', 1, 'Wi-Fi network (optional)',
      '<p id="netinfo" class="muted"></p>' +
      '<div class="actions"><button id="scan">Scan networks</button><span class="status" id="scan_st"></span></div>' +
      '<label>Network<select id="ssidsel"><option value="">(scan first, or type below)</option></select></label>' +
      '<label>Or type the network name<input type="text" id="ssid"></label>' +
      '<label>Password<input type="password" id="wpass" autocomplete="off"></label>' +
      '<div class="actions"><button id="wsave" class="primary">Save network</button><button id="wforget">Forget network</button><span class="status" id="wifi_st"></span></div>' +
      '<p class="note">The station joins this network when its Wi-Fi is on, to set its clock and to upload. Its own Wi-Fi keeps working.</p>') +
    panel('a_clock', 2, 'Clock',
      '<p class="muted" id="clk"></p><div class="actions"><button id="settime" class="primary">Set time from this device</button><span class="status" id="clk_st"></span></div>') +
    panel('a_loc', 3, 'Station location (GPS)',
      '<label>Paste from Google Maps, on any phone or computer<input type="text" id="loc_paste" autocomplete="off" placeholder="14.5995, 120.9842"></label>' +
      '<div class="row"><label>Latitude<input type="number" step="any" min="-90" max="90" id="loc_lat" value="' + esc(c.lat ?? '') + '"></label>' +
      '<label>Longitude<input type="number" step="any" min="-180" max="180" id="loc_lon" value="' + esc(c.lon ?? '') + '"></label></div>' +
      '<p id="loc_link" class="muted"></p>' +
      '<div class="actions"><button id="loc_save" class="primary">Save location</button><button id="loc_gps">Use this device\'s location</button><button id="loc_clear">Clear</button></div>' +
      '<p class="status" id="loc_status"></p>' +
      '<p class="note">Shown on the Live page, and added to every download and upload, so shared data lands on the right spot on a map.</p>') +
    panel('a_set', 4, 'Station settings', '<form id="cform">' +
      '<label>Power source<select name="power_mode"><option value="0"' + (batt ? '' : ' selected') + '>USB adapter, power bank or mini UPS</option>' +
      '<option value="1"' + (batt ? ' selected' : '') + '>Battery</option></select></label>' +
      CF.map(f => '<label>' + f[1] + '<input type="' + (f[2] || 'number') + '" step="any" name="' + f[0] + '" value="' + esc(c[f[0]]) + '"></label>').join('') +
      '<label>Station Wi-Fi password <span class="opt">leave blank to keep; 8–63 characters</span><input type="password" name="ap_pass" autocomplete="new-password"></label>' +
      '<div class="actions"><button type="submit" class="primary">Save settings</button><span class="status" id="set_st"></span></div></form>') +
    panel('a_up', 5, 'Remote upload (optional)', '<form id="uform">' +
      '<label class="inl"><input type="checkbox" name="up_en"' + (c.up_en ? ' checked' : '') + '> Upload enabled</label>' +
      '<label>HTTPS address<input type="url" name="up_url" value="' + esc(c.up_url) + '" placeholder="https://script.google.com/macros/s/…/exec?station=hoa1&key=…"></label>' +
      '<label>Authorization token <span class="opt">' + (c.up_tok_set ? 'saved; leave blank to keep' : 'none saved') + '</span><input type="password" name="up_tok" autocomplete="off"></label>' +
      '<label class="inl"><input type="checkbox" name="clear_tok"> Remove saved token</label>' +
      '<label>Upload every (minutes, 5–1440)<input type="number" name="up_int_min" value="' + esc(c.up_int_min) + '"></label>' +
      '<label class="inl"><input type="checkbox" name="up_insecure"' + (c.up_insecure ? ' checked' : '') + '> Skip certificate check (testing only)</label>' +
      '<div class="actions"><button type="submit" class="primary">Save upload settings</button><span class="status" id="up_st"></span></div></form>' +
      '<label>Root certificate (PEM) <span class="opt">' + (c.ca_set ? 'saved' : 'none saved') + '</span><textarea id="pem" class="mono" placeholder="-----BEGIN CERTIFICATE-----"></textarea></label>' +
      '<div class="actions"><button id="casave">Save certificate</button><button id="uptest">Test connection</button><button id="upnow">Upload now</button></div>' +
      '<p id="upstat" class="status"></p>' +
      '<p class="note">The token is stored unencrypted on the station. Use one that can only write to this one address.</p>') +
    panel('a_pw', 6, 'Admin password',
      '<label>Current password<input type="password" id="op" autocomplete="current-password"></label>' +
      '<label>New password (8–64)<input type="password" id="np" autocomplete="new-password"></label>' +
      '<div class="actions"><button id="pwsave" class="primary">Change password</button><span class="status" id="pw_st"></span></div>') +
    '<div class="actions"><button id="sleepnow">Turn Wi-Fi off now</button><button id="logout">Log out</button><span class="status" id="end_st"></span></div>';

  // ---- Wi-Fi
  const refreshNet = async () => {
    const n = await api('/api/net', undefined, 8000);
    if (!$('#netinfo') || !n._ok) return;
    $('#netinfo').textContent = 'Open this page at http://' + n.host + ' or http://' + (n.ap_ip || '192.168.4.1') + (n.sta_ip ? ' (or http://' + n.sta_ip + ' on the building Wi-Fi)' : '') +
      (n.sta_connected ? '. Connected to "' + n.ssid + '" (' + n.rssi + ' dBm).' : n.saved_ssid ? '. Saved network "' + n.saved_ssid + '" is not connected.' : '. No network saved.');
  };
  refreshNet(); const nt = setInterval(() => { if (tab !== 'admin' || !$('#netinfo')) return clearInterval(nt); refreshNet(); }, 8000);
  $('#scan').onclick = async () => {
    const b = $('#scan'); b.disabled = true; b.textContent = 'Scanning…';
    setStatus($('#scan_st'), 'Scanning for networks, about 10 seconds…', 'busy');
    await api('/api/wifi/scan?start=1');
    let found = null;
    for (let i = 0; i < 20 && !found; i++) {
      await new Promise(r => setTimeout(r, 1500));
      const r = await api('/api/wifi/scan');
      if (r._ok && !r.scanning) found = r;
    }
    b.disabled = false; b.textContent = 'Scan networks';
    if (!found) return setStatus($('#scan_st'), 'No answer from the scan. Try again.', 'err');
    const nets = (found.networks || []).sort((a, b) => b.rssi - a.rssi);
    $('#ssidsel').innerHTML = '<option value="">Select…</option>' + nets.map(n => '<option value="' + esc(n.ssid) + '">' + esc(n.ssid || '(hidden)') + ' (' + n.rssi + ' dBm' + (n.open ? ', open' : '') + ')</option>').join('');
    setStatus($('#scan_st'), nets.length + ' networks found', 'ok');
  };
  $('#ssidsel').onchange = e => { $('#ssid').value = e.target.value; };
  $('#wsave').onclick = async () => {
    const ssid = $('#ssid').value || $('#ssidsel').value;
    if (!ssid) return setStatus($('#wifi_st'), 'Choose or type a network', 'err');
    const r = await act($('#wsave'), $('#wifi_st'), 'Saving…', () => api('/api/wifi/save', {ssid, pass: $('#wpass').value}), 'Saved, connecting');
    if (r._ok) setTimeout(refreshNet, 5000);
  };
  $('#wforget').onclick = async () => { await act($('#wforget'), $('#wifi_st'), 'Forgetting…', () => api('/api/wifi/save', {ssid: '', pass: ''}), 'Network forgotten'); refreshNet(); };

  // ---- clock
  const clk = async () => { const l = await api('/api/live', undefined, 8000); $('#clk') && l._ok && ($('#clk').textContent = 'Station clock: ' + new Date(l.time * 1000).toLocaleString() + (l.time_trusted ? '' : '  (not set)')); };
  clk();
  $('#settime').onclick = async () => {
    await act($('#settime'), $('#clk_st'), 'Sending…', () => api('/api/time', {epoch: Math.floor(Date.now() / 1000)}), 'Time set');
    setTimeout(clk, 1500);
  };

  // ---- location
  bindLocation();

  // ---- settings
  $('#cform').onsubmit = async e => {
    e.preventDefault();
    const d = {};
    new FormData(e.target).forEach((v, k) => { if (v !== '') d[k] = v; });
    await act(e.target.querySelector('button'), $('#set_st'), 'Saving…', () => api('/api/config', d), 'Settings saved');
  };
  $('#uform').onsubmit = async e => {
    e.preventDefault();
    const f = e.target, d = {
      up_en: f.up_en.checked ? '1' : '0', up_url: f.up_url.value, up_int_min: f.up_int_min.value,
      up_insecure: f.up_insecure.checked ? '1' : '0', clear_tok: f.clear_tok.checked ? '1' : '0'
    };
    if (f.up_tok.value) d.up_tok = f.up_tok.value;
    await act(f.querySelector('button'), $('#up_st'), 'Saving…', () => api('/api/config', d), 'Upload settings saved');
  };
  $('#casave').onclick = () => act($('#casave'), $('#upstat'), 'Saving…', () => api('/api/upload/ca', {pem: $('#pem').value}), 'Certificate saved');
  const upLine = s => 'Upload: ' + (s.status || '?') + (s.sync_file ? ' · sent up to ' + s.sync_file + ' @ ' + s.sync_offset + ' bytes' : '');
  const pollUp = async (btn, label) => {
    btn.disabled = true; btn.textContent = label;
    setStatus($('#upstat'), 'Connecting to the building Wi-Fi and the server. This can take up to a minute…', 'busy');
    for (let i = 0; i < 40; i++) {
      await new Promise(r => setTimeout(r, 1500));
      const s = await api('/api/upload/status', undefined, 8000);
      if (!$('#upstat')) return;
      if (s._ok && s.status && !/\.\.\.$/.test(s.status)) { setStatus($('#upstat'), upLine(s), /fail|no |not|bad|skip/i.test(s.status) ? 'err' : 'ok'); break; }
      if (i === 39) setStatus($('#upstat'), 'Still working on it. Check again in a minute.', 'err');
    }
    btn.disabled = false; btn.textContent = btn.id === 'uptest' ? 'Test connection' : 'Upload now';
  };
  $('#uptest').onclick = async () => { await api('/api/upload/test', null); pollUp($('#uptest'), 'Testing…'); };
  $('#upnow').onclick = async () => { await api('/api/upload/now', null); pollUp($('#upnow'), 'Uploading…'); };
  api('/api/upload/status', undefined, 8000).then(s => { const e = $('#upstat'); if (e && s._ok) { e.textContent = upLine(s); e.className = 'status'; } });

  // ---- password / session
  $('#pwsave').onclick = async () => {
    const r = await act($('#pwsave'), $('#pw_st'), 'Saving…', () => api('/api/password', {old: $('#op').value, new: $('#np').value}), 'Password changed');
    if (r._ok) { $('#op').value = ''; $('#np').value = ''; toast(REMIND); }
  };
  $('#sleepnow').onclick = async () => {
    await act($('#sleepnow'), $('#end_st'), 'Turning off…', () => api('/api/sleep', null), 'Wi-Fi is turning off. Press the station button to wake it');
  };
  $('#logout').onclick = async () => { await api('/api/logout', null); isAdmin = false; clearInterval(reminderTimer); loadAdmin(); };
}

pollLive();
