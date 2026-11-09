#!/usr/bin/env node
// theme@1/measure (theme-editor-kit-plan K4): what a design costs the glass, with no app
// around it. The motion walk held to the clock's pre-K4 walk over synthetic frames, the
// baker's estimate read off the hub's shipped Skeleton bundle, and the fit verdict for an
// app with its own pace model, one with only the estimate, and one with neither.
//
//   node headless-measure.test.js
'use strict';
const fs = require('fs');
const path = require('path');
let fails = 0, checked = 0;
const ok = (c, n, x) => { checked++; if (!c || process.env.VERBOSE) console.log((c ? 'PASS ' : 'FAIL ') + n + (x ? '  [' + x + ']' : '')); if (!c) fails++; };

global.window = globalThis;
global.GorbBake = require(path.join(__dirname, '..', 'baker@1', 'orbtheme-baker.js'));
require(path.join(__dirname, '..', 'theme@1', 'measure.js'));
const T = OrbTheme, W = 466, N = W * W;

// ---- the motion walk, against the clock's own walk as it was at 37dc1da
const before = (pxs) => {
  const mask = new Uint8Array(N); let bx = N, by = N, bx2 = -1, by2 = -1, moved = 0;
  for (let f = 0; f < pxs.length; f++) {
    const a = pxs[f], b = pxs[(f + 1) % pxs.length];
    for (let p = 0, i = 0; p < N; p++, i += 4) if (a[i] !== b[i] || a[i + 1] !== b[i + 1] || a[i + 2] !== b[i + 2]) {
      moved++; if (!mask[p]) { mask[p] = 1; const x = p % W, y = (p / W) | 0; if (x < bx) bx = x; if (x > bx2) bx2 = x; if (y < by) by = y; if (y > by2) by2 = y; } }
  }
  let un = 0; for (let p = 0; p < N; p++) if (mask[p]) un++;
  return { unionPx: un, movedPx: moved, bboxW: moved ? bx2 - bx + 1 : 0, bboxH: moved ? by2 - by + 1 : 0 };
};
const frame = (fill) => { const a = new Uint8Array(N * 4); a.fill(fill); return a; };
const paint = (a, x0, y0, w, h, v, ch) => { for (let y = y0; y < y0 + h; y++) for (let x = x0; x < x0 + w; x++) a[(y * W + x) * 4 + (ch || 0)] = v; return a; };
{
  const still = [frame(20), frame(20), frame(20)];
  const r = T.motion(still);
  ok(r.unionPx === 0 && r.movedPx === 0 && r.bboxW === 0 && r.bbox === null, 'still frames move nothing');
  // a square walking right, three frames: the union is the strip it covered
  const walk = [0, 1, 2].map((k) => paint(frame(10), 100 + k * 10, 200, 10, 10, 200));
  const w = T.motion(walk), b = before(walk);
  ok(w.unionPx === b.unionPx && w.movedPx === b.movedPx && w.bboxW === b.bboxW && w.bboxH === b.bboxH, 'a walking square: the pre-K4 numbers exactly', JSON.stringify([w.unionPx, w.movedPx, w.bboxW, w.bboxH]));
  ok(w.bboxW === 30 && w.bboxH === 10 && w.unionPx === 300 && w.bbox.join() === '100,200,130,210', 'its union and bounds are the strip it crossed', w.bbox.join());
  ok(w.movedPx === 600, 'and the last frame is diffed against the first, as the device loops (three pairs, 200 each)', w.movedPx);
  // only an alpha byte changes: not motion (the walk reads colour, as the bake's diff does)
  const alpha = [frame(5), paint(frame(5), 0, 0, 466, 466, 9, 3)];
  ok(T.motion(alpha).movedPx === 0, 'an alpha-only change is not motion');
  // random frames, several channels: the two walks agree on every figure
  let seed = 3; const rnd = () => (seed = (seed * 1103515245 + 12345) >>> 0) / 4294967296;
  const noisy = [0, 1, 2, 3].map(() => { const a = frame(0); for (let i = 0; i < 4000; i++) { const p = (rnd() * N) | 0; a[p * 4 + ((rnd() * 3) | 0)] = (rnd() * 256) | 0; } return a; });
  const n1 = T.motion(noisy), n2 = before(noisy);
  ok(n1.unionPx === n2.unionPx && n1.movedPx === n2.movedPx && n1.bboxW === n2.bboxW && n1.bboxH === n2.bboxH, 'scattered changes: the pre-K4 numbers exactly', JSON.stringify([n1.unionPx, n1.movedPx]));
  let m = 0; for (let p = 0; p < N; p++) m += n1.mask[p]; ok(m === n1.unionPx, 'the mask holds the union');
  const small = [new Uint8Array(10 * 10 * 4), new Uint8Array(10 * 10 * 4)]; [0, 1, 10, 11].forEach((p) => { small[0][p * 4] = 9; });
  ok(T.motion(small, 10).unionPx === 4 && T.motion(small, 10).bbox.join() === '0,0,2,2', 'any size of frame, by its width');
}

