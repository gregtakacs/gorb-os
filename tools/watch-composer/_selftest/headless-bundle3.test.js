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
//   ~/.local/share/nodeenv-gorb/bin/node tools/watch-composer/_selftest/headless-bundle3.test.js
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
refuses(() => B.packElements([E({ class: SPRITE, flags: 0x80 })]), 'unknown flag bits', 'an unknown flag bit is refused rather than zeroed');
refuses(() => B.packElements([E({ class: SPRITE, flags: B.AUX_F_SHADOW3 })]), 'rule for class aux',
        'the pointer shadow twin means nothing on another class, and the door says whose bit it is');
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
// the banks have left. == cap is legal. C1 rewrote the cap check: it used to assert the
// literal 4128768 under a comment claiming those were orb_theme.c's own numbers, and that
// is exactly how four tools shipped a cap the device had not reported since Migration 1 —
// a test that asserts the shipped value cannot notice the shipped value is wrong. The cap
// is now asserted AGAINST the baker's exported ORB_THEME_SLOT_CAP, which the writer itself
// gates on, and the boundary is probed at the same exported figure, not near an old one.
ok(B.GEOM_CAP === 61440 && B.ART_CAP === B.ORB_THEME_SLOT_CAP &&
   B.ORB_THEME_SLOT_CAP === 8323072,
   'the caps are orb_theme.c\u2019s own numbers: ART_CAP imports the exported ORB_THEME_SLOT_CAP, the 8 MB slot less its 64 KB header block',
   B.GEOM_CAP + ' / ' + B.ART_CAP);
let threw = false; try { B.checkBudget3(61440, B.ART_CAP); } catch (e) { threw = true; }
ok(!threw, '== cap on both sums is legal');
refuses(() => B.checkBudget3(61441, 0), 'overrun the pixels', 'one byte of geometry past the cap is refused');
refuses(() => B.checkBudget3(0, B.ART_CAP + 1), 'larger than a slot', 'one byte of payload past the cap is refused');

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
refuses(() => B.parseBundle3(restamp(fixture, 2, 5)), 'reads v3',
        'and so is one stamped ahead of it (v5, past C4\u2019s delta dialect), which is the D24 no-backward-path rule');
refuses(() => B.parseBundle3(restamp(fixture, 5, 61441)), 'overrun the pixels',
        'a header claiming more geometry than the table area is refused at the caps');
refuses(() => B.parseBundle3(restamp(fixture, 7, B.ORB_THEME_SLOT_CAP + 1)), 'larger than a slot',
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
  plate: PL, hands: [HK], shadows: [SK], order: [0],
  // The composer's bake ships this scalar pair with every shadow row: a dial
  // that seats a shadow the flag keeps unbuilt is the "no shadows" bug in one
  // word, and the walk below reads it back so the two can never drift.
  scalars: { shadow_on: 1, shadow_dx: 2, shadow_dy: 2 } });
const walk = B.verifyBundle3(baked.bytes).sections[0];
ok(baked.meter.artSize === 466 * 466 * 2 + 40 * 200 * 3 * 2,
   'the bank costs the plate colour-only and every sprite its alpha stride',
   baked.meter.artSize + ' B (w*h*2 then 2 w*h*3)');
ok(walk.blob.elements.length === 3 && walk.blob.sprites.length === 3 &&
   walk.blob.elements[1].frame0 === 1 && walk.blob.elements[2].frame0 === 2,
   'the bake walks back as three rows seated on three planes, each pointing at its own');
ok(walk.blob.scalars.shadow_on === 1 && walk.blob.elements[2].class === SPRITE &&
   walk.blob.elements[2].slot === 0,
   'the shadow row walks back as the hand’s sprite slot, under the flag that builds it');
ok(walk.rec.art_size === baked.meter.artSize && walk.rec.scr_w === 466 && walk.rec.scr_h === 466,
   'the index record the dial reads carries the bank it names and the panel it was baked for');
// counts(16) + scalars(104) + rows(3 * 23 words * 4 = 276: D34 widened the row from
// D32's 19) + catalogue(48) + 'clock' padded to words(8), behind the index head(8) and
// its one record(40). The 48 B per row the append costs is the honest price of the four
// drive columns, and it is why this line names the width instead of a bare total.
ok(baked.meter.geomSize === 8 + 40 + 16 + 104 + 276 + 48 + 8,
   'the region is the index over one self-describing blob at D34’s 23-word rows',
   baked.meter.geomSize + ' B');
ok(baked.meter.geomCap === 61440 && baked.meter.artCap === B.ART_CAP,
   'the meter names both D29 install caps, not the file shape');
refuses(() => B.bakeClock3({ name: 'x', plate: { rgba: mkPlane(10, 10), w: 10, h: 10 } }),
        'full-screen', 'a plate that is not the whole panel is refused as the dial words it');
refuses(() => B.bakeClock3({ name: 'x', plate: PL, scalars: { tourbillon_n: 4 } }),
        'does not assemble', 'a carriage the section has no rows for is refused at the bake');
refuses(() => B.bakeClock3({ name: 'x', plate: PL, shadows: [SK] }),
        'no hand there', 'a shadow with no hand under it is refused, not silently drawn');
refuses(() => B.bank3([{ rgba: mkPlane(4, 4), w: 4, h: 5 }]), 'RGBA',
        'a plane whose bytes do not match its own extent is refused before an offset is spent');

// ---- 7b. the §31 keyframe plate bank: N whole-panel planes under one row ----
// The bake's claim is the dialect's own: one row, planes contiguous behind the
// plate, colour-only, and the scalars cross-set to agree. Then the shipped
// projection says so too — same bytes, same table, the browser end refusing
// what orb_clock3.c would refuse and accepting what it would project.
const kf = (tint) => { const r = mkPlane(466, 466); for (let i = 0; i < 466 * 466; i++) { r[i * 4] = (i * 7 + tint) & 0xFF; } return r; };
const APL = { rgba: kf(1), w: 466, h: 466, frames: [{ rgba: kf(1), w: 466, h: 466 },
                                                    { rgba: kf(9), w: 466, h: 466 },
                                                    { rgba: kf(17), w: 466, h: 466 }] };
const bakedA = B.bakeClock3({ name: 'Anim Bake', serial: 20261020, bakedMs: 1730000000000,
  plate: APL, hands: [HK], order: [0] });
const walkA = B.verifyBundle3(bakedA.bytes).sections[0];
ok(walkA.blob.elements[0].n_frames === 3 && walkA.blob.elements[0].frame0 === 0,
   'the animated plate projects as one row naming three contiguous planes',
   'n_frames ' + walkA.blob.elements[0].n_frames + ' frame0 ' + walkA.blob.elements[0].frame0);
ok(walkA.blob.sprites[0].size === 466 * 466 * 2 && walkA.blob.sprites[1].size === 466 * 466 * 2 &&
   walkA.blob.sprites[2].size === 466 * 466 * 2,
   'every keyframe costs the plate colour-only, alpha strides and all',
   'rows 0-2: ' + walkA.blob.sprites.slice(0, 3).map(s => s.size).join('/'));
// C1 rewrote this row as the CONTRACT (§4.2, §5.1), not the shipped output. The old
// assertion read `bg_mode === 0 && gif_frames === 0` and called it "delta vocabularies
// zero" — it pinned the exact accident that made the animated plate cost 3.97 MB and the
// tourbillon run at 4 fps. The contract: an animated row is a delta stream. Frame 0 is
// the base plane; every frame after it names a rect in the row's own box; the row carries
// its own delay; and the row names its parent, so the escapement rides the carriage's
// transform instead of being painted into a plate. The shipped bake zeroes all of that,
// so THIS CHECK IS RED ON PURPOSE: the red is the spec C2-C5 implement. Do not restore
// the old assertion and do not make this one read the current output — that is how this
// bug survived its last three test suites.
const dRowA = walkA.blob.elements[0];
// §37: the closing delta is the chain's last link — frame 0 carries the rect that
// returns the wrapped canvas to the keyframe — so every frame now owns a rect and
// APL, whose texels all move, prices that rect as a fourth full plane.
ok(walkA.blob.scalars.bg_loop === 1 && dRowA.n_frames === 3 &&
   dRowA.delay > 0 && dRowA.rect_count === dRowA.n_frames &&
   dRowA.diffw > 0 && dRowA.parent !== undefined &&
   (dRowA.flags & B.EF_DELTA_WRAP) !== 0,
   'the delta contract: every frame of an animated row carries a rect, §37 closes the loop, and the row names delay and parent',
   'n_frames ' + dRowA.n_frames + ' delay ' + dRowA.delay + ' rect_count ' + dRowA.rect_count + ' diffw ' + dRowA.diffw);
