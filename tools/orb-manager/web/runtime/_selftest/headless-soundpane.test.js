#!/usr/bin/env node
// pomodoro-editor §64.8: the kit's sound layer — GorbBake.synth (a pure recipe → PCM
// generator), stage3().sounds (a theme's sound rows for ANY app, planes at the head and
// plate_off merged), and S.Sound (the field rows the pane puts in front of an author).
// Node has no AudioContext, which is exactly why the generator is pure JS and the browser
// decode lives behind the pane; nothing here shims OfflineAudioContext.
//
//   node headless-soundpane.test.js
'use strict';
const path = require('path');
const { spawnSync } = require('child_process');
let fails = 0, checked = 0;
const ok = (c, n, x) => { checked++; console.log((c ? 'PASS ' : 'FAIL ') + n + (x ? '  [' + x + ']' : '')); if (!c) fails++; };
const refuses = (fn, want, name) => {
  try { fn(); ok(false, name, 'accepted'); } catch (e) { ok(String(e.message).indexOf(want) >= 0, name, String(e.message).slice(0, 170)); }
};
const G = require(path.join(__dirname, '..', 'baker@1', 'orbtheme-baker.js'));
const SR = G.SOUND_RATE, W = 466, PLATE = G.classId('plate'), SOUND = G.classId('sound');
const plane = (w, h, rgb) => { const p = { w, h, rgba: new Uint8Array(w * h * 4) }; for (let i = 0; i < w * h; i++) p.rgba.set([rgb[0], rgb[1], rgb[2], 255], i * 4); return p; };
const plateRow = (B) => ({ n: B.id(), 'class': PLATE, slot: 0, x: 0, y: 0, w: W, h: W, order: 0, alpha: 255, n_frames: 1 });
const sum = (p) => { let s = 0; for (let i = 0; i < p.length; i++) s = (s * 31 + (p[i] & 0xffff)) >>> 0; return s >>> 0; };

// the recipes this suite exercises: one per kind, and both variants of a two-event one
const RECIPES = {
  sonar: { name: 'sonar', label: 'Sonar ping', dur: 1.2,
    events: [{ t: 0, kind: 'tone', f: 1200, dur: 0.9, gain: 1.0, attack: 0.004, type: 'sine' },
             { t: 0.18, kind: 'tone', f: 1200, dur: 0.6, gain: 0.35 }],
    brighter: { f_mul: 1.25, gain_mul: 0.9, events: [{ t: 0.4, kind: 'tone', f: 1200, dur: 0.4, gain: 0.5 }] } },
  bell: { name: 'bell', label: 'Brass bell', dur: 1.4,
    events: [{ t: 0, kind: 'bell', f: 660, dur: 1.2, gain: 1.0, ratios: [1, 2.0, 2.76, 4.1, 5.4] }] },
  tick: { name: 'tick', label: 'Wood tick', dur: 0.4,
    events: [{ t: 0, kind: 'click', f: 3200, q: 2, dur: 0.35, gain: 1.0 }] },
  chimeBeep: { name: 'chimeBeep', label: 'Soft beep', dur: 0.6,
    events: [{ t: 0, kind: 'beep', f: 880, dur: 0.5, gain: 0.9 }] },
  high: { name: 'high', label: 'High bell', dur: 1.0,
    events: [{ t: 0, kind: 'bell', f: 3000, dur: 0.9, gain: 1.0, ratios: [1, 2, 3] }] },
};
const rms = (p) => { let s = 0; for (let i = 0; i < p.length; i++) s += p[i] * p[i]; return Math.sqrt(s / p.length); };
const dft = (p, f) => { let re = 0, im = 0; for (let i = 0; i < p.length; i++) { re += p[i] * Math.cos(2 * Math.PI * f * i / SR); im -= p[i] * Math.sin(2 * Math.PI * f * i / SR); } return Math.hypot(re, im) * 2 / p.length / Math.SQRT2; };