// ---- the baker's estimate, on the hub's shipped default theme
{
  const bytes = new Uint8Array(fs.readFileSync(path.join(__dirname, '..', '..', '..', 'themes', 'skeleton.orbtheme')));
  const e = T.estimate(bytes);
  ok(e && e.est_fps >= 1 && e.rung >= 1 && /est \d+ fps/.test(e.sentence), 'the shipped Skeleton has an estimate', e && e.sentence);
  ok(T.estimate(new Uint8Array([1, 2, 3])) === null, 'bytes that are not a bundle have none');
  const G = global.GorbBake; global.GorbBake = null;
  ok(T.estimate(bytes) === null, 'and nor does a page without the baker');
  global.GorbBake = G;
}

// ---- the verdict
{
  let v = T.fit({ rows: 10, cap: 24, want: 15, pace: 15 });
  ok(v.fits && v.full && v.state === 'full' && v.text === '✓ Fits the orb' && v.note === '~15 of 15 fps · 10 of 24 live rows' && v.short === 'Fits · 15 fps' && v.color === '#3fb950' && v.from === 'model',
    'a design at its rate fits', JSON.stringify([v.text, v.note, v.short]));
  v = T.fit({ rows: 10, cap: 24, want: 15, pace: 12 });
  ok(v.state === 'slow' && v.near && /a little slower/.test(v.text) && v.short === 'Fits, slower · 12 fps' && v.color === '#d29922', 'three quarters of the rate is a little slower', v.text);
  v = T.fit({ rows: 10, cap: 24, want: 15, pace: 4 });
  ok(v.state === 'slow' && !v.near && /paints it slower than designed/.test(v.text), 'under three quarters is slower');
  ok(!T.fit({ rows: 1, cap: 24, want: 15, pace: 11 }).near && T.fit({ rows: 1, cap: 24, want: 16, pace: 12 }).near, 'the line is three quarters: 11 of 15 is under it, 12 of 16 on it');
  v = T.fit({ rows: 25, cap: 24, want: 15, pace: 15 });
  ok(!v.fits && v.state === 'over' && v.text === '✕ Too many live layers for the orb' && v.short === 'Over · 25 / 24 rows' && v.color === '#f85149', 'over the cap does not fit, whatever the pace');
  ok(T.fit({ rows: 24, cap: 24, want: 15, pace: 15 }).fits, 'at the cap still fits');
  v = T.fit({ rows: 3, cap: 24, want: 15, est: { est_fps: 9 } });
  ok(v.pace === 9 && v.from === 'estimate' && v.state === 'slow' && v.note === '~9 of 15 fps · 3 of 24 live rows', 'with no model the bundle’s estimate is the pace', v.note);
  v = T.fit({ rows: 3, cap: 24, want: 15, pace: 15, est: { est_fps: 2 } });
  ok(v.pace === 15 && v.from === 'model', 'an app’s own model wins over the estimate');
  v = T.fit({ rows: 3, cap: 24, want: 15 });
  ok(v.from === 'none' && v.fits && v.full && v.note === '3 of 24 live rows' && v.short === 'Fits', 'with neither, it says it fits and claims no rate', JSON.stringify([v.note, v.short]));
}

