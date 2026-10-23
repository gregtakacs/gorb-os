#!/usr/bin/env node
// Mirror suite for the composer's in-browser .orbtheme baker.
//
// The container format now has three implementations: tools/orbtheme.py (the definition),
// the device reader in main/art/orb_theme.c (pinned by tools/host_sd_test.py), and
// tools/watch-composer/orbtheme-baker.js, which lets the studio ship a theme a person can
// copy onto a card without running the bench pipeline. Nothing compiles the JS against the
// Python, so this suite does it instead: orbtheme_vectors.py packs a sentinel model with
// the Python and dumps every input plane as raw RGBA, and the checks below require the JS
// to reproduce the geometry blob, the pixel bank, the composited layers, and the whole
// container byte for byte.
//
// A mismatch here is not a cosmetic failure. A field reordered on one side draws a dial
// whose hand pivots read as overlay offsets — bad art, not an error message — and a bank
// whose rects do not replay leaves a stale rectangle frozen on the glass forever.
//
//   ~/.local/share/nodeenv-gorb/bin/node tools/watch-composer/_selftest/headless-orbtheme.test.js
//
// Needs the bake venv (pillow + numpy) for the reference run:
//   python3 -m venv /tmp/bakevenv && /tmp/bakevenv/bin/pip install pillow numpy
'use strict';
const { spawnSync } = require('child_process');
const fs = require('fs');
const path = require('path');
const B = require('../orbtheme-baker.js');

const ROOT = path.resolve(__dirname, '..', '..', '..');
const OUT = process.env.ORBTHEM_VEC || '/tmp/orbtheme_vectors';
const PY = process.env.PY || '/tmp/bakevenv/bin/python3';
let fails = 0, checked = 0;
const ok = (c, n, x) => { checked++; console.log((c ? 'PASS ' : 'FAIL ') + n + (x ? '  [' + x + ']' : '')); if (!c) fails++; };
const eq = (a, b, n) => ok(JSON.stringify(a) === JSON.stringify(b), n, JSON.stringify(a).slice(0, 90) + ' vs ' + JSON.stringify(b).slice(0, 90));
// Node pools small file reads into a shared ArrayBuffer, so a Buffer's .buffer is not the
// file: copy into a fresh array or the planes read back shifted by the pool offset.
const plane = (f, w, h) => ({ w: w, h: h, rgba: Uint8Array.from(fs.readFileSync(path.join(OUT, f))) });
const hex = (u8) => Buffer.from(u8.buffer.slice(u8.byteOffset, u8.byteOffset + u8.length)).toString('hex');

// ---- the reference, from the Python that defines the format ----------------
const run = spawnSync(process.env.VEC_PY || PY, [path.join(__dirname, 'orbtheme_vectors.py'), OUT],
                      { encoding: 'utf8', maxBuffer: 1 << 26 });
if (run.status !== 0) {
  console.log('the reference vectors could not be produced:\n' + (run.stderr || run.stdout).split('\n').slice(0, 12).join('\n'));
  console.log('0 checks run — install the bake venv (see the header) and re-run');
  process.exit(1);
}
const M = JSON.parse(fs.readFileSync(path.join(OUT, 'units.json'), 'utf8'));

// ---- 1. the geometry blob --------------------------------------------------
const jsGeom = B.packGeom({
  scalars: M.scalars, hands: M.handRecs, shadows: M.shadowRecs, overlay: M.overlayRec,
  tourbillon: M.tourbillonRecs, rects: M.bank.rects, rectFirst: M.bank.first,
  gifDelays: M.bank.delays, bgFrames: [0], comps: M.comps, aux: M.auxRecs, bank: []
});
ok(hex(jsGeom) === M.geomHex, 'geometry blob matches the Python byte for byte',
   'js ' + jsGeom.length + ' B vs py ' + M.geomSize + ' B');
ok(B.crc32(jsGeom) === M.geomCrc, 'geometry CRC matches');