ok(bakedA.meter.artSize === 466 * 466 * 2 * 4 + 40 * 200 * 3,
   'the bank is three keyframes, §37\u2019s full-plane closing delta, and the masked hand',
   bakedA.meter.artSize + ' B');
// ---- 7b-ii. §36: the delta encoder's floor and its self-check read ONE number ----
// The encoder drops a texel whose summed |dR|+|dG|+|dB|+|dA| is at or under DELTA_TOL, so the
// verifier may only refuse drift PAST that same floor. The old byte-exact self-check refused the
// sub-floor dither the encoder throws away on purpose, which is what made an animated Skeleton
// bake fail with "does not composite back" for art that projects clean. These three pins derive
// their probe magnitudes from B.DELTA_TOL so the boundary lives in the codec, not the test.
const TOL36 = B.DELTA_TOL;
const driftPlate = (corner) => {
  const W = 466, H = 466, mk = () => { const r = new Uint8Array(W * H * 4); for (let i = 0; i < W * H; i++) { r[i*4]=3; r[i*4+1]=7; r[i*4+2]=11; r[i*4+3]=255; } return r; };
  const f0 = mk(), f1 = mk();
  for (let y = 220; y < 240; y++) for (let x = 220; x < 240; x++) { const a = (y * W + x) * 4; f1[a]=200; f1[a+1]=200; f1[a+2]=200; }
  f1[(300 * W + 300) * 4] = 3 + corner;   // one faint texel at (300,300), far outside the 20x20 block, red channel only
  return { rgba: f0, w: W, h: H, frames: [{ rgba: f0, w: W, h: H }, { rgba: f1, w: W, h: H }] };
};
const driftWalk = (corner) => B.verifyBundle3(B.bakeClock3({
  name: 'drift ' + corner, plate: driftPlate(corner), hands: [HK], order: [0] }).bytes).sections[0];
const diffw36 = (w) => w && w.blob ? w.blob.elements[0].diffw : -1;
// (a) a lone sub-floor dither texel: it must bake, where the byte-exact verifier refused it. RED until the tolerance fix.
let d36 = null;
try { d36 = driftWalk(TOL36 - 1); } catch (e) { d36 = { threw: String((e && e.message) || e) }; }
ok(d36 && d36.blob, '§36 the sub-floor dither bakes instead of refusing drift the encoder drops on purpose', d36 && d36.threw ? d36.threw.slice(0, 70) : 'diffw ' + diffw36(d36));
// (b) the dropped texel neither ships nor widens the delta rect: it stays the tight 20-wide block.
ok(diffw36(d36) === 20, '§36 the sub-floor texel neither ships nor stretches the delta rect', 'diffw ' + diffw36(d36));
// (c) the floor still bites real motion: one step past DELTA_TOL is a change the rect must reach.
ok(diffw36(driftWalk(TOL36 + 1)) > 20, '§36 a change past the floor is motion, so the delta rect grows to cover it', 'diffw ' + diffw36(driftWalk(TOL36 + 1)));
// (d) exactly at the floor is the still diffRect named, not a drift selfVerify3 refuses: one boundary, two readers.
ok(diffw36(driftWalk(TOL36)) === 20, '§36 a change of exactly DELTA_TOL is read the same by the encoder and the self-check', 'diffw ' + diffw36(driftWalk(TOL36)));
refuses(() => B.bakeClock3({ name: 'x', plate: { rgba: kf(1), w: 466, h: 466, frames: [kf(1)] },
   }), 'two or more', 'a one-frame bank is a still plate and the bake says which');
refuses(() => B.bakeClock3({ name: 'x', plate: APL, scalars: { bg_frames: 4 } }),
        'animation', 'a scalar disagreeing with the row is refused at the bake');
refuses(() => B.bakeClock3({ name: 'x', plate: APL, scalars: { gif_frames: 15 } }),
        'no delta rects', 'a keyframe bank with a gif count on it is refused');
refuses(() => B.bakeClock3({ name: 'x', plate: { rgba: kf(1), w: 466, h: 466, loop: 0,
          frames: [{ rgba: kf(1), w: 466, h: 466 }, { rgba: kf(9), w: 466, h: 466 }] } }),
        'no tempo', 'a one-shot bank with no fps is refused, not frozen');
refuses(() => B.bakeClock3({ name: 'x', plate: { rgba: kf(1), w: 466, h: 466,
          frames: Array.from({ length: 17 }, () => ({ rgba: kf(1), w: 466, h: 466 })) } }),
        'no room for', 'a seventeen-frame bank is refused before a byte is encoded');
// ---- 7c. §31's other carrier: the carriage bank, the motion's bbox ----
const CARF = (a) => ({ rgba: mkPlane(64, 64, a), w: 64, h: 64 });
// Bank AND carriage in one bundle — the answer splitPolicy3 now gives an orbital dial
// whose train also moves. The two carriers were mutually exclusive in the old single
// verdict, which is how every orbital bake lost its orbit. Both scalars must cross-set
// and the row must still seat at hand slot 5.
const bakedAC = B.bakeClock3({ name: 'Bank And Carriage', serial: 20261022, bakedMs: 1730000000002,
  plate: APL, hands: [HK], order: [0],
  carriage: { x: 200, y: 200, w: 64, h: 64, pivot_x: 32, pivot_y: 32, degrees: 0, fps: 4,
              frames: [CARF(200), CARF(220), CARF(240)] } });
const walkAC = B.verifyBundle3(bakedAC.bytes).sections[0];
ok(walkAC.blob.scalars.bg_frames === 2 && walkAC.blob.scalars.tourbillon_n === 3
   && walkAC.blob.scalars.tourbillon_fps === 4,
   'a plate bank and a carriage row cross-set their scalars together: two extra keyframes, three carriage frames',
   'bg_frames ' + walkAC.blob.scalars.bg_frames + ' tourbillon_n ' + walkAC.blob.scalars.tourbillon_n
     + ' @ ' + walkAC.blob.scalars.tourbillon_fps);
const carAC = walkAC.blob.elements[walkAC.blob.elements.length - 1];
ok(carAC.class === 2 && carAC.slot === 5 && carAC.n_frames === 3,
   'and the carriage still rides hand slot 5 beneath the animated plate row',
   'class ' + carAC.class + ' slot ' + carAC.slot + ' n ' + carAC.n_frames);
// The cadence defaults. A bake that names no sweep used to pack second_sweep=0,
// tick_rate=0 — the board read the zeros literally, ran the bank at its 4 fps and
// turned the carriage once a second. The animated dial must now ship sweeping at
// the fastest tempo on the wire, and an author who names the cadence still wins.
ok(walkAC.blob.scalars.second_sweep === 1 && walkAC.blob.scalars.tick_rate === 4,
   'the bank-plus-carriage bake sweeps at the carriage tempo, not the shipped zeros',
   'second_sweep ' + walkAC.blob.scalars.second_sweep + ' tick_rate ' + walkAC.blob.scalars.tick_rate);
const APLT = { rgba: kf(1), w: 466, h: 466, fps: 6,
  frames: [{ rgba: kf(1), w: 466, h: 466 }, { rgba: kf(9), w: 466, h: 466 }] };
const walkAT = B.verifyBundle3(B.bakeClock3({ name: 'Tempo Bake', serial: 20261023,
  bakedMs: 1730000000003, plate: APLT, hands: [HK], order: [0] }).bytes).sections[0];
