#!/usr/bin/env node
// baker@1 estimate4 (app-platform-plan §49.10): the ink-span compositor's price of a clock
// bake on the 60 Hz grid, read off the hub's shipped Skeleton bundle and held to what the
// bench measured on Gold (§49.4, §49.9): its gear tick, the plan it asks for, the fastest
// sweep it can carry and whether that needs pre-paint. Each pin is also held against a
// mutant of the constant it rests on, so a pin that cannot fail is caught.
//
//   node headless-estimate4.test.js
'use strict';
const fs = require('fs');
const path = require('path');
let fails = 0, checked = 0;
const ok = (c, n, x) => { checked++; if (!c || process.env.VERBOSE) console.log((c ? 'PASS ' : 'FAIL ') + n + (x ? '  [' + x + ']' : '')); if (!c) fails++; };

global.window = globalThis;
const G = require(path.join(__dirname, '..', 'baker@1', 'orbtheme-baker.js'));
const SKEL = path.join(__dirname, '..', '..', '..', 'themes', 'skeleton.orbtheme');
const sec = G.verifyBundle3(new Uint8Array(fs.readFileSync(SKEL))).sections[0];
const est = () => G.estimate4(sec);
const keep = Object.assign({}, G.EST4);
const mutate = (k, v, fn) => { G.EST4[k] = v; try { return fn(); } finally { Object.assign(G.EST4, keep); } };

const e = est();
const opt = (r, H) => r.options.find((o) => o.fps === H);
ok(typeof G.estimate4 === 'function' && e && e.options.length, 'estimate4 is exported and answers');

// ---- the grid: only rates a 60 Hz panel holds alike, and 8 as 7/8 scans
ok(e.options.every((o) => Math.abs(o.fps * o.scans - 60) < 0.6), 'every option is a whole number of scans a frame',
   e.options.map((o) => o.fps + 'x' + o.scans).join(' '));
ok(e.options.every((o) => !o.ok || !o.gears || Math.abs(o.fps / o.gears - Math.round(o.fps / o.gears)) < 1e-6),
   'a gear frame always lands on every Nth frame of a plan that fits');

// ---- Skeleton, against the bench (Gold, 80 MHz, spans + nearest shadows)
ok(e.gears_fps === 6 && Math.round(e.bank_fps) === 6, 'the gears ride their own 6 fps (the plan redrew the bank at it)', e.gears_fps + ' / ' + e.bank_fps);
ok(e.est_fps === 6 && !e.prepaint, 'it asks its plan\'s 6 and gets 6, direct', e.sentence);
// measured (§49.15, 6/6 for 60 s, the shadows as shipped): a gear frame 92.1 ms of compositing
// + 9.5 dma, the dearest 103.1. A gear frame that must fit on its own is priced on the bank's
// largest rect, so it sits above the glass's dearest, never under it.
const GEAR_MAX = 103.1, gearFair = (ms) => ms >= GEAR_MAX && ms < GEAR_MAX * 1.3;
ok(gearFair(opt(e, 6).gear_ms), 'the gear tick prices above the glass\'s dearest, within 30%', opt(e, 6).gear_ms + ' ms vs ' + GEAR_MAX);
// measured: 6 on 6 holds 100% of its frames, every frame a gear frame (§49.15)
ok(opt(e, 6).ok && !opt(e, 6).prepaint, '6 on 6 fits, direct', JSON.stringify(opt(e, 6)));
// measured: 4 on 12 pre-painted holds 93.5-94.4%, 4 on 20 88.2%: the frames between are short
// of 1.3x the build, and both are refused (§49.15)
const pl = (r, H, G4) => r.policy.find((x) => x.fps === H && x.gears === G4);
ok(!pl(e, 12, 4).ok && /to build a/.test(pl(e, 12, 4).why), '4 on 12 is refused: the frames between are short of the build', pl(e, 12, 4).why);
ok(!pl(e, 20, 4).ok && /to build a/.test(pl(e, 20, 4).why), '4 on 20 is refused: the frames between are short of the build', pl(e, 20, 4).why);
// measured: 5 on 15 is not a plan here — the gears are 4, and 4 does not land on every Nth of 15
ok(!opt(e, 15).ok, '15 is refused: 4 fps gears do not divide it', opt(e, 15).why);
ok(e.max_fps === 6 && !e.max_prepaint, 'the fastest sweep it carries is 6, direct', e.max_fps + ' ' + e.max_prepaint);
ok(/est 6 fps on the 60 Hz grid/.test(e.sentence) && /plan: gears 6 on 6/.test(e.sentence), 'the sentence says the rate and the plan', e.sentence);

