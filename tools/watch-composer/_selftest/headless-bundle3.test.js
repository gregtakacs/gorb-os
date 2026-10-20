#!/usr/bin/env node
// Mirror suite for the browser's FORMAT 3 bundle writer.
//
// D33 puts the product's format logic in the browser, so orbtheme-baker.js now emits
// FORMAT 3 as well as FORMAT 2, and one container can theme several apps. Four
// implementations read the one layout: tools/orbbundle.py (the authority), main/art/
// orb_bundle.c (the device's door), orbnet's themecontainer.go (the hub's pre-gate), and
// the writer below. Nothing compiles the JS against the Python, so this suite does:
// make_bundle_fixture.py --vectors dumps the model that generated the committed
// tests/fixtures/format3-pair.orbtheme, the checks feed that model to writeBundle3, and
// the bytes must match the fixture exactly — not agree on the fields, match byte for
// byte, because every byte of the index and of the two CRCs is a promise a reader uses.
//
// The failure modes pinned here are the ones that do not crash. An index record read with
// the wrong alignment paints a dial whose element count is somebody's ABI pair. A blob
// whose CRC covers the wrong span installs and then resolves to garbage. A table packed
// without check_element ships a row the device paints wrongly instead of refusing it, and
// §29's rule that an OS-paced region is a SPRITE row would be a convention nobody enforces.
//
//   /tmp/nodeenv/bin/node tools/watch-composer/_selftest/headless-bundle3.test.js
//
// Needs node and a python3 that can import the repo's own tools/. The writer leg needs
// nothing else; the bake-path leg shells out to tools/verify_js_bake.py, which compiles the
// shipped orb_clock3.c and so wants gcc and zlib — without them it prints a SKIP.
'use strict';
const { spawnSync } = require('child_process');
const fs = require('fs');
const path = require('path');
const B = require('../orbtheme-baker.js');

const ROOT = path.resolve(__dirname, '..', '..', '..');
const PY = process.env.B3_PY || 'python3';
const GEN = path.join(ROOT, 'tools', 'make_bundle_fixture.py');
const FIX = path.join(ROOT, 'tests', 'fixtures', 'format3-pair.orbtheme');
const SIDE = FIX.replace(/\.orbtheme$/, '.json');
const OUT = process.env.B3_VEC || '/tmp/bundle3_vectors';
let fails = 0, checked = 0;
const ok = (c, n, x) => { checked++; console.log((c ? 'PASS ' : 'FAIL ') + n + (x ? '  [' + x + ']' : '')); if (!c) fails++; };
const hex = (u8) => Buffer.from(u8.buffer.slice(u8.byteOffset, u8.byteOffset + u8.length)).toString('hex');
const firstDiff = (a, b) => {
  const n = Math.min(a.length, b.length);
  for (let i = 0; i < n; i++) if (a[i] !== b[i]) return i;
  return a.length === b.length ? -1 : n;
};
// A refusal is the CHECK, so a writer that throws nothing has failed. Returns the message.
const refuses = (fn, want, name) => {
  let msg = null;
  try { fn(); } catch (e) { msg = String((e && e.message) || e); }
  if (msg === null) return ok(false, name, 'no refusal at all');
  ok(msg.toLowerCase().indexOf(want.toLowerCase()) >= 0, name, msg.slice(0, 100));
};

// ---- the reference model, from the Python that defines the format ----------
const run = spawnSync(PY, [GEN, '--vectors', OUT], { encoding: 'utf8', maxBuffer: 1 << 26 });
if (run.status !== 0) {
  console.log('the reference vectors could not be produced:\n' +
    (run.stderr || run.stdout).split('\n').slice(0, 12).join('\n'));
  console.log('0 checks run — needs python3 with the repo\u2019s tools/ importable');
  process.exit(1);
}
const M = JSON.parse(fs.readFileSync(path.join(OUT, 'model.json'), 'utf8'));
const fixture = Uint8Array.from(fs.readFileSync(FIX));
const side = JSON.parse(fs.readFileSync(SIDE, 'utf8'));
// Art travels as a file rather than as a description of how to draw it, because the art is
// opaque to write_bundle. Node pools small reads into a shared ArrayBuffer, so copy: a
// Buffer's .buffer is not the file.
const model = {
  name: M.name, serial: M.serial, bakedMs: M.baked_ms,
  sections: M.sections.map(s => Object.assign({}, s, {
    art: s.art_file ? Uint8Array.from(fs.readFileSync(path.join(OUT, s.art_file))) : new Uint8Array(0)
  }))
};
const js = B.writeBundle3(model).bytes;

