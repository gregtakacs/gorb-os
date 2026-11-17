#!/usr/bin/env node
// L1 hub step 3 (§4.4, ruling 42): the clock's pickers take the owner's library verbatim.
//
// Step 1 gave studio@1's file row a `lib` side door; this suite walks what the clock built
// behind it. A picture pick goes through WatchEngine.assetFromUrl and must land with its
// bytes EXACTLY as the library holds them — no fitPicture, no re-draw, no stamping, no
// re-encode — because theme@1 names that file by its SHA-256 on the next save and any
// touched byte would be a second stored copy. A sound pick to a chime slot stores the
// library's own file the same way; a tick pick walks the post-decode path an upload walks,
// because a tick's loop is derived from the samples rather than carried by them. The last
// two checks read the built rows themselves: every picture row names PNG and SVG as its
// accept, and every file row the clock builds carries a library pick beside its upload.
'use strict';
const crypto = require('crypto');
require('./headless-stub.js');
const S = require('./headless-stub.js');                              // SIZES: the fake Image's fixture table
global.React = { createRef: () => ({ current: null }) };
require('../clock-logic.js');
const E = global.WatchEngine;
const G = global.GorbBake;
const SE = window.SoundEngine;

let checked = 0, fails = 0;
const ok = (c, n, x) => { checked++; console.log((c ? 'PASS ' : 'FAIL ') + n + (x ? '  [' + x + ']' : '')); if (!c) fails++; };
const DCLogic = function () { };
DCLogic.prototype.setState = function (p) { Object.assign(this.state, typeof p === 'function' ? p(this.state) : p); };
const Comp = global.ClockLogic(DCLogic);

/* A session, wired the way the component suites wire one (headless-l0-sound.test.js): no
 * art walk, no movement rebuild; the calendar dial, so the same design carries hands, four
 * backdrop slots and a flare this suite can switch to its image icon. */
const i = new Comp({});
i.ready = true;
i.state.dial = 'calendar';
i.state.designs.calendar = JSON.parse(JSON.stringify(E.PRESETS.calendar.design));
i.state.cfg = { tick: 'sweep' };                                       // the tick's beat rides the balance, as under a sweep
i.state.sndSim = false;
i.faces = {};
i.buildMovement = () => { };
i.forceUpdate = () => { };
i.budget = () => ({ fps: 8, dev: { pace: 8, cross: 0, cov: 0, why: '' } });   // renderVals reads it for rate labels

/* The library's own files. The baker encodes with its bundled ADPCM writer, so the WAV is a
 * real GorbBake file — the same form a library row hands over — and the PNG fixture is a
 * real 30x30 red raster. Every fake Image url registers in SIZES so the decode answers at
 * its intrinsic size, and the pick's sha256 is the digest of the exact bytes behind `url`. */
