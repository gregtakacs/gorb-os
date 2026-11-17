#!/usr/bin/env node
// library-live.js — L1 hub step 4's browser gate: the studio's library pickers, driven in a
// real Chromium over the release's own layout.
//
//   node library-live.js --runtime <dir> --editors <dir> [--chrome <path>] [--only <slug>] [--shots <dir>]
//
// <runtime> and <editors> are the same extracted trees `editors-smoke.js` gets from the
// `make web` tarball: an `export-runtime` tree and an `extract-editors` tree. Its stand-in
// site is orb-os-net's: GORB_HOST is `{ kind: "web", api: "/studio-api", send: false,
// publish: false, inbox: false, assets: true, designIds: true, library: true }`, and the
// studio origin answers `GET /studio-api/assets` with two real LibraryAssets (§2.3 of
// l1-design): a 64 × 64 PNG a clock design named Brass holds, and a 1.0 s sound a pomodoro
// design named Desk holds — both made here from real bytes, the sound through the release's
// own baker. `GET /studio-api/asset?sha256=` answers their bytes or 404s; `POST
// /studio-api/design/save` records what the editor posted and answers as the site would.
//
// What it checks, in the owner's own flow: under the site, an editor offers NO file input
// and a "Your library…" button instead (clock, flights, weather, pomodoro); a flights
// picture row's picker lists the clock's picture — thumbnail, `In Brass` and all — and not
// the pomodoro's sound; picking it and pressing Save to library stores NOTHING NEW: the
// save names that one sha256 and carries no other picture. A sound row offers only sounds.
// Under a linked Orb Manager (`{ kind: "hub", library: true }`) a row shows both the file
// input and the button; with no host description at all it is exactly today's row. The
// flights picker is screenshotted at 1280 × 800 for the owner's review.
//
// It is editors-smoke.js's sibling and copies its server and inject idiom (a shared module
// would mean touching editors-smoke, which this step must not); the driving is over CDP in
// rename-live.js's idiom — one headless Chromium, one tab per case, real button presses.
// `make check-web` runs it right after editors-smoke.js, on the same trees.
'use strict';
const fs = require('fs');
const http = require('http');
const os = require('os');
const path = require('path');
const crypto = require('crypto');
const zlib = require('zlib');
const { spawn } = require('child_process');

const TYPES = {
  '.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8', '.json': 'application/json; charset=utf-8',
  '.woff2': 'font/woff2', '.ttf': 'font/ttf', '.png': 'image/png', '.svg': 'image/svg+xml',
};
const typeOf = (f) => TYPES[path.extname(f).toLowerCase()] || 'application/octet-stream';
const IP = '192.0.2.7'; // the hub's address as it appears in an editor URL: TEST-NET-1

function arg(name, fallback) {
  const i = process.argv.indexOf('--' + name);
  return i > 0 && process.argv[i + 1] ? process.argv[i + 1] : fallback;
}
const RUNTIME = path.resolve(arg('runtime', '') || '');
const EDITORS = path.resolve(arg('editors', '') || '');
const ONLY = arg('only', null);
const SHOTS = arg('shots', null);
if (!RUNTIME || !EDITORS) {
  console.log('usage: library-live.js --runtime <export-runtime dir> --editors <extract-editors dir> [--chrome <path>] [--only <slug>] [--shots <dir>]');
  process.exit(2);
}
if (!fs.existsSync(path.join(EDITORS, 'editors.json'))) {
  console.log('no editors.json in ' + EDITORS + ' — run orb-manager extract-editors on the release file first');
  process.exit(2);
}

// the browser: --chrome, then $CHROME, then the names a Linux box answers to.
function chromePath() {
  const named = arg('chrome', process.env.CHROME || '');
  if (named) return fs.existsSync(named) ? named : null;
  for (const c of ['chromium', 'chromium-browser', 'google-chrome']) {
    for (const dir of (process.env.PATH || '').split(path.delimiter)) {
      if (dir && fs.existsSync(path.join(dir, c))) return path.join(dir, c);
    }
  }
  return null;
}
const CHROME = chromePath();
if (!CHROME) {
  console.log('no Chromium: pass --chrome <path>, set CHROME, or put chromium, chromium-browser or google-chrome on PATH');
  process.exit(2);
}

