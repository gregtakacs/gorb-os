#!/usr/bin/env node
// sound-fit.js — the studio kit's sound converter (L1, ruling 42), opened in a real browser.
//
//   node sound-fit.js [--chrome <path>] [--only <n>]
//
// `OrbStudio.fitSound` is the one helper a page calls with a sound its owner chose, the way
// fitPicture is the one for a picture (docs/specs/l1-design.md §3.2): a 22,050 Hz mono PCM16
// WAV or the orb's own ADPCM file comes back byte for byte, and anything else the browser can
// decode becomes that file through the baker's encoder. The decoding is a browser's own work —
// decodeAudioData and an OfflineAudioContext render — so this loads the runtime the way an
// editor does, from a local http server, and builds every test sound inside the page (sines
// written into WAV headers, and one file the baker itself encodes). Nothing is committed.
//
// Deliberately not named headless-*.test.js, like picture-fit: it needs Chromium.
// `make check-pictures` runs both.
'use strict';
const fs = require('fs');
const http = require('http');
const os = require('os');
const path = require('path');
const { spawn } = require('child_process');

const HERE = __dirname;
const RUNTIME = path.join(HERE, '..');                                    // web/runtime
const arg = (n, f) => { const i = process.argv.indexOf('--' + n); return i > 0 && process.argv[i + 1] ? process.argv[i + 1] : f; };

