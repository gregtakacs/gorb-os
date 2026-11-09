#!/usr/bin/env node
// theme@1/send (theme-editor-kit-plan K1): the OS's send, driven through a fake host and a
// fake hub, with no app around it. What any app's editor gets by calling it: one modal act
// with a cancel that locks once the hub holds the bytes, the identity precheck on the head
// slice, a duplicate held for Replace, an exact match stopped before upload, a precheck that
// fails closed, the commit token that makes Replace an empty POST, and the store verdict.
//
//   node headless-send.test.js
'use strict';
const path = require('path');
let fails = 0, checked = 0;
const ok = (c, n, x) => { checked++; if (!c || process.env.VERBOSE) console.log((c ? 'PASS ' : 'FAIL ') + n + (x ? '  [' + x + ']' : '')); if (!c) fails++; };

// the window the service runs in: its parent, its unload guard, its address
const posted = [], unloads = new Set();
global.window = globalThis;
global.parent = { postMessage: (m) => posted.push(m) };
global.addEventListener = (t, fn) => { if (t === 'beforeunload') unloads.add(fn); };
global.removeEventListener = (t, fn) => { if (t === 'beforeunload') unloads.delete(fn); };
global.location = { origin: 'http://hub', protocol: 'http:', search: '' };

// the hub: each route answers from the scenario; the upload is an XHR the test drives by hand
let scen = {}, calls = [];
const resp = (status, j) => ({ status, text: async () => JSON.stringify(j), json: async () => j });
global.fetch = async (url, o) => {
  calls.push([url, o && o.body]);
  if (/\/api\/orb\/known/.test(url)) return resp(200, { ok: true, data: { ip: scen.known || null } });
  if (/precheck=1/.test(url)) return scen.precheck ? scen.precheck(o.body) : resp(200, { ok: true, data: { duplicate: false } });
  if (/\/api\/theme\/commit/.test(url)) return resp(scen.commitStatus || 200, scen.commit || { ok: true, data: { state: 'stored', reboot: 'automatic' } });
  if (/upload-status/.test(url)) return resp(200, { ok: true, data: { phase: 'idle' } });
  return resp(404, { ok: false, error: 'no route' });
};
let xhr = null;
global.XMLHttpRequest = function () {
  xhr = this; this.upload = {}; this.status = 0; this.responseText = '';
  this.open = (m, u) => { this.url = u; }; this.setRequestHeader = () => {}; this.send = (b) => { this.body = b; };
  this.abort = () => { if (this.onabort) this.onabort(); };
};
const answer = (j) => { xhr.status = 200; xhr.responseText = JSON.stringify(j); xhr.onload(); };
// a send that waits on an answer this suite never gives is a failure, not a hang
setTimeout(() => { console.log('FAIL timed out: a send waited on an answer the scenario never gives (' + checked + ' checks run)'); process.exit(1); }, 20000);
const tick = async (n) => { for (let i = 0; i < (n || 4); i++) await new Promise((r) => setImmediate(r)); };

require(path.join(__dirname, '..', 'theme@1', 'send.js'));
const mk = () => {
  const h = { status: '', dup: null, sending: null, orbSeen: undefined };
  const s = OrbTheme.sender({ say: (t) => { h.status = t; }, ask: (d) => { h.dup = d; }, sending: (v) => { h.sending = v; }, orb: (ip) => { h.orbSeen = ip; } });
  return { h, s };
};
// a container whose header names a 200-byte geometry region (word 5), so the head slice is 328 B
const bake = (n) => { const b = new Uint8Array(n || 4096); new DataView(b.buffer).setUint32(20, 200, true); return b; };
const meta = { name: 'Dial', stem: 'clock-dial-000000000000', summary: 'still · 3 KB' };
const busy = () => posted.filter((m) => m.type === 'theme.busy').map((m) => m.on).join();

