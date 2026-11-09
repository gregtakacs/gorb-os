#!/usr/bin/env node
// D43 P4: the stack panel, driven through the real component (renderVals' stack rows).
'use strict';
require('./headless-stub.js');
const legacy3 = global.legacy3;
const E = global.WatchEngine;
global.React = { createRef: () => ({ current: null }) };
require('../clock-logic.js');
let fails = 0, checked = 0;
const ok = (c, n, x) => { checked++; if (!c || process.env.VERBOSE) console.log((c ? 'PASS ' : 'FAIL ') + n + (x ? '  [' + x + ']' : '')); if (!c) fails++; };
const DCLogic = function () { };
DCLogic.prototype.setState = function (p) { Object.assign(this.state, typeof p === 'function' ? p(this.state) : p); };
const Comp = global.ClockLogic(DCLogic);
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

// the top hand down one: the live band (the dial's own hands, D43 P5d) with the top two traded
const fc = it.face(), order = window.OrbStack.liveKeys(it.stackInput()), drawn = order.filter((id) => fc.hands.some((h) => h.id === id));
const topId = drawn[drawn.length - 1], nextId = drawn[drawn.length - 2];
const want = order.slice(); want[order.indexOf(topId)] = nextId; want[order.indexOf(nextId)] = topId;
const topRow = V().stackRows.find((r) => r.isRow && r.upOp === 0.35 && r.arrows === 'visible');
ok(!!topRow && /R1/.test(topRow.upTip), 'the top layer’s up arrow is dim and names R1', topRow && topRow.upTip);
topRow.down(ev);
ok((it.design().order || []).join() === want.join(), 'the down arrow trades the top two hands in design.order', (it.design().order || []).join());
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
ok(V().stackRows.some((r) => r.isRow && r.edge === '#58a6ff' && /\bon\b/.test(r.cls)), 'the selected layer is marked in the panel');

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
// D43 P5c: the preset's hands are comps; the dial's hour and minute by what they show
const hidOf = (k) => it.design().comps.find((c) => c.type === 'hand' && (c.host || 'dial') === 'dial' && c.shows === k).id;
const o2 = it.face().handOrder.slice(), ih = o2.indexOf(hidOf('hour')), im = o2.indexOf(hidOf('minute'));
if (ih >= 0 && im >= 0) { o2[ih] = hidOf('minute'); o2[im] = hidOf('hour'); it.upd((dd) => { dd.order = o2; }); }
const v3 = V();
ok(v3.stackWarns.some((w) => /Coaxial/.test(w.text) && /hour hand sits above the faster minute/.test(w.text)) && v3.stackRows.some((r) => r.isRow && / ⚠$/.test(r.warn)), 'R8: the hour above the minute warns, and its row carries the mark', v3.stackWarns.map((w) => w.text).join(' | '));
V().toggleMech();
ok(!it.design().mechanical && V().stackWarns.length === 0, 'switching it off clears the warnings and the field');

// the plate band's rows open the Movement tab where their controls are: the geartrain at
// the top, the clock insides at Backdrop, which sits at the bottom with the plate's art
{
  const gt = row(/^Geartrain/), mv = row(/^Clock insides$/);
  gt.pick();
  ok(it.state.selKind === 'move' && it.state.tab === 'move' && it._scrollTo === '#', 'the geartrain row opens the Movement tab at its top', it.state.selKind + '/' + it.state.tab + ' ' + it._scrollTo);
  ok(row(/^Geartrain/).edge === '#58a6ff', 'and is marked selected in the panel');
  mv.pick();
  ok(it.state.sel === 'movement' && it.state.tab === 'insides' && it._scrollTo === 'move-backdrop', 'the clock insides row opens its own sub-tab, at Backdrop', it.state.sel + ' ' + it._scrollTo);
  const heads = V().fields.filter((f) => f.isHead);
  const bi = heads.findIndex((f) => f.anchor === 'move-backdrop'), ai = heads.findIndex((f) => /^Background art/.test(f.label));
  ok(bi >= 0 && ai === bi + 1 && ai === heads.length - 1, 'Backdrop is the last section of the Movement tab, with the background art under it', heads.map((f) => f.label).join(' | '));
  it.setState({ tab: 'dial', sel: null, selKind: null });
  ok(!V().fields.some((f) => f.isHead && /^Background art/.test(f.label)) && V().fields.some((f) => f.isHead && /^Painted surface/.test(f.label)), 'the Dial tab keeps its own surface art and no longer offers the plate backdrop');
  it.setState({ tab: 'dial' });
}

// the eye hides a group on the stage only
const eyeRow = row(/^Glass$/);
eyeRow.eyeToggle(ev);
ok(it.state.hidden.glass === true, 'the eye hides the glass on the stage');

// D43 P4b: the lowest hand's down arrow lifts the face below it among the hands
{
  it.upd((dd) => { delete dd.order; delete dd.mechanical; });
  it.setState({ hidden: {} });
  const live = V().stackRows.filter((r) => r.isRow && !r.parent && r.arrows === 'visible' && /hand/i.test(r.label));
  const low = live[live.length - 1];
  low.down(ev);
  const ord = it.design().order || [];
  ok(ord.some((e) => String(e).indexOf('comp:') === 0), 'the arrow writes the lifted face into design.order', ord.join());
  const fc2 = it.face();
  ok(fc2.liveSeq.some((e) => e.kind === 'sub'), 'the face the stage draws carries it in its live band');
  ok(V().stackRows.some((r) => r.isRow && /above the hands/.test(r.note)), 'its row is badged above the hands');
  ok(/^\d+ of 24/.test(V().stackCap), 'the R6 count still reads', V().stackCap);
  it.upd((dd) => { delete dd.order; });
}

