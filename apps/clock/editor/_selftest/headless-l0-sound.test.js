#!/usr/bin/env node
// L0 hub step 3 (§3, §4.2): the owner's sound is carried as the orb's own encoding.
//
// A clock editor's sound entry, a saved design, a reopened design, a migrated schema-1
// document and the plane written for the hub all speak the same one file: GorbBake's
// data URL. The ten checks below walk that ring in the order the plan draws it: open,
// play, keep, save, re-open, bake. The suite's own fixture (../_fixtures/schema-2.orbdesign)
// is read, never written here — docs/specs/l0-hub-fixtures.py verifies it against the
// schema-1 fixture instead, so the two files can never drift apart quietly.
'use strict';
const fs = require('fs');
const path = require('path');
require('./headless-stub.js');                              // jsdom, WatchEngine, the baker, the engine, the session
global.React = { createRef: () => ({ current: null }) };
require('../clock-logic.js');
const E = global.WatchEngine;
const G = global.GorbBake;
const SE = window.SoundEngine;
const DCLogic = function () { };
DCLogic.prototype.setState = function (p) { Object.assign(this.state, typeof p === 'function' ? p(this.state) : p); };
const Comp = global.ClockLogic(DCLogic);

let checked = 0, fails = 0;
const ok = (c, n, x) => { checked++; console.log((c ? 'PASS ' : 'FAIL ') + n + (x ? '  [' + x + ']' : '')); if (!c) fails++; };

/* A session over the skeleton dial, with the same no-op the other Component suites give
 * it: no art to walk, no plane to paint, and the snapshot the host hooks read held here
 * so the bake check can count the bakes it causes. */
function mkSession(over) {
  const i = new Comp({});
  i.ready = true;
  i.state.dial = 'skeleton';
  i.state.designs.skeleton = JSON.parse(JSON.stringify(E.PRESETS.skeleton.design));
  Object.assign(i.state.sounds, over || {});
  i.faces = {};
  i.buildMovement = () => {};
  i.download = (blob, name) => { i._file = { name, blob }; return name; };
  return i;
}
/* The baker's one file form, for the samples an entry below is written from: a second of
 * sine, which encodes as ADPCM (1,248 bytes: §3.1's header plus nine 132-byte blocks). */
const tone = (n, hz, amp) => { const p = new Int16Array(n); for (let k = 0; k < n; k++) p[k] = Math.round((amp === undefined ? 0.7 : amp) * 32767 * Math.sin(2 * Math.PI * (hz || 440) * k / SE.SR)); return p; };
const REF = tone(SE.SR, 440);
const REF_WAV = G.soundFileUrl(REF);
/* A sounds state written the way the editor holds one: the fixture's own, or one named here,
 * over the skeleton preset so every slot the design leaves alone still answers to a voice. */
function sfx(up, over) {
  const P = SE.PRESETS.skeleton;
  return Object.assign({ tick: P.tick, pattern: P.pattern, slots: Object.assign({}, P.slots), up: up, seed: 1 }, over || {});
}
/* Largest int apart from either of two byte runs — 0 for two runs the same bytes. */
const decibels = (x, y) => { let m = 0; for (let i = 0; i < Math.max(x.length, y.length); i++) m = Math.max(m, Math.abs((x[i] || 0) - (y[i] || 0))); return m; };
/* A bake, with a solid plate and one hand: everything the sounds ride beside. The same
 * words the sound suite bakes its own bank with (headless-sound.test.js), so the plane the
 * assertions below read out of the bytes is a plane the writer really wrote. */
