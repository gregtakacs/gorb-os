#!/usr/bin/env node
// svg-vectors.js — fu2-1: the shared SVG vectors through the studio kit's own checker, in a
// real browser.
//
//   node svg-vectors.js [--chrome <path>] [--only <prefix>] [--json <file>]
//
// tools/orb-manager/testdata/svg-vectors.json is one rule set's answer sheet: every input the
// review brought, the cases the three checkers have always held to, and the shapes real art
// arrives in, each with the rule id it must answer (null for a file all three accept). Go
// iterates the same file in TestSvgVectorsFile; the site's svg-art tests will too. The studio
// kit's checker parses with DOMParser, which node does not have, so this is the surface that
// runs them: a page served from the tree loads studio@1/studio.js, is handed the vectors as
// JSON, and answers each one. The report comes back over http and sits in the DOM too, as in
// picture-fit.js.
//
// Deliberately not named headless-*.test.js, like editors-smoke and runner-check: it needs
// Chromium. `make check-editor` runs it, next to the node suite that checks the byte half.
'use strict';
const fs = require('fs');
const http = require('http');
const os = require('os');
const path = require('path');
const { spawn } = require('child_process');

const HERE = __dirname;
const RUNTIME = path.join(HERE, '..');                                    // web/runtime
const ROOT = path.join(RUNTIME, '..', '..', '..', '..');                  // the repo root
const VECTORS = path.join(ROOT, 'tools', 'orb-manager', 'testdata', 'svg-vectors.json');
const arg = (n, f) => { const i = process.argv.indexOf('--' + n); return i > 0 && process.argv[i + 1] ? process.argv[i + 1] : f; };

const vectors = JSON.parse(fs.readFileSync(VECTORS, 'utf8'));
if (!Array.isArray(vectors) || vectors.length < 70) {
  console.log('FAIL ' + VECTORS + ' holds ' + (Array.isArray(vectors) ? vectors.length : '?') + ' vectors; the shared set is at least 70');
  process.exit(1);
}

const FILES = {
  '/react.production.min.js': path.join(RUNTIME, 'vendor', 'react.production.min.js'),
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

// A body that is not valid UTF-8 travels as base64, since a JSON string cannot hold it; the
// page rebuilds those bytes with atob rather than decoding anything itself.
const PROBE = `(async function () {
'use strict';
var V = JSON.parse(document.getElementById('vectors').textContent);
var R = [];
function show() { try { document.getElementById('out').textContent = 'RESULTS:' + JSON.stringify(R) + (window.__vectorsDone ? ' DONE' : ''); } catch (e) {} }
function note(p, n, d) { R.push({ name: n, pass: p ? 1 : 0, detail: String(d == null ? '' : d) }); show(); }
function bytesOf(v) {
  if (v.svg_base64 != null) {
    var raw = atob(v.svg_base64), u = new Uint8Array(raw.length);
    for (var i = 0; i < raw.length; i++) u[i] = raw.charCodeAt(i);
    return u;
  }
  return new TextEncoder().encode(v.svg);
}
try {
  if (!window.OrbStudio || !OrbStudio.checkSvg) note(false, 'the page loaded the studio kit', 'no OrbStudio.checkSvg on the page');
  else {
    note(typeof OrbStudio.checkSvg(new TextEncoder().encode('<svg xmlns="http://www.w3.org/2000/svg"/>')) !== 'undefined',
      '0. the kit answers a plain SVG file', String(OrbStudio.checkSvg(new TextEncoder().encode('<svg xmlns="http://www.w3.org/2000/svg"/>'))));
    // the size ceiling is the one rule the file cannot carry: an accepted file of exactly
    // 256 KB, and the same file one byte heavier, both read off bytes made here
    var pad = function (n) { var t = '<svg xmlns="http://www.w3.org/2000/svg">'; while (t.length < n - 6) t += ' '; return t + '</svg>'; };
    note(OrbStudio.checkSvg(new TextEncoder().encode(pad(262144))) === null,
      '1a. an SVG of exactly 256 KB is kept by its weight', OrbStudio.SVG_MAX_BYTES + ' B');
    var o = OrbStudio.checkSvg(new TextEncoder().encode(pad(262145)));
    note(o && o.rule === 'svg_too_big' && /^an SVG in it is 257 KB/.test(o.message),
      '1b. one byte over is refused as 257 KB', JSON.stringify(o));
    var byRule = {};
    for (var k = 0; k < V.length; k++) {
      var v = V[k], got;
      try { got = OrbStudio.checkSvg(bytesOf(v)); } catch (e) { got = { rule: 'threw', message: String((e && e.message) || e) }; }
      var rule = got === null ? null : got.rule;
      byRule[rule] = (byRule[rule] || 0) + 1;
      var want = v.rule === null || v.rule === undefined ? null : v.rule;
      note(rule === want, v.name, rule === want ? '' : 'want ' + want + ', got ' + (rule === null ? 'kept' : rule) + ' [' + (got && got.message) + ']');
    }
    note(Object.keys(byRule).length > 0, 'the rules the file asks', JSON.stringify(byRule));
  }
} catch (e) { note(false, 'the suite threw', String((e && e.stack) || e).slice(0, 400)); }
window.__vectorsDone = true;
show();
try { await fetch('/__report', { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ done: 1, rows: R }) }); } catch (x) { show(); }
document.title = 'svg-vectors done';
})();`;

const TYPES = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8', '.json': 'application/json; charset=utf-8' };
const typeOf = (f) => TYPES[path.extname(f).toLowerCase()] || 'application/octet-stream';
const page = '<!doctype html>\n<html><head><meta charset="utf-8"><title>svg-vectors</title></head><body>\n'
  + '<pre id="out">running</pre>\n'
  + '<script id="vectors" type="application/json">' + JSON.stringify(vectors).replace(/</g, '\\u003c') + '</script>\n'
  + '<script src="/react.production.min.js"></script>\n<script src="/studio.js"></script>\n'
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
    return res.end(page);
  }
  const f = FILES[p];
  if (f && fs.existsSync(f)) {
    res.writeHead(200, { 'content-type': typeOf(p), 'cache-control': 'no-store' });
    return res.end(fs.readFileSync(f));
  }
  res.writeHead(404, { 'content-type': 'text/plain; charset=utf-8', 'cache-control': 'no-store' });
  res.end('not found');
});