const fakeHub = require(path.join(__dirname, 'fake-hub.js'));
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
async function waitFor(what, ms, fn) {
  const t0 = Date.now();
  for (;;) {
    let v = null;
    try { v = await fn(); } catch (e) { /* the page may not answer yet */ }
    if (v) return v;
    if (Date.now() - t0 > ms) throw new Error('timed out waiting for ' + what);
    await sleep(120);
  }
}
/* ---- the library the stand-in site holds: two assets, every field of l1-design §2.3 ---- */
// A 64 × 64 RGBA PNG, written here from a deterministic noisy gradient: real bytes a browser
// decodes, real pixels the thumbnail shows, and well over 512 B of payload so the session
// treats it as the art-by-hash asset it is (session.js's assetOf floor).
function crc32(buf) {
  let c, t = crc32.t;
  if (!t) t = crc32.t = (() => { const a = new Int32Array(256); for (let n = 0; n < 256; n++) { c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; a[n] = c; } return a; })();
  c = -1;
  for (let i = 0; i < buf.length; i++) c = t[(c ^ buf[i]) & 0xff] ^ (c >>> 8);
  return (c ^ -1) >>> 0;
}
function makePng64() {
  const W = 64, H = 64, raw = Buffer.alloc(H * (1 + W * 4));
  let seed = 0xb105;
  for (let y = 0; y < H; y++) {
    const o = y * (1 + W * 4);
    raw[o] = 0;                                          // filter: none
    for (let x = 0; x < W; x++) {
      seed = (seed * 1103515245 + 12345) & 0x7fffffff;    // deterministic noise over a gradient
      const p = o + 1 + x * 4;
      raw[p] = (x * 4 + (seed & 31)) & 255;
      raw[p + 1] = (y * 4 + (seed >> 5 & 31)) & 255;
      raw[p + 2] = (200 - ((x + y) & 64) - (seed >> 9 & 15)) & 255;
      raw[p + 3] = 255;
    }
  }
  const chunk = (type, data) => {
    const len = Buffer.alloc(4); len.writeUInt32BE(data.length);
    const body = Buffer.concat([Buffer.from(type, 'ascii'), data]);
    const crc = Buffer.alloc(4); crc.writeUInt32BE(crc32(body));
    return Buffer.concat([len, body, crc]);
  };
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(W, 0); ihdr.writeUInt32BE(H, 4);
  ihdr[8] = 8; ihdr[9] = 6;                              // 8-bit RGBA
  return Buffer.concat([Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    chunk('IHDR', ihdr), chunk('IDAT', zlib.deflateSync(raw, { level: 9 })), chunk('IEND', Buffer.alloc(0))]);
}
const PNG_A = makePng64();
const SHA_A = crypto.createHash('sha256').update(PNG_A).digest('hex');
// the sound: ONE SECOND of a decaying tone, encoded by the release's own baker — the same
// bytes an editor would store, so the site's hash rule holds for it too.
const GorbBake = require(path.join(RUNTIME, 'baker@1', 'orbtheme-baker.js'));
const SND_URL = GorbBake.soundFileUrl(Int16Array.from({ length: 22050 }, (_, i) =>
  Math.round(11000 * Math.sin(2 * Math.PI * 440 * i / 22050) * Math.exp(-3 * i / 22050))));
const SND_BYTES = Buffer.from(SND_URL.slice(SND_URL.indexOf(',') + 1), 'base64');
const SHA_B = crypto.createHash('sha256').update(SND_BYTES).digest('hex');
const A = {
  sha256: SHA_A, kind: 'picture', mediaType: 'image/png', prefix: 'data:image/png;base64',
  size: PNG_A.length, name: 'Sunrise', width: 64, height: 64, seconds: null,
  uploadedAt: null, usedAt: '2026-11-16T09:00:00.000Z',
  designs: [{ id: 'clockdesign0000000000a', app: 'clock', name: 'Brass', current: true }],
};
const B = {
  sha256: SHA_B, kind: 'sound', mediaType: 'audio/wav', prefix: SND_URL.slice(0, SND_URL.indexOf(',')),
  size: SND_BYTES.length, name: 'Desk bell', width: null, height: null, seconds: 1.0,
  uploadedAt: '2026-11-15T10:00:00.000Z', usedAt: '2026-11-15T10:00:00.000Z',
  designs: [{ id: 'pomodesign000000000b', app: 'pomodoro', name: 'Desk', current: true }],
};

/* ---- the stand-in site: the editor trees, the runtime, and the studio origin's doors ---- */
const readTree = (root, rel) => {
  if (!rel || rel.split('/').some((s) => s === '' || s === '.' || s === '..')) return null;
  const f = path.join(root, rel);
  if (!f.startsWith(root + path.sep)) return null;
  try { return fs.statSync(f).isFile() ? fs.readFileSync(f) : null; } catch (e) { return null; }
};
const send = (res, status, type, body) => { res.writeHead(status, { 'content-type': type, 'cache-control': 'no-store' }); res.end(body); };
const json = (res, status, v) => send(res, status, 'application/json; charset=utf-8', JSON.stringify(v));

const SITE_HOST = { kind: 'web', api: '/studio-api', send: false, publish: false, inbox: false, assets: true, designIds: true, library: true };
const HUB_LINKED = { kind: 'hub', library: true };
let bySlug = {};
let load = null;                     // the page load the server is answering: host, requests, hub
const saves = [];                    // every recorded POST /studio-api/design/save: {url, body}

