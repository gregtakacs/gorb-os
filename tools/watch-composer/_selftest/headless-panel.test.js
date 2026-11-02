#!/usr/bin/env node
// D43 P1: the Panel view's device blend, held to the shipped C.
//
// GorbBake.mix1616 / blendPx are the composer's copies of mix_16_16 and blend_px from
// main/gfx/orb_gfx.c. This suite LIFTS those two functions verbatim out of the C source
// (no re-typed copy), compiles them with the host gcc, runs random cases through both,
// and requires every result to match — all three blend modes, the 0/255 shortcuts and
// the uint32 wraparound of the packed multiply. Then it holds the Panel view's plate and
// overlay steps to the bytes a real bake stores, so the preview and the bundle cannot
// disagree about a single code.
//
//   node headless-panel.test.js               the checks
//   node headless-panel.test.js --mutations   each mutant of the JS blend must fail them
'use strict';
const fs = require('fs');
const os = require('os');
const path = require('path');
const { spawnSync } = require('child_process');

const ROOT = path.resolve(__dirname, '..', '..', '..');
const BAKER = process.env.PANEL_BAKER || path.join(__dirname, '..', 'orbtheme-baker.js');

if (process.argv.includes('--mutations')) {
  const src = fs.readFileSync(path.join(__dirname, '..', 'orbtheme-baker.js'), 'utf8');
  const M = [
    ['coverage rounds differently', 'var m5 = (mix + 4) >> 3;', 'var m5 = mix >> 3;'],
    ['the packed result loses its field mask', '(((((Math.imul((fg - bg) | 0, m5) + 0x2008010) >>> 0) >>> 5) + bg) & 0x7E0F81F) >>> 0', '((((Math.imul((fg - bg) | 0, m5) + 0x2008010) >>> 0) >>> 5) + bg) >>> 0'],
    ['the blend truncates again', '(Math.imul((fg - bg) | 0, m5) + 0x2008010) >>> 0', '(Math.imul((fg - bg) | 0, m5)) >>> 0'],
    ['MULTIPLY drops its white shortcut', '      if (c === 0xffff) return dest;\n', ''],
    ['MULTIPLY green uses a 5-bit shift', '((c >> 5) & 0x3F)) >> 6) << 5)', '((c >> 5) & 0x3F)) >> 5) << 5)'],
    ['ADDITIVE stops saturating', 'var r = Math.min((dest >> 11) + (c >> 11), 31)', 'var r = ((dest >> 11) + (c >> 11)) & 31'],
    ['the panel reads coverage from the wrong byte', 'i = (y * w + x) * 4; a = rgba[i + 3];', 'i = (y * w + x) * 4; a = rgba[i + 2];'],
    ['the panel overlay forgets the alpha lattice', '          a = ditherAlphaStep(a, ta);\n', ''],
    ['the panel plate truncates when asked to dither', 'if (dither) { t = DITHER_T', 'if (false) { t = DITHER_T'],
  ];
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'pmut.'));
  let caught = 0;
  for (const [name, a, b] of M) {
    if (!src.includes(a)) { console.log('STALE  ' + name + ' — the line it mutates is gone'); continue; }
    const f = path.join(dir, 'baker.js');
    fs.writeFileSync(f, src.replace(a, b));
    const r = spawnSync(process.execPath, [__filename], { env: Object.assign({}, process.env, { PANEL_BAKER: f }), encoding: 'utf8' });
    if (r.status !== 0) caught++;
    console.log((r.status !== 0 ? 'caught ' : 'SURVIVED ') + name);
  }
  console.log(caught === M.length ? 'all ' + M.length + ' mutants caught' : (M.length - caught) + ' of ' + M.length + ' mutants survived');
  process.exit(caught === M.length ? 0 : 1);
}

const B = require(BAKER);
let fails = 0, checked = 0;
const ok = (c, n, x) => { checked++; if (!c || process.env.VERBOSE) console.log((c ? 'PASS ' : 'FAIL ') + n + (x ? '  [' + x + ']' : '')); if (!c) fails++; };