ok(walkAT.blob.scalars.second_sweep === 1 && walkAT.blob.scalars.tick_rate === 6,
   'a plate bank with a named tempo ticks the dial at it',
   'second_sweep ' + walkAT.blob.scalars.second_sweep + ' tick_rate ' + walkAT.blob.scalars.tick_rate);
const walkAD = B.verifyBundle3(B.bakeClock3({ name: 'Deadbeat Bake', serial: 20261024,
  bakedMs: 1730000000004, plate: APL, hands: [HK], order: [0],
  scalars: { second_sweep: 0 } }).bytes).sections[0];
ok(walkAD.blob.scalars.second_sweep === 0,
   'an author naming a deadbeat second keeps it past the sweep default',
   'second_sweep ' + walkAD.blob.scalars.second_sweep);

const bakedC = B.bakeClock3({ name: 'Carriage Bake', serial: 20261020, bakedMs: 1730000000001,
  plate: PL, hands: [HK], order: [0],
  carriage: { x: 200, y: 200, w: 64, h: 64, pivot_x: 32, pivot_y: 32, degrees: 4500, fps: 12,
              frames: [CARF(200), CARF(220), CARF(240)] } });
const walkC = B.verifyBundle3(bakedC.bytes).sections[0];
const carRow = walkC.blob.elements[walkC.blob.elements.length - 1];
ok(carRow.class === 2 && carRow.slot === 5 && carRow.n_frames === 3 && carRow.frame0 === 2,
   'the carriage seats as one hand row at slot 5 naming three contiguous planes',
   'class ' + carRow.class + ' slot ' + carRow.slot + ' n ' + carRow.n_frames + ' frame0 ' + carRow.frame0);
ok(walkC.blob.scalars.tourbillon_n === 3 && walkC.blob.scalars.tourbillon_fps === 12 &&
   carRow.degrees === 4500,
   'the carriage scalars cross-set: three frames at 12 fps, start orientation from the row',
   'n ' + walkC.blob.scalars.tourbillon_n + ' fps ' + walkC.blob.scalars.tourbillon_fps + ' deg ' + carRow.degrees);
ok(walkC.blob.sprites[2].size === 64 * 64 * 3 && walkC.blob.sprites[4].off === walkC.blob.sprites[2].off + 2 * 64 * 64 * 3,
   'carriage frames are masked planes, contiguous, three strides deep',
   walkC.blob.sprites.slice(2, 5).map(s => s.size).join('/'));
// §32.6 (D32 ruling C reversed): the carriage row now carries order 1 — one rung above
// the plate's 0, below every hand — so the classic LVGL path and the L4 tile sort both
// composite it UNDER the hands, as the preview draws it. Its own row and the hand row it
// now rides beneath are read back off the shipped bundle.
const carDepth = walkC.blob.elements[walkC.blob.elements.length - 1];
const handDepth = walkC.blob.elements.find(el => el.class === 2 && el.slot === 0);
ok(carDepth.order === 1 && handDepth.order === 2,
   'the carriage bakes at geartrain depth (order 1) under the hand (order 2)',
   'carriage order ' + carDepth.order + ' hand order ' + handDepth.order);
refuses(() => B.bakeClock3({ name: 'x', plate: PL,
          carriage: { x: 0, y: 0, w: 64, h: 64, pivot_x: 32, pivot_y: 32, fps: 12, frames: [CARF(200)] } }),
        'is an animation', 'a one-frame carriage is a static hand and the bake says which');
refuses(() => B.bakeClock3({ name: 'x', plate: PL,
          carriage: { x: 0, y: 0, w: 64, h: 64, pivot_x: 32, pivot_y: 32, fps: 0, frames: [CARF(200), CARF(220)] } }),
        'no tempo', 'a carriage with no fps is refused, not frozen');
refuses(() => B.bakeClock3({ name: 'x', plate: PL,
          carriage: { x: 0, y: 0, w: 64, h: 64, pivot_x: 32, pivot_y: 32, fps: 12,
                      frames: [CARF(200), { rgba: mkPlane(32, 32, 220), w: 32, h: 32 }] } }),
        'one bank is one extent', 'a carriage frame smaller than its row is refused');
refuses(() => B.bakeClock3({ name: 'x', plate: PL, hands: [HK],
          carriage: { x: 0, y: 0, w: 64, h: 64, pivot_x: 32, pivot_y: 32, fps: 12, frames: [CARF(200), CARF(220)] },
          scalars: { tourbillon_n: 5 } }),
        'row’s planes are the bank’s length', 'a scalar disagreeing with the carriage row is refused');
// ---- 7d. splitPolicy3: the one arithmetic, every measurement ----
// C1 re-anchored every otherB/compB below by +4,194,304 — the difference between the
// stale 4 MB slot these vectors were tuned against and the exported ORB_THEME_SLOT_CAP
// the device actually reports (§7.4: "the boundary tests inherit the error too").
// The ROOM each vector prices — cap less the other art — is the number the decision
// depends on, and it is unchanged; so every expected carrier, keyframe count and
// thinning step below pins the same boundary as before, now against the true cap.
const sp = (m) => B.splitPolicy3(m);
ok(sp({ frames: 15, fps: 15, unionPx: 65, bboxW: 368, bboxH: 265, otherB: 75696, hasCage: false }).carrier === 'still',
   'a 65-px union of AA shimmer bakes the still, not a lie of motion');
// The plate is now baked WITHOUT the carriage, so a caged dial that names no measured
// box cannot be priced at all — and answering 'bank' here would silently drop the
// tourbillon out of the bake. Refuse.
const pBlind = sp({ frames: 8, fps: 4, unionPx: 49312, bboxW: 402, bboxH: 359, otherB: 4253374, hasCage: true });
ok(pBlind.carrier === 'refuse' && /was not measured/.test(pBlind.sentence),
   'a caged dial with no measured carriage box refuses: the plate would ship without its tourbillon',
   pBlind.sentence);
const pThin = sp({ frames: 15, fps: 15, unionPx: 1402, bboxW: 231, bboxH: 71, otherB: 4251754, hasCage: false });
ok(pThin.carrier === 'bank' && pThin.keyframes === 7 && pThin.decim === 2,
   'a 15-frame loop thins to what fits rather than refusing: 7 keyframes by step 2', pThin.sentence);
// The carriage row and the plate carrier are two decisions now. A caged dial whose
// ONLY motion is the cage measures under the shimmer band un-caged, and the plate
// rides the still while the row carries the loop at full length.
const pSp = sp({ frames: 15, fps: 15, unionPx: 65, bboxW: 146, bboxH: 146, otherB: 4251754,
                 hasCage: true, carriageW: 146, carriageH: 146 });
ok(pSp.carrier === 'sprite' && pSp.carriage && pSp.carriage.keyframes === 15 && pSp.keyframes === 1,
   'a cage that is the only motion rides the carriage at full length over a still plate', pSp.sentence);
// Bank AND carriage: a caged dial whose train moves too. The row is billed FIRST, so
// the plate thins against the room left rather than stealing bytes the row needs.
const pBoth = sp({ frames: 15, fps: 15, unionPx: 5000, bboxW: 200, bboxH: 200, otherB: 4251754,
                   hasCage: true, carriageW: 146, carriageH: 146 });
ok(pBoth.carrier === 'bank' && pBoth.carriage && pBoth.carriage.keyframes === 15 && pBoth.keyframes === 5,
   'a moving train on a caged dial bakes the bank thin and keeps the carriage whole', pBoth.sentence);
const pNoCar = sp({ frames: 15, fps: 15, unionPx: 5000, bboxW: 200, bboxH: 200, otherB: 4251754, hasCage: false });
ok(pBoth.keyframes < pNoCar.keyframes,
   'the carriage bills the bank budget before the thin: the same dial without a row keeps more keyframes',
   'bank ' + pNoCar.keyframes + ' vs ' + pBoth.keyframes + ' with the row');
