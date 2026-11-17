#!/usr/bin/env node
// L0 hub step 4 (docs/specs/l0-hub-4-pomodoro.md): the pomodoro carries each owner sound as
// the orb's own file — GorbBake's §3.1 WAV data URL — at design schema 2, with one pure step
// from schema 1, and bakes that file's bytes without encoding them again.
//
// The suite loads the pomodoro's two editor modules in node, exactly as shipped, over the
// baker's kit: no browser. The bake needs a document with canvases, and what it draws is not
// what this suite is about — the stub answers with a zeroed buffer that putImageData writes
// back verbatim, so every art plane is the same bytes in every run and the SOUND planes (made
// from samples by the real baker) are the only sound matter that moves. No AudioContext: none
// of these checks plays anything.
//
//   node headless-pomodoro-sound.test.js
'use strict';
const fs = require('fs');
const path = require('path');
let fails = 0, checked = 0;
const ok = (c, n, x) => { checked++; console.log((c ? 'PASS ' : 'FAIL ') + n + (x ? '  [' + x + ']' : '')); if (!c) fails++; };

/* ---- the canvas the stub editor draws on: a buffer, and nothing else ----
 * Deterministic by construction: every drawing op is a no-op, getImageData answers the
 * canvas's own buffer, and putImageData (the font padEdges) writes it. measureText gives one
 * fixed metric box for every string, so the font stage builds real cells with real pixels. */
function mkCanvas() {
  const c = { width: 300, height: 150 };
  let buf = null, props = {}, fns = {};
  const ensure = () => { const need = c.width * c.height * 4; if (!buf || buf.length !== need) buf = new Uint8Array(need); };
  const ctx = new Proxy({}, {
    get(_, p) {
      if (p === 'getImageData') return () => { ensure(); return { data: new Uint8ClampedArray(buf), width: c.width, height: c.height }; };
      if (p === 'putImageData') return (im) => { ensure(); buf.set(im.data); };
      if (p === 'measureText') return () => ({ width: 10, actualBoundingBoxLeft: -1, actualBoundingBoxRight: 11,
        actualBoundingBoxAscent: 8, actualBoundingBoxDescent: 2, fontBoundingBoxAscent: 12, fontBoundingBoxDescent: 3 });
      if (p === 'createLinearGradient' || p === 'createRadialGradient') return () => ({ addColorStop() { } });
      if (p === 'canvas') return c;
      if (p in props) return props[p];
      return fns[p] || (fns[p] = function () { });
    },
    set(_, p, v) { props[p] = v; return true; },
  });
  c.getContext = () => ctx;
  return c;
}
global.window = globalThis;
global.document = { createElement: (t) => (String(t).toLowerCase() === 'canvas' ? mkCanvas() : {}) };

const G = global.GorbBake = require(path.join(__dirname, '..', 'baker@1', 'orbtheme-baker.js'));
const ROOT4 = path.join(__dirname, '..', '..', '..', '..', '..');
require(path.join(ROOT4, 'apps', 'pomodoro', 'editor', 'pomodoro-bake.js'));
require(path.join(ROOT4, 'apps', 'pomodoro', 'editor', 'pomodoro-editor.js'));
const P = global.PomodoroEditor, APP = P.app;
const FIX = path.join(ROOT4, 'apps', 'pomodoro', 'editor', '_fixtures');
const fixture = (n) => JSON.parse(fs.readFileSync(path.join(FIX, n), 'utf8'));
const clone = (o) => JSON.parse(JSON.stringify(o));
const decibels = (x, y) => { let m = 0; for (let i = 0; i < Math.max(x.length, y.length); i++) m = Math.max(m, Math.abs((x[i] || 0) - (y[i] || 0))); return m; };

/* The writers are counted at the module boundary: an open, a save or a bake that re-made a
 * file from samples would raise these numbers, and §3.1 forbids the re-encode. The baker's
 * OWN encoder is internal and cannot be spied from here, so "no soundEncode for it" is proven
 * with a file no encoder could have written: a tampered clip whose plane bytes are its file's,
 * not an encode of the samples they decode to. */
const enc = { file: 0, url: 0 };
for (const k of ['soundFile', 'soundFileUrl']) {
  const fn = G[k];
  G[k] = function () { enc[k === 'soundFileUrl' ? 'url' : 'file']++; return fn.apply(this, arguments); };
}
/* The sound stage's input, one level under the baker: wrapped where the bake reads it off the
 * module (GB.stage3), so the items handed to St.sounds are the items this suite asserts on. */
const stageCalls = [];
{
  const inner = G.stage3;
  G.stage3 = function () {
    const st = inner.apply(this, arguments), snd = st.sounds;
    st.sounds = function (items, opts) { stageCalls.push(items); return snd.call(this, items, opts); };
    return st;
  };
}

/* The session's own walk (theme@1's api.migrate): steps on deep copies, one schema at a time,
 * the answer stamped n+1. The pomodoro answers app.migrate through host@1, which reads nothing
 * but this object. */
