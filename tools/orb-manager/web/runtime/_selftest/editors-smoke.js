#!/usr/bin/env node
// editors-smoke.js — the shelf's editors, opened exactly as a gorb-os release ships them.
//
//   node editors-smoke.js --runtime <dir> --editors <dir> [--chrome <path>] [--only <slug>]
//
// <runtime> is an `orb-manager export-runtime` tree and <editors> an `orb-manager
// extract-editors` tree — what `orb-web.sh` packed and a site then unpacked, so this is the
// release file's own layout being loaded, not the sources it was built from. Every editor
// the report says `ok` is opened in headless Chromium three times: with no `window.GorbHost`
// and with `{kind: "hub"}`, both at the hub's own URL shape, and with `{kind: "web"}` at the
// site's. The hub pair must behave identically and ask for identical routes, and the web run
// must never reach a hub route at all: that is the seam orb-os-net design §6.3 promises, and
// the one thing no suite inside this repo proves, because they all drive the sources.
//
// Deliberately not named headless-*.test.js: headless-test.js runs suites against the tree,
// with no browser and no unpacked editor, and this needs both. `make check-web` runs it.
// The hub's /editor/<ip>/<slug>/editor.json answers the entry document (serveEditor's
// manifest branch), so this server mirrors that rather than the file on disk; no editor is
// expected to ask.
'use strict';
const fs = require('fs');
const http = require('http');
const os = require('os');
const path = require('path');
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
if (!RUNTIME || !EDITORS) {
  console.log('usage: editors-smoke.js --runtime <export-runtime dir> --editors <extract-editors dir> [--chrome <path>] [--only <slug>]');
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

// what the page posts back from /__smoke: what went wrong, which host the runtime saw, how
// much of an editor it painted, and its title. The title and the node count are the proof it
// drew a studio rather than a blank page; the host object is the seam §6.3 is made of.
//
// One error event is swallowed on purpose: a parser-eager `<img src="{{ r.thumb }}">` inside
// an unrendered sc-if block. Its braces are a template that Babel has not walked yet, and the
// hub 404s exactly the same request — `/editor/<ip>/<slug>/{{ r.thumb }}` is no file in any
// editor. Every case is logged by the server anyway, so the stray would still show in check 7.
const PROBE = "(function(){var e=[];function p(m){if(e.length<40)e.push(String(m));}"
  + "window.addEventListener('error',function(ev){var t=ev.target;"
  + "var src=(t&&(t.src||t.href))||'';if(src.indexOf('{{')>=0||src.indexOf('%7B%7B')>=0)return;"
  + "p(ev&&ev.message?'uncaught: '+ev.message:'resource failed: '+src);},true);"
  + "window.addEventListener('unhandledrejection',function(ev){p('rejected: '"
  + "+((ev&&ev.reason&&(ev.reason.message||ev.reason))||'?'));});"
  + "window.addEventListener('load',function(){setTimeout(function(){try{"
  + "var h=(window.OrbTheme&&OrbTheme.hostInfo)?OrbTheme.hostInfo():null;"
  + "var n=document.body?document.body.getElementsByTagName('*').length:0;"
  + "fetch('/__smoke',{method:'POST',headers:{'content-type':'application/json'},"
  + "body:JSON.stringify({errors:e,host:h,nodes:n,title:document.title})});}catch(x){}},6000);});})();";

// one page load at a time: the requests it made, its fake hub, its report
let load = null;
const editorManifest = 'editor.json';
let bySlug = {};

const readTree = (root, rel) => {
  if (!rel || rel.split('/').some((s) => s === '' || s === '.' || s === '..')) return null;
  const f = path.join(root, rel);
  if (!f.startsWith(root + path.sep)) return null;
  try { return fs.statSync(f).isFile() ? fs.readFileSync(f) : null; } catch (e) { return null; }
};
const send = (res, status, type, body) => { res.writeHead(status, { 'content-type': type, 'cache-control': 'no-store' }); res.end(body); };
const json = (res, status, v) => send(res, status, 'application/json; charset=utf-8', JSON.stringify(v));

// the hub answers a bare /editor/<ip>/<slug>/ — and its editor.json — with the entry
// document, not the manifest; the same here, so an editor that ever asked gets what the hub
// would have given it.
function entryDocument(slug) {
  const row = bySlug[slug];
  if (!row) return null;
  return readTree(path.join(EDITORS, slug), row.editor.entry);
}
function inject(doc) {
  const tag = load.host === undefined ? '' : '<script>window.GorbHost = ' + JSON.stringify(load.host) + ';</script>';
  const scripts = tag + '<script>' + PROBE + '</script>';
  const m = /<head[^>]*>/i.exec(doc.toString('utf8'));
  if (!m) return scripts + doc.toString('utf8');
  return doc.toString('utf8').slice(0, m.index + m[0].length) + scripts + doc.toString('utf8').slice(m.index + m[0].length);
}

// the hub's library routes, one instance per page load, and the two hub doors the editors
// knock on that the fake does not cover.
function apiRoute(res, url, body) {
  const hub = load.hub || (load.hub = fakeHub({
    source: { relation: 'same' },
    rest: () => ({ status: 404, json: async () => ({ ok: false, error: 'no route' }) }),
  }));
  return hub.fetch(url.replace(/^\/studio-api/, '/api'), body)
    .then((ans) => ans.json().then((v) => json(res, ans.status, v)));
}

const server = http.createServer((req, res) => {
  const u = new URL(req.url, 'http://127.0.0.1');
  const p = u.pathname;
  load.requests.push(req.method + ' ' + req.url);
  if (req.method === 'POST' && p === '/__smoke') {
    let text = '';
    req.on('data', (c) => { text += c; });
    req.on('end', () => { try { load.report = JSON.parse(text); } catch (e) { load.report = { errors: ['the probe posted nothing: ' + text.slice(0, 80)] }; } });
    return json(res, 204, { ok: true });
  }
  if (p.startsWith('/runtime/')) {
    const f = readTree(RUNTIME, p.slice('/runtime/'.length));
    return f ? send(res, 200, typeOf(p), f) : send(res, 404, 'text/plain; charset=utf-8', 'not found');
  }
  const hubDir = /^\/editor\/192\.0\.2\.7\/([a-z0-9_-]{1,23})\/(.*)$/.exec(p);
  const webDir = /^\/studio\/([a-z0-9_-]{1,23})\/(.*)$/.exec(p);
  const m = hubDir || webDir;
  if (m && bySlug[m[1]] && req.method === 'GET') {
    const slug = m[1], file = m[2];
    if (file === '' || file === editorManifest) {
      const doc = entryDocument(slug);
      if (!doc) return send(res, 404, 'text/plain; charset=utf-8', 'not found');
      return send(res, 200, 'text/html; charset=utf-8', inject(doc));
    }    const f = readTree(path.join(EDITORS, slug), file);
    return f ? send(res, 200, typeOf(file), f) : send(res, 404, 'text/plain; charset=utf-8', 'not found');
  }
  if (p === '/api/orb/known') return json(res, 200, { ok: true, data: { ip: null } });
  if (p === '/api/theme/stem') return json(res, 400, { ok: false, error: 'no stem here' });
  if (p.startsWith('/api/') || p.startsWith('/studio-api/')) {
    let body = '';
    req.on('data', (c) => { body += c; });
    req.on('end', () => apiRoute(res, p + (u.search || ''), { method: req.method, body }));
    return undefined;
  }
  return json(res, 404, { ok: false, error: 'no route' });
});

/* ---- the cases. `none` is what a hub page looks like today (no GorbHost at all), `hub`
 * says so out loud, and `web` is the site's description at the site's URL shape: a page for
 * which every hub route is a bug. */
const HUB_INFO = { kind: 'hub', api: '/api', send: true, publish: false, inbox: false };
const CASES = [
  { name: 'none', host: undefined, base: '/editor/' + IP + '/' },
  { name: 'hub', host: { kind: 'hub' }, base: '/editor/' + IP + '/' },
  { name: 'web', host: { kind: 'web', api: '/studio-api', send: false, publish: true, inbox: true }, base: '/studio/' },
];
const WEB_INFO = CASES[2].host;

let fails = 0, checked = 0;
const ok = (c, n, x) => {
  checked++;
  if (!c || process.env.VERBOSE) console.log((c ? 'PASS ' : 'FAIL ') + n + (x ? '  [' + x + ']' : ''));
  if (!c) fails++;
};
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// one Chromium, one page, one dump-dom: a fresh profile per run because a shared one carries
// the previous load's storage into the next check.
function open(url) {
  return new Promise((resolve) => {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'orb-smoke-'));
    const args = ['--headless=new', '--no-sandbox', '--disable-gpu', '--no-first-run',
      '--user-data-dir=' + dir, '--virtual-time-budget=20000', '--dump-dom', url];
    const child = spawn(CHROME, args, { stdio: ['ignore', 'pipe', 'pipe'] });
    let dom = '', err = '', killed = false;
    child.stdout.on('data', (d) => { dom += d; });
    child.stderr.on('data', (d) => { err += d; });
    const timer = setTimeout(() => { killed = true; try { child.kill('SIGKILL'); } catch (e) { /* gone */ } }, 120000);
    child.on('exit', (code) => {
      clearTimeout(timer);
      try { child.stdout.destroy(); child.stderr.destroy(); } catch (e) { /* already gone */ }
      fs.rmSync(dir, { recursive: true, force: true });
      // Chromium's own complaints about a container with no dbus and no GPU are not the
      // editor's; only a line naming a real failure is worth a FAIL line's detail.
      const bad = err.split('\n').filter((l) => l && !/ERROR:dbus|ERROR:gpu|ERROR:vaapi|ERROR:bluez|ERROR:field_trial|ERROR:viz_main|ERROR:angle|ERROR:media/
        .test(l)).join(' ').slice(0, 300);
      resolve({ code, killed, dom, err: bad });
    });
  });
}