// ---- 1. the bytes, against the fixture every other reader is pinned to -----
ok(js.length === fixture.length, 'the JS bundle is the fixture\u2019s length',
   js.length + ' vs ' + fixture.length);
ok(firstDiff(js, fixture) === -1,
   'the JS bundle matches the committed fixture byte for byte',
   'first difference at ' + firstDiff(js, fixture));
ok(hex(js) === hex(Uint8Array.from(fs.readFileSync(path.join(OUT, 'container.bin')))),
   'and it matches the Python\u2019s freshly written container, not just the copy on disk');

// ---- 2. the reference READER opens what the browser wrote ------------------
// Byte equality already proves the layout. This leg still earns its place: it is
// read_bundle exercising the JS bytes, so a writer and a fixture that drifted into
// agreeing with EACH OTHER rather than with the authority get caught here.
fs.writeFileSync(path.join(OUT, 'from_js.orbtheme'), Buffer.from(js));
const rb = spawnSync(PY, [GEN, '--read-back', path.join(OUT, 'from_js.orbtheme')],
                     { encoding: 'utf8', maxBuffer: 1 << 26 });
if (rb.status !== 0) {
  ok(false, 'the authority read the JS bytes back', String(rb.stderr).split('\n')[0]);
} else {
  const got = JSON.parse(rb.stdout);
  const same = (k) => JSON.stringify(got[k]) === JSON.stringify(side[k]);
  ok(same('bytes') && same('n_sections') && same('geom_size') && same('art_size') &&
     same('geom_off') && same('art_off') && same('geom_crc') && same('art_crc'),
     'the authority reads the JS bytes with the sidecar\u2019s own header numbers',
     got.geom_size + ' B region, ' + got.art_size + ' B art');
  ok(same('slugs') && same('n_elem') && same('frames'),
     'and the same sections, element counts and animated rows', got.slugs.join(','));
  ok(same('name') && same('abi'), 'and the same name and ABI stamp',
     got.name + ' v' + got.abi);
}

// ---- 3. the element table refuses what the device would refuse -------------
// §29's animated-region rule lives in these rows: an OS-paced region must be a SPRITE, so
// a LAYER or a STATUS row carrying frames is refused, and a BANK carrying EF_OS_ADV is
// refused. A writer that packs without check_element produces a file the device paints
// wrongly, which looks like bad art rather than like a bug.
const E = (o) => Object.assign(B.blankElement(), { n: 1, n_frames: 1 }, o);
const SPRITE = B.classId('sprite'), LAYER = B.classId('layer'), BANK = B.classId('bank');
const HAND = B.classId('hand'), TEXT = B.classId('text'), STATUS = B.classId('status');
refuses(() => B.packElements([E({ class: 99 })]), 'not a class this build knows', 'an unknown class id is refused');
refuses(() => B.packElements([E({ class: SPRITE, flags: 16 })]), 'unknown flag bits', 'an unknown flag bit is refused rather than zeroed');
refuses(() => B.packElements([E({ class: SPRITE, order: 64 })]), 'depth rungs', 'an order past the rungs is refused');
refuses(() => B.packElements([E({ class: SPRITE, n_frames: 0 })]), 'n_frames: 0 draws nothing', 'a zero-frame row is refused');
refuses(() => B.packElements([E({ class: SPRITE, frame0: 350, n_frames: 11 })]), 'run past the art region', 'a frame window past the bound is refused');
refuses(() => B.packElements([E({ class: SPRITE, degrees: 36000 })]), 'is not a direction', 'a direction of 36000 is refused');
refuses(() => B.packElements([E({ class: LAYER, n_frames: 4 })]), 'composited, not animated', 'an animated LAYER row is refused (§29)');
refuses(() => B.packElements([E({ class: STATUS, n_frames: 4 })]), 'composited, not animated', 'an animated STATUS row is refused (§29)');
refuses(() => B.packElements([E({ class: BANK, n_frames: 4, flags: B.EF_OS_ADV })]), 'must not advance its frames', 'a BANK with EF_OS_ADV is refused (§29)');
refuses(() => B.packElements([E({ class: TEXT, flags: B.EF_OS_ADV })]), 'must not advance its frames', 'TEXT with EF_OS_ADV is refused');
refuses(() => B.packElements([E({ class: HAND, w: 40, h: 200, pivot_x: 40, pivot_y: 0 })]), 'outside its own', 'a HAND pivoting off its own plate is refused');
refuses(() => B.packElements([E({ class: TEXT, radius: 50 })]), 'places glyphs', 'a TEXT row with a radius is refused');
refuses(() => B.packElements([E({ class: SPRITE, scale16: 8 })]), 'below one sixteenth', 'a sub-1/16 scale is refused');
refuses(() => B.packElements([E({ class: SPRITE, slot: 0 }), E({ class: SPRITE, slot: 0, n: 2 })]),
        'two rows claim class', 'two rows claiming one (class, slot) are refused');
