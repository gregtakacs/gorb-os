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
const holePts = (h) => E._holePts(h);
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
// ---- P5d.3d: the window retired for the wheel; what is rendered is named so ---------------------
{
  // the conversions from before the design format (windows and classic moons into wheels) are gone (2026-11-02)
  const c = E.canonDesign({ comps: [{ id: 't', type: 'window', host: 'dial', angle: 0, dist: 120, size: 1, shows: 'custom', fmt: 'ddd DD' },
    { id: 'r', type: 'moon', host: 'dial', angle: 0, dist: 60, size: 40, style: 'sphere' }] }).comps;
  const by = (id) => c.find((x) => x.id === id);
  ok(by('t').type === 'window' && by('r').type === 'moon', 'a line of rendered text and a rendered moon stay what they are');
  ok(!('Window' in {}) && E.COMP_TYPES.window === 'Rendered display \u00b7 text' && E.COMP_TYPES.moon === 'Rendered display \u00b7 moon', 'the catalogue names the rendered parts for what they are');
  const mw = face([{ id: 'm', type: 'wheel', host: 'dial', size: 40, shows: 'moon', msize: 0.4, mpos: 0.45 }, { id: 'k', type: 'cutout', host: 'm', shape: 'moon', ap: 0.875 }]).lay;
  ok(mw.wheels[0].msize === 0.4 && mw.wheels[0].mpos === 0.45 && near(mw.dialHoles[0].a, 40), 'a moon wheel sizes its disc, its moons and their place; its aperture is the disc, locked');
}
// ---- the stage's eye, part by part -----------------------------------------------------------------
{
  const DCLogic = function () { };
  DCLogic.prototype.setState = function (p) { Object.assign(this.state, typeof p === 'function' ? p(this.state) : p); };
  const it = new (global.COMP_CLASS(DCLogic))({});
  it.ready = true; it.rendering = false; it.budget = () => ({ fps: 8, dev: { pace: 8, cross: 0, cov: 0, why: '' } });
  it.state.dial = 'skeleton'; it.state.playing = false; it.state.hidden = {};
  it.state.designs = { skeleton: E.canonDesign({ print: 'roman', comps: [{ id: 'w', type: 'wheel', host: 'dial', size: 150, shows: 'date' }, { id: 'k', type: 'cutout', host: 'w' }, { id: 'h', type: 'hand', host: 'dial', shows: 'hour' }] }) };
  const rows = () => it.renderVals().stackRows || it.renderVals().layers || [];
  const row = (id) => rows().find((r) => r.id === id);
  if (row('comp:w')) {
    row('comp:w').eyeToggle({ stopPropagation() { } });
    ok(it.state.hidden.ids && it.state.hidden.ids.w === 1 && !it.state.hidden.complications, 'a wheel\u2019s eye hides that wheel alone, not every disc');
    row('hand:h').eyeToggle({ stopPropagation() { } });
    ok(it.state.hidden.ids.h === 1 && !it.state.hidden.hands, 'and a hand\u2019s that hand alone');
  } else ok(false, 'the layers list carries the wheel row', Object.keys(it.renderVals()).join(','));
}
// ---- P5d.3e: every cutout backed by its wheel; snapping wheels read through their own cutouts ----------
{
  const backed = (w, h) => holePts(h).every(([x, y]) => { const d = Math.hypot(x - w.x, y - w.y); return d <= w.r + 0.01 && d >= (w.form === 'ring' ? w.ri : 0) - 0.01; });
  const odd = [];
  Object.keys(E.PRESETS).forEach((k) => {
    const P = E.PRESETS[k]; if (!P.design.comps.some((c) => c.type === 'wheel')) return;
    const f = face(JSON.parse(JSON.stringify(P.design.comps)));
    f.lay.wheels.forEach((w) => w.holes.forEach((h) => { if (h.kind !== 'moon' && !h.uncovered && !backed(w, h)) odd.push(k + '/' + w.id); if (h.uncovered) odd.push(k + '/' + w.id + ' flagged'); }));
    f.lay.wheels.filter((w) => w.form === 'ring').forEach((w) => { if (Math.hypot(w.x - 233, w.y - 233) > 0.5) odd.push(k + '/' + w.id + ' off-centre'); });
  });
  ok(!odd.length, 'every preset\u2019s wheels are concentric rings, each backing its cutouts all the way round', odd.join(' '));
  const un = face([{ id: 'w', type: 'wheel', host: 'dial', size: 120, form: 'ring', inner: 100, trad: 110, read: 90, shows: 'day' }, { id: 'k', type: 'cutout', host: 'w' }]).lay;
  ok(backed(un.wheels[0], un.dialHoles[0]) && !un.dialHoles[0].uncovered, 'a cutout the author has not sized shrinks onto a narrow ring');
  const au = face([{ id: 'w', type: 'wheel', host: 'dial', size: 120, form: 'ring', inner: 100, trad: 110, read: 90, shows: 'day' }, { id: 'k', type: 'cutout', host: 'w', w: 60, h: 40 }]).lay;
  ok(au.dialHoles[0].uncovered === true, 'one the author sized past the band is flagged, not changed');
  const two = face([{ id: 'a', type: 'wheel', host: 'dial', size: 117, form: 'ring', inner: 66, trad: 91, read: 270, shows: 'day' }, { id: 'ak', type: 'cutout', host: 'a', at: 270 },
    { id: 'b', type: 'wheel', host: 'dial', size: 117, form: 'ring', inner: 66, trad: 91, read: 90, shows: 'month' }, { id: 'bk', type: 'cutout', host: 'b', at: 90 }]);
  global.__TEXT_AUDIT = []; two.comps(new Date(2026, 10, 2), null); const txt = global.__TEXT_AUDIT.map((r) => r.text); global.__TEXT_AUDIT = null;
  ok(txt.includes('MON') && txt.includes('NOV'), 'two rings at one radius each draw their own figures, each seen through its own window');
}
// ---- the preset cards: each watch as picking it would show it -------------------------------------
{
  const DCLogic = function () { };
  DCLogic.prototype.setState = function (p) { Object.assign(this.state, typeof p === 'function' ? p(this.state) : p); };
  const it = new (global.COMP_CLASS(DCLogic))({});
  it.ready = true; it.state.dial = 'opentour'; it.state.caseStyle = 'tachy'; it.state.train = 'skeltour'; it.state.cfg = { temp: 4000 };
  const t = it.thumbOpts('modern');
  ok(t.o.caseStyle === 'polished' && t.o.train === 'classic' && it.state.caseStyle === 'tachy' && it.state.dial === 'opentour', 'a card renders its own case and train, and leaves the selected watch alone', JSON.stringify([t.o.caseStyle, t.o.train]));
  ok(typeof E.prime === 'function', 'and the engine can be primed back to the stage after one');
}
// ---- P5d.3f: a date at 2 o'clock in a window tilted with the ring ---------------------------------
{
  const OUT = { face: 'ring', fang: 0 }, IN = { face: 'ring', fang: 180 };   // tops out, tops in
  const ring = (face2, cut) => face([Object.assign({ id: 'w', type: 'wheel', host: 'dial', size: 175, form: 'ring', inner: 130, trad: 152, read: 60, shows: 'date' }, typeof face2 === 'string' ? { face: face2 } : face2),
    Object.assign({ id: 'k', type: 'cutout', host: 'w' }, cut || {})]).lay.dialHoles.find((h) => h.kind === 'rect');
  const r60 = Math.PI / 3;
  ok(near(ring(OUT).rot, r60) && near(((ring(IN).rot % Math.PI) + Math.PI) % Math.PI, r60),   // a window turned half round is the same window
     'figures along the ring at 2 o\u2019clock: the window tilts with them, 60\u00b0, by itself');
  ok(near(ring('up').rot, 0), 'level figures, a level window');
  ok(near(ring('up', { lie: 'ring' }).rot, r60) && near(ring(OUT, { lie: 'level' }).rot, 0), 'and either can be told otherwise');
  const nw = face([Object.assign({ id: 'n', type: 'wheel', host: 'dial' }, E.NEW_COMP.wheel)]).lay.wheels[0], pitch = (2 * Math.PI * nw.lr) / nw.spec.n;
  ok(nw.fs >= E.LEGIBLE_PX && pitch >= nw.fs * (0.62 * 2 + 0.35) - 1e-6, 'a new wheel\u2019s figures fit round it without touching, at a legible size', nw.fs.toFixed(1) + ' px, pitch ' + pitch.toFixed(1));
  ok(near(ring(OUT, { rot: 5 }).rot, r60 + Math.PI / 36), 'Turn adds to how it lies');
  const h = ring(OUT), d = Math.hypot(h.x - 233, h.y - 233);
  ok(near(d, 152) && near(Math.atan2(h.x - 233, 233 - h.y), r60), 'and it stands on the figures where the wheel reads');
}
// ---- P5d.3g: figures at any angle to the ring, snapping to level, tops out/in and the tangents --------
{
  const L = (w, k) => face([Object.assign({ id: 'w', type: 'wheel', host: 'dial', size: 175, form: 'ring', inner: 130, trad: 152, read: 60, shows: 'date' }, w), Object.assign({ id: 'k', type: 'cutout', host: 'w' }, k || {})]).lay;
  const d2r = Math.PI / 180, rect = (l) => l.dialHoles.find((h) => h.kind === 'rect');
  ok(near(rect(L({ face: 'ring', fang: 90 })).rot, 150 * d2r) && near(rect(L({ face: 'ring', fang: 37 })).rot, 97 * d2r), 'any angle to the ring, the tangent too: the window lies with the figure where it reads (60 + 90, 60 + 37)');
  ok(near(rect(L({ face: 'ring', fang: -90 })).rot, -30 * d2r), 'and the other tangent');
  const cn = E.canonDesign({ comps: [{ id: 'a', type: 'wheel', shows: 'date', face: 'ring', fang: 270, trot: 10 }, { id: 'b', type: 'wheel', shows: 'date', face: 'ring', fang: 0 }, { id: 'c', type: 'wheel', shows: 'date', face: 'sideways' }] }).comps;
  ok(cn[0].face === 'ring' && cn[0].fang === -80 && !('trot' in cn[0]) && cn[1].face === 'ring' && !('fang' in cn[1]) && !('face' in cn[2]), 'one angle to the ring, a turn folded into it; a face this editor does not know reads level', JSON.stringify(cn));
  const DCLogic = function () { };
  DCLogic.prototype.setState = function (p) { Object.assign(this.state, typeof p === 'function' ? p(this.state) : p); };
  const it = new (global.COMP_CLASS(DCLogic))({});
  it.ready = true; it.rendering = false; it.budget = () => ({ fps: 8, dev: { pace: 8, cross: 0, cov: 0, why: '' } });
  it.state.dial = 'skeleton'; it.state.playing = false; it.state.hidden = {};
  it.state.designs = { skeleton: E.canonDesign({ print: 'roman', comps: [{ id: 'w', type: 'wheel', host: 'dial', size: 175, form: 'ring', inner: 130, trad: 152, read: 90, shows: 'date' }] }) };
  it.setState({ sel: 'w', selKind: 'comp', step: 'parts', tab: 'comps' });
  const fld = (l) => it.renderVals().fields.find((x) => x.label === l), wc = () => it.design().comps.find((c) => c.id === 'w');
  fld('Figures face').on({ target: { value: 62 } });
  ok(wc().face === 'ring' && wc().fang === 62, 'the slider sets any angle', JSON.stringify(wc()));
  fld('Figures face').on({ target: { value: 87 } });
  ok(wc().fang === 90, 'and snaps within 4\u00b0 to the tangent', JSON.stringify(wc()));
  fld('Figures face').on({ target: { value: -88 } });
  ok(!wc().face && !('fang' in wc()), 'and to level where it reads (here at -90, the reading point at 90)', JSON.stringify(wc()));
  fld('Snap to').opts.find((o) => o.label === 'Tops in').pick();
  ok(wc().face === 'ring' && wc().fang === 180, 'the chips jump straight to each', JSON.stringify(wc()));
  fld('Snap to').opts.find((o) => o.label === 'Level').pick(); it.setComp('w', { read: 30 });
  ok(!wc().face && wc().read === 30, 'and level stays level when the reading point moves');
}
// ---- P5d.3h: the moon cutout locked to its disc; a rendered moon turns; Layers depth and eyes ----------
{
  const mv = (w, k) => face([Object.assign({ id: 'm', type: 'wheel', host: 'dial', angle: 180, dist: 100, size: 40, shows: 'moon' }, w), Object.assign({ id: 'k', type: 'cutout', host: 'm', shape: 'moon' }, k || {})]).lay;
  const a = mv({}, { dx: 30, dy: -20, at: 77, rot: 40 }), w0 = a.wheels[0], h0 = a.dialHoles[0];
  ok(near(h0.x, w0.x) && near(h0.y, w0.y) && near(h0.rot, w0.read), 'a moon cutout sits on its disc\u2019s centre and opens where the wheel faces, whatever offsets it carries');
  const b = mv({ size: 60, read: 45 }), h1 = b.dialHoles[0];
  ok(near(h1.a, 60) && near(h1.rot, Math.PI / 4), 'and grows and turns with the moon wheel');
  const cm = E.canonDesign({ comps: [{ id: 'm', type: 'wheel', shows: 'moon' }, { id: 'k', type: 'cutout', host: 'm', shape: 'moon', dx: 3, at: 10, w: 50, rot: 9 }] }).comps[1];
  ok(['dx', 'at', 'w', 'rot'].every((k) => !(k in cm)), 'it keeps no placement of its own', JSON.stringify(cm));
  const sw = E.canonDesign({ comps: [{ id: 'm', type: 'wheel', shows: 'moon' }, { id: 'k', type: 'cutout', host: 'm', at: 90, ap: 0.7 }] }).comps[1];
  ok(sw.shape === 'moon' && !('ap' in sw), 'a wheel switched to the moon after its cutout came makes it a moon cutout, its aperture the disc', JSON.stringify(sw));
  const rm = face([{ id: 'r', type: 'moon', host: 'dial', angle: 0, dist: 80, size: 40, style: 'sphere', rot: 30 }]).lay.comps[0];
  ok(near(rm.rrot, Math.PI / 6) && E.canonDesign({ comps: [{ id: 'r', type: 'moon', style: 'sphere', rot: 30 }] }).comps[0].rot === 30, 'a rendered moon keeps its rotation and turns its terminator by it');
  const st = S.build({ comps: [{ id: 'w', type: 'wheel', host: 'dial', shows: 'date' }, { id: 'k', type: 'cutout', host: 'w' }, { id: 'r', type: 'moon', host: 'dial', style: 'sphere' }], hands: [], order: [], E });
  ok(st.find((x) => x.id === 'comp:w').parent === 'dial' && st.find((x) => x.id === 'comp:r').parent === 'dial' && st.find((x) => x.id === 'comp:k').parent === 'comp:w', 'Layers nests the dial\u2019s discs under the dial, and a cutout under its wheel');
  const DCLogic = function () { };
  DCLogic.prototype.setState = function (p) { Object.assign(this.state, typeof p === 'function' ? p(this.state) : p); };
  const it = new (global.COMP_CLASS(DCLogic))({});
  it.ready = true; it.rendering = false; it.budget = () => ({ fps: 8, dev: { pace: 8, cross: 0, cov: 0, why: '' } });
  it.state.dial = 'skeleton'; it.state.playing = false; it.state.hidden = {};
  it.state.designs = { skeleton: E.canonDesign({ print: 'roman', comps: [{ id: 'w', type: 'wheel', host: 'dial', size: 150, shows: 'date' }, { id: 'k', type: 'cutout', host: 'w' }, { id: 'v', type: 'wheel', host: 'dial', size: 80, shows: 'day' }, { id: 'vk', type: 'cutout', host: 'v' }] }) };
  const rows = () => it.renderVals().stackRows || it.renderVals().layers || [];
  const kr = rows().find((r) => r.id === 'comp:k');
  if (kr) { kr.eyeToggle({ stopPropagation() { } }); ok(it.state.hidden.ids && it.state.hidden.ids.k === 1 && !it.state.hidden.complications, 'a cutout\u2019s eye switches off that cutout alone'); }
  else ok(false, 'the cutout row is listed');
  ok(kr && rows().find((r) => r.id === 'comp:k').indent > rows().find((r) => r.id === 'comp:w').indent && rows().find((r) => r.id === 'comp:w').indent > 0, 'and a cutout indents under its wheel, the wheel under the dial');
}
// ---- P5d.3j: plate relief and cast shadows are separate gates ---------------------------------------
{
  const hs = (o) => { const f = E.face(Object.assign({ design: E.canonDesign({ print: 'roman', comps: [{ id: 'h', type: 'hand', host: 'dial', shows: 'hour' }] }) }, O, o)); return f.handShadow(f.hands[0]).alpha; };
  ok(hs({ shRelief: 0 }) > 0 && hs({ shRelief: 0 }) === hs({}), 'relief off leaves the hands\u2019 cast shadows exactly as they were');
  ok(hs({ shMove: 0 }) === 0, 'and cast shadows off is their own switch');
}
// ---- P5d.3k: resets restore the stack; a disc dragged to another face lands there ---------------------
{
  const DCLogic = function () { };
  DCLogic.prototype.setState = function (p) { Object.assign(this.state, typeof p === 'function' ? p(this.state) : p); };
  const it = new (global.COMP_CLASS(DCLogic))({});
  it.ready = true; it.rendering = false; it.budget = () => ({ fps: 8, dev: { pace: 8, cross: 0, cov: 0, why: '' } });
  it.state.dial = 'modern'; it.state.playing = false; it.state.hidden = {};
  const rows = () => (it.renderVals().stackRows || []).filter((r) => r.isRow).map((r) => r.id);
  const fresh = rows();
  it.stackMove('hand:h4', 1); it.stackMove('comp:c3', -1);
  ok(JSON.stringify(rows()) !== JSON.stringify(fresh), 'a reorder changes the stack');
  it.state.tab = 'hands'; it.resetTab('hands');
  ok(JSON.stringify(rows()) === JSON.stringify(fresh), 'the Hands tab\u2019s Reset puts the hands\u2019 depth back too', rows().join(' '));
  it.stackMove('hand:h4', 1); it.state.hidden = { ids: { h2: 1 } };
  // D43 P6: Reset asks twice; the first press only arms it
  it.renderVals().resetDesign();
  ok(JSON.stringify(rows()) !== JSON.stringify(fresh) && /Press Reset again/.test(it.state.status) && it.renderVals().resetAllLabel === 'Press again to reset', 'the first Reset press only asks', it.state.status);
  it.renderVals().resetDesign();
  ok(JSON.stringify(rows()) === JSON.stringify(fresh) && !Object.keys(it.state.hidden.ids || {}).length, 'Reset everything restores the stack and the stage\u2019s eyes');
  const inp = it.stackInput(), pl = global.OrbStack.place(inp, 'comp:c2', 'comp:c3', 'below');
  it.stackApply(pl);
  const w = it.design().comps.find((c) => c.id === 'c2');
  ok(pl.ok && w.host === 'c3' && w.dist === pl.comps.find((c) => c.id === 'c2').dist, 'a wheel dragged onto a sub-dial lands under it, at the place Layers worked out', JSON.stringify([pl.ok, w.host, w.angle, w.dist]));
}
// ---- P5d.3l: each seconds hand its own rhythm, set where the dial's is; Reset <preset> in the title ----
{
  const DCLogic = function () { };
  DCLogic.prototype.setState = function (p) { Object.assign(this.state, typeof p === 'function' ? p(this.state) : p); };
  const it = new (global.COMP_CLASS(DCLogic))({});
  it.ready = true; it.rendering = false; it.budget = () => ({ fps: 8, dev: { pace: 8, cross: 0, cov: 0, why: '', need: 8 } });
  it.state.dial = 'modern'; it.state.playing = false; it.state.hidden = {};
  it.setState({ sel: null, selKind: null, step: 'hands', tab: 'hands' });
  const v0 = it.renderVals();
  ok(!v0.isEdited && v0.resetPresetLabel === 'Reset Modern', 'the title bar names the preset it resets to, once edited');
  const fl = () => it.renderVals().fields, row = (re) => fl().find((f) => re.test(f.label || ''));
  const cen = row(/· centre/), sub = row(/ on /);
  ok(!!row(/^Each seconds hand$/) && cen && sub, 'with two seconds hands, each gets its own rhythm under the dial\u2019s', fl().map((f) => f.label).join('|'));
  cen.opts.find((o) => o.label === 'Sweep').pick(); row(/ on /).opts.find((o) => o.label === '1/s').pick();
  const h = (id) => it.design().comps.find((c) => c.id === id).motion || {};
  ok(h('h4').cadence === 'sweep' && h('h1').cadence === 'jump', 'one sweeps while the other ticks', JSON.stringify([h('h4'), h('h1')]));
  const pr = it.pointerRows3(it.design(), B.compPlan3(it.design().comps)).rows.find((r) => r.id === 'h1');
  ok(pr && pr.cadence === 3, 'and the sub-dial\u2019s hand ships ticking (a jump row) beside the sweeping centre hand', JSON.stringify(pr && pr.cadence));
  ok(it.renderVals().isEdited, 'which makes it an edited Modern, with its reset in the title bar');
}
// ---- P5d.3m: exposed rings, the wheel's look, the cutout pane, sliders ------------------------------------
{
  const cw = E.canonDesign({ comps: [{ id: 'w', type: 'wheel', shows: 'date', exposed: true, ticks: 'all', rim: 'double', wfinish: 'sunray', every: 5, accent: '#ff0000' },
    { id: 'v', type: 'wheel', shows: 'date', exposed: false, ticks: 'none', rim: 'none', wfinish: 'flat', every: 1, accent: 'auto' }] }).comps;
  ok(cw[0].exposed === true && cw[0].ticks === 'all' && cw[0].rim === 'double' && cw[0].wfinish === 'sunray' && cw[0].every === 5 && cw[0].accent === '#ff0000', 'the wheel\u2019s look is kept', JSON.stringify(cw[0]));
  ok(['exposed', 'ticks', 'rim', 'wfinish', 'every', 'accent'].every((k) => !(k in cw[1])), 'and its defaults drop out', JSON.stringify(cw[1]));
  const ex = face([{ id: 'w', type: 'wheel', host: 'dial', size: 150, form: 'ring', inner: 110, shows: 'minute', exposed: true, every: 5, ticks: 'all' }]);
  ok(ex.lay.wheels[0].exposed && ex.lay.wheels[0].every === 5 && ex.comps(new Date(2026, 10, 2), null).tops.dial, 'an exposed ring needs no cutout, and draws over its face');
  const P = B.wheelPlan3([{ id: 'w', type: 'wheel', shows: 'minute', exposed: true }]);
  ok(P.rows.length === 1 && P.rows[0].exposed && P.rows[0].cadence === 'jump', 'and turns as a row (a plate per value would be the whole ring)');
  const pl = (w, h, a) => { const r = new Uint8Array(w * h * 4); for (let i = 0; i < w * h; i++) { r[i * 4] = 9; r[i * 4 + 3] = a; } return r; };
  const disc = Object.assign({}, P.rows[0], { x: 30, y: 30, w: 60, h: 60, pivot_x: 30, pivot_y: 30, rgba: pl(60, 60, 255) });
  let thr = ''; try { B.bakeClock3({ name: 'x', plate: { rgba: pl(466, 466, 255), w: 466, h: 466 }, wheels: [disc] }); } catch (e) { thr = e.message; }
  ok(!thr, 'with no cover, since nothing covers it', thr);
  thr = ''; try { B.bakeClock3({ name: 'x', plate: { rgba: pl(466, 466, 255), w: 466, h: 466 }, wheels: [Object.assign({}, disc, { exposed: false })] }); } catch (e) { thr = e.message; }
  ok(/no cover/.test(thr), 'while a covered one still needs its cover');
  // P2 (D43 §5.3, ruling 3): a turning wheel row dithers when it asks; auto asks for a gradient face
  const wd = (o) => B.wheelDither3(Object.assign({ type: 'wheel', shows: 'minute' }, o));
  ok(!wd({}) && wd({ wfinish: 'gradient' }) && wd({ wfinish: 'sunray' }) && wd({ shows: 'moon' }) && wd({ dither: 'on' }) && !wd({ wfinish: 'sunray', dither: 'off' }),
    'auto dithers a gradient or sunray face and the moon, on and off are the author\u2019s');
  ok(B.wheelPlan3([{ id: 'w', type: 'wheel', shows: 'minute', exposed: true, wfinish: 'sunray' }]).rows[0].dither === true, 'and the wheel row carries the verdict to the bake');
  const pick = (dither) => { const b = B.bakeClock3({ name: 'x', plate: { rgba: pl(466, 466, 255), w: 466, h: 466 }, wheels: [Object.assign({}, disc, { dither })] }); return Array.from(b.bytes).join(','); };
  ok(pick(true) !== pick(false), 'a dithered wheel row ships other texels than a plain one (9 is between two 565 levels)');
  ok(E.canonDesign({ comps: [{ id: 'w', type: 'wheel', shows: 'date', dither: 'auto' }, { id: 'v', type: 'wheel', shows: 'date', dither: 'on' }] }).comps.map((c) => c.dither).join() === ',on', 'canon keeps on/off and drops auto');
  ok(E.canonDesign({ comps: [{ id: 'w', type: 'wheel', shows: 'date' }, { id: 'k', type: 'cutout', host: 'w', shape: 'moon' }] }).comps[1].shape === undefined, 'a moon shape on a wheel that is not the moon falls back to a window');
  const DCLogic = function () { };
  DCLogic.prototype.setState = function (p2) { Object.assign(this.state, typeof p2 === 'function' ? p2(this.state) : p2); };
  const it = new (global.COMP_CLASS(DCLogic))({});
  it.ready = true; it.rendering = false; it.budget = () => ({ fps: 8, dev: { pace: 8, cross: 0, cov: 0, why: '' } });
  it.state.dial = 'skeleton'; it.state.playing = false; it.state.hidden = {};
  it.state.designs = { skeleton: E.canonDesign({ print: 'roman', comps: [{ id: 'w', type: 'wheel', host: 'dial', size: 150, form: 'ring', inner: 110, shows: 'date' }, { id: 'k', type: 'cutout', host: 'w', shape: 'arc' }] }) };
  it.setState({ sel: 'k', selKind: 'comp', step: 'parts', tab: 'comps' });
  const fk = it.renderVals().fields, lk = (l) => fk.find((f) => f.label === l);
  ok(!lk('Shape').opts.some((o) => /Moon/.test(o.label)) && lk('Arc') && !lk('Width') && lk('Height'), 'the cutout pane never offers Moon, and an arc has no Width', lk('Shape').opts.map((o) => o.label).join(','));
  ok(fk.some((f) => f.isNote && /Backed by its wheel|runs past/.test(f.note || '')), 'its coverage line is always there, so a warning cannot move the slider under the pointer');
  const h = lk('Height'); h.on({ target: { value: 0.4 } }); const h0 = it.design().comps.find((c) => c.id === 'k').h;
  h.onNum({ target: { value: '17.5' } }); const h1 = it.design().comps.find((c) => c.id === 'k').h;
  ok(!h0 && h1 === 17.5, 'a slider snaps to 0 near it, and its box takes an exact value', JSON.stringify([h0, h1]));
}
// ---- P5d.3n: the index part, the markers, a band-fitting figure ----------------------------------
{
  const base = { id: 'w', type: 'wheel', host: 'dial', size: 150, form: 'ring', inner: 110, shows: 'hour', exposed: true, read: 90 };
  const f1 = face([base, { id: 'v', type: 'wheel', host: 'dial', size: 190, form: 'ring', inner: 155, shows: 'minute', exposed: true, read: 90 }, { id: 'x', type: 'index', host: 'w', istyle: 'frame', over: 0 }]);
  const ix = f1.lay.indexes[0];
  ok(ix && ix.style === 'frame' && ix.r0 === 110 && ix.r1 === 190 && ix.glow === 0, 'an index spans every exposed ring read at its point, edge to edge with no overrun', JSON.stringify(ix && [ix.r0, ix.r1, ix.glow]));
  const f2 = face([base, { id: 'v', type: 'wheel', host: 'dial', size: 190, form: 'ring', inner: 155, shows: 'minute', exposed: true, read: 90 }, { id: 'x', type: 'index', host: 'w', span: 'own' }]);
  ok(f2.lay.indexes[0].r1 === 154, 'or only its own wheel', String(f2.lay.indexes[0].r1));
  // the bake: an index is crystal paint, so it plans no row and is not live (Orbit failed to bake before this was said)
  const ip = B.compPlan3(E.PRESETS.orbit.design.comps);
  ok(!ip.refuses.length && !ip.rows.some((r) => r.id === 'x1') && !B.liveComps3(E.PRESETS.orbit.design.comps).some((c) => c.type === 'index'),
    'an index plans no comp row and is not live, and Orbit\u2019s plan refuses nothing', JSON.stringify(ip.refuses));
  // and every preset passes the bake's planners: the gate that would have caught Orbit
  const planRefuses = Object.keys(E.PRESETS).map((k) => { const cs = E.canonDesign(JSON.parse(JSON.stringify(E.PRESETS[k].design))).comps || [];
    const r = B.compPlan3(cs).refuses.concat(B.wheelPlan3(cs).refuses); return r.length ? k + ': ' + r[0] : null; }).filter(Boolean);
  ok(!planRefuses.length, 'every preset plans its comps and wheels without a refusal', planRefuses.join(' | '));
  const ci = E.canonDesign({ comps: [{ id: 'w', type: 'wheel', shows: 'hour' }, { id: 'x', type: 'index', host: 'w', istyle: 'line', span: 'all', thick: 2, over: 4, glow: 0, iw: 9, color: 'auto', angle: 5 }] }).comps[1];
  ok(JSON.stringify(ci) === JSON.stringify({ id: 'x', type: 'index', host: 'w', istyle: 'line' }), 'an index keeps no default (a line has no width, and no glow by default)', JSON.stringify(ci));
  // a level two-digit figure read at 3 o'clock lays its width across a 40 px band: the auto size fits it
  const fw = face([base]).lay.wheels[0];
  ok(fw.fs * (0.62 * 2 + 0.1) <= 40 - 4 + 1e-6 && fw.fe * 2 <= 36 + 1e-6, 'an auto-sized ring figure fits across its band as well as round it', fw.fs.toFixed(1));
  ok(face([Object.assign({}, base, { face: 'ring', fang: 0 })]).lay.wheels[0].fs > fw.fs, 'and one tangent to the ring, standing its height across, may be larger');
  // ticks fit to the room the figures leave, from either edge; between figures sit half a step on
  const tw = (o) => E.wheelTicks(face([Object.assign({}, base, { ticks: 'all', trad: 120 }, o)]).lay.wheels[0]);
  const tOut = tw({}), tIn = tw({ tpos: 'inner' }), tBoth = tw({ tpos: 'both' }), tBtw = tw({ ticks: 'between' }), tMix = tw({ ticks: 'both' });
  ok(tOut.length === 12 && tOut[0].spans.length === 1 && tOut[0].spans[0][0] === 148.5, 'outer ticks start a margin in from the rim', JSON.stringify(tOut[0].spans));
  ok(tIn[0].spans[0][0] === 111.5 && tBoth[0].spans.length === 2, 'inner ticks from the inner edge, and both edges draw two', JSON.stringify([tIn[0].spans, tBoth[0].spans]));
  ok(tBtw.length === 12 && tBtw[0].i === 0.5 && tBtw[0].major && tMix.length === 24 && !tMix[1].major, 'between figures half a step on; at-and-between makes the between ones minor');
  const near0 = tw({ trad: 140 });
  ok(near0.room < 3, 'figures moved out to the rim leave the outer ticks no room, which the panel warns of', near0.room.toFixed(1));
  const rims = (o) => E.wheelRims(face([Object.assign({}, base, { rim: 'double' }, o)]).lay.wheels[0]);
  ok(rims({}).length === 4 && rims({ rpos: 'outer' }).length === 2 && rims({ rpos: 'outer', rm: 3, rw: 2 })[0][0] === 150 - 3 - 1 && rims({ rpos: 'outer', rgap: 5 })[1][0] === rims({ rpos: 'outer' })[0][0] - 5, 'rim lines by edge, margin, thickness and spacing');
  const cm = E.canonDesign({ comps: [Object.assign({}, base, { ticks: 'all', tpos: 'outer', tlen: 0, tw: 1.6, tm: 1.5, rim: 'line', rpos: 'both', rw: 1.2, rm: 0.2, rgap: 4 })] }).comps[0];
  ok(['tpos', 'tlen', 'tw', 'tm', 'rpos', 'rw', 'rm', 'rgap'].every((k) => !(k in cm)), 'marker defaults drop out (and a single rim has no spacing)', JSON.stringify(cm));
  const cd = E.canonDesign({ comps: [{ id: 'w', type: 'wheel', shows: 'date', tpos: 'inner', tw: 3, rim: 'line', rpos: 'inner' }] }).comps[0];
  ok(!('tpos' in cd) && !('tw' in cd) && !('rpos' in cd), 'a disc has no inner edge, and no tick fields without ticks', JSON.stringify(cd));
  // the panel: a wheel offers to add its index, the index has its own pane, the markers theirs
  const DCLogic = function () { };
  DCLogic.prototype.setState = function (p2) { Object.assign(this.state, typeof p2 === 'function' ? p2(this.state) : p2); };
  const it = new (global.COMP_CLASS(DCLogic))({});
  it.ready = true; it.rendering = false; it.budget = () => ({ fps: 8, dev: { pace: 8, cross: 0, cov: 0, why: '' } });
  it.state.dial = 'skeleton'; it.state.playing = false; it.state.hidden = {};
  it.state.designs = { skeleton: E.canonDesign({ print: 'roman', comps: [Object.assign({}, base, { ticks: 'all', rim: 'double' })] }) };
  it.setState({ sel: 'w', selKind: 'comp', step: 'parts', tab: 'comps' });
  let fk = it.renderVals().fields; const lk = (l) => fk.find((f) => f.label === l);
  ok(lk('Ticks stand') && lk('Tick length') && lk('Tick thickness') && lk('Tick margin') && lk('Rim on') && lk('Rim thickness') && lk('Rim margin') && lk('Line spacing'), 'the Markers section places, sizes and spaces the ticks and rim lines', fk.map((f) => f.label).join('|'));
  ok(fk.some((f) => f.isNote && /Room for ticks|No room|run into/.test(f.note || '')), 'and says how much room the figures leave the ticks');
  ok(lk('Dither') && lk('Dither').opts.map((o) => o.label).join() === 'Auto \u00b7 off,On,Off', 'a turning flat wheel offers its dither, auto resolving off', lk('Dither') && lk('Dither').opts.map((o) => o.label).join());
  const add = lk('Index').opts.find((o) => /Add an index/.test(o.label)); add.pick();
  const nx = it.design().comps.find((c) => c.type === 'index');
  ok(nx && nx.host === 'w' && it.state.sel === nx.id, 'Add an index puts one on the selected wheel and selects it', JSON.stringify(nx));
  fk = it.renderVals().fields;
  ok(lk('Style') && lk('Spans') && lk('Colour') && lk('Thickness') && lk('Glow') && lk('Overrun') && !lk('Angle') && !lk('Distance') && fk.some((f) => f.isNote && /Reads at/.test(f.note || '')), 'the index pane: style, span, colour, thickness, glow, overrun, and where it stands', fk.map((f) => f.label).join('|'));
  lk('Style').opts.find((o) => o.label === 'Laser lens').pick(); fk = it.renderVals().fields;
  ok(lk('Width'), 'a lens (or frame) has a width');
  const items = global.OrbStack.build(it.stackInput());
  ok(items.some((q) => q.kind === 'index' && q.band === 'crystal' && q.ref === nx.id), 'and the index is a layer on the glass');
}
// ---- canon ----------------------------------------------------------------------------------
{
  const c = E.canonDesign({ comps: [{ id: 'w', type: 'wheel', host: 'dial', shows: 'nope', digit: 'whole', dir: 'cw', cadence: 'jump', ease: 0, overhang: 'shown', wbg: 'auto', blank0: true },
    { id: 'k', type: 'cutout', host: 'w', at: 90, angle: 3, dist: 4, shape: 'window', style: 'frame', span: 30, bar: true, w: 0, frameColor: 'gilt' }] }).comps;
  ok(c[0].shows === 'date' && ['digit', 'dir', 'cadence', 'ease', 'overhang', 'wbg', 'blank0'].every((k) => !(k in c[0])), 'a wheel keeps no default and no field its state cannot use', JSON.stringify(c[0]));
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
  ok(['Shows', 'Digit', 'Form', 'Outer diameter', 'Figures at radius', 'Reads at', 'Edge curvature', 'Figure size', 'Typeface', 'Style', 'Figures face', 'Snap to', 'Disc colour', 'Direction', 'Motion', 'Ease'].every(lab) && !lab('Rotation') && !lab('Radius'),
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
