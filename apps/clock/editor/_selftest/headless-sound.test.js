#!/usr/bin/env node
// D39's generator and the bake that carries it (docs/history/sound-plan.md §6, §7).
//
// The JS port is the authority for the defaults, so §6's three measurements run on ITS
// output: every tick loop at every rate is seamless (the circular second difference at
// the seam is no sharper than the loop's own 99.99th percentile), keeps its weight
// above the speaker's ~300 Hz floor, and holds an even level beat to beat. The catalog
// renders whole, the per-preset mapping names only what exists, and a bake that
// carries sounds walks back with them at the head of the bank — then the shipped C
// projection (tools/verify_js_bake.py) has to accept it.
'use strict';
const { spawnSync } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');
const S = require('../sound-engine.js');
const B = require(require('./runtime-paths.js').BAKER);

const ROOT = require('./runtime-paths.js').ROOT;
const PY = process.env.B3_PY || 'python3';
let checked = 0, fails = 0;
const ok = (c, n, x) => { checked++; console.log((c ? 'PASS ' : 'FAIL ') + n + (x ? '  [' + x + ']' : '')); if (!c) fails++; };
const refuses = (fn, want, name) => {
  try { fn(); ok(false, name, 'accepted'); } catch (e) { ok(String(e.message).indexOf(want) >= 0, name, e.message.slice(0, 160)); }
};

function seamOk(p) {
  const n = p.length, d2 = new Float64Array(n);
  for (let i = 0; i < n; i++) d2[i] = Math.abs(p[(i + 1) % n] - 2 * p[i] + p[(i - 1 + n) % n]);
  const s = Float64Array.from(d2).sort();
  return Math.max(d2[0], d2[n - 1]) <= s[Math.floor(n * 0.9999)];
}
// Energy below ~300 Hz as a fraction of the whole: four cascaded 300 Hz low-passes
// (eighth order, steep enough that a 420 Hz fundamental is not billed to the floor),
// run circularly over a loop by warming on a lap first.
function lowShare(p) {
  const w = 2 * Math.PI * 300 / S.SR, c = Math.cos(w), al = Math.sin(w) / (2 * Math.SQRT1_2), a0 = 1 + al;
  const k = [(1 - c) / 2 / a0, (1 - c) / a0, (1 - c) / 2 / a0, -2 * c / a0, (1 - al) / a0];
  let sig = new Float64Array(2 * p.length);
  for (let i = 0; i < sig.length; i++) sig[i] = p[i % p.length];
  for (let pass = 0; pass < 4; pass++) {
    const y = new Float64Array(sig.length);
    let x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    for (let i = 0; i < sig.length; i++) {
      const o = k[0] * sig[i] + k[1] * x1 + k[2] * x2 - k[3] * y1 - k[4] * y2;
      x2 = x1; x1 = sig[i]; y2 = y1; y1 = o; y[i] = o;
    }
    sig = y;
  }
  let lo = 0, all = 0;
  for (let i = 0; i < p.length; i++) { lo += sig[p.length + i] ** 2; all += p[i] * p[i]; }
  return lo / (all || 1);
}
// Per-beat level, each window exactly one beat: at 8 per second a beat is 2756.25
// samples, and a rounded window would leave a sliver at the end that reads as silence.
function beatRmsSd(p, rate) {
  const nb = S.LOOP_S * rate, db = [];
  for (let b = 0; b < nb; b++) {
    const lo = Math.round(b * S.SR / rate), hi = Math.round((b + 1) * S.SR / rate);
    let s = 0;
    for (let i = lo; i < hi; i++) s += p[i] * p[i];
    db.push(10 * Math.log10(s / (hi - lo) + 1e-9));
  }
  const m = db.reduce((a, v) => a + v, 0) / db.length;
  return Math.sqrt(db.reduce((a, v) => a + (v - m) ** 2, 0) / db.length);
}

