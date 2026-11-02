#!/usr/bin/env node
// D43 P4b: still rows — a sub-dial face lifted among the hands — held to the baker's
// own walk-back and to the SHIPPED C projection (tools/verify_js_bake.py).
'use strict';
const fs = require('fs');
const os = require('os');
const path = require('path');
const { spawnSync } = require('child_process');
const ROOT = path.resolve(__dirname, '..', '..', '..', '..', '..');
if (process.argv.includes('--mutations')) {
  const SRC = path.join(__dirname, '..', 'baker@1', 'orbtheme-baker.js'), src = fs.readFileSync(SRC, 'utf8');
  const M = [
    ['the still row loses its depth', "order: Math.max(1, Math.min(63, rung3[stKey])), alpha: 255, n_frames: 1 }, st, true, true);", "order: 63, alpha: 255, n_frames: 1 }, st, true, true);"],
    ['the still row is not dithered', "order: Math.max(1, Math.min(63, rung3[stKey])), alpha: 255, n_frames: 1 }, st, true, true);", "order: Math.max(1, Math.min(63, rung3[stKey])), alpha: 255, n_frames: 1 }, st, true, false);"],
    ['the still row names a source', "src: SOURCES.indexOf('none'), x: st.x | 0", "src: SOURCES.indexOf('second'), x: st.x | 0"],
    ['a lifted face\u2019s window keeps depth 0', "var bankOrd = c4.id !== undefined && rung3['bank:' + c4.id] !== undefined", "var bankOrd = false"],
    ['a still row without a place is accepted', "if (rung3[stKey] === undefined)\n        throw", "if (false)\n        throw"],
  ];
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'stmut.'));
  let caught = 0;
  for (const [name, a, b] of M) {
    if (!src.includes(a)) { console.log('STALE  ' + name); continue; }
    const f = path.join(dir, 'b.js'); fs.writeFileSync(f, src.replace(a, b));
    const r = spawnSync(process.execPath, [__filename], { env: Object.assign({}, process.env, { STILL_BAKER: f }), encoding: 'utf8' });
    if (r.status !== 0) caught++;
    console.log((r.status !== 0 ? 'caught ' : 'SURVIVED ') + name);
  }
  console.log(caught === M.length ? 'all ' + M.length + ' mutants caught' : (M.length - caught) + ' of ' + M.length + ' mutants survived');
  process.exit(caught === M.length ? 0 : 1);
}
const B = require(process.env.STILL_BAKER || path.join(__dirname, '..', 'baker@1', 'orbtheme-baker.js'));
const PY = process.env.PY || 'python3';
let fails = 0, checked = 0;
const ok = (c, n, x) => { checked++; if (!c || process.env.VERBOSE) console.log((c ? 'PASS ' : 'FAIL ') + n + (x ? '  [' + x + ']' : '')); if (!c) fails++; };
const refuses = (fn, re, n) => { let m = ''; try { fn(); } catch (e) { m = String(e.message); } ok(re.test(m), n, m.slice(0, 120) || 'no refusal'); };

const W = 466;
const plane = (w, h, f) => { const r = new Uint8Array(w * h * 4); for (let i = 0; i < w * h; i++) { const c = f(i % w, (i / w) | 0); r.set(c, i * 4); } return { w, h, rgba: r }; };
const PL = plane(W, W, (x, y) => [20 + (x >> 3), 30, 40 + (y >> 3), 255]);
const hand = (slot, len) => Object.assign(plane(9, len, () => [220, 200, 120, 255]), { slot, x: 229, y: 233 - len + 10, pivot_x: 4, pivot_y: len - 10 });
const face = Object.assign(plane(120, 120, (x, y) => { const d = Math.hypot(x - 60, y - 60); return [180, 170 + (d | 0) % 40, 150, d < 58 ? 255 : 0]; }), { id: 's1', x: 173, y: 263 });
const base = (extra) => Object.assign({ name: 'Still', serial: 1, bakedMs: 1, plate: PL, hands: [hand(0, 120), hand(1, 180)], order: [0, 1],
  scalars: { second_sweep: 0, tick_rate: 1 }, depthOrder: ['centre-hour', 'comp:s1', 'centre-minute'], stills: [face] }, extra || {});

