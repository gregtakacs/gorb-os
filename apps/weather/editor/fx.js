/* fx.js — the weather's particles for the editor's preview: apps/weather/fx.c, line for line.
 *
 * The orb draws the particles from fx.c; the editor draws them from this, so a theme's rain,
 * snow, mist, wisps and stars look in the preview as they will on the glass.
 * tools/host_weather_fx_test.py runs both from one seed and holds them to the same places.
 * Kinds are wx.h's WX_* (the bake's KINDS order); frames the bake's PART_FRAMES order.
 * Math.fround keeps the arithmetic in fx.c's single precision. */
(function (root) {
  'use strict';
  var f = Math.fround;
  var FX_MAX = 16, FX_SIDE = 466, FX_R = f(233);
  var WX = { CLEAR: 0, CLEAR_N: 1, PARTLY: 2, PARTLY_N: 3, CLOUDY: 4, FOG: 5, DRIZZLE: 6, RAIN: 7, SNOW: 8, THUNDER: 9 };
  var F = { DROP: 0, DROP_SMALL: 1, FLAKE: 2, FLAKE_SMALL: 3, MIST: 4, WISP: 5, STAR: 6 };

  function create(seed) {
    var p = [];
    for (var i = 0; i < FX_MAX; i++) p.push({ x: 0, y: 0, vx: 0, vy: 0, ph: 0, rate: 0, f: 0, opa: 0 });
    return { kind: -1, n: 0, seed: seed >>> 0, p: p, bolt: { x: 0, y: 0, t: 0, wait: 0 } };
  }
  function rnd(fx) {
    var x = fx.seed ? fx.seed : 0x9E3779B9;
    x = (x ^ (x << 13)) >>> 0; x = (x ^ (x >>> 17)) >>> 0; x = (x ^ (x << 5)) >>> 0;
    return (fx.seed = x);
  }
  function rndf(fx, lo, hi) { return f(f(lo) + f(f(f(f(hi) - f(lo)) * f(rnd(fx) & 0xFFFF)) / f(65535))); }
  function sinTurns(t) {
    t = f(t - f(t | 0));
    if (t < 0) t = f(t + 1);
    var x = f(f(t < 0.5 ? t : f(t - 1)) * f(6.2831853));
    var s = x < 0 ? -1 : 1;
    x = f(x * s);
    if (x > f(1.5707963)) x = f(f(3.1415927) - x);
    var x2 = f(x * x);
    var v = f(x * f(1 - f(f(x2 / 6) * f(1 - f(x2 / 20)))));
    return f(s * (v > 1 ? 1 : v));
  }
  function halfChord(y) {
    var dy = f(y - FX_R), h2 = f(f(FX_R * FX_R) - f(dy * dy));
    if (h2 <= 0) return 0;
    var r = f(h2 / FX_R);
    for (var i = 0; i < 6; i++) r = f(f(0.5) * f(r + f(h2 / r)));
    return r;
  }
  function inside(x, y, pad) {
    var dx = f(x - FX_R), dy = f(y - FX_R), r = f(FX_R + pad);
    return f(f(dx * dx) + f(dy * dy)) <= f(r * r);
  }
  function place(fx, p, y0, y1) {
    p.y = rndf(fx, y0, y1);
    var hc = halfChord(p.y);
    p.x = f(FX_R + rndf(fx, -hc, hc));
  }
  function spawn(fx, p, fresh) {
    switch (fx.kind) {
      case WX.RAIN: case WX.THUNDER: case WX.DRIZZLE: {
        var light = fx.kind === WX.DRIZZLE;
        p.f = light ? F.DROP_SMALL : F.DROP;
        p.vy = light ? rndf(fx, 150, 210) : rndf(fx, 260, 360);
        p.vx = light ? 0 : rndf(fx, -18, -8);
        p.opa = light ? 170 : 210;
        if (fresh) place(fx, p, 20, FX_SIDE - 20);
        else { p.y = rndf(fx, -10, 30); p.x = f(FX_R + rndf(fx, -halfChord(40), halfChord(40))); }
        break;
      }
      case WX.SNOW:
        p.f = (rnd(fx) & 1) ? F.FLAKE : F.FLAKE_SMALL;
        p.vy = rndf(fx, 28, 55);
        p.vx = 0;
        p.ph = rndf(fx, 0, 1);
        p.rate = rndf(fx, 0.3, 0.7);
        p.opa = 225;
        if (fresh) place(fx, p, 20, FX_SIDE - 20);
        else { p.y = rndf(fx, -10, 20); p.x = f(FX_R + rndf(fx, -halfChord(30), halfChord(30))); }
        break;
      case WX.FOG: case WX.CLOUDY: case WX.PARTLY: case WX.PARTLY_N: {
        var fog = fx.kind === WX.FOG;
        p.f = fog ? F.MIST : F.WISP;
        p.vx = fog ? rndf(fx, 8, 18) : rndf(fx, 10, 22);
        p.vy = 0;
        p.opa = fog ? 150 : 120;
        var y0 = fog ? 150 : 90, y1 = fog ? 370 : 330;
        if (fresh) place(fx, p, y0, y1);
        else { p.y = rndf(fx, y0, y1); p.x = f(f(FX_R - halfChord(p.y)) - 20); }
        break;
      }
      case WX.CLEAR_N:
        p.f = F.STAR;
        p.vx = p.vy = 0;
        p.ph = rndf(fx, 0, 1);
        p.rate = rndf(fx, 0.15, 0.45);
        p.opa = 200;
        place(fx, p, 40, 200);
        break;
      default:
        p.opa = 0;
    }
  }
  function want(kind) {
    switch (kind) {
      case WX.RAIN: return 8;
      case WX.THUNDER: return 10;
      case WX.DRIZZLE: return 6;
      case WX.SNOW: return 10;
      case WX.FOG: return 6;
      case WX.CLOUDY: return 5;
      case WX.PARTLY: case WX.PARTLY_N: return 3;
      case WX.CLEAR_N: return 8;
      default: return 0;
    }
  }
  function set(fx, kind, rows) {
    var n = want(kind);
    if (n > rows) n = rows;
    if (n > FX_MAX) n = FX_MAX;
    if (n < 0) n = 0;
    if (kind === fx.kind && n === fx.n) return;
    fx.kind = kind;
    fx.n = n;
    for (var i = 0; i < n; i++) spawn(fx, fx.p[i], true);
    fx.bolt.t = f(-1);
    fx.bolt.wait = kind === WX.THUNDER ? rndf(fx, 1, 3) : 0;
  }
  function step(fx, dt) {
    dt = f(dt);
    if (dt <= 0) return;
    if (dt > f(0.1)) dt = f(0.1);
    for (var i = 0; i < fx.n; i++) {
      var p = fx.p[i];
      p.x = f(p.x + f(p.vx * dt));
      p.y = f(p.y + f(p.vy * dt));
      p.ph = f(p.ph + f(p.rate * dt));
      if (p.ph >= 1) p.ph = f(p.ph - f(p.ph | 0));
      var gone = (p.vy > 0 && p.y > FX_R && !inside(p.x, p.y, 12)) ||
                 (p.vx > 0 && p.x > FX_R && !inside(p.x, p.y, 30));
      if (gone) spawn(fx, p, false);
    }
    if (fx.kind !== WX.THUNDER) return;
    var b = fx.bolt;
    if (b.t >= 0) {
      b.t = f(b.t + dt);
      if (b.t > f(0.3)) { b.t = f(-1); b.wait = rndf(fx, 2.5, 7); }
    } else {
      b.wait = f(b.wait - dt);
      if (b.wait <= 0) { b.t = 0; b.x = f(FX_R + rndf(fx, -90, 90)); b.y = rndf(fx, 110, 170); }
    }
  }
  // the bolt's opacity now and its centre: {opa, x, y}
  function bolt(fx) {
    var t = fx.bolt.t, o = { x: fx.bolt.x, y: fx.bolt.y, opa: 0 };
    if (fx.kind !== WX.THUNDER || t < 0) return o;
    o.opa = t < f(0.07) ? 255 : t < f(0.12) ? 0 : t < f(0.22) ? 190 : t < f(0.3) ? 90 : 0;
    return o;
  }
  function opa(fx, i) {
    if (i < 0 || i >= fx.n) return 0;
    var p = fx.p[i];
    if (p.f === F.STAR) {
      var s = f(f(0.5) + f(f(0.5) * sinTurns(p.ph)));
      return f(60 + f(f(p.opa - 60) * s)) | 0;
    }
    return p.opa;
  }
  function sway(fx, i) {
    if (i < 0 || i >= fx.n || fx.kind !== WX.SNOW) return 0;
    return f(10 * sinTurns(fx.p[i].ph));
  }
  var api = { FX_MAX: FX_MAX, WX: WX, create: create, set: set, step: step, opa: opa, sway: sway, bolt: bolt };
  root.WeatherFx = api;
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
})(typeof window !== 'undefined' ? window : globalThis);