// The orbital arrow: the motion box is the whole dial BECAUSE the carriage is the
// seconds hand, so the old locality gate refused it the carrier and shipped the orbit
// frozen into the plate. It now takes the row, and the plate rides the still.
const pOrb = sp({ frames: 8, fps: 4, unionPx: 49312, bboxW: 402, bboxH: 359, otherB: 4253374,
                  hasCage: true, orbital: true, carriageW: 389, carriageH: 389 });
ok(pOrb.carrier === 'sprite' && pOrb.carriage && pOrb.carriage.keyframes === 8 && pOrb.carriage.fps === 4,
   'an orbital carriage takes the row at its own tempo over a still plate — the bank could never turn it',
   pOrb.sentence);
const pOrbRef = sp({ frames: 8, fps: 4, unionPx: 49312, bboxW: 402, bboxH: 359, otherB: 7494304,
                      hasCage: true, orbital: true, carriageW: 389, carriageH: 389 });
ok(pOrbRef.carrier === 'refuse' && /frozen orbit/.test(pOrbRef.sentence),
   'an orbital carriage that will not fit refuses and says the byte, rather than banking a frozen orbit',
   pOrbRef.sentence);
const pOrbCaged = sp({ frames: 8, fps: 4, unionPx: 49312, bboxW: 402, bboxH: 359, otherB: 8244304,
                        hasCage: true, carriageW: 146, carriageH: 146 });
ok(pOrbCaged.carrier === 'refuse' && /baked without it/.test(pOrbCaged.sentence),
   'a plain tourbillon whose row will not fit refuses too: the strip is in the plate either way',
   pOrbCaged.sentence);

// ---- 7e. the row's price IS the rect it ships ----
/* The over-cap bake of 2026-10-22: a caged orbital dial whose bake refused at 7,064,426 B
 * against a 4,128,768 B slot, on a policy verdict that had already been told the row was
 * affordable. The cause was two functions deciding the row's box — the measure counted
 * ink at alpha >= 8 and added no pad, while alphaBbox crops at alpha > 0 and
 * bakeTourbillon pads by 2. Under-pricing is the direction a budget miss must never go,
 * so the baker answers the rect and the composer bills it. These vectors hold that line:
 * build a cage canvas whose real ink sits in a 100x100 window, optionally with ONE faint
 * texel far from it, price it, bake it, and read the shipped bytes off the finished file.
 * Both figures must be the same number, or the policy is estimating. */
const cagePlane = (seed, faint) => {
  const r = new Uint8Array(466 * 466 * 4);
  for (let y = 200; y < 300; y++) for (let x = 200; x < 300; x++) {
    const o = (y * 466 + x) * 4;
    r[o] = (x + seed) & 0xFF; r[o + 1] = (y * 3) & 0xFF; r[o + 2] = 128; r[o + 3] = 200;
  }
  if (faint) r[(faint[1] * 466 + faint[0]) * 4 + 3] = faint[2];
  return { rgba: r, w: 466, h: 466 };
};
const rectTight = B.carriageRect3([cagePlane(1, null), cagePlane(2, null)]);
ok(rectTight[0] === 198 && rectTight[1] === 198 && rectTight[2] === 302 && rectTight[3] === 302,
   'the row rect pads the ink box by 2 on every side, the way the bake crops it',
   'rect ' + rectTight.join(','));
const rectFaint = B.carriageRect3([cagePlane(1, [460, 460, 3]), cagePlane(2, [460, 460, 3])]);
ok(rectFaint[2] === 463 && rectFaint[3] === 463,
   'one alpha-3 texel moves the row rect: alpha > 0 governs the crop, not a tidier threshold',
   'rect ' + rectFaint.join(',') + ' over a 104x104 ink box');
const carPrice = (rect) => B.splitPolicy3({ frames: 2, fps: 2, unionPx: 0, bboxW: 0, bboxH: 0,
  otherB: 24000, hasCage: true, carriageW: rect[2] - rect[0], carriageH: rect[3] - rect[1] });
const noRow = B.bakeClock3({ name: 'Price Ship', serial: 1, bakedMs: 1, plate: PL, hands: [HK], order: [0] });
const shipRow = (planes) => B.bakeClock3({ name: 'Price Ship', serial: 1, bakedMs: 1, plate: PL,
  hands: [HK], order: [0], carriage: { x: 0, y: 0, w: 466, h: 466, pivot_x: 250, pivot_y: 250,
                                       degrees: 0, fps: 2, frames: planes } }).meter.artSize;
const cpTight = carPrice(rectTight), shipTight = shipRow([cagePlane(1, null), cagePlane(2, null)]);
ok(cpTight.carriage && cpTight.carriage.bytes === shipTight - noRow.meter.artSize,
   'the row bills the bytes it ships: padded crop priced, padded crop encoded, to the letter',
   cpTight.carriage.bytes + ' B priced against ' + (shipTight - noRow.meter.artSize) + ' B shipped');
const cpFaint = carPrice(rectFaint), shipFaint = shipRow([cagePlane(1, [460, 460, 3]), cagePlane(2, [460, 460, 3])]);
ok(cpFaint.carriage && cpFaint.carriage.bytes === shipFaint - noRow.meter.artSize,
   'and it still agrees when faint rim texels widen the row — the miss that blew the slot',
   cpFaint.carriage.bytes + ' B priced against ' + (shipFaint - noRow.meter.artSize) + ' B shipped');
ok(cpFaint.carriage.bytes > cpTight.carriage.bytes * 6,
   'the faint texel costs the bake six times the row it hides: the tight measure was the lie',
   cpTight.carriage.bytes + ' B billed, ' + cpFaint.carriage.bytes + ' B true');

// The row's start orientation reaches the wire NEGATED. The preview draws
// rotate(seconds*TAU - cageOff) and the kit publishes zeroRotationDeg = -cageOff, but
// slot 5 ADDS the wire figure to the seconds angle (clock_view.c's c_a = a + s_car_start_10).
// A sign flipped here is a carriage seated twice its offset off the take-up angle on the
// glass, and no byte count anywhere shows it — which is how the shipped orbital arrow
// carried +17.69 deg for a pose the editor had drawn at -17.69.
const D2R = Math.PI / 180;
ok(B.carriageStart3(0) === 0, 'a carriage with no cage offset starts at zero');
ok(B.carriageStart3(17.69 * D2R) === 36000 - 1769,
   'a +17.69 deg cage offset ships 342.31: adding it turns the row by minus the offset',
   'wire ' + B.carriageStart3(17.69 * D2R));
ok(B.carriageStart3(-17.69 * D2R) === 1769,
   'and the mirror offset mirrors back: the fold negates, it does not clamp',
   'wire ' + B.carriageStart3(-17.69 * D2R));
ok([0, 1, 90, 180, 359.99, -45, -179, 720].every((d) => {
     const v = B.carriageStart3(d * D2R); return v >= 0 && v < 36000; }),
   'every offset folds into the 0..35999 hundredths the C reader takes without a negative modulo');

// A row cropped away from the point it turns about is art the dial swings off its pin, so
// the rect holds the pivot and the bill grows to match. Widening costs nothing on a closed
// orbit, which always contains its own centre.
const rectPiv = B.carriageRect3([cagePlane(1, null)], { pivot: [100, 250] });
ok(rectPiv[0] === 100 && rectPiv[1] === 198 && rectPiv[2] === 302 && rectPiv[3] === 302,
   'a pivot outside the ink box widens the row rect to hold it', 'rect ' + rectPiv.join(','));
const cpPiv = carPrice(rectPiv);
const shipPiv = B.bakeClock3({ name: 'Pivot Row', serial: 1, bakedMs: 1, plate: PL, hands: [HK], order: [0],
  carriage: { x: 0, y: 0, w: 466, h: 466, pivot_x: 100, pivot_y: 250, degrees: 0, fps: 2,
              frames: [cagePlane(1, null), cagePlane(2, null)] } }).meter.artSize;
