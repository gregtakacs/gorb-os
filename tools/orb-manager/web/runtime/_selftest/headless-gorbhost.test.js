#!/usr/bin/env node
// theme@1 GorbHost seam (orb-os-net design §6.3): the one host description the page sets
// before the editor loads. It normalises (hostInfo), rebuilds hub paths (hostUrl), and is
// byte-identical in send.js and session.js; kind "hub" (the default) changes nothing, kind
// "web" puts the hub's paths under another base and turns Send to orb into a download.
//
//   node headless-gorbhost.test.js
'use strict';
const path = require('path');
const fs = require('fs');
const { spawnSync } = require('child_process');
let fails = 0, checked = 0;
const ok = (c, n, x) => { checked++; if (!c || process.env.VERBOSE) console.log((c ? 'PASS ' : 'FAIL ') + n + (x ? '  [' + x + ']' : '')); if (!c) fails++; };
setTimeout(() => { console.log('FAIL timed out (' + checked + ' checks run)'); process.exit(1); }, 20000);

// the window the runtime runs in: a storage store, an address, a parent, unload guards, a
// fetch that records every [method, url] and dispatches to the current handler, and an XHR
// whose construction is counted (a send: false post must build none).
const store = new Map();
global.window = globalThis;
global.localStorage = { getItem: (k) => (store.has(k) ? store.get(k) : null), setItem: (k, v) => store.set(k, String(v)), removeItem: (k) => store.delete(k) };
global.location = { origin: 'http://hub', search: '', pathname: '/' };
global.parent = { postMessage: () => {} };
global.addEventListener = () => {}; global.removeEventListener = () => {};
let calls = [], fetchHandler = () => { throw new Error('no fetch handler'); };
global.fetch = async (url, o) => { calls.push([(o && o.method) || 'GET', String(url)]); return fetchHandler(url, o); };
let xhr = null, xhrCount = 0;
global.XMLHttpRequest = function () {
  xhrCount++; xhr = this; this.upload = {}; this.status = 0; this.responseText = '';
  this.open = (m, u) => { this.url = u; }; this.setRequestHeader = () => {}; this.send = (b) => { this.body = b; };
  this.abort = () => { if (this.onabort) this.onabort(); };
};
const resp = (status, j) => ({ ok: status < 300, status, json: async () => j, text: async () => JSON.stringify(j), arrayBuffer: async () => new TextEncoder().encode(JSON.stringify(j)).buffer });

const fakeHub = require(path.join(__dirname, 'fake-hub.js'));
require(path.join(__dirname, '..', 'theme@1', 'send.js'));
require(path.join(__dirname, '..', 'theme@1', 'session.js'));
const tick = async (n) => { for (let i = 0; i < (n || 4); i++) await new Promise((r) => setImmediate(r)); };

// the same made-up "tint" app headless-session.test.js drives, so a scenario here is a scenario there
const mk = () => {
  const app = { state: { color: 'red', size: 10, name: '', status: '' }, applied: [], downloads: [] };
  app.set = (p) => Object.assign(app.state, p);
  app.ses = OrbTheme.session({
    slug: 'tint', keys: () => ['color', 'size', 'name'], state: () => app.state, set: app.set, ready: () => true,
    say: (t) => { app.state.status = t; }, restore: (snap, text) => app.set(Object.assign({}, snap, { status: text })),
    burstMs: () => -1, schema: () => 1, migrate: () => ({}),
    content: async () => ({ color: app.state.color, size: app.state.size, name: app.state.name }),
    apply: (o, from) => { if (!o.color) return 'that design has no tint in it'; app.applied.push(from); app.set({ color: o.color, size: o.size, name: o.name || '' }); return null; },
    download: (blob, name) => app.downloads.push({ blob, name }),
  });
  return app;
};
const doc = (color) => ({ format: 'wc-design', schema: 1, app: 'tint', appVersion: 'v1.3.0', color, size: 5, name: 'One' });
// a scripted session: boot, save, list, open, forget the draft, keep another, open a theme.
// Records every [method, url] the fetch stub saw; `map` strips a web base back to /api for the hub.
async function scenario(host, pathname, themeQuery, map) {
  if (host === undefined) delete global.GorbHost; else global.GorbHost = host;
  location.pathname = pathname; location.search = ''; store.clear();
  const themeDoc = new TextEncoder().encode(JSON.stringify(doc('opened')));
  const hub = fakeHub({ source: { relation: 'same' }, rest: () => ({ ok: true, status: 200, arrayBuffer: async () => themeDoc.buffer }) });
  calls = [];
  fetchHandler = (url, o) => hub.fetch(map ? String(url).replace(/^\/studio-api/, '/api') : url, o);
  const a = mk();
  await a.ses.boot();
  await a.ses.saveDraft({ name: 'One', doc: doc('teal') });
  const sawOne = !!a.ses.libraryEntry('One');
  await a.ses.libraryRefresh();
  await a.ses.openEntry('One');
  await a.ses.forgetEntry('One', 'draft');
  await a.ses.saveDraft({ name: 'Two', doc: doc('plum') });
  location.search = themeQuery;
  await a.ses.openTheme();
  return { calls: calls.slice(), hubOrb: a.ses.hubOrb(), sawOne, openedColor: a.state.color, applied: a.applied.slice(), status: a.state.status };
}

