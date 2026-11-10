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
    ['a drop into a face does not re-host', "        hc.host = into;\n", "\n"],
    ['a hand may turn under the dial', "      if (tg.kind === 'disc' || (onDial(tg) && !(tg.kind === 'dial' && above)))", "      if (false)"],
    ['a face\u2019s hands leave its group', "    if (it.kind === 'pointer') return 'R3: a sub-dial\\u2019s hand rides its face", "    if (false) return 'R3: a sub-dial\\u2019s hand rides its face"],
    ['a lifted face never settles back', "return { ok: true, order: full.filter(function (x) { return x !== key(it); }) };", "return { ok: false, reason: 'stuck' };"],
    ['a lifted face is not counted as a row', "liveKeys(input).forEach(function (e) { if (e.indexOf('comp:') === 0 && !foldIn[e.slice(5)]) n += 1; });", ""],
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
  ok(st.filter((x) => x.kind === 'hand').every((x) => iSub < 0 || st.indexOf(x) < iSub), k + ': R3 every hand on the dial sits above every sub-dial face');
  // D43 P5d: a sub-dial's hands are its children, directly under its face row
  ok(st.filter((x) => x.kind === 'pointer').every((x) => { const f = st.findIndex((y) => y.id === x.parent); return f >= 0 && st.indexOf(x) > f && st.slice(f + 1, st.indexOf(x)).every((y) => y.parent === x.parent); }), k + ': a sub-dial\u2019s hands are nested under its face');
  ok(st.filter((x) => x.kind === 'disc').every((x) => st.indexOf(x) > st.findIndex((y) => y.id === x.host)), k + ': R4 every disc sits under the face that frames it');
  ok(iSub < 0 || st.filter((x) => x.kind === 'subdial').every((x) => st.indexOf(x) < iDial), k + ': R3 every sub-dial sits above the dial');
  // the hands, top first, are WatchEngine's draw order reversed
  ok(st.filter((x) => x.kind === 'hand' || x.kind === 'pointer').map((x) => x.ref).reverse().join() === drawn(fc).join(), k + ': the hands are the engine\u2019s draw order, top first', st.filter((x) => x.kind === 'hand' || x.kind === 'pointer').map((x) => x.ref).reverse().join() + ' vs ' + drawn(fc).join());
  // a round trip is a no-op: what the stack writes is what the design already held
  const hs = st.filter((x) => x.group === 'live');
  if (hs.length >= 2) {
    const down = S.move(inp, hs[0].id, 1);
    ok(down.ok, k + ': the top hand moves down one', down.reason);
    const back = S.move(Object.assign({}, inp, { order: down.order }), hs[0].id, -1);
    ok(back.ok && back.order.join() === S.liveKeys(inp).join(), k + ': moving it back restores design.order exactly');
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
    { id: 'w1', type: 'window', host: 'dial', angle: 90, dist: 120, size: 12, shows: 'custom' },
    { id: 'w2', type: 'window', host: 'dial', angle: 270, dist: 120, size: 12, shows: 'custom' },
    { id: 'w3', type: 'window', host: 's2', angle: 0, dist: 20, size: 8, shows: 'custom' },
    { id: 'f1', type: 'flare', host: 'dial', angle: 0, dist: 60, size: 10, style: 'print', icon: 'text', text: 'X' },
    { id: 'o1', type: 'opening', host: 'dial', angle: 45, dist: 80, size: 30 } ] }));
  const fc = E.face(Object.assign({}, OPTS, { design: d, train: 'classic' }));
  const inp = input(d, fc), st = S.build(inp), at = (id) => st.findIndex((x) => x.id === id);
  const live0 = S.liveKeys(inp), top = live0[live0.length - 1];
  ok(live0.join() === 'centre-hour,centre-minute,centre-second', 'the live band is the dial’s own hands; a sub-dial’s hands ride their face', live0.join());
  const want = live0.slice(); want[want.length - 1] = want[want.length - 2]; want[want.length - 2] = top;
  const r = S.move(inp, 'hand:' + top, 1);
  ok(r.ok && r.order.join() === want.join(), 'a hand one step down trades places with the hand below', r.order && r.order.join());
  const sd = S.move(inp, 'comp:s2', 1);
  ok(sd.ok && noH(sd.comps) === 's2,s1,w1,w2,w3,f1,o1', 'a sub-dial down trades its comps slot with the next sub-dial, over the other’s children', sd.ok ? noH(sd.comps) : sd.reason);
  const dd = S.move(inp, 'comp:w2', 1);
  ok(dd.ok && noH(dd.comps) === 's1,s2,w2,w1,w3,f1,o1', 'a disc under the dial trades with the disc below it', dd.ok ? noH(dd.comps) : dd.reason);
  ok(at('comp:w3') > at('comp:s2') && at('comp:w3') < at('comp:s1'), 'a disc a sub-dial frames sits in that sub-dial’s block, above the next face');
  ok(st[at('comp:f1')].parent === 'dial' && st[at('comp:o1')].parent === 'dial', 'what is printed on and cut through the dial rides the dial');
  // D43 P5d: the face's hands are its children, first under it
  ok(at('hand:s2-hand') === at('comp:s2') + 1 && st[at('hand:s2-hand')].parent === 'comp:s2' && at('hand:s1-hand') === at('comp:s1') + 1, 'a sub-dial’s hand is nested directly under its face, like its windows');

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
  refuse('hand:s2-hand', -1, /^R3.*another face/, 'R3: a sub-dial’s hand does not step out of its face; the drag re-hosts it');
  // ---- D43 P4b: lifting a sub-dial face among the hands, its hands with it ----
  const lowest = st.filter((x) => x.group === 'live').pop();
  ok(lowest.id === 'hand:centre-hour', 'the lowest live layer is the dial’s lowest hand', lowest.id);
  const lift = S.move(inp, lowest.id, 1);
  ok(lift.ok && lift.lift === 's2' && lift.order.join() === 'centre-hour,comp:s2,centre-minute,centre-second', 'the lowest hand stepping under s2 lifts s2 just above it', lift.ok ? lift.order.join() : lift.reason);
  const lifted = Object.assign({}, inp, { order: lift.order }), stL = S.build(lifted), atL = (id) => stL.findIndex((x) => x.id === id);
  const s2 = stL[atL('comp:s2')];
  ok(s2.group === 'live' && s2.lifted && /above the hands/.test(s2.badge), 'the lifted face joins the live band, badged above the hands', s2.badge);
  ok(atL('hand:s2-hand') === atL('comp:s2') + 1 && atL('comp:w3') > atL('hand:s2-hand') && stL[atL('comp:w3')].parent === 'comp:s2', 'its hand and its disc come with it (R4)');
  ok(atL('comp:s2') < atL('hand:centre-hour'), 'and it sits over the hand below');
  ok(S.rowCount(lifted).rows === S.rowCount(inp).rows + 1, 'R6: the lifted face counts as one more live row');
  const step = S.move(lifted, 'comp:s2', 1);
  ok(step.ok && step.order.slice(0, 2).join() === 'comp:s2,centre-hour', 'one step down trades it with the hand below, still lifted', step.ok ? step.order.join() : step.reason);
  const back = S.move(Object.assign({}, inp, { order: step.order }), 'comp:s2', 1);
  ok(back.ok && back.order.join() === live0.join(), 'a step down from the bottom of the live band settles it on the dial, and the order is the original', back.ok ? back.order.join() : back.reason);
  const up = S.move(inp, 'comp:s2', -1);
  ok(up.ok && up.order.slice(0, 2).join() === 'centre-hour,comp:s2', 'the top face on the dial stepping up is lifted over the lowest hand', up.ok ? up.order.join() : up.reason);
  // the engine draws what the stack says: a lifted face carries its hands
  const dL = E.canonDesign(JSON.parse(JSON.stringify(d))); dL.order = lift.order;
  const fcL = E.face(Object.assign({}, OPTS, { design: dL, train: 'classic' }));
  ok(fcL.liveSeq.map((e) => (e.kind === 'sub' ? 'comp:' + e.id : e.id)).join() === 's1-hand,centre-hour,comp:s2,s2-hand,centre-minute,centre-second', 'WatchEngine.liveSeq draws the lifted face with its hand on it', fcL.liveSeq.map((e) => (e.kind === 'sub' ? 'comp:' + e.id : e.id)).join());
  ok(fcL.handOrder.join() === 's1-hand,centre-hour,s2-hand,centre-minute,centre-second', 'and handOrder moves that hand with its face', fcL.handOrder.join());
  const plainFc = E.face(Object.assign({}, OPTS, { design: d, train: 'classic' }));
  ok(plainFc.liveSeq.every((e) => e.kind === 'hand') && plainFc.liveSeq.map((e) => e.id).join() === plainFc.handOrder.join(), 'with nothing lifted the live band is handOrder');
  const noSubs = Object.assign({}, inp, { comps: inp.comps.filter((c) => c.type !== 'subdial' && c.host !== 's1' && c.host !== 's2'), hands: inp.hands.filter((h) => !h.sub), order: inp.order.filter((x) => x.indexOf('centre') === 0) });
  const lowC = S.build(noSubs).filter((x) => x.group === 'live').pop();
  const dn = S.move(noSubs, lowC.id, 1);
  ok(!dn.ok && /^R3/.test(dn.reason), 'R3: with no face to lift, the lowest hand cannot sink under the dial', dn.reason);
  // ---- D43 P5d: place(), the drag ----
  const P = (id, t, w) => S.place(inp, id, t, w);
  const into = P('hand:centre-second', 'comp:s1', 'below');
  ok(into.ok && into.host === 's1' && into.comps.find((c) => c.id === 'centre-second').host === 's1' && into.order.indexOf('centre-second') < 0, 'a dial hand dropped into a face’s group is re-hosted on that sub-dial', JSON.stringify(into.order));
  const out = P('hand:s1-hand', 'hand:centre-minute', 'above');
  ok(out.ok && out.host === 'dial' && out.order.join() === 'centre-hour,centre-minute,s1-hand,centre-second', 'a sub-dial’s hand dropped among the dial’s hands turns on the dial there', out.ok ? out.order.join() : out.reason);
  const across = P('hand:s2-hand', 'hand:s1-hand', 'above');
  ok(across.ok && across.host === 's1' && across.comps.filter((c) => c.type === 'hand' && c.host === 's1').map((c) => c.id).join() === 's1-hand,s2-hand', 'dropped onto another face’s hand it moves to that face, above it', JSON.stringify(across.comps.filter((c) => c.type === 'hand').map((c) => c.id + '@' + c.host)));
  const under = P('hand:centre-hour', 'dial', 'below');
  ok(!under.ok && /^R3/.test(under.reason), 'R3: no hand turns under the dial', under.reason);
  const sky = P('hand:centre-hour', 'outer', 'above');
  ok(!sky.ok && /^R1/.test(sky.reason), 'R1: nothing goes above the glass and the case', sky.reason);
  const topFace = P('comp:s1', 'hand:' + top, 'above');
  ok(topFace.ok && topFace.lift === 's1' && topFace.order[topFace.order.length - 1] === 'comp:s1', 'a face dropped above the top hand is lifted to the top of the stack', topFace.ok ? topFace.order.join() : topFace.reason);
  const discUp = P('comp:w2', 'hand:' + top, 'above');
  ok(!discUp.ok && /^R4/.test(discUp.reason), 'R4: a disc stays under the face that frames it, wherever it is dragged', discUp.reason);
  const sink = P('hand:centre-hour', 'dial', 'above');
  ok(sink.ok && sink.order.slice(0, 3).join() === 'centre-hour,comp:s1,comp:s2', 'a hand dropped just above the dial passes under every face: those faces are lifted over it', sink.ok ? sink.order.join() : sink.reason);
  const dS = E.canonDesign(JSON.parse(JSON.stringify(d))); dS.order = sink.order;
  const fcS = E.face(Object.assign({}, OPTS, { design: dS, train: 'classic' }));
  ok(fcS.handOrder[0] === 'centre-hour', 'and the engine draws that hand first, under the faces', fcS.handOrder.join());
  // drag and drop goes step by step and stops at the rule that bites
  const dnd = S.moveTo(inp, 'comp:w2', 'comp:w1');
  ok(dnd.ok && noH(dnd.comps) === 's1,s2,w2,w1,w3,f1,o1', 'a drop onto the disc below trades the two', dnd.reason);
  const bad = S.moveTo(inp, 'comp:w2', 'hand:' + top);
  ok(!bad.ok && /^R4/.test(bad.reason), 'a drop over the dial stops at R4', bad.reason);
  const withHands = (hs) => Object.assign({}, inp, { hands: hs, order: hs.map((h) => h.id) });
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
