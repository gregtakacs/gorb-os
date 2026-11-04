#!/usr/bin/env node
// theme@1/preview (theme-editor-kit-plan K3): the OS's view of the glass, with no app around
// it. The screen's facts and the per-browser calibration (one key for every app, the clock's
// old key carried over, both instruments), the stage's views and captions, the Panel and Split
// blend held byte for byte to the algorithm the clock ran before K3 (straight on GorbBake),
// the frame clock, and the preview's time.
//
//   node headless-preview.test.js
'use strict';
const path = require('path');
let fails = 0, checked = 0;
const ok = (c, n, x) => { checked++; if (!c || process.env.VERBOSE) console.log((c ? 'PASS ' : 'FAIL ') + n + (x ? '  [' + x + ']' : '')); if (!c) fails++; };
setTimeout(() => { console.log('FAIL timed out (' + checked + ' checks run)'); process.exit(1); }, 20000);

const store = new Map(); let refuse = false;
global.window = globalThis;
global.localStorage = {
  getItem: (k) => { if (refuse) throw new Error('blocked'); return store.has(k) ? store.get(k) : null; },
  setItem: (k, v) => { if (refuse) throw new Error('blocked'); store.set(k, String(v)); },
};
const G = global.GorbBake = require(path.join(__dirname, '..', 'baker@1', 'orbtheme-baker.js'));
require(path.join(__dirname, '..', 'theme@1', 'preview.js'));
const T = OrbTheme, S = OrbTheme.screen;
const near = (a, b, e) => Math.abs(a - b) <= (e || 1e-9);

