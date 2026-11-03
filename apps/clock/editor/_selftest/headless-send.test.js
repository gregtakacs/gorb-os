#!/usr/bin/env node
'use strict';
// Send to orb is one modal act (owner report, 2026-11-03: the editor could be left
// mid-upload, and "lord knows where that leads"). From the press to the verdict the page
// holds a modal, tells the hub it is busy, refuses to close and guards the window; the
// owner may cancel only while the bytes are still this page's, and once the hub holds the
// whole file the cancel locks, because the orb's store cannot be stopped part way.
require('./headless-stub.js');
global.React = { createRef: () => ({ current: null }) };
require('./component.js');
let fails = 0, checked = 0;
const ok = (c, n, x) => { checked++; if (!c || process.env.VERBOSE) console.log((c ? 'PASS ' : 'FAIL ') + n + (x ? '  [' + x + ']' : '')); if (!c) fails++; };

const DCLogic = function () { };
DCLogic.prototype.setState = function (p) { Object.assign(this.state, typeof p === 'function' ? p(this.state) : p); };
DCLogic.prototype.forceUpdate = function () { };

// what the page tells its parent, and the window guards it arms
const posted = [];
window.parent = { postMessage: (m) => posted.push(m) };
const unloads = new Set();
const addL = window.addEventListener, rmL = window.removeEventListener;
window.addEventListener = function (t, fn) { if (t === 'beforeunload') unloads.add(fn); else if (addL) addL.apply(this, arguments); };
window.removeEventListener = function (t, fn) { if (t === 'beforeunload') unloads.delete(fn); else if (rmL) rmL.apply(this, arguments); };
global.location = window.location = { origin: 'http://hub', protocol: 'http:', search: '' };

// the hub: the precheck answers clear, the upload is an XHR the test drives by hand
global.fetch = async (url) => ({ status: 200, text: async () => JSON.stringify({ ok: true, data: { duplicate: false } }), json: async () => ({ ok: true, data: { phase: 'idle' } }) });
let xhr = null;
global.XMLHttpRequest = function () {
  xhr = this; this.upload = {}; this.status = 0; this.responseText = '';
  this.open = () => {}; this.setRequestHeader = () => {}; this.send = () => {};
  this.abort = () => { if (this.onabort) this.onabort(); };
};
const answer = (j) => { xhr.status = 200; xhr.responseText = JSON.stringify(j); xhr.onload(); };
const progress = (loaded, total) => xhr.upload.onprogress({ loaded, total });
const tick = () => new Promise((r) => setImmediate(r));

const mk = () => {
  const it = new (global.COMP_CLASS(DCLogic))({});
  it.hubOrbIp = async () => '192.0.2.7';
  // the bake is not this suite's subject: a send press goes straight to the post
  it.exportTheme3 = async () => it.postTheme3ToOrb(new Uint8Array(4096), 'Test', 'clock-test-000000000000', { sentence: 'p' }, 'm', false);
  return it;
};
const busy = () => posted.filter((m) => m.type === 'theme.busy').map((m) => m.on);

(async () => {
  // a send that lands
  let it = mk();
  const run = it.sendTheme3ToOrb();
  await tick(); await tick();
  ok(!!it.state.sending && it.state.sending.canCancel, 'the press opens the send modal, cancellable while the bytes are the page’s');
  ok(busy().join() === 'true', 'and tells the hub the editor is busy', busy().join());
  ok(unloads.size === 1, 'and guards the window against closing');
  posted.length = 0; it.closeEditor();
  ok(!posted.some((m) => m.type === 'theme.close'), 'the editor will not close mid-send');
  ok(!!xhr, 'the bytes ride an XHR to the hub');
  progress(4096, 4096);
  ok(it.state.sending && !it.state.sending.canCancel, 'once the hub holds every byte the cancel locks');
  it.cancelSend3();
  ok(!it._cancel3, 'and a cancel press there does nothing');
  answer({ ok: true, data: { name: 'Test', id: 'clock-test-000000000000', store: 'stored', back: true } });
  await run;
  ok(it.state.sending === null, 'the verdict closes the modal');
  ok(busy().join() === 'false', 'and tells the hub the editor is free', busy().join());
  ok(unloads.size === 0, 'and drops the window guard');
  ok(/the orb is back drawing it/.test(it.state.status), 'the verdict says the orb is back', it.state.status);

  // a send cancelled while the bytes cross to the hub
  posted.length = 0; xhr = null; it = mk();
  const run2 = it.sendTheme3ToOrb();
  await tick(); await tick();
  progress(1024, 4096);
  ok(it.state.sending && it.state.sending.canCancel, 'part way to the hub, the cancel is live');
  it.cancelSend3();
  await run2;
  ok(it.state.sending === null && /cancelled — nothing was sent/.test(it.state.status), 'cancel aborts the crossing and says nothing reached the orb', it.state.status);
  ok(busy().join() === 'true,false' && unloads.size === 0, 'and frees the editor and the window', busy().join());

  // a cancel pressed before the post (mid-bake) stops it before anything leaves
  posted.length = 0; xhr = null; it = mk();
  const bake = it.exportTheme3;
  it.exportTheme3 = async () => { it.cancelSend3(); return bake(); };
  await it.sendTheme3ToOrb();
  ok(xhr === null && /cancelled/.test(it.state.status), 'a cancel during the bake stops the send before any bytes move', it.state.status);

  console.log(fails ? 'RESULT: FAIL — ' + fails + ' of ' + checked + ' checks' : 'all green (' + checked + ' checks)');
  process.exit(fails ? 1 : 0);
})().catch((e) => { console.log('FAIL threw: ' + (e && e.stack || e)); process.exit(1); });
