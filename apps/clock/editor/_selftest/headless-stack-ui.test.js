#!/usr/bin/env node
// D43 P4: the stack panel, driven through the real component (renderVals' stack rows).
'use strict';
require('./headless-stub.js');
const E = global.WatchEngine;
global.React = { createRef: () => ({ current: null }) };
require('./component.js');
let fails = 0, checked = 0;
const ok = (c, n, x) => { checked++; if (!c || process.env.VERBOSE) console.log((c ? 'PASS ' : 'FAIL ') + n + (x ? '  [' + x + ']' : '')); if (!c) fails++; };
const DCLogic = function () { };
DCLogic.prototype.setState = function (p) { Object.assign(this.state, typeof p === 'function' ? p(this.state) : p); };
const Comp = global.COMP_CLASS(DCLogic);
const ev = { stopPropagation() {}, preventDefault() {}, dataTransfer: { setData() {} } };

const it = new Comp({});
it.ready = true; it.rendering = false; it.budget = () => ({ fps: 8, dev: { pace: 8, cross: 0, cov: 0, why: '' } });
it.state.dial = 'perpetual'; it.state.tab = 'dial'; it.state.playing = false; it.state.hidden = {};
it.state.designs = { perpetual: E.canonDesign(JSON.parse(JSON.stringify(E.PRESETS.perpetual.design))) };
const V = () => it.renderVals();
const row = (re) => V().stackRows.find((r) => r.isRow && re.test(r.label));

const v0 = V(), rows0 = v0.stackRows;
ok(rows0.filter((r) => r.isHead).map((r) => r.label).join(' | ') === 'Crystal · pinned on top | Layers | Plate · pinned at the bottom', 'the three bands head the panel', rows0.filter((r) => r.isHead).map((r) => r.label).join(' | '));
ok(rows0.filter((r) => r.isRow).length === window.OrbStack.build(it.stackInput()).length, 'one panel row per stack layer');
ok(/^\d+ of 24 live rows$/.test(v0.stackCap), 'R6: the header counts the live rows against the cap', v0.stackCap);
ok(rows0.filter((r) => r.isRow && r.pad === 22).length > 0, 'children are indented under their host');

// the top hand down one: exactly what moveHand(id, -1) wrote
const fc = it.face(), order = fc.handOrder.slice(), drawn = order.filter((id) => fc.hands.some((h) => h.id === id));
const topId = drawn[drawn.length - 1], nextId = drawn[drawn.length - 2];
const want = order.slice(); want[order.indexOf(topId)] = nextId; want[order.indexOf(nextId)] = topId;
const topRow = V().stackRows.find((r) => r.isRow && r.upOp === 0.35 && r.arrows === 'visible');
ok(!!topRow && /R1/.test(topRow.upTip), 'the top layer’s up arrow is dim and names R1', topRow && topRow.upTip);
topRow.down(ev);
ok((it.design().order || []).join() === want.join(), 'the down arrow writes what moveHand wrote', (it.design().order || []).join());
// a refusal leaves the design alone and says why
const before = JSON.stringify(it.design());
const top2 = V().stackRows.find((r) => r.isRow && r.arrows === 'visible');
top2.up(ev);
ok(JSON.stringify(it.design()) === before && /^R1/.test(it.state.status || ''), 'a refused move changes nothing and the status names the rule', it.state.status);

// the dial does not move, and says so
const dialRow = row(/^Dial$/);
ok(dialRow && dialRow.arrows === 'hidden' && /R7/.test(dialRow.tip), 'the dial shows no arrows and its tip names R7', dialRow && dialRow.tip);

// picking a row selects the layer and opens its tab
const sub = V().stackRows.find((r) => r.isRow && /^Sub-dial/.test(r.label));
sub.pick();
ok(it.state.selKind === 'comp' && it.state.tab === 'comps', 'a sub-dial row selects it on the Complications tab', it.state.selKind + '/' + it.state.tab);
const hand = V().stackRows.find((r) => r.isRow && /hand/i.test(r.label) && r.arrows === 'visible');
hand.pick();
ok(it.state.selKind === 'hand' && it.state.tab === 'hands', 'a hand row selects it on the Hands tab');
ok(V().stackRows.some((r) => r.isRow && r.edge === '#c9a25a'), 'the selected layer is marked in the panel');