// ---- E6: a frame's price from what changed, and the rate a view holds (theme-editor-kit-plan §8)
{
  const E = GorbBake.EST4, rgba = () => new Uint8Array(N * 4);
  const dot = (a, x0, y0, w, h) => { for (let y = y0; y < y0 + h; y++) for (let x = x0; x < x0 + w; x++) a[(y * W + x) * 4] = 255; return a; };
  const still = T.frameCost(rgba(), rgba(), W, W);
  ok(still.restored === 0 && still.walk === 0 && Math.abs(still.ms - E.FIXED) < 1e-9, 'nothing changed: the fixed cost alone', still.ms);
  // one 10×10 square moved 4 px right: per row the span is its old and new place (14 px), the walk the 8 px that changed
  const a = dot(rgba(), 100, 50, 10, 10), b = dot(rgba(), 104, 50, 10, 10), c1 = T.frameCost(a, b, W, W);
  ok(c1.restored === 140 && c1.walk === 80, 'a mover restores its old and new span per row, and walks what changed', JSON.stringify(c1));
  ok(Math.abs(c1.ms - ((E.R * 140 + E.W * 80) / 1000 + E.FIXED)) < 1e-9, 'priced with estimate4’s compositor costs');
  // two movers on the same rows, far apart: the gap between them is not restored (orb_gfx.c INK_GAP_MIN)
  const a2 = dot(dot(rgba(), 10, 0, 4, 1), 300, 0, 4, 1), b2 = dot(dot(rgba(), 12, 0, 4, 1), 302, 0, 4, 1), c2 = T.frameCost(a2, b2, W, W);
  ok(c2.restored === 12, 'a gap of 24 px or more between movers is left out of the span', c2.restored);
  const a3 = dot(dot(rgba(), 10, 0, 4, 1), 30, 0, 4, 1), b3 = dot(dot(rgba(), 12, 0, 4, 1), 32, 0, 4, 1), c3 = T.frameCost(a3, b3, W, W);
  ok(c3.restored === 26, 'a narrower one is restored with them', c3.restored);
  // the rate: a small mover holds the grid's top; a whole-glass change every frame does not
  const run = (n, step, size) => Array.from({ length: n }, (_, i) => dot(rgba(), 50 + i * step, 100, size, size));
  const small = T.pace(run(31, 1, 20), { base: 60, want: 20 });
  ok(small.est === 20 && small.want === 20 && small.plans[0].fits, 'a small mover holds 20 fps', JSON.stringify(small.plans[0]));
  const flood = Array.from({ length: 31 }, (_, i) => { const f = rgba(); f.fill(i * 7 & 255); return f; });
  const big = T.pace(flood, { base: 60, want: 20 });
  ok(big.est < 20 && big.plans.every((p) => p.restored_kpx > 200), 'the whole glass changing every frame does not', JSON.stringify(big.plans.map((p) => [p.fps, p.worst_ms, p.fits])));
  ok(big.est === (big.plans.find((p) => p.fits) || big.plans[big.plans.length - 1]).fps, 'the estimate is the fastest rate that holds');
  ok(T.pace(run(31, 1, 20), { base: 60, want: 12 }).plans[0].fps === 12 && T.pace(run(31, 1, 20), { base: 60, want: 60 }).want === 20, 'the wanted rate caps the plans, and the grid’s top caps it');
  // a period is whole scans: 7.5 fps is priced on its shorter 7
  // a 185 px square jumping 184 px between frames 8 apart costs ~92.5 ms: 7.5 fps is priced
  // as estimate4's judge prices it, on floor(60 / 7.5) = 8 scans (135.5 ms with the margin)
  const p75 = T.pace(run(9, 23, 185), { base: 60, want: 7.5 }).plans[0];
  ok(p75.fps === 7.5 && p75.worst_ms > 85.7 && p75.worst_ms < 100.4 && p75.fits === true, '7.5 fps is priced on estimate4’s 8 scans', JSON.stringify(p75));
}

console.log(fails ? 'RESULT: FAIL — ' + fails + ' of ' + checked + ' checks' : 'all green (' + checked + ' checks)');
process.exit(fails ? 1 : 0);
