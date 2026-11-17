#!/usr/bin/env node
// headless-library.test.js — L1 hub step 1 (docs/specs/l1-hub-1-library-kit.md): the two
// GorbHost flags, the OrbTheme.library client against a stub fetch, and the studio's file
// rows and library picker against a React stand-in whose hooks keep real slots — so the
// picker's effects run, its states change, and the tree a check reads is what an author sees.
//
//   node headless-library.test.js        (VERBOSE=1 prints every PASS too)
'use strict';
const path = require('path');
const fs = require('fs');
let fails = 0, checked = 0;
const ok = (c, n, x) => { checked++; if (!c || process.env.VERBOSE) console.log((c ? 'PASS ' : 'FAIL ') + n + (x ? '  [' + x + ']' : '')); if (!c) fails++; };
setTimeout(() => { console.log('FAIL timed out (' + checked + ' checks run)'); process.exit(1); }, 20000);

// ---- the host: a window, a location, a fetch every test re-points, key handlers captured
global.window = globalThis;
const store = new Map();
global.localStorage = { getItem: (k) => (store.has(k) ? store.get(k) : null), setItem: (k, v) => store.set(k, String(v)), removeItem: (k) => store.delete(k) };
global.location = { origin: 'http://hub', search: '', pathname: '/' };
global.parent = { postMessage: () => {} };
const KEYS = [];
global.addEventListener = (t, fn) => { if (t === 'keydown') KEYS.push(fn); };
global.removeEventListener = (t, fn) => { const i = KEYS.indexOf(fn); if (i >= 0) KEYS.splice(i, 1); };
const FETCH = { calls: [], resp: null };
global.fetch = (url, opt) => {
  const u = String(url);
  FETCH.calls.push([u, (opt && opt.cache) || '']);
  const r = FETCH.resp;
  if (!r) return Promise.reject(new Error('no stub'));
  try { return Promise.resolve(r(u, opt)); } catch (e) { return Promise.reject(e); }
};
const res = (status, j) => ({ ok: status < 300 && status >= 200, status, json: async () => j, text: async () => JSON.stringify(j) });
const binres = (status, bytes) => ({ ok: status < 300 && status >= 200, status, arrayBuffer: async () => bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength) });
const host = (g) => { if (g === undefined) delete global.GorbHost; else global.GorbHost = g; return OrbTheme.hostInfo(); };

// ---- React with real hook slots: every test that wants a fresh component calls render(),
// which zeroes the slot table; effects queue and run on flush(), and state setters write the
// slots, so a re-render sees what the effect did.
let SLOT = { i: 0, h: [] };
const QUEUE = [];
global.React = {
  Fragment: Symbol('fragment'),
  createElement: (type, props, ...kids) => ({ type, props: props || {}, kids: kids.flat(Infinity).filter((k) => k !== null && k !== undefined && k !== false) }),
  useRef: (v) => { const i = SLOT.i++; if (!(i in SLOT.h)) SLOT.h[i] = { current: v }; return SLOT.h[i]; },
  useState: (init) => {
    const i = SLOT.i++;
    if (!(i in SLOT.h)) SLOT.h[i] = { v: typeof init === 'function' ? init() : init };
    const s = SLOT.h[i];
    return [s.v, (x) => { s.v = typeof x === 'function' ? x(s.v) : x; }];
  },
  useEffect: (fn) => { QUEUE.push(fn); },
  useLayoutEffect: (fn) => { QUEUE.push(fn); },
  useCallback: (fn) => fn,
};
const FRAG = React.Fragment;
const expand = (n) => {
  if (n == null || typeof n !== 'object') return n;
  if (typeof n.type === 'function') return expand(n.type(Object.assign({}, n.props, n.kids.length ? { children: n.kids.length === 1 ? n.kids[0] : n.kids } : {})));
  if (n.type === FRAG) return { type: FRAG, props: n.props, kids: n.kids.map(expand).filter((k) => k !== null && k !== undefined) };
  return { type: n.type, props: n.props, kids: n.kids.map(expand).filter((k) => k !== null && k !== undefined) };
};
let LAST = null;
const render = (C, p) => { SLOT = { i: 0, h: [] }; LAST = { C: C, p: p }; return expand(C(Object.assign({}, p))); };
const rerender = () => { SLOT.i = 0; return expand(LAST.C(Object.assign({}, LAST.p))); };
const all = (n, f, out) => { out = out || []; if (n && typeof n === 'object') { if (f(n)) out.push(n); (n.kids || []).forEach((k) => all(k, f, out)); } return out; };
const text = (n) => (n == null ? '' : typeof n !== 'object' ? String(n) : (n.kids || []).map(text).join(''));
const by = (n, tag, cls) => all(n, (x) => x.type === tag && (!cls || String(x.props.className || '').split(' ').includes(cls)));
const tick = async (n) => { for (let i = 0; i < (n || 4); i++) await new Promise((r) => setImmediate(r)); };
// run the queued effects and settle the promises they start; key handlers are captured live
const flush = async () => { for (let k = 0; k < 3; k++) { const q = QUEUE.splice(0); for (const f of q) { try { await f(); } catch (e) { /* an effect that throws is a FAIL in the check that reads its tree */ } } await tick(2); } };

