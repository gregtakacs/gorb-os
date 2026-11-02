#!/usr/bin/env node
// D43 P4: the layer stack (stack.js), held to the presets and to every rule.
//
//   node headless-stack.test.js               the checks
//   node headless-stack.test.js --mutations   each mutant of stack.js must fail them
'use strict';
const fs = require('fs');
const os = require('os');
const path = require('path');
const { spawnSync } = require('child_process');
const SRC = path.join(__dirname, '..', 'stack.js');

if (process.argv.includes('--mutations')) {
  const src = fs.readFileSync(SRC, 'utf8');
  const M = [
    ['every move allowed', 'var nb = items[j] || null, why = refusal(it, nb, dir);', 'var nb = items[j] || null, why = null;'],
    ['the two hands do not trade slots', 'full[a] = key(nb); full[b] = key(it);', 'full[a] = key(it); full[b] = key(nb);'],
    ['a lift skips R3', "return r3(full, input) || { ok: true, order: full, lift: nb.ref };", "return { ok: true, order: full, lift: nb.ref };"],
    ['a lifted face never settles back', "return { ok: true, order: full.filter(function (x) { return x !== key(it); }) };", "return { ok: false, reason: 'stuck' };"],
    ['a lifted face is not counted as a row', "(input.order || []).forEach(function (e) { if (String(e).indexOf('comp:') === 0) n += 1; });", ""],
    ['children block the move', 'while (j >= 0 && j < items.length && items[j].parent && items[j].group !== it.group) j += dir;', ''],
    ['the dial moves', "push({ id: 'dial', kind: 'dial', band: 'rows', label: 'Dial', hide: 'dial', sel: { kind: 'dial', id: 'dial' },\n           badge: 'baked in', movable: false });",
      "push({ id: 'dial', kind: 'dial', group: 'subdials', band: 'rows', label: 'Dial', hide: 'dial', sel: { kind: 'dial', id: 'dial' },\n           badge: 'baked in', movable: true });"],
    ['R8 forgets coaxial order', '(SPEED[a.kind] || 0) > (SPEED[b.kind] || 0)', '(SPEED[a.kind] || 0) < 0'],
    ['shadows are not counted', 'n += input.shadows ? 2 : 1;', 'n += 1;'],
    ['discs leave their host', "if (it.kind === 'disc' && dir < 0) return", "if (it.kind === 'disc' && dir < 0 && false) return"],
  ];
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'smut.'));
  let caught = 0;
  for (const [name, a, b] of M) {
    if (!src.includes(a)) { console.log('STALE  ' + name); continue; }
    const f = path.join(dir, 'stack.js'); fs.writeFileSync(f, src.replace(a, b));
    const r = spawnSync(process.execPath, [__filename], { env: Object.assign({}, process.env, { STACK_SRC: f }), encoding: 'utf8' });
    if (r.status !== 0) caught++;
    console.log((r.status !== 0 ? 'caught ' : 'SURVIVED ') + name);
  }
  console.log(caught === M.length ? 'all ' + M.length + ' mutants caught' : (M.length - caught) + ' of ' + M.length + ' mutants survived');
  process.exit(caught === M.length ? 0 : 1);
}

require('./headless-stub.js');
const legacy3 = global.legacy3;
const E = global.WatchEngine;
const S = require(process.env.STACK_SRC || SRC);
let fails = 0, checked = 0;
const ok = (c, n, x) => { checked++; if (!c || process.env.VERBOSE) console.log((c ? 'PASS ' : 'FAIL ') + n + (x ? '  [' + x + ']' : '')); if (!c) fails++; };
const OPTS = { finish: 'train', plate: 'train', tick: 'sweep', glow: 1, beats: 1, spokes: 'train', etching: 'none', pinion: 'on', center: 'jewel', temp: 2200, innerAngle: 90, outerLight: 1, outerAngle: 315, outerTemp: 2200, trainAngle: 0, plateAngle: 0, plateScale: 1, hue: 30, hueSat: 0, outerHue: 210, outerSat: 0, orbD: 128, orbTip: 194, cageD: 107, rearFinish: 'train', shAngle: -1, shDist: 1, shBlur: 1, shOp: 1, frames: 8 };
const input = (d, fc, extra) => Object.assign({ comps: d.comps || [], hands: fc.hands.map((h) => ({ id: h.id, kind: h.kind, sub: !!h.sub, host: h.host, x: h.x, y: h.y, len: h.len })),
  order: fc.handOrder.slice(), shadows: true, carriage: false, E }, extra || {});