// the scripts the page loads, served from the tree rather than a copy of it, so what is
// checked is the file that ships
const FILES = {
  '/react.production.min.js': path.join(RUNTIME, 'vendor', 'react.production.min.js'),
  '/baker.js': path.join(RUNTIME, 'baker@1', 'orbtheme-baker.js'),
  '/studio.js': path.join(RUNTIME, 'studio@1', 'studio.js'),
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
 * No backslash appears in it — it is a template literal. */
const PROBE = `(async function () {
'use strict';
var R = [];
function show() {
  try { document.getElementById('out').textContent = 'RESULTS:' + JSON.stringify(R) + (window.__fitDone ? ' DONE' : ''); } catch (e) {}
  // and posted as it stands, so a run killed mid-way still says every check it finished:
  // the driver keeps the browser open until the last, awaited report lands.
  try { fetch('/__report', { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ done: window.__fitDone ? 1 : 0, rows: R }) }).catch(function () {}); } catch (e) {}
}
function note(p, n, d) { R.push({ name: n, pass: p ? 1 : 0, detail: String(d == null ? '' : d) }); show(); }
function skip(n, d) { R.push({ name: n, pass: -1, detail: String(d) }); show(); }
function b64(bytes) {                                    // chunked: a megabyte in one apply overflows the stack
  var s = '', CH = 32768;
  for (var i = 0; i < bytes.length; i += CH) s += String.fromCharCode.apply(null, bytes.subarray(i, i + CH));
  return btoa(s);
}
function i16(x) { x = x !== x ? 0 : x < -1 ? -1 : x > 1 ? 1 : x; return Math.round(x * 32767); }
function tone(n, freq, rate) {                           // a sine a browser will happily decode
  var p = new Int16Array(n);
  for (var i = 0; i < n; i++) p[i] = i16(0.7 * Math.sin(2 * Math.PI * freq * i / rate));
  return p;
}
function wav(samples, rate, channels) {                  // PCM16 WAV, the interleaved way
  var n = samples.length, sz = n * 2, o = 0;
  var b = new Uint8Array(44 + sz), v = new DataView(b.buffer);
  function s(t) { for (var i = 0; i < t.length; i++) b[o++] = t.charCodeAt(i); }
  function le(n2) { b[o++] = n2 & 255; b[o++] = (n2 >>> 8) & 255; b[o++] = (n2 >>> 16) & 255; b[o++] = (n2 >>> 24) & 255; }
  s('RIFF'); le(36 + sz); s('WAVE'); s('fmt '); le(16); v.setUint16(o, 1, true); o += 2; v.setUint16(o, channels, true); o += 2;
  le(rate); le(rate * channels * 2); v.setUint16(o, channels * 2, true); o += 2; v.setUint16(o, 16, true); o += 2;
  s('data'); le(sz);
  for (var i = 0; i < n; i++) { v.setInt16(o, samples[i], true); o += 2; }
  return b;
}
async function urlBytes(u) { var r = await fetch(u); return new Uint8Array(await r.arrayBuffer()); }
async function F(bytes, name, type) {
  return await OrbStudio.fitSound(new File([bytes], name, { type: type || 'application/octet-stream' }));
}
try {
  if (!window.OrbStudio || typeof OrbStudio.fitSound !== 'function' || !window.GorbBake) {
    note(false, 'the suite loaded its runtime', 'no OrbStudio.fitSound or GorbBake on the page');
  } else {
    // 1. the file the orb already plays: kept exactly, no decoder touched
    {
      const w = wav(tone(22050, 440, 22050), 22050, 1);
      const r = await F(w, 'tone.wav', 'audio/wav');
      const want = 'data:audio/wav;base64,' + b64(w);
      const rd = GorbBake.soundRead(r.dataUrl);
      note(r.converted === false && r.dataUrl === want && r.samples === 22050 && Math.abs(r.seconds - 1) < 1e-6
        && !!rd && rd.pcm.length === 22050,
        'fitSound keeps a 22,050 Hz mono PCM16 WAV byte for byte',
        'converted=' + r.converted + ' samples=' + r.samples + ' bytes=' + w.length + ' same-url=' + (r.dataUrl === want));
    }
    // 2. the orb's own encoding, encoded by the baker: kept too, byte for byte
    try {
      const pcm = GorbBake.synth({ name: 'fit', dur: 1, events: [{ t: 0, kind: 'bell', f: 880, dur: 1 }] });
      const url = GorbBake.soundFileUrl(pcm);
      const rd = GorbBake.soundRead(url);
      if (!rd || rd.enc !== 1) skip("fitSound keeps the orb's ADPCM file byte for byte", 'the baker encoded this chime as PCM16 (enc ' + (rd && rd.enc) + ')');
      else {
        const r = await F(await urlBytes(url), 'chime.adpcm', 'audio/wav');
        note(r.converted === false && r.dataUrl === url && r.samples === pcm.length,
          "fitSound keeps the orb's ADPCM file byte for byte", 'enc=' + rd.enc + ' samples=' + r.samples);
      }
    } catch (e) { note(false, "fitSound keeps the orb's ADPCM file byte for byte", String((e && e.message) || e)); }
    // 3. another shape entirely: the browser decodes, the baker re-encodes, one file again
    try {
      const N = 44100, st = new Int16Array(N * 2);
      for (var i = 0; i < N; i++) { st[i * 2] = i16(0.6 * Math.sin(2 * Math.PI * 440 * i / 44100)); st[i * 2 + 1] = i16(-0.6 * Math.sin(2 * Math.PI * 440 * i / 44100)); }
      const w = wav(st, 44100, 2);
      const r = await F(w, 'stereo.wav', 'audio/wav');
      const rd = GorbBake.soundRead(r.dataUrl);
      note(r.converted === true && !!rd && rd.pcm.length === r.samples && Math.abs(r.samples - 22050) <= 221
        && Math.abs(r.seconds - r.samples / 22050) < 1e-6,
        "fitSound converts a 44,100 Hz stereo WAV to the editors' file",
        'samples=' + r.samples + ' seconds=' + r.seconds + ' enc=' + (rd && rd.enc));
    } catch (e) { note(false, "fitSound converts a 44,100 Hz stereo WAV to the editors' file", String((e && e.message) || e)); }
    // 4. a long one is converted, not refused: length is the site's shared check's business
    try {
      const w = wav(tone(44100 * 12, 320, 44100), 44100, 1);
      const r = await F(w, 'long.wav', 'audio/wav');
      const rd = GorbBake.soundRead(r.dataUrl);
      note(r.converted === true && !!rd && Math.abs(r.seconds - 12) <= 0.12 && Math.abs(r.samples - 264600) <= 2646
        && r.samples === rd.pcm.length,
        'fitSound converts a 12 s WAV without refusing it', 'samples=' + r.samples + ' seconds=' + r.seconds);
    } catch (e) { note(false, 'fitSound converts a 12 s WAV without refusing it', String((e && e.message) || e)); }
    // 5. not a sound at all: the contract's own sentence
    {
      let msg = 'no rejection at all';
      try { await F(new TextEncoder().encode('hello'), 'hello.bin'); }
      catch (e) { msg = String((e && e.message) || e); }
      note(msg === 'That file is not a sound this browser can read: a sound must be audio the browser can decode (WAV, MP3, OGG …), at most 20.0 s once converted', "fitSound's refusal names what a sound must be", msg);
    }
  }
} catch (e) { note(false, 'the suite threw', String((e && e.stack) || e).slice(0, 400)); }
window.__fitDone = true;
show();
try { await fetch('/__report', { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ done: 1, rows: R }) }); } catch (x) { show(); }
document.title = 'sound-fit done';
})();`;


const TYPES = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8' };
const typeOf = (f) => TYPES[path.extname(f).toLowerCase()] || 'application/octet-stream';
const PAGE = '<!doctype html>\n<html><head><meta charset="utf-8"><title>sound-fit</title></head><body>\n'
  + '<pre id="out">running</pre>\n'
  + '<script src="/react.production.min.js"></script>\n<script src="/baker.js"></script>\n'
  + '<script src="/studio.js"></script>\n'
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

/* One Chromium, one page. Not --dump-dom and no virtual-time budget, the way picture-fit opens:
 * an OfflineAudioContext render is real-time work that virtual time lets run out before it
 * settles — the 12 s case of check 4 died at every dumped run here — so the browser is kept
 * alive with --remote-debugging-port, the page POSTs its rows after every check, and the driver
 * kills it by the pid it was given once the last report lands. A run that dies mid-way still
 * says every check it finished. */
function open(url) {
  return new Promise((resolve) => {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'orb-sound-fit-'));
    const args = ['--headless=new', '--no-sandbox', '--disable-gpu', '--no-first-run',
      '--remote-debugging-port=0', '--user-data-dir=' + dir, url];
    const child = spawn(CHROME, args, { stdio: ['ignore', 'ignore', 'pipe'] });
    let err = '', gone = null;
    child.stderr.on('data', (d) => { err += d; });
    child.on('exit', (code) => {
      if (gone) return; gone = true;
      try { child.stderr.destroy(); } catch (e) { /* already gone */ }
      fs.rmSync(dir, { recursive: true, force: true });
      const bad = err.split('\n').filter((l) => l && !/ERROR:dbus|ERROR:gpu|ERROR:vaapi|ERROR:bluez|ERROR:field_trial|ERROR:viz_main|ERROR:angle|ERROR:media/
        .test(l)).join(' ').slice(0, 300);
      resolve({ code: code, err: bad });
    });
    open.child = child;
  });
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

