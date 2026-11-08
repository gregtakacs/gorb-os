#!/usr/bin/env node
// D43 P2: the still-plane dither, held to its Python twin and to its own promises.
//
// The baker (orbtheme-baker.js) dithers every still, screen-locked plane it ships — the
// plate and its bank, the crystal overlay, window plates, the preview — through a 64x64
// blue-noise table. tools/orbdither.py is the bit-exact twin. This suite requires:
//   1. the table hash is the one both sides pin (make_bluenoise.py --check's);
//   2. JS colour and alpha planes equal the Python's byte for byte, at odd origins;
//   3. a 565-exact colour never moves, at any threshold;
//   4. with no dither asked, the plane is plain truncation (what rgb565_le packs);
//   5. a crop dithers exactly as the same region of its whole plane (screen-locked),
//      through bank3 and through a real bakeClock3 delta plate;
//   6. bakeClock3 dithers the plate, overlay and preview, and leaves a hand alone;
//      o.dither === false turns it off.
//
//   node headless-dither.test.js               the checks
//   node headless-dither.test.js --mutations   each mutant of the baker must fail them
'use strict';
const fs = require('fs');
const os = require('os');
const path = require('path');
const { spawnSync } = require('child_process');

const ROOT = path.resolve(__dirname, '..', '..', '..', '..', '..');
const BAKER = process.env.DITHER_BAKER || path.join(__dirname, '..', 'baker@1', 'orbtheme-baker.js');
const PY = process.env.PY || 'python3';

if (process.argv.includes('--mutations')) {
  const src = fs.readFileSync(path.join(__dirname, '..', 'baker@1', 'orbtheme-baker.js'), 'utf8');
  const M = [
    ['table transposed', 'v = ditherQuant565(rgba[i], rgba[i + 1], rgba[i + 2], DITHER_T[row + ((dith.x + x) & (DITHER_N - 1))]);',
      'v = ditherQuant565(rgba[i], rgba[i + 1], rgba[i + 2], DITHER_T[((dith.x + x) & (DITHER_N - 1)) * DITHER_N + ((dith.y + y) & (DITHER_N - 1))]);'],
    ['frac comparison flipped', '((D5.base[r] + (D5.frac[r] > t ? 1 : 0)) << 11)', '((D5.base[r] + (D5.frac[r] >= t ? 1 : 0)) << 11)'],
    ['truncation restored', "    return ((D5.base[r] + (D5.frac[r] > t ? 1 : 0)) << 11) |",
      "    return ((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3); return ((D5.base[r] + (D5.frac[r] > t ? 1 : 0)) << 11) |"],
    ['a crop forgets its place in the row', 'at2 = dith ? { x: dith.x + (p.ox | 0), y: dith.y + (p.oy | 0) } : null;', 'at2 = dith ? { x: dith.x, y: dith.y } : null;'],
    ['a delta crop never learns its place', 'crop.ox = rect[0]; crop.oy = rect[1];', ''],
    ['alpha shares the colour threshold', 'row = ((dith.y + y + DITHER_ALPHA_SHIFT) & (DITHER_N - 1)) * DITHER_N;', 'row = ((dith.y + y) & (DITHER_N - 1)) * DITHER_N;'],
    ['alpha off the lattice', 'return m5 >= 32 ? 255 : m5 * 8;', 'return m5 >= 32 ? 255 : a;'],
    ['the plate is not dithered', 'order: 0, alpha: 255 }, frames, dms, false, true);', 'order: 0, alpha: 255 }, frames, dms, false, false);'],
    ['the overlay is not dithered', 'n_frames: 1 }, plates[k], true, true);', 'n_frames: 1 }, plates[k], true, false);'],
    ['a hand is dithered', 'row(hrow, h, true, !!h.dither);', 'row(hrow, h, true, true);'],
    ['the opt-out is ignored', 'return dith && o.dither !== false ? { x: e.x | 0, y: e.y | 0 } : null;', 'return dith ? { x: e.x | 0, y: e.y | 0 } : null;'],
  ];
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'dmut.'));
  let caught = 0;
  for (const [name, a, b] of M) {
    if (!src.includes(a)) { console.log('STALE  ' + name + ' — the line it mutates is gone'); continue; }
    const f = path.join(dir, 'baker.js');
    fs.writeFileSync(f, src.replace(a, b));
    const r = spawnSync(process.execPath, [__filename], { env: Object.assign({}, process.env, { DITHER_BAKER: f }), encoding: 'utf8' });
    const hit = r.status !== 0;
    if (hit) caught++;
    console.log((hit ? 'caught ' : 'SURVIVED ') + name);
  }
  console.log(caught === M.length ? 'all ' + M.length + ' mutants caught' : (M.length - caught) + ' of ' + M.length + ' mutants survived');
  process.exit(caught === M.length ? 0 : 1);
}