global.GorbBake = require(path.join(__dirname, '..', 'baker@1', 'orbtheme-baker.js'));
require(path.join(__dirname, '..', 'theme@1', 'send.js'));
require(path.join(__dirname, '..', 'theme@1', 'session.js'));
require(path.join(__dirname, '..', 'studio@1', 'studio.js'));
const S = global.OrbStudio, T = global.OrbTheme;
const dig = async (bytes) => Buffer.from(await crypto.subtle.digest('SHA-256', bytes)).toString('hex');

// ---- the blocks and the flags
{
  const one = (f) => {
    const t = fs.readFileSync(path.join(__dirname, '..', 'theme@1', f), 'utf8');
    const a = t.indexOf('// ---- GorbHost:'), b = t.indexOf('// ---- end GorbHost');
    return a < 0 || b < 0 ? null : t.slice(a, b + '// ---- end GorbHost'.length);
  };
  const s1 = one('session.js'), s2 = one('send.js');
  ok(s1 !== null && s1 === s2, 'the GorbHost blocks stay byte-identical', s1 === null ? 'no block found' : s1 === s2 ? 'both ' + s1.length + ' bytes' : 'blocks differ');
}
{
  const a = host(undefined), h = host({ kind: 'hub' }), w = host({ kind: 'web' }), wo = host({ kind: 'web', files: true }), hn = host({ kind: 'hub', files: false });
  ok(a.files === true && h.files === true && w.files === false && wo.files === true && hn.files === false,
    'files defaults to true under the hub and false under the site', [a.files, h.files, w.files, wo.files, hn.files].join());
}
{
  const a = host(undefined), h = host({ kind: 'hub', library: true }), w = host({ kind: 'web', library: true }), s = host({ library: 'yes' }), n = host({ library: 1 });
  ok(a.library === false && h.library === true && w.library === true && s.library === false && n.library === false,
    'library is on only when the host says so', [a.library, h.library, w.library, s.library, n.library].join());
}