(async () => {
  await new Promise((res) => server.listen(0, '127.0.0.1', res));
  const run = open('http://127.0.0.1:' + server.address().port + '/');
  let died = false;
  run.then(() => { died = true; });
  let killed = false;
  const timer = setTimeout(() => { killed = true; try { open.child.kill('SIGKILL'); } catch (e) { /* gone */ } }, 300000);
  for (let i = 0; i < 5900 && !(report && report.done) && !died; i++) await sleep(50);
  clearTimeout(timer);
  if (open.child && !died) { try { open.child.kill('SIGKILL'); } catch (e) { /* gone */ } }
  await run;
  server.close();
  const rep = report;
  if (!rep || !Array.isArray(rep.rows) || !rep.rows.length) {
    console.log('FAIL the page reported nothing' + (killed ? ' (killed at 300s)' : died ? ' (the browser exited on its own)' : '')
      + '  [' + (rep && rep.rows ? rep.rows.length + ' rows' : 'no rows') + ']');
    console.log('sound-fit: 0 checks ran');
    process.exit(1);
  }
  if (!rep.done) {
    rep.rows.push({ name: 'the page ran every check', pass: 0, detail: (killed ? 'killed at 300s; ' : died ? 'the browser exited; ' : '') + rep.rows.length + ' checks reported' });
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
  console.log(fails ? fails + ' of ' + checked + ' checks FAILED' : 'sound-fit: ' + checked + '/' + checked + ' passed');
  process.exit(fails ? 1 : 0);
})().catch((e) => { console.log('FAIL threw: ' + ((e && e.stack) || e)); try { server.close(); } catch (e2) { /* not listening */ } process.exit(1); });
