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
// ---- P5d.3c: the author sizes the wheel, its figures and its cutout separately -------------------
{
  const base = { id: 'w', type: 'wheel', host: 'dial', angle: 0, dist: 0, size: 150, shows: 'date', digit: 'units', read: 90, fpx: 30 };
  const lay = (w, k) => face([w, Object.assign({ id: 'k', type: 'cutout', host: 'w', at: 90 }, k || {})]).lay;
  const a = lay(base), b = lay(base, { w: 80, h: 60 }), wa = a.wheels[0], wb = b.wheels[0];
  ok(wa.fs === 30 && wb.fs === 30 && wa.lr === wb.lr, 'the figures keep their px size and radius whatever the cutout\u2019s size', wa.fs + '/' + wb.fs);
  const r = lay(Object.assign({}, base, { form: 'ring', inner: 110, trad: 128 })).wheels[0];
  ok(r.form === 'ring' && r.ri === 110 && r.lr === 128 && r.r === 150, 'a ring has its outer and inner radius, and its figures sit where the author puts them');
  const k0 = a.dialHoles.find((h) => h.kind === 'rect'), k1 = lay(base, { dx: 12, dy: -5, rot: 20 }).dialHoles.find((h) => h.kind === 'rect');
  ok(near(k1.x - k0.x, 12) && near(k1.y - k0.y, -5) && near(k1.rot, 20 * Math.PI / 180), 'a cutout moves and turns off the figures by its own offset');
  ok(near(k0.x, 233 + 128) || near(k0.x, 233 + wa.lr), 'and stands on them, at the wheel\u2019s reading point, until moved');
  ok(face([Object.assign({}, base, { fpx: 5 })]).lay.wheels[0].fs === E.LEGIBLE_PX, 'no figure is set below what reads at 2-3 ft (' + E.LEGIBLE_PX + ' px)');
  const bar = face([{ id: 'b', type: 'wheel', host: 'dial', angle: 90, dist: 120, size: 22, form: 'barrel', shows: 'date', digit: 'units' }, { id: 'bk', type: 'cutout', host: 'b' }]).lay;
  ok(bar.wheels[0].form === 'barrel' && near(bar.dialHoles[0].x, bar.wheels[0].x) && near(bar.dialHoles[0].y, bar.wheels[0].y), 'a barrel reads at its front, where its cutout stands');
  ok(face([Object.assign({}, base, { form: 'barrel' })]).comps(new Date(2026, 10, 2), null).dial, 'and draws');
  const mig = E.canonDesign({ comps: [{ id: 'w', type: 'wheel', size: 165, shows: 'date', tsize: 0.52 }] }).comps[0];
  ok(mig.fpx >= E.LEGIBLE_PX && !('tsize' in mig), 'an old relative figure size becomes px, raised to the legible floor', JSON.stringify(mig));
  const rb = B.wheelPlan3([{ id: 'b', type: 'wheel', form: 'barrel', shows: 'date', cadence: 'smooth' }]);
  ok(!rb.rows.length && /barrel/.test(rb.refuses[0]), 'a rolling barrel is refused by name; a snapping one plates', rb.refuses[0]);
}
// ---- Layers: a disc to another face, a cutout to another wheel ----------------------------------
{
  const comps = [{ id: 's', type: 'subdial', host: 'dial', angle: 180, dist: 100, size: 50 },
    { id: 'w', type: 'wheel', host: 'dial', angle: 180, dist: 120, shows: 'date' }, { id: 'k', type: 'cutout', host: 'w', at: 90 },
    { id: 'v', type: 'wheel', host: 'dial', angle: 0, dist: 90, shows: 'day' }];
  const r = S.place({ comps, hands: [], order: [], E }, 'comp:w', 'comp:s', 'below');
  const w = (r.comps || []).find((c) => c.id === 'w');
  ok(r.ok && w.host === 's' && w.dist === 20 && w.angle === 180, 'a wheel dragged onto a sub-dial goes under it, where it stood on the dial', JSON.stringify(r));
  const q = S.place({ comps, hands: [], order: [], E }, 'comp:k', 'comp:v', 'below');
  ok(q.ok && q.comps.find((c) => c.id === 'k').host === 'v', 'a cutout dragged onto another wheel reveals that wheel', JSON.stringify(q));
  const z = S.place({ comps, hands: [], order: [], E }, 'comp:k', 'comp:s', 'below');
  ok(!z.ok && /reveals a wheel/.test(z.reason), 'and onto anything but a wheel, says why', z.reason);
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
// ---- the bake: a turning row per wheel, one cover over them -----------------------------------
{
  const P = B.wheelPlan3([{ id: 'w', type: 'wheel', shows: 'day', ease: 200 }, { id: 'k', type: 'cutout', host: 'w' }, { id: 'j', type: 'wheel', shows: 'date' },
    { id: 'u', type: 'wheel', shows: 'date', digit: 'units', cadence: 'smooth' }, { id: 'y', type: 'wheel', shows: 'year', digit: 'thousands', ease: 300 },
    { id: 'm', type: 'wheel', shows: 'moon', dir: 'ccw', cadence: 'smooth' }, { id: 's', type: 'wheel', shows: 'second', ease: 200 }]);
  const by = (id) => P.rows.find((r) => r.id === id);
  ok(P.rows.length === 5 && !P.rows.some((r) => r.id === 'j') && !P.refuses.length, 'every wheel that moves between figures (smooth, eased, a year) is one row; its cutouts are none', JSON.stringify(P.rows.map((r) => r.id)));
  const cp = B.compPlan3([{ id: 'j', type: 'wheel', shows: 'date', digit: 'units' }, { id: 'k', type: 'cutout', host: 'j' }, { id: 'v', type: 'wheel', shows: 'moon' }, { id: 'e', type: 'wheel', shows: 'day', ease: 200 }, { id: 'y', type: 'wheel', shows: 'year', digit: 'units' }]);
  ok(cp.rows.length === 2 && cp.rows[0].src === 'date' && cp.rows[0].count === 31 && cp.rows[1].src === 'moon_age' && cp.rows[1].count === 30 && !cp.refuses.length,
    'a wheel that snaps is a window\u2019s plates, one per value (a digit ring over the whole domain); an eased one and a year are not', JSON.stringify(cp.rows.map((r) => r.id + '/' + r.src + '/' + r.count)));
  ok(by('w').src === 'weekday' && by('w').vmin === 0 && by('w').vmax === 7 && by('w').span === -3600 && by('w').zero === Math.round(-3600 / 7) && by('w').cadence === 'jump' && by('w').ease === 200,
    'a day ring turns back a figure a day, its Monday on the second figure of a Sunday-first ring', JSON.stringify(by('w')));
  ok(by('u').digit === 'units' && by('u').vmin === 0 && by('u').vmax === 10 && by('u').cadence === 'roll', 'a smooth units ring rolls over ten figures');
  ok(by('y').src === 'year' && by('y').digit === 'thousands' && by('y').vmax === 10 && by('y').ease === 300, 'a year ring reads its thousands, and eases its jump');
  ok(by('m').src === 'moon_age' && by('m').vmax === 60 && by('m').span === -3600 && by('m').cadence === 'roll', 'a moon disc is sixty day-places a turn, turning the way its direction says');
  ok(by('s').ease === 0, 'a seconds ring never eases: a settle every second is refused by the dial');
  const bad = B.wheelPlan3([{ id: 'a', type: 'wheel', shows: 'day', digit: 'units', cadence: 'smooth' }, { id: 'b', type: 'wheel', shows: 'date', digit: 'hundreds', cadence: 'smooth' }]);
  ok(!E.canonDesign({ comps: [{ id: 'a', type: 'wheel', shows: 'day', digit: 'units' }] }).comps[0].digit, 'a day of the week keeps no digit');
  ok(bad.refuses.length === 2 && /day of the week/.test(bad.refuses[0]) && /never reaches/.test(bad.refuses[1]), 'a digit a measure does not have is refused by name', bad.refuses.join(' | '));
  // the editor's ring and the dial's row count the same figures
  const combos = []; Object.keys(E.WHEEL_SHOWS).forEach((sh) => ['whole', 'units', 'tens', 'hundreds', 'thousands'].forEach((dg) => combos.push([sh, dg])));
  const odd = combos.filter(([sh, dg]) => {
    const pr = B.wheelPlan3([{ id: 'x', type: 'wheel', shows: sh, digit: dg, cadence: 'smooth' }]); if (pr.refuses.length) return false;
    const sp = E.wheelSpec(sh, dg); return !sp || sp.n !== pr.rows[0].n;
  });
  ok(!odd.length, 'every wheel the dial can turn carries the editor\u2019s figure count', JSON.stringify(odd));
}
// ---- the bake writes them as the dial reads them -------------------------------------------------
{
  const pl = (w, h, a) => { const r = new Uint8Array(w * h * 4); for (let i = 0; i < w * h; i++) { r[i * 4] = 9; r[i * 4 + 3] = a; } return r; };
  const P = B.wheelPlan3([{ id: 't', type: 'wheel', shows: 'date', digit: 'tens', cadence: 'smooth' }, { id: 'u', type: 'wheel', shows: 'date', digit: 'units', ease: 250 }]);
  const wheels = P.rows.map((r, i) => Object.assign({}, r, { x: 100 + i, y: 100, w: 60, h: 60, pivot_x: 30, pivot_y: 30, rgba: pl(60, 60, 255) }));
  const bk = B.bakeClock3({ name: 'Wheels', plate: { rgba: pl(466, 466, 255), w: 466, h: 466 }, wheels, cover: { x: 90, y: 90, w: 80, h: 80, rgba: pl(80, 80, 200) } });
  const el = B.verifyBundle3(bk.bytes).sections[0].blob.elements.filter((e) => e.class === B.classId('aux'));
  ok(el.length === 3 && el.every((e) => e.order === 0) && el[0].cadence === 6 && el[0].digit === 2 && el[0].vmax === 4 && el[1].cadence === 3 && el[1].ease === 250 && el[1].digit === 1
     && el[2].src === 0 && !el[2].cadence, 'two discs and the cover land at depth 0, discs first: a rolling tens ring, an easing units ring, a still cover', JSON.stringify(el.map((e) => [e.slot, e.order, e.src, e.digit, e.cadence, e.vmax, e.span, e.ease])));
  let threw = ''; try { B.bakeClock3({ name: 'x', plate: { rgba: pl(466, 466, 255), w: 466, h: 466 }, wheels }); } catch (e) { threw = e.message; }
  ok(/no cover/.test(threw), 'wheels with no cover are refused: every disc would show whole over the face', threw);
  const many = Array.from({ length: 16 }, (_, i) => Object.assign({}, wheels[1], { id: 'w' + i }));
  const past = B.bakeClock3({ name: 'x', plate: { rgba: pl(466, 466, 255), w: 466, h: 466 }, wheels: many, cover: { x: 90, y: 90, w: 80, h: 80, rgba: pl(80, 80, 200) } }).meter;
  ok(past.layers === 17 && past.layerCap === 16 && bk.meter.layers === 3, 'the meter counts the layers the stage seats (17 past its 16, where the dial falls back the slow way)', JSON.stringify([past.layers, bk.meter.layers]));
}
// ---- the roll, as the dial's CLK_CAD_ROLL computes it (host_drive_test.py's vectors) -----------
{
  const t = E.wheelSpec('date', 'tens'), u = E.wheelSpec('date', 'units'), w = E.wheelSpec('date');
  ok(near(t.at(new Date(2026, 9, 15, 18)), 1) && near(t.at(new Date(2026, 9, 19, 18)), 1.75) && near(t.at(new Date(2026, 9, 31, 12)), 3.5) && near(t.at(new Date(2026, 10, 30, 12)), 3.5),
    'a tens ring holds, and rolls only through the day before it changes (the 31st, and a 30-day month\u2019s 30th)');
  ok(near(u.at(new Date(2026, 1, 28, 12)), 9.5) && near(w.at(new Date(2026, 1, 28, 18)), 30), 'the 28th of a February rolls on to the 1st: three units figures, four on the whole ring');
  const y = E.wheelSpec('year', 'thousands'), mo = E.wheelSpec('month', 'units'), h = E.wheelSpec('hour', 'tens');
  ok(y.held(new Date(2026, 10, 2)) === 2 && E.wheelSpec('year', 'units').held(new Date(2026, 10, 2)) === 6 && mo.held(new Date(2026, 10, 2)) === 1 && h.held(new Date(2026, 10, 2, 12, 5)) === 1,
    'a digit reads the number shown: 2026\u2019s 2 and 6, November\u2019s 1, noon\u2019s 12');
}
// ---- the moon wheel -----------------------------------------------------------------------------------
{
  const f = face([{ id: 'm', type: 'wheel', host: 'dial', angle: 180, dist: 110, size: 34, shows: 'moon', cadence: 'smooth' }, { id: 'mc', type: 'cutout', host: 'm', at: 0, shape: 'moon' }]);
  const w = f.lay.wheels[0], hole = f.lay.dialHoles.find((q) => q.kind === 'moon');
  ok(w.spec.moon && hole && near(hole.x, w.x) && near(hole.y, w.y) && hole.a === w.r && hole.seal, 'a moon cutout is the moonphase aperture over its wheel\u2019s own centre, sealed on the dial');
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
  ok(['Shows', 'Digit', 'Form', 'Outer diameter', 'Figures at radius', 'Reads at', 'Edge curvature', 'Figure size', 'Typeface', 'Style', 'Figures face', 'Figure turn', 'Disc colour', 'Direction', 'Motion', 'Ease'].every(lab) && !lab('Rotation') && !lab('Radius'),
    'a wheel’s panel: what it shows, its form and sizes, where its figures sit and read, their size, type, face and turn, its edge, and its motion', fl.map((f) => f.label).join('|'));
  ok(/px · \d+\.\d mm/.test(lab('Outer diameter').valText || ''), 'its sizes read in px and in mm on the 1.75 in dial', lab('Outer diameter').valText);
  it.setState({ sel: ks[1].id, selKind: 'comp' });
  const fk = it.renderVals().fields, lk = (l) => fk.find((x) => x.label === l);
  ok(lk('Wheel') && lk('Along the figures at') && lk('Move across') && lk('Move down') && lk('Turn') && lk('Align') && lk('Shape') && lk('Join with') && !lk('Host') && !lk('Angle') && !lk('Distance'),
    'a cutout’s panel: its wheel, where along the figures, moved and turned freely, aligned back in one press; its shape and join', fk.map((f) => f.label).join('|'));
  it.setComp(ks[1].id, { dx: 9, rot: 12 }); const al = it.renderVals().fields.find((x) => x.label === 'Align'); if (al) al.opts[0].pick();
  const k2 = it.design().comps.find((c) => c.id === ks[1].id);
  ok(!k2.dx && !k2.rot, 'Align to the figures puts a moved cutout back on them', JSON.stringify(k2));
  ok(it.renderVals().catOpts.some((c) => /\+ Wheel/.test(c.label)) && it.renderVals().catOpts.some((c) => /\+ Cutout/.test(c.label)), 'the catalogue offers a wheel and a cutout');
}

console.log(fails ? 'RESULT: FAIL — ' + fails + ' of ' + checked + ' checks' : 'all green (' + checked + ' checks)');
process.exit(fails ? 1 : 0);