const bake = B.bakeClock3(base());
const wk = B.verifyBundle3(bake.bytes).sections[0], els = wk.blob.elements;
const aux = els.filter((e) => e.class === B.CLASS_AUX);
const still = aux.find((e) => e.src === 0), hour = aux.find((e) => e.src !== 0 && e.slot === 0), minute = aux.find((e) => e.src !== 0 && e !== hour && !(e.flags & B.AUX_F_SHADOW3) && e.w === 9 && e.h === 180);
ok(!!still, 'the lifted face ships as an AUX row with source none');
ok(still && still.cadence === 0 && still.span === 0 && still.dx === 0 && still.dy === 0 && still.vmin === 0 && still.vmax === 0, 'a still row names no motion: no cadence, span, travel or domain');
ok(still && still.x === 173 && still.y === 263 && still.w === 120 && still.h === 120, 'the still row sits where the face sits');
ok(still && hour && minute && hour.order < still.order && still.order < minute.order, 'depthOrder places it between the hour and the minute hands',
   [hour && hour.order, still && still.order, minute && minute.order].join(' < '));
const spr = wk.blob.sprites[still.frame0], art = wk.art.subarray(spr.off, spr.off + spr.size);
const want = B.rgb565Plane(face.rgba, 120, 120, { x: 173, y: 263 });
ok(Buffer.from(art.subarray(0, 120 * 120 * 2)).equals(Buffer.from(want)), 'the still row is dithered at its place on the panel, like any still plane');

// the shipped C projection accepts it
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'still.'));
const f = path.join(tmp, 'still.orbtheme'); fs.writeFileSync(f, bake.bytes);
const v = spawnSync(PY, [path.join(ROOT, 'tools', 'verify_js_bake.py'), f], { encoding: 'utf8' });
ok(v.status === 0, 'the shipped orb_clock3.c projection accepts a bake with a still row', (v.stdout + v.stderr).trim().split('\n').slice(-3).join(' | ').slice(0, 300));

// a window the lifted face frames rides just under it
const bankBake = B.bakeClock3(base({ depthOrder: ['centre-hour', 'bank:w1', 'comp:s1', 'centre-minute'],
  comps: [{ id: 'w1', slot: 0, src: 'date', x: 200, y: 300, w: 20, h: 14, planes: Array.from({ length: 31 }, () => plane(20, 14, () => [255, 255, 255, 255])) }] }));
const els2 = B.verifyBundle3(bankBake.bytes).sections[0].blob.elements;
const bank = els2.find((e) => e.class === B.classId('bank')), st2 = els2.find((e) => e.class === B.CLASS_AUX && e.src === 0);
const h2 = els2.find((e) => e.class === B.CLASS_AUX && e.src !== 0 && e.slot === 0);
ok(bank && st2 && h2 && h2.order < bank.order && bank.order < st2.order, 'its window sits between the hand below and the face', [h2 && h2.order, bank && bank.order, st2 && st2.order].join(' < '));
const f2 = path.join(tmp, 'still-bank.orbtheme'); fs.writeFileSync(f2, bankBake.bytes);
const v2 = spawnSync(PY, [path.join(ROOT, 'tools', 'verify_js_bake.py'), f2], { encoding: 'utf8' });
ok(v2.status === 0, 'the C projection accepts a window at a lifted depth', (v2.stdout + v2.stderr).trim().split('\n').slice(-2).join(' | ').slice(0, 300));

// without stills the bake is what it always was
const plain = B.bakeClock3(base({ stills: [], depthOrder: ['centre-hour', 'centre-minute'] }));
ok(!B.verifyBundle3(plain.bytes).sections[0].blob.elements.some((e) => e.class === B.CLASS_AUX && e.src === 0), 'no lifted face, no still row');

refuses(() => B.bakeClock3(base({ depthOrder: ['centre-hour', 'centre-minute'] })), /no place in depthOrder/, 'a still row with no place in the stack is refused');
refuses(() => B.bakeClock3(base({ stills: [Object.assign({}, face, { rgba: new Uint8Array(4) })] })), /no pixels/, 'a still row with no art is refused');

console.log(fails ? 'RESULT: FAIL — ' + fails + ' of ' + checked + ' checks' : 'all green (' + checked + ' checks)');
process.exit(fails ? 1 : 0);
