#!/usr/bin/env node
// picture-fit.js — the studio kit's picture fitter (ruling 35), opened in a real browser.
//
//   node picture-fit.js [--chrome <path>] [--only <n>]
//
// `OrbStudio.fitPicture` is the one helper the clock's and the flights' pickers call before a
// picked file becomes art, and its whole job is canvas work: decode, scale, re-encode. No node
// stub can prove a canvas did that, so this loads the runtime the way an editor does — React,
// `studio@1/studio.js`, then the clock's `dial-renderer.js` + `watch-engine.js` — from a local
// http server, and builds every test picture inside the page with a canvas (`toBlob`) or as
// SVG text, wraps it as a `File`, and asks the helper what it makes of it. The page posts its
// report back over http and the DOM holds the same JSON, exactly as editors-smoke.js reports.
//
// Deliberately not named headless-*.test.js, like editors-smoke and runner-check: it needs
// Chromium. `make check-pictures` runs it, and `make check-web` runs that first.
'use strict';
const fs = require('fs');
const http = require('http');
const os = require('os');
const path = require('path');
const { spawn } = require('child_process');

const HERE = __dirname;
const RUNTIME = path.join(HERE, '..');                                    // web/runtime
const ROOT = path.join(RUNTIME, '..', '..', '..', '..');                  // the repo root
const arg = (n, f) => { const i = process.argv.indexOf('--' + n); return i > 0 && process.argv[i + 1] ? process.argv[i + 1] : f; };

// the four scripts the page loads, served from the tree rather than a copy of it, so what is
// checked is the file that ships
const FILES = {
  '/react.production.min.js': path.join(RUNTIME, 'vendor', 'react.production.min.js'),
  '/studio.js': path.join(RUNTIME, 'studio@1', 'studio.js'),
  '/dial-renderer.js': path.join(ROOT, 'apps', 'clock', 'editor', 'dial-renderer.js'),
  '/watch-engine.js': path.join(ROOT, 'apps', 'clock', 'editor', 'watch-engine.js'),
};