function migrateDoc(doc) {
  let d = doc, n = +doc.schema;
  while (n < APP.schema) {
    const step = (APP.migrate || {})[n];
    if (typeof step !== 'function') throw new Error('no step from schema ' + n);
    d = Object.assign(step(JSON.parse(JSON.stringify(d))), { schema: n + 1 }); n++;
  }
  return d;
}

/* One bake, serial and stamp pinned: two designs that differ only in how they carry a sound
 * must answer the same container. */
const bakeOpts = { serial: 4242, bakedS: 1700000000 };
/* The SOUND row's clip, walked back out of a finished bake. */
const rowBytes = (bytes, slot) => {
  const w = G.verifyBundle3(bytes).sections[0];
  const row = w.blob.elements.filter((e) => e['class'] === G.classId('sound') && e.slot === slot)[0];
  const spr = w.blob.sprites[row.frame0];
  return { raw: w.art.subarray(spr.off, spr.off + spr.size), src: row.src };
};

(async () => {
  const D1 = fixture('schema-1.orbdesign'), D2 = fixture('schema-2.orbdesign');
  const SINE = G.soundPcm(D1.design.uploads[0].pcm);          // the fixture's 2,205 inline samples
  const W2 = D2.design.uploads[0].wav, CHIME = G.soundRead(W2);

  // ---- 1. the schema the editor reads, and the schema its manifest names ----------------
  {
    const json = JSON.parse(fs.readFileSync(path.join(FIX, '..', 'editor.json'), 'utf8'));
    ok(APP.schema === 2 && json.schema === 2 && Object.keys(APP.migrate).join() === '1'
      && typeof APP.migrate[1] === 'function',
      'the pomodoro reads design schema 2', 'app ' + APP.schema + ', editor.json ' + json.schema + ', one step');
  }

  // ---- 2. and 3. a pre-L0 design: opened with its sound, saved with a file in its place --
  let openedWav = '';
  {
    const m = migrateDoc(D1), u = m.design.uploads[0];
    const design = APP.fromDoc(m);
    const held = P.waves[design.uploads[0].ref];
    ok(m.schema === 2 && u.wav === G.soundFileUrl(SINE) && !('pcm' in u) && u.samples === 2205
      && held && held.wav === u.wav && decibels(held.pcm, CHIME.pcm) === 0
      && design.chimes.focus.upload === 0 && design.uploads[0].name === 'l0-chime.wav',
      'a pre-L0 design opens with its sound', 'wav ' + (u.wav || '').length + ' chars, ' + (held ? held.pcm.length : 0)
      + ' decoded, focus names upload ' + design.chimes.focus.upload);
    openedWav = u.wav;
    const doc = await APP.toDoc(design, true);
    const s = doc.design.uploads[0];
    ok(s.wav === openedWav && !('pcm' in s) && s.samples === 2205
      && Object.keys(s).sort().join() === 'name,ref,samples,wav',
      'its next save stores a file', Object.keys(s).join('+'));
    const again = APP.fromDoc({ design: { look: 'scope', uploads: [{ name: 'l0-chime.wav', ref: u.ref, samples: 2205 }] } });
    ok(again.uploads[0].ref === u.ref && P.waves[again.uploads[0].ref].wav === openedWav,
      'and the session’s held ref survives an open that carries no bytes');
  }

  // ---- 4. bytes that stay put: the same wav through three turns of the editor -----------
  {
    const e0 = enc.url + enc.file;
    const design = APP.fromDoc(clone(D2));
    const one = P.waves[design.uploads[0].ref].wav;
    const doc = await APP.toDoc(design, true);
    const two = doc.design.uploads[0].wav;
    const again = APP.fromDoc(clone(doc));
    const three = P.waves[again.uploads[0].ref].wav;
    const doc2 = await APP.toDoc(again, true);
    ok(one === W2 && two === W2 && three === W2 && doc2.design.uploads[0].wav === W2
      && enc.url + enc.file === e0,
      'an unchanged sound keeps its bytes across saves', 'writer calls ' + (enc.url + enc.file - e0));
    ok(decibels(P.waves[again.uploads[0].ref].pcm, CHIME.pcm) === 0,
      'and the sound it plays is the one its own file decodes to');
  }

  // ---- 5. the bake: the plane holds the file's own bytes, encoded nowhere ---------------
  {
    const design = APP.fromDoc(clone(D2));
    stageCalls.length = 0;
    const e0 = enc.url + enc.file;
    const bk = await APP.bake(design, bakeOpts);
    const item = stageCalls[0].filter((it) => it.slot === 1)[0];
    const rb = rowBytes(bk.bytes, 1);
    ok(item && item.coded && item.coded.enc === CHIME.enc && item.coded.body.length === CHIME.body.length
      && rb.raw.length === CHIME.body.length && decibels(rb.raw, CHIME.body) === 0 && rb.src === CHIME.enc
      && enc.url + enc.file === e0,
      'the bake uses the stored bytes', 'plane ' + rb.raw.length + ' B src ' + rb.src + ', writer calls ' + (enc.url + enc.file - e0));
    // the recipe chimes the same bake synthesizes are encoded as ever (slots 2 and 3)
    const recipes = stageCalls[0].filter((it) => it.slot !== 1);
    ok(recipes.length === 2 && recipes.every((it) => !it.coded && it.pcm.length > 1),
      'and the generated recipe chimes are handed in without a stored pair');
    // bytes only this file holds — one nibble of the codes turned — reach the plane as they
    // stand, and are NOT an encoder's answer for the samples they decode to: the proof that
    // the bake encoded nothing again for this chime (this sine happens to re-encode to its
    // own bytes, so the witness is a file no encoder could ever have written).
    const tb = Uint8Array.from(G.soundFile(SINE).bytes);
    tb[60 + 40] ^= 0x05;
    const TW = 'data:audio/wav;base64,' + Buffer.from(tb).toString('base64'), TR = G.soundRead(TW);
    const tam = clone(D2);
    tam.design.uploads[0] = { name: 'tampered.wav', ref: 'u5', samples: TR.pcm.length, wav: TW };
    stageCalls.length = 0;
    const bk2 = await APP.bake(APP.fromDoc(tam), bakeOpts);
    const rb2 = rowBytes(bk2.bytes, 1);
    ok(rb2.raw.length === TR.body.length && decibels(rb2.raw, TR.body) === 0
      && decibels(rb2.raw, G.soundEncode(TR.pcm).bytes) > 0,
      'bytes that only this file holds reach the plane as they stand', 'plane ' + rb2.raw.length + ' B src ' + rb2.src);
  }

  // ---- 6. the one step, run twice on copies, says the same thing ------------------------
  {
    const step = APP.migrate[1];
    const a = step(clone(D1)), b = step(clone(D1));
    const kept = clone(D1);
    const none = clone(D1); delete none.design.uploads;
    const n2 = step(clone(none));
    ok(JSON.stringify(a) === JSON.stringify(b) && JSON.stringify(D1) === JSON.stringify(kept)
      && JSON.stringify(n2) === JSON.stringify(none) && a.design.uploads[0].wav === W2
      && !('pcm' in a.design.uploads[0]),
      'the migration step is pure', 'schema-1 input untouched, no-uploads document equal, wav == fixture');
  }

  // ---- 7. a schema-2 file that still carries an inline sound ----------------------------
  {
    const leg = clone(D2);
    leg.design.uploads[0] = { name: 'legacy chime.wav', ref: 'u7', samples: SINE.length, pcm: Array.from(SINE) };
    const design = APP.fromDoc(leg);
    const held = P.waves[design.uploads[0].ref];
    ok(held && held.wav === G.soundFileUrl(SINE) && decibels(held.pcm, CHIME.pcm) === 0,
      'a legacy pcm still reads', 'wav ' + (held ? held.wav.length : 0) + ' chars, made once from the array');
    const obj = clone(D2);
    obj.design.uploads[0] = { name: 'json chime.wav', ref: 'u6', samples: 2205, pcm: JSON.parse(JSON.stringify(SINE)) };
    const d2 = APP.fromDoc(obj);
    ok(P.waves[d2.uploads[0].ref].wav === G.soundFileUrl(SINE),
      'and the JSON object form of a typed array reads too');
  }

  // ---- 8. bytes that do not read: the name stays, the slot owns no sound ----------------
  {
    const bad = clone(D2);
    bad.design.uploads[0] = { name: 'broken chime.wav', ref: 'u88', samples: 0, wav: 'data:audio/wav;base64,AAAA' };
    let design = null, why = 'x';
    try { design = APP.fromDoc(bad); } catch (e) { why = 'threw: ' + e.message; }
    const u = design && design.uploads[0];
    const held = design && P.waves[u.ref];
    const check = design && APP.check(design);
    let bakeThrew = false;
    if (design) { try { await APP.bake(design, bakeOpts); } catch (e) { bakeThrew = /upload/.test(String(e.message)); } }
    ok(why === 'x' && u && u.name === 'broken chime.wav' && u.samples === 0 && !held
      && typeof check === 'string' && /no wave for it/.test(check) && bakeThrew,
      'an unreadable sound keeps its name', String(why) + ' | ' + (u ? JSON.stringify(Object.keys(u)) : '—')
      + ' | check “' + check + '”' + (bakeThrew ? ' | bake refused it' : ' | bake ACCEPTED it'));
    const doc = await APP.toDoc(design, true);
    const s = doc.design.uploads[0];
    ok(s.name === 'broken chime.wav' && !('wav' in s) && !('pcm' in s),
      'and saving it again writes the name with nothing to read back');
  }

  // ---- 9. the theme a pre-L0 design baked is the theme this one bakes -------------------
  {
    const a = APP.fromDoc(migrateDoc(D1)), b = APP.fromDoc(clone(D2));
    const ba = await APP.bake(a, bakeOpts), bb = await APP.bake(b, bakeOpts);
    ok(ba.bytes.length === bb.bytes.length && decibels(ba.bytes, bb.bytes) === 0,
      'the bake does not change', ba.bytes.length + ' bytes both');
  }

  console.log(fails ? 'RESULT: FAIL — ' + fails + ' of ' + checked + ' checks' : 'all green (' + checked + ' checks)');
  process.exit(fails ? 1 : 0);
})();
