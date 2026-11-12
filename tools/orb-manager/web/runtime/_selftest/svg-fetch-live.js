#!/usr/bin/env node
// svg-fetch-live.js — fu2-1's live gate: no SVG the rules accept ever asks the network.
//
//   node svg-fetch-live.js [--chrome <path>] [--only <prefix>] [--json <file>] [--budget <ms>]
//
// docs/specs/fu2-svg-rules.md ends with the claim that the point of all this is: an input the
// checkers accept makes the browser load nothing of itself, in any of the three ways a picked
// picture can end up in front of a browser — opened as image/svg+xml, drawn by an <img>, and
// inlined into an HTML page. The vectors in testdata/svg-vectors.json that name a URL name
// this harness's own beacon (http://127.0.0.1:47613/b/<name>), so a fetch is not a judgement
// about the rules but a line in a log.
//
// Each context runs as its own pass over every vector, with the hit list cleared between
// passes and nothing cached (every response is no-store), so a hit says which context made it.
// The gate is two-sided: an accepted vector that fetched fails it, and so does a run in which
// fewer than ten refused vectors fetched — a harness that can see no fetches proves nothing.
//
// Deliberately not part of `make check`, like check-pictures and check-web: it needs a
// Chromium, and a minute or two of them. `make check-svg-live` runs it.
'use strict';
const fs = require('fs');
const http = require('http');
const os = require('os');
const path = require('path');
const { spawn } = require('child_process');

const HERE = __dirname;
const RUNTIME = path.join(HERE, '..');
const ROOT = path.join(RUNTIME, '..', '..', '..', '..');
const VECTORS = path.join(ROOT, 'tools', 'orb-manager', 'testdata', 'svg-vectors.json');
const arg = (n, f) => { const i = process.argv.indexOf('--' + n); return i > 0 && process.argv[i + 1] ? process.argv[i + 1] : f; };
const PORT = +arg('port', 47613);
const BUDGET = +arg('budget', 3000);
const GIF1x1 = Buffer.from('R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7', 'base64');
const vectors = JSON.parse(fs.readFileSync(VECTORS, 'utf8'));
const only = arg('only', null);
const wanted = vectors.filter((v) => !only || v.name.indexOf(only) === 0);
if (!wanted.length) { console.log('no vector matches --only ' + only); process.exit(1); }

// the bytes a vector describes, from whichever field carries them
function bodyOf(v) {
  return v.svg_base64 != null ? Buffer.from(v.svg_base64, 'base64') : Buffer.from(v.svg, 'utf8');
}
const byName = {};
for (const v of wanted) byName[v.name] = v;

const CONTEXTS = [
  { name: 'direct', url: (n) => '/direct/' + n },
  { name: 'img', url: (n) => '/img/' + n },
  { name: 'inline', url: (n) => '/inline/' + n },
];

// the beacon and the three ways of showing one vector, on the fixed port the vectors name
let hits = new Set();
const server = http.createServer((req, res) => {
  const u = decodeURIComponent(new URL(req.url, 'http://127.0.0.1').pathname);
  const no = { 'cache-control': 'no-store, must-revalidate', 'content-length': undefined };
  res.setHeader('cache-control', 'no-store, must-revalidate');
  let m;
  if ((m = /^\/b\/([\w.:-]+)$/.exec(u))) {
    hits.add(m[1]);
    res.writeHead(200, { 'content-type': 'image/gif' });
    return res.end(GIF1x1);
  }
  if ((m = /^(?:\/direct\/|\/svg\/)([\w.:-]+)$/.exec(u)) && byName[m[1]]) {
    res.writeHead(200, { 'content-type': 'image/svg+xml' });
    return res.end(bodyOf(byName[m[1]]));
  }
  if ((m = /^\/img\/([\w.:-]+)$/.exec(u)) && byName[m[1]]) {
    res.writeHead(200, { 'content-type': 'text/html; charset=utf-8' });
    return res.end('<!doctype html><html><head><meta charset="utf-8"><title>img ' + m[1] + '</title></head><body>'
      + '<img src="/svg/' + m[1] + '" width="60" height="60" alt="the vector">'
      + '<pre>an SVG drawn by an img element</pre></body></html>\n');
  }
  if ((m = /^\/inline\/([\w.:-]+)$/.exec(u)) && byName[m[1]]) {
    // the file's own text, in the page's own body, which is how an inline SVG reaches a DOM
    res.writeHead(200, { 'content-type': 'text/html; charset=utf-8' });
    return res.end('<!doctype html><html><head><meta charset="utf-8"><title>inline ' + m[1] + '</title></head><body>'
      + bodyOf(byName[m[1]]).toString('utf8') + '<pre>an SVG inlined into an HTML page</pre></body></html>\n');
  }
  if (u === '/hits') {
    res.writeHead(200, { 'content-type': 'application/json' });
    return res.end(JSON.stringify([...hits].sort()));
  }
  res.writeHead(404, { 'content-type': 'text/plain; charset=utf-8' });
  res.end('no');
});

