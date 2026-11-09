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
  // ---- 7. the two site flags
  {
    const H = (g) => { global.GorbHost = g; return OrbTheme.hostInfo(); };
    const w = H({ kind: 'web', api: '/studio-api', send: false, publish: false, inbox: false, assets: true, designIds: true });
    ok(w.assets === true && w.designIds === true, 'the site’s assets and designIds flags read true', JSON.stringify(w));
    const h = H({ kind: 'hub' });
    ok(h.assets === false && h.designIds === false, 'a hub sets neither', JSON.stringify(h));
    const n = H(undefined);
    ok(n.assets === false && n.designIds === false, 'and neither does a page with no GorbHost');
    const s = H({ kind: 'web', assets: 'yes', designIds: 1 });
    ok(s.assets === false && s.designIds === false, 'only the literal true counts, for either flag', JSON.stringify(s));
    delete global.GorbHost;
  }

  // the site's fake: the library, an entry's design in stored form, one asset per hash,
  // saves answered with a design id, and a shared bundle. Bodies it was posted are kept.
  const siteFetch = (cfg) => {
    const st = { bodies: [], assets: cfg.assets || new Map() };
    const res = (status, body, raw) => ({
      ok: status < 300, status,
      json: async () => (raw !== undefined ? JSON.parse(new TextDecoder().decode(raw)) : JSON.parse(JSON.stringify(body))),
      arrayBuffer: async () => (raw !== undefined ? raw.buffer : new TextEncoder().encode(JSON.stringify(body)).buffer),
    });
    st.fetch = async (url, o) => {
      const u = new URL(String(url), 'http://site');
      if (u.pathname === '/studio-api/library/entries') return res(200, { ok: true, data: cfg.entries || [] });
      if (u.pathname === '/studio-api/library/design') { const d = (cfg.designs || {})[u.searchParams.get('name')]; return d ? res(200, d) : res(404, { ok: false, error: 'no design for that name' }); }
      if (u.pathname === '/studio-api/theme/design') { const d = (cfg.themes || {})[u.searchParams.get('id')]; return d ? res(200, d) : res(404, { ok: false, error: 'no design with that id' }); }
      if (u.pathname === '/studio-api/asset') { const b = st.assets.get(u.searchParams.get('sha256')); return b ? res(200, undefined, b) : res(404, { ok: false, error: 'no such asset' }); }
      if (u.pathname === '/studio-api/shared/bundle') return cfg.bundleStatus ? res(cfg.bundleStatus, { ok: false, error: cfg.bundleError }) : res(200, cfg.bundle);
      if (u.pathname === '/studio-api/design/save') {
        let d = null; try { d = JSON.parse(o.body); } catch (e) { return res(400, { ok: false, error: 'that is not a design document' }); }
        st.bodies.push(d);
        const a = cfg.save ? cfg.save(String(url), d, st.bodies.length - 1) : { design: 'D' + st.bodies.length };
        if (a.status) return res(a.status, { ok: false, error: a.error });
        return res(200, { ok: true, data: Object.assign({ name: u.searchParams.get('name') }, a.design ? { design: a.design } : {}) });
      }
      return res(404, { ok: false, error: 'no route ' + u.pathname });
    };
    return st;
  };
  // the same app again, this time with art: a design's `art` is strings, some of them images
  const mkArt = (extra) => {
    const app = { state: { color: 'red', size: 10, name: '', art: null, status: '' }, applied: [], docs: [], downloads: [], baked: [] };
    app.set = (p) => Object.assign(app.state, p);
    app.ses = OrbTheme.session(Object.assign({
      slug: 'tint', keys: () => ['color', 'size', 'name', 'art'], state: () => app.state, set: app.set, ready: () => true,
      say: (t) => { app.state.status = t; }, restore: (snap, text) => app.set(Object.assign({}, snap, { status: text })),
      burstMs: () => -1, schema: () => 1, migrate: () => ({}),
      content: async () => ({ color: app.state.color, size: app.state.size, name: app.state.name, art: app.state.art }),
      apply: (o, from) => { if (!o.color) return 'that design has no tint in it'; app.applied.push(from); app.docs.push(o); app.set({ color: o.color, size: o.size, name: o.name || '', art: o.art }); return null; },
      download: (blob, name) => app.downloads.push({ blob, name }),
    }, extra || {}));
    return app;
  };
  const sha256 = async (b) => Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256', b))).map((x) => (x < 16 ? '0' : '') + x.toString(16)).join('');
  const imgBytes = (n, seed) => { const u = new Uint8Array(n); for (let i = 0; i < n; i++) u[i] = (seed * 31 + i * 7) & 0xff; return u; };
  const dataUrl = (b, prefix) => (prefix || 'data:image/png;base64') + ',' + btoa(Array.from(b, (x) => String.fromCharCode(x)).join(''));
  const siteDoc = (nm) => ({ format: 'wc-design', schema: 1, app: 'tint', appVersion: 'v1.3.0', color: 'teal', size: 5, name: nm });
  const saveUrl = (list) => (list.filter((c) => /design\/save/.test(c[1])).pop() || ['', ''])[1];
  // ---- 8. art by hash: a stored design inflates, and a save answers in refs
  {
    global.GorbHost = { kind: 'web', api: '/studio-api', send: false, assets: true, designIds: true };
    location.pathname = '/v1.4.2/'; location.search = '';
    const b1 = imgBytes(600, 3), b2 = imgBytes(512, 5), h1 = await sha256(b1), h2 = await sha256(b2);
    const u1 = dataUrl(b1), u2 = dataUrl(b2, 'data:image/jpeg;base64');
    const stored = () => ({ format: 'wc-design', schema: 1, app: 'tint', appVersion: 'v1.3.0', color: 'teal', size: 5, name: 'Two pics',
      art: ['orb-asset:' + h1, { deep: 'orb-asset:' + h2, note: 'data:,' }],
      orbAssets: [{ sha256: h2, prefix: 'data:image/jpeg;base64', size: 512 }, { sha256: h1, prefix: 'data:image/png;base64', size: 600 }] });
    const two = (assets, entries) => siteFetch({ entries: entries === undefined ? [{ name: 'Two pics', state: 'draft', at: 1, design: 'D1' }] : entries, designs: { 'Two pics': stored() }, assets: assets });
    const open = async (site) => { calls = []; fetchHandler = (url, o) => site.fetch(url, o); const a = mkArt(); await a.ses.boot(); const r = await a.ses.openEntry('Two pics'); return { a, r, urls: calls.map((c) => c[1]) }; };
    const site = two(new Map([[h1, b1], [h2, b2]]));
    const o1 = await open(site);
    const assetUrls = o1.urls.filter((x) => /\/asset\?sha256=/.test(x));
    ok(o1.r && o1.a.state.art[0] === u1 && o1.a.state.art[1].deep === u2, 'an entry stored in refs opens with its images inline, at any depth', o1.a.state.status);
    ok(/refs=1/.test(o1.urls.filter((x) => /library\/design/.test(x))[0] || ''), 'and the read asked for the ref form', o1.urls.join(' '));
    ok(assetUrls.length === 2 && assetUrls[0] === '/studio-api/asset?sha256=' + h2 && assetUrls[1] === '/studio-api/asset?sha256=' + h1 && o1.urls[1] === '/studio-api/library/design?app=tint&name=Two%20pics&refs=1',
      'one fetch per sidecar entry, in its order, after the design read', JSON.stringify(o1.urls));
    ok(!('orbAssets' in o1.a.docs[0]) && o1.a.docs[0].art[1].note === 'data:,', 'the sidecar is gone before the app sees the design, and a non-asset string was left alone');
    ok(JSON.stringify(o1.a.ses.opened) === JSON.stringify({ id: 'D1', name: 'Two pics' }), 'the entry’s design id is what the session holds now', JSON.stringify(o1.a.ses.opened));
    await o1.a.ses.saveDraft({ name: 'Two pics', doc: await o1.a.ses.doc(true) });
    const body1 = site.bodies[0];
    ok(body1.art[0] === 'orb-asset:' + h1 && body1.art[1].deep === 'orb-asset:' + h2, 'saving it unchanged posts the two refs again', JSON.stringify(body1.art));
    const want2 = [{ sha256: h1, prefix: 'data:image/png;base64', size: 600 }, { sha256: h2, prefix: 'data:image/jpeg;base64', size: 512 }].sort((a, b) => (a.sha256 < b.sha256 ? -1 : 1));
    ok(JSON.stringify(body1.orbAssets) === JSON.stringify(want2),
      'with their sidecar, one entry per hash and sorted by it', JSON.stringify(body1.orbAssets));
    ok(JSON.stringify(body1).indexOf(';base64,') < 0, 'and not one image byte in the body');
    ok(/&id=D1(&|$)/.test(saveUrl(calls)), 'the save went to the design it opened', saveUrl(calls));
    const b3 = imgBytes(700, 9), h3 = await sha256(b3), u3 = dataUrl(b3);
    const b4 = imgBytes(800, 13), h4 = await sha256(b4), u4 = dataUrl(b4);
    o1.a.set({ art: [u4, u4] });
    await o1.a.ses.saveDraft({ name: 'Two pics', doc: await o1.a.ses.doc(true) });
    ok(site.bodies[1].art[0] === u4 && site.bodies[1].art[1] === u4, 'the same image twice still rides inline the first time it is saved');
    await o1.a.ses.saveDraft({ name: 'Two pics', doc: await o1.a.ses.doc(true) });
    ok(site.bodies[2].art[0] === 'orb-asset:' + h4 && site.bodies[2].art[1] === 'orb-asset:' + h4 && site.bodies[2].orbAssets.length === 1,
      'and is one ref twice over, with one sidecar entry', JSON.stringify(site.bodies[2].orbAssets));
    o1.a.set({ art: [u1, { deep: u3, note: 'data:,' }] });
    await o1.a.ses.saveDraft({ name: 'Two pics', doc: await o1.a.ses.doc(true) });
    const body2 = site.bodies[3];
    ok(body2.art[0] === 'orb-asset:' + h1 && body2.art[1].deep === u3 && body2.orbAssets.length === 1,
      'one new image posts one ref and one upload', JSON.stringify(body2.orbAssets));
    await o1.a.ses.saveDraft({ name: 'Two pics', doc: await o1.a.ses.doc(true) });
    ok(site.bodies[4].art[1].deep === 'orb-asset:' + h3 && site.bodies[4].orbAssets.length === 2, 'and the next save has learned it too', JSON.stringify(site.bodies[4].orbAssets));
    // the refusals: changed bytes, and an image the site does not answer
    const bad = await open(two(new Map([[h1, imgBytes(601, 3)], [h2, b2]])));
    ok(!bad.r && bad.a.state.status === 'Could not open “Two pics”: an image of this design came back changed' && !bad.a.applied.length,
      'bytes whose hash is not the one the design named refuse the open, and nothing is applied', bad.a.state.status);
    // a sidecar that is not a hash, and one image the design names twice
    const jdoc = siteFetch({ entries: [{ name: 'Two pics', state: 'draft', at: 1, design: 'D1' }], designs: { 'Two pics': Object.assign(stored(), { orbAssets: [{ sha256: 'nope' }, { sha256: h1.toUpperCase(), prefix: 'data:image/png;base64', size: 600 }] }) }, assets: new Map([[h1, b1]]) });
    calls = []; fetchHandler = (url, o) => jdoc.fetch(url, o);
    const g = mkArt(); await g.ses.boot();
    ok(await g.ses.openEntry('Two pics') && g.docs[0].art[1].deep === 'orb-asset:' + h2 && calls.filter((c) => /\/asset\?/.test(c[1])).length === 0,
      'a sidecar entry that is not a lower-case hex hash is left as the ref it is, and its image is never asked for', calls.map((c) => c[1]).join(' '));
    await g.ses.saveDraft({ name: 'Two pics', doc: await g.ses.doc(true) });
    ok(jdoc.bodies[0].art[0] === 'orb-asset:' + h1 && !('orbAssets' in jdoc.bodies[0]),
      'and a save posts a ref this session never inflated back as it stands, with no sidecar', JSON.stringify(jdoc.bodies[0].art));
    const gone = await open(two(new Map([[h2, b2]])));
    ok(!gone.r && gone.a.state.status === 'Could not open “Two pics”: could not fetch an image of this design (404)' && !gone.a.applied.length,
      'and an image the site does not answer says so with its status', gone.a.state.status);
    // no sidecar: nothing extra is asked, and a save with nothing to say about art is today's
    calls = []; fetchHandler = (url, o) => { const s = siteFetch({ entries: [{ name: 'One', state: 'draft', at: 1, design: 'D9' }] , designs: { One: siteDoc('teal') } }); return s.fetch(url, o); };
    const a4 = mkArt(); await a4.ses.boot();
    ok(await a4.ses.openEntry('One') && !/\/asset\?/.test(calls.map((c) => c[1]).join(' ')) && a4.state.color === 'teal',
      'a design stored with no sidecar asks for no image', calls.map((c) => c[1]).join(' '));
  }
  // ---- 9. saves by design id
  {
    location.pathname = '/v1.4.2/'; location.search = '';
    global.GorbHost = { kind: 'web', api: '/studio-api', designIds: true };
    const site = siteFetch({ entries: [], save: (u, b, i) => ({ design: ['D1', 'D2', 'D3'][i] }) });
    calls = []; fetchHandler = (url, o) => site.fetch(url, o);
    const a = mkArt(); await a.ses.boot();
    ok(await a.ses.saveDraft({ name: 'One', doc: siteDoc('One') }) && /&saveAs=1/.test(saveUrl(calls)), 'a fresh editor’s save asks for a new design', saveUrl(calls));
    ok(JSON.stringify(a.ses.opened) === JSON.stringify({ id: 'D1', name: 'One' }), 'and the answer’s id is the design it holds', JSON.stringify(a.ses.opened));
    await a.ses.saveDraft({ name: 'One', doc: siteDoc('One') });
    ok(/&id=D1(&|$)/.test(saveUrl(calls)), 'the next save overwrites it by id', saveUrl(calls));
    await a.ses.saveDraft({ name: 'Two', doc: siteDoc('Two') });
    ok(/&id=D2(&|$)/.test(saveUrl(calls)) && JSON.stringify(a.ses.opened) === JSON.stringify({ id: 'D3', name: 'Two' }),
      'a renamed Save still names the design it opened, and the session follows the answer', JSON.stringify(a.ses.opened));
    await a.ses.saveAs({ name: 'Two', doc: siteDoc('Two') });
    ok(/&saveAs=1/.test(saveUrl(calls)), 'api.saveAs always asks for a new one', saveUrl(calls));
    const dup = siteFetch({ entries: [], save: () => ({ status: 409, error: 'you already have a design called “One”' }) });
    calls = []; fetchHandler = (url, o) => dup.fetch(url, o);
    const b = mkArt(); await b.ses.boot();
    ok(!(await b.ses.saveDraft({ name: 'One', doc: siteDoc('One') })) && b.state.status === 'Not saved: you already have a design called “One”',
      'a name the owner already has comes through in the site’s own sentence', b.state.status);
    ok(b.ses.opened === null, 'and a save that did not land named nothing');
    const bad = siteFetch({ entries: [{ name: 'One', state: 'draft', at: 1 }], designs: { One: siteDoc('teal') }, save: () => ({}) });
    calls = []; fetchHandler = (url, o) => bad.fetch(url, o);
    const c = mkArt(); await c.ses.boot();
    await c.ses.openEntry('One');
    await c.ses.saveDraft({ name: 'One', doc: siteDoc('One') });
    ok(c.ses.opened === null && /&saveAs=1$/.test(saveUrl(calls)) && !/&id=/.test(saveUrl(calls)) && JSON.stringify(calls.filter((x) => /asset/.test(x[1]))) === '[]',
      'an entry with no design id asks for a new design, not an overwrite', saveUrl(calls));
    // the Edit door on a site: under designIds the id is a design and the library says its name
    const door = siteFetch({ entries: [{ name: 'Card one', state: 'theme', at: 1, design: 'D5' }], themes: { D5: siteDoc('teal') } });
    calls = []; fetchHandler = (url, o) => door.fetch(url, o);
    location.search = '?theme=D5';
    const f = mkArt(); await f.ses.boot();
    ok(await f.ses.openTheme() && JSON.stringify(f.ses.opened) === JSON.stringify({ id: 'D5', name: 'Card one' }) && !/&refs=1/.test(calls.map((c) => c[1]).join(' ')),
      'the site’s Edit door opens the design, names it from the library, and (no assets flag) asks for no ref form', JSON.stringify([f.ses.opened, saveUrl(calls)]));
    const door2 = siteFetch({ entries: [{ name: 'Card one', state: 'theme', at: 1, design: 'D6' }], themes: { D5: siteDoc('teal') } });
    calls = []; fetchHandler = (url, o) => door2.fetch(url, o);
    const g2 = mkArt(); await g2.ses.boot();
    ok(await g2.ses.openTheme() && g2.ses.opened === null, 'a design the library does not list is opened but never claimed', JSON.stringify(g2.ses.opened));
    location.search = '';
    // without the flag the url, the body and the session are exactly today's
    global.GorbHost = { kind: 'web', api: '/studio-api' };
    const off = siteFetch({ entries: [] });
    calls = []; fetchHandler = (url, o) => off.fetch(url, o);
    const d = mkArt(); await d.ses.boot();
    await d.ses.saveDraft({ name: 'One', doc: siteDoc('One') });
    ok(saveUrl(calls) === '/studio-api/design/save?app=tint&name=One' && !('orbAssets' in off.bodies[0]) && d.ses.opened === null,
      'a web host without the flags saves exactly as it does today', saveUrl(calls));
    // under the assets flag, only a real image becomes an asset
    global.GorbHost = { kind: 'web', api: '/studio-api', assets: true, designIds: true };
    const tiny = siteFetch({ entries: [] });
    calls = []; fetchHandler = (url, o) => tiny.fetch(url, o);
    const e = mkArt(); await e.ses.boot();
    e.set({ art: ['data:,', 'data:image/png;base64,e30=', 'not a url at all', { x: 'data:application/octet-stream;base64,' + btoa('short') }] });
    await e.ses.saveDraft({ name: 'One', doc: await e.ses.doc(true) });
    ok(JSON.stringify(tiny.bodies[0].art) === JSON.stringify(e.state.art) && !('orbAssets' in tiny.bodies[0]),
      'nothing malformed, not-base64 or under 512 bytes becomes an asset', JSON.stringify(tiny.bodies[0].art).slice(0, 60));
    const b5 = imgBytes(600, 11), u5 = dataUrl(b5);
    e.set({ art: ['data:,', u5] });
    await e.ses.saveDraft({ name: 'One', doc: await e.ses.doc(true) });
    ok(tiny.bodies[1].art[1] === u5 && !('orbAssets' in tiny.bodies[1]),
      'an image the site has not seen rides inline, with nothing to say about art', JSON.stringify(tiny.bodies[1].art).slice(0, 40));
    await e.ses.saveDraft({ name: 'One', doc: await e.ses.doc(true) });
    const side5 = tiny.bodies[2].orbAssets;
    ok(tiny.bodies[2].art[1] === 'orb-asset:' + await sha256(b5) && tiny.bodies[2].art[0] === 'data:,' && side5.length === 1 && side5[0].size === 600 && side5[0].prefix === 'data:image/png;base64',
      'and the save after that posts its ref with its sidecar entry', JSON.stringify(side5));
  }
  // ---- 10. the shared download (?shared=<id>[&download=1])
  {
    const SID = 'abcdefghijklmnopqrstuv';
    const b6 = imgBytes(600, 21), h6 = await sha256(b6), u6 = dataUrl(b6);
    const THEME3 = new Uint8Array([0x54, 0x52, 0x4f, 0x47, 3, 0, 0, 1]);
    const designOf2 = (name) => ({ format: 'wc-design', schema: 1, app: 'tint', appVersion: 'v1.3.0', color: 'teal', size: 5, name: 'Skeleton',
      shared: name === null ? undefined : { name }, art: ['orb-asset:' + h6], orbAssets: [{ sha256: h6, prefix: 'data:image/png;base64', size: 600 }] });
    const bundleOf = (design, assets) => Object.assign({ format: 'orb-design-bundle', bundle: 1, design: design },
      assets === undefined ? { assets: Object.fromEntries([[h6, u6.split(',')[1]]]) } : { assets });
    global.GorbHost = { kind: 'web', api: '/studio-api', send: false, assets: true, designIds: true };
    location.pathname = '/v1.4.2/';
    const run = (o) => {
      const shared = o.name === undefined ? 'Skeleton by @greg' : o.name;
      location.search = '?shared=' + (o.shared || SID) + (o.dl === false ? '' : '&download=1');
      const site = siteFetch(Object.assign({ entries: [{ name: 'Skeleton', state: 'theme', at: 1, design: 'D7' }], bundle: bundleOf(designOf2(shared)) }, o.site || {}));
      calls = []; fetchHandler = (url, opt) => site.fetch(url, opt);
      const seq = o.names || [shared === null ? 'Skeleton' : shared];
      global.GorbBake = o.noBaker ? null : { verifyBundle3: () => ({ name: seq[Math.min(o.i = (o.i || 0) + 1, seq.length) - 1] }) };
      const a = mkArt(o.noBake ? {} : { bake: async () => { a.baked.push(a.ses.bakeName('Skeleton')); if (o.says) a.set({ status: o.says }); return o.bytes === null ? null : THEME3; } });
      return a;
    };
    // the happy path: a bundle in, one bake, a file out
    const a = run({});
    ok(a.ses.opening === 'Opening the shared design…', 'the cover says it while the bundle is on its way in', String(a.ses.opening));
    await a.ses.boot();
    ok(calls.length === 0, 'a shared link reads no library and moves no old browser designs');
    const done = await a.ses.openTheme();
    ok(done && calls.length === 1 && calls[0][1] === '/studio-api/shared/bundle?id=' + SID,
      'one request for the whole design, at /shared/bundle', JSON.stringify(calls));
    ok(a.state.art[0] === u6 && !('orbAssets' in a.docs[0]), 'its image came out of the bundle, inline', a.state.status);
    ok(a.baked[0] === 'Skeleton by @greg', 'the bake is asked for under the shared name', JSON.stringify(a.baked));
    ok(a.downloads.length === 1 && a.downloads[0].name === 'Skeleton by @greg.orbtheme', 'one file, named for the shared name', JSON.stringify(a.downloads.map((d) => d.name)));
    ok(a.downloads[0].blob.type === 'application/octet-stream' && new Uint8Array(await a.downloads[0].blob.arrayBuffer()).join() === THEME3.join(), 'carrying the bake’s own bytes');
    ok(a.state.status === 'Downloaded “Skeleton by @greg”', 'and the line says so', a.state.status);
    ok(a.ses.opened === null && a.ses.library().length === 0, 'nothing of it is this editor’s design to save', JSON.stringify([a.ses.opened, a.ses.library()]));
    ok(await a.ses.openTheme() && a.downloads.length === 2, 'and the link still works a second time');
    a.ses.apply(siteDoc('Mine'), 'the card');
    ok(a.ses.bakeName('Skeleton') === 'Skeleton', 'a later apply clears the shared name again');
    // a name with characters a file cannot hold
    const fs = run({ name: 'A/B:C*D?E"F<G>H|I' });
    await fs.ses.boot(); await fs.ses.openTheme();
    ok(fs.downloads[0].name === 'A_B_C_D_E_F_G_H_I.orbtheme', 'every one of them becomes an underscore in the file name', fs.downloads[0].name);
    // the refusals
    const bad = run({ site: { bundle: Object.assign(bundleOf(designOf2('X by @y')), { format: 'orb-json' }) } });
    await bad.ses.boot();
    ok(!(await bad.ses.openTheme()) && !bad.applied.length && !bad.downloads.length && bad.state.status === 'Could not open that design: that is not a design bundle',
      'a bundle of another format is refused before anything is applied', bad.state.status);
    const nores = run({ site: { bundleStatus: 404 } });
    await nores.ses.boot();
    ok(!(await nores.ses.openTheme()) && nores.state.status === 'Could not open that design: the server answered 404', 'a site with no such link says so in its status', nores.state.status);
    const said = run({ site: { bundleStatus: 403, bundleError: 'that design is not shared' } });
    await said.ses.boot();
    ok(!(await said.ses.openTheme()) && said.state.status === 'Could not open that design: that design is not shared', 'and in its own sentence when it has one', said.state.status);
    const missing = run({ site: { bundle: bundleOf(designOf2('Skeleton by @greg'), Object.fromEntries([])) } });
    await missing.ses.boot();
    ok(!(await missing.ses.openTheme()) && missing.state.status === 'Could not open that design: the bundle is missing an image', 'a bundle without an image it names is refused', missing.state.status);
    const anon = run({ name: null });
    await anon.ses.boot(); await anon.ses.openTheme();
    ok(anon.baked[0] === 'Skeleton' && anon.downloads[0].name === 'Skeleton.orbtheme', 'a bundle with no shared name bakes under the design’s own', JSON.stringify([anon.baked[0], anon.downloads[0].name]));
    // the bake leg
    const nobake = run({ noBake: true });
    await nobake.ses.boot(); await nobake.ses.openTheme();
    ok(nobake.applied.length === 1 && nobake.state.status === 'Not downloaded: this editor cannot bake here' && !nobake.downloads.length, 'an editor with no bake says so and still opens', nobake.state.status);
    const nullbake = run({ bytes: null });
    await nullbake.ses.boot(); await nullbake.ses.openTheme();
    ok(nullbake.state.status === 'Not downloaded: the bake did not finish', 'a bake that returns nothing says the fallback', nullbake.state.status);
    const whybake = run({ bytes: null, says: 'Not baked: the movement has not settled' });
    await whybake.ses.boot(); await whybake.ses.openTheme();
    ok(whybake.state.status === 'Not downloaded: Not baked: the movement has not settled', 'or the editor’s own line', whybake.state.status);
    const retry = run({ names: ['Skeleton', 'Skeleton', 'Skeleton by @greg'] });
    await retry.ses.boot(); await retry.ses.openTheme();
    ok(retry.baked.length === 3 && retry.downloads.length === 1, 'a bake that still named the old design is given three tries', JSON.stringify([retry.baked.length, retry.state.status]));
    const wrong = run({ names: ['Someone else'] });
    await wrong.ses.boot(); await wrong.ses.openTheme();
    ok(wrong.baked.length === 3 && !wrong.downloads.length && wrong.state.status === 'Not downloaded: the bake came out named “Someone else”',
      'and three is enough: it reports the name it got', wrong.state.status);
    const nobaker = run({ noBaker: true });
    await nobaker.ses.boot(); await nobaker.ses.openTheme();
    ok(nobaker.baked.length === 1 && nobaker.downloads.length === 1, 'with no baker to ask, one bake and the download', JSON.stringify([nobaker.baked.length, nobaker.downloads.length]));
    const both = run({});
    location.search = '?shared=' + SID + '&theme=D7&download=1';
    await both.ses.boot();
    ok(await both.ses.openTheme() && both.downloads.length === 1, 'and ?shared= wins over ?theme= on the same link');
    const nodl = run({ dl: false });
    await nodl.ses.boot(); await nodl.ses.openTheme();
    ok(nodl.applied.length === 1 && !nodl.baked.length && !nodl.downloads.length && nodl.state.art[0] === u6, 'without download=1 the shared design just opens');
    // what the link itself can refuse, and what a name can cost
    const tiny0 = run({ shared: 'short' });
    ok(tiny0.ses.opening === null && (await tiny0.ses.boot()) && !(await tiny0.ses.openTheme()) && !calls.some((c) => /shared/.test(c[1])),
      'a shared id that is not 22 characters asks the site for nothing', JSON.stringify(calls.map((c) => c[1])));
    const longish = run({ name: '—'.repeat(20), names: ['Skeleton'] });
    await longish.ses.boot(); await longish.ses.openTheme();
    ok(longish.baked[0] === 'Skeleton' && longish.downloads[0].name === 'Skeleton.orbtheme',
      'a shared name past 55 bytes is no shared name, so the design’s own is baked', JSON.stringify([longish.baked[0], longish.downloads[0] && longish.downloads[0].name]));
    const others = run({ site: { bundle: bundleOf(Object.assign(designOf2('X by @y'), { app: 'weather' })) } });
    await others.ses.boot();
    ok(!(await others.ses.openTheme()) && others.state.status === 'Could not open that design: that design is the weather app’s, not tint’s',
      'a bundle for another app is refused the way every open refuses it', others.state.status);
    const garbage = run({ site: { bundle: bundleOf(designOf2('X by @y'), Object.fromEntries([[h6, 'not base64!!']])) } });
    await garbage.ses.boot();
    ok(!(await garbage.ses.openTheme()) && garbage.state.status === 'Could not open that design: the bundle is missing an image',
      'and an asset that is not base64 is the same refusal', garbage.state.status);
    const offline = run({});
    await offline.ses.boot();
    fetchHandler = () => { throw new TypeError('Failed to fetch'); };
    ok(!(await offline.ses.openTheme()) && offline.state.status === 'Could not open that design: Failed to fetch',
      'a site that cannot be reached at all is said in its own words', offline.state.status);
    location.search = '';
    delete global.GorbBake; delete global.GorbHost;
  }

  delete global.GorbHost;

  console.log(fails ? 'RESULT: FAIL — ' + fails + ' of ' + checked + ' checks' : 'all green (' + checked + ' checks)');
  process.exit(fails ? 1 : 0);
})().catch((e) => { console.log('FAIL threw: ' + (e && e.stack || e)); process.exit(1); });