refuses(() => B.packElements([E({ class: SPRITE }), E({ class: LAYER, n: 1 })]), 'row identity 1 repeats', 'two rows sharing an identity are refused');
const tooMany = [];
for (let i = 0; i < 33; i++) tooMany.push(E({ class: SPRITE, slot: i, n: i + 1 }));
refuses(() => B.packElements(tooMany), 'past the bound of 32', '33 rows are refused');
ok(B.packElements([E({ class: SPRITE, n_frames: 12, flags: B.EF_OS_ADV | B.EF_LOOP })]).length === B.ELEMENT_WORDS,
   'an animated SPRITE row — the legal shape — packs to one fixed row');
ok(B.packElements([E({ class: BANK, n_frames: 4 })]).length === B.ELEMENT_WORDS,
   'and a value-per-plate BANK without OS_ADV packs too');

// ---- 4. the D29 gate, at the boundary rather than near it ------------------
// The sums are the INSTALL's: geometry against the dial's table area, payload against what
// the banks have left. == cap is legal, and the suites that moved these caps on the Python
// side are the reason the numbers are asserted before the boundary is probed.
ok(B.GEOM_CAP === 61440 && B.ART_CAP === 4128768, 'the caps are orb_theme.c\u2019s own numbers',
   B.GEOM_CAP + ' / ' + B.ART_CAP);
let threw = false; try { B.checkBudget3(61440, 4128768); } catch (e) { threw = true; }
ok(!threw, '== cap on both sums is legal');
refuses(() => B.checkBudget3(61441, 0), 'overrun the pixels', 'one byte of geometry past the cap is refused');
refuses(() => B.checkBudget3(0, 4128769), 'larger than a slot', 'one byte of payload past the cap is refused');

// ---- 5. the read-side gates, in the device's refusal order -----------------
// A hub or editor that validates in its own order reports a reason the dial will never
// print, and the owner then chases a problem the card never had.
const fmt2 = Uint8Array.from(fs.readFileSync(path.join(OUT, 'container.bin')));
fmt2[4] = 2;                                             // FORMAT word, little-endian byte 0
refuses(() => B.parseBundle3(fmt2), 'takes format 3', 'a FORMAT 2 container is refused as a format');
const fixed = Uint8Array.from(fixture); fixed[112] ^= 0xff;
refuses(() => B.parseBundle3(fixed), 'checksum mismatch', 'a damaged header CRC is refused');
const torn = Uint8Array.from(fixture).subarray(0, 64);      // under the header's 128 bytes
refuses(() => B.parseBundle3(torn), 'too short to hold a theme header', 'a short file is refused');
const cut = Uint8Array.from(fixture).subarray(0, fixture.length - 4096);
refuses(() => B.verifyBundle3(cut), 'runs past the end', 'a container truncated mid-art is refused');
const dmg = Uint8Array.from(js); dmg[140] ^= 0x01;
refuses(() => B.verifyBundle3(dmg), 'checksum mismatch', 'a flipped bit in the geometry region is refused');

// ---- 6. what the writer itself refuses ------------------------------------
refuses(() => B.writeBundle3({ name: '', sections: model.sections }), 'needs a name',
        'a bundle with no name is refused (R4: it is the theme_id input)');
refuses(() => B.writeBundle3({ name: 'X', sections: [] }), 'holds 1..8', 'an empty bundle is refused');
const nine = [];
for (let i = 0; i < 9; i++) nine.push({ slug: 'app' + i, elements: [E({ class: SPRITE })], art: new Uint8Array(64) });
refuses(() => B.writeBundle3({ name: 'X', sections: nine }), 'holds 1..8', 'nine sections are refused (B5\u2019s cap)');
refuses(() => B.writeBundle3({ name: 'X', sections: [
  { slug: 'clock', elements: [E({ class: SPRITE })], art: new Uint8Array(64) },
  { slug: 'clock', elements: [E({ class: SPRITE })], art: new Uint8Array(64) }] }),
  'twice', 'one slug listed twice is refused: one slug resolves to one section');
