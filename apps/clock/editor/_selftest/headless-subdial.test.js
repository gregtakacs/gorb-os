#!/usr/bin/env node
// D43 P5e: a sub-dial is a face, not a value row. The baker plans it as nothing at all —
// no plate bank, no pointer, no refusal. Its hands are pointer rows the editor plans and the
// writer bakes through bakeClock3's aux seam; its wheels are wheels, and a disc may now ride
// a depthOrder rung or turn under a lifted face instead of a cutout cover.
'use strict';
require('./headless-stub.js');
const E = global.WatchEngine;
global.React = { createRef: () => ({ current: null }) };
require('../clock-logic.js');
const B = require(require('./runtime-paths.js').BAKER);
let fails = 0, checked = 0;
const ok = (c, n, x) => { checked++; if (!c || process.env.VERBOSE) console.log((c ? 'PASS ' : 'FAIL ') + n + (x ? '  [' + x + ']' : '')); if (!c) fails++; };
const pl = (w, h, a) => { const r = new Uint8Array(w * h * 4); for (let i = 0; i < w * h; i++) { r[i * 4] = 9; r[i * 4 + 3] = a; } return r; };
const PLATE = () => ({ rgba: pl(466, 466, 255), w: 466, h: 466 });
// a date wheel that turns (a plate bank would be 31 whole dials): its row comes from the
// planner, so the bake sees exactly the fields the editor feeds it
const WHEEL_ROW = B.wheelPlan3([{ id: 'w', type: 'wheel', shows: 'date', cadence: 'smooth' }]).rows[0];

// ---- 1. the face plans nothing at all ---------------------------------------------------
{
  const round = B.compPlan3([{ id: 's', type: 'subdial', host: 'dial', scale: 'seconds' },
                             { id: 'h', type: 'hand', host: 's', shows: 'second' }]);
  ok(round.rows.length === 0 && round.refuses.length === 0,
     'a seconds face with its hand plans no window row and no refusal: the hand is its pointer, the face is paint',
     JSON.stringify(round));
  const legacy = B.compPlan3([{ id: 's', type: 'subdial', scale: 'date', carrier: 'pointer', form: 'fan' }]);
  ok(legacy.rows.length === 0 && legacy.refuses.length === 0,
     'an old design carrier, scale and form are ignored outright: a date face that asked for a pointer leaves the bank with nothing',
     JSON.stringify(legacy));
  ok(B.liveComps3([{ id: 's', type: 'subdial', scale: 'date' }, { type: 'hand', host: 's', shows: 'date' }]).length === 1,
     'the sub-dial is not a live comp; only its hand is, so the button count and the export guard agree');
  const hosted = B.compPlan3([{ id: 's', type: 'subdial', scale: 'none' }, { id: 'm', type: 'moon', host: 's' }]);
  ok(hosted.rows.length === 1 && hosted.rows[0].id === 'm' && hosted.rows[0].hosted === null,
     'a moon hosted on a face is a plain unhosted row: the face leaves no tile to punch under',
     JSON.stringify(hosted.rows));
}

// ---- 2. a wheel rides the depthOrder rung it is named on ----------------------------------
{
  const disc = (o) => Object.assign({}, WHEEL_ROW, { x: 200, y: 200, w: 40, h: 40, pivot_x: 20, pivot_y: 20, rgba: pl(40, 40, 255) }, o);
  const bare = B.bakeClock3({ name: 'w0', plate: PLATE(), wheels: [disc({ exposed: true })] });
  const e0 = B.verifyBundle3(bare.bytes).sections[0].blob.elements.filter((e) => e.class === B.classId('aux'));
  ok(e0.length === 1 && e0[0].order === 0, 'an exposed wheel the stack does not name rides depth 0 as it always did', JSON.stringify(e0.map((e) => e.order)));
  const named = B.bakeClock3({ name: 'w1', plate: PLATE(), depthOrder: ['wheel:w', 'comp:f'], wheels: [disc({ exposed: true })] });
  const e1 = B.verifyBundle3(named.bytes).sections[0].blob.elements.filter((e) => e.class === B.classId('aux'));
  ok(e1.length === 1 && e1[0].order > 0, 'a wheel the stack names rides the rung depthOrder gave it, not depth 0', JSON.stringify(e1.map((e) => e.order)));
}