const B = require(BAKER);
let fails = 0, checked = 0;
const ok = (c, n, x) => { checked++; if (!c || process.env.VERBOSE) console.log((c ? 'PASS ' : 'FAIL ') + n + (x ? '  [' + x + ']' : '')); if (!c) fails++; };
const hex = (u8) => Buffer.from(u8.buffer, u8.byteOffset, u8.length).toString('hex');
let seed = 12345;
const rnd = () => (seed = (seed * 1103515245 + 12345) & 0x7fffffff) & 255;
const randPlane = (w, h, alphaAll) => {
  const r = new Uint8Array(w * h * 4);
  for (let i = 0; i < r.length; i++) r[i] = rnd();
  if (alphaAll !== undefined) for (let i = 0; i < w * h; i++) r[i * 4 + 3] = alphaAll;
  return { w, h, rgba: r };
};
// a smooth radial gradient, the shape that bands
const gradPlane = (w, h, shift) => {
  const r = new Uint8Array(w * h * 4);
  for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
    const i = (y * w + x) * 4, d = Math.hypot(x - w / 2, y - h / 2) / (w / 2);
    r[i] = Math.min(255, 20 + 60 * d + shift); r[i + 1] = Math.min(255, 12 + 40 * d); r[i + 2] = Math.min(255, 30 + 90 * (1 - d)); r[i + 3] = 255;
  }
  return { w, h, rgba: r };
};

// ---- 1. the table ------------------------------------------------------------
const crypto = require('crypto');
const jsSha = crypto.createHash('sha256').update(Buffer.from(B.DITHER_T)).digest('hex');
ok(jsSha === B.DITHER_SHA256, 'the JS table hashes to the hash it pins', jsSha.slice(0, 16));
const pyHash = spawnSync(PY, ['-c', 'import sys; sys.path.insert(0, sys.argv[1]); import orbdither as d; print(d.TABLE_SHA256 if d.table_ok() else "BAD")',
  path.join(ROOT, 'tools')], { encoding: 'utf8' });
ok(pyHash.status === 0 && pyHash.stdout.trim() === B.DITHER_SHA256, 'tools/orbdither.py pins the same table', (pyHash.stdout || pyHash.stderr).trim().slice(0, 80));

// ---- 2. byte parity with the Python twin ---------------------------------------
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'dith.'));
const cases = [[17, 13, 5, 61], [64, 64, 0, 0], [33, 70, 431, 7], [129, 5, 63, 64]];
cases.forEach(([w, h, ox, oy], k) => {
  const p = randPlane(w, h);
  fs.writeFileSync(path.join(tmp, k + '.rgba'), p.rgba);
  const py = spawnSync(PY, ['-c', [
    'import sys; sys.path.insert(0, sys.argv[1]); import orbdither as d',
    'raw = open(sys.argv[2], "rb").read(); w, h, ox, oy = map(int, sys.argv[3:7])',
    'sys.stdout.write(d.dither565(raw, w, h, ox, oy).hex() + "\\n" + d.dither_alpha(raw, w, h, ox, oy).hex())'].join('\n'),
    path.join(ROOT, 'tools'), path.join(tmp, k + '.rgba'), w, h, ox, oy], { encoding: 'utf8', maxBuffer: 1 << 24 });
  const [pc, pa] = (py.stdout || '').split('\n');
  ok(py.status === 0 && hex(B.rgb565Plane(p.rgba, w, h, { x: ox, y: oy })) === pc,
     'colour plane equals the Python, ' + w + 'x' + h + ' at (' + ox + ',' + oy + ')', (py.stderr || '').slice(0, 120));
  ok(py.status === 0 && hex(B.alphaPlane(p.rgba, w, h, { x: ox, y: oy })) === pa,
     'alpha plane equals the Python, ' + w + 'x' + h + ' at (' + ox + ',' + oy + ')');
});