// ---- synth: deterministic within a process, and across two -----------------------
{
  const a = G.synth(RECIPES.sonar), b = G.synth(RECIPES.sonar);
  ok(a.length === b.length && a.every((v, i) => v === b[i]), 'synth: two runs in one process give identical bytes');
  ok(a instanceof Int16Array && a.length === Math.round(1.2 * SR), 'synth: the answer is an Int16Array at 22050 Hz over the recipe length', a.length);
  const child = 'const G=require(' + JSON.stringify(path.join(__dirname, '..', 'baker@1', 'orbtheme-baker.js')) + ');' +
    'const r=' + JSON.stringify(RECIPES.sonar) + ';const p=G.synth(r);let s=0;for(let i=0;i<p.length;i++)s=(s*31+(p[i]&0xffff))>>>0;console.log(s>>>0);';
  const run = spawnSync(process.execPath, ['-e', child], { encoding: 'utf8' });
  ok(String(run.stdout).trim() === String(sum(a)), 'synth: a fresh node process gives the same bytes (a re-bake is byte-identical)', String(run.stdout).trim() + ' vs ' + sum(a));
  const v1 = G.synth(RECIPES.sonar, { variant: 'brighter' }), v0 = G.synth(RECIPES.sonar);
  ok(v1.length === v0.length && v1.some((v, i) => v !== v0[i]), 'synth: the brighter variant changes the bytes and not the length');
}

// ---- synth: the 350 Hz speaker floor, every recipe, both variants ---------------
{
  let worst = -Infinity, who = '';
  for (const key of Object.keys(RECIPES)) for (const variant of [undefined, 'brighter']) {
    const p = G.synth(RECIPES[key], variant ? { variant } : {}), r = rms(p) || 1;
    for (const f of [100, 200, 300]) {
      const db = 20 * Math.log10((dft(p, f) || 1e-9) / r);
      if (db > worst) { worst = db; who = key + (variant ? '/' + variant : '') + '@' + f; }
    }
  }
  ok(worst <= -40, 'synth: everything below 350 Hz is at least 40 dB under the clip’s own RMS', 'worst ' + worst.toFixed(1) + ' dB at ' + who);
}

// ---- synth: a top partial past 8 kHz is rendered, not truncated (DAC runs 22050) --
{
  const p = G.synth(RECIPES.high), hi = dft(p, 9000), mid = dft(p, 3000);
  ok(hi > rms(p) * 0.02 && mid > 0, 'synth: a 9 kHz partial survives the render (nothing is cut at 8 kHz)', '9k=' + hi.toFixed(1) + ' 3k=' + mid.toFixed(1));
}

// ---- synth: the ADPCM round trip holds at ADPCM_MIN_SNR_DB (20 dB) ---------------
{
  let low = 99, who = '', anyAdpcm = false;
  for (const key of Object.keys(RECIPES)) {
    const p = G.synth(RECIPES[key]), se = G.soundEncode(p);
    if (se.enc !== 1) continue;                       // a clip the encoder kept as PCM16 is not billed here
    anyAdpcm = true;
    const dec = G.adpcmDecode(se.bytes, p.length);
    let sig = 0, err = 0;
    for (let i = 0; i < p.length; i++) { const x = p[i], d = x - dec[i]; sig += x * x; err += d * d; }
    const snr = 10 * Math.log10((sig || 1) / (err || 1e-9));
    if (snr < low) { low = snr; who = key; }
  }
  ok(!anyAdpcm || low >= 20, 'synth: an ADPCM-stored chime round-trips at the 20 dB floor', anyAdpcm ? 'lowest ' + low.toFixed(1) + ' dB (' + who + ')' : 'every recipe fell to PCM16');
}

// ---- synth refusals, each naming the recipe --------------------------------------
{
  refuses(() => G.synth({ name: 'z', dur: 0.5, events: [{ t: 0, kind: 'triangle', f: 500, dur: 0.4 }] }), 'kind', 'synth refuses an unknown kind, naming the recipe');
  refuses(() => G.synth({ name: 'z', dur: 0.5, events: [{ t: -0.1, kind: 'tone', f: 500, dur: 0.4 }] }), 't=', 'synth refuses a negative t, naming the recipe');
  refuses(() => G.synth({ name: 'z', dur: 0.5, events: [] }), 'no events', 'synth refuses empty events, naming the recipe');
  refuses(() => G.synth({ name: 'z', dur: 0, events: [{ t: 0, kind: 'tone', f: 500, dur: 0.4 }] }), 'lasts 0', 'synth refuses a dur of 0, naming the recipe');
  refuses(() => G.synth({ name: 'long', dur: 6, events: [{ t: 0, kind: 'tone', f: 500, dur: 6 }] }), 'the long chime is 6.0 s: an app', 'synth refuses a chime past 4.0 s with the seconds named');
}