// ---- 3. a wheel may turn under a lifted face instead of a cutout cover -------------------
{
  const disc = (o) => Object.assign({}, WHEEL_ROW, { x: 200, y: 200, w: 40, h: 40, pivot_x: 20, pivot_y: 20, rgba: pl(40, 40, 255) }, o);
  const faceTile = { id: 'f', x: 180, y: 180, w: 80, h: 80, rgba: pl(80, 80, 200) };
  const under = B.bakeClock3({ name: 'wf', plate: PLATE(), depthOrder: ['wheel:w', 'comp:f'],
    wheels: [disc({ face: 'f' })], stills: [faceTile] });
  const el = B.verifyBundle3(under.bytes).sections[0].blob.elements.filter((e) => e.class === B.classId('aux'));
  ok(el.length === 2 && el.some((e) => e.src !== 0) && el.some((e) => e.src === 0),
     'a disc under a lifted face bakes as a turning row plus the face still row, with no cover',
     JSON.stringify(el.map((e) => [e.order, e.src])));
  let threw = ''; try { B.bakeClock3({ name: 'x', plate: PLATE(), depthOrder: ['wheel:w', 'comp:f'], wheels: [disc({ face: 'f' })], stills: [faceTile] }); } catch (e) { threw = e.message; }
  ok(!/no cover/.test(threw), 'a wheel that turns under a lifted face is exempt from the cover requirement', threw);
  threw = ''; try { B.bakeClock3({ name: 'x', plate: PLATE(), depthOrder: ['wheel:w'], wheels: [disc({ face: 'f' })], stills: [faceTile] }); } catch (e) { threw = e.message; }
  ok(/has no place just under/.test(threw), 'a disc naming a face the stack does not place just under it is refused by name', threw);
  threw = ''; try { B.bakeClock3({ name: 'x', plate: PLATE(), depthOrder: ['comp:f', 'wheel:w'], wheels: [disc({ face: 'f' })], stills: [faceTile] }); } catch (e) { threw = e.message; }
  ok(/has no place just under/.test(threw), 'and refused when the face is named BELOW the wheel, not above it', threw);
}