function entryDocument(slug) {
  const row = bySlug[slug];
  if (!row) return null;
  return readTree(path.join(EDITORS, slug), row.editor.entry);
}
function inject(doc) {
  const tag = load.host === undefined ? '' : '<script>window.GorbHost = ' + JSON.stringify(load.host) + ';</script>';
  const m = /<head[^>]*>/i.exec(doc.toString('utf8'));
  if (!m) return tag + doc.toString('utf8');
  return doc.toString('utf8').slice(0, m.index + m[0].length) + tag + doc.toString('utf8').slice(m.index + m[0].length);
}
// the studio origin's /studio-api/* and the hub's /api/*: the library doors first, everything
// else through the same fake hub editors-smoke.js answers with (its design/save shape).
function apiRoute(res, url, body) {
  const u = new URL(url, 'http://site');
  const p = u.pathname;
  if (p === '/studio-api/assets' || p === '/api/assets') {
    return json(res, 200, { ok: true, data: { assets: [A, B] } });
  }
  if (p === '/studio-api/asset' || p === '/api/asset') {
    const h = u.searchParams.get('sha256');
    if (h === A.sha256) return send(res, 200, 'application/octet-stream', PNG_A);
    if (h === B.sha256) return send(res, 200, 'application/octet-stream', SND_BYTES);
    return json(res, 404, { ok: false, error: 'no such asset' });
  }
  if (reqIsPost(body) && (p === '/studio-api/design/save' || p === '/api/design/save')) {
    saves.push({ url: p + u.search, body: body });
  }
  const hub = load.hub || (load.hub = fakeHub({
    source: { relation: 'same' },
    rest: () => ({ status: 404, json: async () => ({ ok: false, error: 'no route' }) }),
  }));
  return hub.fetch(url.replace(/^\/studio-api/, '/api'), body)
    .then((ans) => ans.json().then((v) => json(res, ans.status, v)));
}
// a POST carries its body text; a GET answers null. Only POSTs reach apiRoute with a string.
const reqIsPost = (body) => typeof body === 'string';

let chromeProc = null, chromeDir = null;
const server = http.createServer((req, res) => {
  const u = new URL(req.url, 'http://127.0.0.1');
  const p = u.pathname;
  if (load) load.requests.push(req.method + ' ' + req.url);
  if (p.startsWith('/runtime/')) {
    const f = readTree(RUNTIME, p.slice('/runtime/'.length));
    return f ? send(res, 200, typeOf(p), f) : send(res, 404, 'text/plain; charset=utf-8', 'not found');
  }
  const hubDir = /^\/editor\/192\.0\.2\.7\/([a-z0-9_-]{1,23})\/(.*)$/.exec(p);
  const webDir = /^\/studio\/([a-z0-9_-]{1,23})\/(.*)$/.exec(p);
  const m = hubDir || webDir;
  if (m && bySlug[m[1]] && req.method === 'GET') {
    const slug = m[1], file = m[2];
    if (file === '' || file === 'editor.json') {
      const doc = entryDocument(slug);
      if (!doc) return send(res, 404, 'text/plain; charset=utf-8', 'not found');
      return send(res, 200, 'text/html; charset=utf-8', inject(doc));
    }
    const f = readTree(path.join(EDITORS, slug), file);
    return f ? send(res, 200, typeOf(file), f) : send(res, 404, 'text/plain; charset=utf-8', 'not found');
  }
  if (p === '/api/orb/known') return json(res, 200, { ok: true, data: { ip: null } });
  if (p === '/api/theme/stem') return json(res, 400, { ok: false, error: 'no stem here' });
  if (req.method === 'POST' && (p.startsWith('/api/') || p.startsWith('/studio-api/'))) {
    let text = '';
    req.on('data', (c) => { text += c; });
    req.on('end', () => Promise.resolve(apiRoute(res, p + (u.search || ''), text)).catch(() => json(res, 500, { ok: false, error: 'stand-in site' })));
    return undefined;
  }
  if (p.startsWith('/api/') || p.startsWith('/studio-api/')) {
    return Promise.resolve(apiRoute(res, p + (u.search || ''), null)).catch(() => json(res, 500, { ok: false, error: 'stand-in site' }));
  }
  return json(res, 404, { ok: false, error: 'no route' });
});
/* ---- one headless Chromium, CDP over the platform WebSocket (rename-live.js's idiom) ---- */
class CDP {
  constructor(url) {
    this.url = url; this.id = 0; this.pending = new Map(); this.listeners = [];
    this.ws = new WebSocket(url);
    this.open = new Promise((res, rej) => { this.ws.onopen = res; this.ws.onerror = () => rej(new Error('CDP socket failed')); });
    this.ws.onmessage = (e) => this._onMessage(JSON.parse(e.data));
  }
  _onMessage(m) {
    if (m.id && this.pending.has(m.id)) {
      const p = this.pending.get(m.id); this.pending.delete(m.id);
      if (m.error) p.reject(new Error(m.method + ': ' + m.error.message)); else p.resolve(m.result);
      return;
    }
    for (const fn of this.listeners) fn(m);
  }
  send(method, params, sessionId) {
    const id = ++this.id;
    return new Promise((res, rej) => {
      this.pending.set(id, { resolve: res, reject: rej });
      this.ws.send(JSON.stringify({ id, method, params: params || {}, sessionId }));
    });
  }
  on(fn) { this.listeners.push(fn); }
  close() { try { this.ws.close(); } catch (e) { /* gone */ } }
}