// ---- determinism ----
const a1 = S.renderLoop('mantel', 1, 5), a2 = S.renderLoop('mantel', 1, 5), a3 = S.renderLoop('mantel', 1, 6);
ok(a1.length === a2.length && a1.every((v, i) => v === a2[i]), 'one seed renders one loop, byte for byte');
ok(a1.some((v, i) => v !== a3[i]), 'another seed renders another loop');
const c1 = S.renderSound('westminster_q4', 3), c2 = S.renderSound('westminster_q4', 3);
ok(c1.every((v, i) => v === c2[i]), 'one seed renders one chime, byte for byte');

// ---- §6 on every loop: length, seam, floor, level ----
let loops = 0, badSeam = [], lowWorst = 0, lowWho = '', sdWorst = 0, sdWho = '';
for (const v of Object.keys(S.VOICES)) {
  const rates = v === 'geiger' ? [0] : S.VOICES[v].rates;
  for (const r of rates) {
    const p = S.renderLoop(v, r);
    loops++;
    if (p.length !== S.SR * S.LOOP_S || !seamOk(p)) badSeam.push(v + '@' + r);
    const lo = lowShare(p);
    if (lo > lowWorst) { lowWorst = lo; lowWho = v + '@' + r; }
    if (r && ['mantel', 'pocket', 'wrist', 'classic', 'regulator', 'woodblock'].indexOf(v) >= 0) {
      const sd = beatRmsSd(p, r);
      if (sd > sdWorst) { sdWorst = sd; sdWho = v + '@' + r; }
    }
  }
}
ok(loops === 16 * 4 + 2, 'every voice renders at every rate it offers', loops + ' loops');
ok(!badSeam.length, 'every loop is 8 s and seamless by the circular second difference', badSeam.join(', ') || 'all');
ok(lowWorst < 0.02, 'no loop puts 2% of its energy below the speaker’s 300 Hz floor',
   (100 * lowWorst).toFixed(2) + '% worst, ' + lowWho);
ok(sdWorst < 2.0, 'beat-to-beat level holds within 2 dB SD on the escapement voices',
   sdWorst.toFixed(2) + ' dB worst, ' + sdWho);
refuses(() => S.renderLoop('heartbeat', 4), 'no 4-per-second loop', 'a rate-limited voice refuses a rate it does not offer');

// ---- the catalog, the slot fits, and §7.2's mapping ----
let cat = 0, badCat = [];
for (const id of Object.keys(S.CATALOG)) {
  const p = S.renderSound(id);
  let pk = 0; for (const v of p) pk = Math.max(pk, Math.abs(v));
  cat++;
  if (!(p.length > 100) || pk < 29000 || pk > 29300) badCat.push(id + ' (' + p.length + ' samples, peak ' + pk + ')');
}
ok(!badCat.length, 'every catalog sound renders, normalized to -1 dBFS', cat + ' sounds' + (badCat.length ? '; ' + badCat.join(', ') : ''));
const slotNames = B.SOUND_SLOTS.filter((s) => s !== 'tick_loop');
ok(slotNames.every((s) => S.fits(s).length > 0), 'every one-shot slot has catalog sounds that fit it');
let mapBad = [];
for (const [preset, m] of Object.entries(S.PRESETS)) {
  if (m.tick !== null && !S.VOICES[m.tick]) mapBad.push(preset + ': tick ' + m.tick);
  if (B.HOUR_PATTERNS.indexOf(m.pattern) < 0) mapBad.push(preset + ': pattern ' + m.pattern);
  for (const [slot, id] of Object.entries(m.slots)) {
    if (B.SOUND_SLOTS.indexOf(slot) < 0) mapBad.push(preset + ': slot ' + slot);
    else if (!S.CATALOG[id]) mapBad.push(preset + ': ' + slot + ' = ' + id + ' (no such sound)');
    else if (S.fits(slot).indexOf(id) < 0) mapBad.push(preset + ': ' + id + ' does not fit ' + slot);
  }
  if ((m.pattern === 'count' || m.pattern === 'ships_bells') && !m.slots.strike_hour) mapBad.push(preset + ': strikes with no stroke');
}
ok(!mapBad.length, 'all 16 presets map to sounds that exist and fit their slots', mapBad.join('; ') || Object.keys(S.PRESETS).length + ' presets');
ok(Object.keys(S.PRESETS).length === 16, 'the mapping covers sixteen of the composer’s presets');

