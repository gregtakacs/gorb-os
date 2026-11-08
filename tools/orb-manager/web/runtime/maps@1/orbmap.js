/* orbmap.js — the OS maps' tool, in JavaScript (radar-plan.md §2.4, R4).
 *
 * Three ports, each held to its original by a test:
 *  - an OpenMapTiles vector tile (MVT v2) reader, as tools/mvt.py;
 *  - the .orbmap FORMAT 1 builder and writer, as tools/orbmap.py, byte for byte
 *    (tools/host_orbmap_js_test.py builds the same tiles with both and compares);
 *  - the role rasteriser, as main/map/orb_map_raster.c, pixel for pixel (the same test
 *    renders the same files with both). The C works in single-precision floats, so this
 *    port rounds every float step to one (Math.fround), in the C's order.
 *
 * DOM-free, so the hub page and node run the same code. The page fetches tiles through
 * the hub's forwarder (§0 ruling 10) and hands the bytes in; nothing here fetches.
 */
(function (root) {
  'use strict';

  var FORMAT = 1, HEADER = 36, UNIT = 16, HALF = 330;
  var CLIP = (HALF + 4) * UNIT, TOL = UNIT / 4;
  var ZOOMS = [6, 7, 8, 9, 10, 11];
  var WATER = 0, LAND = 1, URBAN = 2, LAKE = 3, RIVER = 4, ROAD2 = 5, ROAD1 = 6, STATE = 7,
    BORDER = 8, COAST = 9, CITY = 10, AIRPORT = 11, RUNWAY = 12;
  var FILL = 0, LINE = 1;
  var MARK_CITY = 0, MARK_TOWN = 1, MARK_VILLAGE = 2, MARK_AIRPORT = 3;
  var ROLES = ['water', 'land', 'urban', 'lake', 'river', 'minor road', 'major road', 'state line',
    'border', 'coast', 'city', 'airport', 'runway'];

  /* ---- tools/mvt.py: the vector tile ------------------------------------------------ */

  function Rd(b) { this.b = b; this.i = 0; }
  Rd.prototype.varint = function () {
    var r = 0, s = 1, c;
    do {
      c = this.b[this.i++];
      if (c === undefined) throw new Error('mvt: truncated varint');
      r += (c & 0x7F) * s; s *= 128;
    } while (c >= 0x80);
    return r;
  };
  Rd.prototype.fields = function (each) {
    var n = this.b.length;
    while (this.i < n) {
      var k = this.varint(), f = Math.floor(k / 8), t = k & 7, v;
      if (t === 0) v = this.varint();
      else if (t === 2) { var l = this.varint(); v = this.b.subarray(this.i, this.i + l); this.i += l; }
      else if (t === 1) { v = this.b.subarray(this.i, this.i + 8); this.i += 8; }
      else if (t === 5) { v = this.b.subarray(this.i, this.i + 4); this.i += 4; }
      else throw new Error('mvt: wire type ' + t);
      each(f, t, v);
    }
  };
  function packed(b) {
    var r = new Rd(b), out = [];
    while (r.i < b.length) out.push(r.varint());
    return out;
  }
  var utf8 = typeof TextDecoder !== 'undefined' ? new TextDecoder('utf-8') : null;
  function str(b) { return utf8.decode(b); }
  function value(b) {
    var out;
    new Rd(b).fields(function (f, t, v) {
      if (out !== undefined) return;
      if (f === 1) out = str(v);
      else if (f === 2) out = new DataView(v.buffer, v.byteOffset, 4).getFloat32(0, true);
      else if (f === 3) out = new DataView(v.buffer, v.byteOffset, 8).getFloat64(0, true);
      else if (f === 4 || f === 5) out = v;
      else if (f === 6) out = v % 2 ? -(v + 1) / 2 : v / 2;
      else if (f === 7) out = !!v;
    });
    return out;
  }
  function zz(v) { return v % 2 ? -(v + 1) / 2 : v / 2; }
  function geom(cmds) {
    var out = [], cur = null, x = 0, y = 0, i = 0;
    while (i < cmds.length) {
      var c = cmds[i++], op = c & 7, cnt = c >> 3, k;
      if (op === 1) {
        for (k = 0; k < cnt; k++) {
          if (cur && cur.length) out.push(cur);
          x += zz(cmds[i]); y += zz(cmds[i + 1]); i += 2;
          cur = [[x, y]];
        }
      } else if (op === 2) {
        for (k = 0; k < cnt; k++) { x += zz(cmds[i]); y += zz(cmds[i + 1]); i += 2; cur.push([x, y]); }
      } else if (op === 7 && cur && cur.length) {
        cur.push(cur[0]);
      }
    }
    if (cur && cur.length) out.push(cur);
    return out;
  }
  /* bytes (already gunzipped) -> {layer: {extent, features: [[gtype, props, rings]]}} */
  function readTile(bytes) {
    var layers = {};
    new Rd(bytes).fields(function (f, t, lb) {
      if (f !== 3) return;
      var name = '', extent = 4096, keys = [], vals = [], feats = [];
      new Rd(lb).fields(function (lf, lt, lv) {
        if (lf === 1) name = str(lv);
        else if (lf === 5) extent = lv;
        else if (lf === 3) keys.push(str(lv));
        else if (lf === 4) vals.push(value(lv));
        else if (lf === 2) feats.push(lv);
      });
      var out = feats.map(function (fb) {
        var tags = [], gtype = 0, cmds = [];
        new Rd(fb).fields(function (ff, ft, fv) {
          if (ff === 2) tags = packed(fv);
          else if (ff === 3) gtype = fv;
          else if (ff === 4) cmds = packed(fv);
        });
        var props = {};
        for (var k = 0; k + 1 < tags.length; k += 2) props[keys[tags[k]]] = vals[tags[k + 1]];
        return [gtype, props, geom(cmds)];
      });
      layers[name] = { extent: extent, features: out };
    });
    return layers;
  }

  /* ---- tools/orbmap.py: classes, geometry, the file ---------------------------------- */

  function classify(layer, p, gtype) {
    var c = p['class'];
    if (layer === 'landuse' && (c === 'residential' || c === 'commercial' || c === 'industrial' || c === 'retail') && gtype === 3)
      return [URBAN, FILL];
    if (layer === 'water' && gtype === 3) return [c === 'ocean' ? WATER : LAKE, FILL];
    if (layer === 'waterway' && (c === 'river' || c === 'canal')) return [RIVER, LINE];
    if (layer === 'transportation' && (c === 'secondary' || c === 'tertiary')) return [ROAD2, LINE];
    if (layer === 'transportation' && (c === 'motorway' || c === 'trunk' || c === 'primary')) return [ROAD1, LINE];
    if (layer === 'boundary' && p.maritime !== 1) {
      if (p.admin_level === 4) return [STATE, LINE];
      if (p.admin_level === 2) return [BORDER, LINE];
    }
    if (layer === 'aeroway' && c === 'runway') return [RUNWAY, gtype === 3 ? FILL : LINE];
    return null;
  }

  /* Rounding, the one rule both writers use: half up (floor(v + 0.5)). */
  function rnd(v) { return Math.floor(v + 0.5); }

  function e4(e6) {                /* millionths to ten-thousandths, half away from zero, C truncation */
    var v = e6 >= 0 ? e6 + 50 : e6 - 50;
    return v >= 0 ? Math.floor(v / 100) : -Math.floor(-v / 100);
  }
  function mapName(place, latE6, lonE6, z) {
    return place + '-' + e4(latE6) + '_' + e4(lonE6) + '-z' + z + '.orbmap';
  }
  var DEG = Math.PI / 180;
  function world(lat, lon, z) {
    var n = 256.0 * Math.pow(2, z), s = Math.sin(Math.max(-85.0, Math.min(85.0, lat)) * DEG);
    return [(lon + 180) / 360 * n, (0.5 - Math.log((1 + s) / (1 - s)) / (4 * Math.PI)) * n];
  }
  /* The tiles a level reads: every 256 px tile the ±HALF window around the place touches. */
  function tilesFor(lat, lon, z) {
    var c = world(lat, lon, z), out = [], n = Math.pow(2, z);
    var x0 = Math.floor((c[0] - HALF) / 256), x1 = Math.floor((c[0] + HALF) / 256);
    var y0 = Math.max(0, Math.floor((c[1] - HALF) / 256)), y1 = Math.min(n - 1, Math.floor((c[1] + HALF) / 256));
    for (var x = x0; x <= x1; x++)
      for (var y = y0; y <= y1; y++) out.push({ z: z, x: ((x % n) + n) % n, y: y, ox: x });
    return out;
  }

  function eq(a, b) { return a[0] === b[0] && a[1] === b[1]; }

  function clipPoly(ring, lo, hi) {            /* Sutherland-Hodgman against [lo, hi]^2 */
    function edge(pts, inside, cut) {
      var out = [];
      for (var i = 0; i < pts.length; i++) {
        var cur = pts[i], prev = pts[i === 0 ? pts.length - 1 : i - 1];
        if (inside(cur)) {
          if (!inside(prev)) out.push(cut(prev, cur));
          out.push(cur);
        } else if (inside(prev)) {
          out.push(cut(prev, cur));
        }
      }
      return out;
    }
    function cx(v) { return function (a, b) { return [v, a[1] + (b[1] - a[1]) * (v - a[0]) / (b[0] - a[0])]; }; }
    function cy(v) { return function (a, b) { return [a[0] + (b[0] - a[0]) * (v - a[1]) / (b[1] - a[1]), v]; }; }
    var pts = ring.length > 1 && eq(ring[0], ring[ring.length - 1]) ? ring.slice(0, -1) : ring;
    var steps = [[function (p) { return p[0] >= lo; }, cx(lo)], [function (p) { return p[0] <= hi; }, cx(hi)],
                 [function (p) { return p[1] >= lo; }, cy(lo)], [function (p) { return p[1] <= hi; }, cy(hi)]];
    for (var k = 0; k < 4; k++) {
      if (!pts.length) return [];
      pts = edge(pts, steps[k][0], steps[k][1]);
    }
    return pts;
  }

  function clipLine(line, lo, hi) {            /* Liang-Barsky a segment; visible pieces join */
    var runs = [], cur = [];
    for (var s = 0; s + 1 < line.length; s++) {
      var a = line[s], b = line[s + 1];
      var t0 = 0.0, t1 = 1.0, dx = b[0] - a[0], dy = b[1] - a[1], ok = true;
      var PQ = [[-dx, a[0] - lo], [dx, hi - a[0]], [-dy, a[1] - lo], [dy, hi - a[1]]];
      for (var k = 0; k < 4; k++) {
        var p = PQ[k][0], q = PQ[k][1];
        if (p === 0) { if (q < 0) { ok = false; break; } }
        else {
          var t = q / p;
          if (p < 0) t0 = Math.max(t0, t); else t1 = Math.min(t1, t);
        }
      }
      if (!ok || t0 > t1) {
        if (cur.length > 1) runs.push(cur);
        cur = []; continue;
      }
      var pa = [a[0] + t0 * dx, a[1] + t0 * dy], pb = [a[0] + t1 * dx, a[1] + t1 * dy];
      if (!cur.length || !eq(cur[cur.length - 1], pa)) {
        if (cur.length > 1) runs.push(cur);
        cur = [pa];
      }
      cur.push(pb);
      if (t1 < 1.0) {
        if (cur.length > 1) runs.push(cur);
        cur = [];
      }
    }
    if (cur.length > 1) runs.push(cur);
    return runs;
  }

  function simplify(pts, tol, closed) {        /* Douglas-Peucker, iterative */
    var n = pts.length;
    if (n < 3) return pts;
    var keep = new Array(n).fill(false);
    keep[0] = keep[n - 1] = true;
    var spans = [[0, n - 1]];
    if (closed) {
      var far = 0, fd = -1;
      for (var i = 0; i < n; i++) {
        var ex = pts[i][0] - pts[0][0], ey = pts[i][1] - pts[0][1], d = ex * ex + ey * ey;
        if (d > fd) { fd = d; far = i; }
      }
      keep[far] = true; spans = [[0, far], [far, n - 1]];
    }
    var t2 = tol * tol;
    while (spans.length) {
      var sp = spans.pop(), a = sp[0], b = sp[1];
      if (b - a < 2) continue;
      var ax = pts[a][0], ay = pts[a][1], bx = pts[b][0], by = pts[b][1];
      var dx = bx - ax, dy = by - ay, L = dx * dx + dy * dy, best = -1.0, bi = -1;
      for (var k = a + 1; k < b; k++) {
        var px = pts[k][0], py = pts[k][1], dd;
        if (L === 0) dd = (px - ax) * (px - ax) + (py - ay) * (py - ay);
        else {
          var t = Math.max(0.0, Math.min(1.0, ((px - ax) * dx + (py - ay) * dy) / L));
          var qx = px - ax - t * dx, qy = py - ay - t * dy;
          dd = qx * qx + qy * qy;
        }
        if (dd > best) { best = dd; bi = k; }
      }
      if (best > t2) { keep[bi] = true; spans.push([a, bi], [bi, b]); }
    }
    return pts.filter(function (p, i) { return keep[i]; });
  }

  function quantise(pts) {
    var out = [];
    for (var i = 0; i < pts.length; i++) {
      var q = [rnd(pts[i][0]), rnd(pts[i][1])];
      if (!out.length || !eq(out[out.length - 1], q)) out.push(q);
    }
    return out;
  }

  /* ---- the bytes ------------------------------------------------------------------ */

  function Out() { this.a = new Uint8Array(1 << 16); this.n = 0; }
  Out.prototype.byte = function (v) {
    if (this.n === this.a.length) { var b = new Uint8Array(this.a.length * 2); b.set(this.a); this.a = b; }
    this.a[this.n++] = v;
  };
  Out.prototype.bytes = function (bs) { for (var i = 0; i < bs.length; i++) this.byte(bs[i]); };
  Out.prototype.varint = function (v) {
    for (;;) {
      var b = v % 128; v = Math.floor(v / 128);
      if (v) this.byte(b | 0x80); else { this.byte(b); return; }
    }
  };
  function zig(v) { return v >= 0 ? 2 * v : -2 * v - 1; }

  var CRC_T = (function () {
    var t = new Uint32Array(256);
    for (var n = 0; n < 256; n++) {
      var c = n;
      for (var k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1;
      t[n] = c >>> 0;
    }
    return t;
  })();
  function crc32(b) {
    var c = 0xFFFFFFFF;
    for (var i = 0; i < b.length; i++) c = CRC_T[(c ^ b[i]) & 0xFF] ^ (c >>> 8);
    return (c ^ 0xFFFFFFFF) >>> 0;
  }
  var utf8e = typeof TextEncoder !== 'undefined' ? new TextEncoder() : null;

  /* features: [[role, kind, [ring...]]] in 1/16 px; marks: [[kind, rank, x, y, name]] */
  function encode(lat, lon, zoom, features, marks) {
    var body = new Out();
    var feats = features.map(function (f, i) { return [f, i]; }).sort(function (p, q) {
      return p[0][1] - q[0][1] || p[0][0] - q[0][0] || p[1] - q[1];       /* kind, role; stable */
    }).map(function (p) { return p[0]; });
    feats.forEach(function (f) {
      body.byte(f[0]); body.byte(f[1]); body.varint(f[2].length);
      f[2].forEach(function (ring) {
        body.varint(ring.length);
        var px = 0, py = 0;
        ring.forEach(function (p) { body.varint(zig(p[0] - px)); body.varint(zig(p[1] - py)); px = p[0]; py = p[1]; });
      });
    });
    marks.forEach(function (m) {
      var nb = utf8e.encode(m[4]).subarray(0, 23);
      body.byte(m[0]); body.byte(Math.min(m[1], 255));
      body.varint(zig(m[2])); body.varint(zig(m[3]));
      body.byte(nb.length); body.bytes(nb);
    });
    var b = body.a.subarray(0, body.n), out = new Uint8Array(HEADER + body.n), dv = new DataView(out.buffer);
    out.set([0x4F, 0x52, 0x42, 0x4D]);
    dv.setUint16(4, FORMAT, true); dv.setUint16(6, HEADER, true);
    dv.setInt32(8, rnd(lat * 1e6), true); dv.setInt32(12, rnd(lon * 1e6), true);
    out[16] = zoom;
    dv.setUint32(20, feats.length, true); dv.setUint32(24, marks.length, true);
    dv.setUint32(28, body.n, true); dv.setUint32(32, crc32(b), true);
    out.set(b, HEADER);
    return out;
  }

  /* The file's header, or null when it is not a FORMAT 1 map or its body fails the CRC. */
  function info(buf) {
    if (buf.length < HEADER || buf[0] !== 0x4F || buf[1] !== 0x52 || buf[2] !== 0x42 || buf[3] !== 0x4D) return null;
    var dv = new DataView(buf.buffer, buf.byteOffset, buf.length);
    if (dv.getUint16(4, true) !== FORMAT) return null;
    var hl = dv.getUint16(6, true), blen = dv.getUint32(28, true);
    if (hl < HEADER || buf.length - hl < blen) return null;
    if (crc32(buf.subarray(hl, hl + blen)) !== dv.getUint32(32, true)) return null;
    return { latE6: dv.getInt32(8, true), lonE6: dv.getInt32(12, true), zoom: buf[16],
             features: dv.getUint32(20, true), marks: dv.getUint32(24, true), bytes: buf.length };
  }

  /* One level from its decoded tiles: tiles is [{x, y, layers}] (x unwrapped: the column
   * the window put it in), in any order; they are read by (x, y) as orbmap.py does. */
  function build(tiles, lat, lon, zoom) {
    var c = world(lat, lon, zoom), cx = c[0], cy = c[1];
    var feats = [], marks = [], seen = {}, raw = 0;
    var order = tiles.slice().sort(function (a, b) { return a.x - b.x || a.y - b.y; });
    order.forEach(function (tile) {
      var tx = tile.x, ty = tile.y;
      Object.keys(tile.layers).forEach(function (layer) {
        var L = tile.layers[layer], ext = L.extent;
        var to = function (g) { return [(tx * 256 + g[0] * 256 / ext - cx) * UNIT, (ty * 256 + g[1] * 256 / ext - cy) * UNIT]; };
        L.features.forEach(function (ft) {
          var gtype = ft[0], props = ft[1], rings = ft[2];
          if ((layer === 'place' || layer === 'aerodrome_label') && rings.length) {
            var xy = to(rings[0][0]), x = xy[0], y = xy[1], kind, name, rank;
            if (Math.abs(x) > HALF * UNIT || Math.abs(y) > HALF * UNIT) return;
            if (layer === 'place') {
              kind = { city: MARK_CITY, town: MARK_TOWN, village: MARK_VILLAGE }[props['class']];
              name = props['name:latin'] || props.name_en || props.name;
              rank = Math.trunc(Number(props.rank !== undefined ? props.rank : 20));
            } else {
              kind = MARK_AIRPORT; name = props.iata || props.icao; rank = props.iata ? 1 : 8;
            }
            if (kind === undefined || !name || seen[kind + '\u0000' + name]) return;
            seen[kind + '\u0000' + name] = 1;
            marks.push([kind, rank, rnd(x), rnd(y), name]);
            return;
          }
          var cl = classify(layer, props, gtype);
          if (!cl) return;
          var role = cl[0], kd = cl[1], out = [];
          rings.forEach(function (r) {
            var pts = r.map(to);
            raw += pts.length;
            if (kd === FILL) {
              var q = quantise(simplify(clipPoly(pts, -CLIP, CLIP), TOL, true));
              if (q.length >= 3) out.push(q);
            } else {
              clipLine(pts, -CLIP, CLIP).forEach(function (run) {
                var q2 = quantise(simplify(run, TOL, false));
                if (q2.length >= 2) out.push(q2);
              });
            }
          });
          if (out.length) feats.push([role, kd, out]);
        });
      });
    });
    var mi = marks.map(function (m, i) { return [m, i]; });
    mi.sort(function (p, q) { return p[0][1] - q[0][1] || p[0][0] - q[0][0] || p[1] - q[1]; });
    return { bytes: encode(lat, lon, zoom, feats, mi.map(function (p) { return p[0]; })), raw: raw };
  }

  /* ---- main/map/orb_map_raster.c: the role raster ------------------------------------ */

  var W = 466, TW = 59, RASTER_B = TW * TW * 32;
  var f32 = Math.fround;
  function nib(x, y) { return (((y >> 3) * TW + (x >> 3)) << 6) | ((y & 7) << 3 | (x & 7)); }
  function roleAt(ras, x, y) { var i = nib(x, y); return (i & 1) ? ras[i >> 1] & 0x0F : ras[i >> 1] >> 4; }
  function put(ras, x, y, v) {
    if (x < 0 || x >= W || y < 0 || y >= W) return;
    var i = nib(x, y), j = i >> 1;
    ras[j] = (i & 1) ? (ras[j] & 0xF0) | v : (ras[j] & 0x0F) | (v << 4);
  }
  function span(ras, y, x0, x1, v) {           /* [x0, x1) */
    if (x0 < 0) x0 = 0;
    if (x1 > W) x1 = W;
    for (var x = x0; x < x1; x++) put(ras, x, y, v);
  }
  var DASH = [[0, 0, 0, 0], [4, 3, 0, 0], [9, 4, 0, 0], [1, 3, 0, 0], [7, 3, 1, 3]];

  function render(buf, st) {
    if (buf.length < HEADER || buf[0] !== 0x4F || buf[1] !== 0x52 || buf[2] !== 0x42 || buf[3] !== 0x4D ||
        (buf[4] | buf[5] << 8) !== 1) return { err: -1 };
    var dv = new DataView(buf.buffer, buf.byteOffset, buf.length);
    var hl = dv.getUint16(6, true), nf = dv.getUint32(20, true), nm = dv.getUint32(24, true), blen = dv.getUint32(28, true);
    if (hl < HEADER || hl > buf.length || buf.length - hl < blen) return { err: -2 };
    var p = hl, end = hl + blen, bad = false;
    function rv() {
      var v = 0;
      for (var s = 0; s < 35; s += 7) {
        if (p >= end) { bad = true; return 0; }
        var b = buf[p++];
        v = (v | ((b & 0x7F) << s)) >>> 0;
        if (b < 0x80) return v;
      }
      bad = true; return 0;
    }
    function rz() { var v = rv(); return (v >>> 1) ^ -(v & 1); }
    function rb() { if (p >= end) { bad = true; return 0; } return buf[p++]; }

    var ras = new Uint8Array(RASTER_B).fill(LAND | LAND << 4);
    var k = f32(f32(st.scale) / UNIT), rot = f32(st.rot_rad || 0);
    var cs = f32(Math.cos(rot)), sn = f32(Math.sin(rot));
    var ta = f32(k * cs), tb = f32(k * -sn), tc = f32(k * sn), td = f32(k * cs);
    function tx(x, y) {
      return [f32(f32(f32(W * 0.5) + f32(ta * x)) + f32(tb * y)), f32(f32(f32(W * 0.5) + f32(tc * x)) + f32(td * y))];
    }
    var stats = { features: 0, fills: 0, lines: 0, edges: 0, segments: 0, marks: 0, skipped: 0 };

    var edges = [];
    function addEdge(x0, y0, x1, y1) {
      if (y0 === y1) return;
      if (y0 > y1) { var t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; }
      var r0 = Math.ceil(f32(y0 - 0.5)), r1 = Math.ceil(f32(y1 - 0.5));
      if (r0 < 0) r0 = 0;
      if (r1 > W) r1 = W;
      if (r0 >= r1) return;
      var dxdy = f32(f32(x1 - x0) / f32(y1 - y0));
      edges.push({ dxdy: dxdy, x: f32(x0 + f32(f32(f32(f32(r0) + 0.5) - y0) * dxdy)), y0: r0, y1: r1 });
    }
    function fill(v) {
      var n = edges.length;
      if (!n) return;
      var cnt = new Uint32Array(W + 1), order = new Uint32Array(n), act = [], i;
      for (i = 0; i < n; i++) cnt[edges[i].y0 + 1]++;
      for (var r = 0; r < W; r++) cnt[r + 1] += cnt[r];
      for (i = 0; i < n; i++) order[cnt[edges[i].y0]++] = i;
      var next = 0, xs = [];
      for (var y = edges[order[0]].y0; y < W && (next < n || act.length); y++) {
        while (next < n && edges[order[next]].y0 === y) act.push(order[next++]);
        xs.length = 0;
        for (i = 0; i < act.length;) {
          var g = edges[act[i]];
          if (g.y1 <= y) { act[i] = act[act.length - 1]; act.pop(); continue; }
          if (xs.length < 256) xs.push(g.x);
          g.x = f32(g.x + g.dxdy);
          i++;
        }
        for (i = 1; i < xs.length; i++) {
          var kk = xs[i], j = i;
          while (j && xs[j - 1] > kk) { xs[j] = xs[j - 1]; j--; }
          xs[j] = kk;
        }
        for (i = 0; i + 1 < xs.length; i += 2)
          span(ras, y, Math.ceil(f32(xs[i] - 0.5)), Math.ceil(f32(xs[i + 1] - 0.5)), v);
        if (!act.length && next < n) y = edges[order[next]].y0 - 1;
      }
    }

    function dashOn(pen) {
      if (!pen.pat || pen.plen <= 0) return true;
      var t = f32(pen.phase % pen.plen);
      for (var i = 0; i < 4 && pen.pat[i]; i++) {
        if (t < pen.pat[i]) return !(i & 1);
        t = f32(t - pen.pat[i]);
      }
      return false;
    }
    function stamp(pen, x, y) {
      if (pen.w <= 1) { put(ras, x, y, pen.v); return; }
      if (pen.w === 2) {
        put(ras, x, y, pen.v); put(ras, x + 1, y, pen.v); put(ras, x, y + 1, pen.v); put(ras, x + 1, y + 1, pen.v);
        return;
      }
      var lo = -((pen.w - 1) >> 1), hi = pen.w >> 1;
      for (var dy = lo; dy <= hi; dy++)
        if (y + dy >= 0 && y + dy < W) span(ras, y + dy, x + lo, x + hi + 1, pen.v);
    }
    function clipSeg(s, m) {                   /* s = [x0, y0, x1, y1], floats */
      var t0 = 0, t1 = 1, dx = f32(s[2] - s[0]), dy = f32(s[3] - s[1]);
      var P = [-dx, dx, -dy, dy];
      var Q = [f32(s[0] + m), f32(f32(f32(W - 1) + m) - s[0]), f32(s[1] + m), f32(f32(f32(W - 1) + m) - s[1])];
      for (var i = 0; i < 4; i++) {
        if (P[i] === 0) { if (Q[i] < 0) return false; continue; }
        var t = f32(Q[i] / P[i]);
        if (P[i] < 0) { if (t > t1) return false; if (t > t0) t0 = t; }
        else { if (t < t0) return false; if (t < t1) t1 = t; }
      }
      var ax = f32(s[0] + f32(t0 * dx)), ay = f32(s[1] + f32(t0 * dy));
      s[2] = f32(s[0] + f32(t1 * dx)); s[3] = f32(s[1] + f32(t1 * dy)); s[0] = ax; s[1] = ay;
      return true;
    }
    function hyp(ax, ay, bx, by) {
      var dx = f32(bx - ax), dy = f32(by - ay);
      return f32(Math.sqrt(f32(f32(dx * dx) + f32(dy * dy))));
    }
    function rnd32(v) { return Math.floor(f32(v + 0.5)); }
    function segment(pen, fx0, fy0, fx1, fy1) {
      var s = [fx0, fy0, fx1, fy1], x, y, x1, y1, dx, sx, dy, sy, err, e2;
      if (!pen.pat) {
        if (!clipSeg(s, pen.w)) return;
        x = rnd32(s[0]); y = rnd32(s[1]); x1 = rnd32(s[2]); y1 = rnd32(s[3]);
        dx = Math.abs(x1 - x); sx = x < x1 ? 1 : -1; dy = -Math.abs(y1 - y); sy = y < y1 ? 1 : -1;
        err = dx + dy;
        for (;;) {
          stamp(pen, x, y);
          if (x === x1 && y === y1) break;
          e2 = 2 * err;
          if (e2 >= dy) { err += dy; x += sx; }
          if (e2 <= dx) { err += dx; y += sy; }
        }
        return;
      }
      var len = hyp(fx0, fy0, fx1, fy1);
      if (!clipSeg(s, pen.w)) { pen.phase = f32(pen.phase + len); return; }
      var lead = hyp(fx0, fy0, s[0], s[1]);
      pen.phase = f32(pen.phase + lead);
      x = rnd32(s[0]); y = rnd32(s[1]); x1 = rnd32(s[2]); y1 = rnd32(s[3]);
      dx = Math.abs(x1 - x); sx = x < x1 ? 1 : -1; dy = -Math.abs(y1 - y); sy = y < y1 ? 1 : -1;
      var steps = dx > -dy ? dx : -dy;
      var stepl = steps ? f32(hyp(s[0], s[1], s[2], s[3]) / f32(steps)) : 0;
      err = dx + dy;
      for (;;) {
        if (dashOn(pen)) stamp(pen, x, y);
        if (x === x1 && y === y1) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x += sx; }
        if (e2 <= dx) { err += dx; y += sy; }
        pen.phase = f32(pen.phase + stepl);
      }
      pen.phase = f32(pen.phase + f32(f32(len - lead) - f32(stepl * f32(steps))));
    }

    var marksOut = [];
    for (var f = 0; f < nf && !bad; f++) {
      var role = rb(), kind = rb(), nr = rv();
      var on = role < 16 && ((st.role_on >> role) & 1) && (kind === 0 || st.width[role & 15]);
      stats.features++;
      var q, np, x, y, i, o;
      if (kind === 0) {
        edges = [];
        for (q = 0; q < nr && !bad; q++) {
          np = rv(); x = 0; y = 0;
          var fx0 = 0, fy0 = 0, px = 0, py = 0;
          for (i = 0; i < np && !bad; i++) {
            x += rz(); y += rz();
            if (!on) continue;
            o = tx(x, y);
            if (i === 0) { fx0 = o[0]; fy0 = o[1]; }
            else addEdge(px, py, o[0], o[1]);
            px = o[0]; py = o[1];
          }
          if (on && np > 2) addEdge(px, py, fx0, fy0);
        }
        if (!on) continue;
        fill(role);
        stats.fills++; stats.edges += edges.length;
      } else {
        var d = st.dash[role & 15] < DASH.length ? st.dash[role & 15] : 0;
        var pen = { v: role, w: st.width[role & 15], pat: d ? DASH[d] : null, plen: 0, phase: 0 };
        if (d) pen.plen = DASH[d][0] + DASH[d][1] + DASH[d][2] + DASH[d][3];
        for (q = 0; q < nr && !bad; q++) {
          np = rv(); x = 0; y = 0;
          var lx = 0, ly = 0;
          pen.phase = 0;
          for (i = 0; i < np && !bad; i++) {
            x += rz(); y += rz();
            if (!on) continue;
            o = tx(x, y);
            if (i) { segment(pen, lx, ly, o[0], o[1]); stats.segments++; }
            lx = o[0]; ly = o[1];
          }
        }
        if (on) stats.lines++;
      }
    }
    var HALFW = [1, 2, 2, 2, 1];
    for (var m = 0; m < nm && !bad; m++) {
      var mk = rb(), rank = rb(), mx = rz(), my = rz(), n = rb();
      if (end - p < n) { bad = true; break; }
      var name = utf8 ? utf8.decode(buf.subarray(p, p + n)) : '';
      p += n;
      var mo = tx(mx, my);
      marksOut.push({ kind: mk, rank: rank, x: mo[0], y: mo[1], name: name });
      if (mk > 7 || !((st.mark_on >> mk) & 1)) continue;
      var mrole = mk === MARK_AIRPORT ? AIRPORT : CITY;
      if (!((st.role_on >> mrole) & 1)) continue;
      var cxp = rnd32(mo[0]), cyp = rnd32(mo[1]), dy2;
      if (mrole === AIRPORT) {
        for (dy2 = -2; dy2 <= 2; dy2++) if (cyp + dy2 >= 0 && cyp + dy2 < W) span(ras, cyp + dy2, cxp - 2, cxp + 3, mrole);
      } else {
        for (dy2 = -2; dy2 <= 2; dy2++)
          if (cyp + dy2 >= 0 && cyp + dy2 < W) span(ras, cyp + dy2, cxp - HALFW[dy2 + 2], cxp + HALFW[dy2 + 2] + 1, mrole);
      }
      stats.marks++;
    }
    return { err: bad ? -2 : 0, raster: ras, stats: stats, marks: marksOut };
  }

  /* The level a radius draws from, as orb_map.c's pick_level: the zoom whose own scale is
   * nearest the view's, so it draws at 0.71x-1.41x of the level. */
  function pickLevel(latE6, radiusM) {
    var c = Math.cos(latE6 / 1e6 * Math.PI / 180.0), want = radiusM / (W / 2);
    var v = Math.log2(156543.03392 * c / want);
    var z = v < 0 ? -Math.round(-v) : Math.round(v);              /* lround: half away from zero */
    if (Math.abs(v - Math.trunc(v)) === 0.5) z = v < 0 ? Math.floor(v) : Math.ceil(v);
    z = Math.max(6, Math.min(11, z));
    return { z: z, scale: f32(156543.03392 * c / Math.pow(2, z) / want) };
  }

  /* RGBA pixels of a raster through 16 ARGB words (0xAARRGGBB), for a preview canvas. */
  function paint(ras, argb16, rgba) {
    var out = rgba || new Uint8ClampedArray(W * W * 4);
    for (var y = 0; y < W; y++)
      for (var x = 0; x < W; x++) {
        var c = argb16[roleAt(ras, x, y)] >>> 0, o = (y * W + x) * 4;
        out[o] = (c >> 16) & 255; out[o + 1] = (c >> 8) & 255; out[o + 2] = c & 255; out[o + 3] = c >>> 24;
      }
    return out;
  }

  var api = {
    FORMAT: FORMAT, UNIT: UNIT, HALF: HALF, ZOOMS: ZOOMS, ROLES: ROLES, W: W, RASTER_B: RASTER_B,
    readTile: readTile, classify: classify, world: world, tilesFor: tilesFor, e4: e4, mapName: mapName,
    clipPoly: clipPoly, clipLine: clipLine, simplify: simplify, quantise: quantise,
    encode: encode, info: info, build: build, crc32: crc32,
    render: render, roleAt: roleAt, nib: nib, pickLevel: pickLevel, paint: paint
  };
  root.GorbMaps = api;
})(typeof window !== 'undefined' ? window : (typeof globalThis !== 'undefined' ? globalThis : this));
if (typeof module !== 'undefined' && module.exports) module.exports = (typeof window !== 'undefined' ? window : globalThis).GorbMaps;