(async () => {
  // ---- 1. the normalisation table
  window.__ORB_APP = { slug: 'tint', version: 'v1.3.0' };
  {
    const H = (g) => { global.GorbHost = g; return OrbTheme.hostInfo(); };
    const is = (g, kind, api, send) => { const i = H(g); return i.kind === kind && i.api === api && i.send === send; };
    ok(is(undefined, 'hub', '/api', true), 'no GorbHost is a hub');
    ok(is(null, 'hub', '/api', true) && is([], 'hub', '/api', true) && is('web', 'hub', '/api', true), 'null, an array and a bare string are a hub');
    ok(is({}, 'hub', '/api', true) && is({ kind: 'hub' }, 'hub', '/api', true) && is({ kind: 'WEB' }, 'hub', '/api', true), 'kind hub, an unknown kind and a wrong-case kind are all hub');
    ok(is({ kind: 'web' }, 'web', '/studio-api', false), 'kind web is /studio-api and cannot send');
    ok(is({ kind: 'web', api: '/x/y' }, 'web', '/x/y', false), 'a clean api base is taken verbatim');
    ok(['https://x', '//x', '/a/', '/a//b', 'a', '/a b', '/a/../b', '/..', ''].every((a) => is({ kind: 'web', api: a }, 'web', '/studio-api', false)), 'every malformed api base falls back to the kind default');
    ok(is({ kind: 'hub', api: '/x' }, 'hub', '/x', true) && H({ kind: 'web', api: 42 }).api === '/studio-api', 'a good api rides either kind; a non-string falls back');
    ok(H({ kind: 'hub', send: false }).send === false, 'a hub may opt out of send');
    ok(H({ kind: 'web', send: true }).send === true, 'and a web host may opt in');
    const f = H({ publish: 'yes', inbox: 1 });
    ok(f.publish === false && f.inbox === false, 'publish and inbox are only true for the literal true', JSON.stringify(f));
    ok(Object.isFrozen(H({ kind: 'web' })), 'the host info comes back frozen');
    try { H({ kind: 'hub' }).kind = 'web'; } catch (e) { /* strict: throws */ }
    global.GorbHost = { kind: 'web' }; const a1 = OrbTheme.hostInfo().kind; global.GorbHost = { kind: 'hub' }; const a2 = OrbTheme.hostInfo().kind;
    ok(a1 === 'web' && a2 === 'hub', 'it is read fresh on every call, never cached', a1 + '/' + a2);
    delete global.GorbHost;
    ok(OrbTheme.hostUrl('/library/entries?app=clock') === '/api/library/entries?app=clock', 'hostUrl prefixes the hub base', OrbTheme.hostUrl('/library/entries?app=clock'));
    global.GorbHost = { kind: 'web' };
    ok(OrbTheme.hostUrl('/library/entries?app=clock') === '/studio-api/library/entries?app=clock', 'and the web base', OrbTheme.hostUrl('/library/entries?app=clock'));
    delete global.GorbHost;
  }

  // ---- 2. the block is shared between the two files
  {
    const dir = path.join(__dirname, '..', 'theme@1');
    const seg = (f) => { const t = fs.readFileSync(path.join(dir, f), 'utf8').split('\n'); const a = t.findIndex((l) => l.includes('// ---- GorbHost: keep this block')); const b = t.findIndex((l) => l.includes('// ---- end GorbHost')); return t.slice(a, b + 1).join('\n'); };
    ok(seg('session.js') === seg('send.js'), 'the text between the two marker lines is byte-identical in send.js and session.js');
    const sendPath = path.join(dir, 'send.js');
    const r = spawnSync(process.execPath, ['-e', 'require(' + JSON.stringify(sendPath) + ');var i=globalThis.OrbTheme.hostInfo();process.stdout.write(JSON.stringify([typeof globalThis.OrbTheme.hostInfo,typeof globalThis.OrbTheme.hostUrl,i.kind,i.api,i.send]))'], { encoding: 'utf8' });
    const got = JSON.parse(r.stdout || '[]');
    ok(r.status === 0 && got[0] === 'function' && got[1] === 'function' && got[2] === 'hub' && got[3] === '/api', 'send.js alone defines hostInfo/hostUrl (a fresh process)', (r.stdout || '') + (r.stderr || ''));
  }

  // ---- 3. the hub is unchanged across an absent, an explicit and a base-named hub block
  {
    const base = await scenario(undefined, '/editor/192.0.2.7/tint/', '?theme=abc123&src=shelf');
    const ex = await scenario({ kind: 'hub' }, '/editor/192.0.2.7/tint/', '?theme=abc123&src=shelf');
    const named = await scenario({ kind: 'hub', api: '/api' }, '/editor/192.0.2.7/tint/', '?theme=abc123&src=shelf');
    ok(JSON.stringify(base.calls) === JSON.stringify(ex.calls) && JSON.stringify(ex.calls) === JSON.stringify(named.calls), 'an absent, an explicit and a base-named hub ask the same requests');
    ok(base.calls.every((c) => c[1].indexOf('/api/') === 0), 'and every hub url starts with /api/', base.calls.map((c) => c[1]).join(' '));
    ok(base.sawOne && base.openedColor === 'opened' && base.applied[base.applied.length - 1] === 'the hub’s shelf', 'the draft round-trips and the hub’s Edit door still opens a shelf theme');
    global.__hubBase = base.calls;
  }

  // ---- 4. web mode: same behaviour at /studio-api, no editor/source, the theme opens by id
  {
    const w = await scenario({ kind: 'web', api: '/studio-api', send: false, publish: true, inbox: true }, '/v1.4.2/', '?theme=abc123', true);
    const keep = global.__hubBase.filter((c) => !/editor\/source/.test(c[1]));
    const want = keep.map((c) => [c[0], /\/theme\/design/.test(c[1]) ? '/studio-api/theme/design?id=abc123' : '/studio-api' + c[1].slice(4)]);
    ok(w.calls.every((c) => c[1].indexOf('/studio-api/') === 0) && w.calls.every((c) => c[1].indexOf('/api/') !== 0), 'every web url is under /studio-api and none is under /api', w.calls.map((c) => c[1]).join(' '));
    ok(!w.calls.some((c) => /editor\/source/.test(c[1])), 'a web host asks no editor/source');
    ok(JSON.stringify(w.calls) === JSON.stringify(want), 'the web recording is the hub’s moved under /studio-api, minus editor/source, theme opened by id', JSON.stringify(w.calls) + ' vs ' + JSON.stringify(want));
    ok(w.hubOrb && w.hubOrb.slug === 'tint' && w.hubOrb.ip === null, 'hubOrb is truthy under web, so the library UI works unchanged', JSON.stringify(w.hubOrb));
    ok(w.sawOne && w.applied[0] === 'the library', 'the draft round-trips on the site’s library');
    ok(w.status === 'Opening the design…', 'the web open’s status line is verbatim Opening the design…', w.status);
    ok(w.applied[w.applied.length - 1] === 'the design', 'a web theme open applies the design from “the design”', w.applied.join(','));
  }
  // ---- 5. sender with send: false: a download, never a request
  {
    global.GorbHost = { kind: 'web', send: false };
    const mkH = () => { const h = { status: '', dup: null, sending: null, orbSeen: 'unset', downloads: [] }; h.s = OrbTheme.sender({ say: (t) => { h.status = t; }, ask: (d) => { h.dup = d; }, sending: (v) => { h.sending = v; }, orb: (ip) => { h.orbSeen = ip; }, download: (b, n) => h.downloads.push({ b, n }) }); return h; };
    const bake = (n) => new Uint8Array(n || 4096);
    fetchHandler = () => { throw new Error('a send: false post must not fetch'); };
    calls = [];
    const h = mkH();
    ok(await h.s.findOrb() === null && h.orbSeen === null && calls.length === 0, 'findOrb resolves null, reports no orb and asks nothing');
    location.search = '?orb=192.0.2.9';
    const hn = mkH();
    ok(await hn.s.findOrb() === null && hn.orbSeen === null && calls.length === 0, 'and an ?orb= is ignored');
    location.search = '';
    xhrCount = 0;
    const p = mkH();
    await p.s.post(bake(), { name: 'Dial', stem: 'clock-dial-000000000000', summary: 'x' }, false);
    ok(calls.length === 0 && xhrCount === 0 && p.downloads.length === 1, 'a post makes zero fetches, builds no XHR, and downloads once', calls.length + '/' + xhrCount + '/' + p.downloads.length);
    const blob = p.downloads[0].b, bb = new Uint8Array(await blob.arrayBuffer());
    ok(blob.type === 'application/octet-stream' && bb.length === 4096 && bb.every((v) => v === 0), 'the download carries the exact bake bytes', blob.type + '/' + bb.length);
    ok(p.downloads[0].n === 'clock-dial-000000000000.orbtheme', 'named from the stem', p.downloads[0].n);
    ok(p.status === 'Downloaded "Dial" as clock-dial-000000000000.orbtheme — import it in Orb Manager to send it to an orb', 'and the exact status line', p.status);
    const pn = mkH();
    await pn.s.post(bake(8), { name: 'Glider', stem: null, summary: '' }, false);
    ok(pn.downloads[0].n === 'Glider.orbtheme', 'a null stem names it from the name', pn.downloads[0].n);
    const p2 = mkH();
    await p2.s.send('Dial', async () => { await p2.s.post(bake(16), { name: 'Dial', stem: 'd', summary: '' }, false); });
    ok(p2.s.active === false && p2.downloads.length === 1, 'a send runs as one modal act and ends in the download', p2.s.active);
  }

  // ---- 6. a web host that can send: the same flow, at the studio-api base
  {
    global.GorbHost = { kind: 'web', send: true, api: '/studio-api' };
    fetchHandler = (url) => {
      if (/orb\/known/.test(url)) return resp(200, { ok: true, data: { ip: '192.0.2.9' } });
      if (/precheck=1/.test(url)) return resp(200, { ok: true, data: { duplicate: false } });
      if (/upload-status/.test(url)) return resp(200, { ok: true, data: { phase: 'idle' } });
      return resp(404, { ok: false, error: 'no route' });
    };
    calls = []; xhr = null;
    const h = OrbTheme.sender({ say: () => {}, ask: () => {}, sending: () => {} });
    ok(await h.findOrb() === '192.0.2.9' && calls[0][1] === '/studio-api/orb/known', 'findOrb asks the studio-api base', calls[0] && calls[0][1]);
    xhr = null; calls = [];
    const bytes = (function () { const b = new Uint8Array(4096); new DataView(b.buffer).setUint32(20, 200, true); return b; })();
    const run = h.send('Dial', () => h.post(bytes, { name: 'Dial', stem: 'd', summary: '' }, false));
    await tick();
    const pre = calls.find((c) => /precheck=1/.test(c[1]));
    ok(pre && pre[1] === '/studio-api/theme/upload?ip=192.0.2.9&precheck=1', 'a post asks the studio-api upload door with the precheck first', pre && pre[1]);
    if (xhr) { xhr.status = 200; xhr.responseText = JSON.stringify({ ok: true, data: { store: 'stored', back: true } }); xhr.onload(); }
    await run;
  }
  delete global.GorbHost;

  console.log(fails ? 'RESULT: FAIL — ' + fails + ' of ' + checked + ' checks' : 'all green (' + checked + ' checks)');
  process.exit(fails ? 1 : 0);
})().catch((e) => { console.log('FAIL threw: ' + (e && e.stack || e)); process.exit(1); });