// ---- 3. exact colours never move; the dither is unbiased ------------------------
const dec5 = (l) => (l << 3) | (l >> 2), dec6 = (l) => (l << 2) | (l >> 4);
let exact = true;
for (let l = 0; l < 32 && exact; l++) for (let g = 0; g < 64 && exact; g += 3) for (let t = 0; t < 256; t += 5)
  if (B.ditherQuant565(dec5(l), dec6(g), dec5(31 - l), t) !== ((l << 11) | (g << 5) | (31 - l))) { exact = false; break; }
ok(exact, 'a 565-exact colour bakes to itself at every threshold');
let worst = 0;
for (let v = 0; v < 256; v++) { let s = 0; for (let t = 0; t < 256; t++) s += dec5(B.ditherQuant565(v, 0, 0, t) >> 11); worst = Math.max(worst, Math.abs(s / 256 - v)); }
ok(worst < 0.05, 'the mean decode is the input value (no truncation darkening)', 'worst ' + worst.toFixed(3));
let lattice = true;
for (let a = 0; a < 256; a++) for (let t = 0; t < 256; t += 3) { const s = B.ditherAlphaStep(a, t); if (!(s === 0 || s === 255 || s % 8 === 0)) lattice = false; }
ok(lattice, 'every dithered coverage byte sits on the mix5 lattice (0, 8·m, 255)');

// ---- 4. no dither asked = truncation -----------------------------------------
{
  const p = randPlane(31, 9);
  const t = new Uint8Array(31 * 9 * 2);
  for (let i = 0; i < 31 * 9; i++) { const v = ((p.rgba[i * 4] & 0xf8) << 8) | ((p.rgba[i * 4 + 1] & 0xfc) << 3) | (p.rgba[i * 4 + 2] >> 3); t[i * 2] = v & 255; t[i * 2 + 1] = v >> 8; }
  ok(hex(B.rgb565Plane(p.rgba, 31, 9)) === hex(t), 'without a dither origin the plane truncates, as rgb565_le does');
  ok(hex(B.alphaPlane(p.rgba, 31, 9)) === hex(Uint8Array.from({ length: 31 * 9 }, (_, i) => p.rgba[i * 4 + 3])), 'and the alpha plane is the raw coverage');
}

// ---- 5. screen-locked: a crop dithers as its region of the whole ---------------
{
  const p = randPlane(100, 80), at = { x: 7, y: 5 };
  const whole = B.bank3([p], { alpha: true, dither: at }).art;
  const cx = 13, cy = 9, cw = 40, ch = 30;
  const crop = { w: cw, h: ch, rgba: new Uint8Array(cw * ch * 4), ox: cx, oy: cy };
  for (let y = 0; y < ch; y++) crop.rgba.set(p.rgba.subarray(((cy + y) * 100 + cx) * 4, ((cy + y) * 100 + cx + cw) * 4), y * cw * 4);
  const part = B.bank3([crop], { alpha: true, dither: at }).art;
  let same = true;
  for (let y = 0; y < ch && same; y++) for (let x = 0; x < cw; x++) {
    const wi = (cy + y) * 100 + cx + x, pi = y * cw + x;
    if (whole[wi * 2] !== part[pi * 2] || whole[wi * 2 + 1] !== part[pi * 2 + 1] || whole[100 * 80 * 2 + wi] !== part[cw * ch * 2 + pi]) { same = false; break; }
  }
  ok(same, 'bank3: a crop at its offset dithers exactly as that region of the whole plane');
}

// ---- 6. bakeClock3: what is dithered and what is not ---------------------------
const W = 466;
const plateRgba = () => gradPlane(W, W, 0);
const hand = () => { const p = gradPlane(39, 201, 0); for (let i = 0; i < 39 * 201; i++) p.rgba[i * 4 + 3] = 200; return Object.assign(p, { slot: 0, x: 233, y: 233, pivot_x: 19, pivot_y: 190 }); };
const ov = () => { const p = gradPlane(W, W, 5); for (let i = 0; i < W * W; i++) p.rgba[i * 4 + 3] = 40 + (i % 97); return Object.assign(p, { x: 0, y: 0 }); };
const prev = () => gradPlane(232, 232, 9);
const base = (extra) => Object.assign({ name: 'Dither', serial: 1, bakedMs: 1, plate: plateRgba(), hands: [hand()], order: [0],
  plates: [ov()], preview: { plane: prev(), x: 0, y: 0 }, scalars: { second_sweep: 0, tick_rate: 1 } }, extra || {});