ok(cpPiv.carriage && cpPiv.carriage.bytes === shipPiv - noRow.meter.artSize,
   'and the widened row bills its widened box, to the byte',
   cpPiv.carriage.bytes + ' B priced against ' + (shipPiv - noRow.meter.artSize) + ' B shipped');


const pNoCage = sp({ frames: 15, fps: 15, unionPx: 2222, bboxW: 146, bboxH: 146, otherB: 4251754, hasCage: false });
ok(pNoCage.carrier === 'bank' && !pNoCage.carriage,
   'the same box without a cage is still a bank — the carriage row is the cage, and nothing else');
const pRef = sp({ frames: 15, fps: 15, unionPx: 40000, bboxW: 466, bboxH: 466, otherB: 8194304, hasCage: false });
ok(pRef.carrier === 'refuse' && /exceed/.test(pRef.sentence),
   'a bake with no room left names the byte it tripped on', pRef.sentence);
const pAux = sp({ frames: 15, fps: 15, unionPx: 1402, bboxW: 231, bboxH: 71, otherB: 4251754, auxB: 3 * 466 * 466 * 2, hasCage: false });
ok(pAux.carrier === 'bank' && pAux.keyframes < pThin.keyframes,
   'pointer plates bill the bank budget before the thin: three whole-panel plates take three keyframes away',
   pThin.sentence + ' | ' + pAux.sentence);
// liveComps3: the one count behind the button note and the export guard. The §31
// first cut refused every comp row and quoted the raw count — it called Skeleton's
// own opening a "live complication" while its note had promised the bake.
ok(B.liveComps3([{ type: 'opening' }, { type: 'flare', icon: 'star' }, { type: 'window', shows: 'custom' }]).length === 0,
   'openings, engraved flares and custom art windows are not live: the baked plate carries them');
ok(B.liveComps3([{ type: 'window', shows: 'date' }, { type: 'moon' }, { type: 'subdial' }, { type: 'flare', icon: 'datetext' }]).length === 4,
   'calendar windows, moons, subdial hands and date-printing flares are live: the comp table drives them');
ok(B.liveComps3([{ type: 'subdial', scale: 'none' }]).length === 0,
   'a subdial with no scale has no hand: engraved plate art, not a row');
ok(B.liveComps3([{ type: 'whatever-comes-next' }]).length === 1,
   'an unrecognised comp counts live — the safe direction is the refusal the author can see');
ok(B.liveComps3(undefined).length === 0 && B.liveComps3([]).length === 0,
   'a design with no comps is no comp');
// ---- 7e. compPlan3: the design table, as window rows the dial drives itself ----
// D32 ruling A/B: every live comp becomes one window row over its calendar
// domain, and only what the element row truly cannot carry (glyph work,
// unknown types) refuses. The plan is the single source behind the note, the
// guard and the bake.
const cpCal = B.compPlan3([{ id: 'c1', type: 'opening' },
                           { id: 'c2', type: 'window', host: 'dial', shows: 'day' },
                           { id: 'c3', type: 'window', host: 'dial', shows: 'month' },
                           { id: 'c4', type: 'subdial', host: 'dial', scale: 'date' },
                           { id: 'c5', type: 'moon', host: 'c4' }]);
ok(cpCal.refuses.length === 0 && cpCal.rows.length === 4,
   'the calendar compiles whole: two windows, a date subdial and its hosted moon');
const cpBy = {}; cpCal.rows.forEach((r) => { cpBy[r.id] = r; });
ok(cpBy.c2.src === 'weekday' && cpBy.c2.count === 7 && cpBy.c3.count === 12 &&
   cpBy.c4.src === 'date' && cpBy.c4.count === 31 && cpBy.c4.valueMin === 1 &&
   cpBy.c5.src === 'moon_age' && cpBy.c5.count === 30,
   'rows name the source and the domain\u2019s own count, never a number the bake invented');
ok(cpCal.rows.map((r) => r.id).join(',') === 'c2,c3,c5,c4' && cpCal.rows[2].hosted === 'c4',
   'the hosted moon is ordered before its host: the runtime paints record order, and the host tile punches over the tile below');
const cpReg = B.compPlan3([{ id: 'c1', type: 'subdial', scale: 'hours' },
                           { id: 'c2', type: 'subdial', scale: 'seconds' },
                           { id: 'c3', type: 'window', host: 'c1', shows: 'date' }]);
ok(cpReg.rows.map((r) => r.id).join(',') === 'c3,c1',
   'the regulator\u2019s hosted window rides first, under its hours subdial\u2019s punched plate (the bare seconds ring is the pointer\u2019s by default and leaves the bank)');
ok(B.pointerPlan3([{ id: 'c1', type: 'subdial', scale: 'hours' },
                   { id: 'c2', type: 'subdial', scale: 'seconds' },
                   { id: 'c3', type: 'window', host: 'c1', shows: 'date' }]).rows.map((r) => r.id).join(',') === 'c2',
   'and the regulator\u2019s seconds hand is the one pointer the default puts on the dial');
const cpPhase = B.compPlan3([{ id: 'c1', type: 'subdial', scale: 'none' },
                             { id: 'c2', type: 'moon', host: 'c1' }]);
ok(cpPhase.rows.length === 1 && cpPhase.rows[0].src === 'moon_age' && !cpPhase.rows[0].hosted,
   'a moon over an engraved no-hand subdial is one plain row: the static plate rides in the bank');
const cpFlare = B.compPlan3([{ id: 'c9', type: 'flare', icon: 'datetext' }]);
ok(cpFlare.rows.length === 0 && /glyph/.test(cpFlare.refuses[0] || ''),
   'a date-printing flare refuses in the dial\u2019s own words: no element row holds glyph fields',
   cpFlare.refuses[0]);
const cpUnk = B.compPlan3([{ id: 'z1', type: 'astrolabe' }]);
ok(cpUnk.rows.length === 0 && /astrolabe/.test(cpUnk.refuses[0] || ''),
   'an unknown comp type refuses rather than freezes silently');
const cpMany = B.compPlan3(Array.from({ length: 17 }, (_, i) => ({ id: 'w' + i, type: 'window', shows: 'date' })));
ok(cpMany.rows.length === 17 && cpMany.refuses.length === 1 && /cap of 16/.test(cpMany.refuses[0]),
   'seventeen windows exceed the dial\u2019s 16 complication slots and say so before the guard runs');
// ---- 7e2. pointerPlan3 (D34): the drive that replaces the plates ----
const ptSec = B.pointerPlan3([{ id: 'c2', type: 'subdial', scale: 'seconds', carrier: 'pointer' },
                              { id: 'c1', type: 'subdial', scale: 'hours', carrier: 'plates' }]);
ok(ptSec.rows.length === 1 && ptSec.rows[0].id === 'c2' && ptSec.rows[0].src === 'second' &&
   ptSec.rows[0].vmin === 0 && ptSec.rows[0].vmax === 60 && ptSec.rows[0].span === 3600 &&
   ptSec.rows[0].zero === 0 && ptSec.rows[0].motion === 'rotate' && ptSec.refuses.length === 0,
   'a pointer seconds subdial plans one whole-turn row over sixty values \u2014 one plate, not sixty',
   JSON.stringify(ptSec));
ok(B.compPlan3([{ id: 'c2', type: 'subdial', scale: 'seconds', carrier: 'pointer' }]).rows.length === 0,
   'a pointer-driven subdial never also claims a window row: the drive replaces the plates, it does not join them');
const ptDate = B.pointerPlan3([{ id: 'c4', type: 'subdial', scale: 'date', carrier: 'pointer' }]);
ok(ptDate.rows.length === 0 && /pointer/.test(ptDate.refuses[0] || '') && /plates/.test(ptDate.refuses[0] || ''),
   'a date ring refuses the pointer drive in one sentence: its hand never closes its own turn',
   ptDate.refuses[0]);
const pt9 = B.pointerPlan3(Array.from({ length: 9 }, (_, i) => ({ id: 'p' + i, type: 'subdial', scale: 'seconds', carrier: 'pointer' })));
ok(pt9.rows.length === 9 && pt9.refuses.length === 1 && /pointer slots/.test(pt9.refuses[0]),
   'nine pointers overflow the dial\u2019s eight pointer slots and say so before any art is drawn');