const b64 = (u8) => Buffer.from(u8).toString('base64');
const crcT = (() => { const t = []; for (let n = 0; n < 256; n++) { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1; t[n] = c >>> 0; } return t; })();
const crc32 = (b) => { let c = 0xFFFFFFFF; for (const x of b) c = crcT[(c ^ x) & 255] ^ (c >>> 8); return (c ^ 0xFFFFFFFF) >>> 0; };
function chunk(type, data) {
  const t = Buffer.from(type, 'ascii'), len = Buffer.alloc(4); len.writeUInt32BE(data.length);
  const crc = Buffer.alloc(4); crc.writeUInt32BE(crc32(Buffer.concat([t, data])));
  return Buffer.concat([len, t, data, crc]);
}
function redPng() {
  const ih = Buffer.alloc(25); ih.writeUInt32BE(30, 0); ih.writeUInt32BE(30, 4); ih[8] = 8; ih[9] = 6;
  const raw = Buffer.alloc(30 * (1 + 30 * 4));
  for (let y = 0; y < 30; y++) { raw[y * (1 + 120)] = 0;
    for (let x = 0; x < 30; x++) { const o = y * (1 + 120) + 1 + x * 4; raw[o] = 214; raw[o + 1] = 40; raw[o + 2] = 40; raw[o + 3] = 255; } }
  return Buffer.concat([Buffer.from([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A]),
    chunk('IHDR', ih), chunk('IDAT', require('zlib').deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]);
}
function pickOf(url, name, mime, bytes) {
  S.SIZES[url] = [30, 30];
  const sha256 = crypto.createHash('sha256').update(bytes).digest('hex');
  return { ok: true, url, name, sha256, kind: mime === 'image/png' ? 'picture' : 'sound', mediaType: mime };
}
const PNG_BYTES = redPng();
const PNG_URL = 'data:image/png;base64,' + b64(PNG_BYTES);
const PNG_PICK = pickOf(PNG_URL, 'red-crest.png', 'image/png', PNG_BYTES);
const SVG_TEXT = '<svg xmlns="http://www.w3.org/2000/svg" width="30" height="30"><rect width="30" height="30" fill="#2255aa"/></svg>';
const SVG_URL = 'data:image/svg+xml;base64,' + b64(Buffer.from(SVG_TEXT, 'utf8'));
const SVG_PICK = pickOf(SVG_URL, 'azure-vane.svg', 'image/svg+xml', Buffer.from(SVG_TEXT, 'utf8'));
/* Two seconds of a beat every half second — a tick's own rhythm, in the library's form. */
function tickWav() {
  const n = 2 * SE.SR, pcm = new Int16Array(n);
  for (let k = 0; k < n; k++) {
    const ph = (k % SE.SR) / SE.SR, env = ph < 0.04 ? 1 - ph / 0.04 : 0;
    pcm[k] = Math.round(9000 * env * Math.sin(2 * Math.PI * 2200 * k / SE.SR));
  }
  return { url: G.soundFileUrl(pcm), pcm };
}
/* The baker's writers counted at the module boundary: a verbatim path may not re-make a
 * file from samples (l0-sound's witnesses, the same spies the L0 suite installs). */
const enc = { file: 0, url: 0, encode: 0 };
for (const k of ['soundFile', 'soundFileUrl', 'soundEncode']) {
  const fn = G[k];
  G[k] = function () { enc[k === 'soundFileUrl' ? 'url' : k === 'soundEncode' ? 'encode' : 'file']++; return fn.apply(this, arguments); };
}

const flush = () => new Promise((r) => setTimeout(r, 25));

(async () => {
  // ---- 1. and 2. pictures: the bytes move, and nothing else happens to them ------------
  {
    const a = await E.assetFromUrl(PNG_PICK);
    ok(!a.error && a.dataUrl === PNG_URL && String(a.sha).length === 8 && a.w === 30 && a.h === 30
      && a.id === 'red-crest-' + PNG_PICK.sha256.slice(0, 8),
      'a library picture lands verbatim', 'id ' + a.id + ', ' + E.assetBytesOf(a.dataUrl).length + ' B, sha32 ' + a.sha);
    const b = await E.assetFromUrl(SVG_PICK);
    ok(!b.error && b.dataUrl === SVG_URL && b.kind === 'svg' && b.el && b.el._src === SVG_URL
      && b.id === 'azure-vane-' + SVG_PICK.sha256.slice(0, 8),
      'a library SVG lands verbatim', 'no stamping: ' + b.dataUrl.length + ' chars in, the same out');
  }

  // ---- 3. a mediaType the dial cannot hold is refused, in the file path's words ---------
  {
    const bad = await E.assetFromUrl({ ok: true, url: PNG_URL, name: 'x.jpg', sha256: PNG_PICK.sha256, kind: 'picture', mediaType: 'image/jpeg' });
    const none = await E.assetFromUrl({ ok: true, url: '', name: 'gone.png', sha256: '0'.repeat(64), kind: 'picture', mediaType: 'image/png' });
    ok(bad && bad.error && /image\/png and image\/svg\+xml/.test(bad.error) && none && none.error,
      'a jpeg pick is refused by the clock', bad.error);
  }

  // ---- 4. and 5. a chime pick: the library's file in the slot, and the same file in the save
  {
    const wav = G.soundFileUrl(tickWav().pcm);                          // any short clip: this pick goes to a chime slot
    const pk = { ok: true, url: wav, name: 'library-chime.wav', sha256: crypto.createHash('sha256').update(E.assetBytesOf(wav)).digest('hex'), kind: 'sound', mediaType: 'audio/wav' };
    const e0 = enc.file + enc.url + enc.encode;
    i.sndLibrary(pk, 'chime_hour');
    const d = i.sndDesign();
    ok(d.up.chime_hour && d.up.chime_hour.wav === wav && d.slots.chime_hour === 'upload'
      && d.up.chime_hour.name === 'library-chime.wav' && d.up.chime_hour.loop === false
      && enc.file + enc.url + enc.encode === e0 && /chime hour \(\d+\.\d\d s, \d+ KB in the theme\)/.test(i.state.sndMsg),
      'a library chime lands verbatim', i.state.sndMsg + ' | maker calls ' + (enc.file + enc.url + enc.encode - e0));
    const doc = await i.designDoc(true);
    ok(doc.sounds.up.chime_hour && doc.sounds.up.chime_hour.wav === wav && !('pcm' in doc.sounds.up.chime_hour),
      'a library chime survives a save', 'wav ' + (doc.sounds.up.chime_hour.wav || '').length + ' chars, unchanged');
  }

  // ---- 6. a tick pick takes the upload's post-decode path: a derived loop, a new file ---
  {
    const t = tickWav();
    const pk = { ok: true, url: t.url, name: 'library-tick.wav', sha256: crypto.createHash('sha256').update(E.assetBytesOf(t.url)).digest('hex'), kind: 'sound', mediaType: 'audio/wav' };
    const e0 = enc.encode + enc.file;
    i.sndLibrary(pk, 'tick_loop');
    const d = i.sndDesign(), u = d.up.tick_loop;
    ok(d.tick === 'upload' && u && u.loop === true && typeof u.rate === 'number' && typeof u.first === 'number'
      && u.wav && u.wav !== t.url && enc.encode + enc.file > e0
      && /tick loop \(\d+\.\d\d s, \d+ KB in the theme\)/.test(i.state.sndMsg),
      'a library tick becomes a loop like an upload', (u && u.wav ? 'derived file, loop at ' + u.rate + ' Hz, ' : '') + i.state.sndMsg);
    /* The same samples through the upload's front door, on a fresh instance: identical
     * derived loop parameters prove the SAME post-decode path ran (sndAdopt), which is what
     * §4.4 asks the refactor to guarantee. */
    const j = new Comp({});
    Object.assign(j, { ready: true, faces: {}, buildMovement: () => { }, forceUpdate: () => { } });
    j.state.dial = 'calendar'; j.state.designs.calendar = JSON.parse(JSON.stringify(E.PRESETS.calendar.design));
    j.state.cfg = { tick: 'sweep' }; j.state.sndSim = false;
    const f32 = () => Float32Array.from(t.pcm, (v) => v / 32768);
    const dec = { duration: t.pcm.length / SE.SR, numberOfChannels: 1, sampleRate: SE.SR, length: t.pcm.length, getChannelData: f32 };
    j.sndCtx = () => ({ decodeAudioData: async () => dec });
    global.OfflineAudioContext = function (ch, len) {
      this.destination = {};
      this.createBuffer = (c2, l2) => ({ length: l2, numberOfChannels: c2, getChannelData: () => f32().subarray(0, l2) });
      this.createBufferSource = () => ({ buffer: null, connect() { }, start() { } });
      this.startRendering = async () => dec;
    };
    await j.sndUpload({ target: { files: [{ name: 'library-tick.wav', arrayBuffer: async () => new Uint8Array(44).buffer }] } }, 'tick_loop');
    const u2 = j.sndDesign().up.tick_loop;
    ok(u2 && u2.loop === true && u2.rate === u.rate && u2.first === u.first,
      'the upload and the pick derive the same loop', 'rate ' + (u2 && u2.rate) + ' first ' + (u2 && u2.first));
  }

  // ---- 7. and 8. the built rows: PNG and SVG only, and a library side door on every one -
  {
    const labels = (tab) => { i.state.tab = tab; i.state.step = null; i.state.sel = null; i.state.selKind = null; return i.renderVals().fields; };
    const selTab = (kind, id, tab) => { i.state.tab = tab; i.state.step = 'parts'; i.state.sel = id; i.state.selKind = kind; return i.renderVals().fields; };
    const files = [];
    const collect = (tag, rows) => (rows || []).filter((f) => f && f.isFile).forEach((f) => files.push([tag + ':' + f.label, f]));
    ['dial', 'insides'].forEach((t) => collect(t, labels(t)));
    labels('sound').forEach((f) => collect('sound', [f]));
    // a hand sprite row: the hands tab's panel for a hand this session has art on
    const hands = i.state.designs.calendar.comps.filter((c) => c.type === 'hand');
    const ah = hands.find((h) => h.art) || hands[0];
    ah.art = ah.art || 'l1-sprite';
    E.assetRestore([{ id: 'l1-sprite', name: 'sprite.png', kind: 'png', dataUrl: PNG_URL, sha: PNG_PICK.sha256.slice(0, 8) }]);
    i.state.sndSim = false;
    collect('hand', selTab('hand', ah.id, 'hands'));
    // an image component's row: a flare comp switched to the image icon, then selected
    const comps = i.state.designs.calendar.comps;
    const flare = comps.find((c) => c.type === 'flare')
      || (comps.push({ id: 'l1-flare', type: 'flare', icon: 'text', x: 0, y: 150 }), comps.slice(-1)[0]);
    flare.icon = 'image';
    collect('comp', selTab('comp', flare.id, 'comps'));
    const pic = [], snd = [];
    files.forEach(([w, f]) => ((f.accept === 'audio/*' ? snd : pic).push([w, f])));
    ok(pic.length >= 4 && snd.length >= 5 && pic.every(([w, f]) => f.accept === 'image/png,image/svg+xml'),
      'the clock\'s picture rows offer only png and svg', pic.length + ' picture rows, ' + snd.length + ' sound rows');
    ok(files.every(([w, f]) => f.lib && typeof (f.lib.pick || f.lib) === 'function')
      && snd.every(([w, f]) => typeof f.lib.play === 'function'),
      'every clock file row has a library pick', files.length + ' file rows: ' + files.map(([w]) => w).join(', '));
  }

  console.log(fails ? 'RESULT: FAIL — ' + fails + ' of ' + checked + ' checks' : 'all green (' + checked + ' checks)');
  process.exit(fails ? 1 : 0);
})();