refuses(() => B.writeBundle3({ name: 'X', sections: [{ slug: 'Clock', elements: [E({ class: SPRITE })] }] }),
  'lowercase a-z 0-9 _', 'an uppercase slug is refused');
refuses(() => B.writeBundle3({ name: 'X', sections: [{ slug: 'a'.repeat(24), elements: [E({ class: SPRITE })] }] }),
  'lowercase a-z 0-9 _ of 1..23', 'a 24-char slug is refused');
refuses(() => B.writeBundle3({ name: 'X', sections: [{ slug: 'app' }] }),
  'neither elements nor art',
  'a section with elements and no art is legal (D31); one with NEITHER is refused');
ok(B.writeBundle3({ name: 'X', sections: [{ slug: 'app', elements: [E({ class: SPRITE })] }] }).artSize === 0,
   'and a table-only section really does write with an empty bank', 'art_size 0');
refuses(() => B.writeBundle3({ name: 'X', sections: [{ slug: 'app', art: new Uint8Array(64),
  sprites: [{ off: 0, size: 65, w: 1, h: 1 }] }] }),
  'runs past its own bank', 'a sprite catalogue pointing past its bank is refused');
// A name past the field is REFUSED by both writers, never cut in half: the header's name
// is the theme's identity, so a truncated one files the theme under the hash of a string
// its owner never typed. _setword learned this after the JS port was written, and the two
// now agree by this check rather than by whoever last read the other file.
const longName = 'Chronometer of the Long Now '.repeat(3);
refuses(() => B.packHeader3({ name: longName, geomSize: 0, geomCrc: 0, artSize: 0,
                              artCrc: 0, nSections: 1, artOff: 4096 }),
        'the header holds at most', 'a past-field name is refused by the browser writer');
const cutPy = spawnSync(PY, ['-c',
  'import sys; sys.path.insert(0, sys.argv[1]); import orbbundle as ob; ' +
  'ob.pack_header3(sys.argv[2], 1, 0, 0, 4096, 0, 0)',
  path.join(ROOT, 'tools'), longName], { encoding: 'utf8', maxBuffer: 1 << 20 });
ok(cutPy.status !== 0 && /header holds at most/.test(cutPy.stderr),
   'and the authority refuses the same name for the same reason',
   String(cutPy.stderr).trim().split('\n').pop());
ok(!B.nameOk(longName), 'and nameOk tells the studio before the bake, not after',
   B.nameBytes(longName) + ' > ' + (B.NAME_MAX - 1));

// ---- 7. the round trip, through the browser's own reader -------------------
// writeBundle3 then verifyBundle3 is the shortest path that catches a writer whose offsets
// disagree with its own reader, which is the bug class the D26 refusal sentences exist for.
const back = B.verifyBundle3(js);
ok(back.header.name === side.name && back.header.nSections === side.n_sections,
   'the browser reader opens what the browser wrote', back.header.name);
ok(back.sections.map(s => s.blob.slug).join(',') === side.slugs.join(','),
   'and finds the sections slug-sorted', back.sections.map(s => s.blob.slug).join(','));
ok(back.sections.every(s => s.blob.elements.length === side.n_elem[s.blob.slug]),
   'with the element count the index records and the blob carries agreeing');
const wx = back.sections.filter(s => s.blob.slug === 'weather')[0];
// The fixture carries TWO multi-frame rows on purpose: the 12-frame sprite D32's animated
// region rides, and a 4-plate bank. Only the first may carry EF_OS_ADV, and §29's rule is
// exactly that distinction, so the check names both rather than counting rows.
const spr = wx.blob.elements.filter(e => e.class === SPRITE)[0];
const bnk = wx.blob.elements.filter(e => e.class === BANK)[0];
ok(spr && spr.n_frames === 12 && (spr.flags & B.EF_OS_ADV) !== 0,
   'the animated region survived as a SPRITE row with OS_ADV set (§29)',
   spr ? 'sprite:' + spr.n_frames + ' flags 0x' + spr.flags.toString(16) : 'no sprite row');
ok(bnk && bnk.n_frames === 4 && (bnk.flags & B.EF_OS_ADV) === 0,
   'and the value-per-plate BANK kept its frames with OS_ADV clear',
   bnk ? 'bank:' + bnk.n_frames + ' flags 0x' + bnk.flags.toString(16) : 'no bank row');
ok(wx.blob.elements.every(e => e.class !== LAYER || e.n_frames === 1),
   'and no composited row came back carrying frames');