const bakeOf = (sounds, pattern) => {
  const rgba = (w, h, v) => { const r = new Uint8Array(w * h * 4); for (let k = 0; k < w * h; k++) { r[k * 4] = v; r[k * 4 + 1] = 7; r[k * 4 + 2] = 11; r[k * 4 + 3] = 255; } return r; };
  return G.bakeClock3({ name: 'L0 Bake', serial: 20261117, bakedMs: 1730000000000,
    plate: { rgba: rgba(466, 466, 3), w: 466, h: 466 },
    hands: [{ slot: 0, rgba: rgba(39, 201, 1), x: 233, y: 233, w: 39, h: 201, pivot_x: 19, pivot_y: 190 }],
    order: [0], scalars: { second_sweep: 0, tick_rate: 1 }, sounds: sounds, soundPattern: pattern });
};
/* One Sound slot's clip, walked back out of a baked theme's bytes. */
const rowBytes = (bytes, slot) => {
  const w = G.verifyBundle3(bytes).sections[0];
  const row = w.blob.elements.filter((e) => e.class === G.CLASS_SOUND && G.SOUND_SLOTS[e.slot] === slot)[0];
  const spr = w.blob.sprites[row.frame0];
  return { raw: w.art.subarray(spr.off, spr.off + spr.size), src: row.src };
};
/* The host's own hooks (clock-host.js), loaded the way its suite loads them: enough React
 * for the module to build, and no mount, which the write view does not need. */
function loadHost() {
  if (global.ClockHost) return global.ClockHost.app;
  global.React = {
    createRef: () => ({ current: null }), createElement: () => ({}), Component: class { },
    useState: (v) => [v, () => { }], useEffect: () => { }, useRef: () => ({ current: null }),
    useMemo: (f) => f(), useCallback: (f) => f(), useReducer: (r, v) => [v, () => { }],
    useLayoutEffect: () => { }, forwardRef: (f) => f, Fragment: 'f',
  };
  global.ReactDOM = { createRoot: () => ({ render() { }, unmount() { } }), render() { }, flushSync: (f) => f() };
  require('../clock-host.js');
  return global.ClockHost.app;
}
/* Counting the baker's one encoder: a save or a bake that writes an owner's sound as it is
 * stored must not reach these at all, and a spy is the only witness of a call not made. */
const enc = { file: 0, url: 0, encode: 0 };
for (const k of ['soundFile', 'soundFileUrl', 'soundEncode']) {
  const fn = G[k];
  G[k] = function () { enc[k === 'soundFileUrl' ? 'url' : k === 'soundEncode' ? 'encode' : 'file']++; return fn.apply(this, arguments); };
}
const fixture = (n) => JSON.parse(fs.readFileSync(path.join(__dirname, '..', '_fixtures', n), 'utf8'));
function fileOf(n) {
  const b = fs.readFileSync(path.join(__dirname, '..', '_fixtures', n));
  return { name: n, arrayBuffer: async () => b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength) };
}
/* The one fake a browser has that this harness does not: what the click's file decodes to.
 * sndUpload reads the bytes, asks the AudioContext to decode them, and has the offline
 * render resample the result to the voice rate — so a fake of the two contexts, answering
 * with `secs` seconds of the clip the click named, is the same walk to the sample count
 * the bound is set on. The upload's own file is a stub: no suite here is testing codecs. */
async function upload(i, slot, secs, hz) {
  const SR = SE.SR, n = Math.round(secs * SR), x = new Float32Array(n);
  for (let k = 0; k < n; k++) x[k] = 0.6 * Math.sin(2 * Math.PI * (hz || 330) * k / SR);
  const dec = { duration: n / SR, numberOfChannels: 1, sampleRate: SR, length: n, getChannelData: () => x };
  const prev = { ctx: i.sndCtx, off: global.OfflineAudioContext, cut: i.state.sndCut };
  i.sndCtx = () => ({ decodeAudioData: async () => dec });
  global.OfflineAudioContext = function (ch, len) {
    this.destination = {};
    this.createBuffer = (c2, l2) => ({ length: l2, numberOfChannels: c2, getChannelData: () => x.subarray(0, l2) });
    this.createBufferSource = () => ({ buffer: null, connect() { }, start() { } });
    this.startRendering = async () => dec;
  };
  i.state.sndCut = false;
  try {
    await i.sndUpload({ target: { files: [{ name: 'marche.wav', arrayBuffer: async () => new Uint8Array(44).buffer }] } }, slot);
  } finally { i.sndCtx = prev.ctx; global.OfflineAudioContext = prev.off; i.state.sndCut = prev.cut; }
  return i.state.sndMsg;
}

