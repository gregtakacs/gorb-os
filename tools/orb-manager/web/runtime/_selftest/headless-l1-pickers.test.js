#!/usr/bin/env node
// L1 hub step 3 (§4.4, ruling 42): flights, weather and pomodoro take the owner's library
// pick verbatim.
//
// Step 1 gave studio@1's file row a `lib` side door; step 3 wired one behind every picture
// and sound picker. What a pick must never meet is any of the normalising the upload path
// does — a picture is not fitted, drawn or re-encoded, and a sound is not decoded and
// re-writ — because theme@1 names the file by its SHA-256 on the next save, so a touched
// byte is a second stored copy. This suite drives each editor's own fields() over the
// shipped runtime (baker@1, theme@1, studio@1) under the same node stubs the pomodoro's
// sound suite uses, picks through the row's own `lib`, and reads the design back.
//
//   node headless-l1-pickers.test.js
'use strict';
const fs = require('fs');
const path = require('path');
let fails = 0, checked = 0;
const ok = (c, n, x) => { checked++; console.log((c ? 'PASS ' : 'FAIL ') + n + (x ? '  [' + x + ']' : '')); if (!c) fails++; };
const ROOT = path.resolve(__dirname, '..', '..', '..', '..', '..');
const RUNTIME = path.join(__dirname, '..');

/* ---- the world the runtime suites share: a canvas that is a buffer, and no audio ---- */
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
global.React = { createRef: () => ({ current: null }), createElement: (type, props, ...kids) => ({ type, props: props || {},
  kids: kids.flat(Infinity).filter((k) => k != null && k !== false) }), useRef: (v) => ({ current: v }),
  useState: (v) => [v, () => {}], useEffect: () => {}, useLayoutEffect: () => {}, useMemo: (f) => f(),
  useCallback: (f) => f(), useReducer: (r, v) => [v, () => {}], forwardRef: (f) => f, Fragment: Symbol() };

const G = global.GorbBake = require(path.join(RUNTIME, 'baker@1', 'orbtheme-baker.js'));
require(path.join(RUNTIME, 'theme@1', 'send.js'));
require(path.join(RUNTIME, 'theme@1', 'session.js'));
require(path.join(RUNTIME, 'theme@1', 'preview.js'));
require(path.join(RUNTIME, 'theme@1', 'measure.js'));
require(path.join(RUNTIME, 'studio@1', 'studio.js'));

/* The writers counted at the module boundary: a verbatim path may not re-make a file from
 * samples (the same witnesses headless-pomodoro-sound.test.js installs). */
const enc = { file: 0, url: 0, encode: 0 };
for (const k of ['soundFile', 'soundFileUrl', 'soundEncode']) {
  const fn = G[k];
  G[k] = function () { enc[k === 'soundFileUrl' ? 'url' : k === 'soundEncode' ? 'encode' : 'file']++; return fn.apply(this, arguments); };
}