// ---- the library client
const GOOD = { sha256: 'a'.repeat(64), kind: 'picture', mediaType: 'image/png', prefix: 'data:image/png;base64', name: 'p.png', size: 1024, width: 64, height: 64, seconds: null, designs: [] };
async function tList() {
  const junk = [Object.assign({}, GOOD, { sha256: 'Z'.repeat(64) }), Object.assign({}, GOOD, { kind: 'video', sha256: 'b'.repeat(64) }), Object.assign({}, GOOD, { prefix: 'http://x/a', sha256: 'c'.repeat(64) }), null, 7];
  host({ kind: 'web' }); FETCH.calls.length = 0;
  FETCH.resp = () => res(200, { ok: true, data: { assets: [GOOD].concat(junk) } });
  const w = await T.library.list();
  const wurl = FETCH.calls[0];
  host({ kind: 'hub' }); FETCH.calls.length = 0;
  FETCH.resp = () => res(200, { ok: true, data: { assets: [GOOD].concat(junk) } });
  const h = await T.library.list();
  ok(w.ok === true && w.assets.length === 1 && w.assets[0].sha256 === GOOD.sha256
    && wurl[0] === '/studio-api/assets' && wurl[1] === 'no-store' && FETCH.calls[0][0] === '/api/assets' && h.ok === true,
    "library.list reads the host's assets", JSON.stringify([wurl, FETCH.calls[0]]));
}
async function tListFails() {
  host({ kind: 'hub' });
  FETCH.resp = () => res(200, { ok: false, error: 'Storage is full on this hub' });
  const a = await T.library.list();
  FETCH.resp = () => res(401, { ok: false, error: 'no' });
  const b = await T.library.list();
  FETCH.resp = () => { throw new Error('offline'); };
  const c = await T.library.list();
  FETCH.resp = () => ({ ok: true, status: 200, json: async () => { throw new SyntaxError('bad'); } });
  const d = await T.library.list();
  FETCH.resp = () => res(500, { ok: true, data: { assets: [GOOD] } });
  const e = await T.library.list();
  FETCH.resp = () => res(200, 'not even an object');
  const f2 = await T.library.list();
  ok(a.ok === false && a.message === 'Storage is full on this hub'
    && b.ok === false && b.message === 'Sign in to use your library'
    && c.ok === false && c.message === 'Your library could not be reached'
    && d.ok === false && d.message === 'Your library could not be reached'
    && e.ok === false && e.message === 'Your library could not be reached'
    && f2.ok === false && f2.message === 'Your library could not be reached',
    'library.list says why it failed', [a.message, b.message, c.message, d.message, e.message, f2.message].join(' | '));
}

async function tPick() {
  host({ kind: 'hub' });
  const bytes = new TextEncoder().encode('orb-bytes-for-a-pick');
  const hex = await dig(bytes);
  const asset = Object.assign({}, GOOD, { sha256: hex, name: 'p.png' });
  FETCH.calls.length = 0;
  FETCH.resp = () => binres(200, bytes);
  const r = await T.library.pick(asset);
  const url = 'data:image/png;base64,' + Buffer.from(bytes).toString('base64');
  ok(r.ok === true && r.url === url && r.name === 'p.png' && r.sha256 === hex && r.kind === 'picture' && r.mediaType === 'image/png'
    && FETCH.calls[0][0] === '/api/asset?sha256=' + hex,
    'library.pick builds the data URL from the prefix and checks the hash', JSON.stringify(r).slice(0, 120));
  FETCH.resp = () => binres(200, new TextEncoder().encode('other-bytes-entirely'));
  const w = await T.library.pick(asset);
  FETCH.resp = () => binres(404, new Uint8Array(0));
  const n = await T.library.pick(asset);
  const b = await T.library.pick({ sha256: 'zz', prefix: 'data:image/png;base64' });
  ok(w.ok === false && w.message === 'That asset did not arrive whole'
    && n.ok === false && n.message === 'That asset could not be fetched'
    && b.ok === false && b.message === 'That asset could not be fetched',
    'library.pick refuses the file that is not the one the listing named', [w.message, n.message, b.message].join(' | '));
}

