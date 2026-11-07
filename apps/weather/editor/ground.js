/* ground.js — the weather ground's dial finishes (weather-plan §2.9).
 *
 * The clock's dial colours, surfaces and wear, lifted from apps/clock/editor
 * (watch-engine.js surface/carbon/waveGrooves/surfaceShade/weather, dial-renderer.js
 * enamelFill/rng) so the weather's ground offers what the clock's dial does (owner,
 * 2026-11-06). Apps share no code (each is atomic), so this is the weather's own copy;
 * the drawing is the clock's, light from the upper left. Pure canvas: no orb, no bake.
 *   WeatherGround.paint(ctx, g)  g = {finish, a, b, tint, dark, tex, weaveAng, weaveSc, seed}
 *   WeatherGround.wear(ctx, g, {age, stain, dust})
 *   WeatherGround.COLOURS / GROUPS / FINISHES / pair(key) */
(function (root) {
  'use strict';
  var S = 466, TAU = Math.PI * 2, LIGHT = { outer: 225 * Math.PI / 180, outerK: 1 };
  var FINISHES = { plain: 'Gradient', enamel: 'Enamel', guilloche: 'Sunburst', azure: 'Azurage', matte: 'Grained', metal: 'Silvered metal',
    carbon: 'Carbon · woven', forged: 'Carbon · forged', wave: 'Waves' };
  var COLOURS = {
    cream: ['#efe5cb', '#c9b388'], porcelain: ['#f8f4ea', '#e0d9c8'], ivory: ['#f3ead6', '#d8c9a6'], white: ['#fbfaf7', '#e2e0da'], salmon: ['#efc2a4', '#c98b6a'], sky: ['#c9dcea', '#8eaac2'],
    slate: ['#5a636d', '#2b3036'], navy: ['#2a3f66', '#0f1a2e'], forest: ['#2f5243', '#122219'], burgundy: ['#6b2430', '#2c0b11'], charcoal: ['#2c2823', '#0f0d0a'], black: ['#26241f', '#0c0b09'],
    magenta: ['#ff2a9d', '#a10c5c'], bubblegum: ['#ff9fd2', '#e0559c'], red: ['#ff3b3b', '#a3121a'], tangerine: ['#ff8a1f', '#c24a00'], sunflower: ['#ffd12a', '#d08f00'], acid: ['#e4ff3a', '#93b300'],
    lime: ['#7dff4a', '#2f9e12'], mint: ['#7ff5c8', '#22b383'], turquoise: ['#1fe0c4', '#06807a'], electric: ['#2f7bff', '#0a2fb0'], violet: ['#9a4dff', '#4214a8'], ultraviolet: ['#4b17d6', '#14054a'],
  };
  var GROUPS = { classic: ['cream', 'porcelain', 'ivory', 'white', 'salmon', 'sky', 'slate', 'navy', 'forest', 'burgundy', 'charcoal', 'black'],
    vivid: ['magenta', 'bubblegum', 'red', 'tangerine', 'sunflower', 'acid', 'lime', 'mint', 'turquoise', 'electric', 'violet', 'ultraviolet'] };
  var NO_TINT = ['cream', 'porcelain', 'ivory', 'white', 'charcoal', 'black'];

  function rng(seed) {
    return function () {
      seed |= 0; seed = (seed + 0x6d2b79f5) | 0;
      var t = Math.imul(seed ^ (seed >>> 15), 1 | seed);
      t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
      return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
  }
  var P = function (x, y, r, a) { return [x + Math.cos(a) * r, y + Math.sin(a) * r]; };
  function lum(hex) { var n = parseInt(hex.slice(1), 16); return (0.299 * (n >> 16) + 0.587 * ((n >> 8) & 255) + 0.114 * (n & 255)) / 255; }
  // a named colour's pair and whether it tints a finish (the clock's colorEn)
  function pair(key) {
    var c = COLOURS[key] || COLOURS.cream;
    return { a: c[0], b: c[1], dark: lum(c[0]) < 0.5, tint: NO_TINT.indexOf(key) >= 0 ? null : c[0] };
  }

  function enamel(ctx, x, y, r, g) {
    var gr = ctx.createRadialGradient(x - r * 0.3, y - r * 0.35, r * 0.1, x, y, r * 1.05);
    gr.addColorStop(0, g.a); gr.addColorStop(1, g.b);
    ctx.fillStyle = gr; ctx.fill();
  }
  function surfaceShade(ctx, x, y, r) {
    var g = ctx.createRadialGradient(x, y, r * 0.55, x, y, r);
    g.addColorStop(0, 'rgba(255,250,235,.05)'); g.addColorStop(0.85, 'rgba(60,35,10,0)'); g.addColorStop(1, 'rgba(40,22,6,.45)');
    ctx.fillStyle = g; ctx.fillRect(x - r, y - r, 2 * r, 2 * r);
    var lx = Math.cos(LIGHT.outer) * r, ly = Math.sin(LIGHT.outer) * r, k = Math.min(1.6, LIGHT.outerK);
    g = ctx.createLinearGradient(x + lx, y + ly, x - lx, y - ly);
    g.addColorStop(0, 'rgba(255,248,225,' + 0.2 * k + ')'); g.addColorStop(0.5, 'rgba(0,0,0,0)'); g.addColorStop(1, 'rgba(40,22,6,.3)');
    ctx.fillStyle = g; ctx.fillRect(x - r, y - r, 2 * r, 2 * r);
  }
  // carbon fibre: woven twill (bundle brightness follows the light) or forged (random flakes)
  function carbon(ctx, x, y, r, kind, seed, T, tint, ang, sc) {
    ctx.save(); ctx.fillStyle = '#0b0c0e'; ctx.fillRect(x - r - 2, y - r - 2, 2 * r + 4, 2 * r + 4);
    var La = LIGHT.outer, tk = Math.min(1.8, T), wa = (ang || 0) * Math.PI / 180, wsc = Math.max(0.3, sc == null ? 1 : sc), g;
    if (kind === 'carbon') {
      var c = 8 * wsc, lh = 0.3 + 0.7 * Math.abs(Math.cos(La - wa)), lv = 0.3 + 0.7 * Math.abs(Math.sin(La - wa));
      ctx.save(); ctx.translate(x, y); ctx.rotate(wa); ctx.translate(-x, -y);
      for (var gy = Math.floor((y - r * 1.45) / c) * c; gy < y + r * 1.45; gy += c) for (var gx = Math.floor((x - r * 1.45) / c) * c; gx < x + r * 1.45; gx += c) {
        if (Math.hypot(gx + c / 2 - x, gy + c / 2 - y) > r + c * 1.5) continue;
        var i = Math.round(gx / c), j = Math.round(gy / c), hz = (((i + j) % 4) + 4) % 4 < 2, b = (hz ? lh : lv) * tk;
        g = hz ? ctx.createLinearGradient(0, gy, 0, gy + c) : ctx.createLinearGradient(gx, 0, gx + c, 0);
        var hi = Math.round(22 + 58 * b);
        g.addColorStop(0, 'rgb(7,7,9)'); g.addColorStop(0.5, 'rgb(' + hi + ',' + hi + ',' + (hi + 4) + ')'); g.addColorStop(1, 'rgb(7,7,9)');
        ctx.fillStyle = g; ctx.fillRect(gx, gy, c + 0.25, c + 0.25);
      }
      ctx.restore();
    } else {
      var R = rng(seed * 7 + 11);
      for (var n = 0; n < (2400 * (r / 221) * (r / 221) + 200) / Math.max(0.5, wsc); n++) {
        var a = R() * TAU, d = Math.sqrt(R()) * (r + 6), px = x + Math.cos(a) * d, py = y + Math.sin(a) * d, rot = R() * Math.PI, w = (4 + R() * 16) * wsc, h = (2 + R() * 7) * wsc;
        var lm = Math.pow(Math.abs(Math.cos(rot - La)), 3), v = Math.round(12 + 20 * R() + 72 * lm * tk);
        ctx.save(); ctx.translate(px, py); ctx.rotate(rot); ctx.beginPath();
        for (var q = 0; q < 5; q++) { var aa = (q / 5) * TAU, rr = 0.6 + R() * 0.5, qx = (Math.cos(aa) * w) / 2 * rr, qy = (Math.sin(aa) * h) / 2 * rr; if (q) ctx.lineTo(qx, qy); else ctx.moveTo(qx, qy); }
        ctx.closePath(); ctx.fillStyle = 'rgba(' + v + ',' + v + ',' + (v + 3) + ',.88)'; ctx.fill(); ctx.restore();
      }
    }
    if (tint) {
      ctx.globalCompositeOperation = 'color'; ctx.globalAlpha = 0.9; ctx.fillStyle = tint; ctx.fillRect(x - r, y - r, 2 * r, 2 * r);
      ctx.globalCompositeOperation = 'soft-light'; ctx.globalAlpha = 0.55; ctx.fillRect(x - r, y - r, 2 * r, 2 * r);
      ctx.globalCompositeOperation = 'source-over'; ctx.globalAlpha = 1;
    }
    g = ctx.createLinearGradient(x - r, y - r, x + r, y + r);
    g.addColorStop(0, 'rgba(255,255,255,.09)'); g.addColorStop(0.5, 'rgba(0,0,0,0)'); g.addColorStop(1, 'rgba(0,0,0,.28)');
    ctx.fillStyle = g; ctx.fillRect(x - r, y - r, 2 * r, 2 * r);
    ctx.restore();
  }
  // the wave finish's grooves: rows of one swell, their phase drifting row by row
  function waveGrooves(ctx, x, y, r, pen, troughA, lipA) {
    var gap = Math.max(6, r * 0.062), amp = r * 0.05, wl = r * 0.95;
    var row = function (y0, dy) {
      ctx.beginPath();
      for (var px = x - r - 4; px <= x + r + 4; px += 3) { var py = y0 + dy + amp * Math.sin(((px - x) / wl) * TAU + ((y0 - y) / r) * 1.1); if (px === x - r - 4) ctx.moveTo(px, py); else ctx.lineTo(px, py); }
      ctx.stroke();
    };
    ctx.lineCap = 'round';
    for (var y0 = y - r - gap; y0 < y + r + gap; y0 += gap) {
      if (troughA > 0) { pen(Math.max(1.2, gap * 0.3), troughA, 0); row(y0, 0); }
      if (lipA > 0) { pen(Math.max(0.7, gap * 0.1), lipA, 1); row(y0, gap * 0.26); }
    }
  }
  // the finish over the whole glass: base colour, its texture, then the light
  function paint(ctx, g) {
    var x = S / 2, y = S / 2, r = S / 2, T = g.tex == null ? 1 : +g.tex, st = g.finish || 'plain', seed = g.seed || 7;
    ctx.save();
    ctx.beginPath(); ctx.arc(x, y, r, 0, TAU);
    if (st === 'carbon' || st === 'forged') { ctx.save(); ctx.clip(); carbon(ctx, x, y, r, st, seed, T, g.tint, g.weaveAng, g.weaveSc); ctx.restore(); }
    else if (st === 'metal') {
      var g0 = ctx.createLinearGradient(x - r, y - r, x + r, y + r);
      g0.addColorStop(0, '#ece9e2'); g0.addColorStop(0.5, '#aeaaa2'); g0.addColorStop(1, '#5f5c56');
      ctx.fillStyle = g0; ctx.fill();
      if (g.tint) { ctx.save(); ctx.clip(); ctx.globalCompositeOperation = 'multiply'; ctx.globalAlpha = 0.75; ctx.fillStyle = g.tint; ctx.fillRect(x - r, y - r, 2 * r, 2 * r); ctx.restore(); }
    } else enamel(ctx, x, y, r, g);
    ctx.save(); ctx.beginPath(); ctx.arc(x, y, r, 0, TAU); ctx.clip();
    var lite = (g.dark ? 0.13 : 0.19) * T, dk = (g.dark ? 0.26 : 0.16) * T, i;
    if (st === 'guilloche') {
      var n = Math.round(r * 2.6); ctx.lineWidth = 0.6;
      for (i = 0; i < n; i++) { var q = P(x, y, r, (i / n) * TAU); ctx.strokeStyle = i % 2 ? 'rgba(255,250,235,' + lite + ')' : 'rgba(0,0,0,' + dk + ')'; ctx.beginPath(); ctx.moveTo(x, y); ctx.lineTo(q[0], q[1]); ctx.stroke(); }
      var cg = ctx.createConicGradient(LIGHT.outer, x, y);
      [[0, 0.24], [0.25, 0], [0.5, 0.24], [0.75, 0], [1, 0.24]].forEach(function (s) { cg.addColorStop(s[0], 'rgba(255,250,235,' + Math.min(0.8, s[1] * T) + ')'); });
      ctx.fillStyle = cg; ctx.fillRect(x - r, y - r, 2 * r, 2 * r);
      var cd = ctx.createConicGradient(LIGHT.outer, x, y);
      [[0, 0], [0.25, 0.2], [0.5, 0], [0.75, 0.2], [1, 0]].forEach(function (s) { cd.addColorStop(s[0], 'rgba(0,0,0,' + Math.min(0.7, s[1] * T) + ')'); });
      ctx.fillStyle = cd; ctx.fillRect(x - r, y - r, 2 * r, 2 * r);
    } else if (st === 'azure' || st === 'metal') {
      ctx.lineWidth = 0.7;
      for (var qq = 2, k = 0; qq < r; qq += st === 'metal' ? 1.2 : 1.5, k++) { ctx.strokeStyle = k % 2 ? 'rgba(255,250,235,' + lite + ')' : 'rgba(0,0,0,' + dk * 0.8 + ')'; ctx.beginPath(); ctx.arc(x, y, qq, 0, TAU); ctx.stroke(); }
      var cz = ctx.createConicGradient(LIGHT.outer + Math.PI / 2, x, y);
      [[0, 0.2], [0.07, 0], [0.43, 0], [0.5, 0.2], [0.57, 0], [0.93, 0], [1, 0.2]].forEach(function (s) { cz.addColorStop(s[0], 'rgba(255,250,235,' + Math.min(0.7, s[1] * T) + ')'); });
      ctx.fillStyle = cz; ctx.fillRect(x - r, y - r, 2 * r, 2 * r);
    } else if (st === 'matte') {
      var R = rng(seed * 7 + 3), m = Math.round(r * r * 0.5);
      for (i = 0; i < m; i++) { var a = R() * TAU, d = Math.sqrt(R()) * r; ctx.fillStyle = R() < 0.5 ? 'rgba(0,0,0,' + R() * 0.12 * T + ')' : 'rgba(255,255,255,' + R() * 0.1 * T + ')'; ctx.fillRect(x + Math.cos(a) * d, y + Math.sin(a) * d, 1, 1); }
    } else if (st === 'wave') {
      var bs = ctx.createLinearGradient(x - r, y, x + r, y);
      [[0, 0], [0.22, 0.12], [0.4, 0], [0.62, 0.1], [0.8, 0], [1, 0.06]].forEach(function (s) { bs.addColorStop(s[0], 'rgba(255,250,235,' + Math.min(0.5, s[1] * T) + ')'); });
      ctx.fillStyle = bs; ctx.fillRect(x - r, y - r, 2 * r, 2 * r);
      waveGrooves(ctx, x, y, r, function (lw, al, dy) { ctx.lineWidth = lw; ctx.strokeStyle = (dy ? 'rgba(255,250,235,' : 'rgba(0,0,0,') + Math.min(0.85, al * T) + ')'; }, g.dark ? 0.7 : 0.45, lite * 1.8);
    }
    if (st !== 'plain') surfaceShade(ctx, x, y, r);
    ctx.restore();
    ctx.restore();
  }
  // ageing (patina, yellowing, crazing), stains (blotches, pits, chips) and dust (scratches,
  // specks), each 0..1.5, over the whole glass
  function wear(ctx, g, W) {
    var x = S / 2, y = S / 2, rIn = 0, rOut = S / 2, age = +W.age || 0, st = +W.stain || 0, du = +W.dust || 0;
    if (age + st + du <= 0.001) return;
    var R = rng(g.seed || 7), dark = !!g.dark, i, j, px, py, p;
    var GR = dark ? '120,104,78' : '48,28,8', BL = dark ? '140,120,88' : '70,44,16', AR = dark ? '150,132,100' : '50,30,10';
    ctx.save(); ctx.beginPath(); ctx.arc(x, y, rOut, 0, TAU); ctx.clip();
    var pt = function () { var a = R() * TAU, d = rIn + Math.sqrt(R()) * (rOut - rIn); return [x + Math.cos(a) * d, y + Math.sin(a) * d, a, d]; };
    if (age > 0) {
      var ga = dark ? 0.35 : 1, gg = ctx.createRadialGradient(x, y, rIn, x, y, rOut);
      gg.addColorStop(0, 'rgba(' + GR + ',' + 0.55 * age * ga + ')'); gg.addColorStop(0.16, 'rgba(' + GR + ',0)');
      gg.addColorStop(0.8, 'rgba(' + GR + ',0)'); gg.addColorStop(1, 'rgba(' + GR + ',' + 0.6 * age * ga + ')');
      ctx.fillStyle = gg; ctx.fillRect(0, 0, S, S);
      if (!dark) { ctx.globalCompositeOperation = 'multiply'; ctx.fillStyle = 'rgba(205,165,95,' + Math.min(0.35, 0.09 * age) + ')'; ctx.fillRect(0, 0, S, S); ctx.globalCompositeOperation = 'source-over'; }
      ctx.lineCap = 'round';
      for (i = 0; i < 70 * age; i++) {
        p = pt(); px = p[0]; py = p[1]; var dir = R() * TAU;
        ctx.beginPath(); ctx.moveTo(px, py);
        var n = 3 + ((R() * 9) | 0);
        for (j = 0; j < n; j++) { dir += (R() - 0.5) * 1.6; px += Math.cos(dir) * (3 + R() * 7); py += Math.sin(dir) * (3 + R() * 7); ctx.lineTo(px, py); }
        ctx.strokeStyle = dark ? 'rgba(190,172,140,' + (0.04 + R() * 0.07) * age + ')' : 'rgba(60,38,14,' + (0.12 + R() * 0.22) * age + ')'; ctx.lineWidth = 0.45; ctx.stroke();
      }
      for (i = 0; i < 40 * age; i++) {
        p = pt(); var span = 0.05 + R() * 0.35;
        ctx.strokeStyle = 'rgba(' + AR + ',' + (0.06 + R() * 0.12) * age * (dark ? 0.6 : 1) + ')'; ctx.lineWidth = 0.4;
        ctx.beginPath(); ctx.arc(x, y, p[3], p[2], p[2] + span); ctx.stroke();
      }
    }
    if (st > 0) {
      for (i = 0; i < 30 * st; i++) {
        p = pt(); var sr = 10 + R() * 40;
        ctx.save(); ctx.translate(p[0], p[1]); ctx.rotate(R() * TAU); ctx.scale(1, 0.35 + R() * 0.65);
        var qg = ctx.createRadialGradient(0, 0, 0, 0, 0, sr);
        qg.addColorStop(0, 'rgba(' + BL + ',' + (0.06 + R() * 0.12) * st * (dark ? 0.6 : 1) + ')'); qg.addColorStop(1, 'rgba(' + BL + ',0)');
        ctx.fillStyle = qg; ctx.fillRect(-sr, -sr, sr * 2, sr * 2); ctx.restore();
      }
      for (i = 0; i < 900 * st; i++) {
        p = pt();
        ctx.fillStyle = (dark ? 'rgba(32,29,25,' : 'rgba(232,220,192,') + (0.3 + R() * 0.5) * Math.min(1, st) + ')';
        ctx.fillRect(p[0], p[1], 0.6 + R() * 1.4, 0.6 + R() * 1.2);
      }
      for (i = 0; i < 400 * st; i++) {
        p = pt();
        ctx.fillStyle = dark ? 'rgba(170,150,115,' + (0.05 + R() * 0.12) + ')' : 'rgba(60,40,18,' + (0.08 + R() * 0.2) + ')';
        ctx.fillRect(p[0], p[1], 0.5 + R() * 1.2, 0.5 + R());
      }
      for (i = 0; i < 14 * st; i++) {
        p = pt(); var cr = 0.6 + Math.pow(R(), 2) * 2.6;
        ctx.beginPath();
        for (j = 0; j < 7; j++) { var aj = (j / 7) * TAU, rj = cr * (0.6 + R() * 0.7); if (j) ctx.lineTo(p[0] + Math.cos(aj) * rj, p[1] + Math.sin(aj) * rj); else ctx.moveTo(p[0] + Math.cos(aj) * rj, p[1] + Math.sin(aj) * rj); }
        ctx.closePath(); ctx.fillStyle = dark ? 'rgba(128,116,96,' + (0.5 + R() * 0.35) + ')' : 'rgba(28,16,5,' + (0.45 + R() * 0.35) + ')'; ctx.fill();
        ctx.strokeStyle = dark ? 'rgba(0,0,0,.5)' : 'rgba(255,250,235,.35)'; ctx.lineWidth = 0.5; ctx.stroke();
      }
    }
    if (du > 0) {
      for (i = 0; i < 140 * du; i++) {
        p = pt(); px = p[0]; py = p[1];
        var dr = R() * TAU, len = 3 + Math.pow(R(), 2) * 38, cvv = (R() - 0.5) * 0.6;
        var ex = px + Math.cos(dr) * len, ey = py + Math.sin(dr) * len, mx = (px + ex) / 2 + Math.cos(dr + 1.57) * len * cvv, my = (py + ey) / 2 + Math.sin(dr + 1.57) * len * cvv;
        ctx.lineWidth = 0.35 + R() * 0.45;
        ctx.strokeStyle = dark ? 'rgba(255,240,210,' + (0.06 + R() * 0.12) * du + ')' : 'rgba(40,24,8,' + (0.1 + R() * 0.2) * du + ')';
        ctx.beginPath(); ctx.moveTo(px, py); ctx.quadraticCurveTo(mx, my, ex, ey); ctx.stroke();
        ctx.strokeStyle = dark ? 'rgba(0,0,0,' + (0.2 + R() * 0.3) * du + ')' : 'rgba(255,252,240,' + (0.1 + R() * 0.2) * du + ')';
        ctx.beginPath(); ctx.moveTo(px + 0.5, py + 0.5); ctx.quadraticCurveTo(mx + 0.5, my + 0.5, ex + 0.5, ey + 0.5); ctx.stroke();
      }
      for (i = 0; i < 1400 * du; i++) {
        p = pt();
        ctx.fillStyle = R() < 0.5 ? (dark ? 'rgba(0,0,0,' + R() * 0.35 + ')' : 'rgba(50,30,10,' + R() * 0.25 + ')') : 'rgba(255,250,235,' + R() * (dark ? 0.1 : 0.18) + ')';
        ctx.fillRect(p[0], p[1], 0.6 + R() * 0.8, 0.6 + R() * 0.8);
      }
    }
    ctx.restore();
  }
  var api = { FINISHES: FINISHES, COLOURS: COLOURS, GROUPS: GROUPS, pair: pair, lum: lum, paint: paint, wear: wear };
  root.WeatherGround = api;
})(typeof window !== 'undefined' ? window : globalThis);
