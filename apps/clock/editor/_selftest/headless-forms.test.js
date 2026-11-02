#!/usr/bin/env node
// D43 P5d.2: face forms (round, fan, slider), direction, the hand's overrides, markers,
// Set from the art, and what the bake ships for each.
'use strict';
require('./headless-stub.js');
const E = global.WatchEngine;
global.React = { createRef: () => ({ current: null }) };
require('./component.js');
const B = require(require('./runtime-paths.js').BAKER);
let fails = 0, checked = 0;
const ok = (c, n, x) => { checked++; if (!c || process.env.VERBOSE) console.log((c ? 'PASS ' : 'FAIL ') + n + (x ? '  [' + x + ']' : '')); if (!c) fails++; };
const near = (a, b, e) => Math.abs(a - b) < (e || 0.05);
const deg = (a) => (((a * 180) / Math.PI) % 360 + 360) % 360;
const O = { finish: 'brass', train: 'classic', frames: 16, tick: 'sweep', beats: 1, hands: 'breguet', handFinish: 'blued' };
const face = (comps, extra) => E.face(Object.assign({ design: E.canonDesign(Object.assign({ print: 'roman', comps }, extra || {})) }, O));
const at = (s, ms) => new Date(2026, 2, 7, 10, 9, s, ms || 0);
const H = (f, id) => f.hands.find((h) => h.id === id);
const sub = (o) => Object.assign({ id: 's1', type: 'subdial', host: 'dial', angle: 180, dist: 100, size: 60, scale: 'seconds' }, o);