const sessions = new Map();   // sessionId -> {type, url}
let cdp = null;
function attachEvents() {
  cdp.on((m) => {
    if (m.method === 'Target.attachedToTarget') {
      const { sessionId, targetInfo } = m.params;
      sessions.set(sessionId, { type: targetInfo.type, url: targetInfo.url, targetId: targetInfo.targetId });
      cdp.send('Runtime.enable', {}, sessionId).catch(() => {});
    } else if (m.method === 'Target.detachedFromTarget') {
      sessions.delete(m.params.sessionId);
    } else if (m.method === 'Target.targetInfoChanged') {
      const info = sessions.get(m.params.sessionId);
      if (info && m.params.targetInfo) { info.url = m.params.targetInfo.url; info.type = m.params.targetInfo.type; }
    }
  });
}
async function launchChromium() {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'orb-library-live-'));
  const args = ['--headless=new', '--no-sandbox', '--disable-gpu', '--disable-dev-shm-usage',
    '--disable-background-networking', '--disable-component-update', '--no-first-run',
    '--disable-extensions', '--mute-audio', '--user-data-dir=' + dir, '--remote-debugging-port=0'];
  const proc = spawn(CHROME, args, { stdio: ['ignore', 'ignore', 'ignore'], detached: true });
  try {
    const port = Number(await waitFor('DevToolsActivePort', 25000, async () => {
      try { const s = fs.readFileSync(path.join(dir, 'DevToolsActivePort'), 'utf8').split('\n')[0].trim(); return /^\d+$/.test(s) ? s : null; } catch (e) { return null; }
    }));
    const ver = await (await fetch('http://127.0.0.1:' + port + '/json/version')).json();
    cdp = new CDP(ver.webSocketDebuggerUrl);
    await cdp.open;
    attachEvents();
    await cdp.send('Target.setAutoAttach', { autoAttach: true, waitForDebuggerOnStart: false, flatten: true });
    return { proc, dir };
  } catch (e) {
    try { process.kill(-proc.pid, 'SIGKILL'); } catch (e2) { /* gone */ }
    fs.rmSync(dir, { recursive: true, force: true });
    throw e;
  }
}
function shutdown(proc, dir) {
  try { cdp && cdp.close(); } catch (e) { /* gone */ }
  try { if (proc) process.kill(-proc.pid, 'SIGKILL'); } catch (e) { /* reaped */ }
  fs.rmSync(dir, { recursive: true, force: true });
}

// one tab, at the site's window size, with its document driven by plain page expressions.
async function openTab(url) {
  const t = await cdp.send('Target.createTarget', { url: 'about:blank' });
  const sid = await waitFor('tab session for target ' + t.targetId, 30000, async () => {
    for (const [s, info] of sessions) if (info.targetId === t.targetId) return s;
    return null;
  });
  await cdp.send('Page.enable', {}, sid);
  await cdp.send('Page.addScriptToEvaluateOnNewDocument', {
    source: "window.__ERRS=[];window.addEventListener('error',function(e){if(window.__ERRS.length<20)window.__ERRS.push(String(e&&e.message||e));});"
      + "window.addEventListener('unhandledrejection',function(e){if(window.__ERRS.length<20)window.__ERRS.push('rejected: '+((e&&e.reason&&(e.reason.message||e.reason))||'?'));});",
  }, sid);
  await cdp.send('Emulation.setDeviceMetricsOverride', { width: 1280, height: 800, deviceScaleFactor: 1, mobile: false }, sid);
  await cdp.send('Page.navigate', { url }, sid);
  await waitFor('the page at ' + url, 30000, async () =>
    (await evalIn(sid, 'location.href')) === url ? true : null);
  return { targetId: t.targetId, sid };
}
async function closeTab(tab) {
  try { await cdp.send('Target.closeTarget', { targetId: tab.targetId }); } catch (e) { /* gone */ }
  await sleep(150);
}
function evalIn(sid, expression) {
  return cdp.send('Runtime.evaluate', { expression, awaitPromise: true, returnByValue: true }, sid)
    .then((r) => {
      if (r.exceptionDetails) throw new Error('page threw: ' + String((r.exceptionDetails.exception && r.exceptionDetails.exception.description) || r.exceptionDetails.text).slice(0, 200));
      return r.result.value;
    });
}

