#!/usr/bin/env node
// D43 P5d.3: wheels and the cutouts they own -- what a wheel reads, its digits, how it
// turns, where its cutouts land (joined into one frame or not), Layers, canon, the bake.
'use strict';
require('./headless-stub.js');
const E = global.WatchEngine;
global.React = { createRef: () => ({ current: null }) };
require('./component.js');
const B = require(require('./runtime-paths.js').BAKER);
const S = require('../stack.js');
let fails = 0, checked = 0;
const ok = (c, n, x) => { checked++; if (!c || process.env.VERBOSE) console.log((c ? 'PASS ' : 'FAIL ') + n + (x ? '  [' + x + ']' : '')); if (!c) fails++; };
const near = (a, b, e) => Math.abs(a - b) < (e || 1e-6);
const O = { finish: 'brass', train: 'classic', frames: 16, tick: 'sweep', beats: 1 };
const face = (comps) => E.face(Object.assign({ design: E.canonDesign({ print: 'roman', comps }) }, O));

// ---- what a wheel reads ----------------------------------------------------------------
{
  const d = (dd, h, m) => new Date(2026, 0, dd, h || 0, m || 0);
  const w = E.wheelSpec('date', 'whole'), t = E.wheelSpec('date', 'tens', true), u = E.wheelSpec('date', 'units');
  ok(w.n === 31 && w.lab(0) === '1' && w.lab(30) === '31' && near(Math.floor(w.at(d(9, 12))), 8), 'a date wheel carries 1..31 and stands at the date');
  ok(t.n === 4 && t.lab(0) === '' && t.lab(3) === '3' && u.n === 10, 'a big date’s tens wheel runs 0..3 (0 left blank on request), its units 0..9');
  ok(Math.floor(t.at(d(9, 12))) === 0 && Math.floor(u.at(d(9, 12))) === 9 && Math.floor(t.at(d(27, 12))) === 2 && Math.floor(u.at(d(27, 12))) === 7, 'the 9th reads 0|9 and the 27th 2|7');
  ok(Math.floor(t.at(d(9, 23, 59))) === 0 && t.at(d(9, 23, 59)) > 0.99 && Math.floor(u.at(d(19, 23, 59))) === 9, 'a digit only creeps in the unit before it changes, and holds its figure until then');
  ok(E.wheelSpec('hour', 'tens').n === 2 && E.wheelSpec('hours24', 'tens').n === 3 && E.wheelSpec('minute', 'tens').n === 6, 'tens wheels size to the measure (1x hour, 2x 24-hour, 5x minute)');
  ok(E.wheelSpec('day').lab(1) === 'MON' && E.wheelSpec('month').lab(10) === 'NOV', 'day and month wheels print their names');
}
// ---- cutouts: holes in the face the wheel sits under ----------------------------------------
{
  const f = face([
    { id: 'wt', type: 'wheel', host: 'dial', angle: 90, dist: 110, size: 74, shows: 'date', digit: 'tens', blank0: true },
    { id: 'wu', type: 'wheel', host: 'dial', angle: 90, dist: 110, size: 54, shows: 'date', digit: 'units' },
    { id: 'ct', type: 'cutout', host: 'wt', at: 270, join: 'cu' }, { id: 'cu', type: 'cutout', host: 'wu', at: 270 },
    { id: 'wd', type: 'wheel', host: 'dial', angle: 180, dist: 130, size: 60, shows: 'day' }, { id: 'cd', type: 'cutout', host: 'wd', at: 0, shape: 'arc', span: 70 },
    { id: 's1', type: 'subdial', host: 'dial', angle: 270, dist: 100, size: 46, scale: 'none' },
    { id: 'wm', type: 'wheel', host: 's1', angle: 90, dist: 30, size: 30, shows: 'month', overhang: 'shown' }, { id: 'cm', type: 'cutout', host: 'wm', at: 270 }]);
  const L = f.lay, wt = L.wheels.find((q) => q.id === 'wt'), wu = L.wheels.find((q) => q.id === 'wu');
  const rects = L.dialHoles.filter((h) => h.kind === 'rect');
  ok(rects.length === 1 && rects[0].seal && wt.holes[0] === wu.holes[0], 'two joined cutouts are one sealed frame on the dial, owned by both wheels', rects.length + '');
  ok((L.bars.dial || []).length === 1, 'with a bar across the seam');
  ok(L.dialHoles.some((h) => h.kind === 'arc' && h.r1 > h.r0 && h.a1 > h.a0), 'an arc cutout is an annular sector about its wheel');
  ok(L.subHoles.s1.some((h) => h.kind === 'rect') && L.wheels.find((q) => q.id === 'wm').overhang === true, 'a wheel under a sub-dial cuts that sub-dial, and may show its overhang');
  ok(L.hits.some((h) => h.id === 'cd') && L.hits.some((h) => h.id === 'wd'), 'a cutout and its wheel can both be picked on the stage');
  const c = f.comps(new Date(2026, 10, 2, 10, 9), null);
  ok(c && c.dial && c.subs.s1, 'wheels draw into the layers under their faces');
  const sorted = L.wheels.filter((q) => q.host === 'dial').map((q) => q.r);
  ok(sorted.length === 3, 'every wheel is laid out');
}
// ---- canon ----------------------------------------------------------------------------------
{
  const c = E.canonDesign({ comps: [{ id: 'w', type: 'wheel', host: 'dial', shows: 'nope', digit: 'whole', dir: 'cw', cadence: 'jump', ease: 0, overhang: 'shown', wbg: 'auto', tsize: 1, blank0: true },
    { id: 'k', type: 'cutout', host: 'w', at: 90, angle: 3, dist: 4, shape: 'window', style: 'frame', span: 30, bar: true, w: 0, frameColor: 'gilt' }] }).comps;
  ok(c[0].shows === 'date' && ['digit', 'dir', 'cadence', 'ease', 'overhang', 'wbg', 'tsize', 'blank0'].every((k) => !(k in c[0])), 'a wheel keeps no default and no field its state cannot use', JSON.stringify(c[0]));
  ok(['angle', 'dist', 'shape', 'style', 'span', 'bar', 'w', 'frameColor'].every((k) => !(k in c[1])) && c[1].at === 90, 'a cutout keeps only where it reads', JSON.stringify(c[1]));
}
// ---- Layers ----------------------------------------------------------------------------------
{
  const comps = [{ id: 'w', type: 'wheel', host: 'dial', shows: 'date' }, { id: 'k', type: 'cutout', host: 'w', at: 90 }, { id: 'k2', type: 'cutout', host: 'w', at: 270 }];
  const st = S.build({ comps, hands: [], order: [], shadows: false, carriage: false, E });
  const iw = st.findIndex((x) => x.id === 'comp:w'), ik = st.findIndex((x) => x.id === 'comp:k');
  ok(iw >= 0 && st[iw].kind === 'disc' && st[ik].parent === 'comp:w' && st[ik + 1].parent === 'comp:w', 'a wheel is a disc under the dial, its cutouts nested under it');
  const r = S.move({ comps, hands: [], order: [], E }, 'comp:k', -1);
  ok(!r.ok && /R4.*wheel/.test(r.reason), 'a cutout does not move on its own: it belongs to its wheel', r.reason);
  ok(S.rowCount({ comps, hands: [], order: [] }).rows === 1, 'R6: a wheel is one live row, its cutouts none');
}
// ---- the bake -----------------------------------------------------------------------------------
{
  const p = B.compPlan3([{ id: 'w', type: 'wheel', shows: 'day' }, { id: 'k', type: 'cutout', host: 'w' }, { id: 'u', type: 'wheel', shows: 'date', digit: 'units' }]);
  ok(p.rows.length === 2 && p.rows[0].src === 'weekday' && p.rows[0].count === 7 && p.rows[1].src === 'date' && p.rows[1].count === 31 && !p.refuses.length,
    'a jumping wheel is one plate per value (a digit wheel over the whole domain); its cutouts are not rows', JSON.stringify(p.rows.map((r) => r.src + '/' + r.count)));
  const q = B.compPlan3([{ id: 'w', type: 'wheel', shows: 'date', cadence: 'smooth' }]);
  ok(!q.rows.length && /turns smoothly.*Jump/.test(q.refuses[0] || ''), 'a smooth wheel is refused by name', q.refuses[0]);
}
// ---- the panels and adding -------------------------------------------------------------------------
{
  const DCLogic = function () { };
  DCLogic.prototype.setState = function (p) { Object.assign(this.state, typeof p === 'function' ? p(this.state) : p); };
  const it = new (global.COMP_CLASS(DCLogic))({});
  it.ready = true; it.rendering = false; it.budget = () => ({ fps: 8, dev: { pace: 8, cross: 0, cov: 0, why: '' } });
  it.state.dial = 'skeleton'; it.state.playing = false; it.state.hidden = {};
  it.state.designs = { skeleton: E.canonDesign({ print: 'roman', comps: [] }) };
  it.addComp('cutout');
  ok(!it.design().comps.length && /select the wheel/.test(it.state.status), 'a cutout cannot be added without a wheel to read');
  it.addComp('wheel');
  const w = it.design().comps.find((c) => c.type === 'wheel'), k = it.design().comps.find((c) => c.type === 'cutout');
  ok(w && k && k.host === w.id && it.state.sel === w.id, 'a new wheel comes with its cutout, and is selected', JSON.stringify(it.design().comps));
  it.addComp('cutout');
  const ks = it.design().comps.filter((c) => c.type === 'cutout');
  ok(ks.length === 2 && ks[1].host === w.id && ks[1].at !== ks[0].at, 'another cutout goes on the selected wheel, at a free place', JSON.stringify(ks));
  it.setState({ sel: w.id, selKind: 'comp', step: 'parts', tab: 'comps' });
  const fl = it.renderVals().fields, lab = (l) => fl.find((x) => x.label === l);
  ok(lab('Shows') && lab('Digit') && lab('Direction') && lab('Motion') && lab('Ease') && lab('Disc colour') && !lab('Rotation') && !lab('Style'), 'a wheel’s panel: what it shows, its digit, direction, motion and look');
  it.setState({ sel: ks[1].id, selKind: 'comp' });
  const fk = it.renderVals().fields, lk = (l) => fk.find((x) => x.label === l);
  ok(lk('Wheel') && lk('Reads at') && lk('Shape') && lk('Join with') && !lk('Host') && !lk('Angle') && !lk('Distance'), 'a cutout’s panel: its wheel, where it reads, its shape and join -- no host or placement of its own');
  ok(it.renderVals().catOpts.some((c) => /\+ Wheel/.test(c.label)) && it.renderVals().catOpts.some((c) => /\+ Cutout/.test(c.label)), 'the catalogue offers a wheel and a cutout');
}

console.log(fails ? 'RESULT: FAIL — ' + fails + ' of ' + checked + ' checks' : 'all green (' + checked + ' checks)');
process.exit(fails ? 1 : 0);
