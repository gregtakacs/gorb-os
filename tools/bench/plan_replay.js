#!/usr/bin/env node
// plan_replay.js — estimate4's verdicts against a plan_matrix.py run, offline (plan §49.15).
//
// Re-judges every plan the glass ran, with the calculator as it stands now (and any EST4
// override given as K=V), and prints where the calculator and the glass disagree.
//
//   node tools/bench/plan_replay.js BAKES RUN.json [MARGIN=23 CAR=0.9 ...]
'use strict';
const fs = require('fs');
const path = require('path');
global.window = globalThis;
const G = require(path.join(__dirname, '..', 'orb-manager', 'web', 'runtime', 'baker@1', 'orbtheme-baker.js'));
const [bakes, run, ...kv] = process.argv.slice(2);
kv.forEach((x) => { const [k, v] = x.split('='); G.EST4[k] = +v; });
const res = JSON.parse(fs.readFileSync(run, 'utf8'));
let agree = 0, n = 0;
for (const [pid, rec] of Object.entries(res)) {
  const sec = G.verifyBundle3(new Uint8Array(fs.readFileSync(path.join(bakes, pid + '.orbtheme')))).sections[0];
  const e = G.estimate4(sec);
  for (const a of rec.arms) {
    const [g, h] = a.key.split('/').map(Number);
    if (h <= 1) continue;                     // a beat-only dial: the grid is the beat, nothing to hold
    const j = e.policy.find((x) => x.fps === h && x.gears === g);
    if (!j) continue;
    n++; const same = j.ok === a.glass_ok; agree += same;
    console.log((same ? '   ' : '!! ') + pid.padEnd(10) + a.key.padEnd(8) + ' calc ' + (j.ok ? 'fits' : 'no  ') + ' glass ' + (a.glass_ok ? 'held' : 'miss') +
      '  ' + (a.at_nominal * 100).toFixed(1) + '%  frame ' + j.frame_ms + ' gear ' + j.gear_ms + ' period ' + j.period_ms + (j.why ? '  ' + j.why : ''));
  }
}
console.log(agree + ' of ' + n + ' agree');