// ---- an uploaded tick, made into a loop at each rate ----
const clickT = S.renderSound('sine_blip');
const fromTick = S.RATES.map((r) => S.loopFromTick(clickT, r, 4));
ok(fromTick.every((p) => p.length === S.SR * S.LOOP_S && seamOk(p)),
   'one uploaded tick becomes a seamless 8 s loop at 1, 2, 4 and 8 per second');

// ---- the bake: sounds at the head, the plate behind them, the dial accepts it ----
const plateRgba = (w, h) => { const r = new Uint8Array(w * h * 4); for (let i = 0; i < w * h; i++) { r[i * 4] = 3; r[i * 4 + 1] = 7; r[i * 4 + 2] = 11; r[i * 4 + 3] = 255; } return r; };
const mkPlane = (w, h, a) => { const r = plateRgba(w, h); if (a !== undefined) for (let i = 0; i < w * h; i++) r[i * 4 + 3] = a; return r; };
const PL = { rgba: mkPlane(466, 466), w: 466, h: 466 };
// an odd-sized hand (39 x 201 x 3 B per plane is odd) proves the head placement keeps
// every sound even however the pixels fall
const HK = { slot: 0, rgba: mkPlane(39, 201, 200), x: 233, y: 233, w: 39, h: 201, pivot_x: 19, pivot_y: 190 };
const tickLoop = S.renderLoop('pocket', 1, 7);
const sounds = [
  { slot: 'chime_hour', pcm: S.renderSound('station_dingdong_3tone') },
  { slot: 'tick_loop', pcm: tickLoop, beat_chz: 100, first_beat: 0 },
  { slot: 'jump_minute', pcm: S.renderSound('station_minute_clunk') },
];
const baked = B.bakeClock3({ name: 'Sound Bake', serial: 20261031, bakedMs: 1730000000000,
  plate: PL, hands: [HK], order: [0], sounds, soundPattern: 'none',
  scalars: { second_sweep: 0, tick_rate: 1 } });
const walk = B.verifyBundle3(baked.bytes).sections[0];
const els = walk.blob.elements, spr = walk.blob.sprites, sc = walk.blob.scalars;
const srows = els.filter((e) => e.class === B.CLASS_SOUND);
// OS v1.2.12: each clip in the encoding the bake chose (ADPCM unless it would lose too much)
const soundB = spr.slice(0, 3).reduce((s, p) => s + p.size, 0);
ok(srows.length === 3 && srows.map((e) => e.slot).join() === '0,4,6',
   'three sound rows walk back, in slot order', srows.map((e) => e.slot).join());
ok(srows.every((e, k) => e.frame0 === k && spr[k].off % 2 === 0 && spr[k].h === 1 &&
                         spr[k].size === B.soundBytes(e.src, spr[k].w) && e.src === B.soundEncode(Array.from(
                           [tickLoop, sounds[0].pcm, sounds[2].pcm][k])).enc),
   'their planes are the first three, even, one row high, each in the encoding the bake chose');
ok(els[0].class === B.classId('plate') && spr[els[0].frame0].off === soundB && sc.plate_off === soundB,
   'the plate is still row 0 and sits, and is named by plate_off, just past the sounds', 'plate_off ' + sc.plate_off);