(async () => {
  // ---- the screen
  ok(S.PANEL.px === 466 && S.PANEL.inch === 1.75 && near(S.MM_PER_PX, 0.0953862, 1e-6), 'the panel is 466 px across 1.75 in', S.MM_PER_PX);
  ok(S.PANEL_MM === '44.45', 'captions print the panel as 44.45 mm, not the float’s 44.4', S.PANEL_MM);
  ok(S.REAL_CSS === 168 && S.realCss(1) === 168 && S.realCss(2) === 336, 'true size is 168 css px at the 96 px/in standard, scaled by the factor');
  ok(S.rulerPx(1) === 151 && S.screenInch(168, 1) === 1.75, 'the 40 mm ruler and the inch read back at the standard', S.rulerPx(1));
  const card = S.cardCss(1);
  ok(card.w === 324 && card.h === 204 && card.r === 12, 'a bank card’s outline at true size', JSON.stringify(card));
  ok(S.load().set === false && S.load().cal === 1, 'a browser that holds nothing is uncalibrated, at 1');
  store.set('wc.viewCal', '1.5');
  const leg = S.load();
  ok(leg.set && leg.cal === 1.5 && store.get('orb.screen.cal') === '1.5', 'the clock’s old key is read once and carried over to the shared one');
  store.delete('wc.viewCal');
  ok(S.load().cal === 1.5, 'and the shared key is what is read after');
  ok(S.save(9) === 3 && S.save(0.1) === 0.5 && S.save('x') === 1, 'a saved factor is clamped to 0.5–3');
  ok(S.save(1.23456789) === 1.2346 && store.get('orb.screen.cal') === '1.2346', 'and written rounded');
  store.set('orb.screen.cal', '7');
  ok(S.load().set === false, 'a stored factor out of range is not a calibration');
  refuse = true;
  ok(S.load().set === false && S.save(1.4) === 1.4, 'blocked storage calibrates for the page only, without throwing');
  refuse = false;
  ok(S.fromRuler(1, 50) === 0.8 && S.fromRuler(1.2, 40) === 1.2 && S.fromRuler(1, 2) === null, 'the ruler: what it really measures solves the factor', S.fromRuler(1, 50));
  ok(near(S.fromCard(1, 64.2), 1.3333, 1e-4) && S.fromCard(1, 85.6) === 1 && S.fromCard(1, 5) === null, 'the card: its measured width solves the factor', S.fromCard(1, 64.2));
  ok(S.step(1) === 0.005 && S.step(2.5) === 0.01, 'the step widens past 2×');

  // ---- the views
  const V = (o) => T.view(Object.assign({ view: 'fit', zoom: 2, fitPx: 466, cal: 1, calibrated: false }, o));
  let v = V({});
  ok(v.k === 1 && v.css === 466 && v.win === 466 && !v.zoomed && v.flow === 'hidden' && /^Fit · 1\.00×/.test(v.tag), 'Fit at the pane’s size', v.tag);
  v = V({ fitPx: 700 });
  ok(v.css === 700 && v.zoomed && v.win === 466 && v.flow === 'auto' && /1\.50×/.test(v.tag), 'Fit past 466 scrolls the frame through the box', v.tag);
  v = V({ view: 'full' });
  ok(v.k === 1 && v.css === 466 && v.tag === '1:1 · 466 px' && /own scale: 4\.85 in/.test(v.text), '1:1 is the artwork’s own pixels', v.text.slice(0, 60));
  v = V({ view: 'zoom', zoom: 3 });
  ok(v.k === 3 && v.css === 1398 && v.zoomed && v.tag === '3× · scroll to pan', 'Magnify is a zoom stop');
  v = V({ view: 'true', cal: 1.25 });
  ok(v.css === 210 && v.showRuler && v.rulerPx === 189 && /True size · 1\.75 in/.test(v.tag), 'True size draws 1.75 screen inches and the ruler', v.tag);
  ok(/96 px\/in standard/.test(v.text) && /ruler or a bank card/.test(V({ view: 'true' }).text), 'uncalibrated, it says it is assuming the standard');
  ok(/True size: 1\.75 in on this screen, the 1\.75 in panel at 44\.45 mm/.test(V({ view: 'true', calibrated: true }).text), 'calibrated, it states the panel’s size');
  ok(!V({}).showRuler && !V({ view: 'zoom' }).showRuler, 'the ruler belongs to true size only');
  ok(near(T.viewK({ view: 'true', cal: 2 }), (168 / 466) * 2) && near(V({ view: 'true', cal: 2 }).k, (168 / 466) * 2), 'True size’s scale carries the calibration');
  ok(V({}).ring === 12 && V({ view: 'true' }).ring === 4 && V({ fitPx: 100 }).ring === 3, 'the bezel ring follows the size, within 3–12');
  ok(V({}).rulerTicks.length === 41 && V({}).rulerTicks[10].lab === '10' && V({}).rulerTicks[5].h === 10, 'forty-one millimetre ticks, labelled every ten');
  ok(T.VIEWS.map((x) => x[0]).join() === 'fit,full,true,zoom' && T.ZOOMS.join() === '1.5,2,3,4' && T.RENDERS.map((x) => x[0]).join() === 'design,panel,compare', 'the view, zoom and render choices');

  // ---- Panel and Split, held to the pre-K3 algorithm
  let seed = 7; const rnd = () => (seed = (seed * 1103515245 + 12345) >>> 0) / 4294967296;
  const layer = (alphaZero) => { const a = new Uint8ClampedArray(466 * 466 * 4); for (let i = 0; i < a.length; i++) a[i] = (i & 3) === 3 ? (rnd() < alphaZero ? 0 : (rnd() * 256) | 0) : (rnd() * 256) | 0; return a; };
  const under = layer(0), hands = layer(0.8), still = layer(0.9), top = layer(0.85);
  const keep = new Uint8Array(466 * 466); for (let p = 0; p < keep.length; p += 7) keep[p] = 1;
  // the clock's drawPanel as it was at 6e3f575, on the same layers
  const before = (live, keepM) => {
    const codes = G.panelPlate(under, 466, 466, true);
    if (keepM) { const tr = G.panelPlate(under, 466, 466, false); for (let p = 0; p < keepM.length; p++) if (keepM[p]) codes[p] = tr[p]; }
    live.forEach((l) => G.panelLayer(codes, 466, l.px, 466, 466, { dither: !!l.still }));
    G.panelLayer(codes, 466, top, 466, 466, { dither: true });
    const out = new Uint8ClampedArray(466 * 466 * 4); G.panelRGBA(codes, out); return out;
  };
  const ctxOf = () => { const c = { puts: [], strokes: 0 };
    c.createImageData = (w, h) => ({ width: w, height: h, data: new Uint8ClampedArray(w * h * 4) });
    c.putImageData = (img, x, y, dx, dy, dw, dh) => c.puts.push({ data: img.data, args: [x, y, dx, dy, dw, dh].filter((a) => a !== undefined) });
    c.save = c.restore = c.beginPath = c.moveTo = c.lineTo = () => {}; c.stroke = () => { c.strokes++; };
    return c; };
  const same = (a, b) => a.length === b.length && a.every((x, i) => x === b[i]);
  const live = [{ px: hands, still: false }, { px: still, still: true }];
  let c = ctxOf();
  ok(T.panel(c, { under, live, top, keep }, { mode: 'panel' }) && c.puts.length === 1 && same(c.puts[0].data, before(live, keep)) && c.puts[0].args.join() === '0,0',
    'Panel equals the pre-K3 blend byte for byte: plate, kept texels, moving and still rows, glass');
  c = ctxOf();
  T.panel(c, { under, live: [{ px: hands, still: false }], top }, { mode: 'panel' });
  ok(same(c.puts[0].data, before([{ px: hands, still: false }], null)), 'and without a keep mask');
  c = ctxOf();
  T.panel(c, { under, live, top, keep }, { mode: 'compare', wipe: 200 });
  ok(c.puts.length === 1 && c.puts[0].args.join() === '0,0,0,0,200,466' && c.strokes === 1 && same(c.puts[0].data, before(live, keep)), 'Split puts the panel left of the wipe and draws the line', c.puts[0].args.join());
  c = ctxOf();
  T.panel(c, { under, live, top }, { mode: 'compare', wipe: 0 });
  ok(c.puts.length === 0 && c.strokes === 1, 'a wipe at 0 leaves the design whole');
  const keepG = global.GorbBake; global.GorbBake = null;
  ok(T.panel(ctxOf(), { under, live, top }, {}) === false, 'without the baker there is no panel to draw');
  global.GorbBake = keepG;

  // ---- the frame clock and the time
  let n = 0; const t1 = T.ticker(100, false, () => n++);
  await new Promise((r) => setTimeout(r, 205)); t1.stop();
  ok(n >= 12 && n <= 22, 'the ticker runs at its fps', n);
  let m = 0; const t2 = T.ticker(100, true, () => m++);
  await new Promise((r) => setTimeout(r, 205)); t2.stop();
  ok(m >= 2 && m <= 6, 'slow runs at a fifth of it', m);
  const before3 = n; await new Promise((r) => setTimeout(r, 60));
  ok(n === before3, 'a stopped ticker stops');
  const p = T.previewTime('pose');
  ok(p.getHours() === 10 && p.getMinutes() === 9 && p.getSeconds() === 36, 'the pose is 10:09:36');
  ok(Math.abs(T.previewTime('live') - Date.now()) < 1000, 'live is now');
  ok(T.previewTime('cycle', { cycle: () => new Date(2026, 0, 31) }).getDate() === 31 && Math.abs(T.previewTime('mystery', {}) - Date.now()) < 1000, 'an app’s own mode is the app’s; an unknown one is live');

  console.log(fails ? 'RESULT: FAIL — ' + fails + ' of ' + checked + ' checks' : 'all green (' + checked + ' checks)');
  process.exit(fails ? 1 : 0);
})().catch((e) => { console.log('FAIL threw: ' + (e && e.stack || e)); process.exit(1); });