// ---- sounds(): the empty list writes nothing and leaves plate_off alone ----------
{
  const B = G.stage3({});
  const ans = B.sounds([]);
  ok(ans.bytes === 0 && ans.rows === 0 && ans.enc.length === 0 && B.bank.size === 0 && B.bank.sprites.length === 0,
    'sounds([]): no bytes, no rows, no encoding, and the bank never moved', JSON.stringify(ans));
  B.row(plateRow(B), plane(W, W, [10, 20, 30]), false, true);
  const b = B.finish({ name: 'NoSound', slug: 'pomo', abi: G.GEOM_ABI6, scalars: {} });
  const sec = G.verifyBundle3(b.bytes).sections[0].blob;
  ok(sec.elements.length === 1 && sec.elements[0]['class'] === PLATE, 'sounds([]): the plate is the only row');
  ok(sec.scalars.plate_off === 0, 'sounds([]): plate_off is left alone (the plate still paints at zero)', sec.scalars.plate_off);
  const C = G.stage3({}); C.row(plateRow(C), plane(W, W, [10, 20, 30]), false, true);
  const c = C.finish({ name: 'NoSound', slug: 'pomo', abi: G.GEOM_ABI6, scalars: {} });
  ok(Buffer.compare(Buffer.from(b.bytes), Buffer.from(c.bytes)) === 0, 'sounds([]): the bake matches one that never called sounds() at all');
}

// ---- sounds(): planes at the head, plate_off merged, proven by unpacking the bake --
{
  const pcm = G.synth(RECIPES.sonar);
  const B = G.stage3({});
  const ans = B.sounds([{ slot: 1, pcm, name: 'focus' }], { pattern: 'none' });
  ok(ans.bytes === G.soundBytes(1, pcm.length) && ans.rows === 1 && ans.enc[0].enc === 1,
    'sounds: one chime encodes to ADPCM and its byte count is the whole head', JSON.stringify(ans.bytes));
  B.row(plateRow(B), plane(W, W, [10, 20, 30]), false, true);
  const b = B.finish({ name: 'One', slug: 'pomo', abi: G.GEOM_ABI6, scalars: {} });
  const un = G.verifyBundle3(b.bytes).sections[0].blob;   // unpackBlob3's own answer, off finished bytes
  const plate = un.elements.find((e) => e['class'] === PLATE), snd = un.elements.filter((e) => e['class'] === SOUND);
  ok(un.scalars.plate_off === ans.bytes, 'sounds: finish merged plate_off = the head byte count', un.scalars.plate_off);
  ok(un.sprites[plate.frame0].off === un.scalars.plate_off,
    'sounds: the plate row’s sprite offset equals the plate_off scalar (the scheduler paints the plate, not PCM)',
    un.sprites[plate.frame0].off + ' == ' + un.scalars.plate_off);
  ok(snd.length === 1 && snd[0].n_frames === 1 && snd[0].kind === 4 && snd[0].src === 1 && snd[0].flags === 0 && !snd[0].vmax && !snd[0].vmin,
    'sounds: the SOUND row is one-shot, pattern none, ADPCM, no beat', JSON.stringify([snd[0].kind, snd[0].src, snd[0].flags]));
  ok(un.sprites[snd[0].frame0].off === 0 && (un.sprites[snd[0].frame0].off & 1) === 0,
    'sounds: the plane sits at the head, at an even offset', un.sprites[snd[0].frame0].off);
  const C = G.stage3({}); C.sounds([{ slot: 2, pcm }]);
  C.row(plateRow(C), plane(W, W, [1, 2, 3]), false, true);
  const cu = G.verifyBundle3(C.finish({ name: 'Two', slug: 'pomo', abi: G.GEOM_ABI6, scalars: { plate_off: 4242 } }).bytes).sections[0].blob;
  ok(cu.scalars.plate_off === 4242, 'sounds: a bake that set plate_off itself keeps its own value', cu.scalars.plate_off);
  // the merge lands on a copy: the bake's own meta object is left as it wrote it
  const E = G.stage3({}); E.sounds([{ slot: 1, pcm }]);
  E.row(plateRow(E), plane(W, W, [1, 2, 3]), false, true);
  const meta = { name: 'Copy', slug: 'pomo', abi: G.GEOM_ABI6, scalars: {} };
  E.finish(meta);
  ok(Object.keys(meta.scalars).length === 0 && meta.name === 'Copy',
    'sounds: the plate_off merge does not write through into the caller’s meta', JSON.stringify(meta.scalars));
}