// one Chromium, one page, the flags picture-fit.js and editors-smoke.js open with
function open(url) {
  return new Promise((resolve) => {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'orb-svg-vectors-'));
    const args = ['--headless=new', '--no-sandbox', '--disable-gpu', '--no-first-run',
      '--user-data-dir=' + dir, '--virtual-time-budget=600000', '--dump-dom', url];
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
  for (let i = 0; i < 120 && !report; i++) await sleep(50);
  server.close();
  const rep = report || fromDom(run.dom);
  if (!rep || !Array.isArray(rep.rows)) {
    console.log('FAIL the page reported nothing' + (run.killed ? ' (killed at 300s)' : ' (' + run.dom.length + ' B dumped)')
      + (run.err ? '  [' + run.err.slice(0, 240) + ']' : ''));
    console.log('svg-vectors: 0 checks ran');
    process.exit(1);
  }
  if (!rep.done) {
    rep.rows.push({ name: 'the page ran every check', pass: 0, detail: (run.killed ? 'killed at 300s; ' : '') + rep.rows.length + ' checks reported' + (run.err ? '; ' + run.err.slice(0, 160) : '') });
  }
  const rows = rep.rows;
  const only = arg('only', null);
  let checked = 0, fails = 0;
  const failed = [];
  for (const r of rows) {
    if (only && r.name.indexOf(only) !== 0) continue;
    checked++;
    if (!r.pass) { fails++; failed.push(r); }
    if (!r.pass || process.env.VERBOSE) console.log('FAIL ' + r.name + (r.detail ? '  [' + r.detail + ']' : ''));
  }
  if (arg('json', null)) {
    fs.writeFileSync(arg('json', ''), JSON.stringify(rows.map((r) => ({ name: r.name, pass: !!r.pass, detail: r.detail })), null, 1) + '\n');
  }
  console.log(fails ? 'svg-vectors: ' + (checked - fails) + '/' + checked + ' passed, ' + fails + ' FAILED'
    : 'svg-vectors: ' + checked + '/' + checked + ' passed');
  process.exit(fails ? 1 : 0);
})().catch((e) => { console.log('FAIL threw: ' + ((e && e.stack) || e)); try { server.close(); } catch (e2) { /* not listening */ } process.exit(1); });
