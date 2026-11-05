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
ok(e.gears_fps === 4 && e.bank_fps === 4, 'the gears ride their own 4 fps', e.gears_fps + ' / ' + e.bank_fps);
ok(e.est_fps === 4 && !e.prepaint, 'it asks 4 and gets 4, direct', e.sentence);
// measured: a gear frame ~71 ms of compositing + ~9.6 dma (§49.4: hook 71.3, dma 9.6)
ok(Math.abs(opt(e, 12).gear_ms - 81) / 81 < 0.15, 'the gear tick prices within 15% of the bench', opt(e, 12).gear_ms + ' ms vs 81');
// measured: 4 on 12 holds 5 scans 140 of 140 with pre-paint, 86 of 130 without (§49.9, §49.11)
ok(opt(e, 12).ok && opt(e, 12).prepaint, '4 on 12 fits, pre-painted', JSON.stringify(opt(e, 12)));
// measured: 4 on 20 holds 3 scans 224 of 229 pre-painted with the in-place landing (§49.11)
ok(opt(e, 20).ok && opt(e, 20).prepaint, '4 on 20 fits, pre-painted', JSON.stringify(opt(e, 20)));
// measured: 5 on 15 is not a plan here — the gears are 4, and 4 does not land on every Nth of 15
ok(!opt(e, 15).ok, '15 is refused: 4 fps gears do not divide it', opt(e, 15).why);
ok(e.max_fps === 20 && e.max_prepaint, 'the fastest sweep it carries is 20, pre-painted', e.max_fps + ' ' + e.max_prepaint);
ok(/est 4 fps on the 60 Hz grid/.test(e.sentence) && /could run 20 \(pre-painted\)/.test(e.sentence), 'the sentence says both', e.sentence);

// ---- the general walk: any mix of seconds hands. A second seconds hand ticking 1/4
// beside Skeleton's sweeping one: 4 on 20 still holds (its beat lands every 5th frame),
// 5 on 15 cannot (4 does not divide 15), and the ticking hand is a mover of its own.
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
ok(mp(20, 4).ok, 'with both, 4 on 20 still fits', JSON.stringify(mp(20, 4)));
ok(!mp(15, 5).ok && /1\/4 hand does not land/.test(mp(15, 5).why), 'with both, 5 on 15 is refused for the 1/4 beat', mp(15, 5).why);
// ---- the slot: a one-second bank costs the keyframe plus G rect deltas, fps x cutout area
ok(e.bytes_at(20) > e.bytes_at(12) && e.bytes_at(12) > e.bytes_at(4) && e.bytes_at(4) > 0, 'a faster bank costs more bytes',
   [4, 12, 20].map((g) => e.bytes_at(g)).join(' '));
// a loop is never under 8 frames: 4 fps holds 8 (two seconds), 12 holds 12 (one second)
ok(e.frames_at(4) === 8 && e.frames_at(7.5) === 8 && e.frames_at(12) === 12 && e.frames_at(15) === 15,
   'a bank holds max(8, G) frames', [4, 7.5, 12, 15, 20].map((g) => e.frames_at(g)).join(' '));
// ---- the plans (§49.15): searched by fps x cutout area, not tabled by hand mode
// Skeleton's big open train: the sweep at 20 under gears at 4, 5 on 15, or gears at 7.5 on 7.5
// (measured on Gold, §49.15: 10/10 holds 37% of its frames, so it is off the frontier)
const P = (r) => r.plans.map(G.planKey4).join(' ');
ok(P(e) === '4/20 5/15 7.5/7.5', 'Skeleton sweeps on the frontier 4/20, 5/15, 7.5/7.5', P(e));
ok(e.picks.sweep && G.planKey4(e.picks.sweep) === '4/20' && G.planKey4(e.picks.gears) === '7.5/7.5' && G.planKey4(e.picks.balanced) === '5/15',
   'favour sweep 4/20, gears 7.5/7.5, balanced 5/15', JSON.stringify(Object.keys(e.picks).map((k) => k + ' ' + G.planKey4(e.picks[k]))));
ok(e.plans.every((x) => !e.plans.some((y) => y !== x && y.fps >= x.fps && y.gears >= x.gears)), 'no plan is beaten on both the hands and the gears');
ok(e.policy.filter((x) => x.ok).every((x) => e.plans.some((y) => y.fps >= x.fps && y.gears >= x.gears)), 'every plan that fits is on or under the frontier');
ok(G.planKey4(G.pickPlan4(e, 'sweep', '5/15')) === '5/15' && G.planKey4(G.pickPlan4(e, 'gears', '6/12')) === '7.5/7.5',
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
ok(mutate('MARGIN', 0, () => est().policy.find((x) => x.fps === 10 && x.gears === 10).ok),
   'mutant: no margin would call 10/10 a fit (the glass holds 37% of its frames)');
ok(mutate('CAR', 0.676, () => true) && G.EST4.CAR === 0.9, 'the carriage term is the §49.15 refit (Kitetour 6/6, Orbital 5/5 miss on the glass)');
ok(mutate('SWAP', 0.2, () => !opt(est(), 20).ok), 'mutant: the copying swap (0.2 us/px) would refuse 4 on 20');
ok(mutate('R', 0.2, () => Math.abs(opt(est(), 12).gear_ms - 81) / 81 >= 0.15), 'mutant: a cheaper restore misprices the gear tick');

console.log(fails ? fails + ' of ' + checked + ' checks FAILED' : 'all ' + checked + ' checks pass');
process.exit(fails ? 1 : 0);