// ---- sounds(): a shared plane for two byte-equal slots, two rows ------------------
{
  const pcm = G.synth(RECIPES.sonar);
  const B = G.stage3({});
  const ans = B.sounds([{ slot: 1, pcm }, { slot: 2, pcm: pcm.slice() }, { slot: 3, pcm: pcm.slice() }], { pattern: 'none' });
  ok(ans.rows === 3 && B.bank.sprites.length === 1 && ans.bytes === G.soundBytes(1, pcm.length),
    'sounds: three byte-equal slots share ONE plane (one put, three rows name it)', [ans.rows, B.bank.sprites.length].join());
  B.row(plateRow(B), plane(W, W, [4, 5, 6]), false, true);
  const sec = G.verifyBundle3(B.finish({ name: 'Three', slug: 'pomo', abi: G.GEOM_ABI6, scalars: {} }).bytes).sections[0].blob;
  const snd = sec.elements.filter((e) => e['class'] === SOUND);
  ok(snd.length === 3 && snd.every((e) => e.frame0 === snd[0].frame0) && snd.map((e) => e.slot).join() === '1,2,3',
    'sounds: the three rows all name the same plane, in slot order', snd.map((e) => e.slot + ':' + e.frame0).join());
  const C = G.stage3({});
  C.sounds([{ slot: 1, pcm }, { slot: 2, pcm: G.synth(RECIPES.bell) }]);
  ok(C.bank.sprites.length === 2, 'sounds: a different waveform is its own plane', C.bank.sprites.length);
}

// ---- sounds(): a repeated slot refused before the bank moves; slot 0 refused ------
{
  const pcm = G.synth(RECIPES.sonar);
  const B = G.stage3({});
  const before = B.bank.size;
  refuses(() => B.sounds([{ slot: 1, pcm }, { slot: 1, pcm: pcm.slice() }]), 'two sounds at slot 1', 'sounds refuses a repeated slot');
  ok(B.bank.size === before && B.bank.sprites.length === 0, 'sounds: the refusal happens before the bank moves', B.bank.size);
  const D = G.stage3({});
  let msg = null;
  try { D.sounds([{ slot: 0, pcm }]); D.row(plateRow(D), plane(W, W, [1, 2, 3]), false, true); D.finish({ name: 'z', slug: 'pomo', abi: 6, scalars: {} }); }
  catch (e) { msg = e.message; }
  ok(/tick loop is not flagged to loop/.test(msg || ''), 'sounds refuses a non-looping sound at slot 0 with the device’s sentence', (msg || '').slice(0, 90));
}