// ---- 2. the pixel bank, packed in bake_clock.py's blob order --------------
function packArt() {
  const bl = B.blob();
  const frames = M.frames.map((f, i) => plane(f, 466, 466));
  const bank = B.bakeBgBank(bl, frames, { fps: M.fps, diffThresh: M.diffThresh, denoise: M.denoise,
                                          maxRects: M.maxRects, maxFlush: M.maxFlush, occl: null, matte: null });
  const eqRect = (name, a, b) => ok(JSON.stringify(a) === JSON.stringify(b), name, a.length + ' vs ' + b.length);
  eqRect('bank keyframe offset', bank.keyOff, M.bank.keyOff);
  eqRect('bank rect list', bank.rects, M.bank.rects);
  eqRect('bank rect index', bank.first, M.bank.first);
  eqRect('bank per-frame delays', bank.delays, M.bank.delays);
  ok(bank.frames === M.bank.frames, 'bank frame count', bank.frames + ' vs ' + M.bank.frames);

  function sprite(s) {
    const p = plane(s.file, s.w, s.h);
    const crop = B.cropLayer(p.rgba, p.w, p.h, M.pad);
    const ox = crop ? crop.offx : 0, oy = crop ? crop.offy : 0;
    const px = s.pivot ? s.pivot[0] : ox, py = s.pivot ? s.pivot[1] : oy;
    const cx = s.centre ? s.centre[0] : ox, cy = s.centre ? s.centre[1] : oy;
    if (!crop) return B.blankSprite(cx, cy, 0);
    return B.emitSprite(bl, crop, ox, oy, px, py, cx, cy, s.blend, s.show);
  }
  const ov = sprite(M.overlaySpec);
  const handR = M.handSpecs.map(s => sprite(s));
  const shadowR = M.shadowSpecs.map(s => sprite(s));
  const tb = M.tourbillon.map(f => plane(f, 466, 466));
  const tbRecs = B.bakeTourbillon(bl, tb, { pivot: [233, 233], centre: [233, 233], pad: M.pad, diffThresh: M.diffThresh });
  M.tiles.forEach(t => B.packWindowTile(bl, plane(t.file, t.w, t.h)));
  const aux = M.auxSpec, ap = plane(aux.file, 70, 26);
  const ac = B.cropLayer(ap.rgba, ap.w, ap.h, M.pad);
  B.emitSprite(bl, ac, ac.offx, ac.offy, aux.pivot[0], aux.pivot[1], aux.centre[0], aux.centre[1], 0, true);
  return { bl: bl, overlay: ov, hands: handR, shadows: shadowR, tourbillon: tbRecs };
}
const packed = packArt();
const bl = packed.bl;
ok(bl.len === M.artSize, 'pixel bank length matches', bl.len + ' vs ' + M.artSize);
const art = bl.join();
ok(B.crc32(art) === M.artCrc, 'pixel bank CRC matches (quantisation, crop-to-ink, rect payloads)');
ok(JSON.stringify(packed.hands) === JSON.stringify(M.handRecs), 'hand records match the Python',
   JSON.stringify(packed.hands[0]) + ' vs ' + JSON.stringify(M.handRecs[0]));
ok(JSON.stringify(packed.shadows) === JSON.stringify(M.shadowRecs), 'shadow records match the Python');
ok(JSON.stringify(packed.overlay) === JSON.stringify(M.overlayRec), 'overlay record matches');
ok(JSON.stringify(packed.tourbillon) === JSON.stringify(M.tourbillonRecs), 'carriage diff rects match the Python',
   JSON.stringify(packed.tourbillon[0] || {}).slice(0, 80));
// ---- 3. the whole file -----------------------------------------------------
const cont = B.writeContainer({ geom: jsGeom, art: art, scrW: M.scrW, scrH: M.scrH,
                                serial: M.serial, bakedMs: M.bakedMs, name: M.name });
const ref = fs.readFileSync(path.join(OUT, 'container.bin'));
ok(Buffer.from(cont).equals(ref), 'the container is the same file the bench produces',
   'js ' + cont.length + ' B vs py ' + ref.length + ' B');
const dv = new DataView(cont.buffer, cont.byteOffset, cont.byteLength);
const wordStr = (base, nbytes) => {
  let s = '';
  for (let i = 0; i < nbytes; i++) {
    const c = (dv.getUint32((base + (i / 4 | 0)) * 4, true) >>> (8 * (i % 4))) & 0xFF;
    if (!c) break;
    s += String.fromCharCode(c);
  }
  return s;
};
// The header fields the /themes page and the card actually use.
ok(dv.getUint32(0, true) >>> 0 === 0x474f5254, 'magic');
ok(dv.getUint32(4, true) === 2 && B.FORMAT === 2,
   'format 2: the one value that decides whether main/art/orb_theme.c reads this file at all');
