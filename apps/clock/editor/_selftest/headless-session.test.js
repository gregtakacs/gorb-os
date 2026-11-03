#!/usr/bin/env node
'use strict';
// D43 P6 (§7): the session. Undo and redo walk the watch's snapshots (a burst of edits is one
// step), autosave writes the newest to this browser and a reload restores it, My designs keeps
// named Design IDs with a JSON export and import, Reset asks twice, and the hub's ?orb= is the
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
// timers: autosave waits 700 ms; run them on demand
const timers = [];
global.setTimeout = (fn) => { timers.push(fn); return timers.length; };
global.clearTimeout = () => {};
const flush = () => { while (timers.length) timers.shift()(); };
const comps = (it) => JSON.stringify(it.design().comps);

{
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

  // autosave, and a reload that restores it
  it.setComp(sid, { size: 66 }); it.setState({ themeName: 'Mine' }); flush();
  const saved = JSON.parse(window.localStorage.getItem('wc.session.clock') || 'null');
  ok(saved && saved.v === 1 && saved.dial === 'modern' && saved.themeName === 'Mine', 'autosave writes the watch to this browser', saved && Object.keys(saved).join(','));
  const it2 = mk(); it2.state.dial = 'skeleton';
  ok(it2.loadSession() && it2.state.dial === 'modern' && it2.design().comps.find((c) => c.id === sid).size === 66 && it2.state.themeName === 'Mine', 'a reload picks it up', it2.state.status);
  ok(/Picked up where you left off/.test(it2.state.status), 'and says so');
  it2.trackHistory();
  ok(!it2._undo.length, 'the restored watch is where the new session’s history starts');
  it2.setComp(sid, { size: 70 });
  ok(it2._undo.length === 1, 'and the first edit after it is undoable');
  window.localStorage.setItem('wc.session.clock', '{not json');
  ok(!mk().loadSession(), 'a damaged session is ignored, not thrown');

  // Reset asks twice, and is undoable
  const it3 = mk(); it3.trackHistory(); it3.setComp(sid, { size: 30 });
  it3.renderVals().resetDesign();
  ok(it3.design().comps.find((c) => c.id === sid).size === 30 && /again/.test(it3.state.status), 'the first Reset press only asks');
  it3.renderVals().resetDesign();
  ok(comps(it3) === c0, 'the second resets', it3.state.status);
  it3.undo();
  ok(it3.design().comps.find((c) => c.id === sid).size === 30, 'and Ctrl+Z brings the edited watch back');

  // My designs: save, list, export/import, open, delete
  window.localStorage.removeItem('wc.mine.clock');
  const it4 = mk(); it4.trackHistory(); it4.setComp(sid, { size: 77 });
  it4.saveMine('Big sub');
  const l = it4.mineList();
  ok(l.length === 1 && l[0].name === 'Big sub' && /^WCD5-/.test(l[0].id) && l[0].dial === 'modern', 'Save current keeps a named Design ID', JSON.stringify(l[0] && l[0].name));
  it4.saveMine('Big sub');
  ok(it4.mineList().length === 1, 'saving under the same name replaces it');
  let blob = null; global.URL = { createObjectURL: (b) => { blob = b; return 'blob:x'; }, revokeObjectURL() {} }; global.Blob = function (parts) { this.text = parts.join(''); };
  const doc = global.document; const clicks = []; doc.createElement = ((orig) => (t) => { const el = orig ? orig.call(doc, t) : {}; if (t === 'a') { el.click = () => clicks.push(el.download); el.remove = () => {}; } return el; })(doc.createElement);
  doc.body = doc.body || {}; doc.body.appendChild = doc.body.appendChild || (() => {});
  it4.exportMine();
  ok(clicks[0] === 'clock-designs.json' && blob && JSON.parse(blob.text).designs.length === 1, 'Export all downloads one JSON file', clicks.join());
  const text = blob.text;
  window.localStorage.removeItem('wc.mine.clock');
  const it5 = mk(); it5.importMine(text);
  ok(it5.mineList().length === 1 && it5.mineList()[0].name === 'Big sub', 'Import brings them into another browser', it5.state.status);
  it5.importMine(text);
  ok(it5.mineList().length === 1 && /already here/.test(it5.state.status), 'and does not duplicate what is there');
  it5.importMine('{"format":"nope"}');
  ok(/not a My designs export/.test(it5.state.status), 'a foreign file is refused by name');
  const rv = it5.renderVals();
  ok(rv.mine.length === 1 && !rv.mineEmpty, 'the Start sheet lists them');
  rv.mine[0].load();
  ok(it5.state.dial === 'modern' && it5.design().comps.find((c) => c.id === sid).size === 77 && it5.state.themeName === 'Big sub', 'Open loads the watch and its name');
  it5.renderVals().mine[0].del();
  ok(it5.mineList().length === 1, 'Delete asks first');
  it5.renderVals().mine[0].del();
  ok(it5.mineList().length === 0 && it5.renderVals().mineEmpty, 'and removes it on the second press');
}

// ?orb= names the orb; ?app= keys the browser storage
{
  const it = mk();
  global.location = { search: '?orb=192.168.2.34&app=clock&label=Gold', protocol: 'http:' };
  let asked = false; global.fetch = async () => { asked = true; return { json: async () => ({ ok: true, data: { ip: '10.0.0.9' } }) }; };
  it.hubOrbIp().then((ip) => {
    ok(ip === '192.168.2.34' && !asked, 'the hub’s ?orb= is the address, without asking /api/orb/known', ip);
    global.location = { search: '?app=hsb', protocol: 'http:' };
    ok(it.sessionKey() === 'wc.session.hsb' && it.mineKey() === 'wc.mine.hsb', '?app= keys the session and My designs per app');
    global.location = { search: '?app=../x', protocol: 'http:' };
    ok(it.sessionKey() === 'wc.session.clock', 'a slug that is not one falls back to the clock’s');
    console.log(fails ? 'RESULT: FAIL — ' + fails + ' of ' + checked + ' checks' : 'all green (' + checked + ' checks)');
    process.exit(fails ? 1 : 0);
  });
}