ok(srows[0].flags === 2 && srows[0].vmax === 100 && srows[1].flags === 0 && srows[1].vmax === 0,
   'the tick loop alone loops and carries the 1-per-second beat');
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'snd3.'));
const bk = path.join(tmp, 'sound.orbtheme');
fs.writeFileSync(bk, baked.bytes);

// a delta plate: the canvas seeds from bg_key_off, so it must follow the plate too
const kf = (v) => { const r = plateRgba(466, 466); for (let i = 0; i < 4000; i++) r[i * 4] = v; return r; };
const APL = { rgba: kf(1), w: 466, h: 466, frames: [{ rgba: kf(1), w: 466, h: 466 }, { rgba: kf(9), w: 466, h: 466 }, { rgba: kf(17), w: 466, h: 466 }] };
const bakedA = B.bakeClock3({ name: 'Sound Anim', serial: 20261031, bakedMs: 1730000000000,
  plate: APL, hands: [HK], order: [0], sounds, soundPattern: 'none', scalars: { second_sweep: 0, tick_rate: 1 } });
const walkA = B.verifyBundle3(bakedA.bytes).sections[0];
ok(walkA.blob.scalars.bg_mode === 1 && walkA.blob.scalars.bg_key_off === soundB && walkA.blob.scalars.plate_off === soundB,
   'on a delta plate bg_key_off follows the keyframe past the sounds, or the canvas would seed from PCM',
   'bg_key_off ' + walkA.blob.scalars.bg_key_off);
const bkA = path.join(tmp, 'sound-anim.orbtheme');
fs.writeFileSync(bkA, bakedA.bytes);

const pr = spawnSync(PY, [path.join(ROOT, 'tools', 'verify_js_bake.py'), bk, bkA], { encoding: 'utf8' });
ok(pr.status === 0, 'the shipped C projection accepts both sound bakes', (pr.stderr || pr.stdout || '').trim().slice(0, 300));
const pl = spawnSync(PY, [path.join(ROOT, 'tools', 'orbsound.py'), 'list', bk], { encoding: 'utf8' });
ok(pl.status === 0 && /tick_loop\s+176400 samples/.test(pl.stdout) && /jump_minute/.test(pl.stdout),
   'tools/orbsound.py reads the JS bake’s sounds back', (pl.stdout || pl.stderr).trim().split('\n').join(' | ').slice(0, 300));

// ---- the bake refuses what the dial refuses, in its words ----
refuses(() => B.bakeClock3({ name: 'x', plate: PL, hands: [HK], order: [0], soundPattern: 'none',
  sounds: [{ slot: 'tick_loop', pcm: tickLoop, beat_chz: 400 }], scalars: { second_sweep: 0, tick_rate: 1 } }),
  'the sound would walk off the hand', 'a 4-per-second loop under a 1-per-second step is refused at the bake');
refuses(() => B.bakeClock3({ name: 'x', plate: PL, hands: [HK], order: [0], soundPattern: 'count',
  sounds: [{ slot: 'chime_hour', pcm: sounds[0].pcm }], scalars: { second_sweep: 1 } }),
  'carries no hour stroke', 'a counting pattern with no stroke is refused at the bake');
refuses(() => B.bakeClock3({ name: 'x', plate: PL, hands: [HK], order: [0], soundPattern: 'none',
  sounds: [{ slot: 'tick_loop', pcm: tickLoop.subarray(0, 22051), beat_chz: 100 }], scalars: { second_sweep: 1 } }),
  'not a whole number of beats', 'a loop that ends mid-beat is refused at the bake');
refuses(() => B.bakeClock3({ name: 'x', plate: PL, hands: [HK], order: [0],
  sounds: [{ slot: 'chime_q5', pcm: sounds[0].pcm }] }), 'the clock declares', 'an unknown slot is refused at the bake');

console.log(fails ? 'FAIL ' + fails + ' of ' + checked : 'all green (' + checked + ' checks)');
process.exit(fails ? 1 : 0);