(async () => {
  const D1 = fixture('schema-1.orbdesign'), D2 = fixture('schema-2.orbdesign');
  const W2 = D2.sounds.up.chime_hour.wav, CHIME = G.soundRead(W2);

  // ---- 1. the schema the clock reads, the schema its manifest names, and one step ------
  {
    const json = JSON.parse(fs.readFileSync(path.join(__dirname, '..', 'editor.json'), 'utf8'));
    global.window.__ORB_APP = { slug: 'clock', version: 'v1.8.1', schema: json.schema };
    const i = mkSession();
    const app = loadHost();
    i.session;                                               // the OS's session refuses to start when the two numbers disagree
    ok(i.DESIGN_SCHEMA === 2 && json.schema === 2 && app.schema === 2,
      'the clock reads design schema 2', 'clock ' + i.DESIGN_SCHEMA + ', editor.json ' + json.schema + ', host ' + app.schema);
    const step = app.migrate[1], own = i.DESIGN_MIGRATE[1], text = fs.readFileSync(path.join(__dirname, '..', 'clock-logic.js'), 'utf8');
    ok(Object.keys(i.DESIGN_MIGRATE).join() === '1' && Object.keys(app.migrate).join() === '1'
      && typeof step === 'function' && step.name === 'migrateSoundFiles'
      // the host and the class name the same definition: one function, written down once
      && String(step) === String(own) && (text.match(/\bmigrateSoundFiles\b/g) || []).length === 2
      && JSON.stringify(step(JSON.parse(JSON.stringify(D1)))) === JSON.stringify(own(JSON.parse(JSON.stringify(D1)))),
      'the step from schema 1 to schema 2 is one step, defined once for both sessions');
  }

  // ---- 2. and 3. a pre-L0 design: opened with its sound, saved with a file in its place --
  let openedWav = '';
  {
    const i = mkSession();
    const r0 = await i.openFile(fileOf('schema-1.orbdesign'));
    const held = i.state.sounds.skeleton;
    const D1PCM = G.soundPcm(D1.sounds.up.chime_hour.pcm);      // the schema-1 file holds its samples as a JSON object
    ok(r0 === true && held && held.up.chime_hour && held.up.chime_hour.name === 'l0-chime.wav'
      && held.up.chime_hour.pcm instanceof Int16Array && held.up.chime_hour.wav === G.soundFileUrl(D1PCM),
      'a pre-L0 design opens with its sound', 'wav ' + (held.up.chime_hour.wav || '').length + ' chars, pcm ' + (held.up.chime_hour.pcm || []).length);
    const doc = await i.designDoc(true);
    const up = doc.sounds.up;
    openedWav = up.chime_hour.wav;
    ok(doc.schema === 2 && up.chime_hour.wav === openedWav && !('pcm' in up.chime_hour)
      && Object.keys(up.chime_hour).sort().join() === 'loop,name,wav' && !/"pcm"/.test(JSON.stringify(up)),
      'its next save stores a file', Object.keys(up.chime_hour).join('+'));
    const app = loadHost();
    const d = app.fromDoc(JSON.parse(JSON.stringify(doc)));
    ok(d.sounds.upRef && Object.keys(d.sounds.up).length === 0 && d.sounds.upSig === 'chime_hour:l0-chime.wav:' + CHIME.pcm.length,
      'the host reads it as one sound, held under a ref and signed without its bytes', JSON.stringify(d.sounds.upSig));
    ok(JSON.stringify(d.sounds).indexOf('data:audio') < 0 && JSON.stringify(d.sounds).indexOf('"wav"') < 0,
      'the same snapshot hands the hub no file to carry twice');
  }

  // ---- 4. bytes that stay put: the same wav through three turns of the editor -----------
  {
    const i = mkSession();
    await i.openFile(fileOf('schema-2.orbdesign'));
    const one = i.state.sounds.skeleton.up.chime_hour.wav;
    const e0 = enc.url + enc.file;
    const doc = await i.designDoc(true);
    const two = doc.sounds.up.chime_hour.wav;
    ok(await i.applyDesignDoc(Object.assign({}, doc, { made: new Date().toISOString() }), 'round trip') === null, 'the saved document opens again');
    const three = (await i.designDoc(true)).sounds.up.chime_hour.wav;
    ok(one === W2 && two === W2 && three === W2 && enc.url + enc.file === e0,
      'an unchanged sound keeps its bytes across saves', 'url/file calls ' + (enc.url + enc.file - e0));
    const held = i.state.sounds.skeleton.up.chime_hour;
    ok(held.pcm instanceof Int16Array && decibels(held.pcm, CHIME.pcm) === 0,
      'and the sound it plays is the one its own file decodes to');
  }
  // ---- 5. the bake: the plane holds the file's own bytes, encoded nowhere --------------
  {
    const i = mkSession();
    await i.openFile(fileOf('schema-2.orbdesign'));
    const e0 = enc.encode + enc.file + enc.url;
    const sb = i.sndBake();
    const hour = sb.sounds.filter((s) => s.slot === 'chime_hour')[0];
    const bk = bakeOf(sb.sounds, sb.soundPattern);
    const rb = rowBytes(bk.bytes, 'chime_hour');
    ok(hour && hour.coded && hour.coded.enc === CHIME.enc && rb.raw.length === CHIME.body.length
      && decibels(rb.raw, CHIME.body) === 0 && rb.src === CHIME.enc && enc.encode + enc.file + enc.url === e0,
      'the bake uses the stored bytes', 'plane ' + rb.raw.length + ' B, encoder calls ' + (enc.encode + enc.file + enc.url - e0));
    // the same file with one nibble of its codes turned: the bake still ships those bytes
    const f = G.soundFile(REF), tb = Uint8Array.from(f.bytes);
    tb[60 + 40] ^= 0x05;
    const TW = 'data:audio/wav;base64,' + Buffer.from(tb).toString('base64'), TR = G.soundRead(TW);
    const i2 = mkSession({ skeleton: sfx({ chime_q1: Comp.soundEntry({ name: 'tampered.wav', loop: false, wav: TW }) }, { slots: { chime_q1: 'upload' }, pattern: 'none' }) });
    const sb2 = i2.sndBake();
    const t2 = sb2.sounds.filter((s) => s.slot === 'chime_q1')[0];
    const rt = rowBytes(bakeOf(sb2.sounds, sb2.soundPattern).bytes, 'chime_q1');
    ok(t2.coded && t2.coded.body.length === TR.body.length && decibels(rt.raw, TR.body) === 0
      && decibels(rt.raw, G.soundEncode(TR.pcm).bytes) > 0,
      'bytes that only this file holds reach the plane as they stand', 'plane ' + rt.raw.length + ' B src ' + rt.src);
    // the loop the editor DERIVES from an owner's tick has no file of its own: encode as ever
    const lp = SE.loopFromTick(CHIME.pcm, 4, 7);
    const i3 = mkSession({ skeleton: sfx({ tick_loop: Object.assign(Comp.soundEntry({ name: 'marche.wav', loop: false, wav: REF_WAV }), { pcm: lp }) }, { tick: 'upload', slots: {}, pattern: 'none' }) });
    const sb3 = i3.sndBake();
    const stick = sb3.sounds.filter((s) => s.slot === 'tick_loop')[0];
    const r3 = rowBytes(bakeOf(sb3.sounds, sb3.soundPattern).bytes, 'tick_loop');
    ok(!stick.coded && decibels(G.soundEncode(stick.pcm).bytes, r3.raw) === 0,
      'a derived loop is encoded by the bake as it always was');
  }

  // ---- 6. the one step, run twice on copies, says the same thing ----------------------
  {
    const i = mkSession();
    const a = Comp.migrateSoundFiles(JSON.parse(JSON.stringify(D1))), b = Comp.migrateSoundFiles(JSON.parse(JSON.stringify(D1)));
    const kept = JSON.parse(JSON.stringify(D1)), none = { format: 'wc-design', schema: 1, app: 'clock', dial: 'skeleton', design: { comps: [] } };
    const n2 = Comp.migrateSoundFiles(JSON.parse(JSON.stringify(none)));
    ok(JSON.stringify(a) === JSON.stringify(b) && JSON.stringify(D1) === JSON.stringify(kept)
      && JSON.stringify(n2) === JSON.stringify(none) && a.sounds.up.chime_hour.wav === W2
      && !('pcm' in a.sounds.up.chime_hour),
      'the migration step is pure', 'schema-1 input untouched, no-sounds document equal, wav == fixture');
  }

  // ---- 7. and 8. a schema-2 file that carries samples, and one whose bytes lie ---------
  {
    const base = fixture('schema-2.orbdesign');
    const leg = JSON.parse(JSON.stringify(base));
    leg.sounds.up.chime_q1 = { name: 'legacy chime.wav', loop: false, pcm: Array.from(REF) };
    const i = mkSession();
    ok(await i.applyDesignDoc(leg, 'legacy.orbdesign') === null, 'a schema-2 file with a sample array in it opens');
    const held = i.state.sounds.skeleton.up;
    ok(held.chime_q1.wav === G.soundFileUrl(REF) && decibels(held.chime_q1.pcm, REF) === 0,
      'a legacy pcm in a schema-2 file still reads', 'wav ' + (held.chime_q1.wav || '').length + ' chars');
    const bad = JSON.parse(JSON.stringify(base));
    delete bad.sounds.up.chime_q1;
    bad.sounds.up.chime_q1 = { name: 'broken chime.wav', loop: false, wav: 'data:audio/wav;base64,AAAA' };
    const j = mkSession();
    let why = 'x', sb = null;
    try { why = j.applyDesignDoc(bad, 'broken.orbdesign'); sb = j.sndBake(); } catch (e) { why = 'threw: ' + e.message; }
    const u = j.state.sounds.skeleton.up.chime_q1;
    const q1 = sb.sounds.filter((s) => s.slot === 'chime_q1')[0];
    ok(why === null && u.name === 'broken chime.wav' && u.pcm === null && !('wav' in u) && q1.coded === undefined,
      'an unreadable sound keeps its name', String(why) + ' | ' + JSON.stringify(Object.keys(u)) + ' | bake ' + (q1 ? 'holds its built-in' : 'silent'));
    const doc = await j.designDoc(true);
    ok(doc.sounds.up.chime_q1.name === 'broken chime.wav' && !('wav' in doc.sounds.up.chime_q1) && !('pcm' in doc.sounds.up.chime_q1),
      'and saving it again writes the name with nothing to read back');
  }

  // ---- 9. the door §2 sets, at the owner's answer: 20.1 seconds is refused in words, 19.9 goes in
  {
    const i = mkSession();
    const before = JSON.stringify(i.state.sounds.skeleton || null);
    const e0 = enc.file + enc.url + enc.encode;
    const msg = await upload(i, 'chime_q1', 20.1, 220);
    ok(msg === 'marche.wav is 20.1 s: a sound can be at most 20.0 s, so it was not taken'
      && JSON.stringify(i.state.sounds.skeleton || null) === before && enc.file + enc.url + enc.encode === e0,
      'a 20.1 s upload is refused with its length', msg);
    const msg2 = await upload(i, 'chime_q1', 19.9, 220);
    const u = i.state.sounds.skeleton.up.chime_q1;
    const rd = G.soundRead(u.wav);
    ok(/chime q1 \(19\.90 s, \d+ KB in the theme\)/.test(msg2) && u && u.loop === false && u.name === 'marche.wav'
      && rd && rd.pcm.length === u.pcm.length && decibels(rd.pcm, u.pcm) === 0,
      'a 19.9 s upload is taken', msg2);
  }

  // ---- 10. the theme a pre-L0 design baked is the theme this one bakes ----------------
  {
    const a = mkSession(), b = mkSession();
    await a.openFile(fileOf('schema-1.orbdesign'));
    await b.openFile(fileOf('schema-2.orbdesign'));
    const sa = a.sndBake(), s2 = b.sndBake();
    const ba = bakeOf(sa.sounds, sa.soundPattern), bb = bakeOf(s2.sounds, s2.soundPattern);
    ok(ba.bytes.length === bb.bytes.length && decibels(ba.bytes, bb.bytes) === 0,
      'the bake does not change', ba.bytes.length + ' bytes both');
  }

  console.log(fails ? 'RESULT: FAIL — ' + fails + ' of ' + checked + ' checks' : 'all green (' + checked + ' checks)');
  process.exit(fails ? 1 : 0);
})();