ok(dv.getUint32(8, true) === 1, 'geometry ABI');
// FORMAT 2's identity rule in one row. The name is the ONLY thing that names a theme now
// — the card filename is derived from it — so the field is compared as raw bytes against
// the bench's own, not through a JS string: the sentinel name carries an em dash, and a
// writer that mangled the UTF-8 would still print something close.
const field = Buffer.from(cont.buffer, cont.byteOffset + B.W_NAME * 4, B.NAME_MAX);
ok(field.equals(Buffer.from(ref.buffer, ref.byteOffset + B.W_NAME * 4, B.NAME_MAX)),
   'the name travels in the header as the same bytes the bench wrote',
   Buffer.from(M.name).toString('hex').slice(0, 24) + ' vs ' + field.toString('hex').slice(0, 24));
ok(M.nameBytes === Buffer.byteLength(M.name, 'utf8') && M.nameBytes <= B.NAME_MAX - 1 &&
   M.nameBytes > Buffer.byteLength(M.name, 'latin1'),
   'the sentinel is multibyte and inside the field, so the row above is a byte compare',
   M.nameBytes + ' bytes in ' + M.name.length + ' characters');
ok(field[M.nameBytes] === 0 && field[M.nameBytes + 1] === 0,
   'the field keeps its NUL: setWord cuts at NAME_MAX - 1, and the C reader stops there');
const sha256 = (s) => new Uint8Array(require('crypto').createHash('sha256').update(s, 'utf8').digest());
ok(wordStr(B.W_NAME, B.NAME_MAX).slice(0, 6) === 'Mirror',
   'the field reads back as the reader reads it: little-endian bytes out of words 14 up');
ok(B.themeIdFromDigest(sha256(M.name)) === M.themeId,
   'the JS id rule matches orbtheme.theme_id(): three implementations, one identity',
   B.themeIdFromDigest(sha256(M.name)) + ' vs ' + M.themeId);
ok(B.themeIdFromDigest(new Uint8Array(5)) === null && B.themeIdFromDigest(null) === null,
   'a short or missing digest is refused, not padded into an id');
ok(dv.getUint32(44, true) === M.serial, 'serial');
ok(dv.getUint32(20, true) === M.geomSize, 'geom_size');
ok(dv.getUint32(28, true) === M.artSize, 'art_size');
ok(dv.getUint32(52, true) === ((128 + M.geomSize + 4095) & ~4095), 'the payload starts on a sector');

// ---- 4. the layer merge maths, against PIL ---------------------------------
// The sub-dial merge and the tint merge are alpha_composite calls; a rounding difference
// there is a layer a shade off the studio, which is exactly what nobody notices until a
// theme is on a dial in somebody's hall.
const cd = plane(M.composite.dst, M.composite.w, M.composite.h), cs = plane(M.composite.src, M.composite.w, M.composite.h);
const merged = B.copyPlane(cd);
B.compositeOver(merged, cs);
const want = fs.readFileSync(path.join(OUT, M.composite.out));
let diff = 0, first = -1;
for (let i = 0; i < want.length; i++) if (merged.rgba[i] !== want[i]) { if (first < 0) first = i; diff++; }
ok(diff === 0, 'the alpha_composite port reproduces PIL texel for texel', diff + ' bytes differ, first at ' + first);