const walk = (o) => B.verifyBundle3(B.bakeClock3(o).bytes).sections[0];
const spriteBytes = (wk, e, k) => { const s = wk.blob.sprites[e.frame0 + (k || 0)]; return wk.art.subarray(s.off, s.off + s.size); };
function rows(wk) {
  const els = wk.blob.elements;
  return { plate: els.find((e) => e.class === B.classId('plate') && e.slot === 0),
           over: els.find((e) => e.class === B.classId('plate') && e.slot === 1),
           hand: els.find((e) => e.class === B.CLASS_AUX && !(e.flags & B.AUX_F_SHADOW3)),
           prev: els.find((e) => e.class === 9) };
}
{
  const o = base(), wk = walk(o), r = rows(wk);
  ok(!!wk.art, 'the bake walks back with its art region', Object.keys(wk).join(','));
  const P = o.plate, O = o.plates[0], H = o.hands[0], V = o.preview.plane;
  // OS v1.2.12: the still plate ships as the panel's disc of that square (orb_disc.h)
  ok(hex(spriteBytes(wk, r.plate)) === hex(B.discPack(B.rgb565Plane(P.rgba, W, W, { x: 0, y: 0 }))), 'the still plate ships dithered at the panel origin');
  // OS v1.2.12: the glass ships as the panel's masked disc, and the preview as its own disc
  ok(hex(spriteBytes(wk, r.over)) === hex(B.discPackMasked(new Uint8Array([...B.rgb565Plane(O.rgba, W, W, { x: 0, y: 0 }), ...B.alphaPlane(O.rgba, W, W, { x: 0, y: 0 })]), W)),
     'the crystal overlay ships dithered, colour and alpha');
  // §60: and deflated when that is smaller; what it inflates to is the dithered disc
  const pvDisc = B.discPack(B.rgb565Plane(V.rgba, 232, 232, { x: 0, y: 0 }), 232), pvZ = B.zlibFixed(pvDisc);
  ok(hex(spriteBytes(wk, r.prev)) === hex(pvZ.length < pvDisc.length ? pvZ : pvDisc), 'the preview ships dithered');
  const hb = spriteBytes(wk, r.hand);
  ok(hex(hb.subarray(0, 39 * 201 * 2)) === hex(B.rgb565Plane(H.rgba, 39, 201)), 'a hand ships truncated: an arm is not dithered unless it asks');
  const off = walk(base({ dither: false })), r2 = rows(off);
  ok(hex(spriteBytes(off, r2.plate)) === hex(B.discPack(B.rgb565Plane(P.rgba, W, W))), 'dither: false bakes the plate truncated');
  ok(hex(spriteBytes(off, r2.over).subarray(B.discPx(W) * 2)) === hex(B.discPack(B.alphaPlane(O.rgba, W, W), W, 1)), 'and the overlay alpha raw');
}
{
  // a delta plate: keyframe 0, then frames that change one band each — every delta crop
  // must be the same bytes as that region of its whole frame, dithered at the panel origin
  const fr = [0, 1, 2].map((k) => { const p = gradPlane(W, W, 0); for (let y = 100 + 40 * k; y < 130 + 40 * k; y++) for (let x = 150; x < 300; x++) p.rgba[(y * W + x) * 4] = 200 - 30 * k; return p; });
  const o = base({ plate: { rgba: fr[0].rgba, w: W, h: W, frames: fr } });
  const wk = walk(o), r = rows(wk);
  let good = r.plate.n_frames === 3, why = 'n_frames ' + r.plate.n_frames;
  for (let k = 1; k < 3 && good; k++) {
    const s = wk.blob.sprites[r.plate.frame0 + k], full = B.rgb565Plane(fr[k].rgba, W, W, { x: 0, y: 0 }), got = wk.art.subarray(s.off, s.off + s.size);
    for (let y = 0; y < s.h && good; y++) for (let x = 0; x < s.w; x++) {
      const fi = ((s.diffy + y) * W + s.diffx + x) * 2, gi = (y * s.w + x) * 2;
      if (full[fi] !== got[gi] || full[fi + 1] !== got[gi + 1]) { good = false; why = 'frame ' + k + ' texel (' + (s.diffx + x) + ',' + (s.diffy + y) + ')'; break; }
    }
  }
  ok(good, 'every delta crop is its region of the whole frame, dithered in place', why);
}

console.log(fails ? 'RESULT: FAIL — ' + fails + ' of ' + checked + ' checks' : 'all green (' + checked + ' checks)');
process.exit(fails ? 1 : 0);
