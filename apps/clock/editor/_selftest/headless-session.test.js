#!/usr/bin/env node
'use strict';
// D43 P6 (§7), K9: the session. Undo and redo walk the watch's snapshots (a burst of edits is
// one step), nothing is autosaved to this browser, My designs are files on the hub (art and
// all, through runtime/_selftest/fake-hub.js), Reset asks twice, and the hub's ?orb= is the
// address the editor sends to.
require('./headless-stub.js');
const E = global.WatchEngine;
global.React = { createRef: () => ({ current: null }) };
require('./component.js');
let fails = 0, checked = 0;
const ok = (c, n, x) => { checked++; if (!c || process.env.VERBOSE) console.log((c ? 'PASS ' : 'FAIL ') + n + (x ? '  [' + x + ']' : '')); if (!c) fails++; };

const DCLogic = function () { };
DCLogic.prototype.setState = function (p) { Object.assign(this.state, typeof p === 'function' ? p(this.state) : p); this.componentDidUpdate && this.trackHistory(); };
DCLogic.prototype.forceUpdate = function () { };
const mk = () => {
  const it = new (global.COMP_CLASS(DCLogic))({});
  it.ready = true; it.rendering = false; it.budget = () => ({ fps: 8, dev: { pace: 8, cross: 0, cov: 0, why: '' } });
  it.state.dial = 'modern'; it.state.playing = false; it.state.hidden = {};
  it.BURST_MS = -1;   // every edit its own step, unless a check says otherwise
  return it;
};
// timers: run them on demand
const timers = [];
global.setTimeout = (fn) => { timers.push(fn); return timers.length; };
global.clearTimeout = () => {};
const flush = () => { while (timers.length) timers.shift()(); };
const comps = (it) => JSON.stringify(it.design().comps);