const byByte = B.packIndex3(back.sections.map(s => s.rec));
const pyHead = Buffer.from(fs.readFileSync(path.join(OUT, 'container.bin')));
ok(hex(byByte) === pyHead.subarray(128, 128 + byByte.length).toString('hex'),
   'the index region the reader rebuilt re-packs to the bytes the writer wrote');

// ---- 8. the trap that cost the Go gate a fix -------------------------------
// The record leads with FOUR u16s, so n_elem lives at byte 8 and not at 4. A reader that
// assumed a u32 first read every later field eight bytes early and looked like a corrupt
// bundle rather than a wrong offset. Pinned here so no implementation rediscovers it.
ok(B.IDX_ORDER.slice(0, 4).join(',') === 'slug_off,slug_len,abi_min,abi_max' &&
   B.IDX_OFF[4] === 8 && B.IDX_OFF[5] === 12 && B.IDX_OFF[11] === 36 && B.IDX_WORDS === 10,
   'the index record\u2019s field offsets are the four-u16 layout, not a u32 alignment',
   'n_elem@' + B.IDX_OFF[4] + ' geom_off@' + B.IDX_OFF[5] + ' scr_w@' + B.IDX_OFF[11]);
// The file the other three readers are pinned against has to clear this gate too, or the
// browser would refuse a bundle the device installs.
let gateErr = null;
try { B.verifyBundle3(Uint8Array.from(fs.readFileSync(FIX))); } catch (e) { gateErr = String(e.message || e); }
ok(gateErr === null, 'the committed fixture passes the browser gate as written',
   gateErr ? gateErr.slice(0, 90) : side.bytes + ' B');

// Restamp one header word and repair the header CRC, so a check can reach the gate that
// sits BEHIND the checksum instead of tripping the checksum on the way. The span is the
// whole 128-byte header with word 28 itself zeroed — orbtheme.py's _hdr_crc, and the same
// span the firmware re-CRCs before it believes a card.
function restamp(src, word, value) {
  const out = Uint8Array.from(src), v = new DataView(out.buffer, out.byteOffset, out.length);
  v.setUint32(word * 4, value >>> 0, true);
  v.setUint32(112, 0, true);
  const z = new Uint8Array(128);
  for (let i = 0; i < 128; i++) z[i] = out[i];
  v.setUint32(112, B.crc32(z) >>> 0, true);
  return out;
}
refuses(() => B.parseBundle3(restamp(fixture, 2, 2)), 'reads v3',
        'a bundle ABI this build is not (v2) is refused as an ABI, not as damage');
refuses(() => B.parseBundle3(restamp(fixture, 2, 4)), 'reads v3',
        'and so is one stamped ahead of it, which is the D24 no-backward-path rule');
refuses(() => B.parseBundle3(restamp(fixture, 5, 61441)), 'overrun the pixels',
        'a header claiming more geometry than the table area is refused at the caps');
refuses(() => B.parseBundle3(restamp(fixture, 7, 4128769)), 'larger than a slot',
        'and the same for the payload, before the name or the bounds get a turn');

// The container's own ABI stamp is a door check; a section's abi_min..abi_max pair is the
// RESOLVER's, and the two rules differ on purpose: the interval form lets one bundle carry
// a section for a future build while a current build still resolves the rest. A gate that
// conflated them would refuse a bundle the dial installs.
ok(B.sectionAbiOk({ abi_min: 3, abi_max: 3 }) && B.sectionAbiOk({ abi_min: 2, abi_max: 3 }) &&
   B.sectionAbiOk({ abi_min: 3, abi_max: 4 }),
   'a section whose interval straddles v3 resolves, including one aimed ahead');
ok(!B.sectionAbiOk({ abi_min: 1, abi_max: 2 }) && !B.sectionAbiOk({ abi_min: 4, abi_max: 4 }) &&
   !B.sectionAbiOk({ abi_min: 0, abi_max: 9 }),
   'and one that cannot reach v3 does not');
const fwd = B.writeBundle3({ name: 'Forward', sections: [{ slug: 'clock', abi_min: 3, abi_max: 4,
  elements: [E({ class: SPRITE })], art: new Uint8Array(8192) }] });
ok(B.verifyBundle3(fwd.bytes).sections[0].rec.abi_max === 4,
   'the walk opens a forward-straddling section rather than refusing it');