// ---- S.Sound: the pane's rows for a slot, and its readout ------------------------
{
  global.window = globalThis;
  global.React = { createElement: (type, props, ...kids) => ({ type, props: props || {}, kids: kids.flat(Infinity).filter((k) => k != null && k !== false) }),
    useRef: (v) => ({ current: v }), useState: (v) => [v, () => {}], useEffect: () => {}, useLayoutEffect: () => {}, Fragment: Symbol() };
  global.GorbBake = G;
  require(path.join(__dirname, '..', 'studio@1', 'studio.js'));
  const S = global.OrbStudio, Kit = S.fieldKit({});
  const pcm = G.synth(RECIPES.sonar);
  const played = [], stopped = [];
  const store = { focus: { rec: 'sonar' }, brk: { upload: true } };
  const rows = S.Sound(Kit, {
    recipes: [RECIPES.sonar, RECIPES.bell],
    slots: [{ key: 'focus', label: 'Focus ends', silent: false }, { key: 'brk', label: 'Break ends', silent: true }],
    get: (k) => store[k] || null,
    set: (k, v) => { store[k] = v; },
    uploads: { brk: { name: 'mine.wav', pcm } },
    upload: () => Promise.resolve({ name: 'x', pcm }),
    play: (p, n) => played.push([p.length, n]), stop: () => stopped.push(1),
  });
  const heads = rows.filter((r) => r.isHead).map((r) => r.label);
  ok(heads.join() === 'Focus ends,Break ends', 'S.Sound: one heading per slot', heads.join());
  const sels = rows.filter((r) => r.isSelect);
  ok(sels.length === 2, 'S.Sound: one chime picker per slot');
  const focusOpts = sels[0].opts.map((o) => o.v), brkOpts = sels[1].opts.map((o) => o.v);
  ok(focusOpts.join() === 'rec:sonar,rec:bell', 'S.Sound: a slot with no upload lists only the generated recipes', focusOpts.join());
  ok(brkOpts.indexOf('silent') === 0 && brkOpts.indexOf('upload') > 0,
    'S.Sound: a silent-able slot with an upload lists Silent first and Yours · file after', brkOpts.join());
  ok(/Yours · mine\.wav/.test(sels[1].opts.find((o) => o.v === 'upload').label), 'S.Sound: the upload reads `Yours · <file name>`');
  const files = rows.filter((r) => r.isFile);
  ok(files.length === 2 && files[0].accept === 'audio/*', 'S.Sound: an upload row accepting audio/* per slot');
  const chips = rows.filter((r) => r.isChips);
  ok(chips.length === 2 && chips[0].opts.map((o) => o.label).join().indexOf('Play') >= 0, 'S.Sound: a row of Play / Stop chips');
  const rd = rows.filter((r) => r.label === 'In the theme');
  ok(rd.length === 2, 'S.Sound: one readout per slot');
  ok(/\d+\.\d s/.test(rd[0].valText) && /samples/.test(rd[0].valText) && /B in the theme/.test(rd[0].valText),
    'S.Sound: the readout prints seconds, samples and the ADPCM bytes', rd[0].valText);
  const wantBytes = G.soundBytes(G.soundEncode(pcm).enc, pcm.length);
  ok(rd[0].valText.indexOf(wantBytes + ' B') >= 0, 'S.Sound: the bytes are exactly soundBytes(soundEncode(pcm).enc, n)', rd[0].valText + ' want ' + wantBytes);
  ok(rd[1].valText.indexOf(wantBytes + ' B') >= 0, 'S.Sound: an upload’s readout reports the same byte count', rd[1].valText);
  chips[0].opts[0].pick();
  ok(played.length === 1 && played[0][0] === pcm.length, 'S.Sound: Play hands the app the rendered pcm (the kit opens no AudioContext)', JSON.stringify(played));
  chips[0].opts[1].pick();
  ok(stopped.length === 1, 'S.Sound: Stop calls the app’s stop');
  sels[0].on({ target: { value: 'rec:bell' } });
  ok(store.focus.rec === 'bell', 'S.Sound: choosing a recipe writes it back through o.set');
  const pinned = new Int16Array(2000); for (let i = 0; i < pinned.length; i++) pinned[i] = i % 3 ? 32767 : -32768;
  const warnRows = S.Sound(Kit, { recipes: [RECIPES.sonar], slots: [{ key: 'focus', label: 'F', silent: false }],
    get: () => ({ upload: true }), set: () => {}, uploads: { focus: { name: 'hot.wav', pcm: pinned } }, upload: () => {}, play: () => {}, stop: () => {} })
    .filter((r) => r.isNote);
  ok(warnRows.length === 1, 'S.Sound: a full-scale upload reports a warning row, and still keeps the slot', warnRows.length);
}

console.log((fails ? fails + ' FAILED of ' : '') + checked + ' checks');
process.exit(fails ? 1 : 0);