(async () => {
  window.localStorage.removeItem('wc.session.clock');
  const it = mk();
  it.trackHistory();                      // the baseline
  const c0 = comps(it), sz0 = it.design().comps.find((c) => c.type === 'subdial');
  const sid = sz0 && sz0.id;
  ok(!!sid, 'Modern has a sub-dial to edit', c0.slice(0, 80));
  it.setComp(sid, { size: 40 });
  it.setComp(sid, { size: 44 });
  ok(it._undo.length === 2, 'two edits, two undo steps', String(it._undo.length));
  it.undo();
  ok(it.design().comps.find((c) => c.id === sid).size === 40, 'undo steps back one edit');
  it.undo();
  ok(comps(it) === c0 && !it._undo.length, 'and back to the start', it.state.status);
  it.redo(); it.redo();
  ok(it.design().comps.find((c) => c.id === sid).size === 44 && !it._redo.length, 'redo walks forward again');
  it.undo(); it.setComp(sid, { size: 50 });
  ok(!it._redo.length, 'a new edit after an undo drops the redo branch');
  // a burst (a slider drag) is one step
  it.BURST_MS = 60000; it._snapAt = 0; const before = it._undo.length;   // the burst starts after a pause
  it.setComp(sid, { size: 51 }); it.setComp(sid, { size: 52 }); it.setComp(sid, { size: 53 });
  ok(it._undo.length === before + 1, 'a burst of edits is one undo step', String(it._undo.length - before));
  it.undo();
  ok(it.design().comps.find((c) => c.id === sid).size === 50, 'which undoes back to before the burst');
  it.BURST_MS = -1;
  // Ctrl+Z / Ctrl+Shift+Z, but not in a text box
  it.setComp(sid, { size: 60 });
  const key = (k, shift, tag, type) => it.onKey({ key: k, ctrlKey: true, shiftKey: !!shift, target: { tagName: tag || 'CANVAS', type }, preventDefault() {} });
  ok(key('z') && it.design().comps.find((c) => c.id === sid).size === 50, 'Ctrl+Z undoes');
  ok(key('Z', true) && it.design().comps.find((c) => c.id === sid).size === 60, 'Ctrl+Shift+Z redoes');
  ok(!key('z', false, 'INPUT', 'text') && it.design().comps.find((c) => c.id === sid).size === 60, 'and a text box keeps its own Ctrl+Z');
  ok(key('z', false, 'INPUT', 'range') && it.design().comps.find((c) => c.id === sid).size === 50, 'while a focused slider hands it to the watch');
  // a movement or case change is an undo step too
  it.setState({ caseStyle: 'none' });
  it.undo();
  ok(it.state.caseStyle !== 'none', 'the case is part of the watch: undone like a part');

  // no autosave (K9): an edit writes nothing to this browser
  window.localStorage.removeItem('wc.session.clock');
  it.setComp(sid, { size: 66 }); it.setState({ themeName: 'Mine' }); flush();
  ok(window.localStorage.getItem('wc.session.clock') === null && !('loadSession' in it) && !('saveSession' in it), 'an edit autosaves nothing, and there is no session to pick up');

  // Reset asks twice, and is undoable
  const it3 = mk(); it3.trackHistory(); it3.setComp(sid, { size: 30 });
  it3.renderVals().resetDesign();
  ok(it3.design().comps.find((c) => c.id === sid).size === 30 && /again/.test(it3.state.status), 'the first Reset press only asks');
  it3.renderVals().resetDesign();
  ok(comps(it3) === c0, 'the second resets', it3.state.status);
  it3.undo();
  ok(it3.design().comps.find((c) => c.id === sid).size === 30, 'and Ctrl+Z brings the edited watch back');

  // My designs, on the hub (K9): save with art, list, open, delete
  const fh = require('../../../../tools/orb-manager/web/runtime/_selftest/fake-hub.js')();
  const keepLoc = global.location, keepFetch = global.fetch;
  global.location = { pathname: '/editor/192.0.2.7/clock/', search: '', protocol: 'http:' };
  global.fetch = (url, o) => fh.fetch(url, o);
  const it4 = mk(); it4.trackHistory(); it4.setComp(sid, { size: 77 });
  E.assetRestore([{ id: 'k9-art', name: 'bg.png', kind: 'png', dataUrl: 'data:image/png;base64,azk=' }]);
  it4.state.designs = Object.assign({}, it4.state.designs, { modern: Object.assign(it4.design(), { bgArt: 'k9-art' }) });
  await it4.saveMine('Big sub');
  const l = it4.mineList(), held = fh.designs.get('clock').get('Big sub');
  ok(l.length === 1 && l[0].name === 'Big sub' && l[0].dial === 'modern' && /Saved “Big sub” to My designs on the hub/.test(it4.state.status), 'Save current files the watch on the hub under its name', it4.state.status);
  ok(held && held.format === 'wc-design' && held.art === true && (held.assets || []).some((a) => a.id === 'k9-art'), 'with its art inside', held && JSON.stringify(held.assets || []).slice(0, 80));
  await it4.saveMine('Big sub');
  ok(it4.mineList().length === 1, 'saving under the same name replaces it');
  ok(/save it to My designs, or export the design file or the theme, to keep it/.test(it4.artNote()), 'the art line names My designs as a way to keep it', it4.artNote());
  const it5 = mk(); it5.buildMovement = () => {}; await it5.session.boot();
  const rv = it5.renderVals();
  ok(rv.mine.length === 1 && !rv.mineEmpty && typeof rv.openFile === 'function', 'another page lists it beside Open a file');
  await rv.mine[0].load();
  ok(it5.state.dial === 'modern' && it5.design().comps.find((c) => c.id === sid).size === 77 && it5.state.themeName === 'Big sub' && E.assetKnown('k9-art'), 'Open loads the watch, its name and its art', it5.state.status);
  it5.renderVals().mine[0].del();
  ok(it5.mineList().length === 1, 'Delete asks first');
  await it5.renderVals().mine[0].del();
  ok(it5.mineList().length === 0 && it5.renderVals().mineEmpty, 'and removes it on the second press');
  global.location = keepLoc; global.fetch = keepFetch;
})().then(() => {

// ?orb= names the orb; ?app= keys the browser storage
{
  const it = mk();
  global.location = { search: '?orb=192.168.2.34&app=clock&label=Gold', protocol: 'http:' };
  let asked = false; global.fetch = async () => { asked = true; return { json: async () => ({ ok: true, data: { ip: '10.0.0.9' } }) }; };
  it.hubOrbIp().then((ip) => {
    ok(ip === '192.168.2.34' && !asked, 'the hub’s ?orb= is the address, without asking /api/orb/known', ip);
    global.location = { search: '?app=hsb', protocol: 'http:' };
    ok(it.session.slug() === 'hsb' && it.mineKey() === 'wc.mine.hsb', '?app= names the app My designs are filed under');
    global.location = { search: '?app=../x', protocol: 'http:' };
    ok(it.session.slug() === 'clock', 'a slug that is not one falls back to the clock’s');
    console.log(fails ? 'RESULT: FAIL — ' + fails + ' of ' + checked + ' checks' : 'all green (' + checked + ' checks)');
    process.exit(fails ? 1 : 0);
  });
}
});