const ptDflt = B.pointerPlan3([{ id: 'c1', type: 'subdial', scale: 'seconds' },
                               { id: 'c2', type: 'subdial', scale: 'hours', carrier: 'plates' },
                               { id: 'c3', type: 'subdial', scale: 'date' },
                               { id: 'c4', type: 'subdial', scale: 'hours' },
                               { id: 'c5', type: 'window', host: 'c4', shows: 'date' }]);
ok(ptDflt.rows.length === 1 && ptDflt.rows[0].id === 'c1' && ptDflt.refuses.length === 0,
   'pointer is the default for a bare seconds or hours ring: plates stays the opt-out, hosted and other scales stay on the bank',
   JSON.stringify(ptDflt));
ok(B.compPlan3([{ id: 'c1', type: 'subdial', scale: 'seconds' }]).rows.length === 0,
   'the defaulted pointer never also claims a window row: the drive replaces the plates, it does not join them');
const ptHost = B.pointerPlan3([{ id: 'c4', type: 'subdial', scale: 'hours', carrier: 'pointer' },
                               { id: 'c5', type: 'window', host: 'c4', shows: 'date' }]);
ok(ptHost.rows.length === 0 && /hosts/.test(ptHost.refuses[0] || '') && /plates/.test(ptHost.refuses[0] || ''),
   'a subdial hosting another indication refuses the pointer drive: the row carries nothing to punch',
   ptHost.refuses[0]);
// the plan's own row shape must bake through bakeClock3's aux seam, unmodified but for art
const bakedPT = B.bakeClock3({ name: 'pt-plan', serial: 7, bakedMs: 1, plate: PL, hands: [HK], order: [0],
  aux: ptSec.rows.map((r, i) => Object.assign({}, r, { slot: i, x: 300, y: 120, w: 28, h: 64,
        pivot_x: 14, pivot_y: 56, plane: { rgba: mkPlane(28, 64, 200), w: 28, h: 64 } })) });
// D34 bench: the composer exported order:[1,2] for hands at slots 0,1 — FORMAT 2's
// show-table +1 — and the watch drew minute, skipped the empty slot 2, and never
// painted hour. The device treats order verbatim; a hand the table omits is refused.
try {
  B.bakeClock3({ name: 'off-by-one', serial: 9, bakedMs: 1, plate: PL, hands: [HK], order: [1] });
  ok(false, 'the order-table hand guard fired', 'bakeClock3 accepted a seated hand it never names');
} catch (e) {
  ok(/never names it/.test(e.message),
     'a seated hand the order table omits is refused before the file is written', e.message);
}
const walkPT = B.verifyBundle3(bakedPT.bytes).sections[0];
const ptRow = walkPT.blob.elements.filter((e) => e.class === B.CLASS_AUX)[0];
ok(!!ptRow && ptRow.kind === B.MOTION_ROTATE && ptRow.span === 3600 && ptRow.vmax === 60 && ptRow.n_frames === 1,
   'the planned row bakes as one whole-turn, one-frame AUX record the shipped C projects');
// ---- 7f. the comp table baked: bank rows, masked contiguous planes ----
const CMP = (slot, src, n, w, h) => ({ slot, src, x: 8, y: 8, w, h,
  planes: Array.from({ length: n }, (_, v) => ({ rgba: mkPlane(w, h, (slot * 100 + v) & 0xFF), w, h })) });
const bakedK = B.bakeClock3({ name: 'Comp Bake', serial: 20261020, bakedMs: 1730000000000,
  plate: PL, hands: [HK], order: [0], comps: [CMP(0, 'date', 31, 40, 24), CMP(1, 'second', 60, 32, 32)] });
const walkK = B.verifyBundle3(bakedK.bytes).sections[0];
const bank0 = walkK.blob.elements[2], bank1 = walkK.blob.elements[3];
ok(walkK.blob.elements.length === 4 && bank0.class === 5 && bank0.kind === 0 && bank0.src === 3 &&
   bank0.n_frames === 31 && bank1.class === 5 && bank1.src === 7 && bank1.n_frames === 60,
   'two bank rows walk back over the plate and the hand: window kind, date and second by id, their domains\u2019 own counts');
ok(bank0.slot === 0 && bank1.slot === 1 && bank0.frame0 === 2 && bank1.frame0 === 33 &&
   walkK.blob.sprites.length === 93 && walkK.blob.sprites[2].size === 40 * 24 * 3 &&
   walkK.blob.sprites[92].size === 32 * 32 * 3,
   'the catalogue seats 31 + 60 masked planes contiguous behind the hand, every tile at w*h*3');
ok(bakedK.meter.artSize === 466 * 466 * 2 + 40 * 200 * 3 + (40 * 24 * 3 * 31) + (32 * 32 * 3 * 60),
   'the bank sums to plate colour, one hand, and the comp tiles at stride',
   bakedK.meter.artSize + ' B');
ok(walkK.rec.abi_min === 3 && walkK.rec.abi_max === 3 && B.parseBundle3(bakedK.bytes).abi === 3,
   'a comp-carrying bake declares the one dialect its 19-word rows need, at the header and both index ends');
refuses(() => B.bakeClock3({ name: 'x', plate: PL, comps: [CMP(0, 'date', 30, 40, 24)] }),
        'indexes 31 plates', 'a date window handed thirty plates is refused, not padded');
refuses(() => B.bakeClock3({ name: 'x', plate: PL, comps: [CMP(0, 'text', 1, 40, 24)] }),
        'no such input', 'a comp the wall clock cannot drive refuses as orb_clock3.c words it');
refuses(() => B.bakeClock3({ name: 'x', plate: PL, comps: [CMP(16, 'date', 31, 40, 24)] }),
        'may not invent one', 'a seventeenth complication slot is the dial\u2019s own sentence');
refuses(() => B.bakeClock3({ name: 'x', plate: PL, comps: [{ slot: 0, src: 'date', x: 440, y: 8, w: 40, h: 24,
  planes: Array.from({ length: 31 }, () => ({ rgba: mkPlane(40, 24), w: 40, h: 24 })) }] }),
        'off the glass', 'a window that would hang over the panel edge refuses at the bake');
refuses(() => B.bakeClock3({ name: 'x', plate: PL, comps: [CMP(0, 'date', 31, 40, 24), CMP(0, 'moon_age', 30, 20, 20)] }),
        'two rows claim', 'two comps on one bank slot is the duplicate the table already refuses');
// C1 re-anchored this vector at the true 8 MB slot (the same over-cap bake, now at
// 230x230 tiles: 60 whole-second plates plus the plate and hand blow 8,323,072 B
// exactly like the 4 MB slot made the old 200x200 ones do). The refusal and its
// sentence — the comp bytes an owner can trade away — are the contract; the old
// literal tile size was one more cap-relative constant, nothing more.
refuses(() => B.bakeClock3({ name: 'x', plate: PL, comps: [CMP(0, 'second', 60, 230, 230)] }),
        'complication tiles cost', 'the over-cap sentence names the comp bytes an owner can trade away');
const pCompR = sp({ frames: 8, fps: 4, unionPx: 49312, bboxW: 402, bboxH: 359, otherB: 4253374,
                    compB: 4000000, hasCage: true, carriageW: 146, carriageH: 146 });
ok(pCompR.carrier === 'refuse' && /4000000 B of complication tiles/.test(pCompR.sentence),
   'a comp table that eats the bank forces the refusal, and the sentence names its cost', pCompR.sentence);
const pCompT = sp({ frames: 15, fps: 15, unionPx: 1402, bboxW: 231, bboxH: 71, otherB: 4251754,
                    compB: 1300000, hasCage: false });
ok(pCompT.carrier === 'bank' && pCompT.keyframes === 5 && pCompT.decim === 3,
   'the same bank thins for the tile bill first: 15 frames to 5 by step 3', pCompT.sentence);