// ---- the general walk: any mix of seconds hands. A second seconds hand ticking 1/4
// beside Skeleton's sweeping one: 4 on 4 holds (its beat lands every frame), 6 on 6 cannot
// (4 does not divide 6), and the ticking hand is a mover of its own.
const mixed = (() => {
  const m = G.verifyBundle3(new Uint8Array(fs.readFileSync(SKEL))).sections[0];
  const els = m.blob.elements, isec = els.findIndex((x) => x['class'] === 8 && x.src === 7 && !(x.flags & 16));
  const tick = Object.assign({}, els[isec], { slot: 30, cadence: 2, cad_arg: 4 });
  els.push(tick);
  return G.estimate4(m);
})();
ok(mixed.movers.some((x) => x.kind === 'step' && x.beats === 4) && mixed.movers.some((x) => x.kind === 'sweep'),
   'a ticking and a sweeping seconds hand are both movers', JSON.stringify(mixed.movers));
const mp = (fps, gears) => mixed.policy.find((x) => x.fps === fps && x.gears === gears);
ok(mp(4, 4).ok && G.planKey4(mixed.plans[0]) === '4/4', 'with both, 4 on 4 is the plan', JSON.stringify(mp(4, 4)));
ok(!mp(6, 6).ok && /1\/4 hand does not land/.test(mp(6, 6).why), 'with both, 6 on 6 is refused for the 1/4 beat', mp(6, 6).why);
ok(!mp(15, 5).ok && /1\/4 hand does not land/.test(mp(15, 5).why), 'with both, 5 on 15 is refused for the 1/4 beat', mp(15, 5).why);
// ---- the slot: a one-second bank costs the keyframe plus G rect deltas, fps x cutout area
ok(e.bytes_at(20) > e.bytes_at(12) && e.bytes_at(12) > e.bytes_at(4) && e.bytes_at(4) > 0, 'a faster bank costs more bytes',
   [4, 12, 20].map((g) => e.bytes_at(g)).join(' '));
// a loop is never under 8 frames: 4 fps holds 8 (two seconds), 12 holds 12 (one second)
ok(e.frames_at(4) === 8 && e.frames_at(7.5) === 8 && e.frames_at(12) === 12 && e.frames_at(15) === 15,
   'a bank holds max(8, G) frames', [4, 7.5, 12, 15, 20].map((g) => e.frames_at(g)).join(' '));
// ---- the plans (§49.15): searched by fps x cutout area, not tabled by hand mode
// Skeleton's big open train, its smoothed hour and minute shadows as shipped: one plan, 6 on 6
// (measured on Gold, §49.15: 6/6 holds 100% for 60 s, 7.5/7.5 57.6%, 4/12 93.5%)
const P = (r) => r.plans.map(G.planKey4).join(' ');
ok(P(e) === '6/6', 'Skeleton sweeps on the frontier 6/6 alone', P(e));
ok(e.picks.sweep && ['sweep', 'gears', 'balanced'].every((k) => G.planKey4(e.picks[k]) === '6/6'),
   'every favour picks 6/6', JSON.stringify(Object.keys(e.picks).map((k) => k + ' ' + G.planKey4(e.picks[k]))));
// the plan reaches the device: the sweeping seconds row names the plan's hand grid as its
// rate (cad_arg), which orb_clock3.c makes the beat the scheduler paces (§49.15, v1.2.9)
const secRow = sec.blob.elements.find((x) => x['class'] === 8 && x.src === 7 && x.cadence === 1);
ok(secRow && secRow.cad_arg === e.picks.sweep.fps, 'the shipped seconds row sweeps at the plan\'s grid',
   (secRow && secRow.cad_arg) + ' vs ' + e.picks.sweep.fps);