// the probe posts when the page has settled; the browser can be gone before that lands
async function reported() {
  for (let i = 0; i < 40 && !load.report; i++) await sleep(50);
  return load.report;
}
const paths = (reqs, prefix) => [...new Set(reqs.filter((r) => r.split(' ')[1].split('?')[0].startsWith(prefix))
  .map((r) => r.split(' ')[0] + ' ' + r.split(' ')[1].split('?')[0]))].sort();

async function smoke(row) {
  const seen = {};
  for (const c of CASES) {
    load = { requests: [], report: null, hub: null, host: c.host };
    const r = await open('http://127.0.0.1:' + server.address().port + c.base + row.slug + '/');
    seen[c.name] = { requests: load.requests.slice(), report: await reported(), run: r };
    if (process.env.VERBOSE) console.log('  ' + c.name + ': ' + seen[c.name].requests.join(' '));
    const at = row.slug + ' ' + c.name;
    const where = (r.killed ? 'killed at 120s; ' : 'exit ' + r.code + '; ') + r.dom.length + ' B dumped'
      + (r.err ? '; ' + r.err.slice(0, 200) : '');
    ok(!!seen[c.name].report, at + ': the page ran and reported', where);
    if (!seen[c.name].report) continue;
    const rep = seen[c.name].report;
    ok(rep.errors.length === 0, at + ': nothing threw, no subresource 404ed', rep.errors.join(' | '));
    // A stub editor (radar until R7) is a heading, a paragraph and a canvas: six elements, so a
    // fixed node floor would fail a page that loaded perfectly. The title is the honest proof
    // the entry document arrived and ran, and the studio's own DOM size is checked on top of it
    // wherever the theme runtime — and therefore a studio — is in play.
    ok(rep.nodes > 0 && rep.title === entryTitle(row),
      at + ': the editor drew its own page (' + rep.nodes + ' nodes, "' + rep.title + '")', 'want "' + entryTitle(row) + '"');
    if (usesTheme(row)) {
      ok(rep.nodes > 20, at + ': the studio runtime painted into it (' + rep.nodes + ' nodes)', rep.title);
    }
    if (usesTheme(row)) {
      const want = c.name === 'web' ? WEB_INFO : HUB_INFO;
      ok(rep.host && Object.keys(want).every((k) => rep.host[k] === want[k]),
        at + ': the runtime read the host as ' + JSON.stringify(want), JSON.stringify(rep.host));
    }
    const folder = c.base + row.slug + '/';
    const bad = load.requests.filter((q) => {
      const p = q.split(' ')[1].split('?')[0];
      // the hub's own orb routes (host@1 reads the orb's home and places) are the hub's API;
      // a site's editor has no orb, so under the web host they are outside it
      return !(p.startsWith('/runtime/') || p.startsWith(folder) || p.startsWith('/api/') ||
        (c.name !== 'web' && /^\/orb\/\d{1,3}(\.\d{1,3}){3}\/api\//.test(p)) ||
        p.startsWith('/studio-api/') || p === '/__smoke' || p === '/favicon.ico');
    });
    ok(bad.length === 0, at + ': asked for nothing outside its own editor, the runtime and the API', bad.join(' '));
  }
  const at = row.slug;
  const same = JSON.stringify(paths(seen.none.requests, '/api/')) === JSON.stringify(paths(seen.hub.requests, '/api/'));
  ok(same, at + ' none/hub: an explicit hub host asks for exactly what no host description asks',
    paths(seen.none.requests, '/api/').join(' ') + ' vs ' + paths(seen.hub.requests, '/api/').join(' '));
  const web = seen.web.requests;
  ok(!web.some((q) => q.split(' ')[1].split('?')[0].startsWith('/api/')),
    at + ' web: not one request to a hub route', web.filter((q) => q.includes(' /api/')).join(' '));
  ok(!web.some((q) => q.includes('editor/source')), at + ' web: no editor/source, which is a hub’s shelf question');
  if (usesTheme(row)) {
    ok(web.some((q) => q.startsWith('GET /studio-api/library/entries')),
      at + ' web: the library was read at the site’s own base', paths(web, '/studio-api/').join(' '));
  }
}
const usesTheme = (row) => (row.editor.runtime || []).includes('theme@1');

// the <title> the entry document declares, as it sits in the unpacked editor
const titles = {};
function entryTitle(row) {
  const slug = row.slug;
  if (!(slug in titles)) {
    const html = fs.readFileSync(path.join(EDITORS, slug, row.editor.entry), 'utf8');
    const m = html.match(/<title[^>]*>([^<]*)<\/title>/i);
    titles[slug] = m ? m[1].trim() : '';
  }
  return titles[slug];
}

const report = JSON.parse(fs.readFileSync(path.join(EDITORS, 'editors.json'), 'utf8'));
const rows = (report.apps || []).filter((a) => a.status === 'ok' && (!ONLY || a.slug === ONLY));

(async () => {
  if (!rows.length) { console.log('no editor to smoke in ' + EDITORS + (ONLY ? ' named ' + ONLY : '')); process.exit(1); }
  bySlug = {};
  for (const r of rows) bySlug[r.slug] = r;
  await new Promise((res) => server.listen(0, '127.0.0.1', res));
  for (const row of rows) await smoke(row);
  server.close();
  console.log(fails ? fails + ' FAILED' : 'all green (' + checked + ' checks)');
  process.exit(fails ? 1 : 0);
})().catch((e) => { console.log('FAIL threw: ' + ((e && e.stack) || e)); server.close(); process.exit(1); });