// a move's comps answer, the hands left out: their depth is the live band's, not the comps order
const noH = (cs) => cs.filter((c) => c.type !== 'hand').map((c) => c.id).join();
const drawn = (fc) => fc.handOrder.filter((id) => fc.hands.some((h) => h.id === id));
const ids = (items) => items.map((x) => x.id);

// ---- every preset: the stack is the design, banded, each layer once ---------------
let presets = 0;
Object.keys(E.PRESETS).forEach((k) => {
  const P = E.PRESETS[k], d = E.canonDesign(JSON.parse(JSON.stringify(P.design)));
  const fc = E.face(Object.assign({}, OPTS, { design: d, train: P.train || 'classic', hands: P.hands, handFinish: P.handFinish }));
  const inp = input(d, fc), st = S.build(inp);
  presets++;
  ok(st[0].id === 'outer' && st[1].id === 'glass' && st[st.length - 1].band === 'plate', k + ': crystal pinned on top, plate at the bottom');
  ok(new Set(ids(st)).size === st.length, k + ': every layer appears once');
  ok(fc.hands.every((h) => st.some((x) => x.id === 'hand:' + h.id)), k + ': every hand is a layer');
  ok((d.comps || []).every((c) => st.some((x) => x.id === (c.type === 'hand' ? 'hand:' : 'comp:') + c.id)), k + ': every complication is a layer, a hand as a hand');
  ok(!st.some((x) => x.kind === 'shadow') && st.filter((x) => x.group === 'live').every((x) => x.shadow), k + ': R2 shadows are their hands’ property, never a layer');
  const iSub = st.findIndex((x) => x.kind === 'subdial'), iDial = st.findIndex((x) => x.id === 'dial');
  ok(st.filter((x) => x.group === 'live').every((x) => iSub < 0 || st.indexOf(x) < iSub), k + ': R3 every hand sits above every sub-dial face');
  ok(st.filter((x) => x.kind === 'disc').every((x) => st.indexOf(x) > st.findIndex((y) => y.id === x.host)), k + ': R4 every disc sits under the face that frames it');
  ok(iSub < 0 || st.filter((x) => x.kind === 'subdial').every((x) => st.indexOf(x) < iDial), k + ': R3 every sub-dial sits above the dial');
  // the hands, top first, are WatchEngine's draw order reversed
  ok(st.filter((x) => x.group === 'live').map((x) => x.ref).reverse().join() === drawn(fc).join(), k + ': the hands are the engine\u2019s draw order, top first');
  // a round trip is a no-op: what the stack writes is what the design already held
  const hs = st.filter((x) => x.group === 'live');
  if (hs.length >= 2) {
    const down = S.move(inp, hs[0].id, 1);
    ok(down.ok, k + ': the top hand moves down one', down.reason);
    const back = S.move(Object.assign({}, inp, { order: down.order, hands: down.order.map((id) => inp.hands.find((h) => h.id === id)).filter(Boolean) }), hs[0].id, -1);
    ok(back.ok && back.order.join() === fc.handOrder.join(), k + ': moving it back restores design.order exactly');
  }
  const rc = S.rowCount(inp);
  ok(!rc.over && rc.rows > 0, k + ': R6 the live rows fit the cap', rc.rows + ' of ' + rc.cap);
  ok(S.warnings(inp).filter((w) => /Coaxial/.test(w.text)).length === 0, k + ': R8 the preset’s centre hands are in coaxial order');
});
ok(presets >= 17, 'every preset was built', presets);