/* ---- the page side: presses and reads, in the studio's own words and classes ---- */
const J = (v) => JSON.stringify(v);
const EX_NEW = "(function(){var c=document.querySelectorAll('button.st-choice');"
  + "for(var i=0;i<c.length;i++){var b=c[i].getElementsByTagName('b')[0];"
  + "if(b&&(b.textContent||'').trim()==='New'){c[i].click();return true;}}return false;})()";
const exStep = (label) => "(function(){var b=[].slice.call(document.querySelectorAll('.st-steps button'))"
  + ".find(function(x){return (x.textContent||'').trim()===" + J(label) + ";});"
  + "if(!b)return false;b.click();return true;})()";
const exChip = (label) => "(function(){var b=[].slice.call(document.querySelectorAll('button'))"
  + ".filter(function(x){return !x.closest('.st-steps');})"
  + ".find(function(x){return (x.textContent||'').trim()===" + J(label) + ";});"
  + "if(!b)return false;b.click();return true;})()";
// a row whose pick is too long for chips is a <select> (studio.js's Ch): choose the option by
// its label, the way a person does, and let React's own change handler take it.
const exSelect = (rowLabel, optionText) => "(function(){var r=[].slice.call(document.querySelectorAll('.st-row'))"
  + ".find(function(x){var l=x.querySelector('label');return l&&(l.textContent||'').trim().indexOf(" + J(rowLabel) + ")===0;});"
  + "if(!r)return 'no row labelled " + rowLabel + "';var s=r.querySelector('select');if(!s)return 'that row is not a pick list';"
  + "var o=[].slice.call(s.options).find(function(x){return (x.textContent||'').trim().indexOf(" + J(optionText) + ")>=0;});"
  + "if(!o)return 'no option naming " + optionText + "';s.value=o.value;"
  + "s.dispatchEvent(new Event('change',{bubbles:true}));return true;})()";
const EX_LIB_BTNS = "(function(){return [].slice.call(document.querySelectorAll('button'))"
  + ".filter(function(b){return (b.textContent||'').trim()===" + J('Your library…') + ";}).length;})()";
const EX_FILE_INPUTS = "(function(){return JSON.stringify([].slice.call(document.querySelectorAll('input[type=file]'))"
  + ".map(function(i){return i.accept||'';}));})()";
const exOpenPicker = "(function(){var bs=[].slice.call(document.querySelectorAll('button'))"
  + ".filter(function(b){return (b.textContent||'').trim()===" + J('Your library…') + ";});"
  + "if(!bs.length)return false;bs[0].click();return true;})()";
// the picker as the owner sees it: its title, its rows with the thumbnail's rendered width,
// and its whole text — what a row NAMES is what these checks read.
const EX_PICKER = "(function(){var m=document.querySelector('.st-modal .st-lib');if(!m)return null;"
  + "var t=m.querySelector('.st-mtitle');var rows=[].slice.call(m.querySelectorAll('.st-lib-a')).map(function(r){"
  + "var im=r.querySelector('img.st-lib-th');var rc=im?im.getBoundingClientRect():null;"
  + "return {text:(r.textContent||'').replace(/\\s+/g,' ').trim(),thumb:im?(im.getAttribute('src')||'').slice(0,32):null,"
  + "tw:rc?Math.round(rc.width):null,th:rc?Math.round(rc.height):null};});"
  + "return {title:t?(t.textContent||'').trim():null,rows:rows,play:!!m.querySelector('.st-lib-play'),"
  + "text:(m.textContent||'').replace(/\\s+/g,' ').trim()};})()";
const exPickRow = (want) => "(function(){var m=document.querySelector('.st-modal .st-lib');if(!m)return false;"
  + "var r=[].slice.call(m.querySelectorAll('.st-lib-a')).find(function(x){return (x.textContent||'').indexOf(" + J(want) + ")>=0;});"
  + "if(!r)return false;r.click();return true;})()";
const EX_CLOSED = "(function(){return !document.querySelector('.st-modal .st-lib');})()";
const exPress = (label) => "(function(){var b=[].slice.call(document.querySelectorAll('button'))"
  + ".find(function(x){return (x.textContent||'').indexOf(" + J(label) + ")>=0;});"
  + "if(!b)return false;b.click();return true;})()";
// the rename sheet fu2-4 puts in front of a save whose name needs it: accept it and save.
const EX_RENAME = "(function(){var m=document.querySelector('.st-rename');if(!m)return false;"
  + "var b=[].slice.call(m.querySelectorAll('button')).find(function(x){return (x.textContent||'').indexOf('Rename and save')>=0;});"
  + "if(!b||b.disabled)return false;b.click();return true;})()";