// one Chromium per vector per context, the flags the review's own probe drove it with
function open(url) {
  return new Promise((resolve) => {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'orb-svg-live-'));
    const args = ['--headless=new', '--no-sandbox', '--disable-gpu', '--disable-dev-shm-usage', '--no-first-run',
      '--user-data-dir=' + dir, '--virtual-time-budget=' + BUDGET, '--run-all-compositor-stages-before-draw',
      '--dump-dom', url];
    const child = spawn(CHROME, args, { stdio: ['ignore', 'ignore', 'pipe'] });
    let err = '', killed = false;
    child.stderr.on('data', (d) => { err += d; });
    const timer = setTimeout(() => { killed = true; try { child.kill('SIGKILL'); } catch (e) { /* gone */ } }, 30000);
    child.on('exit', () => {
      clearTimeout(timer);
      try { child.stderr.destroy(); } catch (e) { /* already gone */ }
      fs.rmSync(dir, { recursive: true, force: true });
      const bad = err.split('\n').filter((l) => l && !/ERROR:dbus|ERROR:gpu|ERROR:vaapi|ERROR:bluez|ERROR:field_trial|ERROR:viz_main|ERROR:angle|ERROR:media|Failed to connect to the bus/
        .test(l)).join(' ').slice(0, 200);
      resolve({ killed, err: bad });
    });
  });
}

// a few browsers at a time: the loads have to be independent, not serial, for a run over
// every candidate to finish in about a minute
async function pool(items, limit, fn) {
  const out = [];
  let next = 0;
  async function worker() {
    while (next < items.length) {
      const i = next++;
      out[i] = await fn(items[i], i);
    }
  }
  const workers = [];
  for (let i = 0; i < Math.min(limit, items.length); i++) workers.push(worker());
  await Promise.all(workers);
  return out;
}

const CHROME = (function () {
  const named = arg('chrome', process.env.CHROME || '');
  if (named) return fs.existsSync(named) ? named : null;
  for (const c of ['chromium', 'chromium-browser', 'google-chrome', 'google-chrome-stable']) {
    for (const dir of (process.env.PATH || '').split(path.delimiter)) {
      if (dir && fs.existsSync(path.join(dir, c))) return path.join(dir, c);
    }
  }
  return null;
})();
if (!CHROME) {
  console.log('no Chromium: pass --chrome <path>, set CHROME, or put chromium, chromium-browser or google-chrome on PATH');
  process.exit(2);
}

/* Only the vectors that can say anything here are driven: every accepted one (an accepted file
 * that stays quiet is the claim) and every vector that carries a beacon URL (a refused file that
 * fetches is the harness's own proof that it would have noticed). A refused vector with no URL
 * in it could not fetch anything whatever the rules said, so it adds no evidence. */
const candidates = wanted.filter((v) => v.rule === null || bodyOf(v).toString('latin1').indexOf('47613') >= 0);
const accepted = candidates.filter((v) => v.rule === null);
const refused = candidates.filter((v) => v.rule !== null);

(async () => {
  await new Promise((res) => server.listen(PORT, '127.0.0.1', res));
  const fetched = {};                       // vector name -> [contexts that fetched it]
  function mark(n, ctx) {
    if (!fetched[n]) fetched[n] = [];
    if (fetched[n].indexOf(ctx) < 0) fetched[n].push(ctx);
  }
  let strays = [];
  for (const ctx of CONTEXTS) {
    hits = new Set();
    let done = 0;
    await pool(candidates, 4, async (v) => {
      await open('http://127.0.0.1:' + PORT + ctx.url(v.name));
      done++;
      if (done % 15 === 0) process.stderr.write('  ' + ctx.name + ' ' + done + '/' + candidates.length + '\n');
    });
    for (const h of hits) {
      if (byName[h]) mark(h, ctx.name); else strays.push(h);
    }
  }
  server.close();

  const MIN_REFUSAL_FETCHES = 10;
  const acceptedFetched = accepted.filter((v) => fetched[v.name]);
  const refusedFetched = refused.filter((v) => fetched[v.name]);
  let fails = 0;
  for (const v of acceptedFetched) {
    fails++;
    console.log('FAIL ' + v.name + ' is accepted by the rules and still fetched, in ' + fetched[v.name].join(', '));
  }
  for (const v of refusedFetched) {
    if (process.env.VERBOSE) console.log('note ' + v.name + ' (' + v.rule + ') fetched in ' + fetched[v.name].join(', '));
  }
  if (strays.length) {
    fails++;
    console.log('FAIL the beacon was asked for by no vector this run drove: ' + [...new Set(strays)].join(', '));
  }
  if (refusedFetched.length < MIN_REFUSAL_FETCHES) {
    fails++;
    console.log('FAIL only ' + refusedFetched.length + ' refused vectors fetched; the harness has to see at least '
      + MIN_REFUSAL_FETCHES + ' to be able to say anything about the rest');
  }
  if (arg('json', null)) {
    fs.writeFileSync(arg('json', ''), JSON.stringify(fetched, null, 1) + '\n');
  }
  console.log('svg-fetch-live: accepted ' + acceptedFetched.length + ' fetched, refused '
    + refusedFetched.length + ' fetched (harness live)');
  process.exit(fails ? 1 : 0);
})().catch((e) => { console.log('FAIL threw: ' + ((e && e.stack) || e)); try { server.close(); } catch (e2) { /* not listening */ } process.exit(1); });
