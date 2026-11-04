/* flights-bake.js — the tracker's theme, baked in the browser (flight-tracker-design §12.2).
 *
 * The browser's port of apps/flights/art/bake_theme.py: a design (schema 1, below) drawn
 * on canvases from flights-art.js and assembled by the OS's stage bake (GorbBake.stage3)
 * into the rows apps/flights/main.c reads:
 *   - the plate: the background with the range rings and the home dot drawn in;
 *   - slot 0: the radar arm, a sweeping seconds row at SWEEP_FPS (the stage's rate);
 *   - slots 1..15: the plane pool, one bank of 49 frames (7 shapes x 7 bands, shape-major);
 *   - slots 16, 17: the selection and emergency rings;
 *   - fonts 0..3 (body, accent, quiet, warning) and the labels at slots 20..23;
 *   - the picker's preview.
 * Needs a document (Image, canvas, document.fonts), flights-art.js and GorbBake. */
(function (root) {
  'use strict';
  var A = function () { return root.FlightsArt; };

  var SHAPES = ['light', 'turboprop', 'bizjet', 'airliner', 'heavy', 'helicopter', 'generic'];
  var SHAPE_TITLES = ['Light', 'Turboprop', 'Business jet', 'Airliner', 'Heavy', 'Helicopter', 'Other'];
  var PLANE_ROWS = 15, SWEEP_FPS = 15, SEL_SLOT = 16, EM_SLOT = 17, SEL_R = 28, EM_R = 32;
  var LABEL_SLOT0 = 20, NONE = 0xF;
  var CHARSET = (function () {
    var c = [];
    for (var i = 0x20; i < 0x7F; i++) c.push(String.fromCharCode(i));
    return c.concat(['↑', '↓', '→', '←', '°']);
  })();
  // the fonts by slot: the tone each draws and its size (§11.5: 14 px is the smallest)
  var FONTS = [['body', 16], ['accent', 16], ['quiet', 14], ['warning', 14]];
  // the labels by slot (main.c LAB_SLOT0): fonts by tone (normal, accent, warning, quiet)
  var LABELS = [
    { slot: 20, name: 'header', box: [113, 10, 240, 26], plaque: [163, 12, 140, 24], pad: 2, tones: [0, 1, NONE, NONE] },
    { slot: 21, name: 'footer', box: [113, 428, 240, 24], plaque: [163, 430, 140, 24], pad: 2, tones: [2, NONE, 3, NONE] },
    { slot: 22, name: 'card', box: [103, 326, 260, 96], pad: 8, tones: [0, NONE, NONE, NONE], panel: true },
    { slot: 23, name: 'range', box: [333, 362, 80, 22], plaque: [340, 364, 62, 20], pad: 2, tones: [2, NONE, NONE, NONE] },
  ];
  // the runtime's faces (fonts@1) the editor offers
  var FACES = ['IBM Plex Sans', 'IBM Plex Mono', 'Space Grotesk', 'Space Mono', 'Barlow Condensed', 'Jost',
    'Oswald', 'Michroma', 'Orbitron', 'Playfair Display', 'Cormorant Garamond', 'DM Serif Display', 'VT323'];

  /* ---- the design (schema 1) and the presets: the five hand-baked families ---- */
  var SCHEMA = 1;
  var WORDS = {
    scope: { face: 'IBM Plex Sans', body: '#E6F0EA', accent: '#39FF14', accent2: null, quiet: '#8A9A90', warning: '#FFB23C',
      card: { fill: '#101820', opacity: 0.82, edge: '#2A6A44' }, plaques: false },
    chart: { face: 'IBM Plex Sans', body: '#14202C', accent: '#1E6B2E', accent2: null, quiet: '#5B5240', warning: '#B85C00',
      card: { fill: '#F4EEDC', opacity: 0.94, edge: '#2F5C8C' }, plaques: false },
    brass: { face: 'Cormorant Garamond', body: '#F2E6C8', accent: '#F8E7A8', accent2: '#B8892E', quiet: '#A8946A', warning: '#FF9A3C',
      card: { fill: '#161815', opacity: 0.88, edge: '#C9A24E' }, plaques: true },
    blueprint: { face: 'IBM Plex Mono', body: '#FFFFFF', accent: '#9FE3FF', accent2: null, quiet: '#B8CCE6', warning: '#FFD27A',
      card: { fill: '#1C4A84', opacity: 0.88, edge: '#DCE9F8' }, plaques: false },
  };
  function preset(fam) {
    var a = A(), f = a.families[fam], bg = f.pair[0], look = a.backgrounds[bg].look;
    var w = JSON.parse(JSON.stringify(WORDS[bg]));
    return {
      name: 'Flights ' + f.title,
      scope: { bg: bg, colour: look.blipbg, image: null, rings: 2, ringColour: look.ring, ringOpacity: look.ring_opa, home: look.home },
      arm: { kind: f.pair[1], trail: 30, colour: '#3DFF8E', lead: '#D8FFE8' },
      planes: { family: fam, form: 'types', size: a.plane_px, shapes: SHAPES.slice(), colour: true,
        bands: a.bands.map(function (b) { return b[1]; }), ground: 0.45, sel: look.sel, em: '#FF2020' },
      words: w,
    };
  }
  var PRESETS = ['modern', 'silver', 'brass', 'blueprint', 'paper'];

  /* ---- SVG for each part ---- */
  function svgDoc(vb, body, defs) {
    return '<svg xmlns="http://www.w3.org/2000/svg" viewBox="' + vb + '">' + (defs ? '<defs>' + defs + '</defs>' : '') + body + '</svg>';
  }
  // one frame: slot `si`'s drawn shape in band `bi`
  function planeSvg(d, si, bi) {
    var a = A(), f = a.families[d.planes.family], key = d.planes.form === 'single' ? 'single' : (d.planes.shapes[si] || SHAPES[si]);
    var body;
    if (d.planes.colour) body = f.shapes[key].split('{{B}}').join(d.planes.bands[bi]);
    else if (f.neutral) body = f.shapes[key].split('{{B}}').join(f.neutral);
    else body = f.plain[key];
    return svgDoc('0 0 100 100', body, a.gradients);
  }
  function pt(r, deg) { var t = deg * Math.PI / 180; return [r * Math.sin(t), -r * Math.cos(t)]; }
  // make_art.arm_wedge, ported: a lead line and a fading trail, pivot at (0, 0)
  function wedge(trail, colour, lead, L) {
    L = L || A().ring_r;
    var n = Math.max(8, Math.round(trail * 0.8)), s = '';
    for (var i = 0; i < n; i++) {
      var a0 = -trail * i / n, a1 = -trail * (i + 1) / n, p0 = pt(L, a0), p1 = pt(L, a1);
      var op = 0.42 * Math.pow(1 - i / n, 2);
      s += '<path d="M0 0L' + p0[0].toFixed(2) + ' ' + p0[1].toFixed(2) + 'A' + L + ' ' + L + ' 0 0 0 ' +
        p1[0].toFixed(2) + ' ' + p1[1].toFixed(2) + 'Z" fill="' + colour + '" fill-opacity="' + op.toFixed(3) + '"/>';
    }
    var x0 = Math.ceil(-L * Math.sin(trail * Math.PI / 180)) - 4, vb = [x0, -L - 4, -x0 + 6, L + 10];
    var body = '<defs><filter id="g" x="-50%" y="-5%" width="200%" height="110%"><feGaussianBlur stdDeviation="2.2"/></filter></defs>' + s +
      '<line x1="0" y1="0" x2="0" y2="' + (-L) + '" stroke="' + colour + '" stroke-width="6" filter="url(#g)"/>' +
      '<line x1="0" y1="0" x2="0" y2="' + (-L) + '" stroke="' + lead + '" stroke-width="2"/>';
    return { svg: svgDoc(vb.join(' '), body), pivot: [-vb[0], -vb[1]], size: [vb[2], vb[3]] };
  }
  // the arm: the wedge from its fields, another arm whole, or none (a clear 2 x 2 row, so the
  // stage still runs at the sweep's rate)
  function arm(d) {
    if (d.arm.kind === 'wedge') return wedge(Math.max(5, Math.min(90, +d.arm.trail || 30)), d.arm.colour, d.arm.lead);
    if (d.arm.kind === 'none') return { svg: svgDoc('0 0 2 2', ''), pivot: [1, 1], size: [2, 2] };
    var x = A().arms[d.arm.kind];
    return { svg: x.svg, pivot: x.pivot.slice(), size: x.size.slice() };
  }
  // the rings and home dot over the background
  function ringsSvg(d) {
    var a = A(), s = '', n = Math.max(0, Math.min(4, d.scope.rings | 0));
    for (var k = 1; k <= n; k++)
      s += '<circle cx="' + a.cx + '" cy="' + a.cx + '" r="' + (a.ring_r * k / n).toFixed(1) + '" fill="none" stroke="' +
        d.scope.ringColour + '" stroke-opacity="' + d.scope.ringOpacity + '" stroke-width="1.5"/>';
    s += '<circle cx="' + a.cx + '" cy="' + a.cx + '" r="3" fill="' + d.scope.home + '"/>';
    return svgDoc('0 0 ' + a.glass + ' ' + a.glass, s);
  }
  function markerSvg(r, colour) {
    return svgDoc('0 0 ' + 2 * r + ' ' + 2 * r, '<circle cx="' + r + '" cy="' + r + '" r="' + (r - 1) + '" fill="none" stroke="' + colour + '" stroke-width="2"/>');
  }
  function labelBox(d, lab) { return d.words.plaques && lab.plaque ? lab.plaque : lab.box; }
  function panelSvg(d, lab) {
    var b = labelBox(d, lab), w = b[2], h = b[3];
    if (!(lab.panel || (d.words.plaques && lab.plaque))) return svgDoc('0 0 ' + w + ' ' + h, '');
    var c = d.words.card;
    return svgDoc('0 0 ' + w + ' ' + h, '<rect x="0.75" y="0.75" width="' + (w - 1.5) + '" height="' + (h - 1.5) + '" rx="' +
      (lab.panel ? 10 : 6) + '" fill="' + c.fill + '" fill-opacity="' + c.opacity + '" stroke="' + c.edge + '" stroke-width="1.5"/>');
  }

  /* ---- rasterising ---- */
  function canvas(w, h) { var c = document.createElement('canvas'); c.width = w; c.height = h; return c; }
  function image(src) {
    return new Promise(function (ok, bad) {
      var im = new Image();
      im.onload = function () { ok(im); };
      im.onerror = function () { bad(new Error('an image did not draw')); };
      im.src = src;
    });
  }
  function svgImage(svg) { return image('data:image/svg+xml;charset=utf-8,' + encodeURIComponent(svg)); }
  function plane(c) {
    var x = c.getContext('2d').getImageData(0, 0, c.width, c.height);
    return { w: c.width, h: c.height, rgba: new Uint8Array(x.data.buffer) };
  }
  async function rast(svg, w, h) {
    var c = canvas(w, h);
    c.getContext('2d').drawImage(await svgImage(svg), 0, 0, w, h);
    return c;
  }
  // the plate: the background (a drawing, a colour, or an image cut to fill the glass), the rings over it
  async function plateCanvas(d) {
    var a = A(), G = a.glass, c = canvas(G, G), x = c.getContext('2d');
    if (d.scope.bg === 'colour') { x.fillStyle = d.scope.colour; x.fillRect(0, 0, G, G); }
    else if (d.scope.bg === 'image' && d.scope.image) {
      var im = await image(d.scope.image), k = Math.max(G / im.width, G / im.height);
      x.fillStyle = '#000'; x.fillRect(0, 0, G, G);
      x.drawImage(im, (G - im.width * k) / 2, (G - im.height * k) / 2, im.width * k, im.height * k);
    } else x.drawImage(await svgImage(a.backgrounds[d.scope.bg in a.backgrounds ? d.scope.bg : 'scope'].svg), 0, 0, G, G);
    x.drawImage(await svgImage(ringsSvg(d)), 0, 0, G, G);
    return c;
  }
  async function planeFrames(d) {
    var px = Math.max(30, Math.min(48, d.planes.size | 0)), out = [];
    for (var si = 0; si < 7; si++)
      for (var bi = 0; bi < 7; bi++) {
        var c = await rast(planeSvg(d, si, bi), px, px);
        if (!d.planes.colour && bi === 0) {       // colour off: ground traffic recedes
          var x = c.getContext('2d'), img = x.getImageData(0, 0, px, px), g = +d.planes.ground;
          for (var i = 3; i < img.data.length; i += 4) img.data[i] = Math.round(img.data[i] * g);
          x.putImageData(img, 0, 0);
        }
        out.push(c);
      }
    return out;
  }
  // one font: every CHARSET glyph in one cell, its ink from the cell's left edge on a shared
  // baseline (bake_theme.font's rule), in a colour or a top-to-bottom gradient
  async function font(face, px, colour, colour2) {
    var spec = px + 'px "' + face + '"';
    try { await document.fonts.load(spec, CHARSET.join('')); } catch (e) { /* the fallback face draws */ }
    var m = canvas(4, 4).getContext('2d');
    m.font = spec;
    var top = 0, bottom = 0, w = 1, boxes = CHARSET.map(function (ch) {
      var t = m.measureText(ch);
      return { l: Math.floor(-t.actualBoundingBoxLeft), r: Math.ceil(t.actualBoundingBoxRight),
        t: Math.ceil(t.actualBoundingBoxAscent), b: Math.ceil(t.actualBoundingBoxDescent),
        asc: t.fontBoundingBoxAscent, desc: t.fontBoundingBoxDescent };
    });
    boxes.forEach(function (b) {
      if (b.r > b.l) { top = Math.max(top, b.t); bottom = Math.max(bottom, b.b); w = Math.max(w, b.r - b.l); }
      top = Math.max(top, Math.ceil(b.asc || 0)); bottom = Math.max(bottom, Math.ceil(b.desc || 0));
    });
    var h = top + bottom, glyphs = boxes.map(function (b, i) {
      var c = canvas(w, h), x = c.getContext('2d');
      x.font = spec; x.textBaseline = 'alphabetic';
      if (colour2) { var g = x.createLinearGradient(0, 0, 0, h); g.addColorStop(0, colour); g.addColorStop(1, colour2); x.fillStyle = g; }
      else x.fillStyle = colour;
      if (b.r > b.l) x.fillText(CHARSET[i], -b.l, top);
      return c;
    });
    return { w: w, h: h, glyphs: glyphs, space: Math.max(1, Math.round(m.measureText(' ').width)), track: 1 };
  }
  // the picker's picture: the plate, the arm and a few contacts, at 232 px
  async function previewCanvas(plateC, armC, armPiv, frames, px) {
    var a = A(), c = canvas(a.glass, a.glass), x = c.getContext('2d');
    x.drawImage(plateC, 0, 0);
    x.drawImage(armC, a.cx - armPiv[0], a.cx - armPiv[1]);
    [[3, 6, 70, -90, 220], [4, 5, -110, -20, 95], [0, 2, 40, 60, 330], [2, 4, -60, 100, 15], [5, 3, 110, 40, 120]].forEach(function (p) {
      x.save(); x.translate(a.cx + p[2], a.cx + p[3]); x.rotate(p[4] * Math.PI / 180);
      x.drawImage(frames[p[0] * 7 + p[1]], -px / 2, -px / 2); x.restore();
    });
    var o = canvas(232, 232);
    o.getContext('2d').drawImage(c, 0, 0, 232, 232);
    return o;
  }
  var toneStr = function (t) { return t.reduce(function (s, f, k) { return s + (f << (4 * k)); }, 0); };

  /* ---- the bake ---- */
  // the design baked to .orbtheme bytes: {bytes, meter, est, rows}. `design` is the
  // design document's bytes to carry (optional).
  async function bake(d, o) {
    o = o || {};
    var a = A(), G = root.GorbBake, S = G.stage3({ dither: true }), AUX = G.classId('aux'), TEXT = G.classId('text');
    var plateC = await plateCanvas(d);
    S.row({ n: S.id(), class: G.classId('plate'), slot: 0, x: 0, y: 0, w: a.glass, h: a.glass, order: 0, alpha: 255, n_frames: 1 },
      plane(plateC), false, true);
    var ar = arm(d), armC = await rast(ar.svg, ar.size[0], ar.size[1]);
    S.row({ n: S.id(), class: AUX, slot: 0, x: a.cx - ar.pivot[0], y: a.cx - ar.pivot[1], w: ar.size[0], h: ar.size[1],
      pivot_x: ar.pivot[0], pivot_y: ar.pivot[1], order: 1, alpha: 255, n_frames: 1,
      src: G.SOURCES.indexOf('second'), span: 3600, vmax: 60, cadence: 1, cad_arg: SWEEP_FPS, end: 0 }, plane(armC), true, true);
    var px = Math.max(30, Math.min(48, d.planes.size | 0)), frames = await planeFrames(d);
    var rows = [];
    for (var i = 0; i < PLANE_ROWS; i++) rows.push({ slot: 1 + i, x: a.cx - (px >> 1), y: a.cx - (px >> 1), order: 2 });
    S.pool({ class: AUX, w: px, h: px, pivot_x: px >> 1, pivot_y: px >> 1, alpha: 255 }, frames.map(plane), rows, true, true);
    for (var mk of [[SEL_SLOT, SEL_R, d.planes.sel], [EM_SLOT, EM_R, d.planes.em]]) {
      var md = 2 * mk[1];
      S.row({ n: S.id(), class: AUX, slot: mk[0], x: a.cx - mk[1], y: a.cx - mk[1], w: md, h: md, pivot_x: mk[1], pivot_y: mk[1],
        order: 3, alpha: 255, n_frames: 1 }, plane(await rast(markerSvg(mk[1], mk[2]), md, md)), true, true);
    }
    var W = d.words;
    for (var fi = 0; fi < FONTS.length; fi++) {
      var tone = FONTS[fi][0], f = await font(W.face, FONTS[fi][1], W[tone], tone === 'accent' ? W.accent2 : null);
      S.rowFrames({ n: S.id(), class: TEXT, slot: fi, x: 0, y: 0, w: f.w, h: f.h, order: 0, n_frames: f.glyphs.length,
        pivot_x: Math.min(f.space, f.w), pivot_y: f.track }, f.glyphs.map(plane), true, false);
    }
    for (var lab of LABELS) {
      var b = labelBox(d, lab);
      S.row({ n: S.id(), class: AUX, slot: lab.slot, kind: 2, src: G.SOURCES.indexOf('text'), x: b[0], y: b[1], w: b[2], h: b[3],
        pivot_x: b[2] >> 1, pivot_y: b[3] >> 1, order: 4, alpha: 255, n_frames: 1, str: toneStr(lab.tones), radius: lab.pad,
        digit: 1, end: 1 }, plane(await rast(panelSvg(d, lab), b[2], b[3])), true, false);
    }
    S.preview({ plane: plane(await previewCanvas(plateC, armC, ar.pivot, frames, px)), x: 0, y: 0, order: 3 });
    var out = S.finish({ name: d.name || 'Flights', slug: 'flights', serial: o.serial || 1, abi: G.GEOM_ABI6, design: o.design || null });
    return { bytes: out.bytes, meter: out.meter, est: out.est, rows: S.elements.length };
  }

  root.FlightsBake = {
    SCHEMA: SCHEMA, SHAPES: SHAPES, SHAPE_TITLES: SHAPE_TITLES, LABELS: LABELS, FACES: FACES, PRESETS: PRESETS,
    PLANE_ROWS: PLANE_ROWS, SWEEP_FPS: SWEEP_FPS, preset: preset, bake: bake,
    // the preview draws from the same parts
    planeSvg: planeSvg, arm: arm, ringsSvg: ringsSvg, markerSvg: markerSvg, panelSvg: panelSvg, labelBox: labelBox,
    plateCanvas: plateCanvas, rast: rast, font: font, svgImage: svgImage, image: image,
  };
})(typeof window !== 'undefined' ? window : globalThis);
