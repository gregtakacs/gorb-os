#!/usr/bin/env node
/* headless-estimate.test.js — the bake's commit price (GorbBake.estimate3), pinned and mutated.
 *
 * docs/unified-render-path-design-2026-10-30.md §5/§7: bakeClock3 prices the flush the
 * device will pay for the bytes it just wrote, and splitPolicy3's sentence carries the
 * rate. Three claims are pinned here, each against a mutant that must break it:
 *   1. the rung is orb_sched_pick_rung's, line for line (the vectors are §6's table and
 *      host_sched_rung_test.py's founding cases);
 *   2. the price lands inside 15% of the glass on the repo's own shipped Skeleton —
 *      measured on orb-A at v1.1.38: flush avg 95 ms, 4 fps (rung 4);
 *   3. slice 5's grant rule: smoothing only where the estimate WITH the AA term holds 12,
 *      and est_fps is priced with the term exactly then.
 *
 * Run: node tools/watch-composer/_selftest/headless-estimate.test.js
 */
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const BAKER = path.join(__dirname, '..', 'orbtheme-baker.js');
const SKEL = path.join(__dirname, '..', '..', 'orb-manager', 'themes', 'skeleton.orbtheme');
const SRC = fs.readFileSync(BAKER, 'utf8');

function load(src) {
  const ctx = { window: {}, console, Uint8Array, Uint16Array, Uint32Array, Int16Array, Int32Array,
                DataView, ArrayBuffer, Math, Date, TextEncoder, TextDecoder, Error, JSON };
  ctx.self = ctx.window;
  vm.createContext(ctx);
  vm.runInContext(src, ctx);
  return ctx.window.GorbBake;
}

const plane = (w, h, a) => { const r = new Uint8Array(w * h * 4); for (let i = 0; i < w * h; i++) { r[i * 4] = 40; r[i * 4 + 1] = 40; r[i * 4 + 2] = 40; r[i * 4 + 3] = a; } return r; };
const PL = { rgba: plane(466, 466, 255), w: 466, h: 466 };
const hand = (slot, w, h, py) => ({ slot, rgba: plane(w, h, 255), x: 233 - (w >> 1), y: 233 - py, w, h, pivot_x: w >> 1, pivot_y: py });
const GLASS = { x: 0, y: 0, w: 466, h: 466, rgba: plane(466, 466, 20) };

/* Each vector: [name, fn(G) -> bool]. A vector reads the baker it is handed, so the
 * same list judges the shipped source and every mutant of it. */