const bkPath = '/tmp/js_bake_parity.orbtheme', bkAnim = '/tmp/js_bake_anim.orbtheme';
const bkCar = '/tmp/js_bake_carriage.orbtheme', bkComp = '/tmp/js_bake_comp.orbtheme',
  bkAux = '/tmp/js_bake_aux.orbtheme';
fs.writeFileSync(bkPath, Buffer.from(baked.bytes));
fs.writeFileSync(bkAnim, Buffer.from(bakedA.bytes));
fs.writeFileSync(bkCar, Buffer.from(bakedC.bytes));
fs.writeFileSync(bkComp, Buffer.from(bakedK.bytes));
// ---- 8. D34: the pointer row, baked here and projected by the shipped C ----
// A sub-dial hand used to cost one whole dial per value: Skeleton's 78.5 px small
// seconds was 60 plates and 3.8 MB, which the D29 guard refused by that number. The
// AUX class carries the same indication as ONE masked plate that MOVES, with the span,
// the origin and the domain written by the theme. The row is the unit of the check, so
// these assertions are about the row's fields, not about a picture.
const AXW = { rgba: mkPlane(28, 64, 120), w: 28, h: 64 };
const auxSec = { slot: 0, src: 'second', motion: 'rotate', span: 3600, zero: 0,
                 vmin: 0, vmax: 60, x: 300, y: 120, w: 28, h: 64,
                 pivot_x: 14, pivot_y: 56, order: 3, plane: AXW };
const auxBar = { slot: 1, src: 'battery', motion: 'slide-x', span: 90, zero: -45,
                 vmin: 0, vmax: 100, x: 200, y: 400, w: 90, h: 10,
                 order: 4, plane: { rgba: mkPlane(90, 10, 90), w: 90, h: 10 } };
const bakedX = B.bakeClock3({ name: 'Pointer Bake', serial: 20261021, bakedMs: 1730000000000,
  plate: PL, hands: [HK], order: [0], aux: [auxSec, auxBar] });
const walkX = B.verifyBundle3(bakedX.bytes).sections[0];
const auxRows = walkX.blob.elements.filter((e) => e.class === B.CLASS_AUX);
ok(auxRows.length === 2, 'two pointer rows walk back as class aux',
   walkX.blob.elements.map((e) => B.CLASS_NAMES[e.class]).join(','));
ok(auxRows[0].kind === B.MOTION_ROTATE && auxRows[0].src === B.SOURCES.indexOf('second') &&
   auxRows[0].span === 3600 && auxRows[0].vmax === 60 && auxRows[0].vmin === 0,
   'the rotate row carries its own drive back off its own bytes',
   JSON.stringify(auxRows.map((r) => [r.kind, r.src, r.span, r.vmin, r.vmax])));
ok(auxRows[1].kind === B.MOTION_SLIDE_X && auxRows[1].span === 90 && auxRows[1].zero === -45,
   'the slide row reads as a slide: a motion id and a span in PIXELS',
   JSON.stringify([auxRows[1].kind, auxRows[1].span, auxRows[1].zero]));
ok(B.motionIsSlide(auxRows[1].kind) && !B.motionIsSlide(auxRows[0].kind),
   'motionIsSlide is the one predicate both the drive and the mark read');
ok(bakedX.meter.artSize === 466 * 466 * 2 + 40 * 200 * 3 + (28 * 64 * 3) + (90 * 10 * 3),
   'the bank costs a pointer ONE masked plate, whatever its domain covers',
   bakedX.meter.artSize + ' B');
// Sixty seconds on a sub-dial is one row now. The number the old model spent.
// The claim is the one the class exists for, stated as the ratio it actually is:
// one masked plate against one plate per second of a minute, at a tile the size of
// the sub-dial itself. Over 50x on a 40 px dial; Skeleton's real 78.5 px small seconds
// measured 3.8 MB, which is the number the guard quoted, and this row is ~5 KB.
ok(28 * 64 * 3 * 50 < 60 * 40 * 40 * 3,
   'one pointer plate is over fifty times under the 60 whole dials it replaces',
   (28 * 64 * 3) + ' B against ' + (60 * 40 * 40 * 3) + ' B');
refuses(() => B.bakeClock3({ name: 'x', plate: PL, aux: [Object.assign({}, auxSec, { vmax: 0 })] }),
        'nothing can be computed from it', 'a pointer over a domain of nothing refuses rather than divide by zero');
refuses(() => B.bakeClock3({ name: 'x', plate: PL, aux: [Object.assign({}, auxSec, { src: 'none' })] }),
        'would sit still', 'a pointer driven by a value that never changes is plate art, refused as a row');
refuses(() => B.bakeClock3({ name: 'x', plate: PL, aux: [Object.assign({}, auxSec, { span: 7200 })] }),
        'full turn', 'a pointer cannot travel past one turn; that is a disc');
refuses(() => B.bakeClock3({ name: 'x', plate: PL, aux: [Object.assign({}, auxSec, { pivot_x: 400 })] }),
        'outside its own', 'a pointer cannot turn about a point that is not on it');
refuses(() => B.bakeClock3({ name: 'x', plate: PL, aux: [Object.assign({}, auxBar, { span: 900 })] }),
        'px panel', 'a slide longer than the glass refuses in the panel\u2019s own pixels');
refuses(() => B.bakeClock3({ name: 'x', plate: PL, aux: [Object.assign({}, auxSec, { slot: 8 })] }),
        'pointer slots', 'a ninth pointer is past the cap the dial declares');
refuses(() => B.bakeClock3({ name: 'x', plate: PL, aux: [Object.assign({}, auxSec, { motion: 'spin' })] }),
        'names none of them', 'a motion the vocabulary has no word for refuses by name');
// §26 slice 4: the pointer's shadow twin. The row that darkens a sub-dial hand is a
// SECOND aux row — flagged with AUX_F_SHADOW3 in the u8 column, slotted directly after
// its hand — whose pivot column lands its centre on hand centre + light, the same
// §25.2 placement the centred hands' shadows carry. The hand above is 28x64 at
// (300,120) about (14,56); its twin pads two and takes the light (4,4): the plate
// draws at (302,122) turning about (16,58), centre (318,180).
const TWP = { rgba: mkPlane(32, 68, 60), w: 32, h: 68 };
const auxTwin = Object.assign({}, auxSec, {
  shadow: { rgba: TWP.rgba, w: 32, h: 68, x: 302, y: 122, pivot_x: 16, pivot_y: 58 } });
const bakedT = B.bakeClock3({ name: 'Twin Bake', serial: 20261022, bakedMs: 1730000000001,
  plate: PL, hands: [HK], order: [0], aux: [auxTwin, auxBar] });
const walkT = B.verifyBundle3(bakedT.bytes).sections[0];
const twRows = walkT.blob.elements.filter((e) => e.class === B.CLASS_AUX);
ok(twRows.length === 3, 'a shadowed pointer bakes as two rows and the battery bar keeps its own, renumbered',
   JSON.stringify(twRows.map((r) => r.slot)));
ok(twRows[0].flags === 0 && twRows[1].flags === B.AUX_F_SHADOW3,
   'the twin row carries the shadow bit and its hand row carries none',
   JSON.stringify(twRows.map((r) => r.flags)));
ok(twRows[1].slot === twRows[0].slot + 1 && twRows[2].slot === 2,
   'the twin takes the slot directly after its hand, and every pointer behind it shifts by one',
   JSON.stringify(twRows.map((r) => r.slot)));
ok(twRows[1].x + twRows[1].pivot_x === twRows[0].x + twRows[0].pivot_x + 4 &&
   twRows[1].y + twRows[1].pivot_y === twRows[0].y + twRows[0].pivot_y + 4,
   'the twin’s centre is the hand’s centre plus the light (4,4): §25.2 in three numbers',
   JSON.stringify(twRows.map((r) => [r.x + r.pivot_x, r.y + r.pivot_y])));