// the browser: --chrome, then $CHROME, then the names a Linux box answers to.
function chromePath() {
  const named = arg('chrome', process.env.CHROME || '');
  if (named) return fs.existsSync(named) ? named : null;
  for (const c of ['chromium', 'chromium-browser', 'google-chrome', 'google-chrome-stable']) {
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
/* The suite, as one string: the page needs no build step and the checks read as written.
 * No backslash appears in it — it is a template literal, and a string literal would have
 * eaten a regex's escapes. */
const PROBE = `(async function () {
'use strict';
var R = [];
function show() { try { document.getElementById('out').textContent = 'RESULTS:' + JSON.stringify(R) + (window.__fitDone ? ' DONE' : ''); } catch (e) {} }
function note(p, n, d) { R.push({ name: n, pass: p ? 1 : 0, detail: String(d == null ? '' : d) }); show(); }
function skip(n, d) { R.push({ name: n, pass: -1, detail: String(d) }); show(); }
// a picture the browser itself encoded, so the bytes are a real encoder's answer
function pic(w, h, type, noise) {
  return new Promise(function (ok, bad) {
    var c = document.createElement('canvas'); c.width = w; c.height = h;
    var x = c.getContext('2d');
    if (noise) {
      var im = x.createImageData(w, h), p = im.data;
      for (var i = 0; i < p.length; i += 4) { p[i] = (Math.random() * 256) | 0; p[i + 1] = (Math.random() * 256) | 0; p[i + 2] = (Math.random() * 256) | 0; p[i + 3] = 255; }
      x.putImageData(im, 0, 0);
    } else {
      x.fillStyle = '#123456'; x.fillRect(0, 0, w, h);
      x.fillStyle = '#ee9922'; x.fillRect(0, 0, w >> 1, h >> 1);
      x.fillStyle = '#22cc88'; x.fillRect(w >> 1, h >> 1, w - (w >> 1), h - (h >> 1));
      x.strokeStyle = '#ffffff'; x.lineWidth = Math.max(2, Math.round(Math.min(w, h) / 40));
      x.strokeRect(x.lineWidth, x.lineWidth, w - 2 * x.lineWidth, h - 2 * x.lineWidth);
    }
    var ext = type === 'image/webp' ? 'webp' : type === 'image/jpeg' ? 'jpg' : 'png';
    c.toBlob(function (b) {
      b ? ok(new File([b], 'art.' + ext, { type: type })) : bad(new Error('the browser would not encode ' + type));
    }, type, 0.9);
  });
}
function urlOf(file) {
  return new Promise(function (ok, bad) {
    var r = new FileReader(); r.onerror = function () { bad(new Error('unread')); };
    r.onload = function () { ok(String(r.result)); }; r.readAsDataURL(file);
  });
}
function dec(u) {
  return new Promise(function (ok) {
    var im = new Image(); im.onload = function () { ok({ w: im.naturalWidth, h: im.naturalHeight }); };
    im.onerror = function () { ok({ w: -1, h: -1 }); }; im.src = u;
  });
}
// an encoder that never answers is a browser with no encoder: a skip, never a hang
function soon(p, ms) {
  return new Promise(function (ok, bad) {
    const t = setTimeout(function () { bad(new Error('the browser gave no answer in ' + ms + 'ms')); }, ms);
    p.then(function (v) { clearTimeout(t); ok(v); }, function (e) { clearTimeout(t); bad(e); });
  });
}
const dims = (r) => r.type + ' ' + r.width + 'x' + r.height + ',' + (r.scaled ? ' scaled' : ' kept');
const SVG = '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" width="24" height="24"><rect width="24" height="24" fill="#334455"/><circle cx="12" cy="12" r="9" fill="#ee9922"/></svg>';
const svgFile = () => new File([SVG], 'art.svg', { type: 'image/svg+xml' });
const F = window.OrbStudio ? OrbStudio.fitPicture : null;
try {
  if (!F) note(false, 'the page loaded the studio kit', 'no OrbStudio.fitPicture on the page');
  else {
    note(typeof F === 'function' && OrbStudio.FIT_MAX === 1024, '0. the studio kit carries fitPicture with a 1024 cap', typeof F);
    let f, r, d;
    f = await pic(3000, 2000, 'image/png'); r = await F(f); d = await dec(r.dataUrl);
    note(r.type === 'image/png' && r.width === 1024 && r.height === 683 && r.scaled === true && d.w === 1024 && d.h === 683,
      '1. a 3000 x 2000 PNG fits to 1024 x 683 and the answer decodes', dims(r) + ', decodes ' + d.w + 'x' + d.h);
    f = await pic(3000, 2000, 'image/jpeg'); r = await F(f); d = await dec(r.dataUrl);
    note(r.type === 'image/jpeg' && r.width === 1024 && r.height === 683 && r.scaled === true && d.w === 1024 && d.h === 683,
      '2. a 3000 x 2000 JPEG stays a JPEG and fits to 1024 x 683', dims(r) + ', decodes ' + d.w + 'x' + d.h);
    f = await pic(2000, 3000, 'image/png'); r = await F(f);
    note(r.type === 'image/png' && r.width === 683 && r.height === 1024 && r.scaled === true,
      '3. a 2000 x 3000 PNG fits to 683 x 1024', dims(r));
    f = await pic(800, 600, 'image/png'); r = await F(f);
    note(r.scaled === false && r.type === 'image/png' && r.width === 800 && r.height === 600 && r.dataUrl === await urlOf(f),
      '4. an 800 x 600 PNG comes back as the file it was, byte for byte', dims(r));
    f = await pic(1024, 1024, 'image/png'); r = await F(f);
    note(r.scaled === false && r.width === 1024 && r.height === 1024, '5a. a 1024 x 1024 PNG is already inside the bounds', dims(r));
    f = await pic(1025, 10, 'image/png'); r = await F(f);
    note(r.scaled === true && r.width === 1024 && r.height === 10, '5b. a 1025 x 10 PNG fits to 1024 x 10', dims(r));
    r = await F(svgFile()); d = await dec(r.dataUrl);
    note(r.scaled === true && r.type === 'image/png' && r.width === 1024 && r.height === 1024 && d.w === 1024 && d.h === 1024,
      '6a. a 24 x 24 SVG is rasterised to a 1024 x 1024 PNG', dims(r) + ', decodes ' + d.w + 'x' + d.h);
    r = await F(svgFile(), { max: 192 });
    note(r.scaled === true && r.type === 'image/png' && r.width === 192 && r.height === 192,
      '6b. the same SVG at max 192 is rasterised to 192 x 192', dims(r));
    // the WebP contract has two halves. A WebP source is read and, when the caller asks for a
    // type, re-encoded: this browser will encode a small WebP, so that half is always checked.
    // Scaling a big one down needs a 3000 x 2000 WebP, and this Chromium never answers a toBlob
    // for one that size — no encoder for a phone-sized photo is exactly the case the spec lets skip.
    r = await F(await pic(64, 48, 'image/webp'), { type: 'image/png' });
    d = await dec(r.dataUrl);
    note(r.scaled === true && r.type === 'image/png' && r.width === 64 && r.height === 48 && d.w === 64 && d.h === 48,
      '7a. a picked WebP is read and re-encoded to the PNG the caller asked for', dims(r) + ', decodes ' + d.w + 'x' + d.h);
    let wf = null, wnote = '';
    try { wf = await soon(pic(3000, 2000, 'image/webp'), 30000); }
    catch (e) { wnote = String((e && e.message) || e); }
    if (wf && String(wf.type) !== 'image/webp') { wnote = 'the browser encoded ' + wf.type + ' instead'; wf = null; }
    if (!wf) skip('7b. a 3000 x 2000 WebP fits to 1024 x 683', 'no 3000 x 2000 WebP to fit: ' + (wnote || 'the browser gave no blob'));
    else {
      r = await F(wf); d = await dec(r.dataUrl);
      note(r.type === 'image/png' && r.width === 1024 && r.height === 683 && r.scaled === true && d.w === 1024 && d.h === 683,
        '7b. a 3000 x 2000 WebP fits to 1024 x 683', dims(r) + ', decodes ' + d.w + 'x' + d.h);
    }
    let msg = 'no rejection at all';
    try { await F(new File([new TextEncoder().encode('this is a text file, not a picture')], 'x.png', { type: 'image/png' })); }
    catch (e) { msg = String((e && e.message) || e); }
    note(msg === 'That file is not a picture this browser can read', '8. a text file named x.png is refused', msg);
    const E = window.WatchEngine;
    if (!E || !E.assetFromFile) note(false, '9. the clock engine loaded', 'no window.WatchEngine.assetFromFile on the page');
    else {
      const a = await E.assetFromFile(await pic(3000, 2000, 'image/png'));
      d = await dec(String(a.dataUrl || ''));
      note(!a.error && a.kind === 'png' && a.w === 1024 && a.h === 683
        && String(a.dataUrl).slice(0, 22) === 'data:image/png;base64,' && d.w === 1024 && d.h === 683,
        '9. the clock stores the picked picture as a fitted 1024 x 683 png',
        a.error || (a.kind + ' ' + a.w + 'x' + a.h + ', decodes ' + d.w + 'x' + d.h + ', id ' + (a.id || '')));
      const a2 = await E.assetFromFile(await pic(1500, 1500, 'image/png', true));
      note(!!a2.error && new RegExp('is [0-9]+ KB; the limit is 1.5 MB$').test(String(a2.error)),
        '10. the clock weighs its 1.5 MB limit on the fitted bytes', a2.error || 'no error: a ' + (a2.dataUrl || '').length + ' B data url');
      const a3 = await E.assetFromFile(new File([new TextEncoder().encode('a text file wearing a .png name')], 'art.png', { type: 'image/png' }));
      note(!!a3.error && String(a3.error).indexOf('That file is not a picture this browser can read') === 0
        && String(a3.error).slice(-9) === ': art.png',
        '11. a file the clock cannot decode is refused with the fit message and its name', a3.error || 'no error at all');
    }
    // a 1x1 transparent GIF, the smallest one that decodes: a kept picture must come back
    // as its own bytes, animation and all, and a forced type must still be a re-encode
    const GIF = 'R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7';
    const gifBytes = Uint8Array.from(atob(GIF), (ch) => ch.charCodeAt(0));
    const gifFile = () => new File([gifBytes], 'art.gif', { type: 'image/gif' });
    f = gifFile(); r = await F(f);
    note(r.scaled === false && r.type === 'image/gif' && r.width === 1 && r.height === 1 && r.dataUrl === await urlOf(f),
      '12. a GIF inside the bounds is the same GIF it was, frame for frame', dims(r));
    r = await F(gifFile(), { type: 'image/png' });
    note(r.scaled === true && r.type === 'image/png' && r.width === 1 && r.height === 1,
      '13. a GIF an editor forces to PNG becomes a PNG', dims(r));
  }
} catch (e) { note(false, 'the suite threw', String((e && e.stack) || e).slice(0, 400)); }
window.__fitDone = true;
show();
try { await fetch('/__report', { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ done: 1, rows: R }) }); } catch (x) { show(); }
document.title = 'picture-fit done';
})();`;


const TYPES = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8' };
const typeOf = (f) => TYPES[path.extname(f).toLowerCase()] || 'application/octet-stream';
const PAGE = '<!doctype html>\n<html><head><meta charset="utf-8"><title>picture-fit</title></head><body>\n'
  + '<pre id="out">running</pre>\n'
  + '<script src="/react.production.min.js"></script>\n<script src="/studio.js"></script>\n'
  + '<script src="/dial-renderer.js"></script>\n<script src="/watch-engine.js"></script>\n'
  + '<script>\n' + PROBE + '\n</script>\n</body></html>\n';
let report = null;
const server = http.createServer((req, res) => {
  const p = new URL(req.url, 'http://127.0.0.1').pathname;
  if (req.method === 'POST' && p === '/__report') {
    let text = '';
    req.on('data', (c) => { text += c; });
    req.on('end', () => { try { report = JSON.parse(text); } catch (e) { report = { done: 1, rows: [{ name: 'the page posted nothing readable', pass: 0, detail: text.slice(0, 120) }] }; } });
    res.writeHead(204, { 'content-type': 'application/json; charset=utf-8', 'cache-control': 'no-store' });
    return res.end();
  }
  if (p === '/' || p === '/index.html') {
    res.writeHead(200, { 'content-type': 'text/html; charset=utf-8', 'cache-control': 'no-store' });
    return res.end(PAGE);
  }
  const f = FILES[p];
  if (f && fs.existsSync(f)) {
    res.writeHead(200, { 'content-type': typeOf(p), 'cache-control': 'no-store' });
    return res.end(fs.readFileSync(f));
  }
  res.writeHead(404, { 'content-type': 'text/plain; charset=utf-8', 'cache-control': 'no-store' });
  res.end('not found');
});

// one Chromium, one page, the same flags editors-smoke.js opens the shelf's editors with
function open(url) {
  return new Promise((resolve) => {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'orb-picture-fit-'));
    const args = ['--headless=new', '--no-sandbox', '--disable-gpu', '--no-first-run',
      '--user-data-dir=' + dir, '--virtual-time-budget=240000', '--dump-dom', url];
    const child = spawn(CHROME, args, { stdio: ['ignore', 'pipe', 'pipe'] });
    let dom = '', err = '', killed = false;
    child.stdout.on('data', (d) => { dom += d; });
    child.stderr.on('data', (d) => { err += d; });
    const timer = setTimeout(() => { killed = true; try { child.kill('SIGKILL'); } catch (e) { /* gone */ } }, 300000);
    child.on('exit', () => {
      clearTimeout(timer);
      try { child.stdout.destroy(); child.stderr.destroy(); } catch (e) { /* already gone */ }
      fs.rmSync(dir, { recursive: true, force: true });
      const bad = err.split('\n').filter((l) => l && !/ERROR:dbus|ERROR:gpu|ERROR:vaapi|ERROR:bluez|ERROR:field_trial|ERROR:viz_main|ERROR:angle|ERROR:media/
        .test(l)).join(' ').slice(0, 300);
      resolve({ killed, dom, err: bad });
    });
  });
}