// ---- the studio's file rows
const Kit = S.fieldKit({});
const sameTree = (a, b) => {
  if (a === b) return true;
  if (typeof a === 'function' || typeof b === 'function') return a === b;
  if (!a || !b || typeof a !== 'object' || typeof b !== 'object') return false;
  if (!('props' in a) || !('props' in b) || a.type !== b.type) {
    try { return JSON.stringify(a) === JSON.stringify(b); } catch (e) { return false; }   // a plain data prop: a style box, a field descriptor
  }
  const ka = Object.keys(a.props), kb = Object.keys(b.props);
  if (ka.length !== kb.length) return false;
  for (const k of ka) if (!sameTree(a.props[k], b.props[k])) return false;
  if ((a.kids || []).length !== (b.kids || []).length) return false;
  for (let i = 0; i < a.kids.length; i++) if (!sameTree(a.kids[i], b.kids[i])) return false;
  return true;
};
const LIB = { pick: () => {}, play: () => {} };
function tRowSite() {
  host({ kind: 'web', library: true });
  const f = Kit.Fi('Art', () => {}, 'art.png', undefined, LIB);
  const t = render(S.Fields, { fields: [f] });
  const btns = by(t, 'button').filter((n) => text(n) === 'Your library…');
  const pic = by(t, 'input').filter((n) => n.props.type === 'file');
  const sf = Kit.Fi('Chime', () => {}, '', 'audio/*', LIB);
  const ts = render(S.Fields, { fields: [sf] });
  const sbtn = by(ts, 'button').filter((n) => text(n) === 'Your library…');
  ok(pic.length === 0 && btns.length === 1 && btns[0].props.className === 'ui-ghost st-sm'
    && btns[0].props.title === 'Pick a picture from your orb-os.net library'
    && by(ts, 'input').filter((n) => n.props.type === 'file').length === 0 && sbtn.length === 1
    && sbtn[0].props.title === 'Pick a sound from your orb-os.net library'
    && text(t).indexOf('art.png') >= 0,
    'a file row under the site has no file input and a library button', btns.length + '/' + pic.length);
  const nf = render(S.Fields, { fields: [Kit.Fi('Art', () => {}, 'v')] });
  ok(by(nf, 'input').filter((n) => n.props.type === 'file').length === 0
    && by(nf, 'button').filter((n) => text(n) === 'Your library…').length === 0
    && text(nf).indexOf('v') >= 0,
    'a file row under the site with no library to give says its value and offers nothing', 'row without lib');
}
function tRowHubDefault() {
  const f = Kit.Fi('Art', () => {}, 'art.png');
  host(undefined);
  const noHost = render(S.Fields, { fields: [f] });
  host({ kind: 'hub' });
  const plain = rerender();
  host({ kind: 'web' });
  const site = rerender();
  host({ kind: 'hub', library: true });
  const linked = render(S.Fields, { fields: [f] });   // the same row, offered no lib, on a linked hub
  host(undefined);
  ok(sameTree(noHost, plain) && sameTree(noHost, linked)
    && !sameTree(noHost, site)
    && by(noHost, 'input').filter((n) => n.props.type === 'file').length === 1
    && by(noHost, 'input')[0].props.accept === 'image/png,image/svg+xml,image/jpeg,image/webp'
    && by(noHost, 'button').length === 0
    && by(site, 'input').filter((n) => n.props.type === 'file').length === 0,
    "a file row under the hub without a link is today's row", 'tree compared field-for-field');
}
function tRowLinkedHub() {
  host({ kind: 'hub', library: true });
  const t = render(S.Fields, { fields: [Kit.Fi('Art', () => {}, 'art.png', undefined, LIB)] });
  ok(by(t, 'input').filter((n) => n.props.type === 'file').length === 1
    && by(t, 'button').filter((n) => text(n) === 'Your library…').length === 1,
    'a file row under a linked hub has both', 'input + button');
}

