#!/usr/bin/env node
// est4_fit.js — solve estimate4's frame-cost coefficients against est4_calib.py (plan §49.15).
//
//   commit_ms = R x restored kpx + W x walked kpx + AA x smoothed kpx + CAR x restored kpx (carriage) + FIXED
//
// The terms are each plan's mean frame, as estimate4 computes them from the baked bytes; the
// device's mean commit (hook + DMA) per frame is what they are fit to, by least squares.
//
//   node tools/bench/est4_fit.js BAKES CALIB.json
'use strict';
const fs = require('fs');
const path = require('path');
global.window = globalThis;
const G = require(path.join(__dirname, '..', 'orb-manager', 'web', 'runtime', 'baker@1', 'orbtheme-baker.js'));
const [bakes, run] = process.argv.slice(2);
const calib = JSON.parse(fs.readFileSync(run, 'utf8'));
const X = [], y = [], tag = [];
for (const [pid, arms] of Object.entries(calib)) {
  const sec = G.verifyBundle3(new Uint8Array(fs.readFileSync(path.join(bakes, pid + '.orbtheme')))).sections[0];
  const e = G.estimate4(sec), es = G.estimate4(sec, { hands: 'sweep' });
  for (const [key, r] of Object.entries(arms)) {
    const forced = key.endsWith('s'), [g, h] = key.replace(/s$/, '').split('/').map(Number);
    const ee = forced ? es : e;                         // "s": the seconds were forced to sweep on the glass
    const j = ee.policy.find((x) => x.fps === h && x.gears === g) || G.estimate4(sec, { gears: g || undefined }).options.find((x) => x.fps === h);
    if (!j || !j.terms) continue;
    const t = j.terms, car = e.carriage ? t.R : 0;
    X.push([t.R, t.W, t.S, car, 1]); y.push((r.per_commit.hook_us + r.per_commit.dma_us) / 1000); tag.push(pid + ' ' + key);
  }
}
// normal equations, 5x5
const n = 5, A = Array.from({ length: n }, () => new Array(n + 1).fill(0));
X.forEach((x, k) => { for (let i = 0; i < n; i++) { for (let j = 0; j < n; j++) A[i][j] += x[i] * x[j]; A[i][n] += x[i] * y[k]; } });
// a dataset with no carriage arm leaves that column zero: pin it to the current value
for (let i = 0; i < n; i++) if (A[i][i] === 0) { A[i][i] = 1; A[i][n] = [G.EST4.R, G.EST4.W, G.EST4.AA, G.EST4.CAR, G.EST4.FIXED][i]; }
for (let c = 0; c < n; c++) {
  let p = c; for (let r = c + 1; r < n; r++) if (Math.abs(A[r][c]) > Math.abs(A[p][c])) p = r;
  [A[c], A[p]] = [A[p], A[c]];
  for (let r = 0; r < n; r++) if (r !== c) { const f = A[r][c] / A[c][c]; for (let k = c; k <= n; k++) A[r][k] -= f * A[c][k]; }
}
const b = A.map((row, i) => row[n] / row[i]);
const names = ['R', 'W', 'AA', 'CAR', 'FIXED'];
let ss = 0;
X.forEach((x, k) => { const p = x.reduce((a, v, i) => a + v * b[i], 0), d = p - y[k]; ss += d * d;
  console.log(tag[k].padEnd(22), 'device ' + y[k].toFixed(1).padStart(6), ' fit ' + p.toFixed(1).padStart(6), ' err ' + d.toFixed(1).padStart(6), '  terms', x.slice(0, 4).map((v) => v.toFixed(1)).join(' ')); });
console.log(names.map((nm, i) => nm + ' ' + b[i].toFixed(3)).join('  '), '  rms', Math.sqrt(ss / X.length).toFixed(2), 'ms over', X.length, 'arms');