/* ---- the checks ---- */
let fails = 0, checked = 0;
function ok(cond, name, seen) {
  checked++;
  if (cond) console.log('PASS ' + name);
  else { fails++; console.log('FAIL ' + name + (seen === undefined ? '' : ': ' + String(seen).slice(0, 300))); }
  return !!cond;
}
// one check that runs a flow: it passes when the flow completed and every `want` held, and the
// FAIL line says where it stopped.
async function check(name, fn) {
  try { await fn(); ok(true, name); }
  catch (e) { ok(false, name, (e && e.message) || e); }
}
const want = (cond, why) => { if (!cond) throw new Error(why); };

// the file inputs a page offers, minus the StartFlow design Import (owner question 6: that
// one opens a whole .orbtheme/.orbdesign, not an asset, and stays).
const importish = (accept) => /orbtheme|orbdesign/i.test(String(accept));

/* what the recorded save says about pictures: every image data URL decoded and hashed, every
   orb-asset ref and orbAssets sidecar entry collected, for "names A and carries nothing else".
   One string is set aside: `saved.extra.thumb`, the entry's card thumbnail, which the session
   redraws from the design for the host's shelf and re-encodes, so it never hashes to the file
   the design holds. Everything inside the design itself is read as it stands. */
function analyseSave(rec) {
  const body = JSON.parse(rec.body);
  const thumb = body && body.saved && body.saved.extra && body.saved.extra.thumb;
  if (body && body.saved && body.saved.extra) delete body.saved.extra.thumb;
  const strs = [], where = new Map();
  (function walk(v, at) {
    if (Array.isArray(v)) { v.forEach((x, i) => walk(x, at + '[' + i + ']')); return; }
    if (v && typeof v === 'object') { for (const k in v) if (Object.prototype.hasOwnProperty.call(v, k)) walk(v[k], at ? at + '.' + k : k); return; }
    if (typeof v === 'string') { strs.push(v); if (!where.has(v)) where.set(v, at); }
  })(body, '');
  const refs = new Set(), inline = [], kinds = {};
  for (const s of strs) {
    const r = /^orb-asset:([0-9a-f]{64})$/.exec(s);
    if (r) { refs.add(r[1]); continue; }
    if (/^data:image\//.test(s)) {
      const c = s.indexOf(',');
      kinds[s.slice(5, c)] = (kinds[s.slice(5, c)] || 0) + 1;
      let h = 'undecodable';
      try { h = crypto.createHash('sha256').update(Buffer.from(s.slice(c + 1), 'base64')).digest('hex'); } catch (e) { /* keep the marker */ }
      inline.push(h);
      where.set(h, where.get(s) || '?');
    }
  }
  const side = (Array.isArray(body.orbAssets) ? body.orbAssets : []).map((e) => e && e.sha256).filter(Boolean);
  return { refs, inline, side, kinds, where, bytes: rec.body.length, url: rec.url };
}

// how each editor shows a picture or sound picker: the step tab to press, plus whatever the
// row hides behind — a chip to press (clock's skeleton dial wants `Put the dial back`) or a
// pick-list option (flights' planes row: Family → Your own).
const WEB_CASES = [
  { slug: 'clock', step: 'Dial', chips: ['Put the dial back'], gate: 'lib' },
  { slug: 'flights', step: 'Planes', selects: [['Family', 'Your own']], gate: 'lib' },
  { slug: 'weather', step: 'Ground', gate: 'lib' },
  { slug: 'pomodoro', step: 'Chimes', gate: 'lib' },
];
// the same clock row under an Orb Manager: linked it shows the file input AND the button,
// and with no host description at all it is exactly the row this editor has always drawn.
const HUB_CASES = [
  { slug: 'clock', step: 'Dial', chips: ['Put the dial back'], gate: 'file',
    name: 'a linked Orb Manager shows both: clock', host: HUB_LINKED, libWant: true },
  { slug: 'clock', step: 'Dial', chips: ['Put the dial back'], gate: 'file',
    name: 'an unlinked Orb Manager is unchanged: clock', host: undefined, libWant: false },
];

