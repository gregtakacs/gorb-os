#!/usr/bin/env node
// aa_fit.js — fit estimate4's smoothing term (EST4.AA) to the seconds/shadow AA bench (§49.15).
//
// Re-judges each bench arm with the smooth flags it ran under, and prints the mean frame
// price per arm against the device's commit, then the AA coefficient that fits the deltas.
//
//   node tools/bench/aa_fit.js BAKES AA.json [AA.json ...]
'use strict';
const fs = require('fs');
const path = require('path');
global.window = globalThis;
const G = require(path.join(__dirname, '..', 'orb-manager', 'web', 'runtime', 'baker@1', 'orbtheme-baker.js'));
const [bakes, ...runs] = process.argv.slice(2);
const EF_SMOOTH = 128, SHADOW = 16;            // AUX_F_SHADOW3; rows are CLASS_AUX (8)
const meas = {};
runs.forEach((f) => Object.assign(meas, JSON.parse(fs.readFileSync(f, 'utf8'))));
// arm key -> [preset, plan, seconds smooth, H/M shadows smooth, seconds shadow smooth]
const arm = (k) => {
  const m = k.match(/^(\w+) (\S+) aa(\d)(?: (\w+))?$/); const aa = +m[3], sh = m[4] || 'nearest';
  return [m[1], m[2], aa >= 1, sh === 'baked', aa >= 2];
};
const judgeArm = (pid, key, sSec, sHM, sSecSh, AA) => {
  const sec = G.verifyBundle3(new Uint8Array(fs.readFileSync(path.join(bakes, pid + '.orbtheme')))).sections[0];
  let parent = null;
  for (const e of sec.blob.elements) {
    if (e['class'] !== 8) continue;
    const twin = !!(e.flags & SHADOW) && parent;
    const src = twin ? parent.src : e.src;
    const isSec = src === 7, isHM = src === 4 || src === 5 || src === 6;   // SOURCES: hour12, hour24, minute
    e.flags &= ~EF_SMOOTH;
    if (!twin && isSec && sSec) e.flags |= EF_SMOOTH;
    if (!twin && isHM) e.flags |= EF_SMOOTH;
    if (twin && isHM && sHM) e.flags |= EF_SMOOTH;
    if (twin && isSec && sSecSh) e.flags |= EF_SMOOTH;
    if (!twin) parent = e;
  }
  const keep = G.EST4.AA; G.EST4.AA = AA;
  try {
    const [g, h] = key.split('/').map(Number);
    return G.estimate4(sec).policy.find((x) => x.fps === h && x.gears === g);
  } finally { G.EST4.AA = keep; }
};
const rows = Object.keys(meas).map((k) => {
  const [pid, key, a, b, c] = arm(k), r = meas[k];
  const d0 = judgeArm(pid, key, a, b, c, 0), d1 = judgeArm(pid, key, a, b, c, 1);
  return { k, pid, key, flags: [a, b, c], meas: (r.per_commit.hook_us + r.per_commit.dma_us) / 1000, c0: d0.mean_ms, dS: d1.mean_ms - d0.mean_ms };
});
// AA from the deltas against each arm's own all-nearest twin
let num = 0, den = 0;
for (const r of rows) {
  const base = rows.find((q) => q.pid === r.pid && q.key === r.key && !q.flags[0] && !q.flags[1] && !q.flags[2]) ||
               rows.find((q) => q.pid === r.pid && q.key === r.key && q.flags[0] && !q.flags[1] && !q.flags[2] && r !== q && r.flags.join() !== q.flags.join() && q.k < r.k);
  if (!base || base === r) continue;
  const dm = r.meas - base.meas, dc = r.dS - base.dS;
  num += dm * dc; den += dc * dc;
  r.vs = base.k; r.dm = dm; r.dc = dc;
}
const AA = den ? num / den : 0;
for (const r of rows) console.log(r.k.padEnd(30), 'device ' + r.meas.toFixed(1) + ' ms', ' calc(AA=0) ' + r.c0, ' smooth px term ' + r.dS.toFixed(2),
  r.vs ? ' vs ' + r.vs + ': device +' + r.dm.toFixed(1) + ' calc +' + (r.dc * AA).toFixed(1) : '');
console.log('AA =', AA.toFixed(3), 'ms per smoothed kpx walked');
