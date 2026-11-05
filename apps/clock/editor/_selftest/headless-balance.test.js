#!/usr/bin/env node
/* headless-balance.test.js — the balance wheel's pace across the gear tempos (plan §49.14).
 *
 * Owner, 2026-11-05: a gear loop is never under 8 frames (4 fps: two seconds), and the
 * balance must still look right at 4 fps. So the balance swings BEATS times a SECOND, not per
 * loop: a two-second loop swings twice as often per loop as a one-second one. A bank too
 * shallow for the swing is capped at a tick-tock (two frames a swing), phased onto the
 * extremes so no frame lands the wheel dead centre. Loads the REAL dial-renderer.js (its
 * configure and balanceSwings need no canvas) — the headless stub stands in for it elsewhere.
 *
 * Run: node apps/clock/editor/_selftest/headless-balance.test.js
 */
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');

let fails = 0, checked = 0;
const ok = (c, n, x) => { checked++; console.log((c ? 'PASS ' : 'FAIL ') + n + (x ? '  [' + x + ']' : '')); if (!c) fails++; };

const ctx = { window: {}, Math, console, document: { createElement: () => ({ getContext: () => new Proxy({}, { get: () => () => ({ addColorStop() {} }) }) }) } };
ctx.self = ctx.window;
vm.createContext(ctx);
vm.runInContext(fs.readFileSync(path.join(__dirname, '..', 'dial-renderer.js'), 'utf8'), ctx);
const D = ctx.window.DialRenderer;

// the banks: a loop is never under 8 frames, and one second at every tempo past 8
ok(D.bankFrames(4) === 8 && D.bankFrames(8) === 8 && D.bankFrames(12) === 12 && D.bankFrames(15) === 15 && D.bankFrames(20) === 20,
   'a bank holds max(8, fps) frames', [4, 8, 12, 15, 20].map((f) => f + ':' + D.bankFrames(f)).join(' '));

const pace = (fps, beats) => {
  const n = D.bankFrames(fps);
  D._.configure({ frames: n, tempo: fps, beats });
  const sw = D.balanceSwings(), ph = n / sw <= 2.5 ? Math.PI / 2 : 0;
  const poses = Array.from({ length: n }, (_, k) => Math.sin(2 * Math.PI * (k / n) * sw + ph));
  return { n, sw, perS: sw / (n / fps), frozen: poses.every((v) => Math.abs(v) < 1e-6), distinct: new Set(poses.map((v) => v.toFixed(3))).size };
};
// the pace per second holds across tempos wherever the bank can carry it
for (const beats of [1, 2, 4]) {
  for (const fps of [8, 12, 15, 20]) {
    const p = pace(fps, beats);
    ok(Math.abs(p.perS - beats) < 1e-6 && !p.frozen, beats + ' swing(s)/s at ' + fps + ' fps', 'per s ' + p.perS + ', ' + p.distinct + ' poses');
  }
}
// 4 fps: a two-second loop swings per SECOND, not per loop (the owner's "sped up at 4 fps")
const p41 = pace(4, 1);
ok(p41.sw === 2 && p41.perS === 1 && !p41.frozen, 'at 4 fps one swing a second is two per two-second loop', JSON.stringify(p41));
// 4 fps cannot show 4 swings a second in 8 frames: a tick-tock, extreme to extreme, never centre
const p44 = pace(4, 4);
ok(p44.sw === 4 && p44.distinct === 2 && !p44.frozen, 'at 4 fps a fast balance is a tick-tock, never frozen', JSON.stringify(p44));
// the mutant the phase exists for: a tick-tock sampled through the centre freezes
{
  const n = 8, sw = 4, poses = Array.from({ length: n }, (_, k) => Math.sin(2 * Math.PI * (k / n) * sw));
  ok(poses.every((v) => Math.abs(v) < 1e-6), 'mutant: without the quarter-turn phase, the tick-tock freezes at centre');
}

console.log(fails ? fails + ' of ' + checked + ' checks FAILED' : 'all green (' + checked + ' checks)');
process.exit(fails ? 1 : 0);
