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
                 'quarter', 'value'];
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
   * (SLOT_GEOM_CAP), banks install at +0x10000 inside SLOT_CAP — the payload budget
   * below, 0x3F0000. Each cap is sector-aligned, so == cap installs to the byte.
   * Refusals end in copy_into's own words, so the bench and the dial say one sentence. */
  var RECTS_MAX = 2048, GIFS_MAX = 32, BG3_FRAMES_MAX = 16, TOURB_MAX = 16;
  var SLOT_CAP = 0x003f0000;
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
    // The name IS the theme now: main/art/orb_theme.c files the container under
    // sha256(name)[:12] and refuses a container whose name field is empty, so baking one
    // with no name would only produce a file the device can never list. Refuse it here,
    // where the reason can be said, instead of at the card.
    if (!o.name) throw new Error('the theme has no name to bake — the card files a theme by sha256(name)[:12] and refuses a container with an empty name');
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

  /* The card filename a name yields: hex(sha256(name))[:12], lowercase — the same rule
   * tools/orbtheme.py's theme_id() defines and main/art/orb_theme.c's theme_id() mirrors.
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

  /* The colour plane: native-LE RGB565, w*h*2 bytes. Alpha is ignored here — every
   * texel keeps its stored colour so the planar alpha plane alone decides coverage,
   * which avoids a dark edge fringe (bake_clock.py's rgb565_le). */
  function rgb565Plane(rgba, w, h) {
    var out = new Uint8Array(w * h * 2);
    for (var i = 0, o = 0; i < w * h; i++) {
      var r = rgba[i * 4], g = rgba[i * 4 + 1], b = rgba[i * 4 + 2];
      var v = ((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3);
      out[o++] = v & 0xff; out[o++] = (v >> 8) & 0xff;
    }
    return out;
  }
  function alphaPlane(rgba, w, h) {
    var out = new Uint8Array(w * h);
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
  function mix565(dst, fg, a) {
    var mix = (a + 4) >> 3;
    var dr = (dst >> 11) & 31, dg = (dst >> 5) & 63, db = dst & 31;
    var fr = (fg >> 11) & 31, fg6 = (fg >> 5) & 63, fb = fg & 31;
    var r = (((fr - dr) * mix >> 5) + dr) & 31;
    var g = (((fg6 - dg) * mix >> 5) + dg) & 63;
    var b = (((fb - db) * mix >> 5) + db) & 31;
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
      if ((h.kind === 'hour' || h.kind === 'minute' || h.kind === 'second') && onCentre) centre[h.kind] = h;
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
    // secondHand string is what the STUDIO preview did and prep ignores it; the bank here is
    // always the delta-rect one, which is bake_clock.py's GIF-bg case, and that mode forces
    // the sweep back on (a ticking hand over a bank that already moves every tick strobes)
    // unless the caller asks for --no-sweep.
    var tick = opts.tick ? true : false;
    var sweep = !tick;
    var tickRate = tick ? 1 : fps;
    if (opts.forceSweep) { sweep = true; tickRate = fps; }
    else if (!opts.noSweep && !tick) { sweep = true; }
    if (opts.tickRate) tickRate = opts.tickRate;
    tickRate = Math.max(1, Math.min(255, tickRate | 0));
    if (String(M.secondHand || 'sweep') !== 'sweep' && sweep)
      report.notes.push('the studio previewed the seconds hand as ' + JSON.stringify(M.secondHand) +
        ', but a delta-rect bank animates every tick, so the baked dial sweeps (pass noSweep to keep the step)');
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
  // element row APPENDED kind and src without a bump — the blob's derived row
  // stride (see unpackBlob3) is what makes a narrower old row and a wider new
  // one the same readable file. A stamped 3 on a 19-word row is the promise,
  // and an old reader's fixed 17-word walk refuses the new bytes at its door.
  var SCALARS_N = THEME_FIELDS.length;   // 26: the clock dialect, carried opaque (D31)
  var SPR_WORDS = 4;                     // off, size, w, h — one bank entry's byte range
  var BLOB_HEAD = 4 + SCALARS_N;         // counts + scalars, before the element table
  var GEOM_CAP3 = 0xF000;                // orb_theme.c SLOT_GEOM_CAP
  var SLOT_CAP3 = 0x00400000, SLOT_ART_OFF3 = 0x00010000;
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
    ['kind', 'u8'], ['src', 'u8'],   // D32's ruling A. On an AUX row (D34) the pair
                             // reads as the motion id and the driving source.
    ['span', 'i16'], ['zero', 'i16'], ['vmin', 'u16'], ['vmax', 'u16']
    // D34's four drive columns, an AUX row's and nobody else's. The motion names the
    // unit: tenths of a degree on rotate/retrograde/indexed, pixels on a slide. A row
    // read through a narrower stride carries zero for each, which checkElement refuses
    // on an AUX row rather than draws as a pointer frozen at its own zero.
  ];
  var ELEMENT_WORDS = ELEMENT_FIELDS.length;   // 23 words, 92 bytes a row
  // The pre-D32 width, the floor a derived row stride is refused below
  // (theme_schema.ELEMENT_WORDS_MIN; orb_bundle.c's ORB_BUNDLE_ELEM_WORDS_MIN).
  var ELEMENT_WORDS_MIN = 17;
  var ELEM_MAX = 32, ELEM_ORDER_N = 64, ELEM_FRAMES_MAX = 360;
  var EF_ALPHA = 1, EF_LOOP = 2, EF_OS_ADV = 4, EF_HIDDEN = 8;
  var EF_ALL = EF_ALPHA | EF_LOOP | EF_OS_ADV | EF_HIDDEN | AUX_F_SHADOW3;
  var CLASS_NAMES = ['text', 'sprite', 'hand', 'plate', 'ring', 'bank', 'layer', 'status', 'aux'];
  var CLASS_TEXT = 0, CLASS_SPRITE = 1, CLASS_HAND = 2, CLASS_PLATE = 3;
  var CLASS_RING = 4, CLASS_BANK = 5, CLASS_LAYER = 6, CLASS_STATUS = 7;
  var CLASS_AUX = 8;                       // D34: a pointer, appended at 8
  // How an AUX row moves (theme_schema.MOTION_NAMES / clk_types.h's CLK_MOTION_*).
  // The motion IS the unit of span/zero: an angle in tenths of a degree, or pixels
  // along the named axis. One vocabulary, so a row cannot name a motion and a unit
  // that contradict each other.
  var MOTION_NAMES = ['rotate', 'retrograde', 'indexed', 'slide-x', 'slide-y'];
  var MOTION_ROTATE = 0, MOTION_RETROGRADE = 1, MOTION_INDEXED = 2,
      MOTION_SLIDE_X = 3, MOTION_SLIDE_Y = 4;
  function motionIsSlide(k) { return k === MOTION_SLIDE_X || k === MOTION_SLIDE_Y; }
  // R6: whose frames the OS may advance, and which classes cannot animate at all. This
  // decides the editor's element class for an animated region: an OS-paced region is a
  // SPRITE row, because a bank or a text row holds app state and a layer is composited.
  var CLASS_APP_ADV = [CLASS_BANK, CLASS_TEXT, CLASS_STATUS];
  var CLASS_NO_ANIM = [CLASS_LAYER, CLASS_STATUS];

  function blankElement() {
    var r = {};
    for (var i = 0; i < ELEMENT_FIELDS.length; i++) r[ELEMENT_FIELDS[i][0]] = 0;
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
    if (kind && cls !== CLASS_BANK && cls !== CLASS_AUX) {
      throw new Error(where + '.kind: ' + kind + ' on a ' + CLASS_NAMES[cls] + ' row — how a row ' +
        'draws or moves is a driven element’s question, and this class answers it with its slot');
    }
    if (src && cls !== CLASS_BANK && cls !== CLASS_AUX) {
      throw new Error(where + '.src: ' + src + ' on a ' + CLASS_NAMES[cls] + ' row — what drives ' +
        'a row is a driven element’s question, and this class answers it with its slot');
    }
    /* ---- D34: the pointer row. Rule for rule the Python's and rule for rule
     * orb_clock3.c's: the browser refuses the shapes the dial would refuse, in the
     * dial's own words, so an author never spends an upload learning what the host
     * already knew. No missing drive column is defaulted — a pointer with no domain
     * is a refusal, not a zero. ---- */
    if (cls === CLASS_AUX) {
      var span = Math.trunc(rec.span | 0), zero = Math.trunc(rec.zero | 0);
      var vmin = Math.trunc(rec.vmin >>> 0), vmax = Math.trunc(rec.vmax >>> 0);
      if (!(kind >= 0 && kind < MOTION_NAMES.length))
        throw new Error(where + '.kind: ' + kind + ' is not a way for a pointer to move (' +
          MOTION_NAMES.join(', ') + ')');
      if (!(src >= 0 && src < SOURCES.length))
        throw new Error(where + '.src: ' + src + ' is not a value this dial can read (0..' +
          (SOURCES.length - 1) + ')');
      if (SOURCES[src] === 'none')
        throw new Error(where + ".src: an AUX pointer driven by 'none' never moves, and a row " +
          'that never moves is plate art that was written as a row');
      if (SOURCES[src] === 'text' || SOURCES[src] === 'value')
        throw new Error(where + '.src: an AUX pointer driven by ' + q(SOURCES[src]) +
          ': this dial has no such input');
      if (vmax < 1)
        throw new Error(where + '.vmax: ' + vmax + ' cannot divide a ' + MOTION_NAMES[kind] +
          ' — a pointer’s span is shared across its domain');
      if (vmin > vmax)
        throw new Error(where + '.vmin: ' + vmin + ' is above vmax ' + vmax +
          ' — the domain runs low to high');
      if (!span)
        throw new Error(where + '.span: 0 is a pointer that goes nowhere; give it the ' +
          (motionIsSlide(kind) ? 'distance' : 'angle') + ' it travels');
      if (Math.trunc(rec.n_frames | 0) !== 1)
        throw new Error(where + ': an AUX row carries one plate, not ' + rec.n_frames +
          ' — several plates indexed by a value is a BANK row');
      if (motionIsSlide(kind)) {
        if (Math.abs(span) > SCREEN_W || Math.abs(zero) > SCREEN_W)
          throw new Error(where + ': a ' + MOTION_NAMES[kind] + ' of ' + span + ' px from ' +
            zero + ' cannot stay on a ' + SCREEN_W + ' px panel — a slide’s span and zero ' +
            'are pixels, not tenths');
      } else {
        var pw = Math.trunc(rec.w | 0), ph = Math.trunc(rec.h | 0);
        var ppx = Math.trunc(rec.pivot_x | 0), ppy = Math.trunc(rec.pivot_y | 0);
        if (pw && ph && !(ppx >= 0 && ppx < pw && ppy >= 0 && ppy < ph))
          throw new Error(where + ': an AUX pointer’s pivot (' + ppx + ',' + ppy +
            ') sits outside its own ' + pw + 'x' + ph + ' plate, so it would turn about a ' +
            'point off the glass');
        if (Math.abs(zero) > 3600)
          throw new Error(where + '.zero: ' + zero + ' is an offset past one full turn ' +
            '(3600 tenths), which is not a direction');
        if (Math.abs(span) > 3600)
          throw new Error(where + '.span: ' + span + ' asks a pointer to travel more than one ' +
            'full turn; a ' + MOTION_NAMES[kind] + ' that needs more is a disc, not a hand');
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
    return recs;
  }

  /* Checked pack of a whole table: real rows only, no padding to the cap, and a missing
   * key is the bake's bug rather than a zero — theme_schema.pack_elements' order. */
  function packElements(recs, cap) {
    cap = (cap === undefined) ? ELEM_MAX : cap;
    checkTable(recs);
    if (recs.length > cap) {
      throw new Error('element table: ' + recs.length + ' rows exceed the cap of ' + cap);
    }
    var out = [];
    for (var i = 0; i < recs.length; i++) {
      for (var k = 0; k < ELEMENT_WORDS; k++) {
        var name = ELEMENT_FIELDS[k][0], t = ELEMENT_FIELDS[k][1];
        if (!(name in recs[i])) throw new Error('elem[' + i + '].' + name + ' is missing from the record');
        out.push(val('elem[' + i + '].' + name, t, recs[i][name]));
      }
    }
    return out;
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
  function packBlob3(slug, elements, scalars, sprites) {
    var elems = (elements || []).slice();
    checkTable(elems);
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
    }
    if (elems.length > 0xFFFF || sprWords.length / SPR_WORDS > 0xFFFF) {
      throw new Error('blob ' + q(slug) + ' outgrew its u16 counts');
    }
    var words = [elems.length, sprWords.length / SPR_WORDS, SCALARS_N, sb.length].concat(scal);
    if (elems.length) words = words.concat(packElements(elems));
    words = words.concat(sprWords);
    var out = wordsToBytes(words), pad = (-sb.length) & 3;
    var all = new Uint8Array(out.length + sb.length + pad);
    all.set(out, 0);
    all.set(sb, out.length);
    return all;
  }
  function unpackBlob3(blob) {
    if (blob.length < BLOB_HEAD * 4) {
      throw new Error('section blob is shorter than its counts and scalars');
    }
    var v = new DataView(blob.buffer, blob.byteOffset, blob.length);
    var nElem = v.getUint32(0, true), nSpr = v.getUint32(4, true);
    var nScal = v.getUint32(8, true), nSlug = v.getUint32(12, true);
    if (nScal !== SCALARS_N) {
      throw new Error('blob carries ' + nScal + ' scalars; this dialect has ' + SCALARS_N);
    }
    var wantMin = (4 + nScal + nSpr * SPR_WORDS) * 4 + nSlug + ((-nSlug) & 3);
    // §23.5's append-only rule, mirrored from orbbundle.unpack_blob and
    // orb_bundle.c's blob_check in the same order and the same words: the row
    // width is the blob's own number, so the leftover bytes after the fixed
    // regions must divide into whole nElem rows of a width this dialect wrote.
    if (blob.length < wantMin) {
      throw new Error('blob is ' + blob.length + ' bytes; its counts say at least ' +
        wantMin + ' — a torn file, not a short one');
    }
    var eb = blob.length - wantMin, elemWords = ELEMENT_WORDS;
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
    var off = (BLOB_HEAD + nElem * elemWords) * 4, sprs = [];
    for (i = 0; i < nSpr; i++) {
      sprs.push({ off: v.getUint32(off + i * 16, true), size: v.getUint32(off + i * 16 + 4, true),
                  w: v.getUint32(off + i * 16 + 8, true), h: v.getUint32(off + i * 16 + 12, true) });
    }
    off += nSpr * SPR_WORDS * 4;
    // The slug travels as BYTES, because the device reader memcpys them without decoding
    // and a forged blob can hold anything at all. Latin-1 keeps the round trip lossless
    // and byte-reversible, which is the reader's only promise here.
    var sb = blob.subarray(off, off + nSlug), s = '';
    for (i = 0; i < sb.length; i++) s += String.fromCharCode(sb[i]);
    var sc = {};
    for (i = 0; i < SCALARS_N; i++) sc[THEME_FIELDS[i][0]] = sext(scal[i], THEME_FIELDS[i][1]);
    return { slug: s, slug_b: sb, elements: elems, sprites: sprs, scalars: sc };
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
    hdr[0] = MAGIC >>> 0; hdr[1] = FORMAT3; hdr[2] = GEOM_ABI3; hdr[3] = HDR_WORDS * 4;
    hdr[4] = HDR_WORDS * 4;
    // geom_size/geom_crc/art_size/art_crc/n_sections come out of the writer's own sums and
    // are u32 by construction; serial and baked_ms are the caller's, and pack_header3 stamps
    // them with _u(), which WRAPS. A wall-clock stamp (Date.now() is past 1.7e12 in 2026)
    // must wrap here exactly as it does in Python, so this masks rather than refusing.
    hdr[5] = (o.geomSize >>> 0); hdr[6] = (o.geomCrc >>> 0);
    hdr[7] = (o.artSize >>> 0);  hdr[8] = (o.artCrc >>> 0);
    hdr[9] = (o.nSections >>> 0); hdr[10] = 0;
    hdr[11] = (o.serial >>> 0);
    hdr[12] = (o.bakedMs >>> 0);
    hdr[13] = (o.artOff >>> 0);
    setWord(hdr, W_NAME, o.name, NAME_MAX);
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
    if (!(sections.length >= 1 && sections.length <= SECT_MAX)) {
      throw new Error('a bundle holds 1..' + SECT_MAX + ' app sections, not ' + sections.length);
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
      blobs.push(packBlob3(s.slug, s.elements, s.scalars, sprs));
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
    var hdr = packHeader3({ geomSize: region.length, geomCrc: crc32(region), artOff: artOff,
                           artSize: artSize, artCrc: artCrcAll, nSections: sections.length,
                           serial: o.serial || 0, bakedMs: o.bakedMs || 0, name: o.name });
    var out = new Uint8Array(artOff + artSize), p = HDR_WORDS * 4;
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
    if (w[2] !== GEOM_ABI3) {
      throw new Error('bundle geometry v' + w[2] + '; this build reads v' + GEOM_ABI3 + ' — re-bake it');
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
             artSize: w[7], artCrc: w[8], nSections: w[9], serial: w[11], bakedMs: w[12],
             artOff: w[13], name: name };
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
      var b = unpackBlob3(blob);
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
    return rec.abi_min >= 1 && rec.abi_min <= rec.abi_max &&
           rec.abi_max >= GEOM_ABI3 && rec.abi_min <= GEOM_ABI3;
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
      col = rgb565Plane(p.rgba, p.w, p.h);
      if (alpha) col = concat([col, alphaPlane(p.rgba, p.w, p.h)]);
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
  function bakeClock3(o) {
    var scrW = o.scr_w || SCREEN_W, scrH = o.scr_h || SCREEN_H;
    var hands = o.hands || [], shadows = o.shadows || [], plates = o.plates || [];
    var sc = {}, k;
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
    } else if (sc.bg_frames) {
      throw new Error('bg_frames = ' + sc.bg_frames + ' with a still plate: the scalars and the table disagree, and the pair would name a bank the art does not hold');
    }
    /* §31's sprite-region carrier: a carriage bank — the tourbillon row at hand
     * slot 5 that d32 proved on glass. The dial fans its frames by the seconds
     * angle plus the row's start orientation, so the bake hands it the motion's
     * bounding box as the row's extent and the loop's fps as the scalar. */
    var car = o.carriage;
    if (car) {
      if (sc.tourbillon_n) throw new Error('a carriage row while the scalars name tourbillon_n = ' + sc.tourbillon_n + ': the row’s planes are the bank’s length');
      if (!(car.frames && car.frames.length >= 2)) throw new Error('a carriage bank of ' + (car.frames ? car.frames.length : 0) + ' frames: a carriage is an animation, and one frame is a static hand this bake seats elsewhere');
      if (car.frames.length > TOURB_MAX) throw new Error('a carriage bank of ' + car.frames.length + ' frames: the dial holds ' + TOURB_MAX + ' tourbillon records');
      if (!(car.fps >= 1)) throw new Error('a carriage bank at ' + (car.fps || 0) + ' fps: the row animates at its own tempo, and a loop with no tempo is a frame that never arrives');
      var cw = car.w, chh = car.h;
      for (k = 0; k < car.frames.length; k++) {
        var cf = car.frames[k];
        if (!cf || cf.w !== cw || cf.h !== chh) throw new Error('carriage frame ' + k + ' is ' + (cf ? cf.w + 'x' + cf.h : 'nothing') + ' against a ' + cw + 'x' + chh + ' row: every frame of one bank is one extent, the same box the dial invalidates');
      }
      sc.tourbillon_n = car.frames.length; sc.tourbillon_fps = car.fps;
    } else {
      if (sc.tourbillon_n) throw new Error('tourbillon_n = ' + sc.tourbillon_n + ': a carriage is a row this bake does not assemble, and the scalar would name a bank the section does not hold');
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
      if (ordr < 0 || ordr > 4)
        throw new Error('a bank row at slot ' + slot3 + ' ordered at rung ' + ordr +
          ': a window\u2019s depth is a bucket under one of the dial\u2019s five live layers');
      for (j3 = 0; j3 < pl3.length; j3++) {
        var pp3 = pl3[j3];
        if (!pp3 || pp3.w !== cwr || pp3.h !== chr)
          throw new Error('complication ' + slot3 + ' frame ' + j3 + ' is ' + (pp3 ? pp3.w + 'x' + pp3.h : 'nothing') +
            ' against the row\u2019s ' + cwr + 'x' + chr + ': one bank, one extent');
      }
      compB += cwr * chr * 3 * want3;
    }
    if (!sc.tourbillon_n) sc.tourbillon_n = 0;
    sc.plate_off = 0;
    if (frames === undefined) { sc.bg_frames = 0; sc.gif_frames = 0; }

    // Every row starts from blankElement(): packElements refuses a missing field rather
    // than defaulting it, because a default applied at pack time is a value nobody wrote.
    // The bank builds empty and every plane enters through row() or rowFrames(), so no
    // plane is ever counted twice and the plate's rows keep the colour-only cost the
    // projection reads them at.
    var bank = { art: new Uint8Array(0), sprites: [], size: 0 };
    var elems = [], nid = 1, ord = 1, handSlot = {};
    function row(e, plane, masked) {
      var b = bank3([plane], { alpha: masked });
      bank.art = concat([bank.art, b.art]);
      bank.sprites.push({ off: bank.size, size: b.sprites[0].size, w: plane.w, h: plane.h });
      bank.size += b.sprites[0].size;
      var rec = blankElement();
      for (var f in e) rec[f] = e[f];
      rec.frame0 = bank.sprites.length - 1;
      elems.push(rec);
    }
    // §31: one row, N planes. The keyframes enter contiguously, in bank order,
    // colour-only like the plate — the projection seats the row against exactly
    // this shape, and the bgAnim consumer flips through it on its beat.
    function rowFrames(e, planes, masked) {
      var rec = blankElement();
      for (var f in e) rec[f] = e[f];
      rec.frame0 = bank.sprites.length;
      for (var j = 0; j < planes.length; j++) {
        var b = bank3([planes[j]], { alpha: masked });
        bank.art = concat([bank.art, b.art]);
        bank.sprites.push({ off: bank.size, size: b.sprites[0].size, w: planes[j].w, h: planes[j].h });
        bank.size += b.sprites[0].size;
      }
      elems.push(rec);
    }
    if (frames) {
      rowFrames({ n: nid++, 'class': classId('plate'), slot: 0, x: 0, y: 0, w: scrW, h: scrH,
                  order: 0, alpha: 255, n_frames: frames.length }, frames, false);
    } else {
      row({ n: nid++, 'class': classId('plate'), slot: 0, x: 0, y: 0, w: scrW, h: scrH,
            order: 0, alpha: 255, n_frames: 1 }, pl, false);
    }
    // Depth: plate, then the hands in the order given, then their shadows beneath the hands
    // that cast them, then the overlay last — FORMAT 2's clock_view draws the crystal over
    // everything, and the table says what the renderer already does.
    for (k = 0; k < hands.length; k++) {
      var h = hands[k];
      if (!(h.slot >= 0 && h.slot < 5)) throw new Error('a hand at slot ' + h.slot + ': the compiled clock has five hand fields, slots 0-4');
      handSlot[h.slot] = 1;
      row({ n: nid++, 'class': classId('hand'), slot: h.slot, x: h.x, y: h.y, w: h.w, h: h.h,
            pivot_x: h.pivot_x, pivot_y: h.pivot_y, order: ord++, alpha: h.alpha === undefined ? 255 : h.alpha,
            n_frames: 1, flags: h.flags || 0, scale16: h.scale16 || 0 }, h, true);
    }
    for (k = 0; k < shadows.length; k++) {
      var s = shadows[k];
      if (!handSlot[s.slot]) {
        throw new Error('a shadow at slot ' + s.slot + ' with no hand there: a shadow’s crop pivot is its hand’s centre minus its own draw point, so the light offset is only recoverable from the hand it hangs under');
      }
      row({ n: nid++, 'class': classId('sprite'), slot: s.slot, x: s.x, y: s.y, w: s.w, h: s.h,
            pivot_x: s.pivot_x, pivot_y: s.pivot_y, order: ord++, alpha: s.alpha === undefined ? 255 : s.alpha,
            n_frames: 1, flags: s.flags || 0 }, s, true);
    }
    for (k = 0; k < plates.length; k++) {
      row({ n: nid++, 'class': classId('plate'), slot: 1, x: plates[k].x, y: plates[k].y,
            w: plates[k].w, h: plates[k].h, order: ord++, flags: plates[k].flags || 0,
            alpha: plates[k].alpha === undefined ? 255 : plates[k].alpha, n_frames: 1 }, plates[k], true);
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
      rowFrames({ n: nid++, 'class': classId('hand'), slot: 5, x: cx3, y: cy3,
                  w: cw, h: chh, pivot_x: cpx, pivot_y: cpy,
                  order: ord++, alpha: 255, n_frames: car.frames.length,
                  degrees: car.degrees || 0 }, carPl, true);
    }
    /* D34: the pointers. Every refusal below is orb_clock3.c's AUX projection said
     * here, at the author's elbow, so a bake never ships a pointer the dial would
     * refuse at the flip — the same mirroring the bank rows above run by. A pointer
     * costs ONE masked plate whatever its domain, which is the whole point of the
     * class: a sub-dial seconds hand used to be sixty whole dials. */
    var auxRows = o.aux || [], auxB = 0, k4;
    for (k4 = 0; k4 < auxRows.length; k4++) {
      var ar = auxRows[k4];
      var m4 = ar.motion === undefined ? MOTION_ROTATE
                                       : (typeof ar.motion === 'string' ? MOTION_NAMES.indexOf(ar.motion) : ar.motion);
      if (!(m4 >= 0 && m4 < MOTION_NAMES.length))
        throw new Error('a pointer at slot ' + (ar.slot | 0) + ' moves as motion ' +
          JSON.stringify(ar.motion) + ': the dialect names ' + MOTION_NAMES.join(', ') +
          ' and this names none of them');
      if (!(ar.slot >= 0 && ar.slot < CLK_AUX_MAX))
        throw new Error('a pointer at slot ' + (ar.slot | 0) + ': the clock declares ' +
          CLK_AUX_MAX + ' pointer slots and the manifest of any app may not invent one');
      var s4 = String(ar.src), sid4 = SOURCES.indexOf(s4);
      if (sid4 < 0)
        throw new Error('a pointer at slot ' + (ar.slot | 0) + ' driven by ' + q(s4) +
          ': this dial reads ' + SOURCES.length + ' values and none of them is that');
      if (s4 === 'none' || s4 === 'text' || s4 === 'value')
        throw new Error('a pointer at slot ' + (ar.slot | 0) + ' driven by ' + q(s4) +
          ': a value that never changes, or one this dial cannot derive, is a row that ' +
          'would sit still and read as a watch that is simply wrong');
      if (!(ar.vmax >= 1))
        throw new Error('a pointer at slot ' + (ar.slot | 0) + ' with a domain of ' + ar.vmax +
          ': its span is shared across the domain, so nothing can be computed from it');
      if ((ar.vmin | 0) > (ar.vmax | 0))
        throw new Error('a pointer at slot ' + (ar.slot | 0) + ' whose vmin ' + (ar.vmin | 0) +
          ' is above vmax ' + (ar.vmax | 0) + ': the domain runs low to high');
      if (!(ar.span | 0))
        throw new Error('a pointer at slot ' + (ar.slot | 0) + ' with a span of 0: it would ' +
          'sit at its zero forever, which is a plate drawn as a row');
      if (motionIsSlide(m4)) {
        if (Math.abs(ar.span) > scrW || Math.abs(ar.zero | 0) > scrW)
          throw new Error('a pointer at slot ' + (ar.slot | 0) + ' slides by ' + ar.span +
            ' from ' + (ar.zero | 0) + ': a slide’s span and zero are pixels and this ' +
            'one leaves the ' + scrW + ' px panel');
      } else {
        if (Math.abs(ar.zero | 0) > 3600)
          throw new Error('a pointer at slot ' + (ar.slot | 0) + ' with a zero of ' + (ar.zero | 0) +
            ': an offset past one full turn is not a direction');
        if (Math.abs(ar.span) > 3600)
          throw new Error('a pointer at slot ' + (ar.slot | 0) + ' with a span of ' + ar.span +
            ': more than one full turn is a disc, not a hand');
      }
      var apr = ar.plane;
      if (!apr) throw new Error('a pointer at slot ' + (ar.slot | 0) + ' with no plate');
      if (apr.w !== (ar.w | 0) || apr.h !== (ar.h | 0))
        throw new Error('a pointer at slot ' + (ar.slot | 0) + ' is ' + (apr.w|0) + 'x' + (apr.h|0) +
          ' against the row’s ' + (ar.w|0) + 'x' + (ar.h|0) + ': one row, one extent');
      if (!motionIsSlide(m4)) {
        var apx = ar.pivot_x | 0, apy = ar.pivot_y | 0;
        if (!(apx >= 0 && apx < (ar.w|0) && apy >= 0 && apy < (ar.h|0)))
          throw new Error('a pointer at slot ' + (ar.slot | 0) + ' turns about (' + apx + ',' + apy +
            '), outside its own ' + (ar.w|0) + 'x' + (ar.h|0) + ' plate');
      }
      /* The twin row the composer raised with this pointer (auxPass3): pixels, an
       * extent, and a pivot the dial will rotate it about, same three checks the
       * hand's own plate passes. A slide's twin turns about nothing — its motion is
       * a displacement, and the row below forces the pivot to the plate middle as
       * it does for the hand — so the pivot check is the rotation path's alone. */
      if (ar.shadow) {
        var av = ar.shadow;
        if (!av.rgba) throw new Error('a pointer at slot ' + (ar.slot | 0) + ' carries a shadow row with no pixels');
        if (!(av.w > 0 && av.h > 0) || av.rgba.length !== (av.w | 0) * (av.h | 0) * 4)
          throw new Error('the shadow of the pointer at slot ' + (ar.slot | 0) + ' is ' + (av.w | 0) + 'x' + (av.h | 0)
            + ' against ' + av.rgba.length + ' bytes: one row, one extent');
        if (!motionIsSlide(m4) && !((av.pivot_x | 0) >= 0 && (av.pivot_x | 0) < (av.w | 0)
                                    && (av.pivot_y | 0) >= 0 && (av.pivot_y | 0) < (av.h | 0)))
          throw new Error('the shadow of the pointer at slot ' + (ar.slot | 0) + ' turns about (' + (av.pivot_x | 0) + ',' + (av.pivot_y | 0)
            + '), outside its own ' + (av.w | 0) + 'x' + (av.h | 0) + ' plate');
      }
      auxB += (ar.w | 0) * (ar.h | 0) * 3 + (ar.shadow ? (ar.shadow.w | 0) * (ar.shadow.h | 0) * 3 : 0);
    }
    /* One slot a row, and the twin takes the slot directly after its hand — that
     * adjacency is clock_view's pairing rule, not a layout preference, so the
     * physical numbering is the baker's and every pointer after a shadowed one
     * shifts by two. What the dial cannot hold is refused here, before art. */
    var auxSlot3 = 0;
    for (k4 = 0; k4 < auxRows.length; k4++) auxSlot3 += auxRows[k4].shadow ? 2 : 1;
    if (auxSlot3 > CLK_AUX_MAX)
      throw new Error(auxSlot3 + ' pointer rows — shadow twins counted — exceed the dial\u2019s cap of ' + CLK_AUX_MAX);
    for (k3 = 0; k3 < compRows.length; k3++) {
      var c4 = compRows[k3];
      rowFrames({ n: nid++, 'class': classId('bank'), slot: c4.slot, kind: 0, src: SOURCES.indexOf(String(c4.src)),
                  x: c4.x | 0, y: c4.y | 0, w: c4.w, h: c4.h, order: c4.order | 0, alpha: 255,
                  n_frames: c4.planes.length, scale16: c4.scale16 | 0,
                  flags: c4.hidden ? EF_HIDDEN : 0 }, c4.planes, true);
    }
    var auxSlotW = 0;
    for (k4 = 0; k4 < auxRows.length; k4++) {
      var a5 = auxRows[k4], m5 = a5.motion === undefined ? MOTION_ROTATE
              : (typeof a5.motion === 'string' ? MOTION_NAMES.indexOf(a5.motion) : a5.motion);
      // A slide carries no pivot to turn about; its own centre is the origin the row
      // places, and clk_sprite_t's pivot is read only by the dirty-mark maths. The
      // motion rides the row's `kind` column and the drive its four appended words.
      var or5 = a5.order === undefined ? ord : a5.order;
      row({ n: nid++, 'class': classId('aux'), slot: auxSlotW++, kind: m5, src: SOURCES.indexOf(String(a5.src)),
            x: a5.x | 0, y: a5.y | 0, w: a5.w | 0, h: a5.h | 0,
            pivot_x: motionIsSlide(m5) ? ((a5.w | 0) >> 1) : (a5.pivot_x | 0),
            pivot_y: motionIsSlide(m5) ? ((a5.h | 0) >> 1) : (a5.pivot_y | 0),
            order: or5, alpha: 255, n_frames: 1,
            span: a5.span | 0, zero: a5.zero | 0, vmin: a5.vmin | 0, vmax: a5.vmax | 0,
            flags: a5.hidden ? EF_HIDDEN : 0 }, a5.plane, true);
      /* The twin: the hand's drive words verbatim — the loader validates them like
       * any row's and the driver ignores them, taking the pose from the hand — and
       * its own draw point and pivot column, whose sum (x + pivot) is the hand's
       * centre plus the light, which is the whole §25.2 placement in three numbers.
       * AUX_F_SHADOW3 is what tells the dial this row never drives itself. */
      if (a5.shadow) {
        row({ n: nid++, 'class': classId('aux'), slot: auxSlotW++, kind: m5, src: SOURCES.indexOf(String(a5.src)),
              x: a5.shadow.x | 0, y: a5.shadow.y | 0, w: a5.shadow.w | 0, h: a5.shadow.h | 0,
              pivot_x: motionIsSlide(m5) ? ((a5.shadow.w | 0) >> 1) : (a5.shadow.pivot_x | 0),
              pivot_y: motionIsSlide(m5) ? ((a5.shadow.h | 0) >> 1) : (a5.shadow.pivot_y | 0),
              order: or5, alpha: 255, n_frames: 1,
              span: a5.span | 0, zero: a5.zero | 0, vmin: a5.vmin | 0, vmax: a5.vmax | 0,
              flags: (a5.hidden ? EF_HIDDEN : 0) | AUX_F_SHADOW3 }, a5.shadow, true);
      }
    }
    for (k = 0; k < 5; k++) sc['order' + k] = (o.order && o.order[k]) || 0;
    sc.order_n = o.order ? Math.min(o.order.length, 5) : 0;
    // The D34 bench lesson, enforced: clock_view treats every order entry as a
    // hand-slot id verbatim, so a seated hand that no order entry names is art
    // the file carries and the dial never paints. A bake that mis-spells its
    // order (the +1 FORMAT 2 residue that lost Skeleton its hour hand on glass)
    // must fail here, not on the watch.
    if (o.order) {
      for (k = 0; k < hands.length; k++) {
        if (o.order.indexOf(hands[k].slot) < 0) {
          throw new Error('the hand at slot ' + hands[k].slot + ' is seated but the order table [' +
            o.order.join(',') + '] never names it: order entries are hand-slot ids verbatim, ' +
            'and a hand the order omits draws nothing on the watch');
        }
      }
    }

    // The D29 sum, named before the file is written: the plate bank is the part
    // of an over-cap bake an owner can actually trade away, so the sentence says
    // what it cost. checkBudget3 still refuses the same overage at the door —
    // this is the authoring half of §31, and it must never disagree with it.
    if (bank.size > SLOT_ART_CAP3) {
      throw new Error('the bake\'s art bank is ' + bank.size + ' B against the ' +
        SLOT_ART_CAP3 + ' B the slot leaves for banks' +
        (frames ? ': the plate\'s ' + frames.length + ' keyframes of ' + scrW + 'x' + scrH +
                ' cost ' + (scrW * scrH * 2 * frames.length) + ' B of it, and one keyframe ' +
                'is ' + (scrW * scrH * 2) + ' B' : '') +
        (compB ? (frames ? ', and' : ':') + ' the complication tiles cost ' + compB + ' B' : '') +
        (auxB ? (frames || compB ? ', and' : ':') + ' the pointer plates cost ' + auxB + ' B' : '') +
        ' — drop frames or shrink the art');
    }

    var b = writeBundle3({ name: o.name, serial: o.serial || 0, bakedMs: o.bakedMs || 0,
      sections: [{ slug: 'clock', scr_w: scrW, scr_h: scrH, elements: elems, scalars: sc,
                   sprites: bank.sprites, art: bank.art }] });
    b.meter = budget3(b.bytes);
    b.n_elem = elems.length;
    return b;
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
  function splitPolicy3(m) {
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
    var maxB = Math.min(BG3_FRAMES_MAX, Math.floor(left / kf));
    var b2 = maxB >= 2 ? thin(B, Math.ceil(B / maxB)) : { keyframes: 0, fps: 0, decim: 1, bbox: [bw, bh] };
    if (b2.keyframes >= 2)
      return out('bank', b2, car, 'plate keyframe bank: ' + b2.keyframes + ' of ' + B + ' frames at '
        + b2.fps + ' fps' + (b2.decim > 1 ? ' (thinned by ' + b2.decim + ')' : '') + ', '
        + (b2.keyframes * kf) + ' B' + (car ? ' over the ' + carCost + ' B the carriage took; ' + car.sentence : ''));
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
      if (c.type === 'window') return c.shows !== 'custom';
      if (c.type === 'flare') return c.icon === 'datetext';
      if (c.type === 'subdial') return c.scale !== 'none';   // an engraved dial with no hand is plate art
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
  var COMP3_SCALE_SRC = { seconds: 'second', date: 'date', day: 'weekday', month: 'month', hours: 'hour12' };
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
  function compPlan3(comps) {
    var rows = [], refuses = [];
    (comps || []).forEach(function (c) {
      var src = null, id = c.id === undefined ? '?' : c.id;
      if (c.type === 'opening') return;                                  // a hole in the plate
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
        if (c.scale === 'none') return;                                  // no hand: plate art
        if (c.carrier === 'pointer' || pointerDefault3(c, comps)) return;   // D34: pointerPlan3 owns this row, not the bank
        src = (c.scale === undefined) ? 'second' : COMP3_SCALE_SRC[c.scale];
        if (!src) { refuses.push('the subdial \u201c' + id + '\u201d reads scale \u201c' + c.scale + '\u201d: the dial indexes plates by calendar value and none matches'); return; }
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
  /* D34 follow-up: the Drive chip defaults a pointer-capable subdial to Pointer,
   * and Cx defaults never write into the design — so the bake reads the default
   * the same way the chip shows it. An unset carrier on the seconds or hours
   * ring belongs to the pointer row; an explicit `plates` is the author's
   * escape hatch and stays on the bank. A subdial HOSTING another indication
   * never defaults to the pointer either: the plates carrier punches the host
   * tile so the hosted tile sits inside it, and a pointer row carries nothing
   * to punch. The UI and the bake share this one predicate. */
  function pointerDefault3(c, comps) {
    if (c.type !== 'subdial' || c.carrier != null) return false;
    if (!(c.scale === undefined || c.scale === 'seconds' || c.scale === 'hours')) return false;
    return !(comps || []).some(function (x) { return x.host === c.id; });
  }
  var COMP3_POINTER_SPAN = 3600;                       // one whole turn, tenths of a degree
  function pointerPlan3(comps) {
    var rows = [], refuses = [];
    (comps || []).forEach(function (c) {
      if (c.type !== 'subdial') return;
      if (c.carrier === 'plates') return;
      if (c.carrier !== 'pointer' && !pointerDefault3(c, comps)) return;
      var id = c.id === undefined ? '?' : c.id;
      if ((comps || []).some(function (x) { return x.host === id; })) {
        refuses.push('the subdial \u201c' + id + '\u201d hosts another indication: the plates carrier punches its '
          + 'plate so the hosted tile sits inside the hole, and a pointer row carries nothing to punch — drive it as plates');
        return;
      }
      var src = c.scale === undefined ? 'second' : COMP3_SCALE_SRC[c.scale];
      if (!src || !COMP3_RING_FULL[src]) {
        refuses.push('the subdial \u201c' + id + '\u201d asks to be driven as a pointer and reads scale '
          + '\u201c' + (c.scale === undefined ? 'seconds' : c.scale) + '\u201d: a pointer plate turns through '
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

  root.GorbBake = {
    SCREEN: SCREEN_W, KINDS: KINDS, SOURCES: SOURCES, COMP_KINDS: COMP_KINDS,
    THEME_FIELDS: THEME_FIELDS, SPRITE_FIELDS: SPRITE_FIELDS, GEOM_COUNTS: GEOM_COUNTS,
    HDR_WORDS: HDR_WORDS, NAME_MAX: NAME_MAX, SLOT_CAP: SLOT_CAP,
    GEOM_BUDGET: GEOM_BUDGET,
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
    // FORMAT 3 (D32/D33): the multi-section bundle, the element table, and the D29 gate.
    // The browser owns the whole dialect, caps and refusal order included, so the release
    // ships as a web page that flashes over Web Serial; internal/orbnet's reader is the
    // hub's pre-flight mirror of these same rules, checked against the same committed
    // fixture. packElements refuses through the same check_element / check_table rules the
    // device door applies, so an element a theme cannot name never reaches a card.
    FORMAT3: FORMAT3, GEOM_ABI3: GEOM_ABI3, SECT_MAX: SECT_MAX, IDX_WORDS: IDX_WORDS,
    SCALARS_N: SCALARS_N, ELEMENT_WORDS: ELEMENT_WORDS, ELEMENT_FIELDS: ELEMENT_FIELDS,
    CLASS_NAMES: CLASS_NAMES, CLASS_APP_ADV: CLASS_APP_ADV, CLASS_NO_ANIM: CLASS_NO_ANIM,
    EF_ALPHA: EF_ALPHA, EF_LOOP: EF_LOOP, EF_OS_ADV: EF_OS_ADV, EF_HIDDEN: EF_HIDDEN,
    AUX_F_SHADOW3: AUX_F_SHADOW3,
    ELEM_MAX: ELEM_MAX, ELEM_FRAMES_MAX: ELEM_FRAMES_MAX,
    // D34: the pointer vocabulary, exported so the composer and the suite read the
    // same table the baker refuses against rather than restating ids.
    CLASS_AUX: CLASS_AUX, MOTION_NAMES: MOTION_NAMES, MOTION_ROTATE: MOTION_ROTATE,
    MOTION_SLIDE_X: MOTION_SLIDE_X, MOTION_SLIDE_Y: MOTION_SLIDE_Y,
    motionIsSlide: motionIsSlide, AUX_MAX: CLK_AUX_MAX,
    GEOM_CAP: GEOM_CAP3, ART_CAP: SLOT_ART_CAP3, SLUG_MAX: SLUG_MAX,
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
    bank3: bank3, bakeClock3: bakeClock3, budget3: budget3, splitPolicy3: splitPolicy3,
    carriageRect3: carriageRect3,
    liveComps3: liveComps3, compPlan3: compPlan3, pointerPlan3: pointerPlan3,
    pointerDefault3: pointerDefault3
  };
})(typeof window !== 'undefined' ? window : (typeof globalThis !== 'undefined' ? globalThis : this));
if (typeof module !== 'undefined' && module.exports) module.exports = (typeof window !== 'undefined' ? window : globalThis).GorbBake;