// ---- 7. the bake path: planes in, a bundle the compiled dial projects out ----
// The writer takes encoded pixels; this is the encoding half, and its pass line is not
// "the door opens" but "orb_clock3.c seats it" — the projection is what turns a table into
// a drawable dial, and a bake the door accepts and the projection refuses looks exactly
// like a store bug to the person holding it.
const plateRgba = (w, h) => { const r = new Uint8Array(w * h * 4); for (let i = 0; i < w * h; i++) { r[i * 4] = 3; r[i * 4 + 1] = 7; r[i * 4 + 2] = 11; r[i * 4 + 3] = 255; } return r; };
const mkPlane = (w, h, a) => { const r = plateRgba(w, h); if (a !== undefined) for (let i = 0; i < w * h; i++) r[i * 4 + 3] = a; return r; };
const PL = { rgba: mkPlane(466, 466), w: 466, h: 466 };
const HK = { slot: 0, rgba: mkPlane(40, 200, 200), x: 233, y: 233, w: 40, h: 200, pivot_x: 20, pivot_y: 190 };
const SK = { slot: 0, rgba: mkPlane(40, 200, 90), x: 235, y: 235, w: 40, h: 200, pivot_x: 20, pivot_y: 190 };
const baked = B.bakeClock3({ name: 'Bench Bake', serial: 20261020, bakedMs: 1730000000000,
  plate: PL, hands: [HK], shadows: [SK], order: [0] });
const walk = B.verifyBundle3(baked.bytes).sections[0];
ok(baked.meter.artSize === 466 * 466 * 2 + 40 * 200 * 3 * 2,
   'the bank costs the plate colour-only and every sprite its alpha stride',
   baked.meter.artSize + ' B (w*h*2 then 2 w*h*3)');
ok(walk.blob.elements.length === 3 && walk.blob.sprites.length === 3 &&
   walk.blob.elements[1].frame0 === 1 && walk.blob.elements[2].frame0 === 2,
   'the bake walks back as three rows seated on three planes, each pointing at its own');
ok(walk.rec.art_size === baked.meter.artSize && walk.rec.scr_w === 466 && walk.rec.scr_h === 466,
   'the index record the dial reads carries the bank it names and the panel it was baked for');
// counts(16) + scalars(104) + rows(204) + catalogue(48) + 'clock' padded to words(8),
// behind the index head(8) and its one record(40).
ok(baked.meter.geomSize === 8 + 40 + 16 + 104 + 204 + 48 + 8,
   'the region is the index over one self-describing blob', baked.meter.geomSize + ' B');
ok(baked.meter.geomCap === 61440 && baked.meter.artCap === 4128768,
   'the meter names both D29 install caps, not the file shape');
refuses(() => B.bakeClock3({ name: 'x', plate: { rgba: mkPlane(10, 10), w: 10, h: 10 } }),
        'full-screen', 'a plate that is not the whole panel is refused as the dial words it');
refuses(() => B.bakeClock3({ name: 'x', plate: PL, scalars: { tourbillon_n: 4 } }),
        'carriage tables', 'a carriage the section has no rows for is refused at the bake');
refuses(() => B.bakeClock3({ name: 'x', plate: PL, shadows: [SK] }),
        'no hand there', 'a shadow with no hand under it is refused, not silently drawn');
refuses(() => B.bank3([{ rgba: mkPlane(4, 4), w: 4, h: 5 }]), 'RGBA',
        'a plane whose bytes do not match its own extent is refused before an offset is spent');
const bkPath = '/tmp/js_bake_parity.orbtheme';
fs.writeFileSync(bkPath, Buffer.from(baked.bytes));
const hasGcc = spawnSync('sh', ['-c', 'command -v gcc >/dev/null && test -f /usr/include/zlib.h']).status === 0;
if (!hasGcc) {
  console.log('SKIP the shipped projection: no gcc or no zlib.h on this bench');
} else {
  const pr = spawnSync(PY, [path.join(ROOT, 'tools', 'verify_js_bake.py'), bkPath], { encoding: 'utf8' });
  ok(pr.status === 0, 'the shipped orb_clock3.c projects the browser bake',
     (pr.stdout || pr.stderr).trim().split('\n')[0]);
  ok(/hand\s+\[0\] 40x200 draws at \(233,233\) colour_off 434312/.test(pr.stdout),
     'and seats the hand at the element rect with the plate-colour size as its offset',
     (pr.stdout.match(/hand.*434312/) || ['none'])[0]);
}

console.log(fails ? fails + ' FAILED of ' + checked : 'RESULT: PASS  ' + checked + ' checks');
process.exit(fails ? 1 : 0);