(async () => {
  // ---- finding the orb
  {
    scen = { known: '192.0.2.9' }; calls = [];
    const { h, s } = mk();
    ok(await s.findOrb() === '192.0.2.9' && h.orbSeen === '192.0.2.9', 'the orb comes from /api/orb/known and is reported to the host');
    await s.findOrb();
    ok(calls.filter((c) => /orb\/known/.test(c[0])).length === 1, 'a found orb is cached');
    location.search = '?orb=198.51.100.4';
    ok(await mk().s.findOrb() === '198.51.100.4', 'an editor the hub opened for one orb (?orb=) sends there');
    location.search = '';
    scen = { known: null };
    const n = mk();
    ok(await n.s.findOrb() === null, 'no orb is an answer');
    await n.s.post(bake(), meta, false);
    ok(/Not sent: this hub is not talking to an orb/.test(n.h.status), 'and a send without one says so instead of dialing', n.h.status);
  }

  // ---- one modal act, and a send that lands
  scen = { known: '192.0.2.7' };
  {
    posted.length = 0; xhr = null; calls = [];
    const { h, s } = mk();
    const bytes = bake();
    const run = s.send('Dial', () => s.post(bytes, meta, false));
    await tick();
    ok(s.active && h.sending && h.sending.canCancel, 'the press opens the send, cancellable while the bytes are the page’s');
    ok(busy() === 'true' && unloads.size === 1, 'tells the hub it is busy and guards the window', busy());
    const pre = calls.find((c) => /precheck=1/.test(c[0]));
    ok(pre && pre[1].byteLength === 328, 'the precheck carries the header and the geometry region only', pre && pre[1].byteLength);
    ok(xhr && xhr.body === bytes && !/overwrite/.test(xhr.url), 'then the whole bake rides one XHR, without overwrite');
    xhr.upload.onprogress({ loaded: 4096, total: 4096 });
    ok(h.sending && !h.sending.canCancel, 'once the hub holds every byte the cancel locks');
    s.cancel();
    ok(!s.cancelRequested, 'and a cancel there does nothing');
    answer({ ok: true, data: { name: 'Dial', id: meta.stem, store: 'stored', back: true } });
    await run;
    ok(h.sending && h.sending.done && h.sending.outcome === 'ok' && !s.active && busy() === 'true,false' && unloads.size === 0,
       'the verdict frees the hub and drops the guard, and stays up as the send\u2019s outcome', JSON.stringify(h.sending));
    s.cancel();
    ok(h.sending === null, 'until its button closes it');
    ok(/Sent to 192\.0\.2\.7 as "Dial" \(clock-dial-000000000000\) — still · 3 KB · stored into the one slot, and the orb is back drawing it/.test(h.status),
      'the verdict carries the app’s summary and the store witness', h.status);
  }

  // ---- cancel: mid-crossing aborts, mid-bake stops before anything leaves
  {
    xhr = null;
    const { h, s } = mk();
    const run = s.send('Dial', () => s.post(bake(), meta, false));
    await tick();
    xhr.upload.onprogress({ loaded: 1024, total: 4096 });
    s.cancel();
    await run;
    ok(h.sending === null && /cancelled — nothing was sent/.test(h.status), 'a cancel part way to the hub aborts and says nothing reached the orb', h.status);
    xhr = null; calls = [];
    const b = mk();
    await b.s.send('Dial', async () => { b.s.cancel(); await b.s.post(bake(), meta, false); });
    ok(xhr === null && !calls.some((c) => /precheck/.test(c[0])) && /cancelled/.test(b.h.status), 'a cancel during the bake stops before any bytes move', b.h.status);
  }

  // ---- the precheck's answers
  {
    // a duplicate: held for Replace, asked in the host's own UI, nothing uploaded
    scen = { known: '192.0.2.7', precheck: () => resp(200, { ok: true, data: { duplicate: true, note: 'a different build' } }) };
    xhr = null;
    const { h, s } = mk();
    const bytes = bake();
    await s.send('Dial', () => s.post(bytes, meta, false));
    ok(h.dup && h.dup.pre === true && h.dup.nm === 'Dial' && xhr === null, 'a duplicate is asked before any byte crosses', JSON.stringify(h.dup));
    ok(h.sending === null, 'and the question takes the screen: the send dialog closes', JSON.stringify(h.sending));
    ok(s.held && s.held.bytes === bytes && s.held.nm === 'Dial', 'and the refused bake is held');
    // Replace ships THOSE bytes with overwrite and no second precheck
    scen.precheck = () => { throw new Error('the replace asked again'); };
    const run = s.replace();
    await tick();
    ok(h.dup === null && xhr && xhr.body === bytes && /overwrite=1/.test(xhr.url), 'Replace closes the question and ships the held bytes with overwrite');
    answer({ ok: true, data: { store: 'stored', back: true } });
    await run;
    ok(s.held === null && /\(replacing\)|stored into the one slot/.test(h.status), 'and spends the hold', h.status);
    // a second press on the held name is the same Replace; another name is a fresh bake
    scen.precheck = () => resp(200, { ok: true, data: { duplicate: true } });
    const c = mk();
    await c.s.send('Dial', () => c.s.post(bytes, meta, false));
    let baked = false;
    await c.s.send('Other', async () => { baked = true; });
    ok(baked && c.s.held === null, 'a press under another name drops the hold and bakes afresh');
    await c.s.send('Dial', () => c.s.post(bytes, meta, false));
    c.s.dismiss();
    ok(c.s.held === null && c.h.dup === null && /the card keeps its current "Dial"/.test(c.h.status), 'Cancel on the question drops the hold and says the card is kept', c.h.status);
  }
  {
    // an exact match: stop before the upload, nothing held
    scen = { known: '192.0.2.7', precheck: () => resp(200, { ok: true, data: { exact: true } }) };
    xhr = null;
    const { h, s } = mk();
    await s.send('Dial', () => s.post(bake(), meta, false));
    ok(xhr === null && s.held === null && h.dup === null && /already on the card, byte for byte/.test(h.status), 'an exact match uploads nothing and holds nothing', h.status);
    ok(h.sending && h.sending.done && h.sending.outcome === 'ok', 'and ends as done, not as a failure', JSON.stringify(h.sending));
  }
  {
    // no clean read: fail closed, hold the bake, name the hub's reason
    scen = { known: '192.0.2.7', precheck: () => resp(200, { ok: true, data: { duplicate: false, note: 'the orb did not answer the list' } }) };
    xhr = null;
    const { h, s } = mk();
    await s.send('Dial', () => s.post(bake(), meta, false));
    ok(xhr === null && !!s.held && /could not get a clean read .* the orb did not answer the list/.test(h.status), 'a precheck that could not read the card stops before upload with the hub’s reason', h.status);
    ok(h.sending && h.sending.done && h.sending.outcome === 'fail', 'and ends as not sent, its reason left up to read', JSON.stringify(h.sending));
    // a refusal naming the bytes the index needs is answered once with exactly that many
    const asked = [];
    scen.precheck = (body) => { asked.push(body.byteLength); return asked.length === 1 ? resp(200, { ok: false, error: 'send the first 900 B of the file' }) : resp(200, { ok: true, data: { duplicate: true } }); };
    const n = mk();
    await n.s.send('Dial', () => n.s.post(bake(), meta, false));
    ok(asked.join() === '328,900' && n.h.dup, 'a short-slice refusal is retried once with the count the hub named', asked.join());
  }

  // ---- the late 409 and the commit token
  {
    scen = { known: '192.0.2.7' };
    xhr = null; calls = [];
    const { h, s } = mk();
    const bytes = bake();
    const run = s.send('Dial', () => s.post(bytes, meta, false));
    await tick();
    xhr.status = 409; xhr.responseText = JSON.stringify({ ok: false, error: 'a different "Dial" is already on the card', commit: 'tok123' }); xhr.onload();
    await run;
    ok(h.dup && h.dup.pre === false && s.held && s.held.commit === 'tok123', 'a late device refusal holds the bake with the commit token', JSON.stringify(h.dup));
    xhr = null;
    await s.replace();
    const cm = calls.find((c) => /theme\/commit/.test(c[0]));
    ok(cm && JSON.parse(cm[1]).commit === 'tok123' && xhr === null, 'Replace spends the token on an empty POST: the bytes do not cross again');
    ok(/the new build replaced the old copy on its card · stored — rebooting onto it/.test(h.status), 'and says the new build landed', h.status);
    // a refused commit falls back to shipping the bytes
    const b = mk();
    const run2 = b.s.send('Dial', () => b.s.post(bytes, meta, false));
    await tick();
    xhr.status = 409; xhr.responseText = JSON.stringify({ ok: false, error: 'already on the card', commit: 'stale' }); xhr.onload();
    await run2;
    scen.commitStatus = 410; scen.commit = { ok: false, error: 'stale token' };
    xhr = null;
    const run3 = b.s.replace();
    await tick();
    ok(xhr && xhr.body === bytes && /overwrite=1/.test(xhr.url), 'a refused commit ships the held bytes instead');
    answer({ ok: true, data: { store: 'stored', back: true } });
    await run3;
  }

  // ---- the store verdicts
  for (const [store, re] of [['torn', /the copy tore before its header/], ['refused', /the store refused: busy/], ['lost', /stayed unreported past the wait/]]) {
    scen = { known: '192.0.2.7' };
    xhr = null;
    const { h, s } = mk();
    const run = s.send('Dial', () => s.post(bake(), meta, false));
    await tick();
    answer({ ok: true, data: { store, store_note: 'busy' } });
    await run;
    ok(re.test(h.status), 'a ' + store + ' store is said plainly', h.status);
  }

  console.log(fails ? 'RESULT: FAIL — ' + fails + ' of ' + checked + ' checks' : 'all green (' + checked + ' checks)');
  process.exit(fails ? 1 : 0);
})().catch((e) => { console.log('FAIL threw: ' + (e && e.stack || e)); process.exit(1); });