const VECTORS = [
  // ---- 1. the rung, orb_sched_pick_rung's own answers ----
  ['fluid 15 on the still floor picks 15', (G) => G.estimate3 && rung(G, [{ fps: 15, mode: 'fluid' }], 15) === 15],
  ['a 4 fps bank under a 15 sweep picks 12 on the GIF floor', (G) => rung(G, [{ fps: 4, mode: 'distinct' }, { fps: 15, mode: 'fluid' }], 12) === 12],
  ['distinct 4 + fluid 4 picks 4 (the caged dial)', (G) => rung(G, [{ fps: 4, mode: 'distinct' }, { fps: 4, mode: 'fluid' }], 15) === 4],
  ['a lone distinct 4 on the still floor picks 12 (§6: the cap seeds from the floor)', (G) => rung(G, [{ fps: 4, mode: 'distinct' }], 15) === 12],
  ['distinct 5 has no clean rung and falls back to itself', (G) => rung(G, [{ fps: 5, mode: 'distinct' }], 15) === 5],
  ['a fluid ask under the panel floor clamps to 4', (G) => rung(G, [{ fps: 1, mode: 'fluid' }], 15) === 4],
  // ---- 2. the shipped Skeleton, priced against its own glass ----
  ['Skeleton: rung 4 from its 4 fps bank and 4 bps sweep', (G) => skel(G).rung === 4],
  ['Skeleton: est 4 fps, as measured', (G) => skel(G).est_fps === 4],
  ['Skeleton: flush within 15% of the measured 95 ms', (G) => Math.abs(skel(G).flush_ms - 95) <= 95 * 0.15],
  ['Skeleton: priced as a delta bank', (G) => skel(G).carrier === 'delta bank'],
  ['Skeleton: rung 4 can never take smoothing', (G) => skel(G).smooth === false],
  // ---- 3. synthetic still dials: the cap, the price, the grant ----
  ['a sweeping still dial rides rung 15', (G) => still(G, 15, 1).rung === 15],
  ['a 4 bps sweep is capped at 4 however cheap', (G) => still(G, 4, 1).est_fps === 4],
  ['a ticking still dial rides its own beat', (G) => still(G, 4, 0).rung === 4 && still(G, 4, 0).asks === 'hand beat'],
  ['a cheap still dial at rung 15 is granted smoothing', (G) => still(G, 15, 1).smooth === true],
  ['a granted dial is priced WITH the AA term', (G) => { const e = still(G, 15, 1); return e.est_fps === e.aa_fps && /AA term estimated/.test(e.sentence); }],
  ['a dial the filter slows but still grants ships the slower, smoothed estimate', (G) => { const e = medium(G); return e.smooth && e.near_fps === 15 && e.aa_fps === 13 && e.est_fps === 13; }],
  ['a keyframe bank under a 15 sweep rides the panel floor rung 4', (G) => G.estimate3(fake({ bg_mode: 0, bg_frames: 7, bg_fps: 4, bg_loop: 1, gif_frames: 0, tourbillon_n: 0, second_sweep: 1, tick_rate: 15 }, 0)).rung === 4],
  ['a keyframe bank is priced on the classic path', (G) => /keyframe bank \(LVGL\)/.test(G.estimate3(fake({ bg_mode: 0, bg_frames: 7, bg_fps: 4, bg_loop: 1, gif_frames: 0, tourbillon_n: 0, second_sweep: 1, tick_rate: 15 }, 0)).carrier)],
  ['a costly still dial (wide hands, glass) is refused smoothing', (G) => heavy(G).smooth === false && heavy(G).est_fps === heavy(G).near_fps],
  ['the costly dial still prints what smoothing would cost', (G) => /smoothing would land \d+ fps/.test(heavy(G).sentence)],
  ['the sweeping seconds pays no AA', (G) => { const e = still(G, 15, 1); return e.aa_kpx < e.ink_kpx; }],
  ['the glass rides the box', (G) => heavy(G).glass_cov > 0.9],
  // ---- 4. the policy carries it ----
  ['splitPolicy3 carries est_fps and the rung when handed an estimate', (G) => { const e = still(G, 15, 1), p = pol(G, e); return p.est_fps === e.est_fps && p.rung === 15; }],
  ['and its sentence names the rate', (G) => / est \d+ fps \(/.test(pol(G, still(G, 15, 1)).sentence)],
  ['a refusal stays a refusal', (G) => G.splitPolicy3({ frames: 1, fps: 0, est: still(G, 15, 1) }).carrier === 'refuse'],
  ['bakeClock3 returns the estimate of what it wrote', (G) => { const b = G.bakeClock3(stillOpts(15, 1)); return b.est && b.est.rung === 15; }],
  // ---- 5. slice 5: the grant is baked as EF_SMOOTH (bit 7) on the turned rows ----
  ['EF_SMOOTH is bit 7', (G) => G.EF_SMOOTH === 128],
  ['a granted dial bakes EF_SMOOTH on its hour and minute rows', (G) => { const f = flags(G, stillOpts(15, 1)); return !!(f.h0 & 128) && !!(f.h1 & 128); }],
  ['but never on the sweeping seconds, the hot tick', (G) => !(flags(G, stillOpts(15, 1)).h2 & 128)],
  ['a ticking dial\'s seconds may smooth (it does not re-pose every tick)', (G) => { const f = flags(G, Object.assign(stillOpts(15, 0), { smooth: true })); return !!(f.h2 & 128); }],
  ['a refused dial bakes no bit anywhere', (G) => { const f = flags(G, stillOpts(15, 1, true)); return !(f.any & 128); }],
  ['smooth:false forces nearest on a dial that would be granted', (G) => !(flags(G, Object.assign(stillOpts(15, 1), { smooth: false })).any & 128)],
  ['smooth:true forces the bit on a dial the price refused (the bench arm)', (G) => !!(flags(G, Object.assign(stillOpts(15, 1, true), { smooth: true })).h0 & 128)],
  ['and the report still names the honest verdict for those bytes', (G) => G.bakeClock3(Object.assign(stillOpts(15, 1, true), { smooth: true })).est.smooth === false],
  ['the bit off a turned row is refused at the writer', (G) => { try { G.checkElement(Object.assign(G.blankElement(), { 'class': 3, slot: 0, flags: 128, w: 466, h: 466, n_frames: 1 }), 'e'); return false; } catch (e) { return /smoothing/.test(e.message); } }],
];
/* Each turned row's flags after a bake: h<slot> for hands, any = OR of every row. */
function flags(G, opts) {
  const els = G.verifyBundle3(G.bakeClock3(opts).bytes).sections[0].blob.elements;
  const f = { any: 0 };
  for (const e of els) { f.any |= e.flags; if (e['class'] === 2) f['h' + e.slot] = e.flags; }
  return f;
}

function rung(G, asks, floor) {
  // pickRung3 is private; reach it through a scalar set that yields exactly these asks.
  // The four ask shapes map one-to-one onto rateOf3's inputs.
  const sc = { bg_mode: 0, bg_frames: 0, gif_frames: 0, tourbillon_n: 0, second_sweep: 0, tick_rate: 0, bg_fps: 0, bg_loop: 1 };
  let delay = 0;
  for (const a of asks) {
    if (a.mode === 'fluid') { sc.second_sweep = 1; sc.tick_rate = a.fps; }
    else if (floor === 12) { sc.bg_mode = 1; sc.gif_frames = 8; delay = Math.round(1000 / a.fps); }
    else { sc.tourbillon_n = 8; sc.tourbillon_fps = a.fps; }
  }
  return G.estimate3(fake(sc, delay)).rung;
}
/* The smallest section estimate3 reads: a plate row and nothing that moves. */
function fake(sc, delay) {
  const art = new Uint8Array(4);
  return { art, blob: { scalars: sc, sprites: [{ off: 0, size: 0, w: 0, h: 0 }],
    elements: [{ class: 3, slot: 0, flags: 0, frame0: 0, n_frames: sc.bg_mode === 1 ? 2 : 1, delay, pivot_x: 0, pivot_y: 0 }] } };
}
/* The heavy dial is Station-shaped: a tailed seconds hand, cast shadows, a full glass. */
const HEAVY = [hand(0, 30, 150, 120), hand(1, 26, 200, 170), hand(2, 40, 280, 200)];
const shadowOf = (h) => Object.assign({}, h, { x: h.x + 2, y: h.y + 2 });
const stillOpts = (rate, sweep, heavy) => ({ name: 'Est', serial: 1, bakedMs: 1, plate: PL,
  hands: heavy ? HEAVY : [hand(0, 6, 60, 50), hand(1, 4, 80, 70), hand(2, 2, 90, 80)],
  shadows: heavy ? HEAVY.map(shadowOf) : undefined,
  order: [0, 1, 2], plates: heavy ? [GLASS] : undefined,
  scalars: Object.assign({ second_sweep: sweep, tick_rate: rate }, heavy ? { shadow_on: 1, shadow_dx: 2, shadow_dy: 2 } : {}) });
const memo = new WeakMap();
const cached = (G, k, f) => { let m = memo.get(G); if (!m) memo.set(G, m = {}); return k in m ? m[k] : (m[k] = f()); };
const still = (G, rate, sweep) => cached(G, 's' + rate + sweep, () => G.bakeClock3(stillOpts(rate, sweep)).est);
/* Two wide still hands and a hairline seconds: the AA term costs two rungs of 15, and 13 still clears 12. */
const medium = (G) => cached(G, 'm', () => G.bakeClock3({ name: 'Est', serial: 1, bakedMs: 1, plate: PL,
  hands: [hand(0, 52, 250, 200), hand(1, 52, 250, 200), hand(2, 2, 90, 80)], order: [0, 1, 2],
  scalars: { second_sweep: 1, tick_rate: 15 } }).est);
const heavy = (G) => cached(G, 'h', () => G.bakeClock3(stillOpts(15, 1, true)).est);
const skel = (G) => cached(G, 'k', () => G.estimate3(G.verifyBundle3(new Uint8Array(fs.readFileSync(SKEL))).sections[0]));
const pol = (G, est) => G.splitPolicy3({ frames: 2, fps: 4, unionPx: 0, bboxW: 0, bboxH: 0, otherB: 0, est });

const MUTANTS = [
  ['the fixed te+dma term dropped', 'FIXED_MS: 24', 'FIXED_MS: 0'],
  ['still hands priced by share again', 'var inkK = mover + still,', 'var inkK = mover + still * 0.3,'],
  ['the rung cap ignored', 'return Math.max(1, Math.min(R, Math.round(1000 / ms)));', 'return Math.max(1, Math.round(1000 / ms));'],
  ['fluid asks stop capping', "if (c < cap) cap = c; continue; }", "continue; }"],
  ['the grant threshold loosened', 'SMOOTH_FPS: 12', 'SMOOTH_FPS: 4'],
  ['a granted dial priced nearest', 'var est = smooth ? aaFps : nearFps;', 'var est = nearFps;'],
  ['the sweeping seconds pays AA', "var paysAA = !(e.slot === 2 && sc.second_sweep && c !== CLASS_AUX);", 'var paysAA = true;'],
  ['the glass forgotten', 'glassPx += inkOf3(art, spr[e0.frame0], e0).n / 1000;', 'glassPx += 0;'],
  ['the policy drops the estimate', "r.sentence = r.sentence + '; ' + m.est.sentence;", ''],
  ['the ticking still rides the pick', "if (!anim) return { rate:", "if (false) return { rate:"],
  ['the delta bank seeds the still floor', 'var floor = gif ? 12 : (live ? 4 : 15);', 'var floor = 15;'],
  ['the grant never reaches the rows', 'if (turned && !hot) { er.flags', 'if (false) { er.flags'],
  ['the hot-tick seconds smooths too', 'if (turned && !hot) {', 'if (turned) {'],
  ['the bench force ignored', "var grant = o.smooth === true ? true : (o.smooth === false ? false : est.smooth);", 'var grant = est.smooth;'],
  ['EF_SMOOTH legal on any class', "if ((flags & EF_SMOOTH) && cls !== CLASS_HAND && cls !== CLASS_SPRITE && cls !== CLASS_AUX) {", 'if (false) {'],
];

let checked = 0, fails = 0;
const ok = (c, n, x) => { checked++; console.log((c ? 'PASS ' : 'FAIL ') + n + (x ? '  [' + x + ']' : '')); if (!c) fails++; };
const run = (G) => VECTORS.map(([n, f]) => { try { return [n, !!f(G)]; } catch (e) { return [n, false, e.message]; } });

const G0 = load(SRC);
for (const [n, r, why] of run(G0)) ok(r, n, why);
const sk = skel(G0);
console.log('       Skeleton: ' + sk.sentence);

for (const [name, a, b] of MUTANTS) {
  if (SRC.split(a).length !== 2) { ok(false, 'mutant site present: ' + name, a.slice(0, 60)); continue; }
  let caught;
  try { caught = run(load(SRC.replace(a, b))).filter(([, r]) => !r).map(([n]) => n); }
  catch (e) { caught = ['load: ' + e.message]; }
  ok(caught.length > 0, 'mutant caught: ' + name, caught[0]);
}

console.log(fails ? fails + ' FAILED of ' + checked : 'RESULT: PASS  ' + checked + ' checks');
process.exit(fails ? 1 : 0);