// ---- the picker
const PNG = Object.assign({}, GOOD, { name: 'p.png' });
const JPG = Object.assign({}, GOOD, { sha256: 'b'.repeat(64), mediaType: 'image/jpeg', name: 'q.jpg' });
const WPD = Object.assign({}, GOOD, { sha256: 'c'.repeat(64), mediaType: 'image/webp', name: 'r.webp' });
const SND = Object.assign({}, GOOD, { sha256: 'd'.repeat(64), kind: 'sound', mediaType: 'audio/wav', prefix: 'data:audio/wav;base64', name: 's.wav', width: null, height: null, seconds: 2.4, size: 84 * 1024, designs: [{ name: 'Radar' }, { name: 'Dial' }] });
function stubLB(log) {
  T.library = {
    list: async () => ({ ok: true, assets: [PNG, JPG, WPD, SND] }),
    pick: (a) => { if (log) log.push(a.sha256); return Promise.resolve({ ok: true, url: a.prefix + ',A', name: a.name, sha256: a.sha256, kind: a.kind, mediaType: a.mediaType }); },
  };
}
async function tPickerFilter() {
  stubLB();
  host({ kind: 'hub', library: true });
  const t = render(S.LibraryPicker, { open: true, kind: 'picture', accept: 'image/png,image/svg+xml', onPick: () => {}, onClose: () => {} });
  await flush();
  const t2 = rerender();
  const rows = by(t2, 'div', 'st-lib-a');
  const names = rows.map((r) => text(by(r, 'b', 'st-lib-nm')[0]));
  const ts = render(S.LibraryPicker, { open: true, kind: 'sound', accept: 'audio/*', onPick: () => {}, onClose: () => {} });
  await flush();
  const rowsS = by(rerender(), 'div', 'st-lib-a');
  ok(names.join() === 'p.png' && rowsS.length === 1 && text(by(rowsS[0], 'b', 'st-lib-nm')[0]) === 's.wav',
    "the picker lists only the row's kind and accepted types", names.join() + ' / ' + text(rowsS[0]));
}
async function tPickerStates() {
  let opened = true;
  const closed = [];
  stubLB();
  const P = { open: true, kind: 'picture', accept: 'image/png,image/svg+xml', onPick: () => {}, onClose: () => { closed.push(1); } };
  const t = render(S.LibraryPicker, P);
  const loading = text(t).indexOf('Opening your library…') >= 0 && by(t, 'button').map(text).includes('Cancel');
  T.library = { list: async () => ({ ok: true, assets: [] }), pick: async () => ({ ok: true }) };
  const t2 = render(S.LibraryPicker, P); await flush();
  const te = rerender();
  const empty = text(te).indexOf('No pictures in your library yet.') >= 0 && text(te).indexOf('Add them on orb-os.net, under Assets.') >= 0;
  const t2s = render(S.LibraryPicker, Object.assign({}, P, { kind: 'sound', accept: 'audio/*' })); await flush();
  const emptyS = text(rerender()).indexOf('No sounds in your library yet.') >= 0;
  T.library = { list: async () => ({ ok: false, message: 'Your library could not be reached' }), pick: async () => ({ ok: false }) };
  const t3 = render(S.LibraryPicker, P); await flush();
  const errt = text(rerender());
  const errored = errt.indexOf('Your library could not be reached') >= 0 && errt.indexOf('Add them on orb-os.net, under Assets.') < 0;
  ok(loading && empty && emptyS && errored, "the picker's empty, loading and error states read as the contract says", [loading, empty, emptyS, errored].join());
}
async function tPickerPick() {
  stubLB();
  const picked = [], closed = [];
  const t = render(S.LibraryPicker, { open: true, kind: 'picture', accept: 'image/png,image/svg+xml', onPick: (r) => picked.push(r), onClose: () => closed.push(1) });
  await flush();
  const t2 = rerender();
  const row = by(t2, 'div', 'st-lib-a')[0];
  row.props.onClick();
  await tick(3);
  const got = picked.length === 1 && picked[0].ok === true && picked[0].url === 'data:image/png;base64,A' && picked[0].sha256 === PNG.sha256 && closed.length === 1;
  // Enter on the row picks too, and Escape closes an open picker
  picked.length = 0; closed.length = 0;
  const t3 = rerender();
  by(t3, 'div', 'st-lib-a')[0].props.onKeyDown({ key: 'Enter' });
  await tick(3);
  const byEnter = picked.length === 1 && closed.length === 1;
  picked.length = 0; closed.length = 0;
  KEYS.length = 0;
  render(S.LibraryPicker, { open: true, kind: 'picture', accept: 'image/png,image/svg+xml', onPick: (r) => picked.push(r), onClose: () => closed.push(1) });
  await flush();
  if (KEYS.length) KEYS[KEYS.length - 1]({ key: 'Escape' });
  const esc = closed.length === 1 && picked.length === 0;
  // a refusal says so in the body and keeps the modal open
  const refused = [];
  T.library = { list: async () => ({ ok: true, assets: [PNG] }), pick: async () => ({ ok: false, message: 'That asset did not arrive whole' }) };
  const t4 = render(S.LibraryPicker, { open: true, kind: 'picture', accept: 'image/png,image/svg+xml', onPick: (r) => refused.push(r), onClose: () => refused.push('closed') });
  await flush();
  by(rerender(), 'div', 'st-lib-a')[0].props.onClick();
  await tick(3);
  const stays = refused.length === 0 && text(rerender()).indexOf('That asset did not arrive whole') >= 0;
  // and the row that hosts a picker closes it when the pick lands
  host({ kind: 'web', library: true });
  stubLB();
  const rowPicks = [];
  const f = Kit.Fi('Art', () => {}, '', undefined, { pick: (pk) => rowPicks.push(pk.url) });
  const tr = render(S.Fields, { fields: [f] });
  by(tr, 'button').filter((n) => text(n) === 'Your library…')[0].props.onClick();
  const tr2 = rerender();
  const openedRow = by(tr2, 'div', 'st-lib').length === 1;
  by(tr2, 'button').filter((n) => text(n) === 'Your library…')[0].props.onClick();   // a second press: still one picker
  const tr3 = rerender();
  ok(by(tr3, 'div', 'st-lib').length === 1, 'two quick presses do not open two pickers', by(tr3, 'div', 'st-lib').length + ' open');
  await flush();
  by(rerender(), 'div', 'st-lib-a')[0].props.onClick();
  await tick(3);
  const shut = rowPicks.length === 1 && rowPicks[0] === 'data:image/png;base64,A' && by(rerender(), 'div', 'st-lib').length === 0;
  ok(got && byEnter && esc && stays && openedRow && shut, "picking calls the row's pick with the fetched url and closes",
    [got, byEnter, esc, stays, openedRow, shut].join());
}
function tSoundPass() {
  const libs = [], plays = [];
  const o = {
    recipes: [], slots: [{ key: 'focus', label: 'Focus ends', silent: false }],
    get: () => null, set: () => {}, uploads: {}, upload: () => {},
    play: (p, n) => plays.push([p, n]), stop: () => {},
  };
  let rows = S.Sound(Kit, o);
  const plain = rows.find((r) => r.isFile && r.label === 'Your own');
  o.library = (k, pk) => libs.push([k, pk]);
  rows = S.Sound(Kit, o);
  const row = rows.find((r) => r.isFile && r.label === 'Your own');
  row.lib.pick({ ok: true, url: 'u' });
  ok(plain.lib == null && row.lib && typeof row.lib.pick === 'function' && row.lib.play === o.play
    && row.kind === 'sound' && row.accept === 'audio/*' && libs.length === 1 && libs[0][0] === 'focus' && libs[0][1].url === 'u',
    'the sound row passes the library through', JSON.stringify({ had: !!plain.lib, has: !!row.lib, libs: libs.length }));
}

(async () => {
  await tList(); await tListFails(); await tPick();
  tRowSite(); tRowHubDefault(); tRowLinkedHub();
  await tPickerFilter(); await tPickerStates(); await tPickerPick();
  tSoundPass();
  console.log(fails ? fails + ' FAIL' : 'all green (' + checked + ' checks)');
  process.exit(fails ? 1 : 0);
})().catch((e) => { console.log('FAIL the suite threw: ' + ((e && e.stack) || e)); process.exit(1); });
