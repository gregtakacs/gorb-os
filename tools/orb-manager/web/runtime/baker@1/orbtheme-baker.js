/* orbtheme-baker.js — the bench bake, in JavaScript.
 *
 * Mirrors tools/orbtheme.py (the container and its geometry blob), tools/theme_schema.py
 * (the complication tables), the packing half of tools/bake_clock.py (the delta-rect
 * background bank, the RGB565A8 sprites, the tourbillon frame bank) and the fold/merge
 * decisions of tools/prep_watch_kit.py. The Python stays the single definition of the
 * wire format: this file is a port, and
 * _selftest/headless-orbtheme.test.js fails if the two stop agreeing byte for byte.
 *
 * DOM-free on purpose, so the same code runs in Chrome (where the pixels come from
 * canvases) and under node (where they come from a kit's PNGs, decoded by the parity
 * harness with PIL). Everything after a plane lands is integer maths.
 *
 * A plane is {w, h, rgba}: straight (unpremultiplied) RGBA bytes, length w*h*4.
 *
 * What this port does NOT do yet, and says so in its report: bake_clock.py's --matte
 * static-fold proof (matte_plan's reachability annuli). The --bg-occlude cull IS here,
 * on by default like the bench. A theme baked here is correct, but pays blends at
 * runtime that the bench bake folds away.
 */
(function (root) {
  'use strict';

  var SCREEN_W = 466, SCREEN_H = 466;
  var KINDS = ['hour', 'minute', 'second', 'static1', 'static2'];

  /* ---- tools/orbtheme.py: the container ---------------------------------- */
  var MAGIC = 0x474f5254, FORMAT = 2, GEOM_ABI = 1, SECTOR = 4096;
  var NAME_MAX = 56;                       // one name field: 55 usable bytes + NUL
  var SPRITE_FIELDS = ['w', 'h', 'offx', 'offy', 'pivotx', 'pivoty', 'centerx', 'centery',
                       'blend', 'show', 'color_off', 'diffx', 'diffy', 'diffw', 'diffh'];
  var GEOM_COUNTS = ['n_rects', 'n_first', 'n_delays', 'n_frames', 'n_comp', 'n_aux', 'n_bank'];
  // FORMAT 2 collapsed FORMAT 1's slug+title pair into ONE name field occupying exactly
  // the bytes that pair used (words 14..27), so W_HDR_CRC stays 28 and HDR_WORDS 32: a
  // container baked for either format lands its tables in the same places, and only the
  // identity rule differs. The numbers are pinned against tools/orbtheme.py by the two
  // checks named in the header comment above.
  var W_NAME = 14,
      W_HDR_CRC = W_NAME + ((NAME_MAX + 3) / 4 | 0);
  var HDR_WORDS = W_HDR_CRC + 4;
  /* D43 P7 (§8): an app's design rides after the banks, pointed at by the header's three
   * spare words (all zero when a file carries none). The OS never reads it: it is the app's
   * bytes, opaque here, kept by the card and the shelf and never written into the slot.
   * The header CRC covers the pointer; the region carries its own CRC, so a damaged design
   * is refused as a design while the dial still installs and draws. */
  var W_DESIGN_OFF = W_HDR_CRC + 1, W_DESIGN_SIZE = W_HDR_CRC + 2, W_DESIGN_CRC = W_HDR_CRC + 3;
  // orb_theme.h ORB_THEME_UPLOAD_MAX and the hub's orbnet.MaxThemeUpload: the whole file's
  // ceiling at both doors, so a design never pushes a dial past what an orb will take
  var THEME_UPLOAD_MAX = 0x00800000 + 0x20000;
  var THEME_FIELDS = [
    ['bg', 'u32'], ['plate_follow', 'u8'], ['shadow_on', 'u8'],
    ['shadow_dx', 'i8'], ['shadow_dy', 'i8'],
    ['second_sweep', 'u8'], ['second_railway', 'u8'],
    ['text_over_hands', 'u8'], ['tick_rate', 'u8'],
    ['bg_frames', 'u16'], ['bg_fps', 'i16'], ['bg_loop', 'u8'], ['bg_every', 'u16'],
    ['plate_off', 'u32'],
    ['order0', 'u8'], ['order1', 'u8'], ['order2', 'u8'], ['order3', 'u8'], ['order4', 'u8'],
    ['order_n', 'u8'], ['matte_mask', 'u8'],
    ['bg_mode', 'u8'], ['bg_key_off', 'u32'],
    ['gif_frames', 'u16'], ['tourbillon_n', 'u16'], ['tourbillon_fps', 'u16']
  ];

  /* ---- tools/theme_schema.py: the complication tables --------------------- */
  var CLK_COMP_MAX = 16, CLK_AUX_MAX = 8, CLK_BANK_MAX = 256;
  var SOURCES = ['none', 'weekday', 'month', 'date', 'hour12', 'hour24', 'minute', 'second',
                 'moon_age', 'battery', 'power_reserve', 'text', 'ampm', 'week', 'day_of_year',
                 'quarter', 'value', 'year'];
  var COMP_KINDS = ['window', 'strip', 'ring', 'flare'];
  /* D32 ruling B, mirrored from theme_schema.CLOCK_WINDOW_DOMAIN: a WINDOW whose
   * source has a fixed calendar domain carries exactly that many plates, and the
   * value frame 0 stands for is the domain's own minimum. The suite pins the
   * mirror; a drift fails the bake, not the glass. */
  var CLOCK_WINDOW_DOMAIN = {
    weekday: [0, 7],    month: [0, 12],       date: [1, 31],
    hour12: [0, 12],    hour24: [0, 24],      minute: [0, 60],
    second: [0, 60],    moon_age: [0, 30],    ampm: [0, 2],
    week: [1, 53],      day_of_year: [1, 366], quarter: [1, 4]
  };
  var FLAG_JUMP = 1 << 3, FLAG_24H = 1 << 4, FLAG_AUX_SHADOW = 1 << 8;
  // FORMAT 3's row flag column is a u8, so the pointer's shadow twin rides bit 4 —
  // the slot the element dialect leaves free for an aux row (alpha/loop/os-adv mean
  // nothing to a single-frame pointer) — and orb_clock3.c's AUX_F_SHADOW maps it
  // back onto the runtime FLAG_AUX_SHADOW above. The two names are one contract.
  var AUX_F_SHADOW3 = 1 << 4;
  var BANK_FIELDS = [['w', 'u16'], ['h', 'u16'], ['offx', 'i16'], ['offy', 'i16'],
                     ['pivotx', 'i16'], ['pivoty', 'i16'], ['centerx', 'i16'], ['centery', 'i16'],
                     ['blend', 'u8'], ['show', 'u8'], ['color_off', 'u32'],
                     ['diffx', 'i16'], ['diffy', 'i16'], ['diffw', 'u16'], ['diffh', 'u16']];
  var COMP_FIELDS = [['tag', 'u16'], ['kind', 'u8'], ['src', 'u8'], ['x', 'i16'], ['y', 'i16'],
                     ['w', 'u16'], ['h', 'u16'], ['count', 'u16'], ['first', 'u16'], ['off', 'u32'],
                     ['frame_bytes', 'u32'], ['cx', 'i16'], ['cy', 'i16'], ['radius', 'i16'],
                     ['angle0', 'i16'], ['angle_step', 'i16'], ['value_min', 'i16'], ['scale', 'u16'],
                     ['glyph_step', 'i16'], ['glyph_lead', 'i16'], ['edge_left', 'u16'],
                     ['edge_right', 'u16'], ['flags', 'u16'], ['blend', 'u8'], ['show', 'u8'],
                     ['reserve', 'u32']];
  var AUX_EXTRA_FIELDS = [['tag', 'u16'], ['src', 'u8'], ['kind', 'u8'], ['host', 'u16'],
                          ['span', 'i16'], ['zero', 'i16'], ['value_min', 'u16'],
                          ['value_max', 'u16'], ['flags', 'u16']];
  var AUX_FIELDS = BANK_FIELDS.concat(AUX_EXTRA_FIELDS);

  /* The caps main/art/orb_theme.c refuses over, restated here so the composer refuses on
   * the bench rather than handing out a file the dial then rejects when it stages it.
   * P6.2's budget gate names them as the device's PRE-ERASE sums (orb_theme.c's
   * copy_into): geometry installs at slot +0x1000 under the banks at +0x10000
   * (SLOT_GEOM_CAP), banks install at +0x10000 inside the 8 MB slot — the payload
   * budget below, ORB_THEME_SLOT_CAP = 0x7F0000. The figure is DERIVED here from the
   * same two partition facts the device keeps, not re-declared: §7.4's bug was tools
   * each carrying their own copy, this one still carrying the retired 4 MB pair's
   * 0x3F0000. Each cap is sector-aligned, so == cap installs to the byte.
   * Refusals end in copy_into's own words, so the bench and the dial say one sentence. */
  var RECTS_MAX = 2048, GIFS_MAX = 32, BG3_FRAMES_MAX = 20, TOURB_MAX = 16;   // CLK_BG_MAX, CLK_TOURB_MAX
  /* The tempo a sweep is SAMPLED at when nothing on the wire names one: the
   * scheduler's ceiling (ORB_SCHED_FPS_CEILING, main/gfx/orb_sched.h), which is
   * also the rate the studio's sampler draws a sweep at. bakeClock3's cadence
   * default below reads it, so the two figures cannot drift apart. */
  var SWEEP_TIER3 = 15;
  var DEVICE_SLOT_CAP = 0x00800000;     // orb_theme.c SLOT_CAP: one 8 MB slot
  var SLOT_ART_OFF2   = 0x00010000;     // orb_theme.c SLOT_ART_OFF: banks start here
  var ORB_THEME_SLOT_CAP = DEVICE_SLOT_CAP - SLOT_ART_OFF2;  // 8,323,072 B of banks
  var SLOT_CAP = ORB_THEME_SLOT_CAP;    // writeContainer's payload budget (FORMAT 2)
  var GEOM_BUDGET = 0xf000;

  var RANGES = {
    i8: [-128, 127], u8: [0, 255], i16: [-32768, 32767],
    u16: [0, 65535], i32: [-2147483648, 2147483647], u32: [0, 4294967295]
  };
  function val(what, t, v) {
    // NaN must not reach the wire. Every range test below is a comparison, and every
    // comparison with NaN is false, so a NaN slips past them all and >>> 0 below turns it
    // into a plain 0: a word that reads as a legitimate zero, in a container whose three
    // CRCs still check. That is how a misnamed property shipped a dial full of noise.
    var n = typeof v === 'number' ? v : Number(v);
    if (!Number.isFinite(n) || v === null || typeof v === 'boolean') {
      throw new Error(what + ': ' + JSON.stringify(v) + ' is not a number the wire can carry' +
        ' — the bake computed undefined or NaN, which would pack as a silent zero');
    }
    v = Math.trunc(n);
    var r = RANGES[t];
    if (!r) throw new Error('unknown wire type ' + t + ' for ' + what);
    if (v < r[0] || v > r[1]) {
      throw new Error(what + ': ' + v + ' does not fit a ' + t + ' field (' + r[0] + '..' + r[1] +
        ') — the bake computed something the firmware cannot store');
    }
    return v >>> 0;
  }
  function sext(v, t) {
    var bits = t === 'i8' ? 8 : t === 'i16' ? 16 : t === 'i32' ? 32 : 0;
    if (!bits) return v;
    v &= (1 << bits) - 1;
    return v >= (1 << (bits - 1)) ? v - (1 << bits) : v;
  }
  function sprType(f) {
    if (f === 'w' || f === 'h' || f === 'diffw' || f === 'diffh') return 'u16';
    if (f === 'color_off') return 'u32';
    if (f === 'blend' || f === 'show') return 'u8';
    return 'i16';
  }

  /* ---- CRC-32 ------------------------------------------------------------- */
  /* The same polynomial as zlib.crc32 and esp_rom_crc32_le on the device, so a checksum
   * computed here is checkable there without a second implementation free to drift. */
  var CRCT = (function () {
    var T = new Uint32Array(256);
    for (var n = 0; n < 256; n++) {
      var c = n;
      for (var k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
      T[n] = c >>> 0;
    }
    return T;
  })();
  function crc32(u8) {
    var c = 0xffffffff;
    for (var i = 0; i < u8.length; i++) c = CRCT[(c ^ u8[i]) & 255] ^ (c >>> 8);
    return (c ^ 0xffffffff) >>> 0;
  }

  /* ---- the geometry blob -------------------------------------------------- */
  function wordsToBytes(w) {
    var b = new Uint8Array(w.length * 4), v = new DataView(b.buffer);
    for (var i = 0; i < w.length; i++) v.setUint32(i * 4, w[i] >>> 0, true);
    return b;
  }
  function blankSpr() {
    var r = {};
    for (var i = 0; i < SPRITE_FIELDS.length; i++) r[SPRITE_FIELDS[i]] = 0;
    return r;
  }
  function packSpr(rec) {
    var out = [];
    for (var i = 0; i < SPRITE_FIELDS.length; i++) {
      var f = SPRITE_FIELDS[i];
      out.push(val('sprite.' + f, sprType(f), rec[f]));
    }
    return out;
  }
  function packRecords(recs, fields, cap, tag) {
    if (recs.length > cap) {
      throw new Error(tag + ': ' + recs.length + ' records exceed the cap of ' + cap +
        ' — the extra rows would be dropped by the firmware');
    }
    var out = [];
    for (var i = 0; i < recs.length; i++) {
      for (var k = 0; k < fields.length; k++) {
        var name = fields[k][0], t = fields[k][1];
        if (!(name in recs[i])) throw new Error(tag + '[' + i + '].' + name + ' is missing from the record');
        out.push(val(tag + '[' + i + '].' + name, t, recs[i][name]));
      }
    }
    return out;
  }
  function fieldsOf(kind) {
    return { bank: BANK_FIELDS, comp: COMP_FIELDS, aux: AUX_FIELDS }[kind];
  }
  function wordsOf(kind) { return fieldsOf(kind).length; }


  /* The exact blob length, in words, that a set of counts declares. orbtheme.py's
   * geom_need and orb_theme.c's geom_counts() compute the same number from the same
   * counts; a blob whose length disagrees is refused by the dial as damaged. */
  function geomNeed(counts, nCounts) {
    var need = nCounts + THEME_FIELDS.length + (5 + 5 + 1 + 16) * SPRITE_FIELDS.length;
    need += counts[0] * 5 + counts[1] + counts[2] + counts[3];
    if (nCounts === 7) {
      need += counts[4] * wordsOf('comp') + counts[5] * wordsOf('aux') + counts[6] * wordsOf('bank');
    }
    return need;
  }

  /* The geometry blob, in orbtheme.py's pack_geom order: counts, scalars, the 27 sprite
   * records (5 hands, 5 shadows, the overlay, 16 tourbillon slots padded with blanks),
   * then the variable tables. A bake with no comp/aux/bank rows emits the four original
   * count words and nothing after bg_frames — the blob every theme baked before the
   * calendar existed, and the one a device without them still loads. */
  function packGeom(m) {
    var i, w = [];
    if (m.rects.length % 5) throw new Error('rects must be (x, y, w, h, off) quintuples, got ' + m.rects.length + ' words');
    var counts = [m.rects.length / 5 | 0, m.rectFirst.length, m.gifDelays.length, m.bgFrames.length,
                  (m.comps || []).length, (m.aux || []).length, (m.bank || []).length];
    var nCounts = (counts[4] || counts[5] || counts[6]) ? 7 : 4;
    if (nCounts === 4 && ((m.comps || []).length || (m.aux || []).length || (m.bank || []).length)) {
      throw new Error('comp/aux/bank records need a seven-count blob; refusing to emit a blob a reader would misparse');
    }
    for (i = 0; i < nCounts; i++) w.push(val(GEOM_COUNTS[i], 'u32', counts[i]));
    for (i = 0; i < THEME_FIELDS.length; i++) {
      w.push(val(THEME_FIELDS[i][0], THEME_FIELDS[i][1], m.scalars[THEME_FIELDS[i][0]]));
    }
    for (i = 0; i < m.hands.length; i++) w = w.concat(packSpr(m.hands[i]));
    for (i = 0; i < m.shadows.length; i++) w = w.concat(packSpr(m.shadows[i]));
    w = w.concat(packSpr(m.overlay));
    var tb = m.tourbillon || [];
    for (i = 0; i < Math.min(TOURB_MAX, tb.length); i++) w = w.concat(packSpr(tb[i]));
    for (; i < TOURB_MAX; i++) w = w.concat(packSpr(blankSpr()));
    for (i = 0; i < m.rects.length; i++) {
      w.push(val('rect[' + (i / 5 | 0) + '].' + (['x', 'y', 'w', 'h', 'off'][i % 5]),
                 i % 5 === 4 ? 'u32' : 'u16', m.rects[i]));
    }
    for (i = 0; i < m.rectFirst.length; i++) w.push(val('rect_first[' + i + ']', 'u16', m.rectFirst[i]));
    for (i = 0; i < m.gifDelays.length; i++) w.push(val('gif_delays[' + i + ']', 'u16', m.gifDelays[i]));
    for (i = 0; i < m.bgFrames.length; i++) w.push(val('bg_frames[' + i + ']', 'u32', m.bgFrames[i]));
    w = w.concat(packRecords(m.comps || [], COMP_FIELDS, CLK_COMP_MAX, 'comp'));
    w = w.concat(packRecords(m.aux || [], AUX_FIELDS, CLK_AUX_MAX, 'aux'));
    w = w.concat(packRecords(m.bank || [], BANK_FIELDS, CLK_BANK_MAX, 'bank'));
    if (geomNeed(counts, nCounts) !== w.length) {
      throw new Error('geometry blob came out ' + w.length + ' words but its counts declare ' +
        geomNeed(counts, nCounts) + ' — the packer and the size rule disagree');
    }
    return wordsToBytes(w);
  }

  function setWord(hdr, base, text, nbytes) {
    var enc = new TextEncoder().encode(text);
    // Refuse, never truncate, exactly as orbtheme.py's _setword does: the name inside the
    // header IS the theme's identity, since the card filename is theme_id of these bytes,
    // so cutting an over-long one in half would file the theme under the hash of a string
    // its owner never typed. The studio's nameOk applies the same cap before the bake runs;
    // this is the backstop for a caller that skipped it. A truncating writer and a refusing
    // authority are the drift the mirror suite exists to catch.
    if (enc.length > nbytes - 1) {
      throw new Error('name is ' + enc.length + ' UTF-8 bytes; the header holds at most ' + (nbytes - 1));
    }
    for (var i = 0; i < enc.length; i++) {
      hdr[base + (i / 4 | 0)] |= enc[i] << (8 * (i % 4));
      hdr[base + (i / 4 | 0)] >>>= 0;
    }
  }
  function hdrCrc(hdr) {
    var z = hdr.slice();
    z[W_HDR_CRC] = 0;
    return crc32(wordsToBytes(z));
  }
  function packHeader(o) {
    // The name IS half the theme's identity: main/art/orb_theme.c files the container
    // under sha256(slug/name)[:12] and refuses a container whose name field is empty,
    // so baking one with no name would only produce a file the device can never list.
    // Refuse it here, where the reason can be said, instead of at the card.
    if (!o.name) throw new Error('the theme has no name to bake — the card files a theme by sha256(slug/name)[:12] and refuses a container with an empty name');
    var hdr = new Array(HDR_WORDS).fill(0);
    hdr[0] = MAGIC >>> 0; hdr[1] = FORMAT; hdr[2] = GEOM_ABI; hdr[3] = HDR_WORDS * 4;
    hdr[4] = HDR_WORDS * 4;
    hdr[5] = val('geom_size', 'u32', o.geomSize); hdr[6] = val('geom_crc', 'u32', o.geomCrc);
    hdr[7] = val('art_size', 'u32', o.artSize); hdr[8] = val('art_crc', 'u32', o.artCrc);
    hdr[9] = val('scr_w', 'u32', o.scrW); hdr[10] = val('scr_h', 'u32', o.scrH);
    hdr[11] = val('serial', 'u32', o.serial || 0);
    hdr[12] = val('baked_ms', 'u32', o.bakedMs || 0);
    hdr[13] = val('art_off', 'u32', artOffset(o.geomSize));
    setWord(hdr, W_NAME, o.name || '', NAME_MAX);
    hdr[W_HDR_CRC] = hdrCrc(hdr);
    return wordsToBytes(hdr);
  }
  /* Payload start for a given blob size, sector-aligned: the device mmaps the payload
   * straight out of the assets partition, and an unaligned start costs an MMU page
   * entry to round. */
  function artOffset(geomSize) { return (HDR_WORDS * 4 + geomSize + SECTOR - 1) & ~(SECTOR - 1); }

  /* The card filename's identity tail: hex(sha256(slug + '/' + name))[:12], lowercase
   * — the same rule tools/orbtheme.py's theme_id() defines and main/art/orb_theme.c's
   * theme_id() mirrors, over the OWNING APP and the name: per-app theming made the pair
   * the identity, so the caller digests 'clock/Av Kit', not just the name.
   * The DIGEST arrives from the caller, because the baker runs in three places: Chrome
   * (window.crypto.subtle.digest, in the studio's own async path), node (the mirror
   * suite, require('crypto')), and nowhere that has a hash this file may reimplement.
   * A hand-rolled SHA-256 here would be a third implementation of the one rule the
   * whole format hangs on, checked against the other two only by whoever wrote it.
   * Six bytes is the entire id: reading further would be a second, freer rule. */
  function themeIdFromDigest(digest) {
    if (!digest || digest.length < 6) return null;
    var h = '';
    for (var i = 0; i < 6; i++) {
      h += (digest[i] >> 4).toString(16) + (digest[i] & 15).toString(16);
    }
    return h;
  }

  /* The card stem's legible middle: theme_name_token() in main/art/orb_theme.c and
   * name_token() in tools/orbtheme.py restated over the UTF-8 bytes of the name. The
   * studio offers it so a download can be named like the card will name the file; the
   * card's own rule, and every hand rename that keeps slug and hex, still works. */
  function nameToken(name) {
    var bs = new TextEncoder().encode(String(name)), out = [], pending = false;
    for (var i = 0; i < bs.length; i++) {
      var c = bs[i];
      if (c >= 65 && c <= 90) c += 32;
      var alnum = (c >= 97 && c <= 122) || (c >= 48 && c <= 57);
      if (!alnum) { pending = out.length > 0; continue; }
      if (pending && out.length < 24) out.push(45);
      pending = false;
      if (out.length >= 24) break;
      out.push(c);
    }
    var tok = String.fromCharCode.apply(null, out).replace(/-+$/, '');
    return tok || 'theme';
  }

  /* The whole card filename stem: <slug>-<token>-<12 hex>. Pass the digest of
   * slug + '/' + name (UTF-8), matching themeIdFromDigest's rule. Returns null when
   * the digest leg is missing — a studio without crypto keeps its old download
   * name, and the device renames the upload from the header anyway. */
  function cardStem(slug, name, digest) {
    var id = themeIdFromDigest(digest);
    if (!id || !slug) return null;
    return slug + '-' + nameToken(name) + '-' + id;
  }

  /* A theme name in the unit the header actually measures: UTF-8 BYTES, not JS
   * characters. setWord cuts at NAME_MAX - 1 to leave the NUL the C reader stops at, so
   * "Mirage — Gold" at 15 characters is 17 bytes, and the number the studio has to show
   * an owner is the one the field enforces. An embedded NUL is refused outright: the
   * reader would stop early and the tail of the name would silently not be the name. */
  function nameBytes(s) { return new TextEncoder().encode(String(s)).length; }
  function nameOk(s) {
    return typeof s === 'string' && s.indexOf('\u0000') < 0 &&
           s.length > 0 && nameBytes(s) <= NAME_MAX - 1;
  }

  /* One .orbtheme: header, geometry, zero padding to the sector, pixel payload. */
  function writeContainer(o) {
    var geom = o.geom, art = o.art;
    var off = artOffset(geom.length);
    if (art.length < SCREEN_W * SCREEN_H * 2) throw new Error('payload is shorter than one full plate');
    if (geom.length > GEOM_BUDGET) {
      throw new Error('geometry is ' + geom.length + ' B, over the ' + GEOM_BUDGET +
        ' B of tables a slot holds — geometry would overrun the pixels');
    }
    if (art.length > SLOT_CAP) {
      throw new Error('payload is ' + art.length + ' B, over the ' + SLOT_CAP +
        ' B a theme slot holds — theme is larger than a slot; cut bank frames or sprite size');
    }
    var hdr = packHeader({
      geomSize: geom.length, artSize: art.length,
      geomCrc: crc32(geom), artCrc: crc32(art),
      scrW: o.scrW || SCREEN_W, scrH: o.scrH || SCREEN_H,
      serial: o.serial || 0, bakedMs: o.bakedMs || 0, name: o.name
    });
    var out = new Uint8Array(off + art.length);
    out.set(hdr, 0);
    out.set(geom, HDR_WORDS * 4);
    out.set(art, off);
    return out;
  }

  /* ---- pixel planes: bake_clock.py's Blob + sprite encode ---------------- */
  function Blob() { this.parts = []; this.len = 0; }
  Blob.prototype.add = function (bytes) {
    var off = this.len;
    this.parts.push(bytes);
    this.len += bytes.length;
    return off;
  };
  Blob.prototype.join = function () {
    var out = new Uint8Array(this.len), p = 0;
    for (var i = 0; i < this.parts.length; i++) { out.set(this.parts[i], p); p += this.parts[i].length; }
    return out;
  };

  /* D43 §5.3/§5.4: the still-plane dither. A 64x64 blue-noise threshold table (made once
   * by tools/make_bluenoise.py, pinned by DITHER_SHA256 here and in tools/orbdither.py,
   * the bit-exact Python twin) tiled across the PANEL, so a texel's threshold depends on
   * where it lands on the glass and never on which crop carried it: an unchanged input
   * texel bakes to an unchanged output texel in every keyframe and delta rect.
   * Colour: level = base[v] + (frac[v] > T). base[v] is the highest level whose device
   * decode is <= v and frac[v] where v sits toward the next, 0..255 — a 565-exact colour
   * has frac 0 and never moves, and the mean decode is v, so truncation's darkening goes.
   * Alpha: coverage lands on the 32-step lattice mix_16_16 reads ((a + 4) >> 3), stored
   * as m5 * 8, with the table shifted (32, 32) so it never correlates with the colour. */
  var DITHER_B64 =
    'YrkgVTkcY+YHfDkb7j/WlrFyBrwsTpI4w0oXj1TUBl3tFWGO0yKygjqTYuVDG+5N0iVtSJp3zCbt1G+b48Ek83804KyExJNE' +
    'pflorItxVRY45Vv7f9sD4yNny3Y7lvoqrHfgOVGhEerYUQxymFnAiDqoyBIy8UJrixOuXgk7ks1Olghs9BHSdCJUySy6CP2B' +
    'xCGbPhvEXLKI/Qvppxtqv02TJsJw8UZpK6XRtfcqoxX7dlXohbAMpsMwRfmH6msXpv7CQZxRMuq5iA/lTc6fYapP12yrji11' +
    'QqFXLLvhhjfVB/iwF4DGj7wdgjwR2Htf2wmRtiHYTvZa3Xe6KVez3Ct3YCbatn8AYdg2l3cgNN8K9YgPzUn20xbA231CWhDo' +
    'oWWCVDLWCj39W+FqkEzGMa1Hzzhgc5skgwmXGtadBkLnDdCKE2f7qkOe92W08I5Heyy9N+lhC5poOZIA0ZlytiJB0KfnmWKo' +
    'eQWfJLPyAJdu7xuA/wPQOuawZO9LcMeGuJNGsuU+kBzMJYEXVALIadKcV3KlI3y34x/3baol/k+Q7hAnb0ryIdBQ7MRAgFjh' +
    'I7yeU8KtiWe/Ry7LgjX5VWsx9ll5K9VWcbpI0+mgO7ES7BzcjMjwP1iIs0vfOskFabt7xo8VwDe1hzFmDqvMOYVjDuQrSB7y' +
    'D3qhALYfqBXhnh6qBL2c8grnpC6JcyH4TYOuQQJSLKoIyStiF4x7r+ExW/o624Fo6BGX0/xzGqP3Q9SRbaXZVZjT92HmkEHJ' +
    'CE7UhOxkRH4zj2QUt1fEkWMwy23/umWV2G/sob30WSJFmAmlT7ABnkx1sUUjkVLcB7N6NPQTfbclPVMWctZYfY62bj0nxRmy' +
    '0U7A+j3XCOCnDOqWHoLlFn1JDzx0C9OjwuyGzB5l8yzcHe1evuMya5VbHb5Ryzfrb4/GrDieJewu+RGZ4In/XCLkBHycbS1G' +
    'dL5WNrJHzTr1mMTZj1IzcRVhKnDmk8ZZu4c5nQN/p8Qm0e6iApVgogjkHYf+DL9lRsVbrk1xCaONaalJHumsjPshhOJpCaBc' +
    'H7JjGe+14ZL8Tde2Pw5+PKYMzW3xQxT6TIc9aX/9HtBOsGLRSXjeqQF65xvXMLpE8DfD3oXHYBHPPKEWw/h3vt4yiE0ofQE/' +
    'q4MLnlb4JNlh/00orspdc7YO4rAuwkB38jV+KbhdHYrQmzlpkcmB2hV6J1oUOfBQs2vXWoskTpADcf2ryp9p0CHBNt50q8OO' +
    'GZp734sfmtY0lWEa2lmSuBOY3QTvnTLySiC1+wVKJV+s0pr9uKB5KpcA9EOt3DXuokXWD1sy9lN28WaREi9PbucyuQdYPeQJ' +
    'feu/RIwK6yplyFSohUPCZHXfhlem5Jj3cAJDjGwIv9+DxjB8C2vJGGC8JXaN1BS3mQZG6biC8QKvRddx87xmqVAlovh5x6dL' +
    'hPs3GmnlCqsTxTQYwXgSNU/H6y9S50FiG0yd6biOU6l75pc+6q1Khi/apSdbxz/PhV8lpBKSLfzIcABdNh9s2Aiuc7vUKpP4' +
    'VqFs7j5d3aK5eR2t1Ygl9bDVaB494ij6Nw5XwgVlIu7DYH7QFqBqHZz4w3xMzoEYQInSseSb9TyUJuFJnFqAPuQn05OuI86I' +
    'C+KXYwzConMLh/1Y0XYSwYbbp/h/othxQg/zTJD7MuJSCTfhIOxcuJjfLEuFDFm3zV6JBfUZz7cCiUkUffxDZy9YP/V9NlbN' +
    'RC6jBpSyX5tDai0aUDO5F5mvNL1zBrB8v41jrpZuOQfwZBq/atQqfBLxQcR1qTJlc7LyYMAEnb3xpswjs9kRmPC0bcdIMPTW' +
    'Ab6SzeOQWf6H5Ggg4VnZRSfr0EcOwdqneU+q/zyj7Eqsb5os50+V/8U6oCnlVXUdgxNwj0dq5n8iUd0Y6YgdUn3xXT91CMcq' +
    'SQDOgz2iihBtnxl79SqFQxXPgASSHsWLMdActWAM2iJaC9xujTfT50nE4QH+qRlgu5A3fqVnzK8poRS07KFs23umVLf3F8+6' +
    '/lkvu1GgYPm3M+pZ3HVfC/tT4I56vESB75NOzBC3oitjrDpfmyzJPdUD9Fu+CDzma8c4hidMGbEz8BSVLWpSNYSw2ZHnAsoj' +
    'jGydJK1B5rygawA/8xnTqBa7eiT6RYEG940gfcBOh+ydcLAhRvqcihBN/ddjxfeLYNNyRteApeUESBZmOHavSOMJwkvOFIE4' +
    'IofFrSuZazBi6TqullvWccBQ0fAK4msTVDCF3sh4LVfSdpUEq345CsAen7wH7B3Hd5v0zYkf1plkOPNwjPm1VtbrSWbMUPiN' +
    'SdMDacYS7TWaEG6wPqQmtfvFDZZkGLbupCW7RR/nUJrkVzv2XY5LMWS1KlCn/VYTu4OnGjFhBpd3GKEP4nsHtSWehuZBjCOz' +
    'RugpkFd725JCZ+ZQNdiCCD/aW/Nro9VxLoateSnDqdTvEn/ACTtz7CndVcjlotlDuTD+kSenPMhm/VUsqc9ogdhguPgYyjcF' +
    'eKoooPSsSsVtjw6CyykRu/8G1hjgbwpXikLjXtufxZFDdwGUSnQn8WPRT3DBWOiAzxi5dRhL+wGiFH8zqOZivvMX1XwAaCab' +
    '+TPktjyTZUaPX8FQm0D8oCK9lS55HWMO0a/8Nr0QroIMpYk53BySD6Q6lO/bmL9YPMnfTm0Oh0eYWLo8kM/qWRetdR1T3avq' +
    'HqU674W5L3rqagDxsEr1pVYlZoTuXdM9wyPqArJq9kxv1lEHYzMfjfBqG4zDoO4f3S9u/1IUsn9Dyl6d9QJ3MsyAbCIEYtcQ' +
    'zFGkQciG3TWM5MkcoSmSbfZXdc5CnzDA8iiHx3yr1XS3K6v7PShcyn2vC8mkczXelwrZLr+K01wN9a3H5I1KqziH2RVtKQVy' +
    'thFGeNlR4xa0ni6NXeF7BVyv4RRF9lMIQpdbBtp4swI/i+onR+XGIWv8hEpuIT63lU01WZ8p7mjEI2D+kb3UT/lgmcEFqTOE' +
    'Rwm/+xIktpAdPWiVvByd3cDohMpMmPNp31SaZYMOjlavOLsT6Kb6aN4a0nYQunwXl+enMlWfPoCmLOk8b/+8ZujQT36h0Unp' +
    'zaD0L9iCZSh5FDdtuRk1piO+FbP3L73vApxf0o1TCH8usor7Qdcz+FAPdskK7x/fDMl6II9bFJsmk2427WQxcwxNdgJYNrL/' +
    'TqXzJOOLWc558DnXUKNnQX/eIHaxNMmg7WMBqGuVXKmFvkTgiXC0X5BLr9LtRcbaQfMZrwbGl7OG3r2n7c2ODMViiKxHEP2U' +
    'CU1ujgd56BbHUvVDEdxdHUDMUSjrGs4K3iRlsStN0jb3HGQInSx4AIO5V96LUxr+XzsWiWoYQ3LhOwHXdMNmPrHioB/B0yiw' +
    'iy6nwITymLp4lN29hEu3bkDvlRP7pAWYcr3dglO05aNiL810PuZsLZzG9ipOmumoIpi6VTCe3CZ+yjH6VDuZX9dyB2QnSW8G' +
    '/jETZjnZjiujeNI3Yn3C4yZEpDj2bB5M/IwTnCa7ptgOdFSx28EwWcp5+hzrgQW8UhVlhrVs8QtI/Zzl0q42y1ivgPCqB/xS' +
    'xwFWuuhAGVGF8hbEDY6/1Aur4E/4A31ItemQHHAEfu8ORGGPsUBg95bqqQDlG4DDMrYeVIoS640g4EedJV16rR30iSOnktdm' +
    'tZRcdNtDYDF5QGrGiF7tJGczzkH9qUmTs9otzBLkpDFyRdA2lEul4VmSezzDckymZb8Nbs65PeGUR2zPDnQr7gIx0Z0rruuX' +
    't/EaLrE7zYvABaFfiC3PZSCCpmxReNEQwiB8Xdm+InMPy+0EpPkbzjt++DWK7hFnLdq2NPFIxJp45UsZ+4EEIc9ao3vbEZ9T' +
    '33btFdi2E/U+4grzuiGIWuKOtP8RaT70oCto2lwshN4DlVnWGFGhyoAJmVqFrV4cP8CDtGhP1nJHig3sTnD2GpVKrztxVpp4' +
    'wFuXRzSc+T2lB08tjKrPg1CxiUS6nGpQreshm7R8OOquSvof3wfV/6hbDeg7qJMt+aw6vpAptTjKJoTG8yLcAKcqyXvYA79u' +
    'Ku/FdN5XAzPcEvwe1Ar0xDBzyERo/gJeI3HCfJ83cIss0ZkoyxTiu2Mczl0B1X1aDfpiB6I3jUxr/hmPZ+tPgbFclD4b9ruW' +
    'a8VYejmORhiKVhLjLcuIvJbmEEHLVbwRTXXwYox3VAeE2neZ/0Kq4nO4llDmesLjtEDUVCeoFM0e4A3WoHtFIuo9k6rrb7Lc' +
    'ovK6fagcP9pSNLBn4yTzouCzA0i+9j+d7jZOI7hpGI8pP9UarWMOLoUVmbnjOpL8SIlotCld4amBBr4qFspfJ3k7BpRa7HWj' +
    'FoPwmQCMdl8/JpTXMRuzXcEPqOKGM+1RqOh/L8dC+Z9e6W8FfMRbci6l8UzNkQvIVvNn2FKXAP1Oz2zeSL4J92rKJlS6NdgX' +
    'woL8ZqR+5ChxjctWCcOazFsMZvSScSDSSK8y+EaeDtm4ATl4Ff1uQB+hNITvPruDqBq0NCGZXjGsRNl6+0iskudXD8pMBpTQ' +
    'SPgfZ/FAbxXTi7tMA+C4eQnGh13UI/CDVMvijLcynd25cswPrnTgMGHpjfp+zuaLxAaUGWTOCmoosTd4t/VjE6Q5frWeJIP8' +
    'M6Mi16lcNJPyKJsWrmk1qxyVYiFS6V6BEvpCmSZYDcudQgtaqRFLH3VT67MvoH7yRtub6iE+2YW84wLUT+CvSW3rej6HG+xL' +
    'Z9FR5XrFSt9xQPfFrgXUKU+PX9rB8pFtJNV5wzxwsdH+ojuE3FYgyIpeFmmPqlcra02WLnYYwQTIUg36v2yesw1/vDoD/IkM' +
    'vZ8QgDZ0lqritgQxgEcasvhTpCrZ84kwZRO+bg35tzsDrNU0yQjtwhv8rGTrilya3Y6uYS/TFd1B9yCjYZcrWuww1V7tRcgc' +
    'OG/po2a53ziFA+iLGl4FR5beJ8tJkWib6HpR/4JJdJ2H0DwQy0D5LRVEI+OUUHssjapv6kLB26l6S42oF4pj9VWGxCH/Cnaf' +
    'YchGbLmbyuezflWe5TMZz0QntRGhJdU6C1p8upQerH287nTIAbj0YsNTCtGCEm8fzQT+KcLgB5jSE0iTOljWKO4XqjXfTi1x' +
    'EDjwAXGwgfVhxpBq371f8bXgJ/JuU9JnlVWkNoVCog/nNpYosFX2PZxYtHBSNLFyK6bveMyvh0u0c5H0DICt+WHFjLgk1FML' +
    'phvtNlQAlnAcj0qmA+Y7DSzVGfxm3CTQdbL/aOI1kLnfghvYmoD5Rb/aVwGfHfYN0lYgzWTBGJEo20Ve/UCTvnM/zn+t+kUv' +
    'q85kNL9/n/eIb0ayDJpNiloXRoYCxncOZC3vQg/JHl4NgzNt3kRpmC7nfkSeMuRLp3UXpIYQ3TDnilgPKojB5HwI/onbF1rH' +
    'BOiR0HfwtC/A3ZzPXKYm8krLi6Vn6Kp37Zy49CW0gcdAo70B/nZcygX1vS/MeKtmBJ72uNxcFJ5VPLYgTHWrQQ==';
  var DITHER_SHA256 = '683a759b831dc8d0c83205827301e65adbb2d06f6b5b11af800a694f6f8f2bc7';
  var DITHER_N = 64, DITHER_ALPHA_SHIFT = 32;
  // Decoded here rather than through atob/Buffer: the baker also runs inside bare vm
  // sandboxes (the headless suites) that carry neither.
  var DITHER_T = (function () {
    var A = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/', t = [], acc = 0, bits = 0;
    for (var i = 0; i < DITHER_B64.length; i++) {
      var v = A.indexOf(DITHER_B64.charAt(i));
      if (v < 0) continue;                        // '=' padding
      acc = (acc << 6) | v; bits += 6;
      if (bits >= 8) { bits -= 8; t.push((acc >> bits) & 255); }
    }
    return new Uint8Array(t);
  })();
  function ditherLuts(bits) {
    var n = 1 << bits, dec = [], base = new Uint8Array(256), frac = new Uint8Array(256), l = 0, v;
    for (v = 0; v < n; v++) dec.push((v << (8 - bits)) | (v >> (2 * bits - 8)));
    for (v = 0; v < 256; v++) {
      while (l + 1 < n && dec[l + 1] <= v) l++;
      base[v] = l;
      frac[v] = (l + 1 < n && v > dec[l]) ? Math.floor(256 * (v - dec[l]) / (dec[l + 1] - dec[l])) : 0;
    }
    return { base: base, frac: frac };
  }
  var D5 = ditherLuts(5), D6 = ditherLuts(6);
  function ditherQuant565(r, g, b, t) {
    return ((D5.base[r] + (D5.frac[r] > t ? 1 : 0)) << 11) |
           ((D6.base[g] + (D6.frac[g] > t ? 1 : 0)) << 5) |
           (D5.base[b] + (D5.frac[b] > t ? 1 : 0));
  }
  function ditherAlphaStep(a, t) {
    if (a === 0 || a === 255) return a;
    var exact = a * 32, lo = Math.floor(exact / 255), fr = Math.floor((exact - lo * 255) * 256 / 255);
    var m5 = Math.min(lo + (fr > t ? 1 : 0), 32);
    return m5 >= 32 ? 255 : m5 * 8;
  }

  /* D43 §5.2: the panel, simulated. The device composites in RGB565 on its own mirror:
   * a plate quantized as the bake stored it, then every live row and the crystal overlay
   * through blend_px (main/gfx/orb_gfx.c), whose NORMAL/MULTIPLY/ADDITIVE modes all end
   * in mix_16_16 — coverage on 32 steps, each field truncated. These are those two
   * functions, operation for operation (Math.imul and >>> carry the C's uint32
   * wraparound), so the composer's Panel view shows what the glass will, banding and
   * darkening included. headless-panel.test.js holds them to the C on random inputs. */
  function mix1616(c1, c2, mix) {
    if (mix === 255) return c1;
    if (mix === 0) return c2;
    if (c1 === c2) return c1;
    var m5 = (mix + 4) >> 3;
    var bg = (c2 | (c2 << 16)) & 0x7E0F81F, fg = (c1 | (c1 << 16)) & 0x7E0F81F;
    var res = (((((Math.imul((fg - bg) | 0, m5) + 0x2008010) >>> 0) >>> 5) + bg) & 0x7E0F81F) >>> 0;   // D43 §5.4: rounds, as the firmware does
    return ((res >>> 16) | res) & 0xffff;
  }
  function blendPx(dest, c, a, mode) {
    if (mode === 1) {                                  /* MULTIPLY (shadow layers) */
      if (c === 0xffff) return dest;
      var res = ((((dest >> 11) * (c >> 11)) >> 5) << 11) |
                (((((dest >> 5) & 0x3F) * ((c >> 5) & 0x3F)) >> 6) << 5) |
                (((dest & 0x1F) * (c & 0x1F)) >> 5);
      return mix1616(res, dest, a);
    }
    if (mode === 2) {                                  /* SCREEN (mapped to ADDITIVE) */
      if (c === 0) return dest;
      var r = Math.min((dest >> 11) + (c >> 11), 31), g = Math.min(((dest >> 5) & 0x3F) + ((c >> 5) & 0x3F), 63),
          b = Math.min((dest & 0x1F) + (c & 0x1F), 31);
      return mix1616((r << 11) | (g << 5) | b, dest, a);
    }
    return a >= 255 ? c : mix1616(c, dest, a);        /* NORMAL */
  }
  /* A whole-panel RGBA plane as the codes the plate row stores: dithered at the panel
   * origin when `dither`, truncated otherwise — the two ways bakeClock3 can ship it. */
  function panelPlate(rgba, w, h, dither) {
    var out = new Uint16Array(w * h), x, y, i, t;
    for (y = 0; y < h; y++) for (x = 0; x < w; x++) {
      i = y * w + x;
      if (dither) { t = DITHER_T[(y & (DITHER_N - 1)) * DITHER_N + (x & (DITHER_N - 1))]; out[i] = ditherQuant565(rgba[i * 4], rgba[i * 4 + 1], rgba[i * 4 + 2], t); }
      else out[i] = ((rgba[i * 4] & 0xf8) << 8) | ((rgba[i * 4 + 1] & 0xfc) << 3) | (rgba[i * 4 + 2] >> 3);
    }
    return out;
  }
  /* One RGBA layer blended onto the panel codes in place, as the device blends a row:
   * o.x/o.y its top-left, o.mode the blend (0 NORMAL, 1 MULTIPLY, 2 ADDITIVE), o.dither
   * the still-plane dither the bake applies to its colour and coverage (the overlay,
   * window plates) — off for a hand, whose sprite ships truncated. */
  function panelLayer(dst, pw, rgba, w, h, o) {
    o = o || {};
    var ox = o.x | 0, oy = o.y | 0, mode = o.mode | 0, dith = !!o.dither, x, y, i, a, c, X, Y, t, ta;
    for (y = 0; y < h; y++) {
      Y = oy + y; if (Y < 0 || Y * pw >= dst.length) continue;
      for (x = 0; x < w; x++) {
        X = ox + x; if (X < 0 || X >= pw) continue;
        i = (y * w + x) * 4; a = rgba[i + 3];
        if (dith) {
          t = DITHER_T[(Y & (DITHER_N - 1)) * DITHER_N + (X & (DITHER_N - 1))];
          ta = DITHER_T[((Y + DITHER_ALPHA_SHIFT) & (DITHER_N - 1)) * DITHER_N + ((X + DITHER_ALPHA_SHIFT) & (DITHER_N - 1))];
          a = ditherAlphaStep(a, ta);
          if (!a) continue;
          c = ditherQuant565(rgba[i], rgba[i + 1], rgba[i + 2], t);
        } else {
          if (!a) continue;
          c = ((rgba[i] & 0xf8) << 8) | ((rgba[i + 1] & 0xfc) << 3) | (rgba[i + 2] >> 3);
        }
        dst[Y * pw + X] = blendPx(dst[Y * pw + X], c, a, mode);
      }
    }
    return dst;
  }
  /* Panel codes back to RGBA for a canvas, through the device's own decode. */
  function panelRGBA(codes, out) {
    out = out || new Uint8ClampedArray(codes.length * 4);
    for (var i = 0; i < codes.length; i++) {
      var v = codes[i], r = v >> 11, g = (v >> 5) & 63, b = v & 31;
      out[i * 4] = (r << 3) | (r >> 2); out[i * 4 + 1] = (g << 2) | (g >> 4); out[i * 4 + 2] = (b << 3) | (b >> 2); out[i * 4 + 3] = 255;
    }
    return out;
  }

  /* The colour plane: native-LE RGB565, w*h*2 bytes. Alpha is ignored here — every
   * texel keeps its stored colour so the planar alpha plane alone decides coverage,
   * which avoids a dark edge fringe (bake_clock.py's rgb565_le). `dith` = {x, y}, the
   * plane's top-left on the panel, dithers it (above); without it the plane truncates,
   * byte for byte what rgb565_le packs, which the parity vectors pin. */
  /* OS v1.2.12: a square picture stored as its disc (main/art/orb_disc.h): the plate (or
   * a delta keyframe), the masked glass overlay, and the preview at its own side. Row y
   * keeps x0..x1, the pixels whose centres sit within side/2 + 1 of the centre, by the
   * one integer rule the device and theme_schema.disc_span apply; tools/host_disc_test.py
   * pins the three. */
  var DISC_SIDE = 466;
  function discSpan(y, side) {
    side = side || DISC_SIDE;
    var r = (side >> 1) + 1, dy = 2 * y + 1 - side, room = 4 * r * r - dy * dy, x = 0;
    while (x < side >> 1) { var dx = 2 * x + 1 - side; if (dx * dx <= room) break; x++; }
    return [x, side - 1 - x];
  }
  function discPx(side) {
    side = side || DISC_SIDE;
    var n = 0;
    for (var y = 0; y < side; y++) { var sp = discSpan(y, side); n += sp[1] - sp[0] + 1; }
    return n;
  }
  function discBytes(side) { return discPx(side) * 2; }

  /* §60 (owner, 2026-11-07): a preview is the whole glass, stored as its disc and deflated,
   * and the orb inflates it with the ROM's own tinfl (orb_theme.c). This is that stream: a
   * zlib wrapper round one fixed-Huffman deflate block, matches found by a hash chain. It is
   * deterministic, so a re-bake of an untouched design is the same bytes in any browser and
   * in node; fixed codes cost ~15% over zlib's dynamic ones, measured on Gold's own frames. */
  var LEN_BASE = [3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258];
  var LEN_EXTRA = [0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0];
  var DIST_BASE = [1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073,
    4097, 6145, 8193, 12289, 16385, 24577];
  var DIST_EXTRA = [0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13];
  function zlibFixed(data) {
    var out = [], bb = 0, bc = 0;
    var put = function (v, n) { bb |= v << bc; bc += n; while (bc >= 8) { out.push(bb & 255); bb >>>= 8; bc -= 8; } };
    var rev = function (c, n) { var r = 0; for (var i = 0; i < n; i++) { r = (r << 1) | (c & 1); c >>= 1; } return r; };
    var sym = function (v) {                        /* RFC 1951 §3.2.6's fixed codes */
      if (v < 144) put(rev(0x30 + v, 8), 8);
      else if (v < 256) put(rev(0x190 + v - 144, 9), 9);
      else if (v < 280) put(rev(v - 256, 7), 7);
      else put(rev(0xC0 + v - 280, 8), 8);
    };
    var match = function (len, dist) {
      var li = 28; while (LEN_BASE[li] > len) li--;
      sym(257 + li); if (LEN_EXTRA[li]) put(len - LEN_BASE[li], LEN_EXTRA[li]);
      var di = 29; while (DIST_BASE[di] > dist) di--;
      put(rev(di, 5), 5); if (DIST_EXTRA[di]) put(dist - DIST_BASE[di], DIST_EXTRA[di]);
    };
    out.push(0x78, 0x01);                           /* CMF/FLG: deflate, 32 K window */
    put(1, 1); put(1, 2);                           /* the one block, final, fixed codes */
    var n = data.length, WIN = 32768, HB = 15, head = new Int32Array(1 << HB).fill(-1), prev = new Int32Array(WIN);
    var hash = function (i) { return ((data[i] << 10) ^ (data[i + 1] << 5) ^ data[i + 2]) & ((1 << HB) - 1); };
    var insert = function (i) { if (i + 2 < n) { var h = hash(i); prev[i & (WIN - 1)] = head[h]; head[h] = i; } };
    for (var i = 0; i < n;) {
      var best = 0, bestD = 0;
      if (i + 2 < n) {
        var c = head[hash(i)], chain = 64, max = Math.min(258, n - i);
        while (c >= 0 && i - c <= WIN - 1 && chain-- > 0) {
          if (data[c + best] === data[i + best]) {
            var l = 0; while (l < max && data[c + l] === data[i + l]) l++;
            if (l > best) { best = l; bestD = i - c; if (l === max) break; }
          }
          c = prev[c & (WIN - 1)];
        }
      }
      if (best >= 3) { match(best, bestD); for (var k = 0; k < best; k++) insert(i + k); i += best; }
      else { sym(data[i]); insert(i); i++; }
    }
    sym(256);
    if (bc) out.push(bb & 255);
    var a = 1, b = 0;                               /* Adler-32, big-endian */
    for (var j = 0; j < n; j++) { a = (a + data[j]) % 65521; b = (b + a) % 65521; }
    out.push((b >>> 8) & 255, b & 255, (a >>> 8) & 255, a & 255);
    return new Uint8Array(out);
  }
  // a square plane (`bpp` bytes a pixel, 2 by default) to its disc
  function discPack(sq, side, bpp) {
    side = side || DISC_SIDE; bpp = bpp || 2;
    var out = new Uint8Array(discPx(side) * bpp), o = 0;
    for (var y = 0; y < side; y++) {
      var sp = discSpan(y, side), a = (y * side + sp[0]) * bpp, b = (y * side + sp[1] + 1) * bpp;
      out.set(sq.subarray(a, b), o); o += b - a;
    }
    return out;
  }
  // an RGB565A8 square (colour plane, then alpha plane) to its masked disc
  function discPackMasked(sq, side) {
    var n = side * side;
    return concat([discPack(sq.subarray(0, 2 * n), side, 2), discPack(sq.subarray(2 * n, 3 * n), side, 1)]);
  }
  /* '#rrggbb' as the RGB565 word a font row carries (theme_schema.rgb565). */
  function rgb565(colour) {
    var c = String(colour || '#ffffff').replace('#', '');
    if (c.length === 3) c = c[0] + c[0] + c[1] + c[1] + c[2] + c[2];
    var r = parseInt(c.slice(0, 2), 16), g = parseInt(c.slice(2, 4), 16), b = parseInt(c.slice(4, 6), 16);
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
  }
  function rgb565Plane(rgba, w, h, dith) {
    var out = new Uint8Array(w * h * 2), x, y, i, o = 0, v, row;
    if (dith) {
      for (y = 0; y < h; y++) {
        row = ((dith.y + y) & (DITHER_N - 1)) * DITHER_N;
        for (x = 0; x < w; x++) {
          i = (y * w + x) * 4;
          v = ditherQuant565(rgba[i], rgba[i + 1], rgba[i + 2], DITHER_T[row + ((dith.x + x) & (DITHER_N - 1))]);
          out[o++] = v & 0xff; out[o++] = (v >> 8) & 0xff;
        }
      }
      return out;
    }
    for (i = 0; i < w * h; i++) {
      var r = rgba[i * 4], g = rgba[i * 4 + 1], b = rgba[i * 4 + 2];
      v = ((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3);
      out[o++] = v & 0xff; out[o++] = (v >> 8) & 0xff;
    }
    return out;
  }
  function alphaPlane(rgba, w, h, dith) {
    var out = new Uint8Array(w * h), x, y, row;
    if (dith) {
      for (y = 0; y < h; y++) {
        row = ((dith.y + y + DITHER_ALPHA_SHIFT) & (DITHER_N - 1)) * DITHER_N;
        for (x = 0; x < w; x++)
          out[y * w + x] = ditherAlphaStep(rgba[(y * w + x) * 4 + 3], DITHER_T[row + ((dith.x + x + DITHER_ALPHA_SHIFT) & (DITHER_N - 1))]);
      }
      return out;
    }
    for (var i = 0; i < w * h; i++) out[i] = rgba[i * 4 + 3];
    return out;
  }
  /* The tight alpha bbox grown by pad and clamped to the canvas — crop_layer().
   * Returns null for a fully transparent image, which the caller turns into the
   * 1x1-empty / blank-record path. */
  function alphaBbox(rgba, w, h) {
    var l = w, t = h, r = -1, b = -1;
    for (var y = 0; y < h; y++) {
      for (var x = 0; x < w; x++) {
        if (rgba[(y * w + x) * 4 + 3] > 0) {
          if (x < l) l = x; if (y < t) t = y; if (x > r) r = x; if (y > b) b = y;
        }
      }
    }
    if (r < 0) return null;
    return [l, t, r + 1, b + 1];
  }
  function cropLayer(rgba, w, h, pad) {
    var bb = alphaBbox(rgba, w, h);
    if (!bb) return null;
    var l = Math.max(0, bb[0] - pad), t = Math.max(0, bb[1] - pad);
    var r = Math.min(w, bb[2] + pad), b = Math.min(h, bb[3] + pad);
    var cw = r - l, ch = b - t, out = new Uint8Array(cw * ch * 4);
    for (var y = 0; y < ch; y++) {
      var src = ((t + y) * w + l) * 4;
      out.set(rgba.subarray(src, src + cw * 4), y * cw * 4);
    }
    return { w: cw, h: ch, rgba: out, offx: l, offy: t };
  }
  function copyPlane(p) {
    return { w: p.w, h: p.h, rgba: new Uint8Array(p.rgba) };
  }
  /* One sprite: the colour plane, then the alpha plane appended after it, then the
   * record the geometry carries. The pivot is re-based into cropped coords, which is
   * what lets the baker crop to ink without moving anything on the glass. */
  function emitSprite(blob, crop, offx, offy, pivotX, pivotY, centerX, centerY, blend, show, diff) {
    var colorOff = blob.add(rgb565Plane(crop.rgba, crop.w, crop.h));
    blob.add(alphaPlane(crop.rgba, crop.w, crop.h));
    var d = diff || [0, 0, 0, 0];
    return { w: crop.w, h: crop.h, offx: offx, offy: offy,
             pivotx: pivotX - offx, pivoty: pivotY - offy,
             centerx: centerX, centery: centerY,
             blend: blend, show: show ? 1 : 0, color_off: colorOff,
             diffx: d[0], diffy: d[1], diffw: d[2], diffh: d[3] };
  }
  function blankSprite(cx, cy, show) {
    var s = blankSpr();
    s.centerx = cx; s.centery = cy; s.show = show ? 1 : 0;
    return s;
  }
  /* A trimmed plane onto a full-panel canvas with its pivot pinned to (cx,cy) —
   * prep_watch_kit.py's _placed. The device pins a layer's stored pivot onto its
   * centre and rotates about that, so placing the art with the pivot already on the
   * centre makes (pivot, centre) a property of the placement and not of the pixels. */
  function placedPlane(plane, px, py, cx, cy) {
    var out = new Uint8Array(SCREEN_W * SCREEN_H * 4);
    var dx = Math.round(cx - px), dy = Math.round(cy - py);
    for (var y = 0; y < plane.h; y++) {
      var ty = y + dy;
      if (ty < 0 || ty >= SCREEN_H) continue;
      for (var x = 0; x < plane.w; x++) {
        var tx = x + dx;
        if (tx < 0 || tx >= SCREEN_W) continue;
        var a = plane.rgba[(y * plane.w + x) * 4 + 3];
        if (!a) continue;                       /* paste replaces: a transparent source texel writes nothing */
        var o = (ty * SCREEN_W + tx) * 4, s = (y * plane.w + x) * 4;
        out[o] = plane.rgba[s]; out[o + 1] = plane.rgba[s + 1];
        out[o + 2] = plane.rgba[s + 2]; out[o + 3] = a;
      }
    }
    return { w: SCREEN_W, h: SCREEN_H, rgba: out };
  }


  /* ---- the compositing ops prep_watch_kit.py performs on the layers ------- */
  /* Image.alpha_composite, texel for texel. Pillow does this in 7-bit-extra precision
   * (AlphaComposite.c): coef1 is the source weight scaled by 2^7, coef2 its complement,
   * and both divisions are the SHIFTFORDIV255 trick rather than a float divide. Ported
   * verbatim because the bezel fold and the sub-dial merge run through it, and a
   * rounding difference here is a payload whose CRC no longer matches the bench bake's. */
  function shiftForDiv255(v) { return ((v >> 8) + v) >> 8; }
  function compositeOver(dst, src) {
    if (dst.w !== src.w || dst.h !== src.h) throw new Error('alpha_composite: size mismatch');
    var n = dst.w * dst.h, d = dst.rgba, s = src.rgba;
    for (var i = 0; i < n; i++) {
      var p = i * 4, sa = s[p + 3];
      if (!sa) continue;                                   /* a==0 copies the destination through */
      var da = d[p + 3], inv = 255 - sa;
      var outa255 = sa * 255 + da * inv;
      var coef1 = Math.floor(sa * 255 * 255 * 128 / outa255);
      var coef2 = 255 * 128 - coef1;
      for (var c = 0; c < 3; c++) {
        var v = shiftForDiv255(s[p + c] * coef1 + d[p + c] * coef2 + (0x80 << 7)) >> 7;
        d[p + c] = v > 255 ? 255 : v;
      }
      var a = shiftForDiv255(outa255 + 0x80);
      d[p + 3] = a > 255 ? 255 : a;
    }
    return dst;
  }
  function blankPlane(w, h) { return { w: w, h: h, rgba: new Uint8Array(w * h * 4) }; }
  function over(dst, src) { return compositeOver(dst, src); }
  /* prep's _zero_under: a live layer's ink where the folded rim is opaque. Those texels
   * can never reach the glass, so dropping them is what makes the fold order-correct
   * instead of double-painted. */
  function zeroUnder(plane, mask) {
    var n = plane.w * plane.h, hit = 0, a = plane.rgba;
    for (var i = 0; i < n; i++) if (mask[i] && a[i * 4 + 3]) { a[i * 4 + 3] = 0; hit++; }
    return hit;
  }
  /* prep's _kill_invisible: alpha the eye cannot weigh, so the device's `if (!a)
   * continue` skips the pixel instead of paying a PSRAM read-modify-write blend. */
  function killInvisible(plane, invisible) {
    var n = plane.w * plane.h, a = plane.rgba, n2 = 0;
    for (var i = 0; i < n; i++) {
      var v = a[i * 4 + 3];
      if (v > 0 && v < invisible) n2++;
      if (v < invisible) a[i * 4 + 3] = 0;
    }
    return n2;
  }
  /* Keep only the fully opaque texels of a layer — bake_clock.py's _opaque_only, which
   * is what the background occlusion cull and the fold both test. */
  function opaqueCopy(plane) {
    var out = copyPlane(plane), a = out.rgba;
    for (var i = 0, n = out.w * out.h; i < n; i++) if (a[i * 4 + 3] !== 255) {
      a[i * 4] = a[i * 4 + 1] = a[i * 4 + 2] = a[i * 4 + 3] = 0;
    }
    return out;
  }
  /* A bool mask over the panel, as the fold and the occlusion cull both need one. */
  function inkMask(plane) {
    var m = new Uint8Array(plane.w * plane.h), a = plane.rgba;
    for (var i = 0, n = m.length; i < n; i++) m[i] = a[i * 4 + 3] > 0 ? 1 : 0;
    return m;
  }
  function maskCount(m) { var c = 0; for (var i = 0; i < m.length; i++) c += m[i]; return c; }
  /* prep's _reachable for a mover: every panel pixel it can ever paint. The radius
   * range is measured from the INK to its pivot, because that is the point the rotation
   * turns, but the annulus is drawn about the layer's CENTRE, because that is where the
   * device pins that pivot on the screen. +/-1 px absorbs the quantised sweep step. */
  function reachableAnnulus(plane, px, py, cx, cy) {
    var m = new Uint8Array(SCREEN_W * SCREEN_H);
    var rmin = Infinity, rmax = -Infinity, a = plane.rgba;
    for (var i = 0, n = plane.w * plane.h; i < n; i++) {
      if (!a[i * 4 + 3]) continue;
      var x = i % plane.w, y = (i / plane.w) | 0;
      var r = Math.hypot(x - px, y - py);
      if (r < rmin) rmin = r;
      if (r > rmax) rmax = r;
    }
    if (rmax < 0) return m;
    for (var yy = 0; yy < SCREEN_H; yy++) {
      for (var xx = 0; xx < SCREEN_W; xx++) {
        var R = Math.hypot(xx - cx, yy - cy);
        m[yy * SCREEN_W + xx] = (R >= rmin - 1.0 && R <= rmax + 1.0) ? 1 : 0;
      }
    }
    return m;
  }
  /* A panel-size RGBA frame as the RGB565 codes the device stores — the array every diff,
   * rect and keyframe in the bank is computed from. Alpha is dropped exactly the way
   * bake_clock.py's _frames_to_u16 drops it, after the frame has been made opaque. */
  function framesToU16(plane) {
    var n = plane.w * plane.h, out = new Uint16Array(n), a = plane.rgba;
    for (var i = 0; i < n; i++) {
      var p = i * 4;
      out[i] = ((a[p] & 0xf8) << 8) | ((a[p + 1] & 0xfc) << 3) | (a[p + 2] >> 3);
    }
    return out;
  }
  /* Frames with any transparency are matted over true black first: the right matte on a
   * panel where a black pixel is an off pixel, and what _load_png_frames does before the
   * packer ever sees a frame. */
  function matteOverBlack(plane) {
    var n = plane.w * plane.h, min = 255, a = plane.rgba, i;
    for (i = 0; i < n; i++) if (a[i * 4 + 3] < min) min = a[i * 4 + 3];
    if (min === 255) return plane;
    var out = copyPlane(plane), o = out.rgba;
    for (i = 0; i < n; i++) {
      var p = i * 4, m = o[p + 3];
      for (var c = 0; c < 3; c++) o[p + c] = (o[p + c] * (256 - m) + m * m + 128) >> 8;
      o[p + 3] = 255;
    }
    return out;
  }

  /* Per-pixel count of changed texels in the inclusive 5x5 box around it, by integral
   * image — what separates real motion (gear teeth arrive in dense clusters) from
   * sensor-style transients (a handful of isolated texels page-wide). */
  function boxDensity(mask, w, h) {
    var pw = w + 4, ph = h + 4, i, j;
    var m2 = new Int32Array(pw * ph);
    for (j = 0; j < h; j++) for (i = 0; i < w; i++) m2[(j + 2) * pw + (i + 2)] = mask[j * w + i] ? 1 : 0;
    var S = pw + 1, ii = new Int32Array(S * (ph + 1));
    for (j = 0; j < ph; j++) {
      var run = 0;
      for (i = 0; i < pw; i++) {
        run += m2[j * pw + i];
        ii[(j + 1) * S + (i + 1)] = ii[j * S + (i + 1)] + run;
      }
    }
    var out = new Int32Array(w * h);
    for (j = 0; j < h; j++)
      for (i = 0; i < w; i++)
        out[j * w + i] = ii[(j + 5) * S + (i + 5)] - ii[j * S + (i + 5)] - ii[(j + 5) * S + i] + ii[j * S + i];
    return out;
  }

  /* bool diff mask -> pixel-space rects: snap to the 16 px tile grid, cover with greedy
   * maximal rectangles (the monotone-histogram stack, largest first, repeated), merge the
   * cheapest pairs down to the cap, and collapse to one full-page rect when a cover would
   * spill. The cap matters: LVGL's invalid-area buffer holds 32 rects before a refresh
   * degrades to a whole-screen invalidate, and the rotating hands claim slots of their own
   * every tick. Mirrors bake_clock.py's _pack_mask. */
  function packMask(mask, tile, maxRects, maxFlush) {
    var W = SCREEN_W, H = SCREEN_H, i, j, k;
    var full = [[0, 0, W, H]];
    var any = false;
    for (i = 0; i < mask.length; i++) if (mask[i]) { any = true; break; }
    if (!any) return [];
    var ty = Math.ceil(H / tile), tx = Math.ceil(W / tile), pw = tx * tile;
    var pad = new Uint8Array(pw * (ty * tile));
    for (j = 0; j < H; j++) pad.set(mask.subarray(j * W, j * W + W), j * pw);
    var tm = new Uint8Array(ty * tx), nActive = 0;
    for (k = 0; k < ty * tx; k++) {
      var tyy = (k / tx) | 0, txx = k % tx, on = 0;
      for (j = 0; j < tile && tyy * tile + j < pad.length / pw; j++) {
        for (i = 0; i < tile; i++) if (pad[(tyy * tile + j) * pw + txx * tile + i]) { on = 1; break; }
        if (on) break;
      }
      tm[k] = on;
      nActive += on;
    }
    if (nActive / (ty * tx) > maxFlush) return full;

    var work = Uint8Array.from(tm), rectsT = [];
    for (;;) {
      var live = false;
      for (i = 0; i < work.length; i++) if (work[i]) { live = true; break; }
      if (!live) break;
      var hgt = new Int32Array(tx + 1), best = null;
      for (var r = 0; r < ty; r++) {
        for (i = 0; i < tx; i++) hgt[i] = work[r * tx + i] ? hgt[i] + 1 : 0;
        hgt[tx] = 0;
        var stack = [];
        for (var c = 0; c <= tx; c++) {
          var cur = hgt[c], start = c;
          while (stack.length && stack[stack.length - 1][1] > cur) {
            var e = stack.pop(), area = e[1] * (c - e[0]);
            if (!best || area > best[0]) best = [area, e[0], r - e[1] + 1, e[1], c - e[0]];
            start = e[0];
          }
          if (cur && (!stack.length || stack[stack.length - 1][1] < cur)) stack.push([start, cur]);
        }
      }
      if (!best || !best[0]) return full;                          /* cover stalled */
      rectsT.push([best[1], best[2], best[4], best[3]]);
      for (j = best[2]; j < best[2] + best[3]; j++) for (i = best[1]; i < best[1] + best[4]; i++) work[j * tx + i] = 0;
    }
    while (rectsT.length > maxRects) {
      var bi = -1, bj = -1, bad = null;
      for (i = 0; i < rectsT.length; i++) {
        for (j = i + 1; j < rectsT.length; j++) {
          var A = rectsT[i], Bp = rectsT[j];
          var x0 = Math.min(A[0], Bp[0]), y0 = Math.min(A[1], Bp[1]);
          var x1 = Math.max(A[0] + A[2], Bp[0] + Bp[2]), y1 = Math.max(A[1] + A[3], Bp[1] + Bp[3]);
          var add = (x1 - x0) * (y1 - y0) - A[2] * A[3] - Bp[2] * Bp[3];
          if (bad === null || add < bad) { bad = add; bi = i; bj = j; }
        }
      }
      if (bad === null || bad > 0.25 * ty * tx) return full;
      var M = rectsT[bi], N2 = rectsT[bj];
      var mx = Math.min(M[0], N2[0]), my = Math.min(M[1], N2[1]);
      var mw = Math.max(M[0] + M[2], N2[0] + N2[2]) - mx;
      var mh = Math.max(M[1] + M[3], N2[1] + N2[3]) - my;
      rectsT = rectsT.filter(function (_, t2) { return t2 !== bi && t2 !== bj; });
      rectsT.push([mx, my, mw, mh]);
    }

    var rects = rectsT.map(function (q) {
      return [q[0] * tile, q[1] * tile, Math.min(q[2] * tile, W - q[0] * tile), Math.min(q[3] * tile, H - q[1] * tile)];
    });
    var covered = 0;
    for (i = 0; i < rects.length; i++) covered += rects[i][2] * rects[i][3];
    if (covered > maxFlush * W * H) return full;
    return rects;
  }



  /* Frames whose whole loop stays within +-eps of one code per channel are replaced by
   * the temporal median of the cycle: they become bit-identical forever, contribute zero
   * delta, and can never be left stale. Pixels that move more than eps pass through. */
  function temporalSnap(F, eps) {
    var n = F.length, N = SCREEN_W * SCREEN_H, i, k;
    var quiet = new Uint8Array(N);
    for (i = 0; i < N; i++) quiet[i] = 1;
    var ch = [[], [], []], sh = [11, 5, 0], msk = [0x1f, 0x3f, 0x1f];
    for (var c = 0; c < 3; c++) {
      for (k = 0; k < n; k++) ch[c].push(new Uint16Array(N));
      for (i = 0; i < N; i++) {
        var mn = 999, mx = 0;
        for (k = 0; k < n; k++) {
          var v = (F[k][i] >> sh[c]) & msk[c];
          ch[c][k][i] = v;
          if (v < mn) mn = v;
          if (v > mx) mx = v;
        }
        if (mx - mn > eps) quiet[i] = 0;
      }
    }
    var out = [];
    for (k = 0; k < n; k++) out.push(new Uint16Array(N));
    var buf = new Array(n), med = new Array(3);
    for (i = 0; i < N; i++) {
      if (!quiet[i]) continue;
      for (c = 0; c < 3; c++) {
        for (k = 0; k < n; k++) buf[k] = ch[c][k][i];
        buf.sort(function (a, b) { return a - b; });
        var mid = n % 2 ? buf[(n - 1) / 2] : (buf[n / 2 - 1] + buf[n / 2]) / 2;
        var r = Math.round(mid);                                   /* np.rint rounds .5 to even */
        if (Math.abs(mid - r) === 0.5 && r % 2) r -= 1;
        med[c] = r & msk[c];
      }
      var code = ((med[0] & 0x1f) << 11) | ((med[1] & 0x3f) << 5) | (med[2] & 0x1f);
      for (k = 0; k < n; k++) out[k][i] = code;
    }
    for (k = 0; k < n; k++) {
      var src = F[k];
      for (i = 0; i < N; i++) if (!quiet[i]) out[k][i] = src[i];
    }
    return { frames: out, quiet: quiet };
  }

  /* One canvas->target transition -> {rects, mask}. A texel counts as changed when its
   * perceptual RGB565 max-channel step reaches the threshold AND the change is part of a
   * cluster (>= K of the 25 texels in its 5x5 box changed too): real motion is dense,
   * dither and sparkle are isolated. Texels this simulation already painted are exempt
   * from the cluster test when DIMMING, which is the canvas undoing its own leftover. */
  function gifTransition(canvas, tgt, painted, diffThresh, K, maxRects, maxFlush, occl) {
    var N = canvas.length, i;
    var raw = new Uint8Array(N), mask = new Uint8Array(N), lum = new Int32Array(N);
    var thrs = [diffThresh, diffThresh * 2, diffThresh * 3, diffThresh * 5];
    for (var t = 0; t < thrs.length; t++) {
      var thr = thrs[t], any = false;
      raw.fill(0);
      for (i = 0; i < N; i++) {
        var cv = canvas[i], tv = tgt[i];
        if (cv === tv) continue;
        var dr = ((tv >> 11) & 31) - ((cv >> 11) & 31);
        var dg = ((tv >> 5) & 63) - ((cv >> 5) & 63);
        var db = (tv & 31) - (cv & 31);
        if (Math.max(Math.abs(dr) * 8, Math.abs(dg) * 4, Math.abs(db) * 8) >= thr) {
          raw[i] = 1;
          lum[i] = dr * 76 + dg * 150 + db * 30;                   /* signed luma step */
          any = true;
        }
      }
      if (!any) return { rects: [], mask: null };
      var dens = boxDensity(raw, SCREEN_W, SCREEN_H);
      for (i = 0; i < N; i++) {
        mask[i] = raw[i] && (dens[i] >= K || (painted && painted[i] && lum[i] < 0)) ? 1 : 0;
        if (occl && occl[i]) mask[i] = 0;
      }
      var lst = packMask(mask, 16, maxRects, maxFlush);
      var collapsed = lst.length === 1 && lst[0][0] === 0 && lst[0][1] === 0 &&
                      lst[0][2] === SCREEN_W && lst[0][3] === SCREEN_H;
      if (collapsed && t !== thrs.length - 1) continue;            /* try a stricter view */
      return { rects: lst, mask: Uint8Array.from(mask) };
    }
    return { rects: [[0, 0, SCREEN_W, SCREEN_H]], mask: Uint8Array.from(raw) };
  }


  /* ---- the whole-file pixel bank: bake_clock.py's Blob, in order --------- */

  /* One rect's worth of the frame's own RGB565 rows, little-endian — the run the device
   * memcpy's into its PSRAM canvas at clock_view.c's rect push. */
  function rectBytes(u16, x, y, w, h) {
    var out = new Uint8Array(w * h * 2), dv = new DataView(out.buffer);
    for (var j = 0; j < h; j++)
      for (var i = 0; i < w; i++) dv.setUint16((j * w + i) * 2, u16[(y + j) * SCREEN_W + x + i], true);
    return out;
  }
  function u16RunBytes(u16, from, to) {
    var out = new Uint8Array((to - from) * 2), dv = new DataView(out.buffer);
    for (var i = from; i < to; i++) dv.setUint16((i - from) * 2, u16[i], true);
    return out;
  }

  /* The delta-rect bank: one keyframe plus per-frame changed rects, packed from the
   * simulated device canvas so the state the device ends up with is exactly reproducible.
   * The simulation is warmed over a few whole cycles and the LAST cycle is the one baked;
   * the device then replays that periodic trajectory forever and no pixel drifts.
   * planes are the B composed base frames, opaque, at panel size. */
  function bakeBgBank(blob, planes, o) {
    var n = planes.length, i, j, k;
    if (n < 2) throw new Error('a delta-rect background needs at least two frames');
    if (n > GIFS_MAX) throw new Error(n + ' background frames exceed the dial\'s cap of ' + GIFS_MAX);
    var imgs = planes.map(matteOverBlack);
    for (i = 0; i < imgs.length; i++)
      for (k = 0; k < (o.matte || []).length; k++) imgs[i] = matteFold(imgs[i], o.matte[k]);
    var F = imgs.map(framesToU16);
    var snapped = { frames: F, quiet: null };
    if ((o.denoise || 0) > 0 && n >= 3) snapped = temporalSnap(F, o.denoise);
    F = snapped.frames;
    var maxRects = o.maxRects || 16, maxFlush = o.maxFlush || 0.62, thresh = o.diffThresh || 12;
    var per = null, start = null, canvas = F[0].slice();
    for (var warm = 0; warm < 3; warm++) {                            /* warm, and bake the last cycle */
      start = canvas.slice();
      var painted = new Uint8Array(canvas.length);
      per = {};
      for (k = 0; k < n; k++) {
        var tgtI = (k + 1) % n, tgt = F[tgtI];
        var tr = gifTransition(canvas, tgt, painted, thresh, 12, maxRects, maxFlush, o.occl);
        var blitted = [];
        for (i = 0; i < tr.rects.length; i++) {
          var q = tr.rects[i], x = q[0], y = q[1], w = q[2], h = q[3];
          for (j = 0; j < h; j++) {
            var row = (y + j) * SCREEN_W + x;
            canvas.set(tgt.subarray(row, row + w), row);
            for (var ii = 0; ii < w; ii++) painted[row + ii] = 1;
          }
          blitted.push(q);
        }
        per[tgtI] = blitted;
      }
    }
    var keyOff = blob.add(u16RunBytes(start, 0, SCREEN_W * SCREEN_H));
    var rects = [], first = [0], packedTexels = 0;
    for (j = 0; j < n; j++) {
      var list = per[j] || [];
      for (i = 0; i < list.length; i++) {
        var r = list[i];
        var off = blob.add(rectBytes(F[j], r[0], r[1], r[2], r[3]));
        rects.push(r[0], r[1], r[2], r[3], off);
        packedTexels += r[2] * r[3];
      }
      first.push(rects.length / 5 | 0);
    }
    if (rects.length / 5 > RECTS_MAX) throw new Error((rects.length / 5 | 0) + ' delta rects exceed the dial\'s cap of ' + RECTS_MAX);
    var ms = Math.max(1, Math.round(1000 / (o.fps || 8)));
    return { frames: n, keyOff: keyOff, rects: rects, first: first,
             delays: new Array(n).fill(ms), packedTexels: packedTexels, plane: F };
  }
  /* matte_fold: fold one layer into a base with the DEVICE's blend, so the baked pixel IS
   * the one the runtime composite used to produce. blend_px()'s NORMAL path is
   * mix_16_16(fg, dst, a) on the panel's own 5/6/5-bit channels. */
  function q565(r, g, b) { return ((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3); }
  /* + 16 before >> 5: the device rounds each field since D43 §5.4 (orb_gfx.c's mix_16_16). */
  function mix565(dst, fg, a) {
    var mix = (a + 4) >> 3;
    var dr = (dst >> 11) & 31, dg = (dst >> 5) & 63, db = dst & 31;
    var fr = (fg >> 11) & 31, fg6 = (fg >> 5) & 63, fb = fg & 31;
    var r = ((((fr - dr) * mix + 16) >> 5) + dr) & 31;
    var g = ((((fg6 - dg) * mix + 16) >> 5) + dg) & 63;
    var b = ((((fb - db) * mix + 16) >> 5) + db) & 31;
    return (r << 11) | (g << 5) | b;
  }
  function unq565(v) {
    var r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
    return [(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)];
  }
  function matteFold(base, src) {
    var n = base.w * base.h, out = copyPlane(base), o = out.rgba, s = src.rgba;
    for (var i = 0; i < n; i++) {
      var a = s[i * 4 + 3];
      if (!a) continue;                            /* alpha 0 passes the base through untouched */
      var dst = q565(o[i * 4], o[i * 4 + 1], o[i * 4 + 2]);
      var fg = q565(s[i * 4], s[i * 4 + 1], s[i * 4 + 2]);
      var rgb = unq565(mix565(dst, fg, a));
      o[i * 4] = rgb[0]; o[i * 4 + 1] = rgb[1]; o[i * 4 + 2] = rgb[2]; o[i * 4 + 3] = 255;
    }
    return out;
  }

  function cropRect(plane, x0, y0, x1, y1) {
    var w = x1 - x0, h = y1 - y0, out = new Uint8Array(w * h * 4);
    for (var y = 0; y < h; y++) {
      var s = ((y0 + y) * plane.w + x0) * 4;
      out.set(plane.rgba.subarray(s, s + w * 4), y * w * 4);
    }
    return { w: w, h: h, rgba: out };
  }

  /* THE carriage row's rect — the one place that decides it, and the ONLY one. Both the
   * bake (bakeTourbillon crops to it) and the price (the composer's measure bills it) read
   * this function, because the two disagreeing was what blew the slot: a measure that
   * thresholded ink at alpha >= 8 while the crop runs at alpha > 0, and that added no pad
   * while the crop pads by 2 and clamps to the screen, billed a tight box and shipped a
   * panel-wide one. Under-pricing is the direction this file refuses to be wrong in.
   * Returns the padded, screen-clamped [l, t, r, b] over the cycle-wide union of ink, or
   * null when no plane carries ink at all. */
  function carriageRect3(planes, o) {
    var i, n = planes.length, pad = (o && o.pad !== undefined) ? o.pad : 2;
    var piv = o && o.pivot, l, t, r, b, pw = 0, ph = 0;
    l = 1 << 30; t = 1 << 30; r = -1; b = -1;
    for (i = 0; i < n; i++) {
      var bb = alphaBbox(planes[i].rgba, planes[i].w, planes[i].h);
      if (planes[i].w > pw) pw = planes[i].w;
      if (planes[i].h > ph) ph = planes[i].h;
      if (!bb) continue;
      l = Math.min(l, bb[0]); t = Math.min(t, bb[1]);
      r = Math.max(r, bb[2]); b = Math.max(b, bb[3]);
    }
    if (r <= l || b <= t) return null;
    // Clamped to the planes' own extent, not just the screen: a row whose art fills its
    // canvas exactly (a 64x64 carriage sprite) must not be padded into a 66x66 crop that
    // reads past the plane it was measured on.
    var mw = Math.min(SCREEN_W, pw), mh = Math.min(SCREEN_H, ph);
    l = Math.max(0, l - pad); t = Math.max(0, t - pad);
    r = Math.min(mw, r + pad); b = Math.min(mh, b + pad);
    // A caller that names a pivot gets a rect holding it. Slot 5 turns about that point, so
    // a row cropped away from its own centre of rotation is an art loss the dial would show
    // as a carriage swinging off its pin. A closed orbit always contains its centre, so
    // this costs nothing on the dials that have one; bakeTourbillon passes no pivot, and
    // the FORMAT 2 crop stays byte-identical against bake_clock.py.
    if (piv) {
      l = Math.min(l, Math.max(0, Math.min(mw, piv[0] | 0)));
      t = Math.min(t, Math.max(0, Math.min(mh, piv[1] | 0)));
      r = Math.max(r, Math.max(0, Math.min(mw, (piv[0] | 0) + 1)));
      b = Math.max(b, Math.max(0, Math.min(mh, (piv[1] | 0) + 1)));
    }
    return [l, t, r, b];
  }

  /* The carriage row's START ORIENTATION, in the hundredths-of-a-degree the wire
   * carries — asked of the engine's cage offset rather than estimated at either end.
   * The sign is the whole bug this function exists to kill. The preview draws the
   * carriage with rotate(seconds*TAU - cageOff) (watch-engine.js), and the layer kit
   * publishes zeroRotationDeg = -cageOff for the same reason; but slot 5 ADDS this
   * figure to the seconds angle (clock_view.c's c_a = a + s_car_start_10). So the
   * offset the author sees subtracted must reach the wire negated, or the carriage
   * sits 2x off its own take-up angle. Folded into 0..35999 because the C side takes
   * it through a % that keeps a negative operand negative, which would land the
   * carriage the wrong way round the dial rather than refusing the bake. */
  function carriageStart3(cageOffRad) {
    var h = Math.round((+cageOffRad || 0) * 18000 / Math.PI);
    return ((-h % 36000) + 36000) % 36000;
  }

  /* The tourbillon / carriage bank: every frame cropped to the ONE ink bbox of the whole
   * cycle, so all frames rotate about the same pivot and dirty the same box, each storing
   * the rect that differs from its predecessor — chained and cyclic, like the bank's. */
  function bakeTourbillon(blob, planes, o) {
    var n = planes.length, i, j, c;
    if (n > TOURB_MAX) throw new Error(n + ' carriage frames exceed the baked max of ' + TOURB_MAX);
    var rect = carriageRect3(planes, o);
    if (!rect) throw new Error('the carriage frames are fully transparent');
    var l = rect[0], t = rect[1], r = rect[2], b = rect[3];
    var thr = Math.max(1, o.diffThresh || 12) * 3, recs = [];
    var px = o.pivot[0], py = o.pivot[1], cx = o.centre[0], cy = o.centre[1];
    for (i = 0; i < n; i++) {
      var cur = planes[i], prev = planes[(i - 1 + n) % n];
      var dl = 1 << 30, dt = 1 << 30, dr = -1, db = -1, ww = r - l, hh = b - t;
      for (j = 0; j < ww * hh; j++) {
        var x = l + (j % ww), y = t + (j / ww | 0), sum = 0;
        var p = (y * cur.w + x) * 4;
        for (c = 0; c < 4; c++) sum += Math.abs(cur.rgba[p + c] - prev.rgba[p + c]);
        if (sum > thr) { if (x < dl) dl = x; if (y < dt) dt = y; if (x > dr) dr = x; if (y > db) db = y; }
      }
      var diff = [0, 0, 0, 0];
      if (dr >= 0) {
        var ax = Math.max(0, dl - 3 - l), ay = Math.max(0, dt - 3 - t);
        diff = [ax, ay, Math.min(ww, dr + 4 - l) - ax, Math.min(hh, db + 4 - t) - ay];
      }
      recs.push(emitSprite(blob, cropRect(cur, l, t, r, b), l, t, px, py, cx, cy, 0, true, diff));
    }
    return recs;
  }

  /* A window tile: RGB565 then its planar alpha, packed raw. The reader strides frames by
  /* ---- prep_watch_kit.py's decisions, on the composer's own canvases ------ */

  var RIM_OPAQUE = 250;      // outer pixels at/above this alpha are the bezel
  var INVISIBLE = 6;         // alpha the eye cannot weigh; blend traffic 0

  /* Which bezel texels may fold into every bank frame, and which must stay live. A fold
   * is only legal where no MOVER can ever paint: the commit hook restores a dirty row
   * from the canvas and repaints every live layer over it, so a layer folded beneath a
   * mover gets painted over by that mover. fold and liveRim are disjoint by construction,
   * so no pixel is ever blended twice. */
  function bezelFold(outer, movers) {
    var n = SCREEN_W * SCREEN_H, i, j;
    var out = { fold: new Uint8Array(n), liveRim: new Uint8Array(n), rim: new Uint8Array(n), rimPx: 0 };
    if (!outer) return out;
    var a = outer.rgba;
    for (i = 0; i < n; i++) { out.rim[i] = a[i * 4 + 3] >= RIM_OPAQUE ? 1 : 0; out.rimPx += out.rim[i]; }
    var swept = new Uint8Array(n);
    for (i = 0; i < movers.length; i++) {
      var m = movers[i];
      if (!m.plane) continue;
      var reach = reachableAnnulus(m.plane, m.pivot[0], m.pivot[1], m.centre[0], m.centre[1]);
      for (j = 0; j < n; j++) swept[j] |= reach[j];
    }
    for (i = 0; i < n; i++) {
      out.fold[i] = out.rim[i] && !swept[i] ? 1 : 0;
      out.liveRim[i] = out.rim[i] && swept[i] ? 1 : 0;
    }
    return out;
  }
  /* A copy of a plane keeping only the texels a mask names — prep's fold_img / lr_arr.
   * The fold is a pixel COPY, not a blend: it is opaque by construction, which is why the
   * bezel is the one layer worth folding and the sub-opaque crystal tint is not. */
  function maskedPlane(plane, mask) {
    var out = copyPlane(plane), o = out.rgba;
    for (var i = 0, n = plane.w * plane.h; i < n; i++) if (!mask[i]) { o[i * 4] = o[i * 4 + 1] = o[i * 4 + 2] = o[i * 4 + 3] = 0; }
    return out;
  }
  /* The sub-opaque half of the case layer: everything the fold did not take. */
  function tintOf(outer, rimMask) {
    var out = copyPlane(outer), o = out.rgba;
    for (var i = 0, n = outer.w * outer.h; i < n; i++) if (rimMask[i]) o[i * 4 + 3] = 0;
    return out;
  }

  /* ---- kit word -> wire value --------------------------------------------- */
  var MANIFEST_SOURCE = {
    day: 'weekday', weekday: 'weekday', month: 'month', date: 'date', dayofmonth: 'date',
    moon: 'moon_age', moonphase: 'moon_age', hour: 'hour12', hour24: 'hour24',
    minute: 'minute', second: 'second', battery: 'battery', powerreserve: 'power_reserve',
    balance: 'power_reserve', text: 'text', custom: 'text'
  };
  function normWord(w) { return String(w == null ? '' : w).toLowerCase().replace(/[\s_-]/g, ''); }
  function sourceOf(word) {
    var k = normWord(word);
    if (Object.prototype.hasOwnProperty.call(MANIFEST_SOURCE, k)) return SOURCES.indexOf(MANIFEST_SOURCE[k]);
    throw new Error('complication source ' + JSON.stringify(word) + ' is not one the dial knows how to drive');
  }
  var WEEKDAY_TILES = ['MON', 'TUE', 'WED', 'THU', 'FRI', 'SAT', 'SUN'];   // value 0..6, Mon first
  var MONTH_TILES = ['JAN', 'FEB', 'MAR', 'APR', 'MAY', 'JUN', 'JUL', 'AUG', 'SEP', 'OCT', 'NOV', 'DEC'];
  var ROUND_MOON_STYLES = ['sphere', 'graphic', 'neon'];                   // drawn as a whole circle
  function blankRecord(kind) {
    var f = fieldsOf(kind), r = {};
    for (var i = 0; i < f.length; i++) r[f[i][0]] = 0;
    return r;
  }
  function windowBytes(w, h) { return w * h * 3; }        /* colour plane then planar alpha */

  /* The tiles, in the order the driving value indexes them. Monday-first weekdays and
   * January-first months are the bake's own lists: the kit's listing order is not
   * trusted, because a theme and a dial that disagree about which frame "Saturday" is
   * produce a clock one day wrong that reads as an art bug. */
  function tileOrder(srcWord, tiles) {
    var names = srcWord === 'weekday' ? WEEKDAY_TILES : srcWord === 'month' ? MONTH_TILES : null;
    if (names) {
      var by = {};
      tiles.forEach(function (t) { by[String(t.value).toUpperCase()] = t; });
      return names.map(function (nm) {
        if (!by[nm]) throw new Error('a ' + srcWord + ' window needs tile ' + nm + ', which the design does not draw');
        return by[nm];
      });
    }
    return tiles.slice().sort(function (a, b) {
      return String(a.value) < String(b.value) ? -1 : String(a.value) > String(b.value) ? 1 : 0;
    });
  }

  /* One complication -> one window record plus its tiles. Mirrors bake_clock.py's
   * bake_complications: the centre comes from the kit's own angle/dist measured against
   * its host, a fan moon is anchored on the tile's BOTTOM edge (its flat baseline is what
   * sits on the sub-dial's arch line, so centring it drops the disc h/2 low), and `first`
   * carries the z bucket the kit's drawOrder decided — the only thing that decides whether
   * a tile is visible at all, since a tile baked under the plate it belongs to is bare art
   * the plate paints over. */
  function bakeComplications(blob, layout, tilesOf) {
    var comps = (layout && layout.comps) || [], centre = {}, recs = [];
    var dialC = [SCREEN_W / 2, SCREEN_H / 2];
    for (var i = 0; i < comps.length; i++) {
      var comp = comps[i], cid = comp.id || '';
      var ctype = String(comp.type || '').toLowerCase();
      var host = comp.host || 'dial';
      var base = centre[host] || dialC;
      var ang = +comp.angle || 0, dist = +comp.dist || 0;
      var px = base[0] + dist * Math.sin(ang * Math.PI / 180);
      var py = base[1] - dist * Math.cos(ang * Math.PI / 180);
      centre[cid] = [px, py];
      if (ctype === 'opening' || ctype === 'subdial') continue;   /* the aperture / the host plate */
      var tiles = tilesOf(cid, ctype);
      if (!tiles || !tiles.length) continue;                      /* this comp ships no tiles */
      var word = comp.shows || comp.scale || comp.show || ctype;
      var srcIdx = sourceOf(word), srcWord = SOURCES[srcIdx];
      var order = tileOrder(srcWord, tiles);
      // The box is the tile's own plane, which is bake_clock.py's im.size: a tile carries
      // only {value, plane}, so reading tile.w here packed NaN to 0 and shipped a window
      // the dials strides by zero bytes, past every CRC check, into noise.
      var w = order[0].plane.w, h = order[0].plane.h, off0 = 0, k;
      if (!(w > 0 && h > 0))
        throw new Error('comp ' + cid + ': tile ' + order[0].value + ' is ' + w + 'x' + h +
          ' — an empty tile cannot be a window');
      for (k = 1; k < order.length; k++)
        if (order[k].plane.w !== w || order[k].plane.h !== h)
          throw new Error('comp ' + cid + ': tile ' + order[k].value + ' is ' + order[k].plane.w + 'x' + order[k].plane.h +
            ' but the box is ' + w + 'x' + h + ' — a window strides by one fixed box');
      for (k = 0; k < order.length; k++) {
        var off = packWindowTile(blob, order[k].plane);
        if (!k) off0 = off;
      }
      var fan = ctype === 'moon' && (+comp.rot || 0) === 0 &&
                ROUND_MOON_STYLES.indexOf(String(comp.style || '').toLowerCase()) < 0;
      var bucket = +comp.underLayer || 0;
      if (bucket < 0 || bucket > 4) throw new Error('comp ' + cid + ': underLayer ' + bucket + ' is not a KINDS slot (0..4)');
      var rec = blankRecord('comp');
      rec.tag = /^c\d+$/.test(cid) ? parseInt(cid.slice(1), 10) : 0;
      rec.kind = COMP_KINDS.indexOf('window');
      rec.src = srcIdx;
      rec.x = Math.round(px - w / 2);
      rec.y = Math.round(py - (fan ? w / 2 : h / 2));
      rec.w = w; rec.h = h; rec.count = order.length; rec.first = bucket; rec.off = off0;
      rec.frame_bytes = windowBytes(w, h);
      rec.value_min = srcWord === 'date' ? 1 : 0;
      rec.scale = 16;
      rec.blend = 0; rec.show = 1; rec.flags = 0; rec.reserve = 0;
      recs.push(rec);
    }
    if (recs.length > CLK_COMP_MAX) throw new Error(recs.length + ' complications exceed the dial\'s cap of ' + CLK_COMP_MAX);
    return recs;
  }

  /* The sub-dial pointers: the same 15 sprite words a hand gets, widened by the nine
   * driving words the dial reads. A shadowed pointer is TWO rows, the twin flagged
   * FLAG_AUX_SHADOW so the driver rotates it by the hand's own angle. */
  function bakeAux(blob, aux, o) {
    var rows = [];
    for (var i = 0; i < (aux || []).length; i++) {
      var a = aux[i];
      var srcIdx = SOURCES.indexOf(a.src) >= 0 ? SOURCES.indexOf(a.src) : sourceOf(a.src);
      var px = Math.trunc(a.pivot[0]), py = Math.trunc(a.pivot[1]);
      var cx = Math.trunc(a.centre[0]), cy = Math.trunc(a.centre[1]);
      var show = a.show === undefined ? true : !!a.show;
      var flags = a.jump ? FLAG_JUMP : 0;
      var crop = cropLayer(a.plane.rgba, a.plane.w, a.plane.h, o.pad);
      if (!crop) throw new Error('aux ' + i + ' (' + a.src + '): the pointer draws nothing');
      var rec = emitSprite(blob, crop, crop.offx, crop.offy, px, py, cx, cy, a.blend || 0, show);
      rec = auxRecord(rec, a, srcIdx, flags);
      rows.push(rec);
      if (a.shadowPlane) {
        var sdx = Math.trunc(a.shadowDx || 0), sdy = Math.trunc(a.shadowDy || 0);
        var sc = cropLayer(a.shadowPlane.rgba, a.shadowPlane.w, a.shadowPlane.h, o.pad);
        if (!sc) throw new Error('aux ' + i + ': its shadow draws nothing');
        var sr = emitSprite(blob, sc, sc.offx, sc.offy, px, py, cx + sdx, cy + sdy, 0, show);
        rows.push(auxRecord(sr, a, srcIdx, flags | FLAG_AUX_SHADOW));
      }
    }
    if (rows.length > CLK_AUX_MAX) throw new Error(rows.length + ' aux records exceed the dial\'s cap of ' + CLK_AUX_MAX);
    return rows;
  }
  function auxRecord(spr, a, srcIdx, flags) {
    var rec = blankRecord('aux');
    for (var f in spr) if (Object.prototype.hasOwnProperty.call(spr, f)) rec[f] = spr[f];
    rec.tag = Math.trunc(a.tag || 0);
    rec.src = srcIdx;
    rec.kind = Math.trunc(a.kind || 0);
    rec.host = Math.trunc(a.host || 0);
    rec.span = Math.round(+(a.span === undefined ? 3600 : a.span));
    rec.zero = Math.round(+(a.zero || 0));
    rec.value_min = Math.trunc(a.valueMin || 0);
    rec.value_max = Math.trunc(a.valueMax || 0);
    rec.flags = flags;
    if (rec.value_max <= 0) throw new Error('aux ' + SOURCES[srcIdx] + ': value_max ' + rec.value_max + ' cannot divide a sweep');
    return rec;
  }

  /* ---- the driver --------------------------------------------------------- */

  /* A natural sort that reads digits as numbers, so frame_2 precedes frame_10 — the same
   * ordering _png_frame_items gives the bench bake. */
  function natKey(name) {
    return String(name).replace(/\d+/g, function (m) { return '0' + ('000000000' + m).slice(-9); });
  }
  function namesUnder(planes, prefix) {
    return Object.keys(planes).filter(function (n) { return n.indexOf(prefix) === 0; })
      .sort(function (a, b) { return natKey(a) < natKey(b) ? -1 : natKey(a) > natKey(b) ? 1 : 0; });
  }
  function planeAt(planes, name, what) {
    var p = planes[name];
    if (!p) throw new Error('the kit has no ' + name + ' — nothing to bake for ' + what);
    return p;
  }
  /* LVGL's trig table, in 1024 units, exactly as lv_draw_sw_transform.c derives it: using
   * the true table rather than Math.sin is what makes the pivot that minimises the
   * OBSERVED wobble the same one the bench bake picks (the two are 2 px apart on a real
   * hand, so this is not an approximation either side can afford). */
  function trigo(deg) {
    var d = ((deg % 360) + 360) % 360, v;
    if (d <= 90) v = Math.floor(16384 * Math.sin(d * Math.PI / 180));
    else if (d <= 180) v = Math.floor(16384 * Math.cos((d - 90) * Math.PI / 180));
    else if (d <= 270) v = Math.floor(-16384 * Math.sin((d - 180) * Math.PI / 180));
    else v = Math.floor(-16384 * Math.cos((d - 270) * Math.PI / 180));
    return v;
  }
  function lvglTrig(ang10) {
    var a = ((ang10 % 3600) + 3600) % 3600, lo = Math.floor(a / 10), rem = a - lo * 10;
    if (a % 3600 === 0) return [0, 1024];
    var s1 = trigo(lo), s2 = trigo(lo + 1), c1 = trigo(lo + 90), c2 = trigo(lo + 91);
    var sinma = Math.floor((s1 * (10 - rem) + s2 * rem) / 10);
    var cosma = Math.floor((c1 * (10 - rem) + c2 * rem) / 10);
    return [sinma >> 4, cosma >> 4];
  }
  /* Worst distance the hub's ink centroid drifts from the layer's screen pin over a full
   * revolution, as the rotator renders it. */
  function hubOrbit(alpha, w, h, px, py, hubR) {
    var worst = 0;
    for (var k = 0; k < 60; k++) {                                  /* the 6 deg sweep beat */
      var tg = lvglTrig(k * 60), c = tg[1] / 1024, s = tg[0] / 1024;
      var t = 0, sx = 0, sy = 0, any = false;
      for (var y = 0; y < h; y++) {
        for (var x = 0; x < w; x++) {
          var a = alpha[y * w + x];
          if (!(a > 128)) continue;
          var dx = x - px, dy = -(y - py);
          var X = c * dx - s * dy, Y = s * dx + c * dy, d = Math.hypot(X, Y);
          if (d >= hubR) continue;
          t += a; sx += a * X; sy += a * Y; any = true;
        }
      }
      if (!any || t < 40) continue;
      worst = Math.max(worst, Math.hypot(sx / t, sy / t));
    }
    return worst;
  }
  /* bake_clock.py's snap_pivot_to_hub: adopt a corrected pivot only when it removes a real
   * offset, so a correctly-authored hand keeps the value its author typed. */
  function snapPivotToHub(plane, px, py, winY, winX, minGain) {
    var w = plane.w, h = plane.h, a = new Uint8Array(w * h);
    for (var i = 0, n = w * h; i < n; i++) a[i] = plane.rgba[i * 4 + 3];
    var base = hubOrbit(a, w, h, px, py, 15);
    if (base < minGain) return { x: px, y: py, orbit: base };
    var best = { v: base, x: px, y: py };
    for (var ny = py - winY; ny <= py + winY; ny++)
      for (var nx = px - winX; nx <= px + winX; nx++) {
        if (nx === px && ny === py) continue;
        var o = hubOrbit(a, w, h, nx, ny, 15);
        if (o < best.v) best = { v: o, x: nx, y: ny };
      }
    if (base - best.v < minGain) return { x: px, y: py, orbit: base };
    return { x: best.x, y: best.y, orbit: best.v };
  }

  /* prep_watch_kit.py's _static_run: the consecutive static layers from the kit's own
   * drawOrder. Merging a sub-dial into the dial is exact only while NOTHING composites
   * between them, so an empty complications block is skipped and a real one breaks the
   * merge — but the run is COLLECTED, not truncated, so sub-dials listed after it still
   * ship together as static2. */
  function staticRun(order) {
    var dial = null, run = [];
    for (var i = 0; i < order.length; i++) {
      var nm = order[i].layer;
      if (dial === null) { if (nm === 'dial') dial = order[i].file; continue; }
      if (nm === 'subdial') run.push([order[i].file, true]);
      else if (nm === 'complications' && !(order[i].items || []).length) run.push([null, true]);
      else if (nm === 'complications') run.push([null, false]);
      else break;
    }
    if (dial === null) return { dial: null, subs: [], merged: false };
    var subs = run.filter(function (q) { return q[0]; }).map(function (q) { return q[0]; });
    var merged = run.every(function (q) { return q[1]; }) && subs.length > 0;
    return { dial: dial, subs: subs, merged: merged };
  }

  /* prep's _under_layers: comp id -> the KINDS slot its plate must stay UNDER. The
   * drawOrder says where the kit put the aperture; that is the only thing that decides
   * whether a tile is visible at all, and the bake cannot make the call itself. */
  function underLayers(order, layout, merged) {
    var comps = (layout && layout.comps) || [], hostOf = {}, subIds = {};
    comps.forEach(function (c) { hostOf[c.id] = c.host || 'dial'; });
    if (!merged) order.forEach(function (d) { if (d.layer === 'subdial') subIds[d.id] = 1; });
    var out = {};
    comps.forEach(function (c) {
      var host = c.host || 'dial', depth = 0;
      while (hostOf[host] !== undefined && depth < 8) {
        if (subIds[host]) break;
        host = hostOf[host] || 'dial'; depth++;
      }
      out[c.id] = subIds[host] ? 4 : 0;
    });
    return out;
  }

  /* The vendor's pointer table, exactly as prep_watch_kit.py reads it: a sub-dial pointer
   * turns once over its own value domain, and the ring it reads pins value_min/max. */
  var AUX_KINDS = {
    date: ['date', 1, 31, true], day: ['weekday', 0, 7, true], month: ['month', 0, 12, true],
    hour: ['hour12', 0, 12, false], minute: ['minute', 0, 60, false], second: ['second', 0, 60, false]
  };
  function inkCount(plane) {
    var n = 0, a = plane.rgba;
    for (var i = 3, m = plane.w * plane.h * 4; i < m; i += 4) if (a[i]) n++;
    return n;
  }
  /* A centre the kit names, or the dial's own. */
  function centreOf(v) { return (v || [SCREEN_W / 2, SCREEN_H / 2]).map(Math.round); }

  /* One kit in, one .orbtheme out. The kit is exactly what the layer zip ships: the
   * manifest.json object, and every PNG it names decoded to straight RGBA. That is the
   * contract with tools/prep_watch_kit.py + tools/bake_clock.py, so the browser bake and
   * the bench bake start from identical bytes and their containers can be diffed. */
  function bakeKit(kit, opts) {
    opts = opts || {};
    var M = kit.manifest, planes = kit.planes;
    var pad = opts.pad === undefined ? 2 : opts.pad;
    var diffThresh = opts.diffThresh === undefined ? 12 : opts.diffThresh;
    var denoise = opts.denoise === undefined ? 2 : opts.denoise;
    var maxRects = opts.bgMaxRects || 16, maxFlush = opts.bgMaxFlush || 0.62;
    var fps = +M.fps || 8, B = +M.frames || 0;
    var report = { notes: [], live: [], counts: {} };
    if (M.size !== SCREEN_W) throw new Error('kit size ' + M.size + ' != ' + SCREEN_W);
    if (!B) throw new Error('the kit declares no frames: nothing to bake as the background bank');

    // ---- split the kit's hands into the five baked slots and the aux table
    var centre = {}, auxSpec = [];
    (M.hands || []).forEach(function (h) {
      var onCentre = String(h.mount || 'centre').indexOf('centre') === 0;
      // the first hour, minute and seconds hand on the dial's arbour take the slots; a dial may
      // carry more (D43 P5c), and those join the pointer table like any other hand
      if ((h.kind === 'hour' || h.kind === 'minute' || h.kind === 'second') && onCentre && !centre[h.kind]) centre[h.kind] = h;
      else if (h.kind === undefined || h.kind === null) return;               // a fixture entry, not a hand
      else if (!AUX_KINDS[h.kind]) throw new Error('hand kind ' + JSON.stringify(h.kind) +
        ' is neither a dial hand nor a drivable pointer (' + Object.keys(AUX_KINDS).join(', ') +
        '): the firmware has no driver for it, so it would ship invisible');
      else auxSpec.push(h);
    });
    function sprite(h) {
      var p = planeAt(planes, h.file, h.file);
      var pv = h.pivot || [SCREEN_W / 2, SCREEN_H / 2];
      var cn = centreOf(h.centre);
      // prep's _placed pins the art's own pivot onto the layer's centre, so the pair
      // (pivot, centre) becomes a property of the placement and not of the pixels.
      return { plane: placedPlane(p, pv[0], pv[1], cn[0], cn[1]), centre: cn };
    }
    var hand = {}, shadow = {}, shadowOn = false;
    ['hour', 'minute', 'second'].forEach(function (k) {
      if (!centre[k]) return;
      hand[k] = sprite(centre[k]);
      var sh = centre[k].shadow;
      if (!sh || !sh.file || !planes[sh.file]) return;
      var hx = (centre[k].pivot || [0, 0])[0], hy = (centre[k].pivot || [0, 0])[1];
      if (Math.abs(sh.pivot[0] - hx - Math.round(sh.pivot[0] - hx)) > 0.02 ||
          Math.abs(sh.pivot[1] - hy - Math.round(sh.pivot[1] - hy)) > 0.02)
        throw new Error(k + ': the shadow pivot offset (' + (sh.pivot[0] - hx) + ',' + (sh.pivot[1] - hy) +
          ') is not integral — the shadow does not share the hand\'s rotation frame');
      shadow[k] = placedPlane(planeAt(planes, sh.file, sh.file), sh.pivot[0], sh.pivot[1],
                              hand[k].centre[0], hand[k].centre[1]);
      hand[k].shadowDx = Math.round(sh.offsetPx[0]);
      hand[k].shadowDy = Math.round(sh.offsetPx[1]);
      shadowOn = true;
    });

    // ---- the carriage, and the bezel fold its reach refuses
    var order = M.drawOrder || [];
    var tbNames = namesUnder(planes, 'tourbillon/'), tb = [];
    for (var i = 0; i < tbNames.length; i++) if (/\.png$/i.test(tbNames[i])) tb.push(planes[tbNames[i]]);
    if (tb.length > TOURB_MAX) throw new Error(tb.length + ' carriage frames exceed the baker\'s max of ' + TOURB_MAX);
    var handC = null, tbC = null;
    (M.hands || []).forEach(function (h) { if (!handC && h.centre) handC = h.centre; });
    order.forEach(function (d) { if (d.layer === 'tourbillon' && d.centre) tbC = d.centre; });
    tbC = centreOf(tbC || handC);
    var movers = [];
    if (tb.length) movers.push({ plane: tb[0], pivot: tbC, centre: tbC });
    ['hour', 'minute', 'second'].forEach(function (k) {
      if (!hand[k] || !inkCount(hand[k].plane)) return;
      movers.push({ plane: hand[k].plane, pivot: hand[k].centre, centre: hand[k].centre });
      if (shadow[k]) movers.push({ plane: shadow[k], pivot: hand[k].centre,
                                   centre: [hand[k].centre[0] + (hand[k].shadowDx || 0),
                                            hand[k].centre[1] + (hand[k].shadowDy || 0)] });
    });
    var auxPlanes = [];
    auxSpec.forEach(function (h) {
      var sp = planeAt(planes, h.file, h.file), pv = h.pivot || [SCREEN_W / 2, SCREEN_H / 2];
      var cn = centreOf(h.centre);
      var a = { plane: placedPlane(sp, pv[0], pv[1], cn[0], cn[1]), pivot: cn, centre: cn,
                src: AUX_KINDS[h.kind][0], valueMin: AUX_KINDS[h.kind][1], valueMax: AUX_KINDS[h.kind][2],
                jump: AUX_KINDS[h.kind][3], kind: 0, blend: 0,
                tag: /^c\d+$/.test(String(h.mount)) ? parseInt(String(h.mount).slice(1), 10) : 0,
                zero: Math.round((+h.zeroRotationDeg || 0) * 10), span: 3600 };
      a.host = a.tag;
      var sh = h.shadow;
      if (sh && sh.file && planes[sh.file]) {
        var hx = (pv || [0, 0])[0], hy = (pv || [0, 0])[1], mx = sh.pivot[0] - hx, my = sh.pivot[1] - hy;
        if (Math.abs(mx - Math.round(mx)) > 0.02 || Math.abs(my - Math.round(my)) > 0.02)
          throw new Error('aux ' + h.kind + ': the shadow pivot offset (' + mx + ',' + my + ') is not integral');
        a.shadowPlane = placedPlane(planes[sh.file], sh.pivot[0], sh.pivot[1], cn[0], cn[1]);
        a.shadowDx = Math.round(sh.offsetPx[0]); a.shadowDy = Math.round(sh.offsetPx[1]);
      }
      if (inkCount(a.plane)) movers.push({ plane: a.plane, pivot: cn, centre: cn });
      if (a.shadowPlane && inkCount(a.shadowPlane))
        movers.push({ plane: a.shadowPlane, pivot: cn, centre: [cn[0] + a.shadowDx, cn[1] + a.shadowDy] });
      auxPlanes.push(a);
    });

    var outer = planes['layers/outer.png'] || null;
    var fold = bezelFold(outer, movers);
    report.counts.rimPx = fold.rimPx;
    report.counts.foldedPx = maskCount(fold.fold);
    report.counts.liveRimPx = maskCount(fold.liveRim);
    if (!fold.rimPx) report.notes.push(outer && inkCount(outer)
      ? 'the case draws no pixel at a>=' + RIM_OPAQUE + ': nothing folds, so the whole case blends live every tick'
      : 'the case layer is empty: this design drew no case, so nothing folds and nothing blends for it');

    // ---- the two statics and the overlay, exactly where prep puts them
    var run = staticRun(order);
    if (!run.dial) throw new Error('the kit\'s drawOrder has no dial layer');
    var face = copyPlane(planeAt(planes, run.dial, run.dial));
    report.counts.dialUnderFold = zeroUnder(face, fold.fold);
    report.counts.dialInvisible = killInvisible(face, INVISIBLE);
    var mergedSubs = [];
    if (run.merged) for (i = 0; i < run.subs.length; i++) {
      var s1 = copyPlane(planeAt(planes, run.subs[i], run.subs[i]));
      zeroUnder(s1, fold.fold);
      face = over(face, s1);
      mergedSubs.push(run.subs[i]);
    }
    var static2 = null;
    if (!run.merged && run.subs.length) {
      static2 = blankPlane(SCREEN_W, SCREEN_H);
      for (i = 0; i < run.subs.length; i++) {
        var s2 = copyPlane(planeAt(planes, run.subs[i], run.subs[i]));
        zeroUnder(s2, fold.fold);
        static2 = over(static2, s2);
      }
    }
    var glass = planes['layers/glass.png'] ? copyPlane(planes['layers/glass.png']) : null;
    if (glass) zeroUnder(glass, fold.fold);
    var overlayPlane = glass || blankPlane(SCREEN_W, SCREEN_H);
    if (outer) overlayPlane = over(overlayPlane, tintOf(outer, fold.rim));
    if (maskCount(fold.liveRim)) overlayPlane = over(overlayPlane, maskedPlane(outer, fold.liveRim));
    report.counts.overlayInvisible = killInvisible(overlayPlane, INVISIBLE);
    // The proof the fold exists for: nothing live may paint on a folded texel. A non-zero
    // count here is a bug in this file, not an art choice, so it stops the bake.
    [['dial', face], ['overlay', overlayPlane]].forEach(function (q) {
      var a = q[1].rgba, n = 0;
      for (var k = 0; k < fold.fold.length; k++) if (fold.fold[k] && a[k * 4 + 3]) n++;
      if (n) throw new Error('REFUSING to fold the bezel: ' + q[0] + ' still paints ' + n +
        ' px on top of folded pixels');
    });
    // ---- the bank: every base frame with the folded rim copied into it
    var baseNames = namesUnder(planes, 'base/').filter(function (n) { return /[.]png$/i.test(n); });
    if (!baseNames.length) throw new Error('the kit has no base/ frames');
    var foldImg = outer ? maskedPlane(outer, fold.fold) : null;
    var bankPlanes = baseNames.map(function (n) {
      var im = copyPlane(planes[n]);
      return foldImg ? over(im, foldImg) : im;
    });
    if (bankPlanes.length !== B) report.notes.push('the kit declares ' + B + ' frames but ships ' + bankPlanes.length);

    // ---- pack the bank, then every sprite, in bake_clock.py's blob order
    var blob = new Blob();
    var occl = null;
    if (opts.bgOcclude !== 'off') {
      [face, static2, overlayPlane].forEach(function (L) {
        if (!L) return;
        var o = opaqueCopy(L);
        for (var k = 0; k < o.w * o.h; k++) if (o.rgba[k * 4 + 3] === 255) occl = occl || new Uint8Array(SCREEN_W * SCREEN_H), occl[k] = 1;
      });
      if (occl && !maskCount(occl)) occl = null;
    }
    var bank = bakeBgBank(blob, bankPlanes, { fps: fps, diffThresh: diffThresh, denoise: denoise,
                                              maxRects: maxRects, maxFlush: maxFlush, occl: occl, matte: null });
    var plateOff = bank.keyOff;
    report.counts.rects = bank.rects.length / 5 | 0;
    report.counts.bankPct = Math.round(100 * 2 * bank.packedTexels / (B * SCREEN_W * SCREEN_H * 2));

    var overlayRec;
    var ovCrop = cropLayer(overlayPlane.rgba, overlayPlane.w, overlayPlane.h, pad);
    if (!ovCrop) overlayRec = blankSprite(0, 0, 0);
    else overlayRec = emitSprite(blob, ovCrop, ovCrop.offx, ovCrop.offy, ovCrop.offx, ovCrop.offy,
                                 ovCrop.offx, ovCrop.offy, 0, true);

    var handRecs = [], shadowRecs = [];
    var styleHands = { hour: null, minute: null, second: null, static1: true, static2: !!static2 };
    KINDS.forEach(function (kind) {
      var cx = SCREEN_W / 2, cy = SCREEN_H / 2, blend = 0, show = false, plane = null, px, py;
      if (kind === 'static1') { plane = face; show = true; }
      else if (kind === 'static2') {
        // The sub-dials merged into the dial sprite (prep's mergeable run) or the kit has
        // none, so there is no second static: bake_clock.py emits a blank record and ships
        // no pixels for it, which is what the reference shows as `static2 show=0`.
        if (!static2) { handRecs.push(blankSprite(cx, cy, 0)); shadowRecs.push(blankSprite(cx, cy, 0)); return; }
        plane = static2; show = true;
      }
      else {
        if (!hand[kind]) { handRecs.push(blankSprite(cx, cy, 0)); shadowRecs.push(blankSprite(cx, cy, 0)); return; }
        plane = hand[kind].plane;
        cx = hand[kind].centre[0]; cy = hand[kind].centre[1];
        px = cx; py = cy;                                            // prep pins the pivot onto the centre
        show = inkCount(plane) > 0;
        if (kind === 'second' && tb.length) show = inkCount(tb[0]) > 0;
        if (!opts.noPivotSnap && show) {
          var sn = snapPivotToHub(plane, px, py, 10, 5, 1.0);
          if (sn.x !== px || sn.y !== py) report.notes.push(kind + ' pivot snapped ' + px + ',' + py + ' -> ' + sn.x + ',' + sn.y + ' (orbit ' + sn.orbit.toFixed(2) + ' px)');
          px = sn.x; py = sn.y;
        }
      }
      var crop = cropLayer(plane.rgba, plane.w, plane.h, pad);
      if (!crop) { handRecs.push(blankSprite(cx, cy, 0)); shadowRecs.push(blankSprite(cx, cy, 0)); return; }
      handRecs.push(emitSprite(blob, crop, crop.offx, crop.offy, px === undefined ? cx : px,
                               py === undefined ? cy : py, cx, cy, blend, show));
      var sk = kind === 'static1' || kind === 'static2' ? null : kind;
      if (sk && shadow[sk]) {
        var sc = cropLayer(shadow[sk].rgba, shadow[sk].w, shadow[sk].h, pad);
        shadowRecs.push(emitSprite(blob, sc, sc.offx, sc.offy, px === undefined ? cx : px,
                                   py === undefined ? cy : py,
                                   cx + (hand[sk].shadowDx || 0), cy + (hand[sk].shadowDy || 0),
                                   0, shadowOn && show));
      } else shadowRecs.push(blankSprite(cx, cy, 0));
    });

    var tourbRecs = [];
    if (tb.length) {
      tourbRecs = bakeTourbillon(blob, tb, { pivot: tbC, centre: tbC, pad: pad, diffThresh: diffThresh });
      handRecs[2] = Object.assign({}, tourbRecs[0], { show: 1 });
      styleHands.second = true;
    }

    // ---- complications, then the sub-dial pointers: both append to the blob first
    var buckets = underLayers(order, M.layout, run.merged);
    var layoutComps = JSON.parse(JSON.stringify(M.layout || { comps: [] }));
    layoutComps.comps.forEach(function (c) { c.underLayer = buckets[c.id] === undefined ? 0 : buckets[c.id]; });
    var comps = bakeComplications(blob, layoutComps, function (cid, ctype) {
      var hit = null;
      Object.keys(planes).forEach(function (n) {
        var m = /^complications\/([^/]+)\//.exec(n);
        if (m && m[1].split('_')[0] === cid) hit = hit || [], hit.push({ value: n.split('/').pop().replace(/\.png$/i, ''), plane: planes[n] });
      });
      return hit;
    });
    var compsFolded = rebucket(comps, 0);           // no --matte fold in this bake
    var aux = bakeAux(blob, auxPlanes, { pad: pad });
    // ---- the scalars, the order, and the gates ------------------------------
    var orderIdx = { hour: 0, minute: 1, second: 2, static1: 3, static2: 4 };
    var showFlags = { hour: !!hand.hour, minute: !!hand.minute, second: !!hand.second || !!tb.length,
                      static1: true, static2: !!static2 };
    var below = tb.length ? ['second'] : [];
    var statics = ['static1', 'static2'].filter(function (k) { return showFlags[k]; });

    var zs = {};
    (M.hands || []).forEach(function (h) { zs[h.kind] = h.z || 0; });
    var above = ['second', 'hour', 'minute'].filter(function (k) { return showFlags[k] && below.indexOf(k) < 0; });
    above.sort(function (a, b) { return (zs[a] || 0) - (zs[b] || 0); });
    var kindOrder = below.concat(statics, above).map(function (k) { return orderIdx[k]; });
    if (!kindOrder.length) throw new Error('the dial draws no live layer at all');

    // The cadence, in prep_watch_kit.py's order: its default is a sweep with the bank's own
    // fps as the tick rate (so hand_beat_ticks() is 1 and the angle comes from the wall clock
    // every tick), and --tick pins it to the 1 Hz mechanical step. The kit manifest's
    // secondHand string is the composer's Motion dropdown — 'sweep' or '<N> ticks per
    // second' — and the baked dial now WEARS what that dropdown picked: a tick choice
    // ships second_sweep=0 at its named beat. Over a delta bank a ticking hand strobes
    // (its box repaints between the bank's row pushes); the 60 Hz GIF subdivision keeps
    // the bank itself animating at full tempo, and the operator accepted the strobe as
    // the honest cost of the chosen look. opts.tick/--no-sweep stay the CLI's own lever,
    // opts.forceSweep the escape hatch that still overrules the manifest (the note below
    // fires when it does).
    var tick = opts.tick ? true : false;
    var manifestTickRate = 0;
    if (!tick && !opts.noSweep && !opts.forceSweep) {
      var mh = /^(\d+)\s+ticks?/i.exec(String(M.secondHand || ''));
      if (mh) manifestTickRate = Math.max(1, Math.min(255, +mh[1] | 0));
      else if (centre.second && centre.second.tick && centre.second.tick !== 'sweep')
        manifestTickRate = Math.max(1, Math.min(255, +centre.second.tick | 0));
      if (manifestTickRate) tick = true;
    }
    var sweep = !tick;
    var tickRate = tick ? (manifestTickRate || 1) : fps;
    if (opts.forceSweep) { sweep = true; tickRate = fps; }
    else if (!opts.noSweep && !tick) { sweep = true; }
    if (opts.tickRate) tickRate = opts.tickRate;
    tickRate = Math.max(1, Math.min(255, tickRate | 0));
    if (String(M.secondHand || 'sweep') !== 'sweep' && sweep)
      report.notes.push('the studio previewed the seconds hand as ' + JSON.stringify(M.secondHand) +
        ', but forceSweep overruled it: the baked dial sweeps (drop forceSweep to keep the step)');
    var scalars = {
      bg: 0, plate_follow: 0, shadow_on: shadowOn ? 1 : 0, shadow_dx: 0, shadow_dy: 0,
      second_sweep: sweep ? 1 : 0, second_railway: 0, text_over_hands: 0, tick_rate: tickRate,
      bg_frames: 0, bg_fps: 6, bg_loop: 0, bg_every: 3600,
      plate_off: plateOff,
      order0: kindOrder[0] || 0, order1: kindOrder[1] || 0, order2: kindOrder[2] || 0,
      order3: kindOrder[3] || 0, order4: kindOrder[4] || 0, order_n: kindOrder.length,
      matte_mask: 0, bg_mode: 1, bg_key_off: bank.keyOff, gif_frames: bank.frames,
      tourbillon_n: tourbRecs.length, tourbillon_fps: opts.tourbillonFps || fps
    };
    if (kindOrder.length > 5) throw new Error('the layer order is ' + kindOrder.length + ' long; the dial holds 5');
    if (bank.frames < 2) throw new Error('the bank holds ' + bank.frames + ' frames; a delta-rect background needs two');
    if (occl) report.counts.occludedPx = maskCount(occl);
    report.notes.push('no static fold: the dial and the glass are live layers, so every commit pays their blends (the bench bake proves and folds them with --matte auto)');
    report.live = [
      ['dial', inkCount(face)],
      ['sub-dials', static2 ? inkCount(static2) : 0],
      ['overlay', inkCount(overlayPlane)],
      ['case folded into the bank', report.counts.foldedPx]
    ];

    var art = blob.join();
    var geom = packGeom({
      scalars: scalars, hands: handRecs, shadows: shadowRecs, overlay: overlayRec,
      tourbillon: tourbRecs, rects: bank.rects, rectFirst: bank.first,
      gifDelays: bank.delays, bgFrames: [0], comps: comps, aux: aux, bank: []
    });
    var bytes = writeContainer({
      geom: geom, art: art, scrW: SCREEN_W, scrH: SCREEN_H,
      name: kit.name,
      serial: kit.serial || 0, bakedMs: kit.bakedMs || 0
    });
    report.bytes = bytes.length;
    report.artBytes = art.length;
    report.geomBytes = geom.length;
    report.counts.comps = comps.length;
    report.counts.aux = aux.length;
    report.counts.tourbillon = tourbRecs.length;
    return { bytes: bytes, report: report, scalars: scalars };
  }
  function rebucket(comps, matteMask) {
    var changed = [];
    (comps || []).forEach(function (r) {
      var b = +r.first;
      if (b && (matteMask & (1 << b))) { r.first = 0; changed.push(b); }
    });
    return changed;
  }

  /* A window tile: RGB565 then its planar alpha, packed raw. The reader strides frames by
   * w*h*3 — the value the caller writes into the record's frame_bytes. */
  function packWindowTile(blob, plane) {
    var off = blob.add(rgb565Plane(plane.rgba, plane.w, plane.h));
    blob.add(alphaPlane(plane.rgba, plane.w, plane.h));
    return off;
  }

  /* The page-side reader. DecompressionStream only answers asynchronously, and decodePng
   * is synchronous with inflate injected (so the node parity test can hand it node:zlib),
   * so this pre-passes the chunk headers, awaits the one IDAT inflate, and hands the ready
   * bytes to decodePng as its inflate result. The header walk is cheap; the pixel work
   * happens once. */
  async function decodePngAsync(bytes) {
    if (typeof DecompressionStream !== 'function') throw new Error('this browser has no DecompressionStream, so the kit PNGs cannot be inflated here');
    const u8 = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
    const idat = pngIdat(u8);
    // `Blob` in this file is the art-blob builder, so the stream is drained by hand rather
    // than through Response/Blob pipeThrough.
    const ds = new DecompressionStream('deflate');
    const w = ds.writable.getWriter(); w.write(idat); w.close();
    const r = ds.readable.getReader(); const parts = []; let n = 0;
    for (;;) {
      const { done, value } = await r.read();
      if (done) break;
      parts.push(value); n += value.length;
    }
    const out = new Uint8Array(n); let o = 0;
    for (const p of parts) { out.set(p, o); o += p.length; }
    return decodePng(u8, () => out);
  }
  /* The concatenated IDAT payload bytes, with the same walk decodePng makes. */
  function pngIdat(u) {
    let p = 8; const parts = [];
    while (p + 8 <= u.length) {
      const len = (u[p] << 24 | u[p + 1] << 16 | u[p + 2] << 8 | u[p + 3]) >>> 0, q = p + 4;
      const type = String.fromCharCode(u[q], u[q + 1], u[q + 2], u[q + 3]);
      if (type === 'IDAT') parts.push(u.subarray(q + 4, q + 4 + len));
      if (type === 'IEND') break;
      p = q + 8 + len;
    }
    let n = 0; for (const s of parts) n += s.length;
    const all = new Uint8Array(n); let o = 0; for (const s of parts) { all.set(s, o); o += s.length; }
    return all;
  }

  /* ---- reading the kit's PNGs --------------------------------------------- */

  /* A straight PNG reader, not a canvas round-trip. Chrome stores its canvas backing
   * store premultiplied, so drawImage + getImageData can move the colour under a partly
   * transparent texel by one; the kit's PNG bytes are the art the bench bake reads, and
   * reading them directly is what lets a browser container match a bench container byte
   * for byte. inflate is injected: DecompressionStream in the page, node:zlib in the test.
   * 8-bit non-interlaced RGBA/RGB/grey/palette; anything else is refused rather than
   * guessed at. */
  function decodePng(bytes, inflate) {
    var u = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
    var SIG = [137, 80, 78, 71, 13, 10, 26, 10];
    for (var s = 0; s < 8; s++) if (u[s] !== SIG[s]) throw new Error('not a PNG');
    var p = 8, w = 0, h = 0, depth = 0, ctype = 0, inter = 0, idat = [], plte = null, trns = null;
    while (p < u.length) {
      var len = (u[p] << 24 | u[p + 1] << 16 | u[p + 2] << 8 | u[p + 3]) >>> 0, type = '', q = p + 4;
      for (var t = 0; t < 4; t++) type += String.fromCharCode(u[q + t]);
      var body = u.subarray(q + 4, q + 4 + len);            // past the length AND the four type bytes
      if (type === 'IHDR') {
        w = (body[0] << 24 | body[1] << 16 | body[2] << 8 | body[3]) >>> 0;
        h = (body[4] << 24 | body[5] << 16 | body[6] << 8 | body[7]) >>> 0;
        depth = body[8]; ctype = body[9]; inter = body[12];
      } else if (type === 'IDAT') idat.push(body);
      else if (type === 'PLTE') plte = body;
      else if (type === 'tRNS') trns = body;
      if (type === 'IEND') break;
      p = q + 8 + len;                     // length(4) + type(4) + data(len) + CRC(4)
    }
    if (depth !== 8 || inter !== 0) throw new Error('PNG is bit depth ' + depth + ', interlace ' + inter + '; only 8-bit non-interlaced reads are implemented');
    var channels = ctype === 6 ? 4 : ctype === 2 ? 3 : ctype === 0 ? 1 : ctype === 3 ? 1 : ctype === 4 ? 2 : -1;
    if (channels < 0) throw new Error('unsupported PNG colour type ' + ctype);
    var raw = inflate(concat(idat));
    var stride = w * channels, out = new Uint8Array(w * h * 4), prev = new Uint8Array(stride);
    var pos = 0;
    for (var y = 0; y < h; y++) {
      var ft = raw[pos++], line = raw.subarray(pos, pos + stride); pos += stride;
      var cur = new Uint8Array(stride);
      for (var x = 0; x < stride; x++) {
        var a = x >= channels ? cur[x - channels] : 0, b = prev[x], c = x >= channels ? prev[x - channels] : 0, v = line[x];
        if (ft === 1) v = v + a; else if (ft === 2) v = v + b; else if (ft === 3) v = v + ((a + b) >> 1);
        else if (ft === 4) {
          var pp = a + b - c, pa = Math.abs(pp - a), pb = Math.abs(pp - b), pc = Math.abs(pp - c);
          v = v + (pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
        } else if (ft !== 0) throw new Error('unknown PNG filter ' + ft);
        cur[x] = v & 0xff;
      }
      prev = cur;
      for (var i = 0; i < w; i++) {
        var o = (y * w + i) * 4, k = i * channels, r0, g0, b0, al;
        if (ctype === 6) { r0 = cur[k]; g0 = cur[k + 1]; b0 = cur[k + 2]; al = cur[k + 3]; }
        else if (ctype === 2) { r0 = cur[k]; g0 = cur[k + 1]; b0 = cur[k + 2]; al = trns && r0 === trns[0] && g0 === trns[1] && b0 === trns[2] ? 0 : 255; }
        else if (ctype === 0) { r0 = g0 = b0 = cur[k]; al = trns && cur[k] === (trns[0] << 8 | trns[1]) ? 0 : 255; }
        else if (ctype === 4) { r0 = g0 = b0 = cur[k * 2]; al = cur[k * 2 + 1]; }
        else {
          var idx = cur[i]; if (!plte || idx * 3 + 2 >= plte.length) throw new Error('PNG palette index out of range');
          r0 = plte[idx * 3]; g0 = plte[idx * 3 + 1]; b0 = plte[idx * 3 + 2];
          al = trns ? (idx < trns.length ? trns[idx] : 0) : 255;
        }
        out[o] = r0; out[o + 1] = g0; out[o + 2] = b0; out[o + 3] = al;
      }
    }
    return { w: w, h: h, rgba: out };
  }
  function concat(parts) {
    var n = 0, i;
    for (i = 0; i < parts.length; i++) n += parts[i].length;
    var out = new Uint8Array(n), o = 0;
    for (i = 0; i < parts.length; i++) { out.set(parts[i], o); o += parts[i].length; }
    return out;
  }

  /* ---- FORMAT 3: the bundle ------------------------------------------------
   *
   * tools/orbbundle.py defines this layout; main/art/orb_bundle.c reads it on the
   * device; internal/orbnet/themecontainer.go pre-gates it at the hub. This is the
   * fourth implementation, and the only one a person without a bench or a Go binary can
   * run — Decision D33 puts the format logic in the browser because the release is a web
   * page that flashes over Web Serial, so nothing on this path may require a server.
   * _selftest/headless-bundle3.test.js holds these bytes against the Python's, and the
   * file both reproduce is the committed tests/fixtures/format3-pair.orbtheme, which the
   * C door verifies with `make_bundle_fixture.py --verify-c`.
   *
   * FORMAT 3 is FORMAT 2's header with two stamps swapped and word 9's meaning changed:
   * where a v2 container kept scr_w it keeps n_sections, since each section names its own
   * panel. One container themes several apps, and the element table — not a dialect's
   * field list — is how a theme says what it paints (D32, R1). */
  var FORMAT3 = 3, GEOM_ABI3 = 3, SECT_MAX = 8, IDX_WORDS = 10;   // Ruling A: the
  var GEOM_ABI6 = 6;    // D42: the clock's dialect, and the only one it reads. An app
                        // section stays 3; the clock's 4 and 5 were retired with no legacy
  var DELTA_TOL = 16;   // §36: the summed |dR|+|dG|+|dB|+|dA| (max 1020) a texel must clear to
                       // earn a delta byte. diffRect and selfVerify3 read the same number, so
                       // the codec's floor and the bake's round-trip promise can never disagree.
  // element row APPENDED kind and src without a bump — the blob's derived row
  // stride (see unpackBlob3) is what makes a narrower old row and a wider new
  // one the same readable file. A stamped 3 on a 19-word row is the promise,
  // and an old reader's fixed 17-word walk refuses the new bytes at its door.
  var SCALARS_N = THEME_FIELDS.length;   // 26: the clock dialect, carried opaque (D31)
  var SPR_WORDS = 4;                     // off, size, w, h — one bank entry's byte range
  var BLOB_HEAD = 4 + SCALARS_N;         // counts + scalars, before the element table
  var GEOM_CAP3 = 0xF000;                // orb_theme.c SLOT_GEOM_CAP
  var SLOT_CAP3 = DEVICE_SLOT_CAP, SLOT_ART_OFF3 = SLOT_ART_OFF2;  // orb_theme.c SLOT_CAP /
  // SLOT_ART_OFF — imported from the two partition facts above, never restated (§7.4:
  // this line carried the retired 4 MB pair's figure while the device reported 8 MB)
  var SLOT_ART_CAP3 = SLOT_CAP3 - SLOT_ART_OFF3;   // ORB_THEME_SLOT_CAP: banks' room
  var SLUG_MAX = 23;                     // ORBAPP_SLUG_MAX - 1: the NUL's room

  /* tools/theme_schema.py's element table. Identity is (class, slot), not a string: the
   * app's manifest declares slots by class and ordinal, a row names the slot it fills,
   * and the check in both directions is an integer compare. Fixed width means one u32 per
   * column, so a later schema appends past scale16 and a shorter reader reads what it
   * knows — the append-only rule that keeps a column from costing a format bump (D24). */
  var ELEMENT_FIELDS = [
    ['n', 'u16'], ['class', 'u8'], ['flags', 'u8'], ['order', 'u8'], ['alpha', 'u8'],
    ['slot', 'u16'], ['frame0', 'u16'], ['n_frames', 'u16'],
    ['x', 'i16'], ['y', 'i16'], ['w', 'u16'], ['h', 'u16'],
    ['pivot_x', 'i16'], ['pivot_y', 'i16'], ['radius', 'u16'],
    ['degrees', 'u16'], ['scale16', 'u16'],
    ['kind', 'u8'], ['src', 'u8'],   // D32's ruling A. On an AUX row the pair reads as
                             // the row's shape (D42) and its driving source.
    ['span', 'i16'], ['zero', 'i16'], ['vmin', 'u16'], ['vmax', 'u16'],
    // D34's four drive columns, read by D42 as an AUX row's angular poses: zero is P0's
    // angle, span the turn to P1 (tenths of a degree), across the domain vmin..vmax.
    // C4's delta dialect, appended exactly as D32's ruling A did. Both mean nothing
    // below the v4 clock stamp; a row read through a narrower stride lands on their
    // defaults — parent NONE, delay 0 — the same append-only answer the drive four got.
    ['parent', 'u16'],    // row n of the drawing row this one hangs on; 0xFFFF = none
    ['delay', 'u16'],     // per-frame ms for this row's animation; 0 = the dial's default
    // D41's motion columns (docs/history/hand-motion-plan.md §4), appended the same way. They
    // mean nothing below the v5 clock stamp; a narrower row reads 0 — inherit.
    ['cadence', 'u8'],    // AUX/BANK: CADENCES id, when the row moves
    ['cad_arg', 'u16'],   // step N, stop-to-go P (0.1 s), drag window (min), sweep fps
    ['ease', 'u16'],      // settle/flyback ms
    // D42's five (docs/history/element-model-plan.md §8): the clock's v6 row.
    ['dx', 'i16'], ['dy', 'i16'],   // AUX: the pivot's travel from P0 to P1, px
    ['end', 'u8'],        // AUX: ENDS id — wrap, flyback, hold
    ['digit', 'u8'],      // AUX: DIGITS id — whole, units, tens
    ['str', 'u16']        // AUX text row: string-table offset (E4); 0
  ];
  var ELEMENT_WORDS = ELEMENT_FIELDS.length;   // 33 words, 132 bytes a row: the clock's v6
  var ELEMENT_WORDS_V3 = 23;                    // every app's row, whatever the stamp
  var SPR_WORDS2 = 6;
  var PARENT_NONE = 0xFFFF;
  // The pre-D32 width, the floor a derived row stride is refused below
  // (theme_schema.ELEMENT_WORDS_MIN; orb_bundle.c's ORB_BUNDLE_ELEM_WORDS_MIN).
  var ELEMENT_WORDS_MIN = 17;
  var ELEM_MAX = 128, ELEM_ORDER_N = 64, ELEM_FRAMES_MAX = 1024;   // theme_schema.ELEM_FRAMES_MAX
  var EF_ALPHA = 1, EF_LOOP = 2, EF_OS_ADV = 4, EF_HIDDEN = 8;
  // §37: a looping delta plate carries ONE sprite past its own frame window — the rect
  // that returns the finished canvas to the keyframe — and claims it with this bit. The
  // device applies it when the loop wraps to frame 0; without it the wrap is a keyframe
  // blit the delta table cannot express, which is the once-a-loop stutter and the stale
  // base of frame 1. Bit 6, not 5: 5 is EF_INSTANCE, taken. theme_schema.EF_DELTA_WRAP
  // and orb_clock3.c's plate gate read the same byte; checkElement below is the writer's
  // mirror of that gate.
  var EF_DELTA_WRAP = 64;
  // The unified render path's slice 5: this hand, shadow or pointer row samples bilinear,
  // in the LVGL widget and the L4 painter alike. theme_schema.EF_SMOOTH; the bake sets it
  // from estimate3 and the dial only reads it. All eight bits of the u8 column are named.
  var EF_SMOOTH = 128;
  var EF_ALL = EF_ALPHA | EF_LOOP | EF_OS_ADV | EF_HIDDEN | AUX_F_SHADOW3 | EF_DELTA_WRAP | EF_SMOOTH;
  var CLASS_NAMES = ['text', 'sprite', 'hand', 'plate', 'ring', 'bank', 'layer', 'status', 'aux', 'preview', 'sound', 'values'];
  var CLASS_TEXT = 0, CLASS_SPRITE = 1, CLASS_HAND = 2, CLASS_PLATE = 3;
  var CLASS_RING = 4, CLASS_BANK = 5, CLASS_LAYER = 6, CLASS_STATUS = 7;
  var CLASS_AUX = 8;                       // D34: a pointer, appended at 8
  var CLASS_PREVIEW = 9;                   // the picker's plane, appended at 9
  var CLASS_SOUND = 10;                    // D39: a waveform, appended at 10
  /* radar-plan §2.11: a theme's values, appended at 11 (theme_schema.CLASS_VALUES): one blob
   * of the app's settings, w bytes by 1, at most VALUES_MAX, its layout version in src. */
  var CLASS_VALUES = 11, VALUES_MAX = 4096;
  /* D39: the clock's sound vocabulary (theme_schema.CLOCK_SOUND_SLOTS / HOUR_PATTERNS,
   * orb_clock3.h CLK_SND_* / CLK_PAT_*), in slot order. */
  var SOUND_SLOTS = ['tick_loop', 'chime_q1', 'chime_half', 'chime_q3', 'chime_hour',
    'strike_hour', 'jump_minute', 'jump_hour', 'jump_date', 'morse_dit', 'morse_dah',
    'strike_qtr', 'strike_min'];
  var HOUR_PATTERNS = ['count', 'ships_bells', 'morse', 'repeater', 'none'];
  var SOUND_RATE = 22050;
  /* A theme's sounds are blocked IMA ADPCM (theme_schema.SOUND_ENC_ADPCM, main/audio/
   * orb_adpcm.h): 256 samples a block, each block decoding from its own 4-byte header.
   * adpcmEncode is theme_schema.adpcm_encode line for line; tools/host_adpcm_test.py
   * holds the two to the same bytes. */
  var SOUND_ENC_PCM16 = 0, SOUND_ENC_ADPCM = 1, ADPCM_BLOCK = 256, ADPCM_BLOCK_BYTES = 132;
  var ADPCM_STEP = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552,
    1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484,
    7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385,
    24623, 27086, 29794, 32767];
  var ADPCM_INDEX = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8];
  /* theme_schema.ADPCM_MIN_SNR_DB / sound_encode: a clip is stored compressed only when
   * the round trip holds this; a sharp tick does not, and stays PCM16. */
  var ADPCM_MIN_SNR_DB = 20;
  function adpcmDecode(data, n) {
    var out = new Int16Array(n);
    for (var b = 0, nb = Math.ceil(n / ADPCM_BLOCK); b < nb; b++) {
      var o = b * ADPCM_BLOCK_BYTES, pred = (data[o] | (data[o + 1] << 8)) << 16 >> 16, idx = Math.min(88, data[o + 2]);
      var i0 = b * ADPCM_BLOCK;
      if (i0 < n) out[i0] = pred;
      for (var k = 0; k < ADPCM_BLOCK - 1; k++) {
        var code = (data[o + 4 + (k >> 1)] >> ((k & 1) * 4)) & 15, step = ADPCM_STEP[idx], diff = step >> 3;
        if (code & 4) diff += step;
        if (code & 2) diff += step >> 1;
        if (code & 1) diff += step >> 2;
        pred = code & 8 ? pred - diff : pred + diff;
        pred = pred > 32767 ? 32767 : pred < -32768 ? -32768 : pred;
        idx += ADPCM_INDEX[code];
        idx = idx < 0 ? 0 : idx > 88 ? 88 : idx;
        if (i0 + 1 + k < n) out[i0 + 1 + k] = pred;
      }
    }
    return out;
  }
  function soundEncode(pcm) {
    var enc = adpcmEncode(pcm), dec = adpcmDecode(enc, pcm.length), sig = 0, err = 0;
    for (var i = 0; i < pcm.length; i++) { var x = pcm[i] | 0, d = x - dec[i]; sig += x * x; err += d * d; }
    if (err === 0 || sig >= err * Math.pow(10, ADPCM_MIN_SNR_DB / 10)) return { enc: SOUND_ENC_ADPCM, bytes: enc };
    var pb8 = new Uint8Array(pcm.length * 2), pdv = new DataView(pb8.buffer);
    for (var pi = 0; pi < pcm.length; pi++) pdv.setInt16(pi * 2, pcm[pi], true);
    return { enc: SOUND_ENC_PCM16, bytes: pb8 };
  }
  function soundBytes(enc, n) { return enc === SOUND_ENC_ADPCM ? Math.ceil(n / ADPCM_BLOCK) * ADPCM_BLOCK_BYTES : 2 * n; }
  function adpcmEncode(pcm) {
    var nb = Math.ceil(pcm.length / ADPCM_BLOCK), out = new Uint8Array(nb * ADPCM_BLOCK_BYTES), idx = 0;
    for (var b = 0; b < nb; b++) {
      var o = b * ADPCM_BLOCK_BYTES, b0 = b * ADPCM_BLOCK, pred = pcm[b0] | 0;
      out[o] = pred & 0xFF; out[o + 1] = (pred >> 8) & 0xFF; out[o + 2] = idx; out[o + 3] = 0;
      for (var k = 0; k < ADPCM_BLOCK - 1; k++) {
        var x = b0 + 1 + k < pcm.length ? pcm[b0 + 1 + k] | 0 : 0;
        var step = ADPCM_STEP[idx], d = x - pred, code = d < 0 ? 8 : 0;
        d = Math.abs(d);
        if (d >= step) { code |= 4; d -= step; }
        if (d >= step >> 1) { code |= 2; d -= step >> 1; }
        if (d >= step >> 2) code |= 1;
        var diff = step >> 3;
        if (code & 4) diff += step;
        if (code & 2) diff += step >> 1;
        if (code & 1) diff += step >> 2;
        pred = code & 8 ? pred - diff : pred + diff;
        pred = pred > 32767 ? 32767 : pred < -32768 ? -32768 : pred;
        idx += ADPCM_INDEX[code];
        idx = idx < 0 ? 0 : idx > 88 ? 88 : idx;
        out[o + 4 + (k >> 1)] |= code << ((k & 1) * 4);
      }
    }
    return out;
  }
  // D42 (theme_schema.SHAPES / ENDS / DIGITS, clk_types.h CLK_SHAPE_* / CLK_END_* /
  // CLK_DIGIT_*): what an AUX row's art is, what it does past its domain, and which
  // digit of its source it reads.
  var SHAPES = ['arm', 'ring', 'text', 'drum'];
  var SHAPE_ARM = 0, SHAPE_RING = 1, SHAPE_TEXT = 2, SHAPE_DRUM = 3;   // D43: drum, a barrel the OS rolls
  var ENDS = ['wrap', 'flyback', 'hold'];
  var END_WRAP = 0, END_FLYBACK = 1, END_HOLD = 2;
  var DIGITS = ['whole', 'units', 'tens', 'hundreds', 'thousands'];   // D43 P5d.3: a year's four
  var TURN_10 = 3600, PANEL_PX = 466;
  var CLK_ROW_MAX = 64;                    // theme_schema.CLK_ROW_MAX (theme-pool-plan: 24 until the plane pool)
  var ORB_LAYER_MAX3 = 32;                 // main/gfx/orb_layers.h ORB_LAYER_MAX: drawn rows per app (theme-text T3)
  // theme-text-plan §2.1-2.2 (theme_schema.FONT_* / check_label / check_fonts): a font is a
  // TEXT row of glyph frames; a label an AUX text row naming its fonts a nibble a tone.
  // 129: ASCII, five symbols, §6.22's dotless i and 28 accent marks
  var FONT_ASCII_N = 95, FONT_GLYPHS = 129, FONT_SLOTS = 15, FONT_NONE = 0xF, LABEL_PAD_MAX = 32;
  var TONES = ['normal', 'accent', 'warning', 'quiet'], ALIGNS = ['start', 'centre', 'end'];
  var LABEL_STILL_COLUMNS = ['span', 'zero', 'vmin', 'vmax', 'dx', 'dy', 'cadence', 'cad_arg', 'ease'];
  function labelFonts(str) { return [0, 1, 2, 3].map(function (k) { return ((str | 0) >> (4 * k)) & 0xF; }); }
  function checkLabel(rec, where) {
    var src = rec.src | 0;
    if (SOURCES[src] !== 'text')
      throw new Error(where + '.src: a text row draws the app\u2019s string, so it reads source text, not ' + q(SOURCES[src]));
    LABEL_STILL_COLUMNS.forEach(function (c) {
      if (Math.trunc(rec[c] | 0))
        throw new Error(where + '.' + c + ': ' + rec[c] + ' on a text row \u2014 the app places a label, so it names no motion');
    });
    var dg = rec.digit | 0, en = rec.end | 0;
    if (!(dg >= 0 && dg < ALIGNS.length) || !(en >= 0 && en < ALIGNS.length))
      throw new Error(where + ': a text row aligns by digit ' + dg + ' and end ' + en + ', each one of ' + ALIGNS.join('/'));
    if (labelFonts(rec.str)[0] === FONT_NONE)
      throw new Error(where + '.str: a text row names no font for tone 0 (normal), which every other tone falls back to');
    if ((rec.n_frames | 0) !== 1 || ((rec.flags | 0) & AUX_F_SHADOW3))
      throw new Error(where + ': a text row\u2019s art is its one panel, with no frames and no shadow');
    var pad = rec.radius | 0, w = rec.w | 0, h = rec.h | 0;
    if (pad > LABEL_PAD_MAX || 2 * pad >= w || 2 * pad >= h)
      throw new Error(where + '.radius: a padding of ' + pad + ' leaves no room in a ' + w + 'x' + h + ' panel (at most ' + LABEL_PAD_MAX + ')');
  }
  function checkFonts(recs) {
    var fonts = {};
    recs.forEach(function (r) {
      if ((r['class'] | 0) !== CLASS_TEXT) return;
      var slot = r.slot | 0, where = 'font' + slot, nf = r.n_frames | 0, w = r.w | 0;
      if (slot >= FONT_SLOTS)
        throw new Error(where + ': a stage seats ' + FONT_SLOTS + ' fonts (font0..font' + (FONT_SLOTS - 1) + ')');
      if (nf < FONT_ASCII_N || nf > FONT_GLYPHS)
        throw new Error(where + ': a font carries printable ASCII whole (' + FONT_ASCII_N + ' glyphs) and at most ' + FONT_GLYPHS + ', not ' + nf);
      if ((r.x | 0) || (r.y | 0))
        throw new Error(where + ': a font is never drawn where it stands, so it sits at 0,0');
      if (!((r.pivot_x | 0) > 0 && (r.pivot_x | 0) <= w && (r.pivot_y | 0) >= 0 && (r.pivot_y | 0) <= w))
        throw new Error(where + ': a space of ' + (r.pivot_x | 0) + ' px and tracking of ' + (r.pivot_y | 0) + ' px in a ' + w + ' px cell');
      fonts[slot] = r;
    });
    recs.forEach(function (r) {
      if ((r['class'] | 0) !== CLASS_AUX || (r.kind | 0) !== SHAPE_TEXT) return;
      labelFonts(r.str).forEach(function (f, tone) {
        if (f !== FONT_NONE && !fonts[f])
          throw new Error('ptr' + (r.slot | 0) + ': tone ' + tone + ' (' + TONES[tone] + ') names font' + f + ', which this theme does not carry');
      });
    });
  }
  // The composer's authoring motions (D34's names). The wire no longer carries them —
  // bakeClock3's v6Drive turns each into a row's two poses — but the composer still
  // speaks them until its rows are rebuilt (D42 E2).
  var MOTION_NAMES = ['rotate', 'retrograde', 'indexed', 'slide-x', 'slide-y'];
  var MOTION_ROTATE = 0, MOTION_RETROGRADE = 1, MOTION_INDEXED = 2,
      MOTION_SLIDE_X = 3, MOTION_SLIDE_Y = 4;
  function motionIsSlide(k) { return k === MOTION_SLIDE_X || k === MOTION_SLIDE_Y; }
  var ROW_FRAMES_MAX = 16;                 // clk_types.h CLK_TOURB_MAX: the carriage's frames
  /* theme-pool-plan §2.6: a moving row with frames and no frame time is one whose frame the
   * app picks at runtime (orb_stage_frame), the tracker's plane pool: up to clk_types.h
   * CLK_PICK_MAX frames, consecutive catalogue planes each the row's own size, no shadow
   * twin under it, any number of such rows. Frames WITH a frame time stay the carriage. */
  var CLK_PICK_MAX = 64;                   // theme_schema.CLK_PICK_MAX
  // D41 §2.1 (theme_schema.CADENCES / clk_types.h CLK_CAD_*). D42 retired inherit: 0 is
  // none, a still row's cadence and nobody else's.
  // D43 P5d.3's roll is a wheel's smooth: hold the figure, roll on in the unit before it changes.
  var CADENCES = ['none', 'sweep', 'step', 'jump', 'stop-to-go', 'drag', 'roll'];
  var CAD_NONE = 0, CAD_SWEEP = 1, CAD_STEP = 2, CAD_JUMP = 3, CAD_STOP_TO_GO = 4, CAD_DRAG = 5, CAD_ROLL = 6;
  var CAD_DRAG_SOURCES = ['date', 'weekday', 'month', 'day_of_year'];
  var SWEEP_FPS_MAX = 30;                  // theme_schema.SWEEP_FPS_MAX
  var MOTION_COLUMNS = ['span', 'zero', 'vmin', 'vmax', 'dx', 'dy', 'end', 'digit'];
  var D42_COLUMNS = ['dx', 'dy', 'end', 'digit', 'str'];
  // theme_schema.CLOCK_RETIRED_SCALARS: a v6 clock section carries each as 0.
  var CLOCK_RETIRED_SCALARS = ['plate_follow', 'shadow_on', 'shadow_dx', 'shadow_dy',
    'second_sweep', 'second_railway', 'text_over_hands', 'tick_rate', 'order0', 'order1',
    'order2', 'order3', 'order4', 'order_n', 'matte_mask', 'tourbillon_n', 'tourbillon_fps'];
  // R6: whose frames the OS may advance, and which classes cannot animate at all. This
  // decides the editor's element class for an animated region: an OS-paced region is a
  // SPRITE row, because a bank or a text row holds app state and a layer is composited.
  var CLASS_APP_ADV = [CLASS_BANK, CLASS_TEXT, CLASS_STATUS];
  var CLASS_NO_ANIM = [CLASS_LAYER, CLASS_STATUS, CLASS_PREVIEW, CLASS_SOUND, CLASS_VALUES];

  function blankElement() {
    var r = {};
    for (var i = 0; i < ELEMENT_FIELDS.length; i++) r[ELEMENT_FIELDS[i][0]] = 0;
    r.parent = PARENT_NONE;   // C4: a zeroed row would name the plate; NONE is the default claim
    return r;
  }
  function classId(name) {
    var i = CLASS_NAMES.indexOf(name);
    return i < 0 ? null : i;
  }
  function pad2hex(v) { var s = (v >>> 0).toString(16); while (s.length < 2) s = '0' + s; return s; }
  function q(s) { return "\x27" + s + "\x27"; }
  function utf8(s) { return new TextEncoder().encode(String(s)); }

  /* check_element, rule for rule and in its order. Each of these is a thing a reader
   * would otherwise get wrong quietly: an unknown class id, an order past the depth rungs,
   * a frame window off the end of the art, an animation on a class that cannot be one.
   * The editor refuses at the form rather than shipping a table the device paints wrongly
   * — the refusal direction D26's amendment exists for. */
  function checkElement(rec, where) {
    where = where || 'elem';
    var cls = Math.trunc(rec.class | 0), flags = Math.trunc(rec.flags | 0);
    var nf = Math.trunc(rec.n_frames | 0), f0 = Math.trunc(rec.frame0 | 0);
    if (!(cls >= 0 && cls < CLASS_NAMES.length)) {
      throw new Error(where + '.class: ' + cls + ' is not a class this build knows (0..' +
        (CLASS_NAMES.length - 1) + ')');
    }
    if (flags & ~EF_ALL) {
      throw new Error(where + '.flags: unknown flag bits 0x' + pad2hex(flags & ~EF_ALL) +
        ' — a schema this build does not know, refused rather than drawn wrong');
    }
    if ((flags & AUX_F_SHADOW3) && cls !== CLASS_AUX) {
      throw new Error(where + '.flags: the pointer shadow twin is a rule for class aux; class ' +
        q(CLASS_NAMES[cls]) + ' already owns its shadow\u2019s placement (§25.2)');
    }
    if ((flags & EF_SMOOTH) && cls !== CLASS_HAND && cls !== CLASS_SPRITE && cls !== CLASS_AUX) {
      throw new Error(where + '.flags: smoothing is a rotated layer\u2019s sampling (hand, shadow, pointer); class ' +
        q(CLASS_NAMES[cls]) + ' is not turned by the painter');
    }
    if ((flags & EF_DELTA_WRAP) && cls !== CLASS_PLATE) {
      throw new Error(where + '.flags: the loop-closing delta (§37) belongs to a delta plate and ' +
        'nowhere else; class ' + q(CLASS_NAMES[cls]) + ' has no background loop for it to close');
    }
    var ord = Math.trunc(rec.order | 0);
    if (!(ord >= 0 && ord < ELEM_ORDER_N)) {
      throw new Error(where + '.order: ' + ord + ' is past the dial\u2019s ' + ELEM_ORDER_N + ' depth rungs');
    }
    if (nf < 1) {
      throw new Error(where + '.n_frames: 0 draws nothing, and a declared element that ' +
        'draws nothing is a row nobody meant to write');
    }
    if (f0 + nf > ELEM_FRAMES_MAX) {
      throw new Error(where + ': frames ' + f0 + '..' + (f0 + nf - 1) +
        ' run past the art region\u2019s bound of ' + ELEM_FRAMES_MAX);
    }
    if (Math.trunc(rec.degrees | 0) >= 36000) {
      throw new Error(where + '.degrees: ' + rec.degrees + ' is not a direction (0..35999, 100ths)');
    }
    if ((flags & EF_OS_ADV) && CLASS_APP_ADV.indexOf(cls) >= 0) {
      throw new Error(where + ': class ' + q(CLASS_NAMES[cls]) + ' holds app state, so the ' +
        'OS must not advance its frames — clear the OS_ADV flag and let the app pick');
    }
    if (CLASS_NO_ANIM.indexOf(cls) >= 0 && nf !== 1) {
      throw new Error(where + ': class ' + q(CLASS_NAMES[cls]) + ' is composited, not ' +
        'animated — it carries one frame, not ' + nf);
    }
    // A hand turns about a point inside itself — orbapp.py asks its offer the same
    // question, so the two directions cannot disagree about one hand.
    if (cls === CLASS_HAND && rec.w && rec.h) {
      var px = Math.trunc(rec.pivot_x | 0), py = Math.trunc(rec.pivot_y | 0);
      var w = Math.trunc(rec.w | 0), h = Math.trunc(rec.h | 0);
      if (!(px >= 0 && px < w && py >= 0 && py < h)) {
        throw new Error(where + ': a HAND\u2019s pivot (' + px + ',' + py + ') sits outside ' +
          'its own ' + w + 'x' + h + ' plate, so it would turn about a point off the glass');
      }
    }
    if ((cls === CLASS_TEXT || cls === CLASS_STATUS) && (rec.radius | 0)) {
      throw new Error(where + ': class ' + q(CLASS_NAMES[cls]) + ' places glyphs and has no ' +
        'radius to give it; a non-zero one means a row written for another class');
    }
    var s16 = Math.trunc(rec.scale16 | 0);
    if (s16 && s16 < 16) {
      throw new Error(where + '.scale16: ' + s16 + ' scales the element below one sixteenth ' +
        'of its art, which no bake produces by intent');
    }
    /* ---- D32 ruling A: the two complication columns ----
     * They belong to a BANK row and to nothing else, and a BANK row's kind/src
     * must be real ids; a window on a fixed calendar domain carries exactly
     * the domain's own count of plates. Rules and sentences mirror
     * theme_schema.check_element rule for rule. */
    var kind = Math.trunc(rec.kind | 0), src = Math.trunc(rec.src | 0);
    /* D39 widens the pair the way D34 did: on a SOUND row `kind` is the hour pattern
     * and `src` the encoding; checkClockSounds reads them. */
    if (cls === CLASS_SOUND) {
      if (src !== SOUND_ENC_PCM16 && src !== SOUND_ENC_ADPCM)
        throw new Error(where + '.src: a sound in encoding ' + src + ' — this build plays 16-bit ' +
          'PCM (encoding 0) or IMA ADPCM (encoding 1) at ' + SOUND_RATE + ' Hz');
      if (!(kind >= 0 && kind < HOUR_PATTERNS.length))
        throw new Error(where + '.kind: ' + kind + ' is not an hour pattern (' +
          HOUR_PATTERNS.join('/') + ', 0..' + (HOUR_PATTERNS.length - 1) + ')');
    } else if (kind && cls !== CLASS_BANK && cls !== CLASS_AUX) {
      throw new Error(where + '.kind: ' + kind + ' on a ' + CLASS_NAMES[cls] + ' row — how a row ' +
        'draws or moves is a driven element’s question, and this class answers it with its slot');
    }
    // radar-plan §2.11: on a VALUES row src is the app's layout version (theme_schema.check_element).
    if (cls === CLASS_VALUES) {
      if (!(src >= 1 && src <= 255))
        throw new Error(where + '.src: values of layout version ' + src + ' — the app\u2019s editor stamps its layout from 1 to 255');
      if ((rec.slot | 0) !== 0)
        throw new Error(where + '.slot: values at slot ' + (rec.slot | 0) + ' — a theme carries one values blob, at slot 0');
      if ((rec.x | 0) || (rec.y | 0) || (rec.w | 0) || (rec.h | 0))
        throw new Error(where + ': values at (' + (rec.x | 0) + ',' + (rec.y | 0) + ') sized ' + (rec.w | 0) + 'x' + (rec.h | 0) +
          ' — values place nothing, and their length is their plane\u2019s');
    }
    if (src && cls !== CLASS_BANK && cls !== CLASS_AUX && cls !== CLASS_SOUND && cls !== CLASS_VALUES) {
      throw new Error(where + '.src: ' + src + ' on a ' + CLASS_NAMES[cls] + ' row — what drives ' +
        'a row is a driven element’s question, and this class answers it with its slot');
    }
    /* ---- D42: the moving row. Rule for rule theme_schema.check_element and
     * orb_clock3.c's row_check, in their words: the browser refuses the shapes the dial
     * would refuse, so an author never spends an upload learning what the host knew. */
    var cad = Math.trunc(rec.cadence | 0), carg = Math.trunc(rec.cad_arg | 0),
        ease = Math.trunc(rec.ease | 0);
    if (cls !== CLASS_AUX) {
      D42_COLUMNS.forEach(function (c) {
        if (Math.trunc(rec[c] | 0))
          throw new Error(where + '.' + c + ': ' + rec[c] + ' on a ' + CLASS_NAMES[cls] +
            ' row — a row’s poses are a moving row’s question (class aux)');
      });
      // a font's vmin and vmax are its colours (theme_schema.font_colours)
      if (cls !== CLASS_SOUND && cls !== CLASS_TEXT && ['span', 'zero', 'vmin', 'vmax'].some(function (c) { return Math.trunc(rec[c] | 0); }))
        throw new Error(where + ': span, zero, vmin and vmax are a moving row’s poses, and class ' +
          q(CLASS_NAMES[cls]) + ' does not move — every row that moves is an AUX row (D42)');
    }
    if (cls === CLASS_AUX) {
      var span = Math.trunc(rec.span | 0), zero = Math.trunc(rec.zero | 0);
      var vmin = Math.trunc(rec.vmin >>> 0), vmax = Math.trunc(rec.vmax >>> 0);
      var dx = Math.trunc(rec.dx | 0), dy = Math.trunc(rec.dy | 0);
      var end = Math.trunc(rec.end | 0), digit = Math.trunc(rec.digit | 0);
      if (!(kind >= 0 && kind < SHAPES.length))
        throw new Error(where + '.kind: ' + kind + ' is not a row’s shape (' + SHAPES.join('/') + ')');
      if (kind === SHAPE_TEXT) checkLabel(rec, where);
      else if (Math.trunc(rec.str | 0))
        throw new Error(where + '.str: ' + rec.str + ' — only a text row names fonts');
      if (!(src >= 0 && src < SOURCES.length))
        throw new Error(where + '.src: ' + src + ' is not a value this dial can read (0..' +
          (SOURCES.length - 1) + ')');
      if ((SOURCES[src] === 'text' || SOURCES[src] === 'value') && kind !== SHAPE_TEXT)
        throw new Error(where + '.src: ' + q(SOURCES[src]) + ' is no input of this dial, and a row ' +
          'that moves by it would never move');
      if (Math.trunc(rec.degrees | 0))
        throw new Error(where + '.degrees: ' + rec.degrees + ' — a row’s angle at rest is its `zero`');
      if (kind === SHAPE_TEXT) {
        // checkLabel said it all
      } else if (SOURCES[src] === 'none') {
        MOTION_COLUMNS.concat(['cadence', 'cad_arg', 'ease']).forEach(function (c) {
          if (Math.trunc(rec[c] | 0))
            throw new Error(where + '.' + c + ': ' + rec[c] + ' on a still row — a row with no source ' +
              'never moves, so it names no motion');
        });
      } else {
        if (!cad)
          throw new Error(where + '.cadence: a row moving by ' + q(SOURCES[src]) + ' names when it moves ' +
            '(sweep, step, jump, stop-to-go, drag)');
        if (!(end >= 0 && end < ENDS.length))
          throw new Error(where + '.end: ' + end + ' is not an end rule (' + ENDS.join('/') + ')');
        if (!(digit >= 0 && digit < DIGITS.length))
          throw new Error(where + '.digit: ' + digit + ' is not a digit selector (' + DIGITS.join('/') + ')');
        if (vmax <= vmin)
          throw new Error(where + '.vmax: ' + vmax + ' is not above vmin ' + vmin +
            ' — a domain has to hold at least one unit to divide');
        if (!(span || dx || dy))
          throw new Error(where + ': a moving row that neither turns nor travels goes nowhere; ' +
            'bake it as a still row');
        if (Math.abs(zero) > TURN_10 || Math.abs(span) > TURN_10)
          throw new Error(where + ': zero ' + zero + ' and span ' + span + ' stay within one turn (±' +
            TURN_10 + ' tenths)');
        if (Math.abs(dx) > PANEL_PX || Math.abs(dy) > PANEL_PX)
          throw new Error(where + ': a travel of (' + dx + ',' + dy + ') px cannot stay on a ' +
            PANEL_PX + ' px panel');
        if (end === END_WRAP && kind !== SHAPE_DRUM && (Math.abs(span) !== TURN_10 || dx || dy))
          throw new Error(where + '.end: a wrap starts the next unit at P0, so P1 must look like P0 ' +
            '— a whole turn (±' + TURN_10 + ') and no travel, not span ' + span + ' and (' +
            dx + ',' + dy + ')');
      }
      if (kind === SHAPE_DRUM) {
        // D43: theme_schema.check_element's drum, rule for rule (and orb_clock3.c row_check's)
        var radius = Math.trunc(rec.radius | 0), sh = Math.trunc(rec.h | 0);
        if (SOURCES[src] === 'none') throw new Error(where + ': a drum that never rolls is plate art');
        if (span || zero || dx) throw new Error(where + ': a drum only rolls; it neither turns nor travels across');
        if (!(radius >= 2 && radius <= PANEL_PX / 2)) throw new Error(where + '.radius: a drum of radius ' + radius + ' needs 2..' + (PANEL_PX / 2) + ' px');
        if (Math.abs(dy) !== sh) throw new Error(where + '.dy: a drum rolls ' + dy + ' px over a strip ' + sh + ' tall; one turn is the whole strip');
        if (end !== END_WRAP) throw new Error(where + '.end: a drum turns on past its last figure to its first (wrap)');
        if (nf !== 1 || (flags & AUX_F_SHADOW3)) throw new Error(where + ': a drum is one strip, with no frames and no shadow');
      }
      var pw = Math.trunc(rec.w | 0), ph = Math.trunc(rec.h | 0);
      var ppx = Math.trunc(rec.pivot_x | 0), ppy = Math.trunc(rec.pivot_y | 0);
      if (pw && ph && !(ppx >= 0 && ppx < pw && ppy >= 0 && ppy < ph))
        throw new Error(where + ': a row’s pivot (' + ppx + ',' + ppy + ') sits outside its own ' +
          pw + 'x' + ph + ' plate, so it would turn about a point off the glass');
      if (nf > 1 && Math.trunc(rec.delay | 0) && nf > ROW_FRAMES_MAX)
        throw new Error(where + ': an animated row carries at most ' + ROW_FRAMES_MAX + ' frames, not ' + nf);
      if (nf > 1 && !Math.trunc(rec.delay | 0) && nf > CLK_PICK_MAX)
        throw new Error(where + ': a row offers the app at most ' + CLK_PICK_MAX + ' frames to pick from, not ' + nf);
      if ((flags & AUX_F_SHADOW3) && (nf !== 1 || src || cad))
        throw new Error(where + ': a shadow twin takes its hand’s whole pose — one plate, ' +
          'no source, no cadence');
    }
    // D41 §2.2, row-local (theme_schema.check_element, rule for rule).
    if (cad || carg || ease) {
      if (cls !== CLASS_AUX && cls !== CLASS_BANK)
        throw new Error(where + '.cadence: when a row moves is a driven row’s question (a moving ' +
          'row, a bank); class ' + q(CLASS_NAMES[cls]) + ' does not move by a value');
      if (!(cad >= 0 && cad < CADENCES.length))
        throw new Error(where + '.cadence: ' + cad + ' is not a cadence (' + CADENCES.join('/') +
          ', 0..' + (CADENCES.length - 1) + ')');
      var eff = SOURCES[src] || 'none';
      var cname = CADENCES[cad];
      if (cad === CAD_STEP && carg < 1)
        throw new Error(where + '.cad_arg: step 0 divides a unit into no steps — name how many per unit');
      if ((cad === CAD_NONE || cad === CAD_JUMP || cad === CAD_ROLL) && carg)
        throw new Error(where + '.cad_arg: ' + carg + ' on a ' + cname + ' row, which takes no argument');
      if (cad === CAD_SWEEP && carg > SWEEP_FPS_MAX)
        throw new Error(where + '.cad_arg: a sweep at ' + carg + ' frames a second — the panel draws ' +
          SWEEP_FPS_MAX + ' at most');
      if (cad === CAD_STOP_TO_GO && eff !== 'second')
        throw new Error(where + '.cadence: stop-to-go is a seconds hand’s rhythm, and this row reads ' + q(eff));
      if (cad === CAD_DRAG && CAD_DRAG_SOURCES.indexOf(eff) < 0)
        throw new Error(where + '.cadence: drag holds a calendar row (' + CAD_DRAG_SOURCES.join('/') +
          '), and this row reads ' + q(eff));
      if (cls === CLASS_BANK && cad !== CAD_NONE && cad !== CAD_JUMP)
        throw new Error(where + '.cadence: a complication’s plates are whole values, so it jumps — ' +
          cname + ' waits for a ring the dial can turn');
      if (ease) {
        var efly = cls === CLASS_AUX && Math.trunc(rec.end | 0) === END_FLYBACK;
        var esteps = (cad === CAD_JUMP || cad === CAD_STEP) && eff !== 'second' && cls !== CLASS_BANK;
        if (ease > 1000) throw new Error(where + '.ease: ' + ease + ' ms — an ease runs at most a second');
        if (!efly && !esteps)
          throw new Error(where + '.ease: ' + ease + ' ms with nothing to ease — a settle needs a jump or a ' +
            'step slower than the second, a flyback a flyback row');
      }
    }
    if (cls === CLASS_BANK) {
      if (!(kind >= 0 && kind < COMP_KINDS.length)) {
        throw new Error(where + '.kind: ' + kind + ' is not a way to draw plates (0..' +
          (COMP_KINDS.length - 1) + ')');
      }
      if (!(src >= 0 && src < SOURCES.length)) {
        throw new Error(where + '.src: ' + src + ' is not a value this dial can read (0..' +
          (SOURCES.length - 1) + ')');
      }
      if (kind === 0 && CLOCK_WINDOW_DOMAIN.hasOwnProperty(SOURCES[src])) {
        var dom = CLOCK_WINDOW_DOMAIN[SOURCES[src]];
        if (nf !== dom[1]) {
          throw new Error(where + ': a window driven by ' + q(SOURCES[src]) + ' indexes ' + dom[1] +
            ' plates, and the row carries ' + nf);
        }
      }
    }
    // C4's delta columns, refused under either stamp. The stamp gate itself is
    // checkClockStamp's (and orb_clock3.c's) business; these hold whenever the
    // columns ride, because a v3 pack refuses them at the door before any apply.
    var par = Math.trunc((rec.parent === undefined || rec.parent === null) ? PARENT_NONE : rec.parent);
    if (par !== PARENT_NONE) {
      if (par === 0) {
        throw new Error(where + '.parent: 0 is the plate\u2019s own row tag, and a parent ' +
          'of none is ' + PARENT_NONE + ' (0x' + PARENT_NONE.toString(16) + ') — refused, not guessed');
      }
      if (cls !== CLASS_AUX && cls !== CLASS_BANK) {
        throw new Error(where + '.parent: class ' + q(CLASS_NAMES[cls]) +
          ' draws, it is not hung from — only a pointer or a complication names a parent');
      }
    }
    if (Math.trunc(rec.delay | 0) && nf < 2) {
      throw new Error(where + '.delay: ' + (rec.delay | 0) + ' ms on a row that carries ' + nf +
        ' frame — a tempo nobody will keep is a bake that lost its animation');
    }
    return rec;
  }

  /* Whole-table rules: the ones needing two rows to state. An app resolves an element by
   * (class, slot), so two rows claiming one slot leave it painting one on a coin flip. */
  function checkTable(recs) {
    if (recs.length > ELEM_MAX) {
      throw new Error('element table holds ' + recs.length + ' rows, past the bound of ' + ELEM_MAX);
    }
    var seenN = {}, seenSlot = {};
    for (var i = 0; i < recs.length; i++) {
      checkElement(recs[i], 'elem[' + i + ']');
      var n = Math.trunc(recs[i].n | 0);
      var key = Math.trunc(recs[i].class | 0) + ':' + Math.trunc(recs[i].slot | 0);
      if (seenN.hasOwnProperty(n)) {
        throw new Error('elem[' + i + ']: row identity ' + n + ' repeats the row at index ' + seenN[n]);
      }
      if (seenSlot.hasOwnProperty(key)) {
        throw new Error('elem[' + i + ']: two rows claim class ' + q(CLASS_NAMES[Math.trunc(recs[i].class | 0)]) +
          ' slot ' + Math.trunc(recs[i].slot | 0) + ' (rows ' + seenSlot[key] + ' and ' + i +
          '), and the app cannot paint both');
      }
      seenN[n] = i; seenSlot[key] = i;
    }
    // C4's parent graph: a claim names a row that exists, never itself, at most two
    // links deep, and never a cycle — the refusals orb_clock3.c repeats on the bytes.
    for (i = 0; i < recs.length; i++) {
      var par = Math.trunc((recs[i].parent === undefined || recs[i].parent === null) ? PARENT_NONE : recs[i].parent);
      if (par === PARENT_NONE) continue;
      if (par === Math.trunc(recs[i].n | 0)) {
        throw new Error('elem[' + i + ']: row ' + (recs[i].n | 0) + ' names itself as its parent');
      }
      var hops = 0, cur = Math.trunc(recs[i].n | 0), seen = {};
      for (;;) {
        if (seen.hasOwnProperty(cur)) {
          throw new Error('elem[' + i + ']: the parent chain of row ' + (recs[i].n | 0) + ' is a cycle');
        }
        seen[cur] = 1;
        var j = seenN.hasOwnProperty(par) ? seenN[par] : -1;
        if (j < 0) {
          throw new Error('elem[' + i + ']: row ' + cur + ' names parent ' + par +
            ' and no row in this table carries that tag');
        }
        hops++;
        if (hops > 2) {
          throw new Error('elem[' + i + ']: row ' + cur + ' hangs three deep; the compositor carries two links');
        }
        cur = Math.trunc(recs[j].n | 0);
        par = Math.trunc((recs[j].parent === undefined || recs[j].parent === null) ? PARENT_NONE : recs[j].parent);
        if (par === PARENT_NONE) break;
      }
    }
    return recs;
  }

  /* Checked pack of a whole table: real rows only, no padding to the cap, and a missing
   * key is the bake's bug rather than a zero — theme_schema.pack_elements' order. The
   * clock's v6 width (abi 6, the default) writes all 33 columns; an app section's v3
   * width writes 23 and refuses a claim past them rather than silently truncating it. */
  function packElements(recs, cap, abi) {
    cap = (cap === undefined) ? ELEM_MAX : cap;
    abi = (abi === undefined) ? GEOM_ABI6 : abi;
    checkTable(recs);
    if (recs.length > cap) {
      throw new Error('element table: ' + recs.length + ' rows exceed the cap of ' + cap);
    }
    var words = abi === GEOM_ABI6 ? ELEMENT_WORDS : ELEMENT_WORDS_V3;
    var out = [];
    for (var i = 0; i < recs.length; i++) {
      var r = recs[i];
      if (words === ELEMENT_WORDS_V3) {
        var par = (r.parent === undefined || r.parent === null) ? PARENT_NONE : r.parent;
        var later = ELEMENT_FIELDS.slice(ELEMENT_WORDS_V3).map(function (f) { return f[0]; })
          .filter(function (c) { return c !== 'parent' && Math.trunc(r[c] | 0); });
        if (par !== PARENT_NONE || later.length)
          throw new Error('elem[' + i + '] names ' + (later.join(', ') || 'parent ' + par) +
            ' at stride ' + ELEMENT_WORDS_V3 + ': an app row is 23 words and carries none of ' +
            'the clock’s appended columns');
      }
      for (var k = 0; k < words; k++) {
        var name = ELEMENT_FIELDS[k][0], t = ELEMENT_FIELDS[k][1];
        var v = (name in r) ? r[name] : (name === 'parent' ? PARENT_NONE : 0);
        out.push(val('elem[' + i + '].' + name, t, v));
      }
    }
    return out;
  }
  /* The beat a v6 dial keeps, from its rows (theme_schema.clock_tempo, orb_clock3.c):
   * [sweep, rate]. A sweeping seconds row asks for its own frame rate (cad_arg) when it
   * names one, else the fastest animation the dial carries, else SWEEP_TIER3. */
  function clockTempo(recs, scalars) {
    var sweep = 0, rate = 0, sweepFps = 0, i, r;
    for (i = 0; i < recs.length; i++) {
      r = recs[i];
      if ((r.class | 0) !== CLASS_AUX || ((r.flags | 0) & EF_HIDDEN)) continue;
      if (SOURCES[r.src | 0] !== 'second' || (r.digit | 0)) continue;
      var cad = r.cadence | 0;
      if (cad === CAD_SWEEP || cad === CAD_STOP_TO_GO) sweep = 1;
      if (cad === CAD_SWEEP) sweepFps = Math.max(sweepFps, r.cad_arg | 0);
      else if (cad === CAD_STEP) rate = Math.max(rate, r.cad_arg | 0);
    }
    if (!sweep) return [0, rate || 1];
    if (sweepFps) return [1, sweepFps];
    var sc = scalars || {}, fps = 0;
    for (i = 0; i < recs.length; i++) {
      r = recs[i];
      var d = r.delay | 0;
      if ((r.class | 0) === CLASS_PLATE && (r.slot | 0) === 0 && (r.n_frames | 0) > 1) {
        if ((sc.bg_fps | 0) > 0) fps = sc.bg_fps | 0;          // the rate its author named
      }
      if ((r.class | 0) === CLASS_AUX && (r.n_frames | 0) > 1 && d)
        fps = Math.max(fps, Math.floor((1000 + (d >> 1)) / d));
    }
    return [1, Math.min(255, fps >= 2 ? fps : SWEEP_TIER3)];
  }
  /* D42's whole-table rules (theme_schema.check_clock_rows, orb_clock3.c). */
  function checkClockRows(recs, scalars) {
    checkFonts(recs);
    var aux = {}, byN = {}, i, r;
    for (i = 0; i < recs.length; i++) {
      byN[recs[i].n | 0] = recs[i];
      if ((recs[i].class | 0) === CLASS_AUX) aux[recs[i].slot | 0] = recs[i];
    }
    var minuteJumps = Object.keys(aux).some(function (k) {
      var x = aux[k];
      return SOURCES[x.src | 0] === 'minute' && !(x.digit | 0) && (x.cadence | 0) === CAD_JUMP;
    });
    var steps = {}, nSteps = 0, animated = [];
    var slots = Object.keys(aux).map(Number).sort(function (a, b) { return a - b; });
    for (i = 0; i < slots.length; i++) {
      var slot = slots[i], where = 'ptr' + slot;
      r = aux[slot];
      if (slot >= CLK_ROW_MAX)
        throw new Error(where + ': the clock seats ' + CLK_ROW_MAX + ' moving rows (ptr0..ptr' + (CLK_ROW_MAX - 1) + ')');
      var cad = r.cadence | 0;
      if (cad === CAD_STOP_TO_GO && !minuteJumps)
        throw new Error(where + ': stop-to-go rests until the minute releases it, and no minute row jumps');
      if (SOURCES[r.src | 0] === 'second' && cad === CAD_STEP && !(r.digit | 0) && !steps[r.cad_arg | 0]) {
        steps[r.cad_arg | 0] = 1; nSteps++;
      }
      // frames and a frame time: the carriage; frames and none: the app picks, and any number may
      if ((r.n_frames | 0) > 1 && (r.delay | 0)) animated.push(where);
      if ((r.flags | 0) & AUX_F_SHADOW3) {
        var prev = aux[slot - 1];
        if (!prev)
          throw new Error(where + ': a shadow twin with no row before it — the twin takes its pose from the row in the slot under it');
        if ((prev.flags | 0) & AUX_F_SHADOW3)
          throw new Error(where + ': a shadow twin under another twin — a twin darkens a row, not a shadow');
        if ((prev.n_frames | 0) > 1 && !(prev.delay | 0))
          throw new Error(where + ': a shadow twin under a row whose frames the app picks — the twin would shade one frame of many');
      }
      var par = (r.parent === undefined || r.parent === null) ? PARENT_NONE : r.parent;
      if (par !== PARENT_NONE) {
        var p = byN[par];
        if (!p || (p.class | 0) !== CLASS_AUX)
          throw new Error(where + ': names parent ' + par + ', which is not a moving row — a row rides another row’s pose');
        if ((p.span | 0) && ((p.x | 0) + (p.pivot_x | 0) !== (r.x | 0) + (r.pivot_x | 0) ||
                             (p.y | 0) + (p.pivot_y | 0) !== (r.y | 0) + (r.pivot_y | 0)))
          throw new Error(where + ': rides a parent that turns about another centre — a child of a turning row shares its arbour');
      }
    }
    if (nSteps > 1)
      throw new Error('seconds rows step at ' + Object.keys(steps).join('/') + ' a second: a dial keeps one beat');
    if (animated.length > 1)
      throw new Error(animated.join(', ') + ' are all animated: a dial carries one animated row (the carriage)');
    return clockTempo(recs, scalars);
  }
  function unpackElements(words, n, stride) {
    // The reader honours the stride the BLOB carries, not the width this build writes:
    // the same §23.5 rule orb_bundle.c's orb_bundle_elem_stride() and theme_schema's own
    // stride argument implement. D34 widened the row from 19 words to 23, and a bundle
    // baked before it still has to read — columns past a narrow row's width land on the
    // zeros they were written after, which is the whole point of appending rather than
    // renumbering. A stride narrower than the dialect has ever written is a torn file.
    var w = stride || ELEMENT_WORDS;
    if (w < ELEMENT_WORDS_MIN || w > 0xFFFF) {
      throw new Error('elem: row stride of ' + w + ' words is not a width this dialect ' +
        'has ever written (' + ELEMENT_WORDS_MIN + '..' + ELEMENT_WORDS + ')');
    }
    if (words.length < n * w) {
      throw new Error('elem: blob holds ' + words.length + ' words, too few for ' + n +
        ' records of ' + w);
    }
    var out = [];
    for (var i = 0; i < n; i++) {
      var r = blankElement(), base = i * w;
      for (var k = 0; k < w && k < ELEMENT_WORDS; k++) {
        r[ELEMENT_FIELDS[k][0]] = sext(words[base + k] >>> 0, ELEMENT_FIELDS[k][1]);
      }
      out.push(r);
    }
    return out;
  }

  /* check_slug, and the D29 budget gate in the device's own sums: the boundary is what
   * the INSTALL asks for, not the file's shape (§21's lesson). Geometry installs at slot
   * +0x1000 with a sector-aligned erase, so 61440 B ends exactly on the banks at
   * +0x00010000 and == cap is legal; the payload has SLOT_CAP - SLOT_ART_OFF and the
   * trailing pad never installs, so == cap is legal there too. The sentences are
   * orb_theme.c's copy_into words, which the composer shows and the reader and the device
   * repeat verbatim (D26): a bake that emits what the dial refuses spends an upload and an
   * erase to teach what the host already knew. */
  var SLUG_RE = /^[a-z0-9_]+$/;   // the same rule on bytes: read it before decoding it
  function checkSlug(slug) {
    // Length and charset share one sentence, as they do in check_slug: a slug of the wrong
    // LENGTH is the same mistake to the caller as one of the wrong characters, and the
    // byte-length check below is pack_blob's separate, later promise about the wire.
    if (typeof slug !== 'string' || slug.length < 1 || slug.length > SLUG_MAX || !SLUG_RE.test(slug)) {
      throw new Error('bundle slug ' + q(slug) + ' is not lowercase a-z 0-9 _ of 1..' +
        SLUG_MAX + ' chars');
    }
    var n = utf8(slug).length;
    if (n < 1 || n > SLUG_MAX) {
      throw new Error('bundle slug ' + q(slug) + ' is ' + n + ' bytes, outside 1..' + SLUG_MAX);
    }
    return slug;
  }
  function checkBudget3(regionLen, artLen) {
    if (regionLen > GEOM_CAP3) {
      throw new Error('geometry region is ' + regionLen + ' B; the dial\u2019s table area ' +
        'holds ' + GEOM_CAP3 + ' B — geometry would overrun the pixels');
    }
    if (artLen > SLOT_ART_CAP3) {
      throw new Error('pixel payload is ' + artLen + ' B; a theme slot holds ' + SLOT_ART_CAP3 +
        ' B — theme is larger than a slot');
    }
  }

  /* One section's geometry blob, self-describing from its count head, exactly as
   * orbbundle.pack_blob lays it out: 4 count words, the 26 scalars (the clock dialect,
   * absent ones zero — D31), the element table, the sprite catalogue, then the slug bytes
   * and their NUL pad. The elements pass check_table HERE, so an element a theme cannot
   * even name never reaches a card. */
  function packBlob3(slug, elements, scalars, sprites, abi) {
    var elems = (elements || []).slice();
    checkTable(elems);
    abi = (abi === undefined) ? GEOM_ABI3 : abi;
    if (slug === 'clock') {
      if (abi !== GEOM_ABI6)
        throw new Error('a clock section stamped geometry v' + abi + '; the clock writes v' +
          GEOM_ABI6 + ' and nothing older (D42)');
      checkClockRows(elems, scalars);
      CLOCK_RETIRED_SCALARS.forEach(function (n) {
        if ((scalars || {})[n] | 0)
          throw new Error('clock scalar ' + q(n) + ' is ' + scalars[n] + ': D42 retired it, and a v6 ' +
            'section carries it as 0 — the rows say what it said');
      });
    }
    var delta = abi === GEOM_ABI6;
    var sb = utf8(slug);
    if (sb.length < 1 || sb.length > SLUG_MAX) {
      throw new Error('bundle slug ' + q(slug) + ' is ' + sb.length + ' bytes, outside 1..' + SLUG_MAX);
    }
    var scal = new Array(SCALARS_N).fill(0), name, i, k, hit;
    for (name in (scalars || {})) {
      hit = -1;
      for (i = 0; i < THEME_FIELDS.length; i++) if (THEME_FIELDS[i][0] === name) { hit = i; break; }
      if (hit < 0) {
        throw new Error('blob ' + q(slug) + ' names scalar ' + q(name) + ' the dialect does not carry');
      }
      scal[hit] = val(slug + '.scalars.' + name, THEME_FIELDS[hit][1], scalars[name]);
    }
    var sprWords = [], s = sprites || [];
    for (i = 0; i < s.length; i++) {
      for (k = 0; k < 4; k++) {
        var f = ['off', 'size', 'w', 'h'][k];
        if (!(f in s[i])) throw new Error('blob ' + q(slug) + ' sprite[' + i + '] is missing ' + q(f));
        sprWords.push(val(slug + '.sprite[' + i + '].' + f, 'u32', s[i][f]));
      }
      for (k = 0; k < 2; k++) {
        var df = ['diffx', 'diffy'][k], dv = s[i][df] | 0;
        if (!delta) {
          if (dv) throw new Error('blob ' + q(slug) + ' sprite[' + i + '] names ' + df + ' ' + dv +
            ' at geometry v' + abi + ': the v3 catalogue row has no place for a delta rect');
          continue;
        }
        sprWords.push(val(slug + '.sprite[' + i + '].' + df, 'i16', dv));
      }
    }
    if (elems.length > 0xFFFF || sprWords.length / (delta ? SPR_WORDS2 : SPR_WORDS) > 0xFFFF) {
      throw new Error('blob ' + q(slug) + ' outgrew its u16 counts');
    }
    var words = [elems.length, sprWords.length / (delta ? SPR_WORDS2 : SPR_WORDS), SCALARS_N, sb.length].concat(scal);
    if (elems.length) words = words.concat(packElements(elems, ELEM_MAX, abi));
    words = words.concat(sprWords);
    var out = wordsToBytes(words), pad = (-sb.length) & 3;
    var all = new Uint8Array(out.length + sb.length + pad);
    all.set(out, 0);
    all.set(sb, out.length);
    return all;
  }
  function unpackBlob3(blob, abi) {
    if (blob.length < BLOB_HEAD * 4) {
      throw new Error('section blob is shorter than its counts and scalars');
    }
    abi = (abi === undefined) ? GEOM_ABI3 : abi;
    var delta = abi === GEOM_ABI6;
    var sw = delta ? SPR_WORDS2 : SPR_WORDS;
    var v = new DataView(blob.buffer, blob.byteOffset, blob.length);
    var nElem = v.getUint32(0, true), nSpr = v.getUint32(4, true);
    var nScal = v.getUint32(8, true), nSlug = v.getUint32(12, true);
    if (nScal !== SCALARS_N) {
      throw new Error('blob carries ' + nScal + ' scalars; this dialect has ' + SCALARS_N);
    }
    var wantMin = (4 + nScal + nSpr * sw) * 4 + nSlug + ((-nSlug) & 3);
    // §23.5's append-only rule, mirrored from orbbundle.unpack_blob and
    // orb_bundle.c's blob_check in the same order and the same words: the row
    // width is the blob's own number, so the leftover bytes after the fixed
    // regions must divide into whole nElem rows of a width this dialect wrote.
    if (blob.length < wantMin) {
      throw new Error('blob is ' + blob.length + ' bytes; its counts say at least ' +
        wantMin + ' — a torn file, not a short one');
    }
    // A table with no rows carries no width: it reads as the width its stamp writes.
    var eb = blob.length - wantMin,
        elemWords = delta ? ELEMENT_WORDS : ELEMENT_WORDS_V3;
    if (nElem === 0 ? eb !== 0 : eb % (nElem * 4) !== 0) {
      throw new Error('blob is ' + blob.length + ' bytes; its ' + nElem + ' rows leave ' +
        eb + ' B, no whole number of words — a torn file, not a short one');
    }
    if (nElem) {
      elemWords = eb / (nElem * 4);
      if (elemWords < ELEMENT_WORDS_MIN || elemWords > 0xFFFF) {
        throw new Error('element rows are ' + elemWords + ' words; this dialect has written ' +
          ELEMENT_WORDS_MIN + ' at the narrowest and 65535 at the widest a reader may skip ' +
          'whole columns in');
      }
    }
    if (nElem > 0xFFFF || nSpr > 0xFFFF) throw new Error('blob outgrew its u16 counts');
    var scal = [], words = [], i;
    for (i = 0; i < nScal; i++) scal.push(v.getUint32(16 + i * 4, true));
    for (i = 0; i < nElem * elemWords; i++) words.push(v.getUint32(BLOB_HEAD * 4 + i * 4, true));
    var elems = nElem ? unpackElements(words, nElem, elemWords) : [];
    // Decision B, mirrored: a v3 stamp's zero parent column carried no claim and
    // normalizes to the NONE the loader hands the compositor anyway.
    if (!delta) {
      for (i = 0; i < elems.length; i++) {
        var p0 = (elems[i].parent === undefined || elems[i].parent === null) ? PARENT_NONE : elems[i].parent;
        if (p0 === 0) elems[i].parent = PARENT_NONE;
      }
    }
    var off = (BLOB_HEAD + nElem * elemWords) * 4, sprs = [];
    for (i = 0; i < nSpr; i++) {
      var rec = { off: v.getUint32(off + i * sw * 4, true), size: v.getUint32(off + i * sw * 4 + 4, true),
                  w: v.getUint32(off + i * sw * 4 + 8, true), h: v.getUint32(off + i * sw * 4 + 12, true) };
      if (delta) {
        rec.diffx = sext(v.getUint32(off + i * sw * 4 + 16, true) & 0xFFFF, 'i16');
        rec.diffy = sext(v.getUint32(off + i * sw * 4 + 20, true) & 0xFFFF, 'i16');
      }
      sprs.push(rec);
    }
    // §5.2's plate materialization, mirrored from orb_clock3.c's gif_io walk so a
    // delta row's rect_count and first rect's box reach the caller beside the row.
    // A row with no delta frames reads rect_count 0 and the plate's own box.
    if (delta) {
      for (i = 0; i < elems.length; i++) {
        var e = elems[i], nf = e.n_frames | 0;
        e.rect_count = 0; e.diffw = 0; e.diffh = 0;
        if (nf > 1) {
          for (var f = 1; f < nf; f++) {
            var sp = sprs[(e.frame0 | 0) + f];
            if (!sp || !sp.size) continue;             // a still row: applies nothing
            e.rect_count++;
            if (!e.diffw) { e.diffw = sp.w; e.diffh = sp.h; }
          }
          if (e.flags & 64) e.rect_count++;   // §37: the closing rect, one past the window
        }
      }
    }
    off += nSpr * sw * 4;
    // The slug travels as BYTES, because the device reader memcpys them without decoding
    // and a forged blob can hold anything at all. Latin-1 keeps the round trip lossless
    // and byte-reversible, which is the reader's only promise here.
    var sb = blob.subarray(off, off + nSlug), s = '';
    for (i = 0; i < sb.length; i++) s += String.fromCharCode(sb[i]);
    var sc = {};
    for (i = 0; i < SCALARS_N; i++) sc[THEME_FIELDS[i][0]] = sext(scal[i], THEME_FIELDS[i][1]);
    return { slug: s, slug_b: sb, elements: elems, sprites: sprs, scalars: sc,
             elem_words: elemWords, spr_words: sw, abi: abi };
  }
  /* The clock's stamp gate, mirrored from theme_schema.check_clock_stamp: v6 and
   * nothing older (D42), at exactly the v6 row and the six-word delta catalogue. */
  function checkClockStamp(rec) {
    var abi = rec.abi_max === undefined ? GEOM_ABI3 : rec.abi_max;
    var ew = rec.elem_words === undefined ? ELEMENT_WORDS : rec.elem_words;
    var spw = rec.spr_words || SPR_WORDS2;
    if (abi !== GEOM_ABI6)
      throw new Error('a clock section stamped geometry v' + abi + '; this reader’s clock ' +
        'dialect is v' + GEOM_ABI6 + ' and nothing older (D42) — re-bake it');
    if (ew !== ELEMENT_WORDS)
      throw new Error('clock rows are ' + ew + ' words under geometry v' + abi +
        '; that stamp writes exactly ' + ELEMENT_WORDS);
    if (spw !== SPR_WORDS2)
      throw new Error('clock catalogue rows are ' + spw + ' words under geometry v' + abi +
        '; that stamp writes 6');
  }


  /* The index region: a two-word head, then one fixed 40-byte record per section.
   * IDX_ORDER / IDX_OFF / IDX_TYPES are orbbundle.IDX_ORDER and its "<4H7I2H" wire,
   * spelled out as offsets because this record is where an assumed alignment bites: the
   * record leads with FOUR u16s (slug_off, slug_len, abi_min, abi_max), so n_elem sits at
   * 8 and not at 4, and a reader that assumes a u32 first reads every later field eight
   * bytes early. The Go gate shipped that bug and the Python fixture caught it.
   *
   * geom_off is stored although the walk derives it: the reader requires stored ==
   * derived, so a blob moved by a repack tool says where to look and a torn file
   * contradicts itself out loud. */
  var IDX_ORDER = ['slug_off', 'slug_len', 'abi_min', 'abi_max', 'n_elem', 'geom_off',
                   'geom_size', 'geom_crc', 'art_off', 'art_size', 'art_crc', 'scr_w', 'scr_h'];
  var IDX_OFF   = [0, 2, 4, 6, 8, 12, 16, 20, 24, 28, 32, 36, 38];
  var IDX_TYPES = ['u16', 'u16', 'u16', 'u16', 'u32', 'u32', 'u32', 'u32',
                   'u32', 'u32', 'u32', 'u16', 'u16'];
  function packIndex3(recs) {
    var words = [recs.length, 0];
    for (var i = 0; i < recs.length; i++) {
      var row = new Array(IDX_WORDS).fill(0);
      for (var k = 0; k < IDX_ORDER.length; k++) {
        var f = IDX_ORDER[k];
        if (!(f in recs[i])) throw new Error('index[' + i + '].' + f + ' is missing from the record');
        var w = val('index[' + i + '].' + f, IDX_TYPES[k], recs[i][f]);
        row[IDX_OFF[k] >> 2] = (IDX_OFF[k] & 2) ? (row[IDX_OFF[k] >> 2] | (w << 16))
                                                : (row[IDX_OFF[k] >> 2] | w);
      }
      words = words.concat(row);
    }
    return wordsToBytes(words);
  }
  function unpackIndex3(region) {
    var v = new DataView(region.buffer, region.byteOffset, region.length);
    var n = v.getUint32(0, true);
    if (!(n >= 1 && n <= SECT_MAX)) {
      throw new Error('index claims ' + n + ' sections; a bundle holds 1..' + SECT_MAX);
    }
    if (region.length < 8 + n * IDX_WORDS * 4) {
      throw new Error('index claims ' + n + ' sections in ' + region.length + ' region bytes — torn file');
    }
    var recs = [];
    for (var i = 0; i < n; i++) {
      var o = 8 + i * IDX_WORDS * 4, r = {};
      for (var k = 0; k < IDX_ORDER.length; k++) {
        r[IDX_ORDER[k]] = (IDX_TYPES[k] === 'u16') ? v.getUint16(o + IDX_OFF[k], true)
                                                   : v.getUint32(o + IDX_OFF[k], true);
      }
      recs.push(r);
    }
    return { n: n, recs: recs };
  }

  /* FORMAT 2's header stamped FORMAT 3: the same 128 bytes and the same crc span, with
   * word 9 carrying n_sections where a v2 container kept scr_w. */
  function packHeader3(o) {
    var hdr = new Array(HDR_WORDS).fill(0);
    hdr[0] = MAGIC >>> 0; hdr[1] = FORMAT3; hdr[2] = o.abi | 0; hdr[3] = HDR_WORDS * 4;
    hdr[4] = HDR_WORDS * 4;
    // geom_size/geom_crc/art_size/art_crc/n_sections come out of the writer's own sums and
    // are u32 by construction; serial and baked_ms are the caller's, and pack_header3 stamps
    // them with _u(), which WRAPS. A wall-clock stamp (Date.now() is past 1.7e12 in 2026)
    // must wrap here exactly as it does in Python, so this masks rather than refusing.
    hdr[5] = (o.geomSize >>> 0); hdr[6] = (o.geomCrc >>> 0);
    hdr[7] = (o.artSize >>> 0);  hdr[8] = (o.artCrc >>> 0);
    // word 10 (FORMAT 2's scr_h) is the bake time in Unix seconds, 0 when the caller gives
    // none: word 12's millisecond stamp wraps every 49.7 days and cannot order two bakes
    hdr[9] = (o.nSections >>> 0); hdr[10] = (o.bakedS >>> 0);
    hdr[11] = (o.serial >>> 0);
    hdr[12] = (o.bakedMs >>> 0);
    hdr[13] = (o.artOff >>> 0);
    setWord(hdr, W_NAME, o.name, NAME_MAX);
    if (o.designSize) { hdr[W_DESIGN_OFF] = o.designOff >>> 0; hdr[W_DESIGN_SIZE] = o.designSize >>> 0; hdr[W_DESIGN_CRC] = o.designCrc >>> 0; }
    hdr[W_HDR_CRC] = hdrCrc(hdr);
    return wordsToBytes(hdr);
  }

  /* Emit one FORMAT 3 bundle, byte for byte what tools/orbbundle.py's write_bundle
   * writes. Slug order is normalised here: a person reading an inspector should not have
   * to know the bake's build order, and the reader must find a slug the same way twice.
   *
   * Pass 1 sizes the region; pass 2 stamps the records once every offset exists, because
   * a record's blob offset is where its own name lives. The budget gate is the INSTALL
   * sum and not the file's shape, and it fires before a byte of art is encoded.
   *
   * o = {name, sections: [{slug, elements, scalars, sprites, art, abi_min, abi_max,
   * scr_w, scr_h}], serial, bakedMs}. Section keys are orbbundle.write_bundle's own names,
   * so the vectors a mirror test feeds this function are the vectors it feeds the Python
   * with no translation layer to drift. `art` is a Uint8Array of already-encoded pixels:
   * the baker's plane encoders fill it, and this function looks inside it no further than
   * the sprite ranges. */
  function writeBundle3(o) {
    var sections = (o.sections || []).slice();
    if (!o.name) throw new Error('a bundle needs a name: it is the theme_id input (R4)');
    var seen = {}, s, i;
    for (i = 0; i < sections.length; i++) {
      var slug = checkSlug(sections[i].slug);
      if (seen.hasOwnProperty(slug)) {
        throw new Error('bundle lists app ' + q(slug) + ' twice; one slug resolves to one ' +
          'section and cannot hold two answers');
      }
      seen[slug] = 1;
    }
    // A FORMAT 3 file themes exactly one app: the card stem embeds that app's slug,
    // so a second section is a file no single owner could install or evict, and the
    // device's door (orb_theme.c's one_section_why) refuses it. This writer cannot
    // produce one. The WIRE still reads 1..8 (unpackIndex3 above keeps parse_core's
    // width, because a multi-section file stays a valid REJECTION vector).
    if (sections.length !== 1) {
      throw new Error('a theme file carries exactly one section — this one carries ' + sections.length);
    }
    sections.sort(function (a, b) { return a.slug < b.slug ? -1 : (a.slug > b.slug ? 1 : 0); });

    var blobs = [], arts = [];
    for (i = 0; i < sections.length; i++) {
      s = sections[i];
      var art = s.art && s.art.length ? s.art : new Uint8Array(0);
      var sprs = s.sprites || [];
      for (var k = 0; k < sprs.length; k++) {
        if ((sprs[k].off >>> 0) + (sprs[k].size >>> 0) > art.length) {
          throw new Error('section ' + q(s.slug) + ' sprite[' + k + '] runs past its own bank (' +
            sprs[k].off + '+' + sprs[k].size + ' of ' + art.length + ')');
        }
      }
      if (!(s.elements && s.elements.length) && !art.length) {
        throw new Error('section ' + q(s.slug) + ' carries neither elements nor art');
      }
      blobs.push(packBlob3(s.slug, s.elements, s.scalars, sprs,
                           s.abi_max === undefined ? GEOM_ABI3 : s.abi_max));
      arts.push(art);
    }

    var head = 8 + IDX_WORDS * 4 * sections.length, offs = [], run = head;
    for (i = 0; i < blobs.length; i++) { offs.push(run); run += blobs[i].length; }
    var artOff = artOffset(run);
    var recs = [], at = artOff, ends = [], anyArt = false, artJoin = [];
    for (i = 0; i < sections.length; i++) {
      s = sections[i];
      var sb = utf8(s.slug), a = arts[i], blob = blobs[i];
      recs.push({
        slug_off: offs[i] + blob.length - ((sb.length + 3) & ~3), slug_len: sb.length,
        abi_min: s.abi_min === undefined ? GEOM_ABI3 : s.abi_min,
        abi_max: s.abi_max === undefined ? GEOM_ABI3 : s.abi_max,
        n_elem: (s.elements || []).length,
        geom_off: offs[i], geom_size: blob.length, geom_crc: crc32(blob),
        art_off: at, art_size: a.length, art_crc: a.length ? crc32(a) : 0,
        scr_w: s.scr_w === undefined ? 0 : s.scr_w, scr_h: s.scr_h === undefined ? 0 : s.scr_h
      });
      at += a.length;
      ends.push(at);
      at = (at + SECTOR - 1) & ~(SECTOR - 1);
      if (a.length) { anyArt = true; artJoin.push(a); }
    }
    // art_size EXCLUDES the final bank's alignment pad: the installed slot reads art_size
    // bytes and pads nothing. Inter-bank pad is inside, because the walk that finds each
    // bank's start has to cross it.
    var artEnd = 0;
    for (i = 0; i < ends.length; i++) if (arts[i].length && ends[i] > artEnd) artEnd = ends[i];
    var artSize = anyArt ? (artEnd - artOff) : 0;
    var artCrcAll = anyArt ? crc32(concat(artJoin)) : 0;
    checkBudget3(run, artSize);

    var region = concat([packIndex3(recs)].concat(blobs));
    // The header stamps the clock's v6 when a v6 section rides along, else v3 (D42):
    // the two stamps a reader takes, never an app section's own forward aim.
    var hdrAbi = GEOM_ABI3;
    for (i = 0; i < sections.length; i++)
      if (sections[i].abi_max === GEOM_ABI6) hdrAbi = GEOM_ABI6;
    // the design (P7) follows the last bank's last byte; the whole file stays under the doors' cap
    var design = o.design && o.design.length ? o.design : null, end = artOff + artSize;
    if (design && end + design.length > THEME_UPLOAD_MAX) {
      throw new Error('the dial (' + end + ' B) and its design (' + design.length + ' B) come to more than the ' +
        THEME_UPLOAD_MAX + ' B an orb accepts \u2014 ship the design without its art, or none');
    }
    var hdr = packHeader3({ geomSize: region.length, geomCrc: crc32(region), artOff: artOff,
                           artSize: artSize, artCrc: artCrcAll, nSections: sections.length,
                           serial: o.serial || 0, bakedMs: o.bakedMs || 0, bakedS: o.bakedS || 0, name: o.name, abi: hdrAbi,
                           designOff: design ? end : 0, designSize: design ? design.length : 0, designCrc: design ? crc32(design) : 0 });
    var out = new Uint8Array(end + (design ? design.length : 0)), p = HDR_WORDS * 4;
    if (design) out.set(design, end);
    out.set(hdr, 0);
    out.set(region, p); p += region.length;
    for (i = 0; i < arts.length; i++) out.set(arts[i], recs[i].art_off);
    // The shape is the container plus what the writer had to compute to place it: a caller
    // filling a progress bar or naming a download wants the sums without re-parsing.
    return { bytes: out, records: recs, region: region, geomSize: region.length,
             artOff: artOff, artSize: artSize, geomCrc: crc32(region), artCrc: artCrcAll };
  }

  /* Re-read a bundle in the device's refusal order — orb_bundle3_hdr_parse's exact
   * sequence, because a browser that validates in its own order reports a different
   * reason than the dial will, and the owner then chases a problem the card never had:
   * magic, format, ABI, header CRC, the D29 caps, the name. The writer calls it on its own
   * output so a bake cannot ship a file the door would refuse. */
  function parseBundle3(raw) {
    if (raw.length < HDR_WORDS * 4) throw new Error('too short to hold a theme header');
    var v = new DataView(raw.buffer, raw.byteOffset, raw.length);
    var w = new Array(HDR_WORDS);
    for (var i = 0; i < HDR_WORDS; i++) w[i] = v.getUint32(i * 4, true);
    if (w[0] !== (MAGIC >>> 0)) throw new Error('not an .orbtheme file');
    if (w[1] !== FORMAT3) {
      throw new Error('theme format ' + w[1] + '; this reader takes format ' + FORMAT3);
    }
    if (w[2] !== GEOM_ABI3 && w[2] !== GEOM_ABI6) {
      throw new Error('bundle geometry v' + w[2] + '; this build reads v' + GEOM_ABI3 +
        ' (apps) and v' + GEOM_ABI6 + ' (a clock) — re-bake it');
    }
    var z = w.slice(); z[W_HDR_CRC] = 0;
    if (w[W_HDR_CRC] !== crc32(wordsToBytes(z))) {
      throw new Error('container header checksum mismatch — re-copy the file onto the card');
    }
    checkBudget3(w[5], w[7]);
    var len = 0;
    while (len < NAME_MAX && w[W_NAME + (len >> 2)] !== undefined) {
      if (((w[W_NAME + (len >> 2)] >>> ((len & 3) * 8)) & 0xFF) === 0) break;
      len++;
    }
    var bytes = new Uint8Array(len);
    for (i = 0; i < len; i++) bytes[i] = (w[W_NAME + (i >> 2)] >>> ((i & 3) * 8)) & 0xFF;
    var name = new TextDecoder('utf-8', { fatal: true }).decode(bytes);   // D28: invalid UTF-8 is damage
    if (!name) throw new Error('theme has no name in its header — re-bake it');
    return { fmt: w[1], abi: w[2], hdrSize: w[3], geomOff: w[4], geomSize: w[5], geomCrc: w[6],
             artSize: w[7], artCrc: w[8], nSections: w[9], bakedS: w[10], serial: w[11], bakedMs: w[12],
             artOff: w[13], name: name, designOff: w[W_DESIGN_OFF], designSize: w[W_DESIGN_SIZE], designCrc: w[W_DESIGN_CRC] };
  }
  /* P7: a theme's design, or why there is none to reopen. Never throws for the design's own
   * faults: the dial is good whatever its design says, so a damaged one is a sentence. */
  function readDesign3(raw) {
    var h = parseBundle3(raw);
    if (!h.designSize) return { design: null, why: 'this theme carries no design, so it cannot be reopened' };
    if (h.designOff < h.artOff + h.artSize || h.designOff + h.designSize > raw.length)
      return { design: null, why: 'this theme\u2019s design runs past the end of the file \u2014 it cannot be reopened' };
    var d = raw.subarray(h.designOff, h.designOff + h.designSize);
    if (crc32(d) !== h.designCrc) return { design: null, why: 'this theme\u2019s design is damaged \u2014 it cannot be reopened' };
    return { design: d, why: null };
  }

  /* The full walk: read_bundle's promises, in read_bundle's order — region bounds, region
   * CRC, the index head against the header's count, then per section the derived geom_off,
   * the blob's CRC and length, the slug the index names against the slug the blob answers
   * to, the ABI pair, the derived art_off, the bank CRC, and finally that the slugs are
   * strictly sorted. An editor that just wrote a file checks it here, and an editor
   * loading one gets the same sentence the device would print. */
  function verifyBundle3(raw) {
    var h = parseBundle3(raw);
    if (h.geomOff + h.geomSize > raw.length) throw new Error('geometry region runs past the end of the file');
    var region = raw.subarray(h.geomOff, h.geomOff + h.geomSize);
    if (crc32(region) !== h.geomCrc) throw new Error('geometry checksum mismatch — the copy on the card is damaged');
    if (region.length < 8) {
      throw new Error('index claims ' + h.nSections + ' sections in ' + region.length + ' region bytes — torn file');
    }
    var idx = unpackIndex3(region);
    if (idx.n !== h.nSections) throw new Error('header names ' + h.nSections + ' sections, the index holds ' + idx.n);
    if (h.artOff + h.artSize > raw.length) throw new Error('art region runs past the end of the file');
    var walk = 8 + IDX_WORDS * 4 * idx.n, aoff = h.artOff, out = [], prev = null;
    for (var i = 0; i < idx.recs.length; i++) {
      var r = idx.recs[i];
      if (r.geom_off !== walk) {
        throw new Error('section ' + i + ' stores geom_off ' + r.geom_off + ' where the walk says ' +
          walk + ' — the index and the bytes contradict');
      }
      walk += r.geom_size;
      if (walk > region.length) throw new Error('section ' + i + '\u2019s blob range is outside the region');
      var blob = region.subarray(r.geom_off, r.geom_off + r.geom_size);
      if (crc32(blob) !== r.geom_crc) throw new Error('section ' + i + ' blob checksum mismatch');
      var b = unpackBlob3(blob, r.abi_max === undefined ? GEOM_ABI3 : r.abi_max);
      if (b.slug === 'clock') checkClockStamp({ abi_max: r.abi_max, elem_words: b.elem_words, spr_words: b.spr_words });
      if (r.slug_off + r.slug_len > h.geomSize) throw new Error('section ' + i + ': the index\u2019s slug and the blob\u2019s disagree');
      var want = region.subarray(r.slug_off, r.slug_off + r.slug_len);
      if (want.length !== b.slug_b.length) throw new Error('section ' + i + ': the index\u2019s slug and the blob\u2019s disagree');
      for (var k = 0; k < want.length; k++) {
        if (want[k] !== b.slug_b[k]) throw new Error('section ' + i + ': the index\u2019s slug and the blob\u2019s disagree');
      }
      if (!(b.slug_b.length >= 1 && b.slug_b.length <= SLUG_MAX) || !SLUG_RE.test(b.slug)) {
        throw new Error('bundle slug ' + q(b.slug) + ' is not lowercase a-z 0-9 _ of 1..' + SLUG_MAX + ' chars');
      }
      if (r.n_elem !== b.elements.length) {
        throw new Error('section ' + q(b.slug) + ': the index counts ' + r.n_elem + ' elements, the blob carries ' + b.elements.length);
      }
      if (r.art_off !== aoff) {
        throw new Error('section ' + i + ' stores art_off ' + r.art_off + ' where the art walk says ' +
          aoff + ' — the index and the bytes contradict');
      }
      var a = raw.subarray(r.art_off, r.art_off + r.art_size);
      if (crc32(a) !== r.art_crc) {
        throw new Error('section ' + q(b.slug) + ' bank checksum mismatch — the copy on the card is damaged');
      }
      aoff += (r.art_size + SECTOR - 1) & ~(SECTOR - 1);
      if (prev !== null && !(prev < b.slug)) {
        throw new Error('index is not slug-sorted; the resolver and the inspector both walk it in order, and a shuffled one hides a lost section');
      }
      prev = b.slug;
      out.push({ rec: r, blob: b, art: a });
    }
    return { header: h, sections: out };
  }

  /* The resolver's per-section ABI test, which is NOT part of the walk above: the
   * authority opens a bundle whose sections a given build cannot use, because one container
   * themes several apps and a current build still resolves the others. The interval form is
   * what lets one bundle carry a section for a future build, so [3,4] straddles v3 and is
   * usable, while [1,2] is a section this build has no business painting. D24's
   * no-backward-path rule bites inside the refusal, not the range. */
  function sectionAbiOk(rec) {
    // A section resolves when its interval reaches a stamp this build reads: v3 for an
    // app, v6 for a clock (D42).
    var reach = function (v) { return rec.abi_max >= v && rec.abi_min <= v; };
    return rec.abi_min >= 1 && rec.abi_min <= rec.abi_max && (reach(GEOM_ABI3) || reach(GEOM_ABI6));
  }

  /* The bake path: drawn planes in, an installable bundle out.
   *
   * writeBundle3 takes ENCODED pixels on purpose — the writer's parity is a byte question,
   * which headless-bundle3.test.js answers, and encoding is a canvas question, which is
   * this function's. bank3 follows theme_schema.bank_bytes: RGB565 colour then an A8 alpha
   * stride, so a plane costs w*h*3 and a colour-only bank w*h*2. `alpha: false` is right
   * only for a bank of plates, because orb_clock3.c reads plane 0 colour-only for exactly
   * that reason; every sprite, hand and overlay carries its mask, and dropping it would
   * draw junk where the art has a rounded corner. The rows come out contiguous because
   * they are emitted in byte order — the catalogue cannot claim a hole it did not write. */
  function bank3(planes, o) {
    o = o || {};
    var alpha = o.alpha === undefined ? true : !!o.alpha;
    // o.dither {x, y}: the row's top-left on the panel; a plane's own ox/oy (a delta
    // crop's place inside its row) adds to it. Absent, every plane truncates.
    var dith = o.dither || null, at2;
    var out = [], rows = [], at = 0, i, p, col, cost;
    if (!planes || !planes.length) throw new Error('a bank holds at least one plane; an empty one draws nothing');
    for (i = 0; i < planes.length; i++) {
      p = planes[i];
      if (!(p.w > 0 && p.h > 0)) {
        throw new Error('bank plane ' + i + ' is ' + p.w + 'x' + p.h + ': a plane with no extent carries no pixels and names no catalogue row');
      }
      if (!p.rgba || p.rgba.length !== p.w * p.h * 4) {
        throw new Error('bank plane ' + i + ' is ' + (p.rgba ? p.rgba.length : 0) + ' B of RGBA, not the ' +
          (p.w * p.h * 4) + ' B its ' + p.w + 'x' + p.h + ' extent calls for');
      }
      at2 = dith ? { x: dith.x + (p.ox | 0), y: dith.y + (p.oy | 0) } : null;
      col = rgb565Plane(p.rgba, p.w, p.h, at2);
      if (alpha) col = concat([col, alphaPlane(p.rgba, p.w, p.h, at2)]);
      cost = col.length;
      out.push(col);
      rows.push({ off: at, size: cost, w: p.w, h: p.h });
      at += cost;
    }
    return { art: concat(out), sprites: rows, size: at };
  }

  /* A clock section the compiled dial will project, assembled from planes the caller drew.
   *
   * Every rule here is orb_clock3.c's, not a preference: plate slot 0 and overlay slot 1
   * only; hand slots 0-4; a sprite row is a shadow and its slot is its hand's, because the
   * light offset is only recoverable from the hand it hangs under; the plate is the whole
   * panel and either one still frame or a §31 keyframe bank — pass plate.frames, an array
   * of two or more whole-panel planes, and the bake emits them contiguous and colour-only
   * under one row, with the scalars cross-set to match (bg_frames = frames − 1, bg_mode 0,
   * gif_frames 0). The bank's tempo comes from plate.fps/bg_fps and plate.loop/bg_every;
   * a one-shot with no tempo is refused, not frozen. o.carriage is §31's
   * sprite-region carrier, and o.comps is §31's comp table (D32 ruling A): a
   * comp entry's masked planes enter contiguous under one window row, and the
   * projection's bank rules refuse at the bake. A caller asking for any of the refused shapes
   * gets the dial's own sentence rather than a bundle the door opens and the projection
   * then rejects — the worst failure this path can have, because it looks like a store
   * error.
   *
   * The bank is the plate's planes first (colour only, the dial reads them so), then one
   * masked plane per row in table order, so the catalogue is contiguous and accounts for
   * the art exactly. The D29 art cap is checked against the assembled bank before the
   * write: a full-panel keyframe costs w*h*2 and a comp tile w*h*3, the cap
   * binds long before the dialect's 16-frame bound, and an owner choosing
   * frames needs both numbers. */
  /* D39: the clock's sound rows, refused for the reasons orb_clock3.c refuses them and
   * in theme_schema.check_clock_sounds' words. `sprites` is the catalogue ({off, size,
   * w, h}); the tick loop's beat is held against the section's seconds cadence.
   * Returns the hour pattern the rows agree on, or -1 when there are none. */
  function checkClockSounds(elems, sprites, secondSweep, tickRate) {
    var at = {}, i, slot;
    for (i = 0; i < elems.length; i++) {
      var r = elems[i];
      if ((r['class'] | 0) !== CLASS_SOUND) continue;
      slot = r.slot | 0;
      if (slot >= SOUND_SLOTS.length)
        throw new Error('a sound at slot ' + slot + ': the clock declares ' + SOUND_SLOTS.length + ' sound slots');
      if ((r.n_frames | 0) !== 1)
        throw new Error('a sound at slot ' + slot + ' names ' + r.n_frames + ' planes: a sound is one waveform');
      if ((r.w | 0) || (r.h | 0))
        throw new Error('element ' + r.n + ' is a sound sized ' + r.w + 'x' + r.h + ': a sound places nothing, and its length is its plane\u2019s');
      at[slot] = r;
    }
    var pattern = -1, tick = SOUND_SLOTS.indexOf('tick_loop');
    for (slot = 0; slot < SOUND_SLOTS.length; slot++) {
      var e = at[slot];
      if (!e) continue;
      var sp = sprites[e.frame0 | 0];
      var senc = e.src | 0, swant = soundBytes(senc, Math.max(sp.w, 0));
      if (sp.h !== 1 || sp.w < 2 || sp.size !== swant)
        throw new Error('sound slot ' + slot + '\u2019s plane is ' + sp.w + 'x' + sp.h + ' in ' + sp.size + ' B: a sound in encoding ' + senc + ' is w samples by 1 in ' + swant + ' B');
      if (sp.off & 1)
        throw new Error('sound slot ' + slot + ' starts at odd offset ' + sp.off + ': its samples would load unaligned');
      var kind = e.kind | 0;
      if (pattern >= 0 && kind !== pattern)
        throw new Error('sound slot ' + slot + ' names hour pattern ' + kind + ' and an earlier sound names ' + pattern + ': a theme has one pattern');
      pattern = kind;
      var loop = !!((e.flags | 0) & EF_LOOP), vmax = e.vmax >>> 0, vmin = e.vmin >>> 0;
      if (slot === tick && !loop) throw new Error('the tick loop is not flagged to loop: it would tick once through and stop');
      if (slot !== tick && loop) throw new Error('sound slot ' + slot + ' is flagged to loop: only the tick loop runs until it is stopped');
      if (slot === tick) {
        if (!vmax) throw new Error('the tick loop names no beat: the dial cannot hold it against the seconds hand');
        if ((sp.w * vmax) % (SOUND_RATE * 100))
          throw new Error('the tick loop\u2019s ' + sp.w + ' samples are not a whole number of beats at ' + vmax + ' beats per 100 s');
        var step = tickRate || 1;
        if (!secondSweep && vmax !== step * 100)
          throw new Error('the tick loop beats ' + Math.floor(vmax / 100) + '.' + ('0' + vmax % 100).slice(-2) +
            ' times a second and the seconds hand steps ' + step + ': the sound would walk off the hand');
        if (vmin * vmax >= SOUND_RATE * 100)
          throw new Error('the tick loop\u2019s first beat at sample ' + vmin + ' sits past one beat');
      } else if (vmax || vmin) {
        throw new Error('sound slot ' + slot + ' carries a beat: only the tick loop has one');
      }
    }
    if (pattern >= 0) {
      var has = function (n) { return !!at[SOUND_SLOTS.indexOf(n)]; }, name = HOUR_PATTERNS[pattern];
      if ((name === 'count' || name === 'ships_bells') && !has('strike_hour'))
        throw new Error('the hour pattern strikes and the theme carries no hour stroke');
      if (name === 'morse' && !(has('morse_dit') && has('morse_dah')))
        throw new Error('the hour pattern is morse and the theme lacks a dit or a dah');
      if (name === 'repeater' && !(has('strike_hour') && has('strike_qtr') && has('strike_min')))
        throw new Error('the hour pattern is a repeater and the theme lacks one of its three strokes');
    }
    return pattern;
  }

  /* The composer's authoring motions (MOTION_NAMES) in v6 terms. The art stays where
   * the composer placed it, at P0; a slide's start offset moves the placement itself,
   * because v6 carries no offset at P0. A rotation over a whole turn wraps; a partial
   * one holds at its end; a retrograde and a slide fly back. */
  function v6Drive(motion, d, x, y) {
    var m = motion === undefined ? MOTION_ROTATE
          : (typeof motion === 'string' ? MOTION_NAMES.indexOf(motion) : motion);
    if (!(m >= 0 && m < MOTION_NAMES.length))
      throw new Error('a row moving as ' + JSON.stringify(motion) + ': the composer names ' +
        MOTION_NAMES.join(', ') + ' and this names none of them');
    // vmin is the source's first value (a date runs 1..32): the dial poses at
    // (units - vmin) / (vmax - vmin), so dropping it here put every day 1/32 turn off
    var span = d.span | 0, zero = d.zero | 0, vmax = d.vmax | 0, vmin = d.vmin | 0;
    var out = { span: span, zero: zero, vmin: vmin, vmax: vmax, dx: 0, dy: 0, x: x | 0, y: y | 0,
                end: m === MOTION_RETROGRADE ? END_FLYBACK
                   : Math.abs(span) === TURN_10 ? END_WRAP : END_HOLD,
                jump: m === MOTION_INDEXED, slide: motionIsSlide(m) };
    if (out.slide) {
      // D43 P5d.2: a slider face's track at any angle (degrees, 0 = rightward): the row
      // already carries independent dx and dy, so the travel is just split along it
      if (d.angle != null && isFinite(+d.angle)) {
        var ga = (+d.angle * Math.PI) / 180, cx = Math.cos(ga), cy = Math.sin(ga);
        out.dx = Math.round(span * cx); out.dy = Math.round(span * cy);
        out.x += Math.round(zero * cx); out.y += Math.round(zero * cy);
      } else if (m === MOTION_SLIDE_X) { out.dx = span; out.x += zero; }
      else { out.dy = span; out.y += zero; }
      out.span = 0; out.zero = 0; out.end = END_FLYBACK;
    }
    return out;
  }
  /* theme-editor-kit-plan K5: the stage bake. What any app the stage draws needs to turn
   * its rows into a FORMAT 3 section, with nothing of any one app in it: the art bank
   * (raw bytes such as PCM first, then encoded planes, in the order they enter), one
   * element per row from blankElement(), a still row, a keyframe row, a delta row with its
   * closing rect and its round-trip check, the preview plane, the slot's art cap, and the
   * section packed through writeBundle3 with its meter and estimate. The app decides what
   * each row is (class, slot, kind, source, depth, flags); bakeClock3 is the clock's
   * translation on top of it.
   *
   *   var B = GorbBake.stage3({ scr_w, scr_h, dither });   // dither: false truncates every plane
   *   B.put(bytes, w, h) -> sprite index · B.row(e, plane, masked, dith) · B.rowFrames(e, planes, masked, dith)
   *   B.rowDelta(e, planes, delayMs, masked, dith) -> rec · B.preview({plane, x, y, order}) -> bytes
   *   B.id() -> the next element n · B.element(e) -> a pushed blankElement with e's fields
   *   B.rowPick(e, planes, masked, dith) -> rec: a row whose frame the app picks at runtime
   *     (theme-pool-plan §2.6: frames and no frame time, at most CLK_PICK_MAX, each plane the
   *     row's own w x h) · B.pool(e, planes, rows, masked, dith) -> recs: one picked bank and a
   *     row per entry of rows ({slot, x, y, ...}), every one naming the same frames
   *   B.pack({name, serial, bakedMs, design, slug, scalars, abi}) -> writeBundle3's answer
   *   B.finish(meta) -> pack, the delta round trip, the cap, meter and est (an app with no
   *                     bake passes of its own calls this alone)
   * B.bank and B.elements are the live bank and table: a translation may read them. */
  function stage3(opts) {
    var o = opts || {};
    var scrW = o.scr_w || SCREEN_W, scrH = o.scr_h || SCREEN_H;
    var bank = { art: new Uint8Array(0), sprites: [], size: 0 };
    var elems = [], nid = 1, deltas = [];
    function put(bytes, w, h) {
      bank.art = concat([bank.art, bytes]);
      bank.sprites.push({ off: bank.size, size: bytes.length, w: w, h: h });
      bank.size += bytes.length;
      return bank.sprites.length - 1;
    }
    /* D43 §5.3: `dith` is true for a still, screen-locked plane (the plate, its bank, the
     * crystal overlay, a window's plates, the preview). It dithers at the row's own x/y,
     * unless the whole bake asked for truncation (o.dither === false). */
    function ditherAt(e, dith) { return dith && o.dither !== false ? { x: e.x | 0, y: e.y | 0 } : null; }
    // the plate (class plate, slot 0) as the panel's disc, when it is an opaque full panel
    // a full-panel plate as the panel's disc: the plate itself (slot 0, opaque) and the
    // glass overlay (slot 1, masked: its colour disc, then its alpha disc)
    function discOf(e, plane, masked, art) {
      if ((e['class'] | 0) !== CLASS_PLATE || plane.w !== DISC_SIDE || plane.h !== DISC_SIDE ||
          scrW !== DISC_SIDE || scrH !== DISC_SIDE) return null;
      if (!(e.slot | 0) && !masked) return discPack(art);
      if ((e.slot | 0) === 1 && masked) return discPackMasked(art, DISC_SIDE);
      return null;
    }
    function row(e, plane, masked, dith) {
      var b = bank3([plane], { alpha: masked, dither: ditherAt(e, dith) });
      var disc = discOf(e, plane, masked, b.art);
      if (disc) b = { art: disc, sprites: [{ size: disc.length }] };
      bank.art = concat([bank.art, b.art]);
      bank.sprites.push({ off: bank.size, size: b.sprites[0].size, w: plane.w, h: plane.h });
      bank.size += b.sprites[0].size;
      var rec = blankElement();
      for (var f in e) rec[f] = e[f];
      rec.frame0 = bank.sprites.length - 1;
      elems.push(rec);
      return rec;
    }
    /* OS v1.2.12: a font. Its glyph planes are stored as coverage alone (each plane's
     * alpha, one byte a pixel) and coloured as a label draws them: `top` at the cell's top
     * running to `foot` at its foot ('#rrggbb'; foot omitted for a flat colour), carried
     * as RGB565 in the row's vmin and vmax (theme_schema.font_colours). A font whose
     * glyphs match an earlier font's byte for byte (one face and size in two tones) names
     * that font's frames instead of storing them again.
     * OS v1.2.17: a glyph the font does not carry is null (or an empty plane) in `planes`,
     * stored as an empty frame (0x0, 0 B), so a font keeps only the glyphs its labels draw
     * (a temperature's digits, say); it carries at least one, all one cell. */
    var fontSets = [];
    function font(e, planes, top, foot) {
      var rec = blankElement();
      for (var f in e) rec[f] = e[f];
      var cell = planes.filter(function (p) { return p && p.w && p.h; })[0];
      if (!cell) throw new Error('font' + (e.slot | 0) + ' carries no glyph: every plane is empty');
      var cov = planes.map(function (p) {
        if (!p || !p.w || !p.h) return null;
        var a = new Uint8Array(p.w * p.h);
        for (var i = 0; i < a.length; i++) a[i] = p.rgba[i * 4 + 3];
        return a;
      });
      var hit = null;
      for (var k = 0; k < fontSets.length && !hit; k++) {
        var s = fontSets[k];
        if (s.n !== cov.length || s.w !== cell.w || s.h !== cell.h) continue;
        var same = true;
        for (var j = 0; j < cov.length && same; j++) {
          if (!cov[j] !== !s.cov[j]) { same = false; break; }
          if (cov[j]) for (var b = 0; b < cov[j].length; b++) if (cov[j][b] !== s.cov[j][b]) { same = false; break; }
        }
        if (same) hit = s;
      }
      if (hit) rec.frame0 = hit.frame0;
      else {
        rec.frame0 = bank.sprites.length;
        for (var g = 0; g < cov.length; g++) {
          if (cov[g]) put(cov[g], planes[g].w, planes[g].h);
          else put(new Uint8Array(0), 0, 0);
        }
        fontSets.push({ frame0: rec.frame0, n: cov.length, w: cell.w, h: cell.h, cov: cov });
      }
      rec.n_frames = cov.length;
      rec.vmin = rgb565(top);
      rec.vmax = rgb565(foot || top);
      elems.push(rec);
      return rec;
    }
    // §31: one row, N planes. The keyframes enter contiguously, in bank order,
    // colour-only like the plate — the projection seats the row against exactly
    // this shape, and the bgAnim consumer flips through it on its beat.
    function rowFrames(e, planes, masked, dith) {
      var rec = blankElement();
      for (var f in e) rec[f] = e[f];
      rec.frame0 = bank.sprites.length;
      for (var j = 0; j < planes.length; j++) {
        var b = bank3([planes[j]], { alpha: masked, dither: ditherAt(e, dith) });
        bank.art = concat([bank.art, b.art]);
        bank.sprites.push({ off: bank.size, size: b.sprites[0].size, w: planes[j].w, h: planes[j].h });
        bank.size += b.sprites[0].size;
      }
      elems.push(rec);
      return rec;
    }
    // C5's delta plate (§4.2, §7.2) plus §37's closing rect: frame 0 is the whole-panel
    // base plane, every frame after it a rect cropped to the run of texels it changes,
    // and the row's LAST sprite — one past its own frame window, claimed by
    // EF_DELTA_WRAP — the rect that returns the finished canvas to the keyframe. A
    // frame that turns back into its source (a still in the loop) rides a
    // zero-size row at the bank's running end, which the loader accepts and
    // gif_apply_frame applies as nothing. The running canvas is
    // the composited result the device ends up with; the encode is lossy at the
    // DELTA_TOL floor, so selfVerify3 re-derives it to that same floor, closing rect
    // included, before ship.
    function rowDelta(e, planes, delay, masked, dith) {
      var rec = blankElement();
      for (var f in e) rec[f] = e[f];
      rec.frame0 = bank.sprites.length;
      rec.n_frames = planes.length;
      rec.delay = delay | 0;
      var canvas = null;
      for (var j = 0; j < planes.length; j++) {
        var p = planes[j], spr;
        if (j === 0) {
          canvas = copyPlane(p);                   // frame 0: the whole base plane
          var b0 = bank3([p], { alpha: masked, dither: ditherAt(e, dith) });
          var d0 = discOf(e, p, masked, b0.art);  // stored as the panel's disc (orb_disc.h)
          if (d0) b0 = { art: d0, sprites: [{ size: d0.length }] };
          bank.art = concat([bank.art, b0.art]);
          spr = { off: bank.size, size: b0.sprites[0].size, w: p.w, h: p.h, diffx: 0, diffy: 0 };
          bank.size += b0.sprites[0].size;
        } else {
          var rect = diffRect(p, canvas, DELTA_TOL); // reject a lone dither texel below the shared floor
          if (!rect) {    // a still in the loop: a zero-size row at the bank's running end —
                          // the loader's contiguity walk only believes a row whose off is the
                          // bytes before it, and gif_apply_frame skips zero-extent rects
            spr = { off: bank.size, size: 0, w: 0, h: 0,
                    diffx: 0, diffy: 0 };
          } else {
            paintRect(canvas, p, rect);            // apply the delta, exact
            var crop = cropRect(p, rect[0], rect[1], rect[0] + rect[2], rect[1] + rect[3]);
            crop.ox = rect[0]; crop.oy = rect[1];
            var b2 = bank3([crop], { alpha: masked, dither: ditherAt(e, dith) });
            bank.art = concat([bank.art, b2.art]);
            spr = { off: bank.size, size: b2.sprites[0].size, w: crop.w, h: crop.h,
                    diffx: rect[0], diffy: rect[1] };
            bank.size += b2.sprites[0].size;
          }
        }
        bank.sprites.push(spr);
      }
      // §37: close the loop. The device paints frame 0 over whatever the chain last
      // applied, and without this rect the wrap is a keyframe blit the delta table
      // cannot express: a stalled tick at every wrap and a stale base for frame 1.
      // One delta, derived from the keyframe and the finished canvas, shipped as the
      // row's last sprite and claimed by EF_DELTA_WRAP. A bank whose last frame
      // already equals the keyframe needs no closing rect and ships no flag.
      var wr = diffRect(planes[0], canvas, DELTA_TOL);
      if (wr) {
        var wcrop = cropRect(planes[0], wr[0], wr[1], wr[0] + wr[2], wr[1] + wr[3]);
        wcrop.ox = wr[0]; wcrop.oy = wr[1];
        var b3 = bank3([wcrop], { alpha: masked, dither: ditherAt(e, dith) });
        bank.art = concat([bank.art, b3.art]);
        bank.sprites.push({ off: bank.size, size: b3.sprites[0].size,
                            w: wcrop.w, h: wcrop.h, diffx: wr[0], diffy: wr[1] });
        bank.size += b3.sprites[0].size;
        rec.flags |= EF_DELTA_WRAP;
      }
      elems.push(rec);
      return rec;
    }
    // The union box of texels that differ from the running canvas, within the row.
    // Returns null for a frame that changed nothing (a still in the loop).
    function diffRect(plane, canvas, thresh) {
      if (!canvas) return [0, 0, plane.w, plane.h];
      var dl = 1 << 30, dt = 1 << 30, dr = -1, db = -1;
      for (var i = 0; i < plane.w * plane.h; i++) {
        var a = i * 4, sum = 0;
        for (var c = 0; c < 4; c++) sum += Math.abs(plane.rgba[a + c] - canvas.rgba[a + c]);
        if (sum > thresh) {
          var x = i % plane.w, y = (i / plane.w) | 0;
          if (x < dl) dl = x; if (y < dt) dt = y; if (x > dr) dr = x; if (y > db) db = y;
        }
      }
      if (dr < 0) return null;
      return [dl, dt, dr - dl + 1, db - dt + 1];
    }
    function paintRect(canvas, plane, rect) {
      for (var y = 0; y < rect[3]; y++) {
        var row = ((rect[1] + y) * plane.w + rect[0]) * 4;
        canvas.rgba.set(plane.rgba.subarray(row, row + rect[2] * 4), row);
      }
    }
    // The round trip (§7.2): composite the baked deltas back to whole frames and
    // compare, texel for texel, to the running planes the bake was handed — but to
    // the same DELTA_TOL floor the encoder dropped sub-threshold texels at. A texel
    // inside the rect was copied exact; one outside it never cleared the floor, so a
    // summed change past DELTA_TOL means a rect that missed real motion (a too-small
    // or misplaced rect), which is the drift worth refusing. A delta sprite at (0,0)
    // matching the row box is a still carrying no new pixels, so it applies nothing.
    // This is the bake's own refusal, not a test reading its output.
    function selfVerify3(planes, sprites, baseIndex, wrap) {
      var canvas = copyPlane(planes[0]);
      var close = function (want) {
        var n3 = want.w * want.h;
        for (var j = 0; j < n3; j++) {
          var a4 = j * 4, sum = 0;
          for (var c = 0; c < 4; c++) sum += Math.abs(canvas.rgba[a4 + c] - want.rgba[a4 + c]);
          if (sum > DELTA_TOL) return false;
        }
        return true;
      };
      for (var i = 1; i < planes.length; i++) {
        var sp = sprites[baseIndex + i];
        if (sp.size) paintRect(canvas, planes[i], [sp.diffx, sp.diffy, sp.w, sp.h]);
        if (!close(planes[i])) return false;
      }
      if (wrap) {
        // §37: the closing rect must land the canvas back on the keyframe. This is the
        // one check that says the shipped chain is a CYCLE; without it the bake ships
        // a path that teleports home and the device freezes at the wrap.
        var wsp = sprites[baseIndex + planes.length];
        if (!wsp || !wsp.size) return false;
        paintRect(canvas, planes[0], [wsp.diffx, wsp.diffy, wsp.w, wsp.h]);
        return close(planes[0]);
      }
      return true;
    }
    /* radar-plan §2.11: the app's values, one blob of its own layout stamped with its layout
     * version (1-255). Class 11 slot 0, one plane of the bytes as w by 1, placing nothing:
     * the theme carries them, the OS checks them and hands them to the app as they are. */
    function values(bytes, version) {
      if (!(bytes instanceof Uint8Array) || !bytes.length || bytes.length > VALUES_MAX)
        throw new Error('values of ' + (bytes && bytes.length) + ' B: a theme\u2019s values are 1 to ' + VALUES_MAX + ' bytes');
      if (!(version >= 1 && version <= 255))
        throw new Error('values of layout version ' + version + ': the app\u2019s editor stamps its layout from 1 to 255');
      if (elems.some(function (e) { return (e['class'] | 0) === CLASS_VALUES; }))
        throw new Error('a second values blob: a theme carries one');
      var f = put(bytes, bytes.length, 1);
      var v = blankElement();
      v.n = nid++; v['class'] = CLASS_VALUES; v.slot = 0;
      v.frame0 = f; v.n_frames = 1; v.src = version | 0;
      elems.push(v);
      return bytes.length;
    }
    function preview(pv) {
      var pp = pv.plane || pv;
      if (!pp || !pp.rgba || !(pp.w > 0) || !(pp.h > 0))
        throw new Error('a preview row with no plane, or a plane with no extent: the picker reads pixels, not a nameplate');
      if (pp.rgba.length !== pp.w * pp.h * 4)
        throw new Error('a preview plane of ' + pp.w + 'x' + pp.h + ' carries ' + pp.rgba.length +
          ' B of RGBA, not the ' + (pp.w * pp.h * 4) + ' B its extent calls for');
      if (pp.w !== pp.h)
        throw new Error('a preview plane of ' + pp.w + 'x' + pp.h + ': a preview is a square, so the OS card slot is a square — ' +
          'a non-square plane reads as a wrong bake, not a wrong panel');
      if (pp.w > scrW || pp.h > scrH)
        throw new Error('a preview plane of ' + pp.w + 'x' + pp.h + ' on a ' + scrW + 'x' + scrH + ' panel: a preview is at most ' +
          'the glass itself (§60), and a plane past it cannot be held');
      var pb = bank3([pp], { alpha: false, dither: o.dither === false ? null : { x: 0, y: 0 } });   // RGB565 only: w*h*2 B, the exception orb_asset_slot_bytes names
      // OS v1.2.12: stored as its disc (orb_disc.h), so the picker shows it round; §60: and
      // deflated, whenever that is smaller (it is the whole glass now, and the card reads
      // ~0.4 MB/s), the orb telling the two apart by the size alone
      var pd = discPack(pb.art, pp.w);
      var pz = zlibFixed(pd);
      if (pz.length < pd.length) pd = pz;
      pb = { art: pd, sprites: [{ size: pd.length }] };
      bank.art = concat([bank.art, pb.art]);
      bank.sprites.push({ off: bank.size, size: pb.sprites[0].size, w: pp.w, h: pp.h });
      bank.size += pb.sprites[0].size;
      var prev = blankElement();
      prev.n = nid++; prev['class'] = CLASS_PREVIEW; prev.slot = 0;
      prev.order = pv.order | 0;
      prev.x = pv.x | 0; prev.y = pv.y | 0;
      prev.w = pp.w; prev.h = pp.h;
      prev.frame0 = bank.sprites.length - 1;
      prev.n_frames = 1;
      prev.alpha = 255;
      elems.push(prev);
      return pb.sprites[0].size;
    }
    function element(e) {
      var rec = blankElement();
      for (var f in e) rec[f] = e[f];
      elems.push(rec);
      return rec;
    }
    // frames the app picks: refused here, at the bake, for what the dial would refuse at load
    function pickPlanes(e, planes) {
      var w = e.w | 0, h = e.h | 0;
      if (!planes || planes.length < 2)
        throw new Error('a picked row needs two frames or more to pick between; one frame is a still row');
      if (planes.length > CLK_PICK_MAX)
        throw new Error('a row offers the app at most ' + CLK_PICK_MAX + ' frames to pick from, not ' + planes.length);
      for (var i = 0; i < planes.length; i++) {
        if (planes[i].w !== w || planes[i].h !== h)
          throw new Error('frame ' + i + ' of a picked row is ' + planes[i].w + 'x' + planes[i].h + ' and the row is ' + w + 'x' + h +
            ': the app picks among frames of one size, turned about one pivot');
      }
    }
    function rowPick(e, planes, masked, dith) {
      pickPlanes(e, planes);
      return rowFrames(Object.assign({}, e, { n_frames: planes.length, delay: 0 }), planes, masked, dith);
    }
    // one bank of frames and a row per entry of rows, each naming it: rows[0] lays the bank
    function pool(e, planes, rows, masked, dith) {
      if (!rows || !rows.length) throw new Error('a pool needs at least one row');
      var first = rowPick(Object.assign({}, e, rows[0], { n: nid++ }), planes, masked, dith), out = [first];
      for (var i = 1; i < rows.length; i++)
        out.push(element(Object.assign({}, e, rows[i], { n: nid++, frame0: first.frame0, n_frames: planes.length, delay: 0 })));
      return out;
    }
    function pack(m) {
      return writeBundle3({ name: m.name, serial: m.serial || 0, bakedMs: m.bakedMs || 0, bakedS: m.bakedS || 0, design: m.design || null,
        sections: [{ slug: m.slug, scr_w: scrW, scr_h: scrH, elements: elems, scalars: m.scalars || {},
                     sprites: bank.sprites, art: bank.art,
                     abi_min: m.abi, abi_max: m.abi }] });
    }
    function capCheck() {
      if (bank.size > SLOT_ART_CAP3)
        throw new Error('the bake\'s art bank is ' + bank.size + ' B against the ' + SLOT_ART_CAP3 + ' B the slot leaves for banks — drop frames or shrink the art');
    }
    function finish(m) {
      capCheck();
      var b = pack(m);
      deltas.forEach(function (d) {
        if (!selfVerify3(d.planes, bank.sprites, d.rec.frame0 | 0, !!(d.rec.flags & EF_DELTA_WRAP)))
          throw new Error('the baked delta bank does not composite back to the frames it was handed: the round trip drifted, and a drifting chain corrupts every later frame — refused, not shipped');
      });
      b.meter = budget3(b.bytes);
      b.n_elem = elems.length;
      b.est = estimate3(verifyBundle3(b.bytes).sections[0]);
      return b;
    }
    return { bank: bank, elements: elems, scrW: scrW, scrH: scrH,
             id: function () { return nid++; }, put: put, values: values, ditherAt: ditherAt,
             row: row, rowFrames: rowFrames, font: font,
             rowDelta: function (e, planes, delay, masked, dith) { var r = rowDelta(e, planes, delay, masked, dith); deltas.push({ rec: r, planes: planes }); return r; },
             verifyDelta: selfVerify3, preview: preview, element: element, rowPick: rowPick, pool: pool,
             pack: pack, capCheck: capCheck, finish: finish };
  }
  function bakeClock3(o) {
    var scrW = o.scr_w || SCREEN_W, scrH = o.scr_h || SCREEN_H;
    var hands = o.hands || [], shadows = o.shadows || [], plates = o.plates || [];
    var sc = {}, k, deltaBank = 0, prevB = 0;
    for (k in (o.scalars || {})) sc[k] = o.scalars[k];
    if (!o.plate) throw new Error('a clock section needs a plate: the compiled dial takes its blank-layer centre from the plate’s own middle, so with no plate it has no centre');
    var pl = o.plate;
    if (pl.w !== scrW || pl.h !== scrH || pl.x || pl.y) {
      throw new Error('a plate at extent (' + (pl.x || 0) + ',' + (pl.y || 0) + ',' + pl.w + ',' + pl.h +
        ') of a ' + scrW + 'x' + scrH + ' panel: the compiled clock draws its background full-screen, so a plate that is not the whole panel is not a plate');
    }
    var frames = pl.frames;
    if (frames !== undefined) {
      if (!(frames.length >= 2)) {
        throw new Error('a plate bank of ' + (frames ? frames.length : 0) + ' frames: one keyframe is a still plate, and zero frames draw nothing — pass two or more planes or none');
      }
      if (frames.length > BG3_FRAMES_MAX) {
        throw new Error('a plate animation of ' + frames.length + ' frames: the dial holds ' + BG3_FRAMES_MAX + ', and the rest is a bank it has no room for');
      }
      if (sc.bg_frames !== undefined && sc.bg_frames !== frames.length - 1) {
        throw new Error('a plate row naming ' + frames.length + ' frames while the scalars name ' + sc.bg_frames + ' extra: the row’s planes are the animation’s length');
      }
      if (sc.gif_frames) {
        throw new Error('an animated plate naming ' + sc.gif_frames + ' gif frames: a keyframe bank carries whole planes and no delta rects');
      }
      // A bank with no stated tempo is a loop — what every FORMAT 2 bgAnim kit
      // was. A one-shot must be asked for (loop: 0) and must then name its fps.
      var fps = sc.bg_fps || pl.fps || 0;
      var loop = sc.bg_loop !== undefined ? sc.bg_loop
                                          : (pl.loop !== undefined ? pl.loop : 1);
      if (!loop && fps < 1) {
        throw new Error('a one-shot plate bank at bg_fps ' + fps + ': an animation with no tempo is a frame that never arrives');
      }
      for (k = 0; k < frames.length; k++) {
        var fp = frames[k];
        if (!fp || fp.w !== scrW || fp.h !== scrH) {
          throw new Error('keyframe ' + k + ' of a plate bank is ' + (fp ? fp.w + 'x' + fp.h : 'nothing') + ': every frame of a keyframe bank is the whole panel, the frame the consumer paints at rest included');
        }
      }
      sc.bg_frames = frames.length - 1; sc.bg_fps = fps; sc.bg_loop = loop ? 1 : 0;
      if (!sc.bg_every) sc.bg_every = pl.every || 1;
      sc.bg_mode = 0; sc.gif_frames = 0;
      // Every moving plate bakes as C5's delta bank, a carriage over it included. The
      // fork that kept a caged plate a keyframe bank cited §5.2's one-gif_io rule, a
      // FORMAT 2 note: under FORMAT 3 the carriage's frames ride its own row's sprite
      // window (orb_clock3.c's tourbillon fill) and only the plate uses the bg table,
      // so the two never contend (docs/history/unified-render-path-design-2026-10-30.md §3).
      // The delta bank is the shape the device can restore from its canvas; a
      // keyframe bank has no restore plane and draws every advance through LVGL.
      deltaBank = 1;
    } else if (sc.bg_frames) {
      throw new Error('bg_frames = ' + sc.bg_frames + ' with a still plate: the scalars and the table disagree, and the pair would name a bank the art does not hold');
    }
    /* §31's sprite-region carrier: a carriage bank — the tourbillon row at hand
     * slot 5 that d32 proved on glass. The dial fans its frames by the seconds
     * angle plus the row's start orientation, so the bake hands it the motion's
     * bounding box as the row's extent and the loop's fps as the scalar. */
    var car = o.carriage;
    if (car) {
      if (!(car.frames && car.frames.length >= 2)) throw new Error('a carriage bank of ' + (car.frames ? car.frames.length : 0) + ' frames: a carriage is an animation, and one frame is a static hand this bake seats elsewhere');
      if (car.frames.length > TOURB_MAX) throw new Error('a carriage bank of ' + car.frames.length + ' frames: the dial holds ' + TOURB_MAX + ' tourbillon records');
      if (!(car.fps >= 1)) throw new Error('a carriage bank at ' + (car.fps || 0) + ' fps: the row animates at its own tempo, and a loop with no tempo is a frame that never arrives');
      var cw = car.w, chh = car.h;
      for (k = 0; k < car.frames.length; k++) {
        var cf = car.frames[k];
        if (!cf || cf.w !== cw || cf.h !== chh) throw new Error('carriage frame ' + k + ' is ' + (cf ? cf.w + 'x' + cf.h : 'nothing') + ' against a ' + cw + 'x' + chh + ' row: every frame of one bank is one extent, the same box the dial invalidates');
      }
    }
    /* §31's comp table, D32's ruling A: a window row per live complication, its
     * plates contiguous and masked. Every check below is orb_clock3.c's
     * bank-row projection rule for rule — the bake refuses the shapes the
     * projection would refuse, in the dial's own words where it has them, so
     * an author never spends an upload learning what the host already knew. */
    var compRows = o.comps || [], compB = 0, k3, j3;
    for (k3 = 0; k3 < compRows.length; k3++) {
      var cr = compRows[k3], slot3 = cr.slot;
      if (!(slot3 >= 0 && slot3 < CLK_COMP_MAX))
        throw new Error('a complication at slot ' + slot3 + ': the clock declares ' + CLK_COMP_MAX +
          ' complication slots and the manifest of any app may not invent one');
      var src3 = String(cr.src), srcId3 = SOURCES.indexOf(src3);
      if (srcId3 < 0)
        throw new Error('a bank at slot ' + slot3 + ' driven by ' + q(String(cr.src)) + ': this dial reads ' +
          SOURCES.length + ' values and none of them is that');
      if (src3 === 'battery' || src3 === 'power_reserve' || src3 === 'text' || src3 === 'value')
        throw new Error('a bank at slot ' + slot3 + ' driven by source ' + q(src3) +
          ': this dial has no such input, and D32\u2019s ruling refuses the flip rather than ship a comp that would draw nothing');
      var want3 = src3 === 'none' ? 1 : CLOCK_WINDOW_DOMAIN[src3][1];
      var pl3 = cr.planes;
      if (!pl3 || pl3.length !== want3)
        throw new Error('a window at slot ' + slot3 + ' driven by ' + q(src3) + ' indexes ' + want3 +
          ' plates, and the bake was handed ' + (pl3 ? pl3.length : 0));
      var cwr = cr.w | 0, chr = cr.h | 0;
      if (!(cwr > 0 && chr > 0))
        throw new Error('a window at slot ' + slot3 + ' is ' + cwr + 'x' + chr + ': a plate with no extent carries no pixels');
      if ((cr.x | 0) < 0 || (cr.y | 0) < 0 || (cr.x | 0) + cwr > scrW || (cr.y | 0) + chr > scrH)
        throw new Error('a window at slot ' + slot3 + ' places its ' + cwr + 'x' + chr + ' box at (' +
          (cr.x | 0) + ',' + (cr.y | 0) + ') of a ' + scrW + 'x' + scrH + ' panel: the runtime would draw it off the glass');
      var ordr = cr.order | 0;
      if (ordr < 0 || ordr >= ELEM_ORDER_N)
        throw new Error('a bank row at slot ' + slot3 + ' at depth ' + ordr +
          ': the dial has ' + ELEM_ORDER_N + ' rungs');
      for (j3 = 0; j3 < pl3.length; j3++) {
        var pp3 = pl3[j3];
        if (!pp3 || pp3.w !== cwr || pp3.h !== chr)
          throw new Error('complication ' + slot3 + ' frame ' + j3 + ' is ' + (pp3 ? pp3.w + 'x' + pp3.h : 'nothing') +
            ' against the row\u2019s ' + cwr + 'x' + chr + ': one bank, one extent');
      }
      compB += cwr * chr * 3 * want3;
    }
    sc.plate_off = 0;
    if (frames === undefined) { sc.bg_frames = 0; sc.gif_frames = 0; }

    // Every row starts from blankElement(): packElements refuses a missing field rather
    // than defaulting it, because a default applied at pack time is a value nobody wrote.
    // The bank builds empty and every plane enters through row() or rowFrames(), so no
    // plane is ever counted twice and the plate's rows keep the colour-only cost the
    // projection reads them at.
    var B = stage3({ scr_w: scrW, scr_h: scrH, dither: o.dither }), bank = B.bank;
    /* D39: the theme's sounds enter FIRST, so every one sits at an even offset (the
     * dial refuses an odd one: a 16-bit load from the mapped bank would fault) and
     * every pixel plane after them moves by the same even amount. plate_off and, on
     * a delta plate, bg_key_off follow the plate past them — orb_sched seeds the
     * canvas from blob() + bg_key_off, so a zero left there would paint PCM.
     * o.sounds: [{slot: name, pcm: Int16Array, beat_chz, first_beat}], o.soundPattern. */
    var sounds3 = (o.sounds || []).slice().sort(function (a, b) {
      return SOUND_SLOTS.indexOf(a.slot) - SOUND_SLOTS.indexOf(b.slot); });
    var soundB = 0, soundEnc = [];
    for (var si = 0; si < sounds3.length; si++) {
      var sn = sounds3[si];
      if (SOUND_SLOTS.indexOf(sn.slot) < 0) throw new Error('a sound for slot ' + q(String(sn.slot)) + ': the clock declares ' + SOUND_SLOTS.join(', '));
      var pcm = sn.pcm;
      if (!pcm || !(pcm.length >= 2)) throw new Error('the ' + sn.slot + ' sound carries no samples');
      var se = soundEncode(pcm);
      soundEnc[si] = se.enc;
      B.put(se.bytes, pcm.length, 1);
    }
    soundB = bank.size;
    sc.plate_off = soundB;
    /* §32.6 (D32 ruling C reversed): a theme that ships a carriage reserves order 1 —
     * one rung above the plate's 0, below every hand — so both the classic LVGL path
     * (which creates s_car before the hand loop) and the L4 tile painter (whose
     * insertion sort paints ascending order, lowest deepest) composite the cage at
     * geartrain depth under the hands, exactly as the preview draws it.
     * The sub-dial pointers ride the rung just above that (below the hands — the
     * preview's natural draw order, handSlots: subdial hands first), and the hands
     * walk up from the rung above them. o.depthOrder — the author's ONE hands list
     * from the composer, bottom to top — overrides the natural rungs: the same list
     * the preview turns and the FORMAT 2 kit ships, so the preview, the wire and the
     * glass agree from one control. Rows the list omits ride their natural rung.
     * A dial with no carriage starts the same ladder one rung lower. */
    var SUBDIAL_RUNG = (o.carriage ? 2 : 1);
    var elems = B.elements, ord = SUBDIAL_RUNG + 1, handSlot = {}, handOrd = {};
    var dOrder3 = o.depthOrder || [], rung3 = {};
    for (var q3 = 0, r3 = 2; q3 < dOrder3.length; q3++) {
      var id3 = String(dOrder3[q3]);
      if (rung3[id3] === undefined) rung3[id3] = r3++;
    }
    /* A row the list omits rides its natural rung, and that rung must not be one the list
     * gave another row: the device breaks a tie by table order, not by the stack the
     * preview drew. A composer that shipped its slot hands without their ids put Skeleton's
     * hour hand on its sub-dial seconds pointer's rung, and the glass drew the seconds
     * over the hour (bench, 2026-11-03). */
    var listedRung3 = {};
    for (var lid3 in rung3) listedRung3[rung3[lid3]] = lid3;
    function unlistedRung3(what, id, r) {
      if (dOrder3.length && listedRung3[r] !== undefined)
        throw new Error(what + ' (' + id + ') has no place in depthOrder, and its natural rung ' + r +
          ' is the one depthOrder gave ' + listedRung3[r] + ': the two would stack by table order, not as the preview draws them');
    }
    /* The natural fallback ladder: sub-dial pointers in table order, then the
     * centre stack hour, minute, second — exactly handSlots' order, so a bake
     * with no depthOrder list ships the depth the preview already shows. */
    var nat3 = (o.aux || []).map(function (a) { return a.id ? String(a.id) : null; })
                  .concat(['centre-hour', 'centre-minute', 'centre-second']);
    // the stage bake's rows (stage3): the clock decides what each row is, the builder lays it in the bank
    var ditherAt = B.ditherAt, row = B.row, rowFrames = B.rowFrames, rowDelta = B.rowDelta, selfVerify3 = B.verifyDelta;
    if (frames) {
      if (deltaBank) {
        // Per-frame delta tempo: the row's own bg_fps, or the dial's default figure
        // the shipped banks printed (15 Hz, the §5.1 default delay_ms) when none named.
        var dms = fps >= 1 ? Math.round(1000 / fps) : 67;
        rowDelta({ n: B.id(), 'class': classId('plate'), slot: 0, x: 0, y: 0, w: scrW, h: scrH,
                   order: 0, alpha: 255 }, frames, dms, false, true);
        // bg_mode 1: the row is the delta dialect. gif_frames is the whole loop, the
        // row's own plane count (orb_clock3.c:539), and bg_frames is zero — the base
        // plane is frame 0 of that loop, not an extra keyed frame beside it.
        sc.bg_mode = 1; sc.gif_frames = frames.length; sc.bg_frames = 0;
      } else {
        rowFrames({ n: B.id(), 'class': classId('plate'), slot: 0, x: 0, y: 0, w: scrW, h: scrH,
                    order: 0, alpha: 255, n_frames: frames.length }, frames, false, true);
      }
    } else {
      row({ n: B.id(), 'class': classId('plate'), slot: 0, x: 0, y: 0, w: scrW, h: scrH,
            order: 0, alpha: 255, n_frames: 1 }, pl, false, true);
    }
    /* D42: every hand, shadow, carriage and pointer is one AUX row, claimed in slot
     * order — a hand, then its shadow twin in the very next slot (clock_view's pairing
     * rule), then the next. The composer still hands the bake its centre hands by slot,
     * its shadows as sprites and its pointers with a motion name; v6Row turns each into
     * the two poses the dial reads (orb_clock3.c's row_check, theme_schema's rules), and
     * the depth each carried rides the row's own `order`. What the dial cannot hold is
     * refused here, before art. */
    var auxSlotW = 0, auxB = 0;
    function nextSlot(what) {
      if (auxSlotW >= CLK_ROW_MAX)
        throw new Error(what + ': the dial seats ' + CLK_ROW_MAX + ' moving rows, shadow twins counted, and this bake needs more');
      return auxSlotW++;
    }
    /* An authored cadence, with D41's `inherit` read as the motion it always drew: a
     * sweep for a pointer, the slot's own motion for a hand. */
    function v6Cadence(cad, arg, fallback) {
      var c = typeof cad === 'string' ? CADENCES.indexOf(cad === 'inherit' ? 'none' : cad) : (cad | 0);
      return c > 0 ? { cadence: c, cad_arg: arg | 0 } : fallback;
    }
    // The seconds hand's beat the composer priced, read once and never written: the dial
    // derives its tempo from the rows (D42 ruling 2).
    var authorSweep = sc.second_sweep === undefined ? 1 : (sc.second_sweep | 0);
    var authorRate = sc.tick_rate | 0;
    var secCad = authorSweep ? { cadence: CAD_SWEEP, cad_arg: authorRate >= 2 ? Math.min(SWEEP_FPS_MAX, authorRate) : 0 }
                             : { cadence: CAD_STEP, cad_arg: Math.max(1, authorRate) };
    /* A seconds row whose design says "sweep" and names no rate sweeps at the priced beat:
     * the plan's hand grid rides its cad_arg, or the device falls back to the plate's tempo
     * and plays gears-on-gears (Skeleton's 4/12 shipped as 4/4, §49.15). */
    function secBeat(src, c) {
      return src === 'second' && c.cadence === CAD_SWEEP && !c.cad_arg && secCad.cadence === CAD_SWEEP
           ? { cadence: CAD_SWEEP, cad_arg: secCad.cad_arg } : c;
    }
    var SLOT_DRIVE = [{ src: 'hour12', vmax: 12 }, { src: 'minute', vmax: 60 }, { src: 'second', vmax: 60 },
                      null, null];
    var elemOfHand = {};
    for (k = 0; k < hands.length; k++) {
      var h = hands[k];
      if (!(h.slot >= 0 && h.slot < 5)) throw new Error('a hand at slot ' + h.slot + ': the composer names hands 0-4');
      if (handSlot[h.slot]) throw new Error('two hands at slot ' + h.slot);
      handSlot[h.slot] = 1;
      /* The hand's rung is the author's list position if the list names it, the
       * centre-stack natural otherwise; its shadow twin shares it and draws under it. */
      var hid3 = h.id ? String(h.id) : ('centre-' + (['hour', 'minute', 'second', 'static1', 'static2'][h.slot] || h.slot));
      handOrd[h.slot] = Math.max(1, Math.min(63, dOrder3.indexOf(hid3) >= 0 ? rung3[hid3]
                                     : (h.order | 0) >= 1 ? (h.order | 0)
                                     : SUBDIAL_RUNG + ((o.aux || []).length) + k));
      if (dOrder3.indexOf(hid3) < 0 && !((h.order | 0) >= 1)) unlistedRung3('a hand at slot ' + h.slot, hid3, handOrd[h.slot]);
      var hd5 = h.drive || {}, sd = SLOT_DRIVE[h.slot];
      var hrow = { n: B.id(), 'class': CLASS_AUX, slot: nextSlot('the hand at slot ' + h.slot),
                   kind: SHAPE_ARM, x: h.x, y: h.y, w: h.w, h: h.h,
                   pivot_x: h.pivot_x, pivot_y: h.pivot_y, order: handOrd[h.slot],
                   alpha: h.alpha === undefined ? 255 : h.alpha, n_frames: 1,
                   flags: h.flags || 0, scale16: h.scale16 || 0 };
      var hsrc = hd5.src ? String(hd5.src) : (sd ? sd.src : null);
      if (hsrc) {
        // a slot hand without a source of its own keeps the slot's turn, from the standing
        // angle the composer names (an uploaded hand's facing); 0 when it names none
        var hv = v6Drive(hd5.src ? hd5.motion : 'rotate',
                         hd5.src ? hd5 : { span: TURN_10, zero: hd5.zero | 0, vmax: sd.vmax }, h.x, h.y);
        var hc = secBeat(hsrc, v6Cadence(hd5.cadence, hd5.cad_arg,
                                         hsrc === 'second' ? secCad : { cadence: CAD_SWEEP, cad_arg: 0 }));
        if (hv.jump && hc.cadence === CAD_SWEEP) hc = { cadence: CAD_JUMP, cad_arg: 0 };
        hrow.src = SOURCES.indexOf(hsrc); hrow.x = hv.x; hrow.y = hv.y;
        hrow.span = hv.span; hrow.zero = hv.zero; hrow.vmin = hv.vmin; hrow.vmax = hv.vmax;
        hrow.dx = hv.dx; hrow.dy = hv.dy; hrow.end = hv.end;
        hrow.cadence = hc.cadence; hrow.cad_arg = hc.cad_arg; hrow.ease = hd5.ease | 0;
        if (hv.slide) { hrow.pivot_x = (h.w | 0) >> 1; hrow.pivot_y = (h.h | 0) >> 1; }
      }
      row(hrow, h, true, !!h.dither);   // D43 ruling 3: an arm stays undithered unless it asks
      elemOfHand[h.slot] = hrow;
      for (var ks = 0; ks < shadows.length; ks++) {
        var s = shadows[ks];
        if (s.slot !== h.slot) continue;
        /* The sprite shadow's §25.2 placement, restated as a twin's: its arbour in its
         * own art is the hand's centre less its draw point, and that arbour lands on
         * the hand's centre plus the light (the row's x + pivot column). */
        var hcx = (h.x | 0) + (h.pivot_x | 0), hcy = (h.y | 0) + (h.pivot_y | 0);
        var scx = (s.x | 0) + (s.pivot_x | 0), scy = (s.y | 0) + (s.pivot_y | 0);
        var tpx = hcx - (s.x | 0), tpy = hcy - (s.y | 0);
        row({ n: B.id(), 'class': CLASS_AUX, slot: nextSlot('the shadow of the hand at slot ' + h.slot),
              kind: SHAPE_ARM, x: scx - tpx + hrow.x - (h.x | 0), y: scy - tpy + hrow.y - (h.y | 0),
              w: s.w, h: s.h, pivot_x: tpx, pivot_y: tpy, order: handOrd[h.slot],
              alpha: s.alpha === undefined ? 255 : s.alpha, n_frames: 1,
              flags: (s.flags || 0) | AUX_F_SHADOW3 }, s, true);
      }
    }
    for (k = 0; k < shadows.length; k++) {
      if (!handSlot[shadows[k].slot])
        throw new Error('a shadow at slot ' + shadows[k].slot + ' with no hand there: a shadow twin takes its hand’s pose, and there is no hand to take it from');
    }
    for (k = 0; k < plates.length; k++) {
      row({ n: B.id(), 'class': classId('plate'), slot: 1, x: plates[k].x, y: plates[k].y,
            w: plates[k].w, h: plates[k].h, order: 63 - (plates.length - 1 - k), flags: plates[k].flags || 0,
            alpha: plates[k].alpha === undefined ? 255 : plates[k].alpha, n_frames: 1 }, plates[k], true, true);
    }
    if (car) {
      /* The row ships CROPPED to carriageRect3, and that crop is the byte figure the
       * policy billed. The extent is what the dial invalidates every tick and what the
       * slot charges for, so passing the cage canvas through cost a panel of masked
       * pixels per frame for art that sweeps a fraction of the glass: an orbital arrow's
       * 466x466 cage canvas against 402x359 of ink billed 64,896 B and shipped 1,302,936 B
       * for two frames, and the bake blew the slot. Cropping moves the draw point to the
       * crop's top-left and the pivot back by the same amount, so the row paints the same
       * pixels about the same point and the §25.2 identity still holds. */
      var carPl = car.frames, cx3 = car.x | 0, cy3 = car.y | 0, cpx = car.pivot_x, cpy = car.pivot_y;
      var cr3 = carriageRect3(car.frames, { pivot: [cpx, cpy] });
      if (cr3 && (cr3[0] || cr3[1] || cr3[2] !== cw || cr3[3] !== chh)) {
        carPl = [];
        for (k = 0; k < car.frames.length; k++)
          carPl.push(cropRect(car.frames[k], cr3[0], cr3[1], cr3[2], cr3[3]));
        cx3 += cr3[0]; cy3 += cr3[1]; cpx -= cr3[0]; cpy -= cr3[1];
        cw = cr3[2] - cr3[0]; chh = cr3[3] - cr3[1];
      }
      /* The carriage turns with the seconds, from its own start orientation: a seconds
       * row of its own, at the seconds hand's cadence, animated at its frame time. */
      var carCad = elemOfHand[2] && SOURCES[elemOfHand[2].src] === 'second'
                 ? { cadence: elemOfHand[2].cadence, cad_arg: elemOfHand[2].cad_arg } : secCad;
      rowFrames({ n: B.id(), 'class': CLASS_AUX, slot: nextSlot('the carriage'), kind: SHAPE_ARM,
                  x: cx3, y: cy3, w: cw, h: chh, pivot_x: cpx, pivot_y: cpy,
                  order: 1, alpha: 255, n_frames: car.frames.length,
                  delay: Math.max(1, Math.round(1000 / car.fps)),
                  src: SOURCES.indexOf('second'), span: TURN_10,
                  zero: Math.round((car.degrees || 0) / 10) % TURN_10, vmin: 0, vmax: 60,
                  end: END_WRAP, cadence: carCad.cadence, cad_arg: carCad.cad_arg }, carPl, true);
    }
    /* D34's pointers, as rows: one masked plate whatever its domain — a sub-dial seconds
     * hand used to be sixty whole dials. */
    var auxRows = o.aux || [], k4;
    for (k4 = 0; k4 < auxRows.length; k4++) {
      var a5 = auxRows[k4];
      var s4 = String(a5.src);
      if (SOURCES.indexOf(s4) < 0)
        throw new Error('a pointer driven by ' + q(s4) + ': this dial reads ' + SOURCES.length + ' values and none of them is that');
      var apr = a5.plane;
      if (!apr) throw new Error('a pointer driven by ' + q(s4) + ' with no plate');
      if (apr.w !== (a5.w | 0) || apr.h !== (a5.h | 0))
        throw new Error('a pointer driven by ' + q(s4) + ' is ' + (apr.w|0) + 'x' + (apr.h|0) +
          ' against the row’s ' + (a5.w|0) + 'x' + (a5.h|0) + ': one row, one extent');
      var av5 = v6Drive(a5.motion, a5, a5.x, a5.y);
      var ac5 = secBeat(s4, v6Cadence(a5.cadence, a5.cad_arg, { cadence: CAD_SWEEP, cad_arg: 0 }));
      if (av5.jump && ac5.cadence === CAD_SWEEP) ac5 = { cadence: CAD_JUMP, cad_arg: 0 };
      var aid5 = a5.id ? String(a5.id) : null;
      var or5 = a5.order === undefined ? (aid5 && dOrder3.indexOf(aid5) >= 0 ? rung3[aid5]
                            : (aid5 ? SUBDIAL_RUNG + nat3.indexOf(aid5) : ord)) : a5.order;
      if (a5.order === undefined && !(aid5 && dOrder3.indexOf(aid5) >= 0)) unlistedRung3('a pointer driven by ' + q(s4), aid5 || '(no id)', or5);
      // A slide carries no pivot to turn about; its centre is the origin the row places.
      row({ n: B.id(), 'class': CLASS_AUX, slot: nextSlot('a pointer driven by ' + q(s4)), kind: SHAPE_ARM,
            src: SOURCES.indexOf(s4), x: av5.x, y: av5.y, w: a5.w | 0, h: a5.h | 0,
            pivot_x: av5.slide ? ((a5.w | 0) >> 1) : (a5.pivot_x | 0),
            pivot_y: av5.slide ? ((a5.h | 0) >> 1) : (a5.pivot_y | 0),
            order: or5, alpha: 255, n_frames: 1,
            span: av5.span, zero: av5.zero, vmin: av5.vmin, vmax: av5.vmax,
            dx: av5.dx, dy: av5.dy, end: av5.end,
            cadence: ac5.cadence, cad_arg: ac5.cad_arg, ease: a5.ease | 0,
            flags: a5.hidden ? EF_HIDDEN : 0 }, a5.plane, true, !!a5.dither);   // a ring or disc may ask (D43 ruling 3)
      auxB += (a5.w | 0) * (a5.h | 0) * 3;
      /* The twin the composer raised with this pointer (auxPass3): its own draw point
       * and pivot, whose sum is the pointer's centre plus the light, and no drive —
       * AUX_F_SHADOW3 is what tells the dial it takes its pointer's whole pose. */
      if (a5.shadow) {
        var av = a5.shadow;
        if (!av.rgba) throw new Error('a pointer driven by ' + q(s4) + ' carries a shadow row with no pixels');
        if (!(av.w > 0 && av.h > 0) || av.rgba.length !== (av.w | 0) * (av.h | 0) * 4)
          throw new Error('the shadow of a pointer driven by ' + q(s4) + ' is ' + (av.w | 0) + 'x' + (av.h | 0)
            + ' against ' + av.rgba.length + ' bytes: one row, one extent');
        row({ n: B.id(), 'class': CLASS_AUX, slot: nextSlot('the shadow of a pointer'), kind: SHAPE_ARM,
              x: (av.x | 0) + av5.x - (a5.x | 0), y: (av.y | 0) + av5.y - (a5.y | 0),
              w: av.w | 0, h: av.h | 0,
              pivot_x: av5.slide ? ((av.w | 0) >> 1) : (av.pivot_x | 0),
              pivot_y: av5.slide ? ((av.h | 0) >> 1) : (av.pivot_y | 0),
              order: or5, alpha: 255, n_frames: 1,
              flags: (a5.hidden ? EF_HIDDEN : 0) | AUX_F_SHADOW3 }, a5.shadow, true);
        auxB += (av.w | 0) * (av.h | 0) * 3;
      }
    }
    /* D43 P5d.3: the wheels. Each disc is a turning row standing at its figure 0, and the
     * faces over them are ONE still cover row: the plate's own texels over the discs with
     * the union of their cutouts cut out. All at depth 0 in this table order -- the discs
     * largest first, then the cover -- so they land on the plate and under every window,
     * pointer, carriage and hand (depth_key keeps table order at a tie). */
    var wheels3 = o.wheels || [];
    for (var kw = 0; kw < wheels3.length; kw++) {
      var w3 = wheels3[kw];
      if (!w3.rgba || !(w3.w > 0 && w3.h > 0) || w3.rgba.length !== w3.w * w3.h * 4)
        throw new Error('the wheel ' + q(String(w3.id)) + ' carries no disc: a turning row ships its art');
      if (SOURCES.indexOf(String(w3.src)) < 0 || DIGITS.indexOf(String(w3.digit || 'whole')) < 0 || CADENCES.indexOf(String(w3.cadence)) < 0)
        throw new Error('the wheel ' + q(String(w3.id)) + ' reads ' + q(String(w3.src)) + '/' + q(String(w3.digit)) + ' by ' + q(String(w3.cadence)) + ': not the dial\u2019s vocabulary');
      // D43: a drum's art is its strip (one turn, w3.h tall), placed with its window's top-left at
      // x,y and the window's centre as its pivot; it rolls one strip in its direction per turn
      if (w3.drum && !((w3.radius | 0) >= 2 && (w3.pivot_y | 0) === (w3.radius | 0) && w3.h > w3.radius))
        throw new Error('the drum ' + q(String(w3.id)) + ' carries no radius its strip can roll through');
      row({ n: B.id(), 'class': CLASS_AUX, slot: nextSlot('the wheel ' + q(String(w3.id))), kind: w3.drum ? SHAPE_DRUM : SHAPE_RING,
            src: SOURCES.indexOf(String(w3.src)), digit: DIGITS.indexOf(String(w3.digit || 'whole')),
            x: w3.x | 0, y: w3.y | 0, w: w3.w, h: w3.h, pivot_x: w3.pivot_x | 0, pivot_y: w3.pivot_y | 0,
            radius: w3.drum ? (w3.radius | 0) : 0, dy: w3.drum ? (w3.dir < 0 ? -w3.h : w3.h) : 0,
            order: 0, alpha: 255, n_frames: 1, span: w3.span | 0, zero: w3.zero | 0, vmin: w3.vmin | 0, vmax: w3.vmax | 0,
            end: END_WRAP, cadence: CADENCES.indexOf(String(w3.cadence)), cad_arg: 0, ease: w3.ease | 0 }, w3, true, !!w3.dither);   // ruling 3: a ring or disc may ask
      auxB += w3.w * w3.h * 3;
    }
    if (o.cover) {
      var cv3 = o.cover;
      if (!wheels3.length) throw new Error('a wheel cover with no wheels under it');
      if (!cv3.rgba || !(cv3.w > 0 && cv3.h > 0) || cv3.rgba.length !== cv3.w * cv3.h * 4)
        throw new Error('the wheels\u2019 cover carries no pixels: a still row ships its art');
      row({ n: B.id(), 'class': CLASS_AUX, slot: nextSlot('the wheels\u2019 cover'), kind: SHAPE_ARM, src: SOURCES.indexOf('none'),
            x: cv3.x | 0, y: cv3.y | 0, w: cv3.w, h: cv3.h, order: 0, alpha: 255, n_frames: 1 }, cv3, true, true);
      auxB += cv3.w * cv3.h * 3;
    } else if (wheels3.some(function (w) { return !w.exposed; }))
      throw new Error('wheels with no cover: every disc would show whole over the face');   // exposed rings need none
    /* D43 P4b: a still row — a sub-dial face the author lifted among the hands. It never
     * moves (source none, no cadence, no travel), takes its depth from depthOrder's
     * 'comp:<id>' entry, and is a still, screen-locked plane, so it dithers. */
    var stills = o.stills || [];
    for (var ks = 0; ks < stills.length; ks++) {
      var st = stills[ks], stKey = 'comp:' + st.id;
      if (rung3[stKey] === undefined)
        throw new Error('a lifted face (' + st.id + ') with no place in depthOrder: a still row takes its depth from the stack, and this one has none');
      if (!(st.w > 0 && st.h > 0) || !st.rgba || st.rgba.length !== st.w * st.h * 4)
        throw new Error('a lifted face (' + st.id + ') with no pixels: a still row ships its art or it is not a row');
      row({ n: B.id(), 'class': CLASS_AUX, slot: nextSlot('the lifted face ' + q(String(st.id))), kind: SHAPE_ARM,
            src: SOURCES.indexOf('none'), x: st.x | 0, y: st.y | 0, w: st.w, h: st.h,
            order: Math.max(1, Math.min(63, rung3[stKey])), alpha: 255, n_frames: 1 }, st, true, true);
      auxB += st.w * st.h * 3;
    }
    for (k3 = 0; k3 < compRows.length; k3++) {
      var c4 = compRows[k3];
      // a window a lifted face frames rides just under that face (depthOrder 'bank:<id>')
      var bankOrd = c4.id !== undefined && rung3['bank:' + c4.id] !== undefined ? Math.min(63, rung3['bank:' + c4.id]) : (c4.order | 0);
      rowFrames({ n: B.id(), 'class': classId('bank'), slot: c4.slot, kind: 0, src: SOURCES.indexOf(String(c4.src)),
                  x: c4.x | 0, y: c4.y | 0, w: c4.w, h: c4.h, order: bankOrd, alpha: 255,
                  n_frames: c4.planes.length, scale16: c4.scale16 | 0,
                  flags: c4.hidden ? EF_HIDDEN : 0 }, c4.planes, true, true);
    }
    // D42: the scalars the rows replaced are carried as 0 (CLOCK_RETIRED_SCALARS).
    CLOCK_RETIRED_SCALARS.forEach(function (n) { delete sc[n]; });
    var tempo3 = clockTempo(elems, sc);

    /* A theme's preview: one opaque RGB565 plate the OS picker paints and this dial
     * does not. The caller hands `preview: { plane, x, y }` where plane is the
     * composed dial already downscaled to the preview's own size (a square at half
     * the panel by design). Class 9 slot 0, one frame, at the running end of the
     * bank so no other row's frame0 shifts. A second preview row is refused by
     * checkTable's class:slot uniqueness, not this block. A row's x/y is the caller's
     * picker-slot placement; the shell may scale and place the pixels wherever its
     * card wants, so the pair is authoring metadata the compiled dial ignores. */
    if (o.preview) prevB = B.preview(o.preview);

    /* D39: the sound rows, last in the table (elems[0] stays the plate the delta
     * self-verify reads), each naming its plane at the head of the bank. */
    if (sounds3.length) {
      var patIx = HOUR_PATTERNS.indexOf(o.soundPattern || 'none');
      if (patIx < 0) throw new Error('hour pattern ' + q(String(o.soundPattern)) + ': the clock knows ' + HOUR_PATTERNS.join(', '));
      for (si = 0; si < sounds3.length; si++) {
        var srec = blankElement(), sslot = SOUND_SLOTS.indexOf(sounds3[si].slot), stick = sslot === 0;
        srec.n = B.id(); srec['class'] = CLASS_SOUND; srec.slot = sslot;
        srec.frame0 = si; srec.n_frames = 1; srec.kind = patIx; srec.src = soundEnc[si];
        srec.flags = stick ? EF_LOOP : 0;
        srec.vmax = stick ? (sounds3[si].beat_chz | 0) : 0;
        srec.vmin = stick ? (sounds3[si].first_beat | 0) : 0;
        elems.push(srec);
      }
      if (sc.bg_mode === 1) sc.bg_key_off = soundB;
      checkClockSounds(elems, bank.sprites, tempo3[0], tempo3[1]);
    }

    // The D29 sum, named before the file is written: the plate bank is the part
    // of an over-cap bake an owner can actually trade away, so the sentence says
    // what it cost. checkBudget3 still refuses the same overage at the door —
    // this is the authoring half of §31, and it must never disagree with it.
    if (bank.size > SLOT_ART_CAP3) {
      var why3 = (frames ? ': the plate\'s ' + frames.length + ' keyframes of ' + scrW + 'x' + scrH +
                ' cost ' + (scrW * scrH * 2 * frames.length) + ' B of it, and one keyframe ' +
                'is ' + (scrW * scrH * 2) + ' B' : '') +
        (compB ? (frames ? ', and' : ':') + ' the complication tiles cost ' + compB + ' B' : '') +
        (auxB ? (frames || compB ? ', and' : ':') + ' the pointer plates cost ' + auxB + ' B' : '') +
        (prevB ? (frames || compB || auxB ? ', and' : ':') + ' the preview plane costs ' + prevB + ' B' : '');
      throw new Error('the bake\'s art bank is ' + bank.size + ' B against the ' +
        SLOT_ART_CAP3 + ' B the slot leaves for banks' + why3 +
        ' — drop frames or shrink the art');
    }

    /* D43 P5d.3b: the stage seats a clock's drawn rows in the OS layer table, which holds
     * ORB_LAYER_MAX (16) per app (main/gfx/orb_layers.h); past it the stage refuses the
     * rest and the whole dial falls back to the LVGL path -- measured on the bench, a
     * fraction of the frame rate. Counted the stage's way (clock_layers_build): every
     * element but the plate's canvas, the sounds and a hidden row. */
    var drawn3 = elems.filter(function (e) {
      return !(e['class'] === CLASS_PLATE && (e.slot | 0) === 0) && e['class'] !== classId('sound') && !((e.flags | 0) & EF_HIDDEN);
    }).length;
    // Said on the meter, not refused: a dial past the table still runs, the slow way. Except a
    // drum: only the compositor rolls one (the slow way would slide its strip), so a dial with a
    // drum past the table is refused
    if (drawn3 > ORB_LAYER_MAX3 && elems.some(function (e) { return e['class'] === CLASS_AUX && e.kind === SHAPE_DRUM; }))
      throw new Error('the dial draws ' + drawn3 + ' layers and the orb seats ' + ORB_LAYER_MAX3 + ' at full speed; a drum rolls only there \u2014 drop a part, a shadow, or make a wheel snap');

    // D42: every clock section is v6.
    var secAbi = GEOM_ABI6;
    var pack = function () {
      return B.pack({ name: o.name, serial: o.serial, bakedMs: o.bakedMs, bakedS: o.bakedS, design: o.design, slug: 'clock', scalars: sc, abi: secAbi });
    };
    var b = pack();
    if (deltaBank && !selfVerify3(frames, bank.sprites, (elems[0].frame0 | 0),
                                 !!(elems[0].flags & EF_DELTA_WRAP))) {
      throw new Error('the baked delta bank does not composite back to the frames it was handed' +
        (elems[0].flags & EF_DELTA_WRAP ? ', or close its loop back to the keyframe' : '') +
        ': the round trip drifted, and a drifting chain corrupts every later frame — refused, not shipped');
    }
    /* Slice 5: the bake decides each turned row's sampling from its own price. A dial
     * whose commit still holds 12 fps WITH the AA term gets EF_SMOOTH on every hand,
     * shadow and pointer row but the hot-tick seconds (a sweeping second hand, or a
     * pointer on the seconds source, re-poses every tick and stays nearest — the rule
     * the firmware used to apply itself). o.smooth true/false forces the verdict for a
     * bench arm; the composer never passes it, and the estimate it reports is still the
     * honest one for the bytes. */
    // the hour and minute hands and their twins smooth on every dial (owner 2026-11-02), so the
    // estimate below prices them from the bytes; a bench arm forcing nearest keeps them nearest
    /* §49.15 (owner, 2026-11-05): a dial whose seconds tick once a second or slower gives every
     * move a second to land in, so its hour and minute RINGS smooth like hands there (Orbit);
     * while the seconds sweep or tick faster a ring keeps the old rule and stays nearest. */
    var tick1Dial = elems.some(function (x) {
      return x['class'] === CLASS_AUX && !(x.flags & AUX_F_SHADOW3) && x.src === SOURCES.indexOf('second') &&
             (x.cadence === CAD_JUMP || (x.cadence === CAD_STEP && (x.cad_arg | 0) <= 1));
    });
    var forced = 0, hmPrev = false;
    for (k = 0; k < elems.length; k++) {
      var eh = elems[k];
      if (eh['class'] !== CLASS_AUX) continue;
      var hm = (eh.flags & AUX_F_SHADOW3) ? hmPrev : isHM3(eh) || (tick1Dial && isHM3(Object.assign({}, eh, { kind: 0 })));
      if (!(eh.flags & AUX_F_SHADOW3)) hmPrev = hm;
      if (hm && o.smooth !== false) { eh.flags = (eh.flags | 0) | EF_SMOOTH; forced++; }
    }
    if (forced) b = pack();
    var est = estimate3(verifyBundle3(b.bytes).sections[0]);
    var grant = o.smooth === true ? true : (o.smooth === false ? false : est.smooth);
    /* §49.15 (owner, 2026-11-05): the seconds hand is always smoothed — a sweeping one too, the
     * hot row the old rule kept nearest — and its shadow twin only when o.secShadowSmooth asks
     * (the editor's Smooth shadow switch; off by default: the turning shadow is the dearest
     * filter on the dial). estimate4 prices both from these flags. A bench arm's o.smooth false
     * still forces nearest everywhere. */
    var smoothed = 0, prevHot = false, prevSec3 = false, iSec3 = SOURCES.indexOf('second');
    for (k = 0; k < elems.length; k++) {
      var er = elems[k];
      if (er['class'] !== CLASS_AUX) continue;
      // A twin rides its row (the slot before it): hot with it, cool with it.
      var twin3 = !!(er.flags & AUX_F_SHADOW3);
      var hot = twin3 ? prevHot : !!tempo3[0] && er.src === iSec3;
      var secRow = twin3 ? prevSec3 : er.src === iSec3;
      if (!twin3) { prevHot = hot; prevSec3 = secRow; }
      // the carriage's frames never take the filter (clock_view.c's creation walk)
      if ((er.n_frames | 0) > 1 || o.smooth === false) continue;
      /* smoothing is a must (owner, 2026-11-05): a seconds row smooths whatever its size — a ring
       * or a disc too, now the commit walks only its band (§49.15's gapped spans) — and the plans
       * price what it costs; only its shadow waits on the switch */
      var on = secRow ? (!twin3 || !!o.secShadowSmooth) : (!hot && grant);
      if (on && !(er.flags & EF_SMOOTH)) { er.flags = (er.flags | 0) | EF_SMOOTH; smoothed++; }
    }
    if (smoothed) b = pack();
    b.meter = budget3(b.bytes);
    b.meter.layers = drawn3; b.meter.layerCap = ORB_LAYER_MAX3;
    b.n_elem = elems.length;
    b.est = est;
    b.est.granted = grant;
    return b;
  }

  /* The commit's price, at bake time (docs/history/unified-render-path-design-2026-10-30.md
   * §5): what one flush costs the dial the device will draw, and the rate that buys.
   *
   *   flush_ms = box_kpx x (RESTORE + STATIC x glass) + ink_kpx x (INK + AA) + FIXED
   *
   * box is the rows a flush restores: each per-tick mover's swept box (SWEPT_PAD 3
   * on each side, the firmware's own pad), a carriage's two chord-marked poses, and
   * the delta bank's rects per frame. ink is the rotated ink the painter blends in
   * that box: the movers' own new pose plus EVERY other rotating layer whole — the
   * measured share was ~1.0 on all four dials fitted, because the hour and minute
   * hands sit in every row the seconds wedge marks. The glass is an identity layer
   * under every marked pixel, so it rides the box. The rates are the painter's own,
   * fitted to the slice 1 phase lines on orb-A (v1.1.38, 2026-10-30, DISPLAY_BENCH
   * off) — Station and Skeleton fit, Perpetual and the sprite Orbital check — not
   * the design's first figures: box 500 us/kpx (restore 73-85 + glass 330-355 +
   * the flush gate's compare), rotated hand ink 1,675 out of XIP (the design
   * assumed 185-232), a carriage 720, and te+dma ~24 ms per flush that no pixel
   * term carries. AA is the painter's own bilinear, measured on Station at v1.1.39
   * with the bit on and off inside one window: +28 ms per flush over the 16.1 kpx of
   * smoothed ink this model counts, 1,740 us/kpx (each smoothed texel paid 2.5-2.8 us
   * over its nearest blend). The first figure, 330, was derived from LVGL's AA-off
   * arm and was 5x low. A keyframe bank has no restore plane
   * and draws through LVGL; it is priced on the classic rates instead.
   *
   * The rate is capped by the rung the scheduler will pick from this bake's own
   * scalars (orb_sched_pick_rung, mirrored below), which is why a 4 bps sweep
   * reads 4 however cheap its commit is. `smooth` is slice 5's grant rule: the
   * dial takes bilinear when the estimate WITH the AA term holds 12, and est_fps
   * is priced with the term exactly when it does. */
  var EST3 = { RESTORE: 80, STATIC: 420, INK: 1675, CAR: 720, AA: 1740, FIXED_MS: 24,
               LVGL_PLATE: 555, LVGL_INK: 1300, PAD: 3, SMOOTH_FPS: 12 };
  var RUNG3 = [12, 8, 4, 2, 1];
  function rungClamp3(f) { return f < 4 ? 4 : (f > 15 ? 15 : f); }
  function gcd3(a, b) { while (b) { var t = a % b; a = b; b = t; } return a; }
  /* orb_sched_pick_rung, line for line: the cap starts at the seeded floor and is
   * bound by every FLUID ask; DISTINCT asks must all divide the rung. */
  function pickRung3(asks, floor) {
    var cap = rungClamp3(floor || 1), lcm = 1, over = 0, anyD = 0, slow = 0, i;
    for (i = 0; i < asks.length; i++) {
      var a = asks[i];
      if (!a.fps || a.mode === 'free') continue;
      if (a.mode === 'fluid') { var c = rungClamp3(a.fps); if (c < cap) cap = c; continue; }
      anyD = 1; if (!slow || a.fps < slow) slow = a.fps;
      if (!over) { var l = lcm / gcd3(lcm, a.fps) * a.fps; if (!l || l > 1000) over = 1; else lcm = l; }
    }
    if (!anyD) return cap;
    if (!over) for (i = 0; i < RUNG3.length; i++) if (RUNG3[i] <= cap && RUNG3[i] % lcm === 0) return RUNG3[i];
    return rungClamp3(slow);
  }
  /* sched_fill_asks + sched_seed_floor + sched_tick_period_ms, from the scalars a
   * reader projects (GIF mode assumes the canvas allocates, which it does on every
   * board measured: 7.7 MB free against 434 KB). */
  function rateOf3(sc, plateDelay) {
    var gif = sc.bg_mode === 1 && sc.gif_frames >= 2 && sc.gif_frames <= 32;
    var live = gif || (sc.bg_mode !== 1 && sc.bg_frames > 0);
    var anim = gif || (live && sc.bg_loop) || sc.tourbillon_n || sc.second_sweep;
    if (!anim) return { rate: (!sc.second_sweep && sc.tick_rate >= 2) ? sc.tick_rate : 1, asks: 'hand beat' };
    var asks = [];
    if (gif) { var gt = Math.max(20, plateDelay || Math.round(1000 / Math.max(1, sc.bg_fps || 1))); asks.push({ fps: Math.round(1000 / gt), mode: 'distinct' }); }
    else if (live) asks.push({ fps: Math.max(1, sc.bg_fps), mode: 'distinct' });
    if (sc.tourbillon_n) asks.push({ fps: Math.max(1, sc.tourbillon_fps), mode: 'distinct' });
    if (sc.second_sweep) asks.push({ fps: sc.tick_rate || 15, mode: 'fluid' });
    var floor = gif ? 12 : (live ? 4 : 15);
    return { rate: pickRung3(asks, floor), asks: asks.map(function (a) { return a.mode + ' ' + a.fps; }).join(' + ') };
  }
  /* A plane's ink: its alpha>0 count, and the extent of that ink as corners about
   * the pivot (rotation is about the pivot, so that is the box that swings). */
  function inkOf3(art, spr, e) {
    var w = spr.w, h = spr.h, a0 = spr.off + w * h * 2, n = 0, x0 = w, y0 = h, x1 = -1, y1 = -1;
    if (w === h && spr.size === discPx(w) * 3) {
      // a masked disc (orb_disc.h): its alpha disc, laid back out as the square it reads as
      var sq = new Uint8Array(w * h), at = spr.off + discPx(w) * 2;
      for (var dy = 0; dy < h; dy++) {
        var sp = discSpan(dy, w), m = sp[1] - sp[0] + 1;
        sq.set(art.subarray(at, at + m), dy * w + sp[0]); at += m;
      }
      art = sq; a0 = 0;
    }
    for (var y = 0; y < h; y++) for (var x = 0; x < w; x++) if (art[a0 + y * w + x]) {
      n++; if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y;
    }
    var px = e.pivot_x | 0, py = e.pivot_y | 0;
    return { n: n, pts: n ? [[x0 - px, y0 - py], [x1 + 1 - px, y0 - py], [x0 - px, y1 + 1 - py], [x1 + 1 - px, y1 + 1 - py]] : [] };
  }
  /* The mean swept box of a layer stepping `step` degrees, over a full turn: the
   * union of both poses' rotated ink corners, padded, as apply_angles marks it. */
  function sweptKpx3(pts, step, dx, dy) {
    if (!pts.length) return 0;
    var sum = 0, N = 60;
    for (var k = 0; k < N; k++) {
      var bx0 = 1e9, by0 = 1e9, bx1 = -1e9, by1 = -1e9;
      for (var p = 0; p < 2; p++) {
        var t = (k * 6 + p * step) * Math.PI / 180, cs = Math.cos(t), sn = Math.sin(t);
        for (var q = 0; q < pts.length; q++) {
          var X = pts[q][0] * cs - pts[q][1] * sn, Y = pts[q][0] * sn + pts[q][1] * cs;
          for (var s = 0; s < 2; s++) {
            var XX = X + s * (dx | 0), YY = Y + s * (dy | 0);
            if (XX < bx0) bx0 = XX; if (XX > bx1) bx1 = XX; if (YY < by0) by0 = YY; if (YY > by1) by1 = YY;
          }
        }
      }
      sum += (bx1 - bx0 + 2 * EST3.PAD) * (by1 - by0 + 2 * EST3.PAD);
    }
    return sum / N / 1000;
  }
  /* Owner 2026-11-02: an hour or minute row is always smoothed (EF_SMOOTH), whatever the
   * dial's verdict for its other rows: those hands re-pose rarely and read best antialiased. */
  function isHM3(e) {
    // a hand or pointer, not a turning wheel (a ring row's whole disc is too much ink to filter)
    return !(e.n_frames > 1) && e.kind !== SHAPE_RING &&
      (e.src === SOURCES.indexOf('hour12') || e.src === SOURCES.indexOf('hour24') || e.src === SOURCES.indexOf('minute'));
  }
  function estimate3(sec) {
    var els = sec.blob.elements, spr = sec.blob.sprites, art = sec.art;
    /* D42: the tempo and the carriage are the rows', so the figures the scheduler reads
     * are derived here exactly as orb_clock3.c derives them. */
    var sc = {}, kk;
    for (kk in sec.blob.scalars) sc[kk] = sec.blob.scalars[kk];
    var tp3 = clockTempo(els, sc);
    sc.second_sweep = tp3[0]; sc.tick_rate = tp3[1]; sc.tourbillon_n = 0; sc.tourbillon_fps = 0;
    for (kk = 0; kk < els.length; kk++)
      if (els[kk]['class'] === CLASS_AUX && els[kk].n_frames > 1 && els[kk].delay) {
        sc.tourbillon_n = els[kk].n_frames;
        sc.tourbillon_fps = Math.max(1, Math.round(1000 / els[kk].delay));
      }
    var panel = SCREEN_W * SCREEN_H / 1000;
    var plate = null, glassPx = 0, i;
    for (i = 0; i < els.length; i++) {
      var e0 = els[i], c0 = e0['class'];
      if (c0 === CLASS_PLATE && !(e0.slot & 0xffff)) plate = e0;
      else if (c0 === CLASS_PLATE && !(e0.flags & EF_HIDDEN)) glassPx += inkOf3(art, spr[e0.frame0], e0).n / 1000;
    }
    var r = rateOf3(sc, plate ? plate.delay : 0), R = r.rate;
    var step = sc.second_sweep ? 6 / Math.max(1, R) : 6 / Math.max(1, sc.tick_rate || 1);
    var box = 0, mover = 0, still = 0, aaMover = 0, aaStill = 0, carInk = 0, secHot = false, aaAlways = 0;
    for (i = 0; i < els.length; i++) {
      var e = els[i], c = e['class'];
      if (c !== CLASS_AUX || (e.flags & EF_HIDDEN)) continue;
      if (e.n_frames > 1 && e.delay) {                               // the carriage: chord-marked
        carInk = inkOf3(art, spr[e.frame0], e).n / 1000; box += 2 * carInk; continue;
      }
      // D43: a drum repaints its window every tick (the stage re-marks it, orb_stage.c) and
      // never turns: its whole window is restored and inked each flush, unsmoothed
      if (e.kind === SHAPE_DRUM) { var win = (e.w * 2 * (e.radius | 0)) / 1000; box += win; mover += win; continue; }
      var ink = inkOf3(art, spr[e.frame0], e);
      // A seconds row re-poses every tick; its twin rides it. The hot-tick seconds of a
      // sweeping dial stays nearest.
      var twin = !!(e.flags & AUX_F_SHADOW3);
      var sweeps = twin ? secHot : e.src === SOURCES.indexOf('second');
      if (!twin) secHot = sweeps;
      // a row already carrying EF_SMOOTH (the hour and minute hands and their twins, on every
      // dial since 2026-11-02) pays its AA whatever the verdict on the rest
      if (e.flags & EF_SMOOTH) aaAlways += ink.n / 1000;
      var paysAA = !(sweeps && sc.second_sweep);
      if (sweeps) { box += sweptKpx3(ink.pts, step, 0, 0); mover += ink.n / 1000; if (paysAA) aaMover += ink.n / 1000; }
      else { still += ink.n / 1000; if (paysAA) aaStill += ink.n / 1000; }
    }
    if (plate && sc.bg_mode === 1 && plate.n_frames > 1) {           // the delta bank's own rects
      var rect = 0;
      for (i = 1; i < plate.n_frames; i++) { var sp = spr[plate.frame0 + i]; if (sp) rect += sp.w * sp.h; }
      box += rect / (plate.n_frames - 1) / 1000 * Math.min(1, (sc.bg_fps || R) / R);
    }
    if (box > panel) box = panel;
    var glassCov = glassPx / panel;
    var inkK = mover + still, aaK = aaMover + aaStill;
    var classic = sc.bg_mode !== 1 && sc.bg_frames > 0;
    var flushNear, flushAA;
    if (classic) {
      var redraw = panel * Math.min(1, (sc.bg_fps || R) / R);
      flushNear = flushAA = (redraw * EST3.LVGL_PLATE + (inkK + carInk) * EST3.LVGL_INK) / 1000 + EST3.FIXED_MS;
    } else {
      var base = box * (EST3.RESTORE + EST3.STATIC * glassCov) + inkK * EST3.INK + carInk * EST3.CAR;
      flushNear = (base + aaAlways * EST3.AA) / 1000 + EST3.FIXED_MS;
      flushAA = (base + aaK * EST3.AA) / 1000 + EST3.FIXED_MS;
    }
    var fpsOf = function (ms) { return Math.max(1, Math.min(R, Math.round(1000 / ms))); };
    var nearFps = fpsOf(flushNear), aaFps = fpsOf(flushAA);
    var smooth = !classic && aaK > 0 && 1000 / flushAA >= EST3.SMOOTH_FPS && R >= EST3.SMOOTH_FPS;
    var carrier = classic ? 'keyframe bank (LVGL)' : sc.bg_mode === 1 ? 'delta bank' : sc.tourbillon_n ? 'sprite (carriage over a still)' : 'still';
    var est = smooth ? aaFps : nearFps;
    return { est_fps: est, rung: R, asks: r.asks, carrier: carrier, smooth: smooth,
             flush_ms: +(smooth ? flushAA : flushNear).toFixed(1), near_fps: nearFps, aa_fps: aaFps,
             box_kpx: +box.toFixed(1), ink_kpx: +inkK.toFixed(1), aa_kpx: +aaK.toFixed(1),
             glass_cov: +glassCov.toFixed(3), car_kpx: +carInk.toFixed(1), still_kpx: +still.toFixed(1), mover_kpx: +mover.toFixed(1),
             sentence: 'est ' + est + ' fps (' + carrier + ', rung ' + R + ' from ' + r.asks + '; flush ~' +
               (smooth ? flushAA : flushNear).toFixed(0) + ' ms, ' + (smooth ? 'smooth, AA priced at the Station bench rate' : 'nearest' + (aaK > 0 && !classic ? ': smoothing would land ' + aaFps + ' fps' : '')) + ')' };
  }

  /* estimate4: the price of a clock bake on the ink-span compositor and the 60 Hz grid
   * (app-platform-plan §49.4-49.13), from the bytes alone, for ANY dial: any size of gear
   * cutout, any number of hands, several seconds hands ticking or sweeping in one mix.
   *
   * Geometry. The firmware marks and paints a turning row by its ink per screen row. Each AUX
   * row's alpha is reduced to [umin, umax] per 8-row strip, every strip forward-mapped to a
   * quad at a pose and cut into one padded [x0, x1] per screen row (orb_gfx.c gfx_spans_of).
   * A frame restores, per row, the union of its movers' old and new spans and the bank rect
   * landing on it; every turning row walks its spans in those rows.
   *
   * Cadence, per row (a shadow twin follows its row):
   *   a sweeping or stop-to-go seconds row, a carriage, a drum    every frame
   *   a ticking seconds row, b beats a second (cad_arg)           every (H/b)th frame
   *   the gear bank at G fps                                       every (H/G)th frame
   *   minute and hour rows                                         rarely: priced once, as the
   *                                                                occasional frame they cost
   * A frame rate H holds only if every beat and the gears divide it (7.5: a 1/8 hand moves
   * every frame). One second of frames is walked, each distinct mix of movers priced once:
   *
   *   frame_ms = 0.744 restored_kpx + 0.507 walk_kpx + 1.277 smoothed_kpx
   *            + 0.548 restored_kpx (carriage) + 7.2
   *
   * (§49.15: least squares on 31 direct plans held a minute each at 80 MHz with the shadows as
   * shipped, rms 2.6 ms). A frame fits when its cost x 1.15 + 20 ms is under its shortest
   * period (7.5 alternates 7 and 8 scans). A gear frame
   * that does not fit is pre-painted when the frames between leave the slack to build it and
   * its landing fits (≈ its movers + 0.1 us per rect px); carriage dials are not pre-painted.
   *
   * Storage (owner, 2026-11-05): a bank holds max(8, G) frames — a loop is never under 8
   * frames (4 fps: two seconds, so the movement keeps its detail), never more than one second
   * at the faster tempos — so its bytes are the keyframe plus that many rect deltas (~2 B a
   * px). fps x cutout area is the one currency: the slot pays it in bytes, the compositor in
   * time, and a 20 fps cutout a fifth the area of a 4 fps one costs the same in both. The
   * device holds 20 bank frames (CLK_BG_MAX), one second at the fastest gears. */
  var EST4 = { R: 0.744, W: 0.507, CAR: 0.548, FIXED: 7.225, MARGIN: 20, SLOP: 0.15, IDLE: 26, PREP_K: 1.3, SWAP: 0.1, SCAN: 16.94, STRIP: 8, PAD: 3,
               EPS10: 2, PX_B: 2.06, LOOP_MIN: 8, BANK_MAX: 20, CAR_MAX: 16, HANDS_MIN: 4, AA: 1.277, GAP_MIN: 24 };
  /* The rates a 60 Hz panel holds alike, and 7.5: "8 fps", frames alternating 7 and 8 scans
   * (±8.5 ms, §49.7), or exactly 8 scans when gears ride every second frame of 15. */
  var GRID4 = [20, 15, 12, 10, 7.5, 6, 5, 4, 3, 2, 1];
  /* The gear rates a bank may be baked at: the grid at or over the owner's floor of 4. */
  var GEARS4 = [20, 15, 12, 10, 7.5, 6, 5, 4];
  /* A plan's name in the editor's words: gears on hands ("4/20"). */
  function planKey4(x) { return (x.gears || 0) + '/' + x.fps; }
  /* The favour's plan among any set of plans (the frontier of one estimate, or the editor's
   * plans each priced on a bank drawn at its own tempo): 'sweep' the fastest grid, then the
   * fastest gears on it; 'gears' the fastest gears, then the grid; 'balanced' the largest smaller
   * share of the set's best grid and best gears, then the evener of the two shares. */
  function bestPlan4(plans, favour) {
    if (!plans || !plans.length) return null;
    var hMax = Math.max.apply(null, plans.map(function (x) { return x.fps; }));
    var gMax = Math.max.apply(null, plans.map(function (x) { return x.gears; })) || 1;
    var share = function (x) { return Math.min(x.fps / hMax, (x.gears || gMax) / gMax); };
    var skew = function (x) { return Math.abs(x.fps / hMax - (x.gears || gMax) / gMax); };
    var cmp = favour === 'gears' ? function (p, q) { return q.gears - p.gears || q.fps - p.fps; }
            : favour === 'balanced' ? function (p, q) { return share(q) - share(p) || skew(p) - skew(q) || q.fps * (q.gears || 1) - p.fps * (p.gears || 1); }
            : function (p, q) { return q.fps - p.fps || q.gears - p.gears; };
    return plans.slice().sort(cmp)[0];
  }
  /* The plan a bake asks: an exact pick ("6/12") when it is still on the frontier, else the
   * favour's. Pure, so the editor and the suite read one answer. */
  function pickPlan4(est, favour, key) {
    if (!est || !est.plans || !est.plans.length) return null;
    if (key) for (var i = 0; i < est.plans.length; i++) if (planKey4(est.plans[i]) === key) return est.plans[i];
    return (est.picks && est.picks[favour]) || est.picks.sweep;
  }
  /* Each strip's ink, as the firmware's ink cache keeps it: [lo, hi] and the widest inkless
   * gap inside (§49.15, orb_gfx.c INK_GAP_MIN), so a ring's hollow costs nothing. */
  function inkStrips4(art, sp) {
    var a0 = sp.off + sp.w * sp.h * 2, ns = Math.ceil(sp.h / EST4.STRIP), out = [];
    for (var st = 0; st < ns; st++) {
      var lo = 1e9, hi = -1, col = new Uint8Array(sp.w);
      for (var v = st * EST4.STRIP; v < Math.min(sp.h, (st + 1) * EST4.STRIP); v++)
        for (var u = 0; u < sp.w; u++) if (art[a0 + v * sp.w + u]) { col[u] = 1; if (u < lo) lo = u; if (u > hi) hi = u; }
      var g0 = 1, g1 = 0;
      for (var x = lo + 1; x < hi; ) {
        if (col[x]) { x++; continue; }
        var e = x;
        while (e + 1 < hi && !col[e + 1]) e++;
        if (e - x > g1 - g0) { g0 = x; g1 = e; }
        x = e + 1;
      }
      if (g1 - g0 + 1 < EST4.GAP_MIN) { g0 = 1; g1 = 0; }
      out.push([lo, hi, g0, g1]);
    }
    return out;
  }
  /* One row's set as the firmware keeps it (orb_gfx.c seg_add): [x0, x1] less the gap [h0, h1]. */
  function segAdd4(S, y, a, b) {
    if (S.x0[y] > S.x1[y]) { S.x0[y] = a; S.x1[y] = b; S.h0[y] = 1; S.h1[y] = 0; return; }
    var p = S.h0[y] <= S.h1[y] ? [[S.x0[y], S.h0[y] - 1], [S.h1[y] + 1, S.x1[y]]] : [[S.x0[y], S.x1[y]]];
    p.push([a, b]); p.sort(function (q, r) { return q[0] - r[0]; });
    var m = [p[0].slice()];
    for (var i = 1; i < p.length; i++) { var t = m[m.length - 1]; if (p[i][0] <= t[1] + 1) { if (p[i][1] > t[1]) t[1] = p[i][1]; } else m.push(p[i].slice()); }
    S.x0[y] = m[0][0]; S.x1[y] = m[m.length - 1][1];
    var g0 = 1, g1 = 0;
    for (var k = 0; k + 1 < m.length; k++) if (m[k + 1][0] - m[k][1] - 1 > g1 - g0 + 1) { g0 = m[k][1] + 1; g1 = m[k + 1][0] - 1; }
    S.h0[y] = g0; S.h1[y] = g1;
  }
  function segNew4() {
    return { x0: new Int16Array(SCREEN_H).fill(32767), x1: new Int16Array(SCREEN_H).fill(-1),
             h0: new Int16Array(SCREEN_H).fill(1), h1: new Int16Array(SCREEN_H).fill(0) };
  }
  function segPieces4(S, y) {
    if (S.x0[y] > S.x1[y]) return [];
    return S.h0[y] <= S.h1[y] ? [[S.x0[y], S.h0[y] - 1], [S.h1[y] + 1, S.x1[y]]] : [[S.x0[y], S.x1[y]]];
  }
  function spans4(row, ang10) {
    var S = segNew4();
    var t = -(ang10 % 3600) / 10 * Math.PI / 180, sn = Math.sin(t), cs = Math.cos(t);
    var e = row.e, cx = e.x + e.pivot_x, cy = e.y + e.pivot_y, px = e.pivot_x, py = e.pivot_y, P = EST4.PAD;
    var runs = [];
    row.ink.forEach(function (lh, st) {
      if (lh[1] < lh[0]) return;
      if (lh[2] <= lh[3]) { runs.push([lh[0], lh[2] - 1, st]); runs.push([lh[3] + 1, lh[1], st]); }
      else runs.push([lh[0], lh[1], st]);
    });
    runs.forEach(function (rn) {
      var st = rn[2];
      var u0 = rn[0] - P - px, u1 = rn[1] + 1 + P - px;
      var v0 = st * EST4.STRIP - P - py, v1 = Math.min(row.sp.h, (st + 1) * EST4.STRIP) + P - py;
      var q = [[u0, v0], [u1, v0], [u1, v1], [u0, v1]].map(function (c) { return [cx + cs * c[0] + sn * c[1], cy - sn * c[0] + cs * c[1]]; });
      var ys = q.map(function (c) { return c[1]; });
      var ya = Math.max(0, Math.floor(Math.min.apply(null, ys))), yb = Math.min(SCREEN_H - 1, Math.floor(Math.max.apply(null, ys)));
      for (var y = ya; y <= yb; y++) {
        var mn = 1e9, mx = -1e9;
        for (var k = 0; k < 4; k++) {
          var a = q[k], b = q[(k + 1) & 3], lo = Math.max(Math.min(a[1], b[1]), y), hi = Math.min(Math.max(a[1], b[1]), y + 1);
          if (lo > hi) continue;
          var sl = b[1] !== a[1] ? (b[0] - a[0]) / (b[1] - a[1]) : 0;
          var xa = b[1] !== a[1] ? a[0] + (lo - a[1]) * sl : a[0], xb = b[1] !== a[1] ? a[0] + (hi - a[1]) * sl : b[0];
          mn = Math.min(mn, xa, xb); mx = Math.max(mx, xa, xb);
        }
        if (mn > mx) continue;
        var l = Math.max(0, Math.floor(mn) - 1), r = Math.min(SCREEN_W - 1, Math.floor(mx) + 2);
        segAdd4(S, y, l, r);
      }
    });
    return S;
  }
  /* o.gears: price the dial as if its bank played at that rate (a re-bake's question: "what
   * if the gears ran at 8?"). The bank's own frames are priced; only the tempo moves. */
  function estimate4(sec, o) {
    o = o || {};
    var els = sec.blob.elements, spr = sec.blob.sprites, art = sec.art, kk, i;
    var sc = {};
    for (kk in sec.blob.scalars) sc[kk] = sec.blob.scalars[kk];
    var iSec = SOURCES.indexOf('second'), iMin = SOURCES.indexOf('minute'), iH12 = SOURCES.indexOf('hour12'), iH24 = SOURCES.indexOf('hour24');
    /* every moving row, its cadence, and where it stands at 10:08:30 */
    var rows = [], parent = null, carriage = false;
    for (i = 0; i < els.length; i++) {
      var e = els[i];
      if (e['class'] !== CLASS_AUX || (e.flags & EF_HIDDEN)) continue;
      var twin = !!(e.flags & AUX_F_SHADOW3) && parent;
      var src = twin ? parent.src : e.src, cad = twin ? parent.cad : (e.cadence | 0), arg = twin ? parent.arg : (e.cad_arg | 0);
      var car = e.n_frames > 1 && e.delay > 0;
      if (car) carriage = true;
      var kind = e.kind === SHAPE_DRUM ? 'frame' : car ? 'frame'
               : src === iSec && (cad === CAD_SWEEP || cad === CAD_STOP_TO_GO) ? 'sweep'
               : src === iSec ? 'step'
               : (src === iMin || src === iH12 || src === iH24) ? 'slow' : 'still';
      var frac = src === iSec ? 0.5 : src === iMin ? 8.5 / 60 : (src === iH12 || src === iH24) ? (10 + 8.5 / 60) / 12 : 0;
      var r = { e: e, sp: spr[e.frame0], src: src, cad: cad, arg: arg, kind: kind, drum: e.kind === SHAPE_DRUM,
                smooth: !!(e.flags & EF_SMOOTH),
                /* a carriage repaints every frame: its frames swap at their own tempo, but the cage
                 * turns with the seconds angle on every one (§49.15: Kitetour 5/10 commits 166 ms a
                 * frame on the glass, every frame) */
                beats: kind === 'step' ? Math.max(1, arg || 1) : 0,
                /* a twin stores no turn of its own (zero 0, span 0): it stands where its hand
                 * stands. §49.15: posed at 0 it read as unrotated and was never priced. */
                ang: twin ? parent.ang : ((e.zero | 0) + Math.round(frac * (e.span | 0))) % 3600,
                ink: e.kind === SHAPE_DRUM ? [] : inkStrips4(art, spr[e.frame0]) };
      rows.push(r);
      if (!twin) parent = r;
    }
    var plate = null;
    for (i = 0; i < els.length; i++) if (els[i]['class'] === CLASS_PLATE && !(els[i].slot & 0xffff)) plate = els[i];
    var bank = [], bankFps = 0, bankBytes = 0;
    if (plate && sc.bg_mode === 1 && plate.n_frames > 1) {
      for (i = 0; i < plate.n_frames; i++) if (spr[plate.frame0 + i]) { bankBytes += spr[plate.frame0 + i].size; if (i) bank.push(spr[plate.frame0 + i]); }
      bankFps = plate.delay ? 1000 / plate.delay : (sc.bg_fps || 0);
    }
    /* o.bank (§49.15, the editor's live price): the bank as the editor measured it — one
     * changed box per frame off its own rendered movement — standing in for a baked delta bank,
     * so the plans can be priced before anything is baked. o.bankFps is the tempo it was drawn at. */
    if (o.bank && o.bank.length) {
      bank = o.bank.filter(function (b) { return b.w > 0 && b.h > 0; })
                   .map(function (b) { return { w: b.w, h: b.h, diffx: b.diffx, diffy: b.diffy, size: b.w * b.h * 3 }; });
      bankFps = o.bankFps || bankFps || 4;
      bankBytes = 0;
    }
    var rectPx = 0, worstB = null;
    bank.forEach(function (b) { rectPx += b.w * b.h; if (!worstB || b.w * b.h > worstB.w * worstB.h) worstB = b; });
    rectPx = bank.length ? rectPx / bank.length : 0;
    /* §49.15: a frame holds its slot or misses it on its own, so the gear frame is priced on
     * the bank's LARGEST rect (Regulator's run 6 to 61 kpx: the mean 34 underpriced its worst
     * gear frames by ~20 ms on the glass). The slot's bytes stay on the mean. */
    var worstPx = worstB ? worstB.w * worstB.h : 0;
    var keyBytes = o.bank && bank.length ? SCREEN_W * SCREEN_H * 2 : plate && bank.length ? spr[plate.frame0].size : 0;
    var slotFree = SLOT_ART_CAP3 - (art.length - bankBytes);           // the slot without the bank
    var framesAt = function (G) { return G ? Math.max(EST4.LOOP_MIN, Math.round(G)) : 0; };
    var bytesAt = function (G) { return G ? Math.round(keyBytes + framesAt(G) * rectPx * EST4.PX_B) : 0; };

    /* One frame's price: these rows moving (their step, in tenths), with or without a bank
     * rect, averaged over 12 places round the dial. */
    var cache = {};
    var price = function (moving, withBank) {
      var key = moving.map(function (m) { return m[0] + ':' + m[1]; }).join(',') + (withBank ? '|b' + withBank : '');
      if (cache[key]) return cache[key];
      var R = 0, Wk = 0, Sk = 0, n = 12;
      for (var k = 0; k < n; k++) {
        var RS = segNew4(), rx0 = RS.x0, rx1 = RS.x1;
        var mark = function (sp) { for (var y = 0; y < SCREEN_H; y++) segPieces4(sp, y).forEach(function (q) { segAdd4(RS, y, q[0], q[1]); }); };
        var turn = (k * 300) % 3600;
        var pose = rows.map(function (r) { return r.src === iSec ? (r.ang + turn) % 3600 : r.ang; });
        var newPose = pose.slice();
        moving.forEach(function (m) {
          var j = m[0], r = rows[j];
          if (r.drum) {
            var rr = r.e.radius | 0, y0 = r.e.y + r.e.pivot_y - rr;
            for (var y = Math.max(0, y0); y < Math.min(SCREEN_H, y0 + 2 * rr); y++) segAdd4(RS, y, r.e.x, r.e.x + r.e.w - 1);
            return;
          }
          newPose[j] = (pose[j] + m[1]) % 3600;
          mark(spans4(r, pose[j])); mark(spans4(r, newPose[j]));
        });
        if (withBank && bank.length) {
          var b = withBank === 'mean' ? bank[k % bank.length] : worstB;
          for (var y2 = b.diffy; y2 < b.diffy + b.h && y2 < SCREEN_H; y2++) segAdd4(RS, y2, b.diffx, b.diffx + b.w - 1);
        }
        var r0 = 0, w0 = 0, s0 = 0;
        for (var y3 = 0; y3 < SCREEN_H; y3++) segPieces4(RS, y3).forEach(function (q) { r0 += q[1] - q[0] + 1; });
        if (r0) rows.forEach(function (row, j) {
          if (row.drum || newPose[j] % 3600 === 0) return;
          var sp = spans4(row, newPose[j]), wr = 0;
          for (var y4 = 0; y4 < SCREEN_H; y4++) {
            if (sp.x0[y4] > sp.x1[y4] || rx0[y4] > rx1[y4]) continue;
            var jp = segPieces4(sp, y4), rp = segPieces4(RS, y4);
            for (var i1 = 0; i1 < jp.length; i1++) for (var i2 = 0; i2 < rp.length; i2++) {
              var a = Math.max(jp[i1][0], rp[i2][0]), bb = Math.min(jp[i1][1], rp[i2][1]);
              if (bb >= a) wr += bb - a + 1;
            }
          }
          w0 += wr; if (row.smooth) s0 += wr;
        });
        R += r0; Wk += w0; Sk += s0;
      }
      R /= n * 1000; Wk /= n * 1000; Sk /= n * 1000;
      /* §49.15: a smoothed row (EF_SMOOTH, bilinear) pays AA more per walked texel than nearest */
      var ms = R || withBank ? EST4.R * R + EST4.W * Wk + EST4.AA * Sk + (carriage ? EST4.CAR * R : 0) + EST4.FIXED : EST4.FIXED;
      return (cache[key] = { restored_kpx: +R.toFixed(1), walk_kpx: +Wk.toFixed(1), smooth_kpx: +Sk.toFixed(1), ms: +ms.toFixed(1) });
    };
    var slowSet = [];
    rows.forEach(function (r, j) { if (r.kind === 'slow') slowSet.push([j, EST4.EPS10]); });

    /* One grid plan: H frames a second, the gears at G. Walks one second of frames. */
    /* what a frame needs of its period: its price, the jitter that grows with it (a 140 ms
     * carriage frame misses with 32 ms to spare, a 41 ms one holds with 27), and the
     * refresh wait (§49.15, the shipped-shadow run) */
    var need4 = function (ms) { return ms * (1 + EST4.SLOP) + EST4.MARGIN; };
    var judge = function (H, G) {
      var per = Math.floor(60 / H) * EST4.SCAN, frames = Math.round(H === 7.5 ? 15 : H);
      var res = { fps: H, gears: G, scans: +(60 / H).toFixed(1), period_ms: +per.toFixed(1), prepaint: false, ok: false, why: '' };
      /* every beat and the gears must land on whole frames */
      var every = [];
      for (var j = 0; j < rows.length; j++) {
        var r = rows[j];
        if (r.kind !== 'step') continue;
        var ev = r.beats >= H ? 1 : H / r.beats;
        if (r.beats < H && Math.abs(ev - Math.round(ev)) > 1e-6) { res.why = 'a 1/' + r.beats + ' hand does not land on every Nth frame at ' + H; return res; }
        every[j] = Math.max(1, Math.round(ev));
      }
      var gEvery = G ? H / G : 0;
      if (G && Math.abs(gEvery - Math.round(gEvery)) > 1e-6) { res.why = 'the gears at ' + G + ' do not land on every Nth frame at ' + H; return res; }
      gEvery = Math.round(gEvery);
      if (G && bank.length && framesAt(G) > EST4.BANK_MAX) {
        res.why = 'the gears at ' + G + ' need ' + framesAt(G) + ' bank frames, the device holds ' + EST4.BANK_MAX; return res;
      }
      if (G && carriage && framesAt(G) > EST4.CAR_MAX) {
        res.why = 'the carriage at ' + G + ' needs ' + framesAt(G) + ' frames, the device holds ' + EST4.CAR_MAX; return res;
      }
      if (G && bank.length && bytesAt(G) > slotFree) {
        res.why = 'the gears at ' + G + ' need ' + (bytesAt(G) / 1048576).toFixed(1) + ' MB, the slot leaves ' + (slotFree / 1048576).toFixed(1); return res;
      }
      /* the frames of one second (two at 7.5), each its own mix of movers */
      var types = {}, list = [], worstNo = null, worstGear = null, slack = 0, gears = 0, gearCosts = [];
      for (var f = 0; f < frames; f++) {
        var mv = [];
        rows.forEach(function (r, j) {
          if (r.kind === 'sweep' || r.kind === 'frame') mv.push([j, Math.round(60 / H)]);
          else if (r.kind === 'step' && f % every[j] === 0) mv.push([j, Math.round(60 / r.beats)]);
        });
        var gear = !!(G && bank.length && f % gEvery === 0);
        /* a frame fits on its own, so it is priced on the largest rect; a pre-paint is built
         * across the loop, one gear frame at a time, and is priced on the mean (a late build of
         * the one large frame lands one gear frame a loop late — Skeleton 4/20 holds 97% so) */
        var c = price(mv, gear ? 'worst' : false), cNo = gear ? price(mv, false) : c, cMean = gear ? price(mv, 'mean') : c;
        list.push({ gear: gear, ms: c.ms, mean: cMean.ms, without: cNo.ms, R: cMean.restored_kpx, W: cMean.walk_kpx, S: cMean.smooth_kpx });
        if (gear) { gears++; gearCosts.push(c); if (!worstGear || c.ms > worstGear.ms) worstGear = c; }
        if (!worstNo || cNo.ms > worstNo.ms) worstNo = cNo;
      }
      res.frame_ms = worstNo.ms; res.gear_ms = worstGear ? worstGear.ms : null;
      res.mean_ms = +(list.reduce(function (a, fr) { return a + fr.ms; }, 0) / list.length).toFixed(1);
      /* the frame's terms averaged over the plan's second, for refitting EST4 on the glass */
      res.terms = ['R', 'W', 'S'].reduce(function (o, k) { o[k] = +(list.reduce(function (a, fr) { return a + fr[k]; }, 0) / list.length).toFixed(2); return o; }, {});
      if (need4(worstNo.ms) > per) { res.why = 'a frame (' + worstNo.ms + ' ms) does not fit ' + res.period_ms + ' ms'; return res; }
      /* the occasional slow move (a minute or hour row past its 0.2° gate), on the dearest frame */
      if (slowSet.length) res.slow_ms = price(slowSet, false).ms;
      if (!worstGear || need4(worstGear.ms) <= per) { res.ok = true; return res; }
      if (carriage) { res.why = 'a gear frame (' + worstGear.ms + ' ms) does not fit ' + res.period_ms + ' ms, and a carriage dial is not pre-painted'; return res; }
      if (gEvery < 2) { res.why = 'a gear frame (' + worstGear.ms + ' ms) does not fit ' + res.period_ms + ' ms, and every frame is one'; return res; }
      list.forEach(function (fr) { if (!fr.gear) slack += per - EST4.IDLE - fr.ms; });
      slack = slack / Math.max(1, gears);
      var build = 0, swap = 0;
      list.forEach(function (fr) { if (fr.gear) { build = Math.max(build, fr.mean - fr.without); swap = Math.max(swap, fr.without + EST4.SWAP * worstPx / 1000); } });
      res.build_ms = +build.toFixed(1); res.slack_ms = +slack.toFixed(1); res.swap_ms = +swap.toFixed(1);
      if (need4(swap) > per) { res.why = 'even pre-painted, the gear landing (' + res.swap_ms + ' ms) does not fit'; return res; }
      if (slack < build * EST4.PREP_K) { res.why = 'the frames between leave ' + res.slack_ms + ' ms to build a ' + res.build_ms + ' ms gear frame ahead'; return res; }
      res.ok = true; res.prepaint = true; return res;
    };

    /* The gears' tempo on the grid: the bank's own (14.93 is 15; 8 rides 7.5), or o.gears. */
    var gearOn = function (B) {
      if (!B) return 0;
      if (Math.abs(B - Math.round(B)) < 0.3) B = Math.round(B);
      if (B === 8) return 7.5;
      for (var g = 0; g < GRID4.length; g++) if (GRID4[g] <= B) return GRID4[g];
      return 1;
    };
    var G = gearOn(o.gears || bankFps);
    var sweeps = rows.some(function (r) { return r.kind === 'sweep'; });
    var bMax = 0;
    rows.forEach(function (r) { if (r.kind === 'step' && r.beats > bMax) bMax = r.beats; });
    var tp = clockTempo(els, sc), want = sweeps ? tp[1] : Math.max(1, bMax || tp[1] || 1);
    if (want === 8) want = 7.5;
    var opts = GRID4.map(function (H) { return judge(H, G); });
    var fits = opts.filter(function (x) { return x.ok; });
    var atAsk = fits.filter(function (x) { return x.fps >= want - 0.01; });
    var plan = atAsk.length ? atAsk[atAsk.length - 1] : fits.filter(function (x) { return x.fps <= want; })[0] || fits[fits.length - 1] || opts[opts.length - 1];
    var best = fits[0] || plan;

    /* The plans (owner, 2026-11-05): fps x cutout area is the one currency, so the gear rate is
     * searched, not tabled. Every gear rate on GEARS4 is tried on every hand grid it lands on
     * (each beat and the gears divide the grid), against the slot, the device's 20-frame bank,
     * and the frame time; a small cutout runs its gears as fast as a big one runs its sweep.
     * What fits is cut to its frontier — no plan another beats on both the hands and the gears
     * — and FAVOUR picks from it: 'sweep' the fastest grid, then the fastest gears on it;
     * 'gears' the fastest gears, then the fastest grid; 'balanced' the plan nearest the best of
     * both (the largest smaller share of the frontier's best grid and best gears, then the
     * evener of the two shares). A ticking
     * seconds hand moves on its beats whatever the grid, so there the plan is the fastest gears
     * on the slowest grid that carries them, and every favour is that one. */
    var policy = null, pick = null, plans = [], picks = null;
    var hb = o.hands ? (o.hands === 'sweep' ? 0 : +o.hands) : (sweeps ? 0 : bMax || 0);
    var sweepMode = !hb && (sweeps || o.hands === 'sweep');
    var keepRows = rows;
    if (o.hands) {
      /* price the asked hand mode: every seconds row ticks or sweeps as asked */
      rows = rows.map(function (r) {
        if (r.src !== iSec || r.kind === 'frame') return r;
        var c2 = Object.assign({}, r);
        if (hb) { c2.kind = 'step'; c2.beats = hb; } else { c2.kind = 'sweep'; c2.beats = 0; }
        return c2;
      });
      cache = {};
    }
    var gearSet = bank.length ? GEARS4 : [0];
    policy = [];
    gearSet.forEach(function (g) {
      GRID4.forEach(function (H) {
        /* a sweep never under the floor of 4; a tick on no gears may rest on its own beat */
        if ((g || sweepMode) && H < EST4.HANDS_MIN) return;
        if (g && H < g) return;
        if (!g && !sweepMode && H < Math.max(1, hb || bMax || 1)) return;
        if (g && Math.abs(H / g - Math.round(H / g)) > 1e-6) return;
        policy.push(judge(H, g));
      });
    });
    rows = keepRows; cache = {};
    var fit4 = policy.filter(function (x) { return x.ok; });
    if (!sweepMode) {
      /* ticking: per gear rate, the slowest grid that carries it */
      var byG = {};
      fit4.forEach(function (x) { if (!byG[x.gears] || x.fps < byG[x.gears].fps) byG[x.gears] = x; });
      plans = Object.keys(byG).map(function (k) { return byG[k]; }).sort(function (p, q) { return q.gears - p.gears; });
      if (plans.length) plans = [plans[0]];
    } else {
      plans = fit4.filter(function (x) {
        return !fit4.some(function (y) { return y !== x && y.fps >= x.fps && y.gears >= x.gears && (y.fps > x.fps || y.gears > x.gears); });
      }).sort(function (p, q) { return q.fps - p.fps || q.gears - p.gears; });
    }
    if (plans.length) {
      picks = { sweep: bestPlan4(plans, 'sweep'), gears: bestPlan4(plans, 'gears'), balanced: bestPlan4(plans, 'balanced') };
      pick = picks[o.favour] || picks.sweep;
    }
    var s = 'est ' + plan.fps + ' fps on the 60 Hz grid (' + plan.scans + ' scans a frame' +
            (G ? ', gears ' + G + ' fps' : '') + (plan.prepaint ? ', pre-painted' : '') + '; frames up to ~' + plan.frame_ms + ' ms' +
            (plan.gear_ms ? ', gear frames ~' + plan.gear_ms + ' ms' : '') + ')' + (plan.fps < want ? '; asks ' + want : '') +
            (best.fps > plan.fps ? '; could run ' + best.fps + (best.prepaint ? ' (pre-painted)' : '') : '') +
            (pick ? '; plan: gears ' + pick.gears + ' on ' + pick.fps + (pick.prepaint ? ' (pre-painted)' : '') +
                    (plans.length > 1 ? ' of ' + plans.map(planKey4).join(', ') : '') : '');
    return { est_fps: plan.fps, gears_fps: G, prepaint: plan.prepaint, scans: plan.scans, want_fps: want,
             max_fps: best.fps, max_prepaint: best.prepaint, policy: policy, policy_pick: pick, plans: plans, picks: picks,
             hand_mode: sweepMode ? 'sweep' : String(hb || 1),
             bank_fps: +(bankFps || 0).toFixed(2), carriage: carriage, rect_kpx: +(rectPx / 1000).toFixed(1),
             slot_free: slotFree, bank_bytes: bankBytes, bytes_at: bytesAt, frames_at: framesAt,
             movers: rows.filter(function (r) { return r.kind !== 'still'; }).map(function (r) { return { slot: r.e.slot, kind: r.kind, beats: r.beats }; }),
             options: opts, sentence: s };
  }

  /* §31's motion split, as one pure function so the sweep page, the composer's
   * export, and the headless suite all answer from the same arithmetic. Input
   * is a motion measurement, not pixels — the caller composes and diffs;
   * this only decides, from §31's ruled cap arithmetic:
   *
   *   m = { frames, fps, unionPx, bboxW, bboxH, otherB, compB, auxB, hasCage }
   *
   * Order of preference is cost, not novelty: a local motion on a caged dial
   * rides the carriage at a fraction of a full-panel bank (648 KB against
   * 6.5 MB, §31's own numbers), a whole-panel motion is a keyframe bank, and
   * a bank thins (frame step, fps over it) before it refuses, because
   * §23.11's rule is that the shipped bake carries the tempo that fits.
   * A union under one in a thousand of the panel is antialiasing shimmer —
   * RGB565 quantisation swallows it, and the honest bake is the still.
   * Returns { carrier, keyframes, fps, decim, sentence }. */
  /* The carrier split. Two decisions, not one:
   *
   * 1. THE CARRIAGE ROW. On a caged dial the carriage is priced on m.carriageW/H, which
   *    MUST be the rect GorbBake.carriageRect3 answers for the cage planes — the padded,
   *    screen-clamped cycle-wide union, alpha > 0, the rect bakeTourbillon crops to. Any
   *    tighter estimate under-bills: a measure that thresholded ink at alpha >= 8 and
   *    skipped the pad priced a 402x359 row and shipped a 466x466 one, and the bake blew
   *    the slot by 2.9 MB on art the policy had already been told to expect. The
   *    plate-box fallback (price off bboxW/H when the caller has no cage measure) is the
   *    legacy shape, which every pre-existing vector uses. The row is mandatory when
   *    m.orbital: the carriage IS the seconds hand, and a plate keyframe bank is
   *    blitted unrotated, so a bank ships a dial whose escapement animates and whose
   *    orbit is frozen at the instant the walk sampled it. That is the defect
   *    app-platform-plan.md already records of kitetour — "it had no carriage row, so
   *    its tourbillon never turned" — and locality was how it hid: an orbital arrow's
   *    own sweep covers the dial, so a rule that demands a local box can never price
   *    the one motion that proves the carriage turns. If the row does not fit, refuse.
   *
   * 2. THE PLATE. Measured on its own motion (un-caged, so the orbit does not price
   *    it), and billed against the room the carriage left.
   */
  /* The answer below, plus the commit's price when the caller has one (m.est, an
   * estimate3 of the baked bytes): est_fps and the rung ride the answer, and the
   * sentence names them, so the rate is said before the press like every refusal. */
  function splitPolicy3(m) {
    var r = splitPolicyCore3(m);
    if (m.est && r.carrier !== 'refuse') {
      r.est_fps = m.est.est_fps; r.rung = m.est.rung;
      r.sentence = r.sentence + '; ' + m.est.sentence;
    }
    return r;
  }
  function splitPolicyCore3(m) {
    var panel = SCREEN_W * SCREEN_H, kf = panel * 2, cap = SLOT_ART_CAP3;
    var oth = m.otherB | 0, comp = m.compB | 0, aux = m.auxB | 0, B = m.frames | 0, fps = m.fps | 0;
    comp += aux;   // D34: a pointer plate is art the slot carries before any bank frame fits
    var bw = m.bboxW | 0, bh = m.bboxH | 0, area = bw * bh;
    var thin = function (n, step) { return { keyframes: Math.floor(n / step), fps: Math.max(1, Math.round(fps / step)), decim: step }; };
    var no = function (s) { return { carrier: 'refuse', keyframes: 0, fps: 0, decim: 1, carriage: null, sentence: s }; };
    // One shape for every answer: the PLATE's carrier and length at the top level,
    // the carriage ROW (its own length, tempo and box) under .carriage or null.
    var out = function (carrier, plate, row, s) {
      return { carrier: carrier, keyframes: plate.keyframes, fps: plate.fps, decim: plate.decim,
               bbox: plate.bbox || null, carriage: row, sentence: s };
    };
    if (B < 2 || fps < 1) return no('no measured loop to bake: frames ' + B + ' at ' + fps + ' fps');

    /* --- 1. the carriage row, priced first because the indication comes first ---
     * The walk composed this dial's plate WITHOUT the carriage, so a caged dial that
     * ships no row ships no carriage at all. Every caged dial therefore either gets
     * its row or refuses: dropping art the bake was handed is the one answer worse
     * than the frozen orbit this slice exists to stop. */
    var car = null, carCost = 0;
    if (m.hasCage) {
      var cw = m.carriageW | 0, ch = m.carriageH | 0, cArea = cw * ch;
      if (!cArea)
        return no('a caged dial whose carriage box was not measured: the plate is baked '
          + 'without the carriage, so pricing the row off the plate box would either refuse a '
          + 'carriage that fits or ship a dial missing its tourbillon');
      var per = cArea * 3, maxSp = Math.min(TOURB_MAX, Math.floor((cap - oth - comp) / per));
      var s2 = maxSp >= 2 ? thin(B, Math.ceil(B / maxSp)) : { keyframes: 0, fps: 0, decim: 1 };
      if (s2.keyframes < 2)
        return no('the ' + (m.orbital ? 'orbital carriage is the seconds hand' : 'carriage is a live layer this bake strips from the plate')
          + ' and does not fit: ' + B + ' frames of ' + cw + 'x' + ch + ' cost ' + (2 * per)
          + ' B for the two a loop needs against the ' + (cap - oth - comp) + ' B the slot leaves after '
          + oth + ' B of art' + (comp ? ' and ' + comp + ' B of complication tiles' : '')
          + (m.orbital ? ' — a plate bank cannot turn it, and this bake will not ship a frozen orbit'
                       : ' — and the plate it would ride is baked without it'));
      carCost = s2.keyframes * per;
      car = { keyframes: s2.keyframes, fps: s2.fps, decim: s2.decim, bbox: [cw, ch], bytes: carCost,
              sentence: 'carriage bank at the ' + cw + 'x' + ch + ' box: ' + s2.keyframes
                + ' masked frames, ' + carCost + ' B' };
    }

    /* --- 2. the plate, off its own motion and the room the carriage left --- */
    if (!(m.unionPx > panel / 1000))
      return out(car ? 'sprite' : 'still', { keyframes: 1, fps: 0, decim: 1 }, car,
                 (car ? 'plate under the shimmer band, ' + car.sentence
                      : 'plate union ' + (m.unionPx | 0) + ' px is under the shimmer band; the still bakes the same dial'));
    var left = cap - oth - comp - carCost;
    /* §49.15: the bank ships one keyframe and a delta rect per later frame (rowDelta), so a
     * frame past the first costs its rect, not a panel. Priced at the motion's box grown by
     * the encoder's 16 px tile snap on each side, at 3 B a texel (colour and alpha) — an
     * upper bound on what rowDelta writes, so the pack's own capCheck never meets a bank
     * this let through. A 20-frame bank of a small cutout is then a keyframe and twenty
     * small rects, which is the fps x cutout currency the plan was priced in. */
    var dB = Math.min(kf, Math.min(SCREEN_W, bw + 32) * Math.min(SCREEN_H, bh + 32) * 3);
    var bankB = function (n) { return n ? kf + (n - 1) * dB : 0; };
    var maxB = left >= kf ? Math.min(BG3_FRAMES_MAX, 1 + Math.floor((left - kf) / dB)) : 0;
    var b2 = maxB >= 2 ? thin(B, Math.ceil(B / maxB)) : { keyframes: 0, fps: 0, decim: 1, bbox: [bw, bh] };
    if (b2.keyframes >= 2)
      return out('bank', b2, car, 'plate keyframe bank: ' + b2.keyframes + ' of ' + B + ' frames at '
        + b2.fps + ' fps' + (b2.decim > 1 ? ' (thinned by ' + b2.decim + ')' : '') + ', '
        + bankB(b2.keyframes) + ' B' + (car ? ' over the ' + carCost + ' B the carriage took; ' + car.sentence : ''));
    if (car) return out('sprite', { keyframes: 1, fps: 0, decim: 1 }, car, 'plate rides the still: ' + car.sentence);
    return no('union ' + (m.unionPx | 0) + ' px in a ' + bw + 'x' + bh + ' box fits neither carrier: '
      + B + ' plate keyframes of ' + kf + ' B exceed the ' + left + ' B'
      + (carCost ? ' left after the carriage took ' + carCost + ' B' : ' the slot leaves after '
        + oth + ' B of art' + (comp ? ' and ' + comp + ' B of complication tiles' : ''))
      + ', and the box is ' + (m.hasCage ? 'not local' : 'not a caged motion a carriage row can carry'));
  }


  /* The D29 meter, read back off finished bytes rather than estimated from the model. The
   * editor shows it before an owner stores and after, and a number that disagreed with the
   * file it described would be worse than no number. Both sums are the INSTALL sums — the
   * region a slot reserves for tables, and the room its banks have left — so a full theme
   * reads 100% and is legal, exactly as checkBudget3 accepts == cap. */
  function budget3(raw) {
    var h = parseBundle3(raw);
    return { name: h.name, bytes: raw.length, geomSize: h.geomSize, geomCap: GEOM_CAP3,
             geomPct: Math.round(h.geomSize / GEOM_CAP3 * 1000) / 10,
             artSize: h.artSize, artCap: SLOT_ART_CAP3,
             artPct: Math.round(h.artSize / SLOT_ART_CAP3 * 1000) / 10,
             nSections: h.nSections };
  }

  /* Which comps the compiled clock must carry in its table — the one predicate
   * behind the button's note, the export guard and the bake, so none of the
   * three can promise what another refuses. Live is data-driven: a window
   * printing calendar text, a moon on tonight's phase, a subdial hand that
   * turns. Each maps to one window row by compPlan3 below. Static is
   * geometry-driven and rides inside the baked plate: an opening is a hole
   * with the movement art — the bank itself — under it; a flare is engraved
   * art unless it prints the date; a custom-format window is uploaded art; a
   * subdial with no scale has no hand to drive. Anything unrecognised counts
   * live: the safe direction is the refusal the author can see. */
  function liveComps3(comps) {
    return (comps || []).filter(function (c) {
      if (c.type === 'opening') return false;
      if (c.type === 'index') return false;                  // D43 P5d.3n: crystal paint, still
      if (c.type === 'window') return c.shows !== 'custom';
      if (c.type === 'flare') return c.icon === 'datetext';
      if (c.type === 'subdial') return subScale3(c, comps) !== 'none';   // an engraved dial with no hand is plate art
      return true;
    });
  }

  /* The §31 comp table, planned: one window row per live comp. The dial drives
   * the value (D32 ruling B: the frame-to-value map is the clock's calendar,
   * not a carried figure), so a row names a source and the domain's own count
   * of plates — a date window is 31 plates because a month has 31 days, a
   * seconds subdial is 60 whole dials because the runtime indexes one per
   * tick and a FORMAT 3 pointer snaps to its domain where a FORMAT 2 aux row
   * interpolated. A datetext flare needs the glyph fields no element row
   * holds — orb_clock3.c's refusal, said here so the author hears it before
   * export. Hosted rows are ordered BEFORE their host: the runtime paints its
   * bottom bucket in record order, and a host tile carries a punched aperture
   * over the tile beneath it. Unknown types refuse rather than freeze. */
  var COMP3_SCALE_SRC = { seconds: 'second', minutes: 'minute', date: 'date', day: 'weekday', month: 'month', hours: 'hour12', hours24: 'hour24' };
  /* What a sub-dial reads, and how its hand moves (owner, 2026-11-06: a face prints nothing,
   * the hands aligned to it print and read). The first hand aligned to it names the measure; a
   * face no hand is aligned to is plate art. Its hand sweeping an arc (retrograde) or riding a
   * slider's slot makes it the arc or the track a pointer row drives. A design written before
   * named the measure and the arc on the face (scale, form 'fan'), and still reads so. */
  var HAND3_SCALE = { second: 'seconds', minute: 'minutes', hour: 'hours', hours24: 'hours24', gmt: 'hours24', date: 'date', day: 'day', month: 'month' };
  function faceHand3(c, comps) { return (comps || []).filter(function (x) { return x.type === 'hand' && x.host === c.id; })[0] || null; }
  function subScale3(c, comps) {
    if (c.scale !== undefined) return c.scale;
    var h = faceHand3(c, comps);
    return h ? (HAND3_SCALE[h.shows] || 'none') : 'none';
  }
  function subTrack3(c, comps) {
    if (c.form === 'fan' || c.form === 'slider') return true;
    var h = faceHand3(c, comps), m = h && h.motion;
    return !!(m && (m.path === 'retrograde' || m.path === 'slide' || m.path === 'slide-x' || m.path === 'slide-y'));
  }
  var COMP3_SHOW_SRC = { date: 'date', day: 'weekday', month: 'month' };
  /* D34: the scales whose hand sweeps a WHOLE turn across its domain. A pointer row
   * carries one plate and the dial turns it, so the plate must be the box the hand
   * covers at every value — and the centre of that box is the pivot only when the
   * sweep is whole, because only then is the union a disc about the pivot. A date
   * ring stops 1/31 of a turn short of closing, so its box centre sits off the true
   * pivot by a margin nobody can measure from the pixels alone: the author who wants
   * a pointer on one gets the window row, and this is the sentence that says so.
   * The scales that qualify are the ones the complaint was ever about. */
  var COMP3_RING_FULL = { second: 60, minute: 60, hour12: 12, hour24: 24 };
  /* D43 P5d.3: a wheel is a turning row -- its disc, standing at figure 0 -- and the faces
   * over the wheels are one still cover row with their cutouts cut out of it, both just
   * above the plate. What each measure reads, how many figures its ring carries (a digit
   * ring: as many as that digit of the shown number ever reads), the value its first
   * figure stands for, and the figure its value 0 lands on (the weekday's Monday is the
   * second figure of a Sunday-first ring). The moon is a disc with two moons on it, so a
   * whole turn is two lunations: 60 figure places of a day each. These counts are
   * WatchEngine.wheelSpec's, and headless-wheels pins the two together. */
  var WHEEL3 = {
    date: { src: 'date', n: 31, vmin: 1, tens: 4 }, day: { src: 'weekday', n: 7, off: 1 },
    month: { src: 'month', n: 12, tens: 2 }, year: { src: 'year', n: 0, tens: 10 },
    hour: { src: 'hour12', n: 12, tens: 2 }, hours24: { src: 'hour24', n: 24, tens: 3 },
    minute: { src: 'minute', n: 60, tens: 6 }, second: { src: 'second', n: 60, tens: 6 },
    moon: { src: 'moon_age', n: 60, moon: true }
  };
  // the wheels that turn as rows: smooth, eased, or reading a measure no window can plate
  function wheelTurns3(c) {
    var wd = WHEEL3[(c && c.shows) || 'date'];
    // (an exposed ring is seen whole: a plate per value would be the whole ring, so it turns)
    return !wd || c.cadence === 'smooth' || +c.ease > 0 || c.exposed === true || !CLOCK_WINDOW_DOMAIN[wd.src];
  }
  /* D43 §5.3, ruling 3 (P2's per-layer setting): a turning wheel row may be dithered. `on` and
   * `off` are the author's; `auto` dithers a wheel whose face carries a gradient -- a gradient or
   * sunray finish, or the moon's disc -- where truncation bands it, and leaves a flat one alone.
   * The pattern is baked into the disc and turns with it; at a jump, or the moon's month, it does
   * not crawl. A snapping wheel ships screen-locked plates, which dither as every still plane does. */
  function wheelDither3(c) {
    if (c.dither === 'on') return true;
    if (c.dither === 'off') return false;
    return c.shows === 'moon' || c.wfinish === 'gradient' || c.wfinish === 'sunray';
  }
  function wheelPlan3(comps) {
    var rows = [], refuses = [];
    (comps || []).forEach(function (c) {
      if (!c || c.type !== 'wheel' || !wheelTurns3(c)) return;
      var id = c.id === undefined ? '?' : String(c.id), wd = WHEEL3[c.shows || 'date'];
      if (!wd) { refuses.push('the wheel \u201c' + id + '\u201d shows \u201c' + c.shows + '\u201d: no source on this dial reads that'); return; }
      var dg = c.digit && c.digit !== 'whole' ? String(c.digit) : (c.shows === 'year' ? 'units' : 'whole');
      if (DIGITS.indexOf(dg) < 0) { refuses.push('the wheel \u201c' + id + '\u201d reads digit \u201c' + dg + '\u201d: the dial knows ' + DIGITS.join('/')); return; }
      if (dg !== 'whole' && (wd.moon || c.shows === 'day'))
        { refuses.push('the wheel \u201c' + id + '\u201d reads a digit of the ' + (wd.moon ? 'moon' : 'day of the week') + ', which has none'); return; }
      if ((dg === 'hundreds' || dg === 'thousands') && c.shows !== 'year')
        { refuses.push('the wheel \u201c' + id + '\u201d reads the ' + dg + ' of ' + c.shows + ', which never reaches them'); return; }
      // D43: a barrel that rolls is a drum row: its figures on one flat strip the OS rolls through
      // the window and foreshortens line by line; its shading over the curve rides the cover above
      // it, so a barrel with no face over it (exposed) has nowhere to carry it, and the OS rolls a
      // drum about a level axle only
      var barrel = c.form === 'barrel';
      if (barrel && c.exposed === true) { refuses.push('the barrel \u201c' + id + '\u201d is seen whole: a drum is read through cutouts, whose face carries its shading \u2014 give it a cutout'); return; }
      if (barrel && Math.round(+c.trot || 0) % 360) { refuses.push('the barrel \u201c' + id + '\u201d is turned ' + Math.round(+c.trot) + '\u00b0: the orb rolls a drum about a level axle \u2014 set its Drum turn to 0'); return; }
      var n = dg === 'whole' ? wd.n : dg === 'tens' ? wd.tens : 10;
      var dir = c.dir === 'ccw' ? -1 : 1, smooth = c.cadence === 'smooth';
      // the engine turns a ring back by a figure as its value goes up (the next figure comes
      // round under the cutout), and the moon disc forward, as the classic moonphase does
      var span = wd.moon ? dir * TURN_10 : -dir * TURN_10;
      var vmin = dg === 'whole' ? (wd.vmin | 0) : 0;
      rows.push({ id: id, type: 'wheel', host: c.host || 'dial', exposed: c.exposed === true, src: wd.src, digit: dg, n: n, vmin: vmin, vmax: vmin + n,
                  // a drum neither turns nor starts at an angle: its strip is laid out from the
                  // figure the value's start shows (foff), in its direction (dir), and rolls dir x strip
                  drum: barrel, dir: dir, foff: dg === 'whole' ? (wd.off | 0) : 0,
                  span: barrel ? 0 : span, zero: barrel ? 0 : Math.round(span * (dg === 'whole' ? (wd.off | 0) : 0) / n), end: 'wrap',
                  cadence: smooth ? 'roll' : 'jump', ease: smooth || wd.src === 'second' ? 0 : Math.max(0, Math.min(1000, +c.ease || 0)), dither: wheelDither3(c) });
    });
    return { rows: rows, refuses: refuses };
  }
  function compPlan3(comps) {
    var rows = [], refuses = [];
    (comps || []).forEach(function (c) {
      var src = null, id = c.id === undefined ? '?' : c.id;
      if (c.type === 'opening') return;                                  // a hole in the plate
      if (c.type === 'hand') return;                                     // D43 P5c: a hand slot or a pointer row, never a window row
      if (c.type === 'cutout') return;                                   // D43 P5d.3: a wheel's window; its wheel is the row
      if (c.type === 'index') return;                                    // D43 P5d.3n: painted into the crystal overlay, no row
      if (c.type === 'wheel') {
        // a wheel that snaps between figures is one plate per value, cropped to its cutouts by
        // the comp pass -- what a window cost, and the glass's own path; one that moves between
        // them (smooth, eased) or reads a measure with no window domain turns as a row instead
        if (wheelTurns3(c)) return;
        var wsrc = WHEEL3[c.shows || 'date'].src, wdom = CLOCK_WINDOW_DOMAIN[wsrc];
        rows.push({ id: id, type: 'wheel', src: wsrc, count: wdom[1], valueMin: wdom[0], host: c.host || 'dial' });
        return;
      }
      if (c.type === 'flare') {
        if (c.icon !== 'datetext') return;                               // engraving
        refuses.push('the flare \u201c' + id + '\u201d prints the date, and printing it needs the glyph fields no element row holds');
        return;
      }
      if (c.type === 'window') {
        if (c.shows === 'custom') return;                                // uploaded art is plate content
        src = COMP3_SHOW_SRC[c.shows];
        if (!src) { refuses.push('the window \u201c' + id + '\u201d shows \u201c' + c.shows + '\u201d: no source on this dial reads that'); return; }
      } else if (c.type === 'moon') src = 'moon_age';
      else if (c.type === 'subdial') {
        var sc3 = subScale3(c, comps);
        if (sc3 === 'none') return;                                      // no hand: plate art
        if (subTrack3(c, comps)) return;                                 // D43 P5d.2: its hands are pointer rows
        if (pointerDefault3(c, comps)) return;   // D34: pointerPlan3 owns this row, not the bank
        src = COMP3_SCALE_SRC[sc3];
        if (!src) { refuses.push('the subdial \u201c' + id + '\u201d reads scale \u201c' + sc3 + '\u201d: the dial indexes plates by calendar value and none matches'); return; }
      } else {
        refuses.push('the complication \u201c' + id + '\u201d of type \u201c' + c.type + '\u201d has no mapping to a window row');
        return;
      }
      var dom = CLOCK_WINDOW_DOMAIN[src];
      rows.push({ id: id, type: c.type, src: src, count: dom[1], valueMin: dom[0], host: c.host || 'dial' });
    });
    var byId = {};
    rows.forEach(function (r) { byId[r.id] = r; });
    rows.forEach(function (r) { r.hosted = Object.prototype.hasOwnProperty.call(byId, r.host) ? r.host : null; });
    var ordered = rows.filter(function (r) { return !r.hosted; });
    rows.forEach(function (r) {
      if (!r.hosted) return;
      var at = -1;
      for (var i = 0; i < ordered.length; i++) if (ordered[i].id === r.hosted) { at = i; break; }
      ordered.splice(at < 0 ? ordered.length : at, 0, r);                // host gone static: it stands on the plate
    });
    if (ordered.length > CLK_COMP_MAX)
      refuses.push(ordered.length + ' complications exceed the dial\u2019s cap of ' + CLK_COMP_MAX + ' — remove one');
    return { rows: ordered, refuses: refuses };
  }
  /* D34: the pointer table, planned. An author who sets a subdial's drive to
   * `pointer` says the moving part IS the indication and refuses the plate-per-tick
   * bank for it — one masked hand plate, and the dial turns it. Only a scale whose
   * hand sweeps a WHOLE turn across its domain can take that carrier: the row's
   * linear drive (zero + value*span/vmax, span inside one turn) lands the hand on
   * every tick of a 60- or 12-division ring, and a date ring that stops 1/31 of a
   * turn short of closing does not. A refused drive stays a refusal even when the
   * rest of the bake is legal — silently falling back to plates would ship the
   * carrier the author just said no to. Rows here carry the drive, not the art:
   * the composer measures the plate and the pivot off the pixels and feeds them
   * to bakeClock3's `aux` option. */
  /* Which way a subdial's hand ships is the engine's call alone (owner, 2026-11-05; it was the
   * author's Drive chip, D34): a seconds or hours ring, or any fan or slider, turns one hand
   * plate; a ring that hosts another indication stays on the bank, because the plates carrier
   * punches the host tile so the hosted tile sits inside it and a pointer row carries nothing
   * to punch; every other scale stays on the bank. The editor and the bake share this one
   * predicate, and an old design's `carrier` is ignored. */
  function pointerDefault3(c, comps) {
    if (c.type === 'subdial' && subTrack3(c, comps)) return true;   // an arc or a slider is always pointer-driven
    if (c.type !== 'subdial') return false;   // the engine's call alone (owner, 2026-11-05): no authored carrier
    var sc3 = subScale3(c, comps);
    if (!(sc3 === 'seconds' || sc3 === 'hours')) return false;
    // a hand on the sub-dial is its pointer, not another indication (D43 P5c)
    return !(comps || []).some(function (x) { return x.host === c.id && x.type !== 'hand'; });
  }
  var COMP3_POINTER_SPAN = 3600;                       // one whole turn, tenths of a degree
  function pointerPlan3(comps) {
    var rows = [], refuses = [];
    (comps || []).forEach(function (c) {
      if (c.type !== 'subdial') return;
      if (!pointerDefault3(c, comps)) return;
      var id = c.id === undefined ? '?' : c.id;
      if ((comps || []).some(function (x) { return x.host === id && x.type !== 'hand'; })) {
        refuses.push('the subdial \u201c' + id + '\u201d hosts another indication: the plates carrier punches its '
          + 'plate so the hosted tile sits inside the hole, and a pointer row carries nothing to punch — drive it as plates');
        return;
      }
      var sc3 = subScale3(c, comps), src = COMP3_SCALE_SRC[sc3];
      if (!src || !COMP3_RING_FULL[src]) {
        refuses.push('the subdial \u201c' + id + '\u201d asks to be driven as a pointer and reads scale '
          + '\u201c' + sc3 + '\u201d: a pointer plate turns through '
          + 'a whole linear sweep, and that ring does not close under its own hand — drive it as plates');
        return;
      }
      rows.push({ id: id, type: 'subdial', src: src, vmin: 0, vmax: COMP3_RING_FULL[src],
                  span: COMP3_POINTER_SPAN, zero: 0, motion: MOTION_NAMES[MOTION_ROTATE] });
    });
    if (rows.length > CLK_AUX_MAX)
      refuses.push(rows.length + ' pointers exceed the dial\u2019s ' + CLK_AUX_MAX + ' pointer slots \u2014 remove one');
    return { rows: rows, refuses: refuses };
  }


  /* ---- radar-plan §6.22: the accented Latin letters, as a base and a mark --------------
   * A font's charset goes on past the ASCII and the five symbols with a dotless i and the
   * marks of tools/latin_marks.py (theme_schema.FONT_MARKS): each mark's cell is its carrier
   * letter accented, less the carrier, drawn where the accented carrier's ink would be. The
   * OS lays a mark over another base by their advances (main/stage/orb_latin.h). */
  var FONT_MORE = ['\u0131', { acc: 'ó', car: 'o' }, { acc: 'ò', car: 'o' }, { acc: 'ô', car: 'o' }, { acc: 'õ', car: 'o' }, { acc: 'ö', car: 'o' }, { acc: 'å', car: 'a' }, { acc: 'č', car: 'c' }, { acc: 'ă', car: 'a' }, { acc: 'ō', car: 'o' }, { acc: 'ő', car: 'o' }, { acc: 'ż', car: 'z' }, { acc: 'ç', car: 'c' }, { acc: 'ą', car: 'a' }, { acc: 'ș', car: 's' }, { acc: 'Ó', car: 'O' }, { acc: 'Ò', car: 'O' }, { acc: 'Ô', car: 'O' }, { acc: 'Õ', car: 'O' }, { acc: 'Ö', car: 'O' }, { acc: 'Å', car: 'A' }, { acc: 'Č', car: 'C' }, { acc: 'Ă', car: 'A' }, { acc: 'Ō', car: 'O' }, { acc: 'Ő', car: 'O' }, { acc: 'Ż', car: 'Z' }, { acc: 'Ç', car: 'C' }, { acc: 'Ą', car: 'A' }, { acc: 'Ș', car: 'S' }];
  // the text a charset entry measures by: a mark measures as its carrier, so its cell sits
  // where the carrier's ink does
  function glyphText(e) { return typeof e === 'string' ? e : e.car; }
  // one charset entry onto x (font, fill and baseline set) at (x0, y0): a mark is its accented
  // carrier with the carrier taken out
  function drawGlyph(x, e, x0, y0) {
    if (typeof e === 'string') { x.fillText(e, x0, y0); return; }
    x.fillText(e.acc, x0, y0);
    var op = x.globalCompositeOperation;
    x.globalCompositeOperation = 'destination-out';
    x.fillText(e.car, x0, y0);
    x.globalCompositeOperation = op;
  }

  root.GorbBake = {
    FONT_MORE: FONT_MORE, glyphText: glyphText, drawGlyph: drawGlyph,
    SCREEN: SCREEN_W, KINDS: KINDS, SOURCES: SOURCES, COMP_KINDS: COMP_KINDS,
    THEME_FIELDS: THEME_FIELDS, SPRITE_FIELDS: SPRITE_FIELDS, GEOM_COUNTS: GEOM_COUNTS,
    HDR_WORDS: HDR_WORDS, NAME_MAX: NAME_MAX, SLOT_CAP: SLOT_CAP,
    GEOM_BUDGET: GEOM_BUDGET,
    /* The FORMAT 2 payload budget = the one exported slot cap (§7.4). A suite that
     * prices a refusal must anchor on THIS, not on a literal it carries itself. */
    SLOT_BUDGET: SLOT_CAP,
    // The header layout is exported so a reader can check a finished container instead of
    // trusting the writer: the selftest re-CRCs the geometry and the pixel payload it read
    // back off a real Chrome canvas, exactly as orb_theme.c does before it accepts a theme.
    MAGIC: MAGIC, FORMAT: FORMAT, GEOM_ABI: GEOM_ABI, SECTOR: SECTOR,
    W_NAME: W_NAME, W_HDR_CRC: W_HDR_CRC,
    caps: { comp: CLK_COMP_MAX, aux: CLK_AUX_MAX, bank: CLK_BANK_MAX, rects: RECTS_MAX, gifs: GIFS_MAX, tourb: TOURB_MAX },
    crc32: crc32, blankSpr: blankSpr, packSpr: packSpr, packGeom: packGeom, geomNeed: geomNeed,
    packHeader: packHeader, writeContainer: writeContainer, artOffset: artOffset,
    Blob: Blob, blob: function () { return new Blob(); },
    rgb565Plane: rgb565Plane, alphaPlane: alphaPlane, alphaBbox: alphaBbox,
    // D43 §5.3: the still-plane dither, its table and its per-texel steps, so the
    // parity suite holds them to tools/orbdither.py and Panel view reuses them.
    DITHER_SHA256: DITHER_SHA256, DITHER_T: DITHER_T, ditherQuant565: ditherQuant565,
    ditherAlphaStep: ditherAlphaStep, DITHER_ALPHA_SHIFT: DITHER_ALPHA_SHIFT,
    // D43 §5.2: the device blend and the Panel view's three steps.
    mix1616: mix1616, blendPx: blendPx, panelPlate: panelPlate, panelLayer: panelLayer, panelRGBA: panelRGBA,
    cropLayer: cropLayer, emitSprite: emitSprite, blankSprite: blankSprite,
    copyPlane: copyPlane, blankPlane: blankPlane, compositeOver: compositeOver,
    matteFold: matteFold, matteOverBlack: matteOverBlack,
    placedPlane: placedPlane, reachableAnnulus: reachableAnnulus, bezelFold: bezelFold,
    maskedPlane: maskedPlane, tintOf: tintOf, opaqueCopy: opaqueCopy,
    zeroUnder: zeroUnder, killInvisible: killInvisible, inkMask: inkMask, inkCount: inkCount,
    framesToU16: framesToU16, temporalSnap: temporalSnap, boxDensity: boxDensity,
    packMask: packMask, gifTransition: gifTransition, bakeBgBank: bakeBgBank,
    bakeTourbillon: bakeTourbillon, packWindowTile: packWindowTile,
    bakeComplications: bakeComplications, bakeAux: bakeAux, blankRecord: blankRecord,
    fieldsOf: fieldsOf, wordsOf: wordsOf, sourceOf: sourceOf, tileOrder: tileOrder,
    staticRun: staticRun, underLayers: underLayers, AUX_KINDS: AUX_KINDS,
    bakeKit: bakeKit, decodePng: decodePng, decodePngAsync: decodePngAsync,
    // The name rule the studio enforces before it spends a second baking, read here
    // rather than trusted: the field is NAME_MAX bytes with the NUL reserved, measured
    // in UTF-8 bytes because that is the unit the header cuts in, and an embedded NUL
    // is refused because the C reader stops at it and the rest of the name would be
    // silently gone — from the list, from the id, and from the card filename.
    nameOk: nameOk, nameBytes: nameBytes, themeIdFromDigest: themeIdFromDigest,
    nameToken: nameToken, cardStem: cardStem,
    // FORMAT 3 (D32/D33): the multi-section bundle, the element table, and the D29 gate.
    // The browser owns the whole dialect, caps and refusal order included, so the release
    // ships as a web page that flashes over Web Serial; internal/orbnet's reader is the
    // hub's pre-flight mirror of these same rules, checked against the same committed
    // fixture. packElements refuses through the same check_element / check_table rules the
    // device door applies, so an element a theme cannot name never reaches a card.
    FORMAT3: FORMAT3, GEOM_ABI3: GEOM_ABI3, SECT_MAX: SECT_MAX, IDX_WORDS: IDX_WORDS,
    GEOM_ABI6: GEOM_ABI6, DELTA_TOL: DELTA_TOL,   // the delta floor, so a suite asserts against the codec's number, not a literal
    CADENCES: CADENCES, SHAPES: SHAPES, ENDS: ENDS, DIGITS: DIGITS, CLK_ROW_MAX: CLK_ROW_MAX, CLK_PICK_MAX: CLK_PICK_MAX,
    FONT_ASCII_N: FONT_ASCII_N, FONT_GLYPHS: FONT_GLYPHS, FONT_SLOTS: FONT_SLOTS, FONT_NONE: FONT_NONE, ROW_FRAMES_MAX: ROW_FRAMES_MAX,
    checkClockRows: checkClockRows, clockTempo: clockTempo, v6Drive: v6Drive,
    SCALARS_N: SCALARS_N, ELEMENT_WORDS: ELEMENT_WORDS, ELEMENT_FIELDS: ELEMENT_FIELDS,
    CLASS_NAMES: CLASS_NAMES, CLASS_APP_ADV: CLASS_APP_ADV, CLASS_NO_ANIM: CLASS_NO_ANIM,
    EF_ALPHA: EF_ALPHA, EF_LOOP: EF_LOOP, EF_OS_ADV: EF_OS_ADV, EF_HIDDEN: EF_HIDDEN,
    EF_DELTA_WRAP: EF_DELTA_WRAP, EF_SMOOTH: EF_SMOOTH,
    AUX_F_SHADOW3: AUX_F_SHADOW3,
    ELEM_MAX: ELEM_MAX, ELEM_FRAMES_MAX: ELEM_FRAMES_MAX,
    // D34: the pointer vocabulary, exported so the composer and the suite read the
    // same table the baker refuses against rather than restating ids.
    CLASS_AUX: CLASS_AUX, MOTION_NAMES: MOTION_NAMES, MOTION_ROTATE: MOTION_ROTATE,
    MOTION_SLIDE_X: MOTION_SLIDE_X, MOTION_SLIDE_Y: MOTION_SLIDE_Y,
    motionIsSlide: motionIsSlide, AUX_MAX: CLK_AUX_MAX,
    GEOM_CAP: GEOM_CAP3, ART_CAP: SLOT_ART_CAP3, SLUG_MAX: SLUG_MAX,
    // The one exported slot cap, defined once above from the device's own partition
    // facts (§7.4). A suite that asserts a cap must assert against THIS, not a literal.
    ORB_THEME_SLOT_CAP: ORB_THEME_SLOT_CAP, DEVICE_SLOT_CAP: DEVICE_SLOT_CAP,
    IDX_ORDER: IDX_ORDER, IDX_OFF: IDX_OFF, IDX_TYPES: IDX_TYPES,
    blankElement: blankElement, classId: classId, checkElement: checkElement,
    checkTable: checkTable, packElements: packElements, unpackElements: unpackElements,

    checkSlug: checkSlug, checkBudget3: checkBudget3,
    packBlob3: packBlob3, unpackBlob3: unpackBlob3,
    packIndex3: packIndex3, unpackIndex3: unpackIndex3, packHeader3: packHeader3,
    parseBundle3: parseBundle3, verifyBundle3: verifyBundle3, writeBundle3: writeBundle3,
    sectionAbiOk: sectionAbiOk,
    // The bake path D33 puts in the browser: encoded planes -> a bank and its catalogue,
    // a clock section the compiled dial projects, and the D29 sums read back off finished
    // bytes. bakeClock3 refuses with orb_clock3.c's own sentences, so the editor names the
    // reason the dial will print instead of a reason of its own.
    bank3: bank3, stage3: stage3, bakeClock3: bakeClock3, budget3: budget3, splitPolicy3: splitPolicy3, estimate3: estimate3, estimate4: estimate4, EST4: EST4, GRID4: GRID4, pickPlan4: pickPlan4, planKey4: planKey4, bestPlan4: bestPlan4,
    carriageRect3: carriageRect3, carriageStart3: carriageStart3,
    liveComps3: liveComps3, compPlan3: compPlan3, wheelPlan3: wheelPlan3, wheelDither3: wheelDither3, readDesign3: readDesign3, THEME_UPLOAD_MAX: THEME_UPLOAD_MAX, wheelTurns3: wheelTurns3, pointerPlan3: pointerPlan3,
    pointerDefault3: pointerDefault3,
    // D39: the sound vocabulary and the mirror of the dial's sound refusals.
    SOUND_SLOTS: SOUND_SLOTS, HOUR_PATTERNS: HOUR_PATTERNS, CLASS_SOUND: CLASS_SOUND, CLASS_VALUES: CLASS_VALUES,
    zlibFixed: zlibFixed,
    discSpan: discSpan, discPx: discPx, discBytes: discBytes, discPack: discPack, discPackMasked: discPackMasked,
    adpcmEncode: adpcmEncode, adpcmDecode: adpcmDecode, soundEncode: soundEncode, soundBytes: soundBytes,
    checkClockSounds: checkClockSounds
  };
})(typeof window !== 'undefined' ? window : (typeof globalThis !== 'undefined' ? globalThis : this));
if (typeof module !== 'undefined' && module.exports) module.exports = (typeof window !== 'undefined' ? window : globalThis).GorbBake;