refuses(() => B.bakeClock3({ name: 'x', plate: PL, aux: [Object.assign({}, auxTwin, { shadow: { rgba: mkPlane(32, 68, 60), w: 30, h: 68, x: 302, y: 122, pivot_x: 16, pivot_y: 58 } })] }),
        'one row, one extent', 'a twin plate that lies about its size refuses at the bake');
refuses(() => B.bakeClock3({ name: 'x', plate: PL, aux: [Object.assign({}, auxTwin, { shadow: Object.assign({}, auxTwin.shadow, { pivot_x: 400 }) })] }),
        'outside its own', 'a twin turning about a point off its own plate refuses at the bake');
refuses(() => B.bakeClock3({ name: 'x', plate: PL, aux: Array.from({ length: 5 }, (_, i) => Object.assign({}, auxTwin, { slot: i })) }),
        'twins counted', 'four shadowed pointers is the cap; the fifth refuses before any art is drawn');
const bkTwin = '/tmp/js_bake_twin.orbtheme';
const bkBoth = '/tmp/js_bake_bank_and_carriage.orbtheme';
const bkCrop = '/tmp/js_bake_row_crop.orbtheme';
fs.writeFileSync(bkTwin, Buffer.from(bakedT.bytes));
fs.writeFileSync(bkBoth, Buffer.from(bakedAC.bytes));
// The row the crop exists for: a cage canvas at full panel carrying a 100x100 ink window.
// The dial must seat the row at the crop's top-left, at the crop's size.
const bakedCrop = B.bakeClock3({ name: 'Row Crop Projection', serial: 20261022, bakedMs: 1730000000003,
  plate: PL, hands: [HK], order: [0],
  carriage: { x: 0, y: 0, w: 466, h: 466, pivot_x: 250, pivot_y: 250, degrees: 0, fps: 2,
              frames: [cagePlane(1, null), cagePlane(2, null)] } });
fs.writeFileSync(bkCrop, Buffer.from(bakedCrop.bytes));
fs.writeFileSync(bkAux, Buffer.from(bakedX.bytes));
const hasGcc = spawnSync('sh', ['-c', 'command -v gcc >/dev/null && test -f /usr/include/zlib.h']).status === 0;
if (!hasGcc) {
  console.log('SKIP the shipped projection: no gcc or no zlib.h on this bench');
} else {
  const pr = spawnSync(PY, [path.join(ROOT, 'tools', 'verify_js_bake.py'), bkPath, bkAnim, bkCar, bkComp, bkAux, bkTwin], { encoding: 'utf8' });
  // The one claim this slice adds at the device's own door: a plate bank AND a
  // carriage row in the same section. Run ALONE, so both detail lines are the
  // coexistence bundle's and not another fixture's.
  const prBoth = spawnSync(PY, [path.join(ROOT, 'tools', 'verify_js_bake.py'), bkBoth], { encoding: 'utf8' });
  const prCrop = spawnSync(PY, [path.join(ROOT, 'tools', 'verify_js_bake.py'), bkCrop], { encoding: 'utf8' });
  ok(prCrop.status === 0 && /tourbillon\s*\[\d\] 104x104 draws at \(198,198\)/.test(prCrop.stdout),
     'the compiled dial seats the cropped row at the crop\'s own box, the byte the policy billed',
     (prCrop.stdout.match(/tourbillon[^\n]*/) || [(prCrop.stdout || prCrop.stderr).trim().split('\n')[0]])[0]);
  ok(prBoth.status === 0 && /plate bank: 2 extra keyframes/.test(prBoth.stdout)
     && /tourbillon\s*\[\d\] 64x64 draws at \(200,200\)/.test(prBoth.stdout),
     'and the compiled dial projects a plate bank and a carriage row in one section',
     (prBoth.stdout || prBoth.stderr).trim().split('\\n')[0]);
  ok(pr.status === 0, 'the shipped orb_clock3.c projects all five browser bakes, comp table included',
     (pr.stdout || pr.stderr).trim().split('\n')[0]);
  ok(/hand\s+\[0\] 40x200 draws at \(233,233\) colour_off 434312/.test(pr.stdout),
     'and seats the hand at the element rect with the plate-colour size as its offset',
     (pr.stdout.match(/hand.*434312/) || ['none'])[0]);
  // D34's shadow contract across the wire: the row draws at its own draw point,
  // its PIVOT COLUMN became a centre offset from the hand by the light, and the
  // crop pivot the C recomputed is the hand's centre minus the row's x — the
  // composer's whole shadow placement in three numbers.
  ok(/shadow\s+\[0\] 40x200 draws at \(235,235\)/.test(pr.stdout),
     'and seats the shadow row at its own padded draw point',
     (pr.stdout.match(/shadow.*draws.*/) || ['none'])[0]);
  ok(/shadow\[0\] centre \(255,425\) = hand centre \(253,423\) \+ light \(2,2\), crop pivot \(18,188\)/.test(pr.stdout),
     'and the light offset survives the projection exactly as the composer encoded it',
     (pr.stdout.match(/shadow\[0\] centre.*/) || ['none'])[0]);
  ok(/plate bank: 2 extra keyframes at 434312, 868624/.test(pr.stdout),
     'and the projection fills the bgAnim table with the keyframes\' own offsets',
     (pr.stdout.match(/plate bank.*/) || ['none'])[0]);
  ok(/tourbillon\s*\[\d\] 64x64 draws at \(200,200\)/.test(pr.stdout),
     'and the browser\'s carriage bake projects as a tourbillon bank at its row\'s box',
     (pr.stdout.match(/tourbillon.*/) || ['none'])[0]);
  ok(/2 complications/.test(pr.stdout) &&
     /comp\[0\] window src=date box 40x24 at \(8,8\) 31 plates .* stride 2880 value_min 1/.test(pr.stdout) &&
     /comp\[1\] window src=second .* 60 plates .* stride 3072/.test(pr.stdout),
     'and the dial\u2019s own projection seats both comp rows: date over 31 plates from value 1, second over 60',
     (pr.stdout.match(/comp\[0\].*/) || ['none'])[0]);
  // D34 across the wire: the browser wrote the row, the SHIPPED C read it into a
  // clk_aux_t. This is the check the whole slice turns on — a pointer the dial cannot
  // seat is a hand that sits at twelve and reads as a watch that is simply wrong.
  ok(/2 pointers/.test(pr.stdout),
     'the shipped projection seats both pointers the browser baked',
     (pr.stdout.match(/aux\[0\].*/) || ['none'])[0]);
  ok(/aux\[0\] rotate src=second box 28x64 at \(300,120\) travel 3600 from 0 over 0\.\.60 shown/.test(pr.stdout),
     'and the rotate row arrives with its own span, origin and domain',
     (pr.stdout.match(/aux\[0\].*/) || ['none'])[0]);
  ok(/aux\[1\] slide-x src=battery box 90x10 at \(200,400\) travel 90 from -45 over 0\.\.100 shown/.test(pr.stdout),
     'and the slide arrives as pixels along x, not as an angle',
     (pr.stdout.match(/aux\[1\].*/) || ['none'])[0]);
  // §26 slice 4 across the wire: the shipped C maps the u8 twin bit onto the
  // runtime pairing flag, and the twin's centre arrives as hand centre + light —
  // the pointer twin keeps the same identity the centred hands' shadows print.
  ok(/aux\[1\] rotate src=second box 32x68 at \(302,122\).*shadow-twin/.test(pr.stdout),
     'the pointer twin projects as a flagged second row under its hand',
     (pr.stdout.match(/aux\[1\].*twin.*/) || ['none'])[0]);
  ok(/aux\[1\] shadow centre \(318,180\) = hand centre \(314,176\) \+ light \(4,4\)/.test(pr.stdout),
     'and the pointer twin keeps the §25.2 light identity exactly as the composer encoded it',
     (pr.stdout.match(/aux\[1\] shadow centre.*/) || ['none'])[0]);
}

console.log(fails ? fails + ' FAILED of ' + checked : 'RESULT: PASS  ' + checked + ' checks');
process.exit(fails ? 1 : 0);