// D43 P5d: a drag opens a gap where the layer would land, or names the rule that refuses it
{
  it.upd((dd) => { delete dd.order; });
  const rowsD = () => V().stackRows;
  const dHand = rowsD().find((r) => r.isRow && !r.parent && /hand/i.test(r.label));
  const sub = rowsD().find((r) => r.isRow && /^Sub-dial/.test(r.label));
  dHand.dragStart(ev);
  sub.dragOver(Object.assign({}, ev, { where: 'below' }));
  const r1 = rowsD(), g1 = r1.find((r) => r.isGap);
  ok(g1 && g1.cls === '' && /on Sub-dial/.test(g1.label) && r1.indexOf(g1) === r1.findIndex((r) => r.id === sub.id) + 1, 'hovering below a sub-dial opens a gap inside it, saying the hand goes on that face', g1 && g1.label);
  const dialRow = rowsD().find((r) => r.isRow && r.label === 'Dial');
  dialRow.dragOver(Object.assign({}, ev, { where: 'below' }));
  const g2 = rowsD().find((r) => r.isGap);
  ok(g2 && g2.cls === 'no' && /R3/.test(g2.label), 'hovering under the dial shows the refusal where the gap would be', g2 && g2.label);
  sub.dragOver(Object.assign({}, ev, { where: 'below' }));
  rowsD().find((r) => r.isGap).drop(ev);
  const moved = it.design().comps.find((c) => c.id === dHand.id.slice(5));
  ok(moved && moved.host === sub.id.slice(5) && !rowsD().some((r) => r.isGap), 'releasing on the gap re-hosts the hand on that sub-dial, and the gap closes', JSON.stringify(moved));
  ok(rowsD().some((r) => r.isRow && r.id === dHand.id && r.parent === sub.id), 'and Layers shows it nested under that face');
  it.upd((dd) => { const c = dd.comps.find((x) => x.id === moved.id); c.host = 'dial'; });
}

// D43 P4b: a lifted face over the sweep is priced — the pace drops, the reason names it,
// and its stack row says how often it repaints
{
  const it2 = new Comp({});
  it2.ready = true; it2.rendering = false; it2.state.dial = 'skeleton'; it2.state.playing = false; it2.state.hidden = {};
  it2.state.designs = { skeleton: E.canonDesign(legacy3({ print: 'roman', centre: { hour: 1, minute: 1, second: 1 },
    comps: [{ id: 's1', type: 'subdial', host: 'dial', angle: 180, dist: 100, size: 60, scale: 'none', style: 'guilloche' }] })) };
  const b0 = it2._devPace({ fps: 15, measured: true });
  it2.upd((dd) => { dd.order = it2.face().handOrder.concat(['comp:s1']); });
  const b1 = it2._devPace({ fps: 15, measured: true });
  ok(b0.cov > 0 && b1.covLift > 0 && b1.pace < b0.pace, 'lifting the face the sweep crosses lowers the estimated pace', b0.pace + ' -> ' + b1.pace + ' fps (cov ' + b1.covLift.toFixed(3) + ')');
  ok(/lifted face/.test(b1.why), 'and the estimate says why', b1.why);
  it2.budget = () => ({ fps: 15, dev: it2._devPace({ fps: 15, measured: true }) });
  const row = it2.renderVals().stackRows.find((r) => r.isRow && /above the hands/.test(r.note));
  ok(row && /repainted on ~\d+% of sweep ticks/.test(row.note), 'its stack row names the repaint share', row && row.note);
}

// D43 §2.3: a property that follows the dial says so, and re-links in one click
{
  it.upd((dd) => { dd.comps = (dd.comps || []).concat([{ id: 'fx', type: 'flare', host: 'dial', angle: 0, dist: 60, size: 10, style: 'print', icon: 'text', text: 'LINK' }]); });
  it.setState({ tab: 'comps', sel: 'fx', selKind: 'comp' });
  const tf = () => it.renderVals().fields.find((f) => f.label === 'Typeface' && f.isSelect);
  const f0 = tf();
  ok(f0 && f0.hasLink && /the defaults/.test(f0.linkText), 'a flare\u2019s typeface starts on the default face (there is no dial typeface)', f0 ? f0.linkText : it.state.tab + ' sel=' + it.state.sel + ' ' + it.renderVals().fields.filter((f) => f.isRow).map((f) => f.label).join(','));
  f0.on({ target: { value: E.TYPEFACES ? Object.keys(E.TYPEFACES)[1] : 'grotesk' } });
  const f1 = tf();
  ok(f1 && /re-link/.test(f1.linkText) && it.design().comps.find((c) => c.id === 'fx').font, 'choosing a face overrides it, and the row offers the re-link', f1 && f1.linkText);
  f1.relink();
  ok(!it.design().comps.find((c) => c.id === 'fx').font && /the defaults/.test(tf().linkText), 're-linking goes back to the default face');
}

console.log(fails ? 'RESULT: FAIL — ' + fails + ' of ' + checked + ' checks' : 'all green (' + checked + ' checks)');
process.exit(fails ? 1 : 0);