// ---- 1. the C, lifted ----------------------------------------------------------
const gfx = fs.readFileSync(path.join(ROOT, 'main', 'gfx', 'orb_gfx.c'), 'utf8');
const lift = (sig) => {
  const i = gfx.indexOf(sig);
  if (i < 0) return null;
  let depth = 0, j = gfx.indexOf('{', i);
  for (; j < gfx.length; j++) { if (gfx[j] === '{') depth++; else if (gfx[j] === '}' && --depth === 0) break; }
  return gfx.slice(i, j + 1);
};
const mixSrc = lift('static inline uint16_t mix_16_16('), blendSrc = lift('static inline void blend_px(');
ok(!!mixSrc && !!blendSrc, 'mix_16_16 and blend_px lift out of orb_gfx.c');
// D43 §5.4: LVGL's image blends go through main/gfx/orb_lv_blend.c, whose mix must be
// orb_gfx.c's token for token — or LVGL-painted texels and orb_gfx-painted texels part.
{
  const lvSrc = fs.readFileSync(path.join(ROOT, 'main', 'gfx', 'orb_lv_blend.c'), 'utf8');
  const i = lvSrc.indexOf('static inline uint16_t mix_rounded(');
  let d = 0, j = lvSrc.indexOf('{', i);
  for (; j < lvSrc.length; j++) { if (lvSrc[j] === '{') d++; else if (lvSrc[j] === '}' && --d === 0) break; }
  const norm = (t) => t.replace(/\/\*[\s\S]*?\*\//g, '').replace(/\b(mix_16_16|mix_rounded)\b/g, 'MIX').replace(/\bresult\b/g, 'res').replace(/\s+/g, ' ').trim();
  ok(i >= 0 && norm(lvSrc.slice(i, j + 1)) === norm(mixSrc || ''), 'orb_lv_blend.c rounds exactly as orb_gfx.c\'s mix_16_16 (the two paint paths agree)');
  ok(/0x2008010u/.test(mixSrc || ''), 'the shipped mix_16_16 rounds (D43 §5.4)');
}
const N = 300000;
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'panel.'));
fs.writeFileSync(path.join(tmp, 'p.c'), [
  '#include <stdio.h>', '#include <stdint.h>', mixSrc, blendSrc,
  'int main(void){ uint32_t s = 2463534242u;',
  '  for (int i = 0; i < ' + N + '; i++) {',
  '    s ^= s << 13; s ^= s >> 17; s ^= s << 5; uint16_t dest = s & 0xffff, c = s >> 16;',
  '    s ^= s << 13; s ^= s >> 17; s ^= s << 5; uint8_t a = s & 0xff; int mode = (s >> 8) % 3;',
  '    if (((s >> 12) & 15) == 0) a = 255; if (((s >> 16) & 15) == 0) a = 0; if (((s >> 20) & 31) == 0) c = dest;',
  '    if (((s >> 25) & 31) == 0) c = mode == 1 ? 0xffff : 0;',
  '    uint16_t d = __builtin_bswap16(dest); blend_px(&d, c, a, mode);',
  '    printf("%u %u %u %d %u\\n", dest, c, a, mode, (unsigned)__builtin_bswap16(d)); }',
  '  return 0; }'].join('\n'));
const cc = spawnSync('gcc', ['-O2', '-o', path.join(tmp, 'p'), path.join(tmp, 'p.c')], { encoding: 'utf8' });
ok(cc.status === 0, 'the lifted C compiles on the host', (cc.stderr || '').slice(0, 200));
if (cc.status === 0) {
  const run = spawnSync(path.join(tmp, 'p'), [], { encoding: 'utf8', maxBuffer: 1 << 26 });
  const lines = run.stdout.trim().split('\n');
  let bad = 0, first = '', modes = [0, 0, 0];
  for (const ln of lines) {
    const [dest, c, a, mode, want] = ln.split(' ').map(Number);
    modes[mode]++;
    const got = B.blendPx(dest, c, a, mode);
    if (got !== want) { if (!bad) first = ln + ' js ' + got; bad++; }
  }
  ok(lines.length === N && bad === 0, 'blendPx equals the C on ' + N + ' random texels (NORMAL ' + modes[0] + ', MULTIPLY ' + modes[1] + ', ADDITIVE ' + modes[2] + ')', bad ? bad + ' differ, first: ' + first : '');
}