/* A small PNG, and the kit's own pick shape around it: sha256 over the exact bytes. */
const crypto = require('crypto');
const b64 = (u8) => Buffer.from(u8).toString('base64');
const crcT = (() => { const t = []; for (let n = 0; n < 256; n++) { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1; t[n] = c >>> 0; } return t; })();
const crc32 = (b) => { let c = 0xFFFFFFFF; for (const x of b) c = crcT[(c ^ x) & 255] ^ (c >>> 8); return (c ^ 0xFFFFFFFF) >>> 0; };
function chunk(type, data) {
  const t = Buffer.from(type, 'ascii'), len = Buffer.alloc(4); len.writeUInt32BE(data.length);
  const crc = Buffer.alloc(4); crc.writeUInt32BE(crc32(Buffer.concat([t, data])));
  return Buffer.concat([len, t, data, crc]);
}
function redPng(w, h) {
  const ih = Buffer.alloc(25); ih.writeUInt32BE(w, 0); ih.writeUInt32BE(h, 4); ih[8] = 8; ih[9] = 6;
  const raw = Buffer.alloc(h * (1 + w * 4));
  for (let y = 0; y < h; y++) { raw[y * (1 + w * 4)] = 0;
    for (let x = 0; x < w; x++) { const o = y * (1 + w * 4) + 1 + x * 4; raw[o] = 214; raw[o + 1] = 40; raw[o + 2] = 40; raw[o + 3] = 255; } }
  return Buffer.concat([Buffer.from([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A]),
    chunk('IHDR', ih), chunk('IDAT', require('zlib').deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]);
}
const PNG_BYTES = redPng(30, 30);
const PNG_URL = 'data:image/png;base64,' + b64(PNG_BYTES);
const picturePick = (name) => ({ ok: true, url: PNG_URL, name, sha256: crypto.createHash('sha256').update(PNG_BYTES).digest('hex'),
  kind: 'picture', mediaType: 'image/png' });

/* One editor session: the fields contract host@1 hands (c: {d, K, upd, set, base, say, getIn, …}),
 * with upd walking the dotted path the editor names and set replacing the whole design, both
 * landing on the same live object the assertions read. */
function session(design) {
  const box = { d: design };
  const put = (pathStr, v) => {
    const seg = String(pathStr).split('.');
    const own = seg[seg.length - 1], hold = seg.slice(0, -1).reduce((x, k) => (x == null ? x : x[k]), box.d);
    if (hold == null) throw new Error('upd: no such path ' + pathStr);
    hold[own] = v;
  };
  return box.c = {
    d: box.d, K: global.OrbStudio.fieldKit({}), base: JSON.parse(JSON.stringify(design)),
    upd: put, set: (fn) => { box.d = box.c.d = fn(box.d); }, msgs: [], say: (m) => box.c.msgs.push(m),
    getIn: (o, p) => String(p).split('.').reduce((x, k) => (x == null ? x : x[k]), o),
    view: { count: 3 }, ui: {}, env: {}, geo: {},
  };
}
const fileRows = (rows) => rows.filter((f) => f && f.isFile);

/* Counting every canvas the stub world builds: a pick that re-drew a picture through a
 * canvas would raise this, and the verbatim rule says the row never touches one. */
let canvases = 0;
{
  const inner = global.document.createElement;
  global.document.createElement = (t) => { if (String(t).toLowerCase() === 'canvas') canvases++; return inner(t); };
}

/* ---- flights: three picture rows, each storing the pick's url on its own path ---------- */
{
  const A = 'apps/flights/editor/';
  ['flights-bake.js', 'flights-map.js', 'flights-art.js', 'flights-editor.js'].forEach((f) => require(path.join(ROOT, A + f)));
  const E = globalThis.FlightsEditor;
  const base = () => JSON.parse(fs.readFileSync(path.join(ROOT, A, '_fixtures', 'schema-2.orbdesign'), 'utf8')).design;
  const rows = [
    ['scope', (d) => { d.scope.ground = 'image'; }, 'scope.image', ''],
    ['planes', (d) => { d.planes.family = 'custom'; }, 'planes.custom.image', 'Your plane: one picture for every aircraft, turned to its track'],
    ['frames', (d) => { d.frames.style = 'picture'; }, 'frames.image', ''],
  ];
  const seen = [];
  let verbatim = true, said = true;
  rows.forEach(([step, mut, at, sentence]) => {
    const design = base(); mut(design);
    const c = session(design);
    const fi = fileRows(E.app.fields(step, c))[0];
    seen.push([step + ':' + fi.label, fi]);
    if (!fi || typeof fi.lib !== 'function') { verbatim = false; return; }
    const cv0 = canvases, e0 = enc.file + enc.url + enc.encode;
    fi.lib(picturePick('flight-crest.png'));
    const held = String(at).split('.').reduce((x, k) => (x == null ? x : x[k]), c.d);
    if (held !== PNG_URL || canvases !== cv0 || enc.file + enc.url + enc.encode !== e0) verbatim = false;
    if (sentence && c.msgs[c.msgs.length - 1] !== sentence) said = false;
  });
  ok(verbatim, 'a flights library picture lands verbatim', '3 rows store pick.url, 0 canvases, 0 makers');
  ok(said, 'the flights custom-plane pick says what the upload says');
  ok(seen.length === 3 && seen.every(([w, f]) => typeof f.lib === 'function'),
    'every flights file row has a library pick', seen.map(([w]) => w).join(', ') + ' (scope keeps the kit default accept)');
}

/* ---- weather: the plate's picture row, the file path's own status line ----------------- */
{
  const A = 'apps/weather/editor/';
  ['weather-bake.js', 'fx.js', 'ground.js', 'wx-map.js', 'weather-editor.js'].forEach((f) => require(path.join(ROOT, A + f)));
  const E = globalThis.WeatherEditor;
  const design = JSON.parse(fs.readFileSync(path.join(ROOT, A, '_fixtures', 'schema-1.orbdesign'), 'utf8')).design;
  design.plate.img = null;
  const c = session(design);
  const fi = fileRows(E.app.fields('ground', c))[0];
  const cv0 = canvases, e0 = enc.file + enc.url + enc.encode;
  fi.lib(picturePick('wx-vane.png'));
  ok(c.d.plate.img === PNG_URL && c.d.plate.imgScale === 1 && c.d.plate.imgX === 0 && c.d.plate.imgY === 0
    && canvases === cv0 && enc.file + enc.url + enc.encode === e0,
    'a weather library picture lands verbatim', 'reset to scale 1 at the centre, ' + (canvases - cv0) + ' canvases');
  ok(fi && typeof fi.lib === 'function' && /The picture is the ground/.test(c.msgs.join(' ')),
    'every weather file row has a library pick', 'and the pick says the file path\u2019s line');
}

/* ---- pomodoro: the kit's sound rows, the pick through takeLibrary ---------------------- */
{
  const A = 'apps/pomodoro/editor/';
  require(path.join(ROOT, A, 'pomodoro-bake.js'));
  require(path.join(ROOT, A, 'pomodoro-editor.js'));
  const P = globalThis.PomodoroEditor, B = globalThis.PomodoroBake;
  const tone = (secs, hz) => { const n = Math.round(secs * G.SOUND_RATE), p = new Int16Array(n);
    for (let k = 0; k < n; k++) p[k] = Math.round(0.6 * 32767 * Math.sin(2 * Math.PI * (hz || 660) * k / G.SOUND_RATE) * (k % 4410 < 900 ? 1 : 0.2)); return p; };
  const wavPick = (name, secs) => { const url = G.soundFileUrl(tone(secs));
    return { ok: true, url, name, sha256: crypto.createHash('sha256').update(Buffer.from(url.split('base64,')[1], 'base64')).digest('hex'),
      kind: 'sound', mediaType: 'audio/wav' }; };
  const chimeRows = (c) => fileRows(P.app.fields('chimes', c));
  const own = () => chimeRows(session(JSON.parse(fs.readFileSync(path.join(ROOT, A, '_fixtures', 'schema-2.orbdesign'), 'utf8')).design));

  const design = JSON.parse(fs.readFileSync(path.join(ROOT, A, '_fixtures', 'schema-2.orbdesign'), 'utf8')).design;
  const c = session(design);
  const rows = chimeRows(c);
  const pk = wavPick('library-ding.wav', 1.0);               // (built first: the fixture's own encode is not the pick's)
  const e0 = enc.file + enc.url + enc.encode;
  rows[0].lib.pick(pk);                                     // the focus row's library button
  const up = (c.d.uploads || []).slice(-1)[0], held = P.waves[up && up.ref];
  const rd = G.soundRead(pk.url);
  ok(held && held.wav === pk.url && up.name === 'library-ding.wav' && c.d.chimes.focus.upload === (c.d.uploads.length - 1)
    && enc.file + enc.url + enc.encode === e0 && /\u201clibrary-ding\.wav\u201d is 1\.0 s of chime for the focus\u2019s end/.test(c.msgs.join(' ')),
    'a pomodoro library chime is kept verbatim', 'held ' + (held ? held.wav.length : 0) + ' chars, maker calls ' + (enc.file + enc.url + enc.encode - e0));
  ok(held && held.pcm && rd && held.pcm.length === rd.pcm.length && held.pcm.every((v, x) => v === rd.pcm[x]),
    'and what it plays is the file\u2019s own samples');

  const c2 = session(JSON.parse(fs.readFileSync(path.join(ROOT, A, '_fixtures', 'schema-2.orbdesign'), 'utf8')).design);
  const before = JSON.stringify(c2.d.uploads || []);
  const big = wavPick('long-way.wav', 4.5);
  chimeRows(c2)[0].lib.pick(big);
  ok(c2.msgs.join(' ') === 'Not taken: \u201clong-way.wav\u201d is 4.5 s: a pomodoro chime is at most 4.0 s'
    && JSON.stringify(c2.d.uploads || []) === before,
    'a pomodoro library chime over 4.0 s is refused with its length', c2.msgs.join(' '));

  const c3 = session(JSON.parse(fs.readFileSync(path.join(ROOT, A, '_fixtures', 'schema-2.orbdesign'), 'utf8')).design);
  chimeRows(c3)[0].lib.pick({ ok: true, url: 'data:audio/wav;base64,AAAA', name: 'rattle.wav', sha256: '0'.repeat(64), kind: 'sound', mediaType: 'audio/wav' });
  ok(c3.msgs.join(' ') === 'Not taken: that sound can\'t be read', 'a pomodoro pick that will not decode is refused', c3.msgs.join(' '));

  const all = [];
  ['ground', 'furniture', 'words', 'count', 'ring', 'chimes'].forEach((step) => {
    const cs = session(JSON.parse(fs.readFileSync(path.join(ROOT, A, '_fixtures', 'schema-2.orbdesign'), 'utf8')).design);
    fileRows(P.app.fields(step, cs)).forEach((f) => all.push(['chimes:' + step, f]));
  });
  ok(all.length === 3 && all.every(([w, f]) => f.lib && typeof f.lib.pick === 'function' && typeof f.lib.play === 'function'),
    'every pomodoro file row has a library pick', all.length + ' rows, one per phase end');
}

console.log((fails ? fails + ' FAILED of ' : 'all green (') + checked + ' checks)');
process.exit(fails ? 1 : 0);