// ---- a fan face drives a retrograde sweep ----------------------------------------------
{
  const f = face([sub({ form: 'fan', fanStart: 240, fanSpan: 240 }), { id: 'h1', type: 'hand', host: 's1', shows: 'second' }]), h = H(f, 'h1');
  ok(near(deg(E.handAngle(h, at(0))), 240) && near(deg(E.handAngle(h, at(30))), 0) && near(deg(E.handAngle(h, at(59, 900))), 119.6, 0.3),
    'a hand on a fan sweeps from its start over its span and flies back', [0, 30].map((s) => deg(E.handAngle(h, at(s))).toFixed(1)).join(','));
  ok(h.motion && h.motion.path === 'retrograde' && h.motion.fromFace, 'its motion is the face’s, marked as such');
  const g = f.lay.subs.find((q) => q.id === 's1');
  ok(g.form === 'fan' && g.fan.start === 240 && g.fan.span === 240, 'the layout carries the fan’s geometry');
}
// ---- direction: the face's, overridden per hand -----------------------------------------
{
  const f = face([sub({ form: 'fan', fanStart: 240, fanSpan: 240, tdir: 'ccw' }), { id: 'a', type: 'hand', host: 's1', shows: 'second' }, { id: 'b', type: 'hand', host: 's1', shows: 'second', dir: 'cw' }]);
  ok(near(deg(E.handAngle(H(f, 'a'), at(30))), 120) && H(f, 'a').dir === 'ccw', 'an anticlockwise fan runs its hand anticlockwise');
  ok(near(deg(E.handAngle(H(f, 'b'), at(30))), 0) && H(f, 'b').dir === 'cw', 'and a second hand on the same scale may run the other way');
  const r = face([sub({ tdir: 'ccw' }), { id: 'a', type: 'hand', host: 's1', shows: 'second' }, { id: 'b', type: 'hand', host: 's1', shows: 'second', dir: 'cw' }]);
  ok(near(deg(E.handAngle(H(r, 'a'), at(15))), 270) && near(deg(E.handAngle(H(r, 'b'), at(15))), 90), 'on a round face too: the face’s anticlockwise, and a clockwise override');
  const d = face([{ id: 'm', type: 'hand', host: 'dial', shows: 'minute', dir: 'ccw' }, { id: 'z', type: 'hand', host: 'dial', shows: 'minute', start: 90 }]);
  ok(near(deg(E.handAngle(H(d, 'm'), new Date(2026, 2, 7, 10, 15, 0))), 270), 'a dial hand set anticlockwise runs backwards');
  ok(near(deg(E.handAngle(H(d, 'z'), new Date(2026, 2, 7, 10, 0, 0))), 90), 'and a start override turns its zero');
}
// ---- the hand's sweep override on a fan, any measure ------------------------------------
{
  const f = face([sub({ scale: 'date', form: 'fan', fanStart: 270, fanSpan: 180 }), { id: 'h', type: 'hand', host: 's1', shows: 'date' }, { id: 'k', type: 'hand', host: 's1', shows: 'date', span: 90 }]);
  ok(near(deg(E.handAngle(H(f, 'h'), new Date(2026, 0, 16))), 270 + (180 * 15) / 31, 0.1), 'a date hand on a fan lands day 16 at 15/31 of the sweep');
  ok(near(deg(E.handAngle(H(f, 'k'), new Date(2026, 0, 16))), 270 + (90 * 15) / 31, 0.1), 'a hand’s own sweep overrides the fan’s');
}
// ---- a slider: the slot, the travel, any angle -------------------------------------------
{
  const f = face([sub({ size: 50, form: 'slider', slideLen: 120 }), { id: 'h', type: 'hand', host: 's1', shows: 'second', marker: 'dot' }]), h = H(f, 'h'), g = f.lay.subs[0];
  const p0 = E.handXY(h, at(0)), p30 = E.handXY(h, at(30));
  ok(near(p30[0] - p0[0], 60, 0.01) && near(p30[1], p0[1]), 'a hand on a slider travels half the track in half a minute, along it', JSON.stringify([p0, p30]));
  ok(near(p30[0], g.slide.sx) && near(p30[1], g.slide.sy), 'and sits on the slot at the middle of its domain');
  ok(f.lay.subHoles.s1.some((q) => q.kind === 'slot' && near(q.w, 120 + g.slide.sw)), 'the slot is a real cut through the plate, the track’s length');
  ok(h.marker === 'dot' && h.capR === 0 && h.sp.c.width < 30, 'a dot rides the slot in place of a hand');
  const v = face([sub({ size: 50, form: 'slider', slideLen: 120, slideAngle: 90 }), { id: 'h', type: 'hand', host: 's1', shows: 'second' }]), hv = H(v, 'h');
  const q0 = E.handXY(hv, at(0)), q30 = E.handXY(hv, at(30));
  ok(near(q30[1] - q0[1], 60, 0.01) && near(q30[0], q0[0]), 'a slider at 90° runs top to bottom', JSON.stringify([q0, q30]));
  const mv = face([sub({ size: 50, form: 'slider', slotFloor: 'movement' })]);
  ok(mv.lay.dialHoles.some((q) => q.kind === 'slot' && q.hidden), 'a slot can show the movement: it cuts the dial too');
}
// ---- markers on a round face -----------------------------------------------------------
{
  const f = face([{ id: 'a', type: 'hand', host: 'dial', shows: 'second', marker: 'jewel' }, { id: 'b', type: 'hand', host: 'dial', shows: 'second', marker: 'star' }]);
  ok(['a', 'b'].every((id) => H(f, id).marker && H(f, id).sp && isFinite(H(f, id).sp.px)), 'a jewel and a star stand in for the drawn hand, at the tip');
}
// ---- canon --------------------------------------------------------------------------------
{
  const c = E.canonDesign({ comps: [{ id: 's1', type: 'subdial', scale: 'seconds', form: 'round', fanStart: 10, slideLen: 90, slotStyle: 'bevel' },
    { id: 's2', type: 'subdial', scale: 'seconds', form: 'slider', slideLen: 90, slotStyle: 'bevel', slotFloor: 'dial', fanSpan: 99 },
    { id: 'h', type: 'hand', host: 's1', shows: 'second', dir: 'auto', marker: 'hand', start: '', span: null }] }).comps;
  ok(!('form' in c[0]) && !('fanStart' in c[0]) && !('slideLen' in c[0]), 'a round face carries none of the fan’s or the slider’s fields');
  ok(c[1].form === 'slider' && c[1].slideLen === 90 && !('slotStyle' in c[1]) && !('slotFloor' in c[1]) && !('fanSpan' in c[1]), 'a slider keeps its own and drops its defaults');
  ok(!('dir' in c[2]) && !('marker' in c[2]) && !('start' in c[2]) && !('span' in c[2]), 'a hand that follows its face carries no override');
}
// ---- the baker: vmin and the angled slide ------------------------------------------------
{
  const r = B.v6Drive ? B.v6Drive('rotate', { span: 3600, zero: 0, vmin: 1, vmax: 32 }, 0, 0) : null;
  ok(r && r.vmin === 1 && r.vmax === 32, 'v6Drive keeps a source’s first value (a date runs 1..32)', JSON.stringify(r && [r.vmin, r.vmax]));
  const sl = B.v6Drive ? B.v6Drive('slide-x', { span: 100, zero: -50, vmax: 60, angle: 90 }, 10, 20) : null;
  ok(sl && sl.dx === 0 && sl.dy === 100 && sl.x === 10 && sl.y === -30, 'an angled slide splits its travel into dx/dy and its start into the placement', JSON.stringify(sl && [sl.dx, sl.dy, sl.x, sl.y]));
  ok(B.pointerDefault3({ type: 'subdial', form: 'slider', scale: 'date' }, []) && B.compPlan3([{ id: 's', type: 'subdial', form: 'fan', scale: 'date' }]).rows.length === 0, 'a fan or a slider is pointer-driven, never plated');
}
// ---- the composer's bake hand-off ----------------------------------------------------------
{
  const DCLogic = function () { };
  DCLogic.prototype.setState = function (p) { Object.assign(this.state, typeof p === 'function' ? p(this.state) : p); };
  const Comp = global.COMP_CLASS(DCLogic), it = new Comp({});
  it.ready = true; it.rendering = false; it.budget = () => ({ fps: 8, dev: { pace: 8, cross: 0, cov: 0, why: '' } });
  it.state.dial = 'skeleton'; it.state.playing = false; it.state.hidden = {};
  it.state.designs = { skeleton: E.canonDesign({ print: 'roman', comps: [sub({ form: 'slider', slideLen: 120, size: 50 }), sub({ id: 's2', angle: 0, form: 'fan' }),
    { id: 'p1', type: 'hand', host: 's1', shows: 'second', marker: 'dot' }, { id: 'p2', type: 'hand', host: 's2', shows: 'second' },
    { id: 'hh', type: 'hand', host: 'dial', shows: 'hour' }, { id: 'mm', type: 'hand', host: 'dial', shows: 'minute', dir: 'ccw' }, { id: 'ss', type: 'hand', host: 'dial', shows: 'second' }] }) };
  const seat = it.handSeats3(it.design());
  ok(seat.hh === 0 && seat.ss === 2 && seat.mm === undefined, 'a reversed dial hand gives up its slot; the plain ones keep theirs', JSON.stringify(seat));
  const pp = it.pointerRows3(it.design(), { rows: [] });
  const r = (id) => pp.rows.find((q) => q.id === id) || {};
  ok(r('mm').span === -3600, 'and ships as a pointer row turning backwards', JSON.stringify(r('mm')));
  ok(r('p1').motion === 'slide-x' && r('p1').angle === 0 && r('p1').span === 120 && r('p1').freeze, 'the slider’s hand ships as a slide along its track, lifted at rest', JSON.stringify(r('p1')));
  ok(r('p2').motion === 'retrograde' && r('p2').span === 1800, 'the fan’s hand ships as the retrograde row its face makes', JSON.stringify(r('p2')));
  // the panels
  it.setState({ sel: 's1', selKind: 'comp', step: 'parts', tab: 'comps' });
  const fl = it.renderVals().fields, lab = (l) => fl.find((x) => x.label === l);
  ok(lab('Form') && lab('Track length') && lab('Slot frame') && lab('Slot shows') && !lab('Open centre') && !lab('Drive'), 'a slider’s panel offers its track and slot, and not a round face’s centre or drive');
  ok(fl.some((x) => x.isChips && (x.opts || []).some((o) => /Set from the art/.test(o.label))), 'and Set from the art');
  it.setState({ sel: 'mm', selKind: 'hand', step: 'parts', tab: 'hands' });
  const hp = it.renderVals().handRows[0];
  ok(hp && hp.dirOpts.find((o) => o.sel).v === 'ccw' && /Follow the dial/.test(hp.dirOpts[0].label) && hp.markerOpts.length === Object.keys(E.MARKERS).length, 'a hand’s panel offers Direction (following its face until set) and the indicator', hp && hp.dirOpts.map((o) => o.label).join('|'));
  hp.onDir({ target: { value: 'auto' } });
  ok(!('dir' in global.hand3(it.design(), 'mm')), 'Follow re-links the direction');
  it.setState({ sel: 'p2', selKind: 'hand', step: 'parts', tab: 'hands' });
  const fp = it.renderVals().handRows[0];
  ok(fp.showSpan && fp.spanLabel === 'Sweep' && /face/.test(fp.spanPh) && fp.pathOpts.length === 1 && /from its fan/.test(fp.pathOpts[0].label), 'a hand on a fan shows its sweep (blank follows the fan) and takes its path from it');
  // Set from the art: a fan's start and sweep from two clicks
  const g2 = it.face().lay.subs.find((q) => q.id === 's2');
  it.startCalib({ kind: 'face', id: 's2', form: 'fan' });
  const P = (a, rr) => [g2.x + Math.sin((a * Math.PI) / 180) * rr, g2.y - Math.cos((a * Math.PI) / 180) * rr];
  it.calibPoint(...P(225, 30)); it.calibPoint(...P(135, 30));
  const c2 = it.design().comps.find((q) => q.id === 's2');
  ok(c2.fanStart === 225 && c2.fanSpan === 270 && it.state.calib === null, 'two clicks set a fan’s start and its sweep the way it runs', JSON.stringify([c2.fanStart, c2.fanSpan]));
  it.startCalib({ kind: 'hand', id: 'hh', form: 'round' });
  it.calibPoint(...[233 + 50, 233]); it.calibPoint(...[233, 233 - 50]);
  const hh = global.hand3(it.design(), 'hh');
  ok(hh.start === 90 && hh.dir === 'ccw', 'on a round face: zero where clicked, and the direction the second click went', JSON.stringify([hh.start, hh.dir]));
}

console.log(fails ? 'RESULT: FAIL — ' + fails + ' of ' + checked + ' checks' : 'all green (' + checked + ' checks)');
process.exit(fails ? 1 : 0);