// ---- 5. the gates the device would apply anyway ----------------------------
// A theme the firmware refuses is worse than no export at all: it uploads, lists as
// loadable, and then will not stage. These are the caps main/art/orb_theme.c enforces.
const throws = (fn, has, name) => {
  let err = null;
  try { fn(); } catch (e) { err = e; }
  ok(!!err && (!has || has.test(err.message)), name, err ? err.message.slice(0, 90) : 'no error raised');
};
const baseModel = () => ({
  scalars: Object.assign({}, M.scalars), hands: M.handRecs.slice(), shadows: M.shadowRecs.slice(),
  overlay: M.overlayRec, tourbillon: M.tourbillonRecs, rects: M.bank.rects.slice(),
  rectFirst: M.bank.first.slice(), gifDelays: M.bank.delays.slice(), bgFrames: [0],
  comps: [], aux: [], bank: []
});
throws(() => { const m = baseModel(); m.scalars.tick_rate = 300; B.packGeom(m); }, /tick_rate/, 'a tick rate past a u8 is refused, not wrapped');
throws(() => { const m = baseModel(); m.scalars.shadow_dy = 200; B.packGeom(m); }, /shadow_dy/, 'a shadow offset past i8 is refused (it would land two thousand px off the dial)');
throws(() => { const m = baseModel(); m.rects = m.rects.slice(0, 3); B.packGeom(m); }, /quintuples/, 'a ragged rect list is refused');
throws(() => { const m = baseModel(); m.aux = new Array(9).fill(M.auxRecs[0]); B.packGeom(m); }, /cap/, 'nine aux rows exceed the dial cap');
throws(() => { const m = baseModel(); m.comps = new Array(17).fill(M.comps[0]); B.packGeom(m); }, /cap/, 'seventeen complications exceed the dial cap');
throws(() => { const m = baseModel(); m.bank = new Array(257).fill(baseModel().hands[0]); B.packGeom(m); }, /cap/, 'two hundred fifty-seven bank rows exceed the cap');
throws(() => B.writeContainer({ geom: jsGeom, art: new Uint8Array(128), name: 'x' }),
       /shorter than one full plate/, 'a payload that cannot hold a plate is refused');
throws(() => B.writeContainer({ geom: jsGeom, art: new Uint8Array(B.SLOT_BUDGET + 8), name: 'x' }),
       /over the/, 'a payload bigger than a theme slot is refused before it is ever offered');
throws(() => B.writeContainer({ geom: jsGeom, art: new Uint8Array(B.SLOT_BUDGET + 8), name: 'x' }),
       /larger than a slot/, 'the payload refusal ends in the sentence the dial prints pre-erase (D26)');
ok(B.writeContainer({ geom: jsGeom, art: new Uint8Array(B.SLOT_BUDGET), name: 'x' }).length > 0,
   'the payload exactly at the slot budget is accepted: == cap installs to the byte');
throws(() => B.writeContainer({ geom: new Uint8Array(B.GEOM_BUDGET + 1), art: art, name: 'x' }),
       /overrun the pixels/, 'geometry past the table area is refused at the bake, not after an erase (P6.2)');
ok(B.writeContainer({ geom: new Uint8Array(B.GEOM_BUDGET), art: art, name: 'x' }).length > 0,
   'geometry exactly at the cap is accepted: == cap installs to the byte');
throws(() => B.writeContainer({ geom: jsGeom, art: art, scrW: M.scrW, scrH: M.scrH, title: 'no name' }),
       /no name/, 'a container holding only FORMAT 1 title field is refused: the name is what the card files under');
// A name is free text under FORMAT 2 — the card filename is the hash, so the charset
// rules that kept a slug matchable on the card are gone. What is left is the field.
ok(B.nameOk('Mirror — Gold Nº 3'), 'a name with spaces, an em dash and a superscript is the theme name now');
ok(!B.nameOk(''), 'an empty name is refused: it would hash to an id no header can be matched to');
ok(!B.nameOk('a\u0000b'), 'an embedded NUL is refused: the C reader stops there and the tail would silently not be the name');
ok(B.nameOk('x'.repeat(B.NAME_MAX - 1)), 'the field exactly full is accepted');
ok(!B.nameOk('x'.repeat(B.NAME_MAX)), 'one byte past the field is refused');
ok(!B.nameOk('é'.repeat(28)), 'fifty-six bytes in twenty-eight characters is over the field: the cap counts BYTES');
ok(B.nameBytes('ééé') === 6, 'and nameBytes reports that unit to the studio');

// ---- 6. the blob's own size rule ------------------------------------------
// main/art/orb_theme.c:geom_counts() takes a blob only when its length matches what its
// counts declare, for either the four- or the seven-count layout. A packer that got this
// wrong hands out a file the dial calls damaged, so the JS rule is checked against the
// Python's number rather than trusted.
const nq = M.bank.rects.length / 5;
const need7 = B.geomNeed([nq, M.bank.first.length, M.bank.delays.length, 1, M.comps.length, M.auxRecs.length, 0], 7);
const need4 = B.geomNeed([nq, M.bank.first.length, M.bank.delays.length, 1, 0, 0, 0], 4);
ok(jsGeom.length / 4 === need7, 'the seven-count blob is exactly as long as its counts declare',
   jsGeom.length / 4 + ' vs ' + need7);