// drag and drop between two sub-dials trades their comps slots
const subs = V().stackRows.filter((r) => r.isRow && /^Sub-dial/.test(r.label));
if (subs.length >= 2) {
  const ids0 = it.design().comps.filter((c) => c.type === 'subdial').map((c) => c.id).join();
  subs[0].dragStart(ev); subs[1].drop(ev);
  const ids1 = it.design().comps.filter((c) => c.type === 'subdial').map((c) => c.id).join();
  ok(ids1 !== ids0 && ids1.split(',').sort().join() === ids0.split(',').sort().join(), 'dropping one sub-dial on another reorders them', ids0 + ' -> ' + ids1);
} else ok(false, 'the perpetual preset has two sub-dials to drag');

// Mechanical: off by default; on, a hand stacked against its arbor warns on its row
ok(V().stackWarns.length === 0, 'no warning with the switch off');
V().toggleMech();
ok(it.design().mechanical === 1, 'the Mechanical switch is a design field');
// back to the preset's own order (the earlier arrow and drop moved it), then the hour
// above the minute, written through upd() like every panel move
it.upd((dd) => { delete dd.order; });
ok(V().stackWarns.length === 0, 'R8: the preset\u2019s own order raises no warning with the switch on', V().stackWarns.map((w) => w.text).join(' | '));
const o2 = it.face().handOrder.slice(), ih = o2.indexOf('centre-hour'), im = o2.indexOf('centre-minute');
if (ih >= 0 && im >= 0) { o2[ih] = 'centre-minute'; o2[im] = 'centre-hour'; it.upd((dd) => { dd.order = o2; }); }
const v3 = V();
ok(v3.stackWarns.some((w) => /Coaxial/.test(w.text) && /hour hand sits above the faster minute/.test(w.text)) && v3.stackRows.some((r) => r.isRow && / ⚠$/.test(r.warn)), 'R8: the hour above the minute warns, and its row carries the mark', v3.stackWarns.map((w) => w.text).join(' | '));
V().toggleMech();
ok(!it.design().mechanical && V().stackWarns.length === 0, 'switching it off clears the warnings and the field');

// the eye hides a group on the stage only
const eyeRow = row(/^Glass$/);
eyeRow.eyeToggle(ev);
ok(it.state.hidden.glass === true, 'the eye hides the glass on the stage');

// D43 §2.3: a property that follows the dial says so, and re-links in one click
{
  it.upd((dd) => { dd.comps = (dd.comps || []).concat([{ id: 'fx', type: 'flare', host: 'dial', angle: 0, dist: 60, size: 10, style: 'print', icon: 'text', text: 'LINK' }]); });
  it.setState({ tab: 'comps', sel: 'fx', selKind: 'comp' });
  const tf = () => it.renderVals().fields.find((f) => f.label === 'Typeface' && f.isSelect);
  const f0 = tf();
  ok(f0 && f0.hasLink && /the dial/.test(f0.linkText), 'a flare\u2019s typeface starts linked to the dial', f0 ? f0.linkText : it.state.tab + ' sel=' + it.state.sel + ' ' + it.renderVals().fields.filter((f) => f.isRow).map((f) => f.label).join(','));
  f0.on({ target: { value: E.TYPEFACES ? Object.keys(E.TYPEFACES)[1] : 'grotesk' } });
  const f1 = tf();
  ok(f1 && /re-link/.test(f1.linkText) && it.design().comps.find((c) => c.id === 'fx').font, 'choosing a face overrides it, and the row offers the re-link', f1 && f1.linkText);
  f1.relink();
  ok(!it.design().comps.find((c) => c.id === 'fx').font && /the dial/.test(tf().linkText), 're-linking follows the dial again');
}

console.log(fails ? 'RESULT: FAIL — ' + fails + ' of ' + checked + ' checks' : 'all green (' + checked + ' checks)');
process.exit(fails ? 1 : 0);