// ---- 4. the cover is still required when it is genuinely needed --------------------------
{
  const disc = (o) => Object.assign({}, WHEEL_ROW, { x: 200, y: 200, w: 40, h: 40, pivot_x: 20, pivot_y: 20, rgba: pl(40, 40, 255) }, o);
  let threw = ''; try { B.bakeClock3({ name: 'x', plate: PLATE(), wheels: [disc()] }); } catch (e) { threw = e.message; }
  ok(/no cover/.test(threw), 'a plain covered disc with no face and no cover is still refused', threw);
  const faceTile = { id: 'f', x: 180, y: 180, w: 80, h: 80, rgba: pl(80, 80, 200) };
  threw = ''; try { B.bakeClock3({ name: 'x', plate: PLATE(), depthOrder: ['wheel:w', 'comp:f'],
    wheels: [disc({ face: 'f' })], stills: [faceTile], cover: { x: 90, y: 90, w: 80, h: 80, rgba: pl(80, 80, 200) } }); } catch (e) { threw = e.message; }
  ok(/over no wheel it covers/.test(threw), 'a cover over wheels that every turn under a face is refused: it covers nothing', threw);
}
// ---- 5. faceReach: a hand lifts a face only when it sweeps under it ----------------------
{
  const O = { finish: 'brass', train: 'classic', frames: 16, tick: 'sweep', beats: 1 };
  const dOf = (comps, extra) => E.canonDesign(global.legacy3(Object.assign({ print: 'roman', centre: { hour: 1, minute: 1 }, comps }, extra || {})));
  const fcOf = (d) => E.face(Object.assign({}, O, { design: d, train: 'classic' }));
  const seqOf = (fc) => fc.liveSeq.map((e) => e.kind + ':' + e.id).join(' ');
  const SUB = { id: 's1', type: 'subdial', host: 'dial', angle: 0, dist: 100, size: 60, scale: 'none', style: 'guilloche' };
  const HAND = (len, extra) => Object.assign({ id: 'h1', type: 'hand', host: 'dial', shows: 'hour', angle: 180, dist: 0, length: len, width: 4 }, extra || {});
  const OWN = { id: 'sh', type: 'hand', host: 's1', shows: 'second', length: 20, width: 2 };
  const fo = fcOf(dOf([SUB, HAND(20), OWN], { order: ['h1', 'comp:s1'] }));
  ok(seqOf(fo) === 'hand:sh hand:h1 fold:s1 hand:centre-hour hand:centre-minute'
       && JSON.stringify(E.faceReach(fo.lay, fo.design, 's1', ['sh', 'h1'])) === '{"lifts":false,"by":null}',
     'a face with a lower hand that never reaches under it still folds: the fold entry keeps the ordered depth', seqOf(fo));
  ok(fo.handOrder.join(',') === 'sh,h1,centre-hour,centre-minute',
     'a folded face is out of the live band, and its own hands ride it below the band', fo.handOrder.join(','));
  const li = fcOf(dOf([SUB, HAND(120), OWN], { order: ['h1', 'comp:s1'] }));
  ok(seqOf(li) === 'hand:h1 sub:s1 hand:sh hand:centre-hour hand:centre-minute'
       && JSON.stringify(E.faceReach(li.lay, li.design, 's1', ['h1'])) === '{"lifts":true,"by":"h1"}',
     'a centre hand long enough to sweep under it lifts it to a still row at its depth, its hands with it', seqOf(li));
  const ab = fcOf(dOf([SUB, HAND(120), OWN], { order: ['comp:s1', 'h1'] }));
  ok(seqOf(ab) === 'hand:sh fold:s1 hand:h1 hand:centre-hour hand:centre-minute',
     'a hand the order puts above the face is not below it: the face folds where it stands', seqOf(ab));
  const own = fcOf(dOf([SUB, Object.assign({}, OWN, { length: 120 })], { order: ['comp:s1', 'sh'] }));
  ok(seqOf(own) === 'hand:sh fold:s1 hand:centre-hour hand:centre-minute'
       && JSON.stringify(E.faceReach(own.lay, own.design, 's1', [])) === '{"lifts":false,"by":null}',
     'a sub-dial’s own hand rides its face: it is never the layer that lifts it', seqOf(own));
  const SL = { id: 's3', type: 'subdial', host: 'dial', angle: 0, dist: 170, rot: 0, size: 40, form: 'slider', slideLen: 80, height: 0.4 };
  const sf = fcOf(dOf([SL, HAND(20)], { order: ['h1', 'comp:s3'] }));
  ok(seqOf(sf) === 'hand:h1 fold:s3 hand:centre-hour hand:centre-minute',
     'a slider face folds too: its ink is its plate about its own centre, not the dial’s centre', seqOf(sf));
  const sl = fcOf(dOf([SL, HAND(130)], { order: ['h1', 'comp:s3'] }));
  ok(seqOf(sl) === 'hand:h1 sub:s3 hand:centre-hour hand:centre-minute',
     'the same hand reaching along the slider’s plate lifts the face', seqOf(sl));
  const sd = fcOf(dOf([SL, HAND(20, { shows: 'minute', motion: { path: 'slide-x', travel: 200 } })], { order: ['h1', 'comp:s3'] }));
  ok(seqOf(sd) === 'hand:h1 sub:s3 hand:centre-hour hand:centre-minute',
     'a hand that rides a track reaches as far as the track is long, not as far as its sprite', seqOf(sd));
}
// ---- 6. a wheel lifts a face only with the area it shows -----------------------------------
{
  const O = { finish: 'brass', train: 'classic', frames: 16, tick: 'sweep', beats: 1 };
  const dOf = (comps) => E.canonDesign(global.legacy3({ print: 'roman', centre: { hour: 1, minute: 1 }, comps }));
  const fcOf = (d) => E.face(Object.assign({}, O, { design: d, train: 'classic' }));
  const seqOf = (fc) => fc.liveSeq.map((e) => e.kind + ':' + e.id).join(' ');
  const SUB = { id: 's1', type: 'subdial', host: 'dial', angle: 0, dist: 100, size: 60, scale: 'none', style: 'guilloche' };
  const WHEEL = (o) => Object.assign({ id: 'w', type: 'wheel', host: 'dial', angle: 0, dist: 170, rot: 0, size: 60, form: 'ring', inner: 20, read: 180, shows: 'date' }, o || {});
  const CUT = (o) => Object.assign({ id: 'k', type: 'cutout', host: 'w', shape: 'window', at: 180, w: 18, h: 10 }, o || {});
  const ex = fcOf(dOf([SUB, WHEEL({ shows: 'hour', exposed: true })]));
  ok(seqOf(ex) === 'sub:s1 hand:centre-hour hand:centre-minute' && ex.handOrder.join(',') === 'centre-hour,centre-minute',
     'an exposed wheel whose disc passes under an unordered face lifts it to the bottom of the live band', seqOf(ex));
  const cov = fcOf(dOf([SUB, WHEEL(), CUT({ at: 0 })]));
  ok(cov.liveSeq.every((e) => e.kind === 'hand') && Math.hypot(cov.lay.wheels[0].x - cov.lay.subs[0].x, cov.lay.wheels[0].y - cov.lay.subs[0].y) < cov.lay.wheels[0].r + cov.lay.subs[0].r
       && JSON.stringify(E.faceReach(cov.lay, cov.design, 's1', [])) === '{"lifts":false,"by":null}',
     'a covered wheel whose hidden part lies under the face but whose cutout does not does NOT lift it', seqOf(cov));
  const win = fcOf(dOf([SUB, WHEEL(), CUT({ at: 180 })]));
  ok(seqOf(win) === 'sub:s1 hand:centre-hour hand:centre-minute'
       && JSON.stringify(E.faceReach(win.lay, win.design, 's1', [])) === '{"lifts":true,"by":"w"}',
     'the same wheel lifts the face once the area its cutout shows reaches the ink, and names itself as the lifter', seqOf(win));
  // an unordered face the wheel lifts carries its own hands with it, right after it
  const grp = fcOf(dOf([SUB, { id: 'sh', type: 'hand', host: 's1', shows: 'second', length: 20, width: 2 }, WHEEL({ shows: 'hour', exposed: true })]));
  ok(seqOf(grp) === 'sub:s1 hand:sh hand:centre-hour hand:centre-minute',
     'a face lifted by a wheel leaves the plate group with its own hands riding it, emitted right after it', seqOf(grp));
  const host = fcOf(dOf([SUB, WHEEL({ host: 's1', dist: 0, size: 50, inner: 20 })]));
  ok(host.liveSeq.every((e) => e.kind === 'hand') && JSON.stringify(E.faceReach(host.lay, host.design, 's1', [])) === '{"lifts":false,"by":null}',
     'a wheel hosted on the face is above it, not below it: it does not lift its own face', seqOf(host));
  const rink = fcOf(E.canonDesign(JSON.parse(JSON.stringify(E.PRESETS.rink.design))));
  ok(rink.liveSeq.filter((e) => e.kind === 'sub').map((e) => e.id).join(',') === 's1,s2,s3',
     'Rink’s three ring sub-dials ship as still rows: each exposed ring turns under its face', rink.liveSeq.map((e) => e.kind + ':' + e.id).join(' '));
  let folded = [];
  Object.keys(E.PRESETS).forEach((k) => {
    fcOf(E.canonDesign(JSON.parse(JSON.stringify(E.PRESETS[k].design)))).liveSeq.forEach((e) => { if (e.kind === 'fold') folded.push(k + '/' + e.id); });
  });
  ok(folded.length === 0, 'no preset orders a sub-dial in the live band, so no preset carries a fold entry', folded.join(','));
}
// ---- 7. every consumer treats a fold as plate-baked ----------------------------------------
{
  const O = { finish: 'brass', train: 'classic', frames: 16, tick: 'sweep', beats: 1 };
  const D = new Date(2026, 10, 10, 10, 10, 10);
  const dOf = (comps, extra) => E.canonDesign(global.legacy3(Object.assign({ print: 'roman', centre: { hour: 1, minute: 1 }, comps }, extra || {})));
  const fcOf = (d) => E.face(Object.assign({}, O, { design: d, train: 'classic' }));
  const SUB = { id: 's1', type: 'subdial', host: 'dial', angle: 0, dist: 100, size: 60, scale: 'none', style: 'guilloche' };
  const HAND = (len) => ({ id: 'h1', type: 'hand', host: 'dial', shows: 'hour', angle: 180, dist: 0, length: len, width: 4 });
  const rowsOf = (fc) => (E.composeLayers(O, 0, fc, D, { glass: 1, outer: 1 }).live || []).map((l) => (l.still ? 'still:' + l.id : 'hands')).join('+');
  const fo = fcOf(dOf([SUB, HAND(20)], { order: ['h1', 'comp:s1'] }));
  const li = fcOf(dOf([SUB, HAND(120)], { order: ['h1', 'comp:s1'] }));
  ok(rowsOf(fo) === 'hands', 'composeLayers ships a folded face no live row: one hands row is all the band draws', rowsOf(fo));
  ok(rowsOf(li) === 'hands+still:s1+hands', 'and the lifted face ships as its still row between the hand runs it was ordered among', rowsOf(li));
  global.React = { createRef: () => ({ current: null }) };
  require('../clock-logic.js');
  const S = require('../stack.js');
  const DCLogic = function () { }; DCLogic.prototype.setState = function (p) { Object.assign(this.state, typeof p === 'function' ? p(this.state) : p); };
  const it = new (global.ClockLogic(DCLogic))({});
  it.ready = true; it.rendering = false; it.budget = () => ({ fps: 8, dev: { pace: 8, cross: 0, cov: 0, why: '' } });
  it.state.hidden = {}; it.state.dial = 'perpetual';
  const inputFor = (d) => { it.state.designs = { perpetual: d }; return it.stackInput(); };
  const inpF = inputFor(dOf([SUB, HAND(20)], { order: ['h1', 'comp:s1'] }));
  ok(inpF.order.join(',') === 'h1,comp:s1,centre-hour,centre-minute' && inpF.folded.join(',') === 's1' && inpF.liftedBy.s1 === null,
     'stackInput keeps a folded face’s order key and names it in `folded`, lifted by nothing', JSON.stringify([inpF.order, inpF.folded, inpF.liftedBy]));
  const inpL = inputFor(dOf([SUB, HAND(120)], { order: ['h1', 'comp:s1'] }));
  ok(inpL.folded.length === 0 && inpL.liftedBy.s1 === 'h1', 'a lifted face names the layer that lifts it', JSON.stringify(inpL.liftedBy));
  const rowF = S.build(inpF).find((x) => x.id === 'comp:s1');
  ok(rowF.badge === 'baked in · nothing passes under it' && rowF.lifted && rowF.group === 'live',
     'its row keeps the live position the order gave it and badges why it ships with the plate anyway', rowF.badge + '/' + rowF.group);
  ok(S.build(inpL).find((x) => x.id === 'comp:s1').badge === 'above the hands · drawn once',
     'a lifted face keeps its badge: the fold changed what the plate ships, not the panel’s language');
  ok(S.rowCount(inpF).rows === S.rowCount(inpL).rows - 1, 'R6: the folded face costs no live row, the lifted one costs one',
     S.rowCount(inpF).rows + ' vs ' + S.rowCount(inpL).rows);
  const fs = require('fs'), path = require('path');
  const CL = fs.readFileSync(path.join(__dirname, '..', 'clock-logic.js'), 'utf8');
  const WE = fs.readFileSync(path.join(__dirname, '..', 'watch-engine.js'), 'utf8');
  ok(/if \(e\.kind !== 'sub'\) return;/.test(CL) && /kind === 'fold'/.test(WE),
     't3bake’s depth walk and the composer both walk past a fold entry, so a folded face never asks for a still row');
}
// ---- 8. the spec's own geometries, and every preset still builds --------------------------
{
  const O = { finish: 'brass', train: 'classic', frames: 16, tick: 'sweep', beats: 1 };
  const dOf = (comps, order) => E.canonDesign(global.legacy3({ print: 'roman', centre: { hour: 1, minute: 1 }, comps, order }));
  const fcOf = (d) => E.face(Object.assign({}, O, { design: d, train: 'classic' }));
  const seqOf = (fc) => fc.liveSeq.map((e) => e.kind + ':' + e.id).join(' ');
  const SUB = { id: 's', type: 'subdial', host: 'dial', angle: 180, dist: 102, size: 30, scale: 'none', style: 'guilloche' };
  const OFF = (len) => ({ id: 'x', type: 'hand', host: 'dial', shows: 'second', angle: 0, dist: 120, length: len, width: 3 });
  const short = fcOf(dOf([SUB, OFF(40)], ['x', 'comp:s', 'centre-hour', 'centre-minute']));
  const long = fcOf(dOf([SUB, OFF(300)], ['x', 'comp:s', 'centre-hour', 'centre-minute']));
  ok(seqOf(short) === 'hand:x fold:s hand:centre-hour hand:centre-minute'
       && Math.hypot(short.lay.__reach.x.x - short.lay.subs[0].x, short.lay.__reach.x.y - short.lay.subs[0].y) > 40 + short.lay.subs[0].r
       && seqOf(long) === 'hand:x sub:s hand:centre-hour hand:centre-minute'
       && JSON.stringify(E.faceReach(long.lay, long.design, 's', ['x'])) === '{"lifts":true,"by":"x"}',
     'the reach is measured about the hand’s own arbour: the same off-centre hand folds the face at 40 and lifts it at 300', seqOf(short) + ' / ' + seqOf(long));
  // the Triple calendar: its ring wheels' hidden discs lie under the seconds face, their windows do not
  const cal = fcOf(E.canonDesign(JSON.parse(JSON.stringify(E.PRESETS.calendar.design))));
  const c4 = cal.lay.subs[0], c2 = cal.lay.wheels[0];
  ok(cal.liveSeq.every((e) => e.kind === 'hand') && c2.host === 'dial' && !c2.exposed
       && Math.hypot(c2.x - c4.x, c2.y - c4.y) < c2.r + c4.r
       && JSON.stringify(E.faceReach(cal.lay, cal.design, 'c4', [])) === '{"lifts":false,"by":null}',
     'the shipped triple calendar folds its seconds face: a ring wheel’s disc passes under it, its date window does not', seqOf(cal));
  // an exposed wheel that misses the face does not lift it either
  const miss = fcOf(dOf([SUB, { id: 'w', type: 'wheel', host: 'dial', angle: 0, dist: 150, rot: 0, size: 60, form: 'ring', inner: 20, read: 0, shows: 'hour', exposed: true }], null));
  ok(miss.liveSeq.every((e) => e.kind === 'hand') && JSON.stringify(E.faceReach(miss.lay, miss.design, 's', [])) === '{"lifts":false,"by":null}',
     'an exposed dial wheel whose disc does not overlap the ink does not lift it', seqOf(miss));
  // every preset builds, its hand order is untouched by the fold, and no face lifts unasked
  let drift = [], unearned = [], carries = [];
  Object.keys(E.PRESETS).forEach((k) => {
    const fc = fcOf(E.canonDesign(JSON.parse(JSON.stringify(E.PRESETS[k].design))));
    const hands = fc.liveSeq.filter((e) => e.kind === 'hand').map((e) => e.id);
    if (hands.slice().sort().join(',') !== fc.handOrder.slice().sort().join(',')
      || hands.slice().sort().join(',') !== fc.handSlots.map((h) => h.id).slice().sort().join(',')) drift.push(k);
    fc.liveSeq.forEach((e) => {
      if (e.kind !== 'sub' && e.kind !== 'fold') return;
      carries.push(k + ' ' + e.kind + ':' + e.id);
      const r = E.faceReach(fc.lay, fc.design, e.id, fc.liveSeq.slice(0, fc.liveSeq.indexOf(e)).filter((x) => x.kind === 'hand').map((x) => x.id));
      if (!!r.lifts !== (e.kind === 'sub')) unearned.push(k + '/' + e.id);
    });
  });
  ok(drift.length === 0 && unearned.length === 0,
     'for all ' + Object.keys(E.PRESETS).length + ' presets the hand order is unchanged and every sub/fold entry is the verdict faceReach itself gives',
     drift.join(',') + ' | ' + unearned.join(','));
  ok(true, 'presets whose sequence carries a still or folded face: ' + (carries.length ? carries.join(', ') : 'none'));
}
// ---- 9. the editor's bake pass: one pointer row per unseated hand, on its own pivot ------
{
  const DCLogic = function () { };
  DCLogic.prototype.setState = function (p) { Object.assign(this.state, typeof p === 'function' ? p(this.state) : p); };
  const it = new (global.ClockLogic(DCLogic))({});
  it.ready = true; it.rendering = false; it.budget = () => ({ fps: 8, dev: { pace: 8, cross: 0, cov: 0, why: '' } });
  it.state.hidden = {};
  const dOf = (comps, extra) => E.canonDesign(global.legacy3(Object.assign({ print: 'roman', centre: { hour: 1, minute: 1 }, comps }, extra || {})));
  const load = (name, d) => { it.state.dial = name; it.state.designs = {}; it.state.designs[name] = d; it.faces = {}; return it.pointerRows3(it.design(), B.compPlan3(it.design().comps)); };
  // two hands on one date sub-dial: two rows, each its own source, each its own arbour
  const TWO = dOf([{ id: 's', type: 'subdial', host: 'dial', angle: 90, dist: 110, size: 60, scale: 'none', style: 'guilloche' },
                   { id: 'hd', type: 'hand', host: 's', shows: 'date', length: 24, width: 3 },
                   { id: 'hm', type: 'hand', host: 's', shows: 'month', length: 16, width: 3, angle: 90, dist: 24 }]);
  const pr = load('calendar', TWO), by = {}; pr.rows.forEach((r) => { by[r.id] = r; });
  const sub = it.face().lay.subs[0];
  ok(pr.rows.length === 2 && pr.refuses.length === 0
     && by.hd.src === 'date' && by.hd.vmin === 1 && by.hd.vmax === 32 && by.hd.span === 3600
     && by.hm.src === 'month' && by.hm.vmin === 0 && by.hm.vmax === 12,
     'two hands on one date face plan two pointer rows, each with its own source and range, the wire unchanged', JSON.stringify(pr.rows));
  ok(Math.round(by.hd.pivot.x) === Math.round(sub.x) && Math.round(by.hd.pivot.y) === Math.round(sub.y),
     'a hand left at the face\u2019s centre turns about the face\u2019s centre', JSON.stringify([by.hd.pivot, sub.x, sub.y]));
  ok(Math.hypot(by.hm.pivot.x - sub.x, by.hm.pivot.y - sub.y) > 20,
     'and the hand moved off that centre keeps its own authored arbour: the pivot is the author\u2019s setting, not the face\u2019s centre', JSON.stringify([by.hm.pivot, by.hd.pivot]));
  ok(by.hd.cadence === 3 && by.hd.cad_arg === 0 && by.hm.cadence === undefined,
     'a plain date hand rides cadence jump (3) as the preview draws it; a month hand sweeps through its month as TURN.month does', JSON.stringify([by.hd.cadence, by.hm.cadence]));
  // every preset: exactly the unseated drawn hands become rows, none refused
  let short = [], over = [];
  Object.keys(E.PRESETS).forEach((k) => {
    const d = E.canonDesign(JSON.parse(JSON.stringify(E.PRESETS[k].design))), pp = load(k, d);
    const seat = it.handSeats3(d), want = {};
    (d.comps || []).forEach((c) => { if (c.type === 'hand' && seat[c.id] === undefined) want[c.id] = 1; });
    const got = {}; pp.rows.forEach((r) => { got[r.id] = 1; });
    Object.keys(want).forEach((id) => { if (!got[id]) short.push(k + '/' + id); });
    Object.keys(got).forEach((id) => { if (!want[id]) over.push(k + '/' + id); });
    if (pp.refuses.length) over.push(k + ': ' + pp.refuses[0]);
  });
  ok(short.length === 0 && over.length === 0,
     'for all ' + Object.keys(E.PRESETS).length + ' presets every drawn hand without a slot is exactly one pointer row and none is refused',
     short.join(',') + ' | ' + over.join(','));
  // the planned row reaches the wire: a calendar date pointer bakes as a jump row over 1..32
  const r1 = load('calendar', E.canonDesign(JSON.parse(JSON.stringify(E.PRESETS.calendar.design)))).rows[0];
  const baked = B.bakeClock3({ name: 'p5e3', plate: PLATE(),
    aux: [Object.assign({}, r1, { slot: 0, x: 100, y: 200, w: 40, h: 200, pivot_x: 20, pivot_y: 135, plane: { rgba: pl(40, 200, 255), w: 40, h: 200 } })] });
  const el = B.verifyBundle3(baked.bytes).sections[0].blob.elements.filter((e) => e.class === B.classId('aux'));
  ok(el.length === 1 && el[0].src === 3 && el[0].cadence === 3 && el[0].vmin === 1 && el[0].vmax === 32,
     'the planned date pointer row bakes onto the wire as a jump row over 1..32 of the date source', JSON.stringify(el[0] && [el[0].src, el[0].cadence, el[0].vmin, el[0].vmax]));
}
// ---- 10. depthOrder3: the stack's live band, hands, wheels, windows and lifted faces ------
{
  const dep = (seq, cr, wr) => global.ClockLogic(function () { }).prototype.depthOrder3(seq, cr, wr);
  ok(dep([], [], []) === null, 'an empty sequence names no depth list at all', String(dep([], [], [])));
  ok(dep([{ kind: 'hand', id: 'a' }, { kind: 'hand', id: 'b' }], [], []).join(',') === 'a,b',
     'nothing lifted: the band is the hand ids in sequence, which is handOrder exactly', dep([{ kind: 'hand', id: 'a' }, { kind: 'hand', id: 'b' }], [], []).join(','));
  ok(dep([{ kind: 'hand', id: 'a' }, { kind: 'fold', id: 's' }, { kind: 'hand', id: 'b' }], [{ id: 'w', host: 's' }], [{ id: 'd', host: 's' }]).join(',') === 'a,b',
     'a folded face rides the plate: no comp row, no bank row and no wheel row for it', dep([{ kind: 'hand', id: 'a' }, { kind: 'fold', id: 's' }, { kind: 'hand', id: 'b' }], [{ id: 'w', host: 's' }], [{ id: 'd', host: 's' }]).join(','));
  const seq = [{ kind: 'hand', id: 'a' }, { kind: 'sub', id: 's' }, { kind: 'hand', id: 'b' }];
  const d1 = dep(seq, [{ id: 'win', host: 's' }], [{ id: 'd1', host: 's' }, { id: 'd2', host: 'dial' }]);
  ok(d1.join(',') === 'a,wheel:d1,bank:win,comp:s,b',
     'a lifted face pushes its hosted wheels, then the windows it frames, then its own still row, and a dial wheel is left to the plate', d1.join(','));
  ok(dep(seq, [], [{ id: 'd1', face: 's' }]).join(',') === 'a,wheel:d1,comp:s,b',
     'a wheel row naming its face in `face` rides under it just as one hosted on it does', dep(seq, [], [{ id: 'd1', face: 's' }]).join(','));
}
// ---- 11. the plate-per-value leftovers are gone from the bake pass ------------------------
{
  const fs = require('fs'), path = require('path');
  const CL = fs.readFileSync(path.join(__dirname, '..', 'clock-logic.js'), 'utf8');
  ok(!/handsOf|_ptrHint|PTSCALE3|lifted3/.test(CL),
     'no hand-rides-a-tile map, no plates-driven hint and no lifted-face wheel refusal survives in the bake pass');
  const a0 = CL.indexOf('async auxPass3'), a1 = CL.indexOf('wheelPass3(wp', a0);
  const ap = CL.slice(a0, a1);
  ok(/no aperture punch on the pointer plane/.test(CL) && !/compHoles|punchHoles/.test(ap),
     'the pointer plane is built unpunched: no compHoles and no punchHoles between auxPass3 and wheelPass3');
  const cp = CL.slice(CL.indexOf('async compPass3'), a0);
  ok(/no hand rides a comp tile/.test(CL) && !/handsOf/.test(cp),
     'a comp tile names its hosted rows only: the hands-in-only append is gone with the plate-per-value sub-dial');
  ok(/if \(!l\.still \|\| !lift3\[String\(l\.id\)\]\) continue;/.test(CL),
     'stills are built from the sequence’s own lifted faces: a still with no place in the band cannot ride');
  const dep = CL.slice(CL.indexOf('depthOrder3(seq'), CL.indexOf('depthOrder3(seq') + 1400);
  ok(/return seq\.filter\(\(e\) => e\.kind === 'hand'\)\.map\(\(e\) => e\.id\)/.test(dep) && /return null;/.test(dep),
     'depthOrder3 is a pure method: hand ids alone with nothing lifted, null with an empty band');
}
// ---- 12. P5e-4: a wheel hosted on a lifted face turns just under that face ----------------
{
  const fs = require('fs'), path = require('path');
  const DCLogic = function () { };
  DCLogic.prototype.setState = function (p) { Object.assign(this.state, typeof p === 'function' ? p(this.state) : p); };
  const it = new (global.ClockLogic(DCLogic))({});
  it.ready = true; it.rendering = false; it.budget = () => ({ fps: 8, dev: { pace: 8, cross: 0, cov: 0, why: '' } });
  it.state.hidden = {};
  const cal = (order) => {
    const d = E.canonDesign(Object.assign(JSON.parse(JSON.stringify(E.PRESETS.calendar.design)), order ? { order } : {}));
    d.comps.find((c) => c.id === 'c5').cadence = 'smooth';
    it.state.dial = 'calendar'; it.state.designs = { calendar: d }; it.faces = {};
    const seq = it.face().liveSeq, wp = B.wheelPlan3(d.comps);
    it.wheelFaces3(wp, seq);
    return { d, seq, wp };
  };
  // wheelFaces3: the moon wheel hosted on the LIFTED seconds face names that face on its row
  const li = cal(['h2', 'comp:c4', 'h3', 'h4']);
  const c5l = li.wp.rows.find((r) => r.id === 'c5');
  ok(li.seq.find((e) => e.id === 'c4').kind === 'sub' && c5l && c5l.face === 'c4',
     'a turning wheel hosted on a lifted face names that face on its row', JSON.stringify([c5l && c5l.face]));
  // the same wheel, its face folded (no order): the host is not a live sub, so no face is named
  const fo = cal(null);
  const c5f = fo.wp.rows.find((r) => r.id === 'c5');
  ok(fo.seq.find((e) => e.id === 'c4') === undefined && c5f && c5f.face === undefined,
     'hosted on a folded face the wheel is plate-baked under the cover: no face on its row', JSON.stringify(c5f && c5f.face));
  // pure: a dial-hosted ring and an unknown host never gain a face, and it is idempotent
  const fake = { rows: [{ id: 'r', host: 'dial' }, { id: 'x', host: 'gone' }, { id: 'm', host: 'c4' }] };
  it.wheelFaces3(fake, [{ kind: 'sub', id: 'c4' }]);
  ok(fake.rows[0].face === undefined && fake.rows[1].face === undefined && fake.rows[2].face === 'c4',
     'a ring on the dial and a host that is not a lifted face stay faceless; a face row is left as it was', JSON.stringify(fake.rows.map((r) => r.face)));
  it.wheelFaces3(fake, []);
  ok(fake.rows[2].face === 'c4', 'wheelFaces3 never strips a face an earlier call set: it names, it does not unname', fake.rows[2].face);
  // depthOrder3 seats wheel:c5 right under comp:c4 and above h2
  const dep = it.depthOrder3(li.seq, [], li.wp.rows);
  const at = (k) => dep.indexOf(k);
  ok(at('wheel:c5') === at('comp:c4') - 1 && at('h2') < at('wheel:c5'),
     'depthOrder3 lists the faced wheel just under its lifted face and above the hand below it', dep.join(','));
  // the refusal sentence is gone from the bake pass source, and so is the lifted3 map
  const CL = fs.readFileSync(path.join(__dirname, '..', 'clock-logic.js'), 'utf8');
  const REFUSAL = 'turns under the ' + 'face';   // split so the source check does not re-add the sentence
  const e3 = CL.slice(CL.indexOf('_export3Body'), CL.indexOf('_export3Body') + 9000);
  ok(!CL.includes(REFUSAL) && !/lifted3/.test(e3),
     'the lifted-face wheel refusal and its lifted3 map are gone from the source');
  // the drive page parses add/set/order and each rebuilds and settles
  const DRV = fs.readFileSync(path.join(__dirname, 'bank3-drive.html'), 'utf8');
  // a shared reDrive helper does the faces reset, buildMovement and the settle check for all three
  const rd = DRV.indexOf('const reDrive');
  ok(rd >= 0 && /IT.faces = \{\}/.test(DRV.slice(rd, rd + 400)) && /buildMovement\(\)/.test(DRV.slice(rd, rd + 400)) && /did not settle after the /.test(DRV.slice(rd, rd + 400)),
     'a reDrive helper resets faces, rebuilds the movement and runs the same settle check the ?lift= path uses');
  const drvOk = (n) => { const k = DRV.indexOf("QP.get('" + n + "')"); return k >= 0 && /reDrive\('/.test(DRV.slice(k, k + 900)); };
  ok(drvOk('add') && drvOk('set') && drvOk('order') && DRV.indexOf("QP.get('add')") < DRV.indexOf("QP.get('set')") && DRV.indexOf("QP.get('set')") < DRV.indexOf("QP.get('order')"),
     'bank3-drive parses ?add/?set/?order, drives each through reDrive (build + settle), applied add then set then order');
  // the pass under the node stub: a lone faced disc does not refuse and ships no cover
  try {
    const wp1 = { rows: li.wp.rows.filter((r) => r.face) };
    const out = it.wheelPass3(wp1, []);
    ok(out && !out.refuse && out.cover === null && out.rows.length === 1 && out.rows[0].face === 'c4',
       'wheelPass3 under the stub: a lone faced disc is read through no cutout without refusing, and ships cover null',
       out && (out.refuse || ('cover=' + (out.cover ? 'set' : 'null') + ' rows=' + out.rows.length)));
  } catch (e) {
    ok(false, 'wheelPass3 under the stub ran without throwing', e.message);
  }
  // baker end to end: a still row for the face, a faced wheel row, no cover, real depthOrder
  const WR = li.wp.rows.find((r) => r.id === 'c5');
  const disc = Object.assign({}, WR, { x: 200, y: 200, w: 40, h: 40, pivot_x: 20, pivot_y: 20, rgba: pl(40, 40, 255) });
  const faceTile = { id: 'c4', x: 180, y: 180, w: 80, h: 80, rgba: pl(80, 80, 200) };
  let threw = '';
  try { B.bakeClock3({ name: 'p5e4', plate: PLATE(), depthOrder: dep, wheels: [disc], stills: [faceTile] }); } catch (e) { threw = e.message; }
  ok(!threw, 'the faced wheel bakes through bakeClock3 on the real depthOrder3 list with no cover and does not throw', threw);
}
console.log((fails ? 'FAIL ' + fails + ' of ' : 'all green (') + checked + (fails ? '' : ' checks)'));
process.exit(fails ? 1 : 0);