ok(need4 !== need7, 'the two layouts cannot both fit one blob: that is what lets a reader tell them apart');
// The comp/aux/bank records are the only thing the seven-count layout carries beyond the
// four-count one, so their field widths are the wire contract with the C struct.
ok(B.wordsOf('comp') === 26 && B.wordsOf('aux') === 24 && B.wordsOf('bank') === 15,
   'record field counts match the C structs the dial reads',
   B.wordsOf('comp') + '/' + B.wordsOf('aux') + '/' + B.wordsOf('bank'));
console.log('   geom ' + jsGeom.length + ' B, art ' + M.artSize + ' B, ' + nq + ' delta rects');

// ---- 6b. bakeComplications: the box is the tile's plane, not the tile ------
// Every .orbtheme the browser baker shipped with a complication carried w=0, h=0 and
// frame_bytes=0 in its window record, and the dial drew static over the whole face.
// A tile from the kit reader is {value, plane}, and the box was read off tile.w, a
// property nothing ever sets, so undefined reached rec.w and Math.round(px - w/2)
// reached NaN, both of which pack as zero. The pixels were written correctly, so all
// three CRCs still checked and the loader's bounds tests passed: geometry that merely
// disagrees with its own payload is not damage a checksum can see. The dials then
// strode count frames of 0 bytes and painted whatever they landed on.
// bake_clock.py takes the box from im.size, so plane.w/plane.h is its equal here.
const compTiles = (n, w, h) => Array.from({ length: n }, (_, v) => ({
  value: String(v), plane: { w: w, h: h, rgba: new Uint8Array(w * h * 4) } }));
const moonComp = [{ id: 'c1', type: 'moon', shows: 'moon', host: 'dial',
                    angle: 0, dist: 120, rot: 0, style: 'fan', underLayer: 3 }];
const compBlob = B.blob();
const compRecs = B.bakeComplications(compBlob, { comps: moonComp }, () => compTiles(30, 88, 88));
const w0 = compRecs[0];
ok(compRecs.length === 1 && w0.w === 88 && w0.h === 88,
   'a window records the box its tiles were actually packed at', w0.w + 'x' + w0.h);
ok(w0.frame_bytes === 88 * 88 * 3,
   'and the stride the dials use to reach frame two', w0.frame_bytes + ' vs ' + (88 * 88 * 3));
ok(Number.isInteger(w0.x) && Number.isInteger(w0.y) && !(w0.x === 0 && w0.y === 0),
   'the anchor is a position on the panel, not two zeroed words', w0.x + ',' + w0.y);
ok(w0.count === 30 && w0.kind === 0 && w0.src === B.SOURCES.indexOf('moon_age'),
   'kind, count and the driving value survive too', w0.kind + '/' + w0.count + '/' + w0.src);
// The invariant that broke: every byte the packer appended is reachable by the record.
// A tail the geometry does not name is art no reader will ever draw, and it is what a
// zeroed frame_bytes costs.
ok(w0.off + w0.count * w0.frame_bytes === compBlob.len,
   'the window covers its payload exactly, leaving no orphan tail',
   w0.off + ' + ' + (w0.count * w0.frame_bytes) + ' vs ' + compBlob.len);
throws(() => B.bakeComplications(B.blob(), { comps: moonComp }, () => compTiles(4, 88, 88).concat([{ value: 'x', plane: { w: 40, h: 88, rgba: new Uint8Array(40 * 88 * 4) } }])),
       /one fixed box/, 'a second tile of another size is refused, not silently mis-strided');
throws(() => B.bakeComplications(B.blob(), { comps: moonComp }, () => [{ value: '0', plane: { w: 0, h: 0, rgba: new Uint8Array(0) } }]),
       /empty tile/, 'a tile with no pixels is refused, not packed as a zero-byte window');

