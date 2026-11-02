#!/usr/bin/env node
// D43 P5d.3c legibility audit: every piece of text each preset sets, at the px it lands on the 1.75 in
// panel, against WatchEngine.LEGIBLE_PX (a 1.6 mm cap height: read from 2-3 ft). Exit 1 if any
// text is set below it. `node headless-legible.test.js [--all]` lists the offenders by where they are drawn.
'use strict';
require('./headless-stub.js');
const E = global.WatchEngine, ALL = process.argv.includes('--all');
const O = { finish: 'brass', train: 'classic', frames: 4, tick: 'sweep', beats: 1 };
let bad = 0;
for (const key of Object.keys(E.PRESETS)) {
  const P = E.PRESETS[key];
  global.__TEXT_AUDIT = [];
  const o = Object.assign({}, O, P, P.cfg || {}, { design: E.canonDesign(P.design) });
  const f = E.face(o);
  f.comps(new Date(2026, 10, 2, 10, 9, 36), null);
  if (f.dateFlares) f.dateFlares(new Date(2026, 10, 2, 10, 9, 36));
  const rec = global.__TEXT_AUDIT; global.__TEXT_AUDIT = null;
  const small = rec.filter((r) => r.px > 0 && r.px < E.LEGIBLE_PX - 0.01);
  const fn = (r) => (r.at.match(/at (\S+)/) || [])[1] || '?';
  const by = {}; small.forEach((r) => { const k = fn(r); (by[k] = by[k] || []).push(r); });
  const min = rec.length ? Math.min(...rec.filter((r) => r.px > 0).map((r) => r.px)) : 0;
  console.log((small.length ? 'FAIL ' : 'ok   ') + key.padEnd(10) + ' ' + rec.length + ' runs, smallest ' + min.toFixed(1) + ' px' + (small.length ? ', ' + small.length + ' under ' + E.LEGIBLE_PX : ''));
  if (small.length && ALL) Object.keys(by).forEach((k) => console.log('       ' + k + ': ' + by[k].length + ' e.g. ' + by[k].slice(0, 4).map((r) => JSON.stringify(r.text) + '@' + r.px.toFixed(1)).join(' ')));
  bad += small.length;
}
// every option a design can reach, not only the presets: each print and perimeter ring, each
// sub-dial style and scale (round, fan and slider), a window of each kind at its smallest, and
// text flares at their smallest
const audit = (name, design) => {
  global.__TEXT_AUDIT = [];
  const f = E.face(Object.assign({}, O, { design: E.canonDesign(design) }));
  f.comps(new Date(2026, 10, 2, 10, 9, 36), null); if (f.dateFlares) f.dateFlares(new Date(2026, 10, 2, 10, 9, 36));
  const small = global.__TEXT_AUDIT.filter((r) => r.px > 0 && r.px < E.LEGIBLE_PX - 0.01); global.__TEXT_AUDIT = null;
  if (small.length) { bad += small.length; console.log('FAIL ' + name + ': ' + small.slice(0, 4).map((r) => JSON.stringify(r.text) + '@' + r.px.toFixed(1) + ' ' + ((r.at.match(/at (\S+)/) || [])[1] || '')).join(' ')); }
  return small.length;
};
let opts = 0;
Object.keys(E.PRINTS).forEach((pr) => { opts++; audit('print ' + pr, { print: pr, numerals: 'all' }); });
Object.keys(E.PERIMS || {}).forEach((pe) => { opts++; audit('perim ' + pe, { print: 'roman', perim: pe }); });
Object.keys(E.SUB_DISPLAYS).forEach((st) => Object.keys(E.SCALES).forEach((sc) => ['round', 'fan', 'slider'].forEach((fm) => {
  opts++; audit('sub-dial ' + st + '/' + sc + '/' + fm, { print: 'none', comps: [{ id: 's', type: 'subdial', host: 'dial', angle: 180, dist: 100, size: 40, scale: sc, display: st, form: fm }] });
})));
['date', 'day', 'month', 'custom'].forEach((sh) => { opts++; audit('window ' + sh, { print: 'none', comps: [{ id: 'w', type: 'window', host: 'dial', angle: 90, dist: 150, size: 0.6, shows: sh }] }); });
['text', 'arctext', 'datetext'].forEach((ic) => { opts++; audit('flare ' + ic, { print: 'none', comps: [{ id: 'f', type: 'flare', host: 'dial', angle: 0, dist: 100, size: 4, icon: ic, text: 'BRAND' }] }); });
console.log(opts + ' options audited');
console.log(bad ? 'RESULT: ' + bad + ' text runs under ' + E.LEGIBLE_PX + ' px' : 'every preset reads at 2-3 ft');
process.exit(bad ? 1 : 0);