// ---- the moves write what the old buttons wrote ----------------------------------
{
  const d = E.canonDesign(legacy3({ print: 'roman', centre: { hour: 1, minute: 1, second: 1 }, comps: [
    { id: 's1', type: 'subdial', angle: 180, dist: 100, size: 60, scale: 'seconds', style: 'guilloche' },
    { id: 's2', type: 'subdial', angle: 0, dist: 100, size: 60, scale: 'hours', style: 'guilloche' },
    { id: 'w1', type: 'window', host: 'dial', angle: 90, dist: 120, size: 12, shows: 'date' },
    { id: 'w2', type: 'window', host: 'dial', angle: 270, dist: 120, size: 12, shows: 'day' },
    { id: 'w3', type: 'window', host: 's2', angle: 0, dist: 20, size: 8, shows: 'date' },
    { id: 'f1', type: 'flare', host: 'dial', angle: 0, dist: 60, size: 10, style: 'print', icon: 'text', text: 'X' },
    { id: 'o1', type: 'opening', host: 'dial', angle: 45, dist: 80, size: 30 } ] }));
  const fc = E.face(Object.assign({}, OPTS, { design: d, train: 'classic' }));
  const inp = input(d, fc), st = S.build(inp), at = (id) => st.findIndex((x) => x.id === id);
  // moveHand(hid, dir) on handOrder (bottom first): dir -1 is one step DOWN the stack
  const order = fc.handOrder.slice(), top = order[order.length - 1];
  const want = order.slice(); want[want.length - 1] = want[want.length - 2]; want[want.length - 2] = top;
  const r = S.move(inp, 'hand:' + top, 1);
  ok(r.ok && r.order.join() === want.join(), 'a hand one step down writes what moveHand(id, -1) wrote', r.order && r.order.join());
  const sd = S.move(inp, 'comp:s2', 1);
  ok(sd.ok && noH(sd.comps) === 's2,s1,w1,w2,w3,f1,o1', 'a sub-dial down trades its comps slot with the next sub-dial, over the other’s children', sd.ok ? noH(sd.comps) : sd.reason);
  const dd = S.move(inp, 'comp:w2', 1);
  ok(dd.ok && noH(dd.comps) === 's1,s2,w2,w1,w3,f1,o1', 'a disc under the dial trades with the disc below it', dd.ok ? noH(dd.comps) : dd.reason);
  ok(at('comp:w3') > at('comp:s2') && at('comp:w3') < at('comp:s1'), 'a disc a sub-dial frames sits in that sub-dial\u2019s block, above the next face');
  ok(st[at('comp:f1')].parent === 'dial' && st[at('comp:o1')].parent === 'dial', 'what is printed on and cut through the dial rides the dial');

  // ---- every refusal names its rule ----
  const refuse = (id, dir, re, what) => { const x = S.move(inp, id, dir); ok(!x.ok && re.test(x.reason || ''), what, x.reason || 'allowed'); };
  refuse('hand:' + top, -1, /^R1/, 'R1: the top hand cannot rise above the crystal');
  refuse('comp:w1', 1, /^R1/, 'R1: the lowest disc cannot sink into the plate');
  refuse('glass', 1, /^R1/, 'R1: the glass is pinned');
  refuse('movement', -1, /^R1/, 'R1: the movement is pinned');
  refuse('comp:s1', 1, /^R3/, 'R3: a sub-dial cannot sink under the dial');
  refuse('comp:w2', -1, /^R4/, 'R4: a disc cannot rise over the dial that frames it');
  refuse('comp:w3', -1, /^R4/, 'R4: a disc cannot rise over the sub-dial that frames it');
  refuse('dial', 1, /^R7/, 'R7: the dial does not move');
  refuse('dial', -1, /^R7/, 'R7: the dial does not rise either');
  refuse('comp:f1', -1, /^R3/, 'R3: a flare printed on the dial moves with the dial');
  refuse('comp:o1', 1, /^R3/, 'R3: an opening is cut through the dial');
  const withHands = (hs) => Object.assign({}, inp, { hands: hs, order: hs.map((h) => h.id) });
  // ---- D43 P4b: lifting a sub-dial face among the hands ----
  // natural order, bottom first: s1's pointer, s2's pointer, then the centre hands
  const lowest = st.filter((x) => x.group === 'live').pop();
  ok(lowest.id === 'hand:s1-hand', 'the lowest live layer is the first sub-dial\u2019s pointer', lowest.id);
  const lift = S.move(inp, lowest.id, 1);
  ok(lift.ok && lift.lift === 's2' && lift.order.slice(0, 3).join() === 's1-hand,comp:s2,s2-hand', 'the lowest hand stepping under s2 lifts s2 just above it, under its own pointer', lift.ok ? lift.order.join() : lift.reason);
  const lifted = Object.assign({}, inp, { order: lift.order }), stL = S.build(lifted), atL = (id) => stL.findIndex((x) => x.id === id);
  const s2 = stL[atL('comp:s2')];
  ok(s2.group === 'live' && s2.lifted && /still row/.test(s2.badge), 'the lifted face joins the live band, badged a still row', s2.badge);
  ok(atL('comp:w3') === atL('comp:s2') + 1 && stL[atL('comp:w3')].parent === 'comp:s2', 'its disc comes with it, directly under it (R4)');
  ok(atL('hand:s2-hand') < atL('comp:s2') && atL('comp:s2') < atL('hand:s1-hand'), 'it sits under its own pointer and over the hand below');
  ok(S.rowCount(lifted).rows === S.rowCount(inp).rows + 1, 'R6: the lifted face counts as one more live row');
  const step = S.move(lifted, 'comp:s2', 1);
  ok(step.ok && step.order.slice(0, 2).join() === 'comp:s2,s1-hand', 'one step down trades it with the hand below, still lifted', step.ok ? step.order.join() : step.reason);
  const back = S.move(Object.assign({}, inp, { order: step.order }), 'comp:s2', 1);
  ok(back.ok && back.order.join() === inp.order.join(), 'a step down from the bottom of the live band settles it on the dial, and the order is the original', back.ok ? back.order.join() : back.reason);
  const overOwn = S.move(lifted, 'comp:s2', -1);
  ok(!overOwn.ok && /^R3/.test(overOwn.reason), 'R3: a lifted face cannot rise over its own pointer', overOwn.reason);
  const ownBottom = Object.assign({}, inp, { order: ['s2-hand', 's1-hand'].concat(inp.order.filter((x) => x !== 's1-hand' && x !== 's2-hand')) });
  const sink = S.move(ownBottom, 'hand:s2-hand', 1);
  ok(!sink.ok && /^R3/.test(sink.reason), 'R3: a pointer cannot step under its own face (that would lift the face over it)', sink.reason);
  const up = S.move(inp, 'comp:s2', -1);
  ok(up.ok && up.order.slice(0, 2).join() === 's1-hand,comp:s2', 'the top face on the dial stepping up is lifted over the lowest hand', up.ok ? up.order.join() : up.reason);
  const ownPtrLowest = Object.assign({}, inp, { order: ['s2-hand', 's1-hand'].concat(inp.order.filter((x) => x !== 's1-hand' && x !== 's2-hand')) });
  const up2 = S.move(ownPtrLowest, 'comp:s2', -1);
  ok(!up2.ok && /^R3/.test(up2.reason), 'R3: a face cannot lift over its own pointer when that pointer is the lowest hand', up2.reason);
  // the engine draws what the stack says
  const dL = E.canonDesign(JSON.parse(JSON.stringify(d))); dL.order = lift.order;
  const fcL = E.face(Object.assign({}, OPTS, { design: dL, train: 'classic' }));
  ok(fcL.liveSeq.map((e) => (e.kind === 'sub' ? 'comp:' + e.id : e.id)).join() === lift.order.join(), 'WatchEngine.liveSeq reads the lifted order exactly', fcL.liveSeq.map((e) => e.id).join());
  ok(fcL.handOrder.join() === fc.handOrder.join(), 'and handOrder (what an unlifted bake ships) is untouched by the lift');
  const plainFc = E.face(Object.assign({}, OPTS, { design: d, train: 'classic' }));
  ok(plainFc.liveSeq.every((e) => e.kind === 'hand') && plainFc.liveSeq.map((e) => e.id).join() === plainFc.handOrder.join(), 'with nothing lifted the live band is handOrder');
  const noSubs = Object.assign({}, inp, { comps: inp.comps.filter((c) => c.type !== 'subdial' && c.host !== 's1' && c.host !== 's2'), hands: inp.hands.filter((h) => !h.sub), order: inp.order.filter((x) => x.indexOf('centre') === 0) });
  const lowC = S.build(noSubs).filter((x) => x.group === 'live').pop();
  const dn = S.move(noSubs, lowC.id, 1);
  ok(!dn.ok && /^R3/.test(dn.reason), 'R3: with no face to lift, the lowest hand cannot sink under the dial', dn.reason);
  // drag and drop goes step by step and stops at the rule that bites
  const dnd = S.moveTo(inp, 'comp:w2', 'comp:w1');
  ok(dnd.ok && noH(dnd.comps) === 's1,s2,w2,w1,w3,f1,o1', 'a drop onto the disc below trades the two', dnd.reason);
  const bad = S.moveTo(inp, 'comp:w2', 'hand:' + top);
  ok(!bad.ok && /^R4/.test(bad.reason), 'a drop over the dial stops at R4', bad.reason);

  // ---- R8: warnings, never refusals ----
  ok(S.warnings(inp).length === 0, 'R8: the natural stack raises no mechanical warning', S.warnings(inp).map((w) => w.text).join(' | '));
  const hourAbove = withHands(inp.hands.filter((h) => h.id !== 'centre-hour').concat(inp.hands.filter((h) => h.id === 'centre-hour')));
  ok(S.warnings(hourAbove).some((w) => /Coaxial/.test(w.text) && /hour/.test(w.text)), 'R8: the hour hand above the minute hand warns of coaxial order');
  const ptrUnder = withHands(inp.hands.filter((h) => h.sub).concat(inp.hands.filter((h) => !h.sub)));
  ok(S.warnings(ptrUnder).length === 0, 'R8: pointers under the centre hands raise nothing');
  const minUnderPtr = withHands(inp.hands.filter((h) => h.id === 'centre-minute').concat(inp.hands.filter((h) => h.id !== 'centre-minute')));
  ok(S.warnings(minUnderPtr).some((w) => /Through an arbor/.test(w.text)), 'R8: a minute hand under a sub-dial pointer it sweeps over warns');
  ok(S.move(hourAbove, 'hand:centre-hour', 1).ok, 'R8 never refuses: the warned stack still moves');

  // ---- R6 ----
  const rc = S.rowCount(inp), rc0 = S.rowCount(Object.assign({}, inp, { shadows: false }));
  ok(rc.rows === inp.hands.length * 2 + 3 && rc0.rows === inp.hands.length + 3, 'R6: rows are hands and their twins plus each disc’s value row', rc.rows + ' / ' + rc0.rows);
  const many = Object.assign({}, inp, { comps: inp.comps.concat(Array.from({ length: 20 }, (_, i) => ({ id: 'x' + i, type: 'window', host: 'dial' }))) });
  ok(S.rowCount(many).over, 'R6: a design past 24 live rows is over the cap', S.rowCount(many).rows);
}

console.log(fails ? 'RESULT: FAIL — ' + fails + ' of ' + checked + ' checks' : 'all green (' + checked + ' checks)');
process.exit(fails ? 1 : 0);