ok(e.plans.every((x) => !e.plans.some((y) => y !== x && y.fps >= x.fps && y.gears >= x.gears)), 'no plan is beaten on both the hands and the gears');
ok(e.policy.filter((x) => x.ok).every((x) => e.plans.some((y) => y.fps >= x.fps && y.gears >= x.gears)), 'every plan that fits is on or under the frontier');
ok(G.planKey4(G.pickPlan4(e, 'gears', '6/6')) === '6/6' && G.planKey4(G.pickPlan4(e, 'sweep', '4/12')) === '6/6',
   'an exact plan is honoured while it is on the frontier, else the favour answers');
// a small cutout: the same dial with a 40x40 bank rect runs its gears every frame at 20
const small = (() => {
  const m = G.verifyBundle3(new Uint8Array(fs.readFileSync(SKEL))).sections[0];
  const pl = m.blob.elements.find((x) => x.n_frames > 1 && m.blob.sprites[x.frame0].w === 466);   // the plate's delta bank
  for (let i = 1; i < pl.n_frames; i++) { const sp = m.blob.sprites[pl.frame0 + i]; sp.w = 40; sp.h = 40; sp.diffx = 300; sp.diffy = 300; }
  return m;
})();
const es = G.estimate4(small);
ok(es.picks && G.planKey4(es.picks.sweep) === '20/20', 'a small cutout runs its gears at 20 with the sweep at 20', P(es));
ok(es.frames_at(20) === 20 && es.policy.find((x) => x.fps === 20 && x.gears === 20).ok, 'its 20-frame bank fits the device (CLK_BG_MAX 20)');
ok(mutate('BANK_MAX', 16, () => /device holds 16/.test(G.estimate4(small).policy.find((x) => x.fps === 20 && x.gears === 20).why)),
   'mutant: the old 16-frame cap refuses the 20 fps bank');
// ticking: the hand moves on its beats whatever the grid, so the plan is the fastest gears
const t1 = G.estimate4(sec, { hands: '1' });
ok(t1.plans.length === 1 && t1.plans[0].gears === 6 && t1.plans[0].fps === 6, 'ticking 1/s, Skeleton\'s one plan is gears 6 on 6 (a 1/s beat does not land on 7.5)', P(t1));
const t4 = G.estimate4(sec, { hands: '4' });
ok(t4.plans.length === 1 && t4.plans[0].fps % 4 === 0, 'ticking 4/s, the plan\'s grid carries the beat', P(t4));
ok(mutate('PX_B', 40, () => /the slot leaves/.test(G.estimate4(sec, { gears: 15 }).options.find((x) => x.fps === 15).why)),
   'mutant: a fat delta (40 B/px) would not fit 15 fps gears in the slot');

// ---- each pin bites: the constants it rests on, broken one at a time
// The terms are the §49.15 fit on the shipped-shadow runs (31 direct plans, rms 2.6 ms), and a
// frame needs its price x 1.15 + 20 ms: every frontier plan on every preset held for a minute
// and every plan one step past it missed.
ok(G.EST4.AA === 1.277 && G.EST4.MARGIN === 20 && G.EST4.SLOP === 0.15 && G.EST4.IDLE === 26 && G.EST4.PREP_K === 1.3,
   'the smoothing, margin, slop, idle and build terms are the §49.15 fit');
const fitsAt = (H, G4) => est().policy.find((x) => x.fps === H && x.gears === G4).ok;
ok(mutate('SLOP', 0, () => mutate('MARGIN', 14, () => fitsAt(7.5, 7.5))),
   'mutant: a flat 14 ms margin would call 7.5/7.5 a fit (the glass holds 57.6%)');
ok(mutate('SLOP', 0.5, () => !fitsAt(6, 6)), 'mutant: a 50% slop would refuse 6/6 (the glass holds 100%)');
ok(mutate('PREP_K', 0.9, () => pl(est(), 12, 4).ok), 'mutant: without the build factor 4 on 12 would pre-paint (the glass holds 93.5%)');
ok(mutate('R', 0.2, () => !gearFair(opt(est(), 6).gear_ms)), 'mutant: a cheaper restore misprices the gear tick');

console.log(fails ? fails + ' of ' + checked + ' checks FAILED' : 'all ' + checked + ' checks pass');
process.exit(fails ? 1 : 0);