// ---- 2. the Panel view's steps are the bake's bytes ------------------------------
const W = 466;
const grad = (shift, alphaFn) => {
  const r = new Uint8Array(W * W * 4);
  for (let y = 0; y < W; y++) for (let x = 0; x < W; x++) {
    const i = (y * W + x) * 4, d = Math.hypot(x - 233, y - 233) / 233;
    r[i] = Math.min(255, 20 + 70 * d + shift); r[i + 1] = Math.min(255, 15 + 45 * d); r[i + 2] = Math.min(255, 35 + 80 * (1 - d));
    r[i + 3] = alphaFn ? alphaFn(x, y) : 255;
  }
  return { w: W, h: W, rgba: r };
};
const plate = grad(0), over = Object.assign(grad(30, (x, y) => (x * 7 + y * 3) % 251), { x: 0, y: 0 });
const hand = (() => { const p = { w: 39, h: 201, rgba: new Uint8Array(39 * 201 * 4) }; for (let i = 0; i < 39 * 201; i++) { p.rgba[i * 4] = 200; p.rgba[i * 4 + 1] = 180; p.rgba[i * 4 + 2] = 90; p.rgba[i * 4 + 3] = 255; } return Object.assign(p, { slot: 0, x: 233, y: 233, pivot_x: 19, pivot_y: 190 }); })();
const wk = B.verifyBundle3(B.bakeClock3({ name: 'Panel', serial: 1, bakedMs: 1, plate, hands: [hand], order: [0], plates: [over],
  scalars: { second_sweep: 0, tick_rate: 1 } }).bytes).sections[0];
const els = wk.blob.elements, spr = wk.blob.sprites;
const pe = els.find((e) => e.class === B.classId('plate') && e.slot === 0), oe = els.find((e) => e.class === B.classId('plate') && e.slot === 1);
const sp = spr[pe.frame0], so = spr[oe.frame0];
const stored = new Uint16Array(wk.art.slice(sp.off, sp.off + W * W * 2).buffer);
const panel = B.panelPlate(plate.rgba, W, W, true);
ok(stored.every((v, i) => v === panel[i]), 'panelPlate(dither) is the plate the bake stored, code for code');
// the overlay: the panel's dithered layer step must blend exactly what the bundle's
// overlay row (colour + coverage) blends through blend_px
const oc = new Uint16Array(wk.art.slice(so.off, so.off + W * W * 2).buffer), oa = wk.art.subarray(so.off + W * W * 2, so.off + W * W * 3);
const viaBundle = Uint16Array.from(stored), viaPanel = Uint16Array.from(stored);
for (let i = 0; i < W * W; i++) if (oa[i]) viaBundle[i] = B.blendPx(viaBundle[i], oc[i], oa[i], 0);
B.panelLayer(viaPanel, W, over.rgba, W, W, { dither: true });
let diff = 0; for (let i = 0; i < W * W; i++) if (viaBundle[i] !== viaPanel[i]) diff++;
ok(diff === 0, 'panelLayer(dither) blends the overlay exactly as the stored overlay row does', diff + ' texels differ');
// truncation, when asked: the plate the dither: false bake stores
const wk0 = B.verifyBundle3(B.bakeClock3({ name: 'Panel', serial: 1, bakedMs: 1, plate, hands: [hand], order: [0], plates: [over], dither: false,
  scalars: { second_sweep: 0, tick_rate: 1 } }).bytes).sections[0];
const pe0 = wk0.blob.elements.find((e) => e.class === B.classId('plate') && e.slot === 0), sp0 = wk0.blob.sprites[pe0.frame0];
const stored0 = new Uint16Array(wk0.art.slice(sp0.off, sp0.off + W * W * 2).buffer), panel0 = B.panelPlate(plate.rgba, W, W, false);
ok(stored0.every((v, i) => v === panel0[i]), 'panelPlate(no dither) is the truncated plate, code for code');
// decode
const rgba = B.panelRGBA(Uint16Array.of(0xffff, 0x0000, 0x8410, 0x07e0));
ok([...rgba].join() === '255,255,255,255,0,0,0,255,132,130,132,255,0,255,0,255', 'panelRGBA decodes as the panel does', [...rgba].join());

console.log(fails ? 'RESULT: FAIL — ' + fails + ' of ' + checked + ' checks' : 'all green (' + checked + ' checks)');
process.exit(fails ? 1 : 0);