// open an editor under `host` at `base`, pass its start sheet, walk to a step with a file row
// on it, press any chips the row hides behind, and wait until `gate` says the row is on
// screen. Returns the live tab.
async function openEditor(host, base, c, box) {
  load = { requests: [], hub: null, host: host };
  const tab = await openTab('http://127.0.0.1:' + server.address().port + base + c.slug + '/');
  if (box) box.tab = tab;
  await waitFor(c.slug + "'s start sheet", 60000, async () => await evalIn(tab.sid, EX_NEW) === true);
  await waitFor(c.slug + "'s step tab " + c.step, 30000, async () => await evalIn(tab.sid, exStep(c.step)) === true);
  for (const chip of c.chips || []) await waitFor(c.slug + "'s chip " + chip, 30000, async () => await evalIn(tab.sid, exChip(chip)) === true);
  for (const [row, option] of c.selects || []) {
    const why = await waitFor(c.slug + "'s " + row + ' = ' + option, 30000,
      async () => { const r = await evalIn(tab.sid, exSelect(row, option)); return r === true ? true : String(r); });
    want(why === true, c.slug + ': ' + why);
  }
  const gate = c.gate === 'file' ? gateFileIn : gateLibBtn;
  await waitFor(c.slug + "'s picker row", 30000, async () => await gate(tab.sid) ? true : null);
  return tab;
}
const gateLibBtn = async (sid) => (await evalIn(sid, EX_LIB_BTNS)) > 0;
const gateFileIn = async (sid) => {
  const a = await fileInputAccepts(sid);
  return !!a && a.some((x) => !importish(x));
};
async function fileInputAccepts(sid) {
  try { return JSON.parse(await evalIn(sid, EX_FILE_INPUTS)); } catch (e) { return null; }
}
/* ---- the run: one server, one Chromium, the cases in order ---- */
(async () => {
  const report = JSON.parse(fs.readFileSync(path.join(EDITORS, 'editors.json'), 'utf8'));
  const rows = (report.apps || []).filter((a) => a.status === 'ok');
  bySlug = {};
  for (const r of rows) bySlug[r.slug] = r;
  const wanted = (s) => !ONLY || ONLY === s;
  await new Promise((res) => server.listen(0, '127.0.0.1', res));
  const chrome = await launchChromium();
  chromeProc = chrome.proc; chromeDir = chrome.dir;
  const webNames = (slug) => ["the site's pickers offer no file input: " + slug].concat(
    slug === 'flights' ? ['the picker opens on the library: flights', 'a clock picture picked in flights stores nothing new'] : [],
    slug === 'pomodoro' ? ['a sound row offers only sounds: pomodoro'] : []);
  for (const c of WEB_CASES) {
    if (!wanted(c.slug)) continue;
    const names = webNames(c.slug);
    if (!bySlug[c.slug]) { for (const n of names) ok(false, n, 'no ' + c.slug + ' editor in the release'); continue; }
    let tab = null; const box = {};
    try { tab = await openEditor(SITE_HOST, '/studio/', c, box); }
    catch (e) {
      for (const n of names) ok(false, n, 'the editor never reached its library button: ' + ((e && e.message) || e));
      if (box.tab) await closeTab(box.tab);
      continue;
    }
    // the row as the site draws it: no file input, and the button in its place
    let acc = null, libBtns = 0, pickerOnScreen = false;
    await check(names[0], async () => {
      acc = await fileInputAccepts(tab.sid);
      want(acc !== null, 'the page stopped answering');
      const extra = acc.filter((a) => !importish(a));
      libBtns = await evalIn(tab.sid, EX_LIB_BTNS);
      want(extra.length === 0, acc.length + ' file input(s) on the page, and this host lets no editor take a file: ' + extra.join(','));
      want(libBtns > 0, 'no “Your library…” button on the step');
      pickerOnScreen = true;
    });
    if (!pickerOnScreen) {
      for (const n of names.slice(1)) ok(false, n, 'no picker on the screen to open');
    } else {
      if (c.slug === 'flights') await check('the picker opens on the library: flights', async () => {
        want(await evalIn(tab.sid, exOpenPicker) === true, 'the row\u2019s Your library… button was gone');
        const picked = await waitFor('the picker\u2019s thumbnail for A', 30000, async () => {
          const p = await evalIn(tab.sid, EX_PICKER);
          if (p && p.rows && p.rows.some((r) => r.thumb && r.thumb.indexOf('data:image/png') === 0)) return p;
          return null;
        });
        const rowA = picked.rows.find((r) => r.text.indexOf('In Brass') >= 0);
        want(rowA, 'no row named the clock design: rows were [' + picked.rows.map((r) => r.text).join(' | ') + ']');
        want(picked.title === 'Your library', 'the modal was titled "' + picked.title + '"');
        want(rowA.thumb && rowA.thumb.indexOf('data:image/png') === 0, 'the row\u2019s face was not a PNG thumbnail: ' + rowA.thumb);
        want(rowA.tw === 48 && rowA.th === 48, 'the thumbnail was ' + rowA.tw + ' × ' + rowA.th + ', not 48 px');
        want(picked.text.indexOf(A.name) >= 0, 'the row did not name the asset (' + A.name + '): ' + picked.text.slice(0, 200));
        want(picked.text.indexOf(B.name) < 0 && picked.text.indexOf('In Desk') < 0, 'the sound row sat in a picture picker: ' + picked.text.slice(0, 200));
        // the owner's review shot: the picker open, as the modal paints it at 1280 × 800
        const dir = process.env.DELEGATE_EVIDENCE_DIR || SHOTS;
        if (dir) {
          const shot = path.join(dir, 'l1-studio-picker.png');
          await cdp.send('Target.activateTarget', { targetId: tab.targetId });
          const r = await cdp.send('Page.captureScreenshot', { format: 'png' }, tab.sid);
          fs.mkdirSync(dir, { recursive: true });
          fs.writeFileSync(shot, Buffer.from(r.data, 'base64'));
          console.log('SHOT ' + shot);
        }
      });

      if (c.slug === 'flights') await check('a clock picture picked in flights stores nothing new', async () => {
        want(await evalIn(tab.sid, exPickRow('In Brass')) === true, 'the A row was not pressable');
        await waitFor('the picker to close after the pick', 20000, async () => await evalIn(tab.sid, EX_CLOSED) === true);
        const mark = saves.length;
        await waitFor('the Render step', 30000, async () => await evalIn(tab.sid, exStep('Render')) === true);
        await sleep(700);
        want(await evalIn(tab.sid, exPress('Save to library')) === true, 'no Save to library button on the Render step');
        await waitFor('the save to reach the site', 40000, async () => {
          if (await evalIn(tab.sid, EX_RENAME) === true) return null;
          return saves.length > mark ? true : null;
        });
        const rec = saves[saves.length - 1];
        want(rec && /app=flights/.test(rec.url), 'the site never saw a save for flights' + (rec ? ' (saw ' + rec.url + ')' : ''));
        const seen = analyseSave(rec);
        const byRef = seen.refs.has(SHA_A) && seen.side.length > 0 && seen.side.every((h) => h === SHA_A);
        const byData = seen.inline.includes(SHA_A);
        want(byRef || byData, 'the save did not name A (' + SHA_A.slice(0, 12) + '…): refs=' + [...seen.refs].join(',') + ' inline=' + seen.inline.join(',') + ' sidecar=' + seen.side.join(','));
        const strays = [...seen.refs].filter((h) => h !== SHA_A).concat(seen.side.filter((h) => h !== SHA_A)).concat(seen.inline.filter((h) => h !== SHA_A));
        want(strays.length === 0, 'the save carried ' + strays.length + ' other picture(s): '
          + strays.map((h) => String(h).slice(0, 12) + '@' + (seen.where.get(h) || '?')).join(','));
      });

      if (c.slug === 'pomodoro') await check('a sound row offers only sounds: pomodoro', async () => {
        want(await evalIn(tab.sid, exOpenPicker) === true, 'the row\u2019s Your library… button was gone');
        const picked = await waitFor('the listed sound', 30000, async () => {
          const p = await evalIn(tab.sid, EX_PICKER);
          if (p && p.rows && p.rows.length) return p;
          return null;
        });
        const rowB = picked.rows.find((r) => r.text.indexOf('In Desk') >= 0);
        want(rowB, 'no row named the pomodoro design: ' + picked.rows.map((r) => r.text).join(' | '));
        want(picked.text.indexOf(A.name) < 0 && picked.text.indexOf('In Brass') < 0, 'a picture sat in a sound picker: ' + picked.text.slice(0, 200));
        want(picked.text.indexOf(B.name) >= 0, 'the row did not name the asset (' + B.name + '): ' + picked.text.slice(0, 200));
        want(picked.play, 'the sound row\u2019s face was not a Play button: ' + picked.text.slice(0, 200));
        want(rowB.text.indexOf('1.0 s') >= 0, 'its time did not read 1.0 s: ' + rowB.text);
      });
    }
    await closeTab(tab);
  }
  // the hub pair, on the same clock row: linked shows both controls, unlinked is today's row
  for (const hubCase of HUB_CASES) {
    if (!wanted('clock')) continue;
    await check(hubCase.name, async () => {
      want(bySlug.clock, 'no clock editor in the release');
      const tab = await openEditor(hubCase.host, '/editor/' + IP + '/', hubCase, null);
      const acc = await fileInputAccepts(tab.sid);
      const real = (acc || []).filter((x) => !importish(x));
      const libBtns = await evalIn(tab.sid, EX_LIB_BTNS);
      want(acc !== null, 'the page stopped answering');
      want(real.length > 0, 'no picture row with a file input on the screen');
      want((libBtns > 0) === hubCase.libWant, real.length + ' picture file input(s) and '
        + libBtns + ' library button(s), and this host should' + (hubCase.libWant ? ' show one' : ' show none'));
      await closeTab(tab);
    });
  }

  console.log(fails ? fails + ' FAILED of ' + checked : 'all green (' + checked + ' checks)');
  process.exitCode = fails ? 1 : 0;
})().catch((e) => {
  console.log('FAIL threw: ' + ((e && e.stack) || e));
  process.exitCode = 1;
}).finally(() => {
  try { server.close(); } catch (e) { /* not listening */ }
  try { shutdown(chromeProc, chromeDir); } catch (e) { /* gone */ }
});