/* The report, from the POST if it landed, else out of the dumped DOM: the page writes the same
 * JSON into its <pre> after every check, so a browser killed mid-run still says what it found. */
const entities = (s) => String(s).replace(/&quot;/g, '"').replace(/&#0?39;/g, "'").replace(/&apos;/g, "'")
  .replace(/&lt;/g, '<').replace(/&gt;/g, '>').replace(/&amp;/g, '&');
function fromDom(dom) {
  const m = /<pre id="out">([\s\S]*?)<\/pre>/i.exec(String(dom));
  if (!m) return null;
  const t = entities(m[1]);
  if (t.indexOf('RESULTS:') !== 0) return null;
  try {
    return { done: / DONE\s*$/.test(t) ? 1 : 0, rows: JSON.parse(t.slice(t.indexOf('['), t.lastIndexOf(']') + 1)) };
  } catch (e) { return null; }
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

(async () => {
  await new Promise((res) => server.listen(0, '127.0.0.1', res));
  const run = await open('http://127.0.0.1:' + server.address().port + '/');
  for (let i = 0; i < 60 && !report; i++) await sleep(50);
  server.close();
  const rep = report || fromDom(run.dom);
  if (!rep || !Array.isArray(rep.rows)) {
    console.log('FAIL the page reported nothing' + (run.killed ? ' (killed at 300s)' : ' (' + run.dom.length + ' B dumped)')
      + (run.err ? '  [' + run.err.slice(0, 240) + ']' : ''));
    console.log('picture-fit: 0 checks ran');
    process.exit(1);
  }
  if (!rep.done) {
    rep.rows.push({ name: 'the page ran every check', pass: 0, detail: (run.killed ? 'killed at 300s; ' : '') + rep.rows.length + ' checks reported' + (run.err ? '; ' + run.err.slice(0, 160) : '') });
  }
  const rows = rep.rows;
  const only = arg('only', null);
  let checked = 0, fails = 0;
  for (const r of rows) {
    if (only && r.name.indexOf(only) !== 0) continue;
    if (r.pass === -1) { console.log('SKIP ' + r.name + (r.detail ? '  [' + r.detail + ']' : '')); continue; }
    checked++;
    if (!r.pass) fails++;
    console.log((r.pass ? 'PASS ' : 'FAIL ') + r.name + (r.detail && (!r.pass || process.env.VERBOSE) ? '  [' + r.detail + ']' : ''));
  }
  console.log(fails ? fails + ' of ' + checked + ' checks FAILED' : 'picture-fit: ' + checked + '/' + checked + ' passed');
  process.exit(fails ? 1 : 0);
})().catch((e) => { console.log('FAIL threw: ' + ((e && e.stack) || e)); try { server.close(); } catch (e2) { /* not listening */ } process.exit(1); });
