#!/usr/bin/env node
// plans4.js — estimate4's plans for one baked .orbtheme, as JSON (plan §49.15).
//
// Prints every plan the calculator judged (gears on a hand grid), whether it fits, why not,
// and the frontier and the favour picks — what plan_matrix.py holds the glass against.
//
//   node tools/bench/plans4.js FILE.orbtheme [hands]
'use strict';
const fs = require('fs');
const path = require('path');
global.window = globalThis;
const G = require(path.join(__dirname, '..', 'orb-manager', 'web', 'runtime', 'baker@1', 'orbtheme-baker.js'));
const sec = G.verifyBundle3(new Uint8Array(fs.readFileSync(process.argv[2]))).sections[0];
const e = G.estimate4(sec, process.argv[3] ? { hands: process.argv[3] } : {});
const row = (x) => ({ key: G.planKey4(x), fps: x.fps, gears: x.gears, ok: x.ok, prepaint: x.prepaint, why: x.why,
  frame_ms: x.frame_ms, gear_ms: x.gear_ms, period_ms: x.period_ms, build_ms: x.build_ms, slack_ms: x.slack_ms, swap_ms: x.swap_ms });
console.log(JSON.stringify({
  mode: e.hand_mode, carriage: e.carriage, bank_fps: e.bank_fps, rect_kpx: e.rect_kpx,
  plans: e.plans.map(row), picks: e.picks && Object.fromEntries(Object.entries(e.picks).map(([k, v]) => [k, G.planKey4(v)])),
  judged: e.policy.map(row),
}));