// ---- 7. the kit reader: PNG bytes, not a canvas round-trip -----------------
// The browser bake decodes the kit's PNGs itself rather than painting them to a canvas and
// reading them back, because Chrome stores its canvas premultiplied: a round trip can move
// the colour stored under a partly transparent texel by one, and those are exactly the
// texels bake_clock.py's planar-alpha rule keeps on purpose. Three real PNGs written by
// PIL (RGBA, RGB, palette) pin the filters, the palette, and the tRNS path.
(function pngFixtures() {
  const zlib = require('zlib');
  const dir = '/tmp/orbtheme_png';
  const py = spawnSync(PY, ['-c', `
import os, sys
from PIL import Image
os.makedirs(${JSON.stringify(dir)}, exist_ok=True)
def put(im, f):
    for y in range(im.size[1]):
        for x in range(im.size[0]):
            im.putpixel((x, y), f(x, y))
    return im
cases = {}
a = Image.new('RGBA', (37, 11)); cases['rgba'] = (put(a, lambda x, y: ((x * 7 + y * 13) & 255, (x * 3) & 255, (y * 5) & 255, 0 if (x + y) % 5 else (x * 9 + y) & 255)), a)
b = Image.new('RGB', (20, 5));   cases['rgb'] = (put(b, lambda x, y: ((x * 11 + y) & 255, (x * y * 3) & 255, (x + y * 7) & 255)), b.convert('RGBA'))
c = Image.new('RGBA', (33, 7));  put(c, lambda x, y: ((x * 5 + y * 29) & 255,) * 3 + (255,))
p = c.convert('P'); cases['palet'] = (p, p.convert('RGBA'))
out = {}
for k, (src, want) in cases.items():
    src.save(os.path.join(${JSON.stringify(dir)}, k + '.png'))
    raw = want.tobytes(); open(os.path.join(${JSON.stringify(dir)}, k + '.rgba'), 'wb').write(raw)
    out[k] = {'w': want.size[0], 'h': want.size[1], 'bytes': len(raw)}
import json; sys.stdout.write(json.dumps(out))
`], { encoding: 'utf8' });
  if (py.status !== 0) { ok(false, 'the PNG fixtures could not be produced', (py.stderr || '').split('\n').pop()); return; }
  const spec = JSON.parse(py.stdout);
  for (const k of Object.keys(spec)) {
    const got = B.decodePng(new Uint8Array(fs.readFileSync(dir + '/' + k + '.png')),
                            (b) => new Uint8Array(zlib.inflateSync(Buffer.from(b))));
    const want = fs.readFileSync(dir + '/' + k + '.rgba');
    let d = 0, at = -1;
    for (let i = 0; i < want.length; i++) if (got.rgba[i] !== want[i]) { if (at < 0) at = i; d++; }
    ok(d === 0 && got.w === spec[k].w && got.h === spec[k].h,
       'PNG ' + k + ' decodes to the same texels PIL produced', d + ' bytes differ at ' + at);
  }
  let threw = false;
  try { B.decodePng(new Uint8Array([1, 2, 3, 4, 5, 6, 7, 8, 9, 10]), () => new Uint8Array(0)); }
  catch (e) { threw = /not a PNG/.test(e.message); }
  ok(threw, 'a file that is not a PNG is refused rather than decoded into garbage');

// ---- 8. the whole kit, end to end -----------------------------------------
// The unit checks above pin each stage against a hand-built sentinel. This one pins the
// driver: it runs the real bench pipeline (prep_watch_kit.py then bake_clock.py
// --orbtheme) over the shipped kit, bakes the same zip here, and requires the two
// containers to agree. Everything after the header's bake stamp must match -- the stamp
// is the wall clock at bake time and the header CRC covers it, so those two words are the
// only bytes allowed to differ, and both sides' CRC is verified over its own header.
// The bench run costs tens of seconds, so its output is cached and reused while the zip is newer.
(function realKit() {
  const KIT = path.join(ROOT, 'assets/sources/av-kit/watch-aviator-kit.zip');
  if (!fs.existsSync(KIT)) { ok(false, 'the shipped kit is present to bake', KIT); return; }
  const OUTF = '/tmp/orbtheme_e2e';
  const REF = OUTF + '/ref.orbtheme', STAMP = OUTF + '/.baked';
  const { execFileSync } = require('child_process');
  fs.mkdirSync(OUTF, { recursive: true });
  let refAge = 0;
  try { refAge = fs.statSync(STAMP).mtimeMs; } catch (e) { }
  if (refAge < fs.statSync(KIT).mtimeMs || !fs.existsSync(REF)) {
    fs.rmSync(OUTF, { recursive: true, force: true }); fs.mkdirSync(OUTF, { recursive: true });
    execFileSync(PY, [path.join(ROOT, 'tools/prep_watch_kit.py'), KIT, OUTF + '/theme'],
                 { cwd: ROOT, stdio: 'pipe', maxBuffer: 1 << 24 });
    execFileSync(PY, [path.join(ROOT, 'tools/bake_clock.py'), OUTF + '/theme/theme',
                      '--bg-frames', OUTF + '/theme/frames', '--bg-fps', '15',
                      '--tick-rate', '15', '--matte', 'off', '--bg-occlude', 'auto',
                      '--orbtheme', REF, '--theme-name', 'aviator',
                      '--bin', OUTF + '/a.bin', '--header', OUTF + '/a.h'],
                 { cwd: ROOT, stdio: 'pipe', maxBuffer: 1 << 24 });
    fs.writeFileSync(STAMP, '');
  }
  const { inflateSync } = require('zlib');
  const list = JSON.parse(execFileSync(PY, ['-c',
    'import zipfile,json,sys,base64;' +
    'z=zipfile.ZipFile(sys.argv[1]);' +
    'sys.stdout.write(json.dumps({n: base64.b64encode(z.read(n)).decode() for n in z.namelist()}))',
    KIT], { encoding: 'utf8', maxBuffer: 1 << 28 }));
  const manifest = JSON.parse(Buffer.from(list['manifest.json'], 'base64').toString('utf8'));
  const planes = {};
  for (const n of Object.keys(list)) {
    if (!/[.]png$/i.test(n) || n.indexOf('sources/') === 0) continue;
    planes[n] = B.decodePng(new Uint8Array(Buffer.from(list[n], 'base64')),
                            (u) => new Uint8Array(inflateSync(Buffer.from(u))));
  }
  let js = null, why = '';
  try { js = B.bakeKit({ manifest, planes, name: 'aviator', serial: 0, bakedMs: 0 }).bytes; }
  catch (e) { why = e && e.message || String(e); }
  ok(!!js, 'the real kit bakes without throwing', why);
  if (!js) return;
  const py = fs.readFileSync(REF);
  ok(js.length === py.length, 'the container is as long as the bench one', js.length + ' vs ' + py.length);
  // word 12 of the header is the bake timestamp; word W_HDR_CRC is its checksum
  const MASK = [48, 52, 112, 116];
  const masked = (u) => { const c = Uint8Array.from(u); for (const m of MASK) c.fill(0, m, m + 4); return c; };
  const a = masked(js), b = masked(py);
  let d = 0, at = -1;
  for (let i = 0; i < Math.min(a.length, b.length); i++) if (a[i] !== b[i]) { if (at < 0) at = i; d++; }
  ok(d === 0, 'every byte but the bake stamp matches the bench container', d + ' bytes differ, first at ' + at);
  const hdrCrc = (u) => { const c = Uint8Array.from(u.subarray(0, 128)); c.fill(0, 112, 116); return B.crc32(c); };
  ok(u32(py, 112) === hdrCrc(py), 'the bench header CRC checks over its own header');
  ok(u32(js, 112) === hdrCrc(js), 'and this one does too', '0x' + u32(js, 112).toString(16));
  ok(u32(js, 20) === u32(py, 20) && u32(js, 24) === u32(py, 24),
     'geometry size and CRC agree', u32(js, 20) + '/' + u32(js, 24).toString(16));
  ok(u32(js, 28) === u32(py, 28) && u32(js, 32) === u32(py, 32),
     'art size and CRC agree', u32(js, 28) + '/' + u32(js, 32).toString(16));
  function u32(u, o) { return (u[o] | u[o + 1] << 8 | u[o + 2] << 16 | u[o + 3] << 24) >>> 0; }
})();

})();

console.log(fails ? fails + ' FAILED of ' + checked : 'RESULT: PASS  ' + checked + ' checks');
process.exit(fails ? 1 : 0);

