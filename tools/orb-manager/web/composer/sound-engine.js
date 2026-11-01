/* sound-engine.js — D39's default clock sounds, generated rather than shipped.
 *
 * docs/sound-plan.md §6: the clock's editor region carries this file instead of WAVs.
 * It is the JS port of the three Python prototypes the owner auditioned, and the only
 * generator since they retired on 2026-11-01 (git history keeps them, as
 * tools/sound_{synth,pack,ticks}_proto.py). The seam/RMS/floor checks of §6 run on THIS
 * output in _selftest/headless-sound.test.js.
 *
 * Everything is 16-bit mono at 22,050 Hz, the one rate the orb's engine runs. Every
 * render is seeded, so a theme bakes to the same bytes twice. Two kinds of output:
 *
 *   renderLoop(voice, rate, seed)  an 8 s tick loop at 1, 2, 4 or 8 beats a second,
 *                                  seamless: every beat's tail wraps to the start and
 *                                  the speaker high-pass runs circularly
 *   renderSound(id, seed)          a one-shot from CATALOG: a chime, a stroke, an event
 *
 * plus loopFromTick(), which turns one uploaded tick into a varied loop the same way.
 * PRESETS is §7.2's default mapping per composer preset.
 */
(function (root) {
  'use strict';
  var SR = 22050, TAU = 2 * Math.PI, HP = 350, LOOP_S = 8;
  var RATES = [1, 2, 4, 8];

  /* ---- a seeded random stream (mulberry32 + Box-Muller) ---------------------------- */
  function Rng(seed) {
    var a = (seed >>> 0) || 1, spare = null;
    function u() {
      a = (a + 0x6D2B79F5) | 0;
      var t = Math.imul(a ^ (a >>> 15), 1 | a);
      t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
      return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    }
    function n() {
      if (spare !== null) { var s = spare; spare = null; return s; }
      var u1 = 0; while (!u1) u1 = u();
      var r = Math.sqrt(-2 * Math.log(u1)), th = TAU * u();
      spare = r * Math.sin(th);
      return r * Math.cos(th);
    }
    return { u: u, n: n, uniform: function (lo, hi) { return lo + (hi - lo) * u(); },
             int: function (k) { return Math.floor(u() * k); },
             exp: function (mean) { return -Math.log(1 - u()) * mean; } };
  }
  var R = Rng(1);

  /* ---- signals: Float64Array, built and mixed sample-exactly ------------------------ */
  function len(d) { return Math.round(d * SR); }
  function Z(n) { return new Float64Array(Math.max(0, n)); }
  function mix() {
    var n = 0, i, k;
    for (k = 0; k < arguments.length; k++) n = Math.max(n, arguments[k].length);
    var out = Z(n);
    for (k = 0; k < arguments.length; k++) { var a = arguments[k]; for (i = 0; i < a.length; i++) out[i] += a[i]; }
    return out;
  }
  function scale(x, g) { var y = Z(x.length); for (var i = 0; i < x.length; i++) y[i] = x[i] * g; return y; }
  function peak(x) { var m = 0; for (var i = 0; i < x.length; i++) m = Math.max(m, Math.abs(x[i])); return m; }
  function rms(x) { var s = 0; for (var i = 0; i < x.length; i++) s += x[i] * x[i]; return Math.sqrt(s / Math.max(1, x.length)); }
  /* events: [[start_s, signal]], tail: seconds past the last end */
  function seq(events, tail) {
    var end = 0, i, k;
    for (k = 0; k < events.length; k++) end = Math.max(end, events[k][0] + events[k][1].length / SR);
    var out = Z(len(end + (tail === undefined ? 1 : tail)));
    for (k = 0; k < events.length; k++) {
      var at = Math.round(events[k][0] * SR), s = events[k][1];
      for (i = 0; i < s.length && at + i < out.length; i++) out[at + i] += s[i];
    }
    return out;
  }
  /* the same, laid on a circle of n samples: a tail past the end lands at the start */
  function circ(events, n) {
    var out = Z(n);
    for (var k = 0; k < events.length; k++) {
      var at = Math.round(events[k][0] * SR), s = events[k][1];
      for (var i = 0; i < s.length; i++) out[(((at + i) % n) + n) % n] += s[i];
    }
    return out;
  }
  function note(name) {
    var names = { C: -9, D: -7, E: -5, F: -4, G: -2, A: 0, B: 2 };
    var semi = names[name[0]] + (name.indexOf('#') > 0 ? 1 : (name.length > 2 && name[1] === 'b' ? -1 : 0));
    return 440 * Math.pow(2, (semi + 12 * (parseInt(name[name.length - 1], 10) - 4)) / 12);
  }

  /* ---- primitives (the synth prototype's) --------------------------------------------- */
  function resonator(x, f, q) {
    var w = TAU * f / SR, al = Math.sin(w) / (2 * q), a0 = 1 + al;
    var b0 = al / a0, b2 = -al / a0, a1 = -2 * Math.cos(w) / a0, a2 = (1 - al) / a0;
    var y = Z(x.length), x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    for (var i = 0; i < x.length; i++) {
      var o = b0 * x[i] + b2 * x2 - a1 * y1 - a2 * y2;
      x2 = x1; x1 = x[i]; y2 = y1; y1 = o; y[i] = o;
    }
    return y;
  }
  function tick(dur, modes, clickMs, decayMs) {
    var n = len(dur), exc = Z(n), k = Math.max(1, Math.round(clickMs / 1000 * SR)), i;
    for (i = 0; i < k && i < n; i++) exc[i] = R.n() * Math.exp(-i / (k / 3));
    var out = Z(n);
    for (var m = 0; m < modes.length; m++) {
      var r = resonator(exc, modes[m][0], modes[m][1]);
      for (i = 0; i < n; i++) out[i] += modes[m][2] * r[i];
    }
    for (i = 0; i < n; i++) out[i] *= Math.exp(-i / SR / (decayMs / 1000));
    return out;
  }
  /* one escapement beat: unlock/impulse/drop sub-clicks, each RMS-pinned so loudness
   * variance comes only from the loop's own term (§6's prototype finding) */
  function beat(modes, decayMs, clicks, dur, thumpHz, thumpGain, vr) {
    vr = vr === undefined ? 1 : vr;
    var out = Z(len(dur)), gmax = 0, k, i;
    for (k = 0; k < clicks.length; k++) gmax = Math.max(gmax, clicks[k][1]);
    for (k = 0; k < clicks.length; k++) {
      var off = Math.max(0, clicks[k][0] * (1 + 0.15 * vr * R.n()));
      var m = modes.map(function (md) {
        return [md[0] * (1 + 0.02 * vr * R.n()), md[1] * (1 + 0.1 * vr * R.n()), md[2] * Math.pow(10, 2 * vr * R.n() / 20)];
      });
      var c = tick(dur, m, clicks[k][2], decayMs), r0 = rms(c) || 1;
      for (i = 0; i < c.length; i++) c[i] /= r0;
      if (thumpHz && clicks[k][1] === gmax && thumpGain)
        for (i = 0; i < c.length; i++) c[i] += thumpGain * 3 * Math.sin(TAU * thumpHz * i / SR) * Math.exp(-i / SR / 0.008);
      var at = Math.round(off / 1000 * SR);
      for (i = 0; at + i < out.length; i++) out[at + i] += clicks[k][1] * c[i];
    }
    return out;
  }
  var CLOCK_CLICKS = [[0, 0.35, 1.0], [3.5, 0.6, 1.5], [9.0, 1.0, 1.5]];
  var POCKET_CLICKS = [[0, 0.4, 0.6], [1.8, 0.7, 0.6], [4.5, 1.0, 0.8]];
  var WRIST_CLICKS = [[0, 0.4, 0.4], [1.0, 0.7, 0.4], [2.6, 1.0, 0.5]];

  var BELL = [[0.5, 0.35, 1.0], [1.0, 1.0, 0.55], [1.19, 0.45, 0.4], [1.5, 0.3, 0.3], [2.0, 0.5, 0.22],
              [2.51, 0.2, 0.15], [2.66, 0.15, 0.13], [3.01, 0.12, 0.1], [4.07, 0.08, 0.07]];
  function bell(f0, dur, brightness, ring) {
    dur = dur || 3; brightness = brightness === undefined ? 1 : brightness; ring = ring === undefined ? 1 : ring;
    var n = len(dur), out = Z(n), i;
    for (var p = 0; p < BELL.length; p++) {
      var f = f0 * BELL[p][0];
      if (f > SR * 0.45) continue;
      var a = BELL[p][1] * Math.pow(brightness, BELL[p][0] - 1), d = BELL[p][2] * ring * 6 * Math.sqrt(440 / f0);
      var dets = [1, 1 + 0.0015 * BELL[p][0]];
      for (var j = 0; j < 2; j++) {
        var ph = R.uniform(0, TAU), ff = TAU * f * dets[j] / SR;
        for (i = 0; i < n; i++) out[i] += 0.5 * a * Math.sin(ff * i + ph) * Math.exp(-i / SR / d);
      }
    }
    var strike = Z(n);
    for (i = 0; i < n; i++) strike[i] = R.n() * Math.exp(-i / SR / 0.002) * 0.15;
    return mix(out, resonator(strike, f0 * 3, 4));
  }
  function chimeRod(f0, dur) {
    var n = len(dur || 2.5), out = Z(n), P = [[1, 1.0, 1.2], [2.76, 0.35, 0.5], [5.40, 0.15, 0.2], [8.93, 0.06, 0.08]];
    for (var p = 0; p < P.length; p++) {
      if (f0 * P[p][0] >= SR * 0.45) continue;
      var w = TAU * f0 * P[p][0] / SR;
      for (var i = 0; i < n; i++) out[i] += P[p][1] * Math.sin(w * i) * Math.exp(-i / SR / P[p][2]);
    }
    for (i = 0; i < n; i++) out[i] *= 1 - Math.exp(-i / SR / 0.001);
    return out;
  }

  /* ---- instruments (the pack prototype's) --------------------------------------------- */
  /* parts: [ratio, amp, decay_s]; detune > 0 doubles each partial into a beating pair */
  function partials(f0, parts, dur, attack, detune) {
    attack = attack === undefined ? 0.002 : attack;
    var n = len(dur), out = Z(n), i;
    for (var p = 0; p < parts.length; p++) {
      var f = f0 * parts[p][0];
      if (f >= SR * 0.45) continue;
      var pairs = detune ? [[1, 0.5], [1 + detune * Math.pow(parts[p][0], 0.3), 0.5]] : [[1, 1]];
      for (var j = 0; j < pairs.length; j++) {
        var ph = R.uniform(0, TAU), w = TAU * f * pairs[j][0] / SR, g = pairs[j][1] * parts[p][1];
        for (i = 0; i < n; i++) out[i] += g * Math.sin(w * i + ph) * Math.exp(-i / SR / parts[p][2]);
      }
    }
    if (attack > 0) for (i = 0; i < n; i++) out[i] *= Math.min(1, i / SR / attack);
    return out;
  }
  function click(f, q, gain, durS) {
    f = f || 3000; q = q || 2; gain = gain === undefined ? 0.3 : gain;
    var n = Math.max(1, Math.round((durS || 0.004) * SR)), x = Z(len(0.03)), i;
    for (i = 0; i < n; i++) x[i] = R.n() * Math.exp(-i / (n / 3));
    var y = resonator(x, f, q), m = peak(y) || 1;
    return scale(y, gain / m);
  }
  function envAR(n, attack, release) {
    var e = Z(n), a = Math.round(attack * SR), r = Math.round(release * SR), i;
    for (i = 0; i < n; i++) e[i] = 1;
    for (i = 0; i < a && i < n; i++) e[i] = 0.5 - 0.5 * Math.cos(Math.PI * i / Math.max(1, a - 1));
    for (i = 0; i < r && i < n; i++) e[n - r + i] *= 0.5 + 0.5 * Math.cos(Math.PI * i / Math.max(1, r - 1));
    return e;
  }
  function mul(a, b) { var y = Z(a.length); for (var i = 0; i < a.length; i++) y[i] = a[i] * b[i]; return y; }
  function bandnoise(dur, f, q) {
    var n = len(dur), x = Z(n);
    for (var i = 0; i < n; i++) x[i] = R.n();
    var y = resonator(x, f, q);
    return scale(y, 1 / (rms(y) + 1e-9));
  }
  function wobble(n, depth, rates) {
    rates = rates || [3.1, 4.7, 6.3];
    var ph = rates.map(function () { return R.uniform(0, TAU); }), out = Z(n);
    for (var i = 0; i < n; i++) {
      var s = 0;
      for (var k = 0; k < rates.length; k++) s += Math.sin(TAU * rates[k] * i / SR + ph[k]);
      out[i] = 1 + depth * s / rates.length;
    }
    return out;
  }
  /* an additive oscillator over a frequency curve: harm = [[k, amp]] */
  function osc(fcurve, harm) {
    var n = fcurve.length, out = Z(n), ph = 0, f0 = 0, i;
    for (i = 0; i < n; i++) f0 = Math.max(f0, fcurve[i]);
    var keep = harm.filter(function (h) { return f0 * h[0] < SR * 0.45; });
    for (i = 0; i < n; i++) {
      ph += TAU * fcurve[i] / SR;
      var s = 0;
      for (var k = 0; k < keep.length; k++) s += keep[k][1] * Math.sin(keep[k][0] * ph);
      out[i] = s;
    }
    return out;
  }
  function flat(n, f) { var c = Z(n); for (var i = 0; i < n; i++) c[i] = f; return c; }
  function bar(f, kind, dur) {
    kind = kind || 'vib'; dur = dur || 2.5;
    var P = { vib: [[1, 1, 1.6], [4.0, 0.35, 0.4], [10.0, 0.08, 0.12]],
              marimba: [[1, 1, 0.45], [3.93, 0.4, 0.08], [9.2, 0.1, 0.03]],
              glock: [[1, 1, 1.2], [2.76, 0.5, 0.3], [5.40, 0.25, 0.1]] }[kind];
    var x = partials(f, P, dur);
    if (kind === 'vib') for (var i = 0; i < x.length; i++) x[i] *= 1 + 0.25 * Math.sin(TAU * 5.5 * i / SR);
    return mix(x, click(f * 3, 2, 0.15));
  }
  function tine(f, dur, parts) {
    parts = parts || [[1, 1, 1.4], [6.27, 0.3, 0.15], [17.55, 0.05, 0.04]];
    return mix(partials(f, parts, dur || 1.8, 0.001), click(Math.min(f * 4, 8000), 2, 0.08));
  }
  function celesta(f, dur) {
    return mix(partials(f, [[1, 1, 0.9], [2, 0.2, 0.4], [3, 0.05, 0.2]], dur || 1.6, 0.002), click(f * 2, 2, 0.1));
  }
  function tube(f, dur, g) {
    return scale(partials(f, [[1, 1, 3.0], [2.756, 0.6, 1.2], [5.404, 0.35, 0.45], [8.933, 0.15, 0.2]], dur || 4), g === undefined ? 1 : g);
  }
  function bowl(f, dur) {
    return mix(partials(f, [[1, 1, 6], [2.71, 0.5, 3], [5.12, 0.25, 1.5], [8.0, 0.1, 0.8]], dur || 7, 0.004, 0.002),
               click(f * 2, 2, 0.15));
  }
  function pipe(f, dur, o) {
    o = o || {};
    var attack = o.attack === undefined ? 0.05 : o.attack, release = o.release === undefined ? 0.1 : o.release;
    var harm = o.harm || [[1, 1], [2, 0.25], [3, 0.1]], slide = o.slide || 0, slideT = o.slideT || 0.08;
    var breath = o.breath === undefined ? 0.1 : o.breath, vib = o.vib || 0, droop = o.droop || 0;
    var n = len(dur), fc = Z(n), i;
    for (i = 0; i < n; i++) {
      var t = i / SR;
      fc[i] = f * (1 - slide * Math.exp(-t / slideT));
      if (droop) fc[i] *= 1 - droop * Math.min(1, Math.max(0, (t - (dur - release)) / Math.max(release, 1e-3)));
    }
    if (vib) { var wb = wobble(n, vib); for (i = 0; i < n; i++) fc[i] *= wb[i]; }
    var tone = osc(fc, harm), r0 = rms(tone) + 1e-9, bn = bandnoise(dur, f, 12), env = envAR(n, attack, release), out = Z(n);
    for (i = 0; i < n; i++) out[i] = (tone[i] / r0 + breath * bn[i]) * env[i];
    return out;
  }
  function brass(f, dur) {
    var n = len(dur), fc = wobble(n, 0.002), harm = [], i;
    for (i = 0; i < n; i++) fc[i] *= f;
    for (var k = 1; k < 14; k++) harm.push([k, 1 / k]);
    var x = osc(fc, harm), env = envAR(n, 0.06, 0.15);
    for (i = 0; i < n; i++) x[i] *= (1 + 0.05 * Math.sin(TAU * 31 * i / SR)) * env[i];
    return x;
  }
  function fm(fc, ratio, index, dur, idxDecay, ampDecay, attack) {
    idxDecay = idxDecay || 0.6; ampDecay = ampDecay || 1.2; attack = attack || 0.002;
    var n = len(dur), out = Z(n);
    for (var i = 0; i < n; i++) {
      var t = i / SR, I = index * Math.exp(-t / idxDecay);
      out[i] = Math.sin(TAU * fc * t + I * Math.sin(TAU * fc * ratio * t)) * Math.exp(-t / ampDecay) * Math.min(1, t / attack);
    }
    return out;
  }
  function sineTone(f, dur, attack, release) {
    var n = len(dur);
    return mul(osc(flat(n, f), [[1, 1]]), envAR(n, attack === undefined ? 0.004 : attack, release === undefined ? 0.004 : release));
  }
  function square(f, dur, duty) {
    duty = duty === undefined ? 0.5 : duty;
    var n = len(dur), harm = [];
    for (var k = 1; k < 40; k++) harm.push([k, Math.sin(Math.PI * k * duty) / k]);
    return mul(osc(flat(n, f), harm), envAR(n, 0.003, 0.01));
  }
  function sawPluck(f, dur) {
    var n = len(dur || 0.5), x = Z(n), i;
    for (var k = 1; k < 40 && f * k < SR * 0.45; k++) {
      var w = TAU * f * k / SR;
      for (i = 0; i < n; i++) {
        var t = i / SR, bright = Math.exp(-(k - 1) / (1 + 10 * Math.exp(-t / 0.12)));
        x[i] += Math.sin(w * i) / k * bright;
      }
    }
    for (i = 0; i < n; i++) x[i] *= Math.exp(-i / SR / 0.25) * Math.min(1, i / SR / 0.003);
    return x;
  }
  function echo(x, delay, gain, n) {
    var d = len(delay), out = Z(x.length + d * n);
    for (var k = 0; k <= n; k++) { var g = Math.pow(gain, k); for (var i = 0; i < x.length; i++) out[k * d + i] += x[i] * g; }
    return out;
  }
  /* a short noise-tail reverb, convolved directly over a decimated IR (offline; short) */
  function reverb(x, tail, wet) {
    var n = len(tail), ir = Z(n), i, j, e = 0;
    for (i = 0; i < n; i++) { ir[i] = R.n() * Math.exp(-i / SR / (tail / 5)); e += ir[i] * ir[i]; }
    e = Math.sqrt(e) || 1;
    var out = Z(x.length + n), step = 4;           // every 4th IR tap: diffuse, and cheap
    for (j = 0; j < n; j += step) {
      var h = ir[j] / e * step;
      if (!h) continue;
      for (i = 0; i < x.length; i++) out[i + j] += x[i] * h;
    }
    var m = peak(out) || 1, px = peak(x);
    return mix(x, scale(out, wet * px / m));
  }
  function hann(f, dur) {
    var n = len(dur), out = Z(n);
    for (var i = 0; i < n; i++) { var s = Math.sin(Math.PI * i / n); out[i] = Math.sin(TAU * f * i / SR) * s * s; }
    return out;
  }

  /* ---- finishing: the speaker's floor, a trimmed tail, a fixed peak ---------------- */
  /* Second-order Butterworth high-pass at HP. A one-shot runs it forward then backward
   * (zero phase, fourth order, as the prototype's FFT filter was). A loop runs it over
   * two laps and keeps the second, so the filter's state at sample 0 is the state the
   * loop's own end leaves: circular, and the seam stays seamless. */
  function hpCoefs() {
    var w = TAU * HP / SR, c = Math.cos(w), al = Math.sin(w) / (2 * Math.SQRT1_2), a0 = 1 + al;
    return [(1 + c) / 2 / a0, -(1 + c) / a0, (1 + c) / 2 / a0, -2 * c / a0, (1 - al) / a0];
  }
  function iir(x, k, from, to, step) {
    var y = Z(x.length), x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    for (var i = from; i !== to; i += step) {
      var o = k[0] * x[i] + k[1] * x1 + k[2] * x2 - k[3] * y1 - k[4] * y2;
      x2 = x1; x1 = x[i]; y2 = y1; y1 = o; y[i] = o;
    }
    return y;
  }
  function highpass(x) {
    var k = hpCoefs(), f = iir(x, k, 0, x.length, 1);
    return iir(f, k, x.length - 1, -1, -1);
  }
  /* Each pass runs over three laps and keeps the middle one: by then the filter has
   * seen a whole loop, so its state entering the lap is the state the lap's own end
   * leaves, in either direction. */
  function highpassLoop(x) {
    var n = x.length, k = hpCoefs(), i;
    function middle(sig, fwd) {
      var three = Z(3 * n);
      for (i = 0; i < 3 * n; i++) three[i] = sig[i % n];
      var y = fwd ? iir(three, k, 0, 3 * n, 1) : iir(three, k, 3 * n - 1, -1, -1);
      return y.slice(n, 2 * n);
    }
    return middle(middle(x, true), false);
  }
  function toPcm(x, peakDb) {
    var m = peak(x) || 1, g = Math.pow(10, (peakDb === undefined ? -1 : peakDb) / 20) / m, out = new Int16Array(x.length);
    for (var i = 0; i < x.length; i++) out[i] = Math.max(-32768, Math.min(32767, Math.round(x[i] * g * 32767)));
    return out;
  }
  function mean0(x) { var s = 0, i; for (i = 0; i < x.length; i++) s += x[i]; s /= Math.max(1, x.length); var y = Z(x.length); for (i = 0; i < x.length; i++) y[i] = x[i] - s; return y; }
  function finish(x, opts) {
    opts = opts || {};
    var y = opts.raw ? mean0(x) : highpass(mean0(x)), m = peak(y) || 1, thr = m * 1e-3, last = y.length;
    while (last > 1 && Math.abs(y[last - 1]) <= thr) last--;
    y = y.subarray(0, last);
    var fade = Math.min(y.length, len(0.02));
    for (var i = 0; i < fade; i++) y[y.length - fade + i] *= 1 - i / fade;
    return toPcm(y);
  }
  function finishLoop(x) { return toPcm(highpassLoop(mean0(x))); }

  /* ---- tick loops (the rate-grid prototype's): every voice at every rate ------------ */
  function fit(sig, period) {
    var keep = Math.floor(0.85 * period * SR);
    if (sig.length <= keep) return sig;
    var out = sig.slice(0, keep), f = Math.max(8, Math.floor(keep / 5));
    for (var i = 0; i < f; i++) out[keep - f + i] *= 0.5 * (1 + Math.cos(Math.PI * i / f));
    return out;
  }
  function pair(a, b) { return function (i) { return (i % 2 === 0 ? a : b)(); }; }
  function B(modes, decay, clicks, dur, th, tg, vr) { return function () { return beat(modes, decay, clicks, dur, th || 0, tg || 0, vr); }; }
  function buzz(f) {
    var n = len(0.035), out = Z(n);
    for (var i = 0; i < n; i++) {
      var t = i / SR, s = 0;
      for (var k = 2; k < 30 && f * k < SR * 0.45; k++) s += Math.sin(TAU * f * k * t) / k;
      out[i] = s * Math.exp(-t / 0.012) * Math.min(1, t / 0.002);
    }
    return out;
  }
  function drop(f0) {
    var n = len(0.06), out = Z(n), ph = 0;
    for (var i = 0; i < n; i++) {
      var t = i / SR;
      ph += TAU * f0 * (1 + 1.2 * (1 - Math.exp(-t / 0.01))) / SR;
      out[i] = Math.sin(ph) * Math.exp(-t / 0.02) * Math.min(1, t / 0.001);
    }
    return out;
  }
  function glitchBeat() {
    var n = len(0.01), x = Z(2 * n), i;
    var src = Z(n);
    for (i = 0; i < n; i++) src[i] = R.n();
    for (i = 0; i < n; i++) x[i] = Math.round(src[i - (i % 3)] * 2) / 2 * Math.exp(-i / (n / 3));
    return resonator(x, R.uniform(1500, 4000), 4);
  }
  /* name: [label, beat(i, nb, period), beat_error_ms, jitter_ms, amp_db, rates] */
  var VOICES = {
    mantel: ['Mantel clock', pair(B([[3200, 18, 1.0], [5100, 25, 0.6], [1800, 12, 0.5]], 12, CLOCK_CLICKS, 0.08),
                                  B([[2600, 16, 1.0], [4300, 22, 0.5], [1500, 10, 0.6]], 12, CLOCK_CLICKS, 0.08)), 6, 1.0, 1.0],
    grandfather: ['Grandfather', pair(B([[1900, 20, 1.0], [3100, 25, 0.5], [900, 10, 0.5]], 30, CLOCK_CLICKS, 0.15, 380, 0.25),
                                      B([[1600, 20, 1.0], [2700, 25, 0.5], [800, 10, 0.5]], 30, CLOCK_CLICKS, 0.15, 340, 0.3)), 8, 1.5, 1.0],
    soft: ['Soft', pair(B([[2200, 8, 1.0], [1200, 6, 0.6]], 8, CLOCK_CLICKS, 0.06), B([[2200, 8, 1.0], [1200, 6, 0.6]], 8, CLOCK_CLICKS, 0.06)), 0, 1.0, 1.5],
    pocket: ['Pocket watch', pair(B([[4200, 25, 1.0], [6800, 30, 0.5], [2600, 15, 0.4]], 6, POCKET_CLICKS, 0.04),
                                  B([[4000, 25, 1.0], [6500, 30, 0.5], [2500, 15, 0.4]], 6, POCKET_CLICKS, 0.04)), 1.5, 0.3, 1.0],
    wrist: ['Wristwatch', pair(B([[5500, 30, 1.0], [8200, 30, 0.5], [3500, 20, 0.3]], 4, WRIST_CLICKS, 0.03, 0, 0, 0.7),
                               B([[5300, 30, 1.0], [7900, 30, 0.5], [3400, 20, 0.3]], 4, WRIST_CLICKS, 0.03, 0, 0, 0.7)), 0.6, 0.15, 0.8],
    classic: ['Classic movement', pair(B([[3000, 20, 1.0], [4800, 25, 0.5], [1900, 12, 0.5]], 8, POCKET_CLICKS, 0.05),
                                       B([[3000, 20, 1.0], [4800, 25, 0.5], [1900, 12, 0.5]], 8, POCKET_CLICKS, 0.05)), 1.0, 0.3, 0.8],
    tourbillon: ['Tourbillon', function (i, nb) {
      var sw = 1 + 0.03 * Math.sin(TAU * i / nb);
      return scale(beat([[3400 * sw, 22, 1.0], [5600 * sw, 28, 0.5], [2200 * sw, 14, 0.4]], 7, POCKET_CLICKS, 0.05, 0, 0),
                   1 + 0.15 * Math.sin(TAU * i / nb));
    }, 0, 0.3, 0.8],
    station_relay: ['Station relay', pair(B([[1400, 8, 1.0], [2600, 10, 0.6], [5200, 12, 0.2]], 10, [[0, 1.0, 0.8], [38, 0.35, 0.6]], 0.12, 0, 0, 0.5),
                                          B([[1400, 8, 1.0], [2600, 10, 0.6], [5200, 12, 0.2]], 10, [[0, 1.0, 0.8], [38, 0.35, 0.6]], 0.12, 0, 0, 0.5)), 0, 0.4, 0.8],
    regulator: ['Regulator', pair(B([[2400, 14, 1.0], [3900, 20, 0.5], [520, 6, 0.8]], 25, CLOCK_CLICKS, 0.15, 0, 0, 0.6),
                                  B([[2400, 14, 1.0], [3900, 20, 0.5], [520, 6, 0.8]], 25, CLOCK_CLICKS, 0.15, 0, 0, 0.6)), 0, 0.3, 0.5],
    woodblock: ['Woodblock', pair(B([[1100, 30, 1.0], [2650, 25, 0.5], [4000, 20, 0.25]], 15, [[0, 1.0, 0.8]], 0.08, 0, 0, 0.5),
                                  B([[990, 30, 1.0], [2385, 25, 0.5], [3600, 20, 0.25]], 15, [[0, 1.0, 0.8]], 0.08, 0, 0, 0.5)), 0, 0.6, 0.8],
    neon_buzz: ['Neon buzz', function (i) { return buzz(i % 2 === 0 ? 240 : 200); }, 0, 0.5, 0.8],
    waterdrop: ['Water drop', function () { return drop(900 * (1 + 0.08 * R.n())); }, 0, 3.0, 0.8],
    sine_blip: ['Sine blip', function (i) { return hann((i % 2 === 0 ? 1600 : 1200) * (1 + 0.005 * R.n()), 0.006); }, 0, 0.2, 0.8],
    glitch: ['Glitch', function () { return glitchBeat(); }, 0, 1.0, 2.0],
    double_pip: ['Double pip', function (i) {
      var f = i % 2 === 0 ? 2200 : 1900;
      return seq([[0, hann(f, 0.012)], [0.03, scale(hann(f, 0.012), 0.7)]], 0);
    }, 0, 0.2, 0.8],
    data_patter: ['Data patter', function (i, nb, p) {
      var span = Math.min(0.07, 0.5 * p), ev = [], k = 3 + R.int(3);
      for (var j = 0; j < k; j++) ev.push([R.uniform(0, span), scale(hann(R.uniform(2000, 6000), 0.008), R.uniform(0.3, 1))]);
      return seq(ev, 0.01);
    }, 0, 0.5, 1.5],
    /* rate-limited voices (§4.2): offered only where no stepping hand must match them */
    heartbeat: ['Heartbeat', function () {
      var n = len(0.2), lub = Z(n), dub = Z(n);
      for (var j = 0; j < n; j++) {
        var t = j / SR;
        lub[j] = Math.sin(TAU * 420 * (1 - 0.05 * t) * t) * Math.exp(-t / 0.045);
        dub[j] = 0.6 * Math.sin(TAU * 540 * t) * Math.exp(-t / 0.035);
      }
      return seq([[0, lub], [0.28, dub]], 0);
    }, 0, 8, 0.8, [1]],
    geiger: ['Geiger', null, 0, 0, 0, []]
  };
  function voiceRates(id) { var v = VOICES[id]; return v ? (v[5] || RATES) : []; }

  function renderLoop(id, rate, seed) {
    var v = VOICES[id];
    if (!v) throw new Error('no tick voice ' + JSON.stringify(id));
    R = Rng(seed === undefined ? 23 : seed);
    var n = LOOP_S * SR;
    if (id === 'geiger') {
      var k = 12 + R.int(16), ev = [];
      for (var g = 0; g < k; g++) ev.push([R.uniform(0, LOOP_S), scale(click(2800, 1.5, 1.0, 0.001), R.uniform(0.4, 1.0))]);
      return finishLoop(circ(ev, n));
    }
    if (voiceRates(id).indexOf(rate) < 0) throw new Error('the ' + v[0] + ' voice has no ' + rate + '-per-second loop');
    var period = 1 / rate, nb = LOOP_S * rate, ph = R.uniform(0, TAU), evs = [];
    var be = v[2] / rate, jit = v[3], amp = v[4];
    for (var i = 0; i < nb; i++) {
      var gain = Math.pow(10, (amp * 0.5 * Math.sin(TAU * 2 * i / nb + ph) + amp * 0.5 * R.n()) / 20);
      var start = i * period + (i % 2 ? be / 1000 : 0) + R.n() * jit / 1000;
      evs.push([start, scale(fit(v[1](i, nb, period), period), gain)]);
    }
    return finishLoop(circ(evs, n));
  }

  /* An owner's single tick made into a varied loop (§7 "uploaded ticks"): per-beat pitch
   * and gain by resampling (±2 %), timing jitter, a pitch-shifted copy as the tock, and
   * circular placement. `tick` is Int16Array or Float64Array at SR. */
  function loopFromTick(tick16, rate, seed) {
    R = Rng(seed === undefined ? 29 : seed);
    var src = Z(tick16.length), i;
    for (i = 0; i < src.length; i++) src[i] = tick16[i] / 32768;
    var period = 1 / rate, nb = LOOP_S * rate, ph = R.uniform(0, TAU), evs = [];
    function resample(x, r) {
      var n = Math.floor(x.length / r), y = Z(n);
      for (var j = 0; j < n; j++) { var p = j * r, a = Math.floor(p), f = p - a; y[j] = (x[a] || 0) * (1 - f) + (x[a + 1] || 0) * f; }
      return y;
    }
    for (i = 0; i < nb; i++) {
      var r = (i % 2 ? 0.94 : 1) * (1 + 0.02 * R.n());
      var gain = Math.pow(10, (0.4 * Math.sin(TAU * 2 * i / nb + ph) + 0.4 * R.n()) / 20);
      evs.push([i * period + R.n() * 0.0004, scale(fit(resample(src, r), period), gain)]);
    }
    return finishLoop(circ(evs, LOOP_S * SR));
  }

  /* ---- the one-shot catalog (the pack and synth prototypes') ------------------------ */
  function westminster(ids) {
    var ch = {};
    ['G#5', 'F#5', 'E5', 'B4'].forEach(function (nm) { ch[nm] = bell(note(nm), 3.5, 0.9); });
    var phrases = { 1: 'G#5 F#5 E5 B4', 2: 'E5 G#5 F#5 B4', 3: 'E5 F#5 G#5 E5', 4: 'G#5 E5 F#5 B4', 5: 'B4 F#5 G#5 E5' };
    var ev = [], pos = 0, gap = 0.62;
    ids.forEach(function (k) {
      phrases[k].split(' ').forEach(function (nm) { ev.push([pos, ch[nm]]); pos += gap; });
      pos += gap;
    });
    return seq(ev, 3.5);
  }
  function tingtang(pairs) {
    var ting = chimeRod(note('G5'), 2.5), tang = chimeRod(note('E5'), 2.5), ev = [];
    for (var i = 0; i < pairs; i++) { ev.push([i * 1.1, ting]); ev.push([i * 1.1 + 0.42, tang]); }
    return seq(ev, 2.0);
  }
  function whistle(dur) {
    var hiss = scale(mul(bandnoise(dur, 5000, 1.5), envAR(len(dur), 0.1, 0.3)), 0.08);
    var chord = [440, 523.25, 659.26].map(function (f, i) {
      return pipe(f, dur, { attack: 0.12 + 0.03 * i, release: 0.3, slide: 0.06, slideT: 0.12, breath: 0.25, vib: 0.003, droop: 0.03 });
    });
    return mix.apply(null, chord.concat([hiss]));
  }
  function cuckooOnce() {
    var whoosh = scale(mul(bandnoise(0.12, 1200, 2), envAR(len(0.12), 0.04, 0.06)), 0.15);
    var h = [[1, 1], [2, 0.12], [3, 0.04]];
    return seq([[0, whoosh], [0.04, pipe(note('G5'), 0.28, { attack: 0.02, release: 0.06, harm: h, breath: 0.15 })],
                [0.36, pipe(note('E5'), 0.45, { attack: 0.02, release: 0.15, harm: h, breath: 0.15 })]], 0.2);
  }
  function windchimes(dur, rate) {
    var notes = ['C6', 'D6', 'E6', 'G6', 'A6'].map(note), ev = [], t = 0;
    while (t < dur) { ev.push([t, tube(notes[R.int(notes.length)], 4, R.uniform(0.3, 1.0))]); t += R.exp(1 / rate); }
    return seq(ev, 1.0);
  }
  function clink() {
    return mix(partials(3200 * R.uniform(0.97, 1.03), [[1, 1, 0.25], [1.41, 0.6, 0.18], [1.93, 0.4, 0.12], [2.6, 0.2, 0.08]], 0.6, 0.0005),
               click(6000, 2, 0.2));
  }
  function harmonica(f, dur) { return pipe(f, dur, { attack: 0.3, release: 0.5, harm: [[1, 1], [2, 0.15]], breath: 0.02, vib: 0.003 }); }
  function cabin(nm) { return partials(note(nm), [[1, 1, 1.0], [2, 0.15, 0.4], [3, 0.05, 0.2]], 1.8, 0.005); }
  function pip(d) { return sineTone(1000, d, 0.005, 0.005); }
  function chirp(f1, f2, d) {
    var n = len(d), out = Z(n), ph = 0;
    for (var i = 0; i < n; i++) { var t = i / SR; ph += TAU * f1 * Math.pow(f2 / f1, t / d) / SR; out[i] = Math.sin(ph) * Math.sin(Math.PI * t / d); }
    return out;
  }
  function toy(f) { return mix(partials(f, [[1, 1, 0.8], [2.76, 0.6, 0.25], [5.4, 0.4, 0.08]], 1.2), click(4000, 2, 0.25)); }

  /* id: [label, group, kinds, render]. kinds: which slots it fits (§7.1):
   *   c = chime (chime_q1/half/q3/hour), s = stroke (strike_*), e = event (jump_*),
   *   m = morse symbol. `raw` renders skip the speaker high-pass (none do yet). */
  var CATALOG = {
    westminster_q1: ['Westminster, first quarter', 'Westminster', 'c', function () { return westminster([1]); }],
    westminster_q2: ['Westminster, half', 'Westminster', 'c', function () { return westminster([2, 3]); }],
    westminster_q3: ['Westminster, third quarter', 'Westminster', 'c', function () { return westminster([4, 5, 1]); }],
    westminster_q4: ['Westminster, hour', 'Westminster', 'c', function () { return westminster([2, 3, 4, 5]); }],
    regulator_tingtang_1: ['Ting-tang, one pair', 'Regulator', 'cs', function () { return tingtang(1); }],
    regulator_tingtang_2: ['Ting-tang, two pairs', 'Regulator', 'c', function () { return tingtang(2); }],
    regulator_tingtang_3: ['Ting-tang, three pairs', 'Regulator', 'c', function () { return tingtang(3); }],
    regulator_tingtang_4: ['Ting-tang, four pairs', 'Regulator', 'c', function () { return tingtang(4); }],
    regulator_gong: ['Low gong rod', 'Regulator', 's', function () { return chimeRod(note('G4'), 3.5); }],
    bell_hour: ['Hour bell', 'Bells', 's', function () { return bell(note('E4'), 5.0); }],
    bell_small: ['Small bell', 'Bells', 'cs', function () { return bell(note('E5'), 2.5, 1.2); }],
    rod_hour: ['Gong rod', 'Bells', 's', function () { return chimeRod(note('G4'), 3.0); }],
    temple_bell: ['Temple bell', 'Bells', 'cs', function () { return bell(note('A4'), 6.0, 0.7, 1.5); }],
    station_minute_clunk: ['Station minute clunk', 'Station', 'e', function () {
      var ev = [];
      for (var i = 0; i < 7; i++) ev.push([0.008 * i + R.uniform(0, 0.004), click(R.uniform(1500, 3500), 6, 0.4 * Math.pow(0.7, i))]);
      var cl = tick(0.25, [[700, 6, 1.0], [1300, 8, 0.6], [2900, 12, 0.3]], 3, 45);
      return mix(scale(cl, 1 / (peak(cl) || 1)), seq(ev, 0.05));
    }],
    station_dingdong: ['Platform ding-dong', 'Station', 'c', function () { return seq([[0, bar(note('D6'))], [0.6, bar(note('A5'))]]); }],
    station_dingdong_3tone: ['Platform three-tone', 'Station', 'c', function () { return seq([[0, bar(note('A5'))], [0.45, bar(note('C#6'))], [0.9, bar(note('E6'))]]); }],
    steam_whistle: ['Steam whistle', 'Station', 'c', function () { return whistle(1.8); }],
    steam_whistle_long_long: ['Steam whistle, long-long', 'Station', 'c', function () { return seq([[0, whistle(1.4)], [1.9, whistle(1.6)]], 0.4); }],
    diesel_horn: ['Diesel horn', 'Station', 'c', function () { return mix.apply(null, ['D#5', 'F#5', 'G#5', 'B5', 'D#6'].map(function (n) { return brass(note(n), 1.6); })); }],
    departure_handbell: ['Departure handbell', 'Station', 'c', function () {
      var ev = [];
      for (var i = 0; i < 10; i++) ev.push([0.17 * i, scale(bell(note('C6') * (i % 2 ? 1 : 1.003), 1.5, 1.2, 0.4), i % 2 ? 1 : 0.8)]);
      return seq(ev, 1.0);
    }],
    ships_bell_single: ['Ship’s bell', 'Diver', 's', function () { return bell(note('D5') * (1 + 0.002 * R.n()), 3.0, 1.15, 0.7); }],
    sonar_ping: ['Sonar ping', 'Diver', 'ce', function () {
      var n = len(1.2), p = Z(n);
      for (var i = 0; i < n; i++) { var t = i / SR; p[i] = Math.sin(TAU * 1500 * (1 - 0.01 * t) * t) * Math.exp(-t / 0.35) * Math.min(1, t / 0.003); }
      return reverb(echo(p, 0.38, 0.35, 2), 1.5, 0.3);
    }],
    cuckoo_once: ['Cuckoo', 'Cuckoo', 'cs', cuckooOnce],
    windchimes_flurry: ['Wind chimes, flurry', 'Kite', 'c', function () { return windchimes(2.5, 4); }],
    windchimes_flourish: ['Wind chimes, flourish', 'Kite', 'c', function () { return windchimes(5.0, 7); }],
    teacup_clink: ['Teacup clink', 'Phase', 'ce', function () { return seq([[0, clink()], [0.13, scale(clink(), 0.7)]], 0.3); }],
    glass_harmonica: ['Glass harmonica', 'Phase', 'c', function () {
      return seq([[0, harmonica(note('A5'), 2.0)], [0.5, harmonica(note('C#6'), 1.8)], [1.0, harmonica(note('E6'), 1.6)], [1.5, harmonica(note('A6'), 1.8)]], 0.2);
    }],
    glass_bell: ['Glass bell', 'Phase', 'cs', function () { return bell(note('A6'), 3.0, 1.3, 0.6); }],
    singing_bowl: ['Singing bowl', 'Luna', 'c', function () { return bowl(note('C5')); }],
    small_bowl: ['Small bowl', 'Luna', 'c', function () { return bowl(note('G5'), 4.0); }],
    celesta_lullaby: ['Celesta lullaby', 'Luna', 'c', function () {
      return seq(['G5', 'E6', 'D6', 'B5', 'G5'].map(function (n, i) { return [0.42 * i, celesta(note(n))]; }));
    }],
    synthwave_arp: ['Synthwave arpeggio', 'Neon', 'c', function () {
      return echo(seq(['A4', 'C5', 'E5', 'A5', 'C6', 'E6'].map(function (n, i) { return [0.13 * i, sawPluck(note(n))]; }), 0.2), 0.26, 0.35, 2);
    }],
    synth_pad_bell: ['Pad and bell', 'Neon', 'c', function () {
      var n = len(4.5), pad = Z(n), env = envAR(n, 1.5, 1.5);
      ['C5', 'E5', 'G5', 'B5'].forEach(function (nm) {
        [0.9977, 1.0023].forEach(function (d) {
          var o = osc(flat(n, note(nm) * d), [[1, 1], [3, 1 / 9], [5, 1 / 25]]);
          for (var i = 0; i < n; i++) pad[i] += o[i] * env[i];
        });
      });
      return mix(scale(pad, 0.5 / (peak(pad) || 1)), seq([[1.6, scale(fm(note('E6'), 3.5, 4, 2.5), 0.5)]], 0));
    }],
    synth_bell: ['Synth bell', 'Neon', 'cs', function () { return fm(note('A5'), 3.5, 5, 3.0); }],
    marimba_q2: ['Marimba pair', 'Sector', 'c', function () { return seq([[0, bar(note('C5'), 'marimba', 1.2)], [0.22, bar(note('E5'), 'marimba', 1.2)]]); }],
    marimba_hour: ['Marimba run', 'Sector', 'c', function () {
      return seq(['C5', 'E5', 'G5', 'C6'].map(function (n, i) { return [0.22 * i, bar(note(n), 'marimba', 1.2)]; }));
    }],
    glass_dingdong: ['Glass ding-dong', 'Modern', 'c', function () { return seq([[0, bar(note('B5'))], [0.6, bar(note('G5'))]]); }],
    vibraphone_chord: ['Vibraphone chord', 'Modern', 'c', function () {
      return seq(['C5', 'E5', 'G5', 'B5', 'D6'].map(function (n, i) { return [0.06 * i, bar(note(n), 'vib', 3.0)]; }), 0.5);
    }],
    cabin_ding: ['Cabin ding', 'Aviator', 'ce', function () { return cabin('C6'); }],
    cabin_hilo: ['Cabin hi-lo', 'Aviator', 'c', function () { return seq([[0, cabin('C6')], [0.55, cabin('A5')]]); }],
    cabin_3tone: ['Cabin three-tone', 'Aviator', 'c', function () { return seq([[0, cabin('A5')], [0.45, cabin('C#6')], [0.9, cabin('E6')]]); }],
    pit_radio_blip: ['Pit radio blip', 'Racing', 'ce', function () {
      return seq([[0, scale(mul(bandnoise(0.04, 2000, 2), envAR(len(0.04), 0.003, 0.02)), 0.3)],
                  [0.05, sineTone(1800, 0.08)], [0.16, sineTone(1400, 0.08)]], 0.1);
    }],
    race_start: ['Race start', 'Racing', 'c', function () {
      return seq([[0, square(880, 0.15)], [1, square(880, 0.15)], [2, square(880, 0.15)], [3, square(1760, 0.8)]], 0.2);
    }],
    orbit_arp: ['Orbit arpeggio', 'Orbital', 'c', function () {
      return seq(['C6', 'E6', 'G6', 'B6', 'D7'].map(function (x, i) { return [0.11 * i, celesta(note(x) * (1 + 0.006 * Math.sin(Math.PI * i / 4)))]; }), 0.3);
    }],
    music_box: ['Music box', 'Cute', 'c', function () {
      return seq(['E6', 'G6', 'A6', 'G6', 'E6', 'D6', 'C6', 'D6', 'E6'].map(function (n, i) {
        return [0.28 * i + R.n() * 0.012, tine(note(n) * (1 + 0.002 * R.n()))];
      }));
    }],
    kalimba: ['Kalimba', 'Cute', 'c', function () {
      return seq(['C5', 'E5', 'G5', 'E5', 'A5', 'G5'].map(function (n, i) { return [0.25 * i, tine(note(n), 1.2, [[1, 1, 0.9], [5.9, 0.25, 0.12]])]; }));
    }],
    toy_piano_twinkle: ['Toy piano', 'Cute', 'c', function () {
      return seq(['G5', 'G5', 'D6', 'D6', 'E6', 'E6', 'D6'].map(function (n, i) { return [0.3 * i + (i === 6 ? 0.3 : 0), toy(note(n))]; }));
    }],
    glockenspiel: ['Glockenspiel', 'Cute', 'c', function () {
      return seq(['C6', 'E6', 'G6', 'C7'].map(function (n, i) { return [0.12 * i, bar(note(n), 'glock', 2.0)]; }));
    }],
    owl_hoot: ['Owl', 'Cute', 'c', function () {
      var hoo = function (d) { return pipe(470, d, { attack: 0.08, release: 0.2, harm: [[1, 1], [2, 0.08]], breath: 0.25, droop: 0.04 }); };
      return seq([[0, hoo(0.45)], [0.95, hoo(0.3)], [1.35, hoo(0.5)]], 0.3);
    }],
    dawn_chirp: ['Dawn chirp', 'Cute', 'c', function () {
      var ev = [], i;
      for (i = 0; i < 4; i++) ev.push([0.25 * i, chirp(5500 * R.uniform(0.9, 1.1), 3000, 0.06)]);
      for (i = 0; i < 6; i++) ev.push([1.1 + 0.05 * i, scale(chirp(4500, 3800, 0.035), 0.6)]);
      return seq(ev, 0.2);
    }],
    tingsha: ['Tingsha', 'Cute', 'cs', function () { return mix(bowl(2900, 5.0), bowl(2900 * 1.004, 5.0)); }],
    fm_glass_bell: ['FM glass bell', 'Future', 'cs', function () { return fm(note('A5'), 3.5, 5, 3.0); }],
    time_pips: ['Time pips', 'Future', 'c', function () {
      var ev = [];
      for (var i = 0; i < 5; i++) ev.push([i, pip(0.1)]);
      ev.push([5, pip(0.5)]);
      return seq(ev, 0.2);
    }],
    boot_arpeggio: ['Boot arpeggio', 'Future', 'c', function () {
      return echo(seq(['C5', 'G5', 'C6', 'E6', 'G6', 'C7'].map(function (n, i) { return [0.07 * i, fm(note(n), 2.0, 1.5, 0.35, 0.1, 0.12)]; }), 0.2), 0.21, 0.4, 2);
    }],
    shepard_swell: ['Shepard swell', 'Future', 'c', function () {
      var n = len(4.5), out = Z(n), env = envAR(n, 0.8, 1.0);
      for (var kk = -2; kk < 8; kk++) {
        var ph = 0;
        for (var i = 0; i < n; i++) {
          var t = i / SR, f = 120 * Math.pow(2, kk) * Math.pow(2, 0.25 * t);
          ph += TAU * f / SR;
          if (f >= SR * 0.45) continue;
          var w = Math.exp(-0.5 * Math.pow((Math.log2(f) - Math.log2(1400)) / 1.0, 2));
          out[i] += w * Math.sin(ph) * env[i];
        }
      }
      return out;
    }],
    hologram_shimmer: ['Hologram shimmer', 'Future', 'c', function () {
      var n = len(3.5), out = Z(n), env = envAR(n, 1.0, 1.8);
      [0, 4, 7, 11, 12, 14, 16, 19].forEach(function (u) {
        var fw = wobble(n, 0.002), aw = wobble(n, 0.4, [0.7, 1.3, 2.1]), ph = 0;
        for (var i = 0; i < n; i++) { ph += TAU * 1800 * Math.pow(2, u / 12) * fw[i] / SR; out[i] += Math.sin(ph) * aw[i] * env[i]; }
      });
      return out;
    }],
    door_whoosh_chime: ['Door whoosh', 'Future', 'c', function () {
      var n = len(0.6), y = Z(n), y1 = 0, y2 = 0, env = envAR(n, 0.25, 0.3);
      for (var i = 0; i < n; i++) {
        var f = 600 * Math.pow(4000 / 600, i / n), w = TAU * f / SR, al = Math.sin(w) / 6, a0 = 1 + al;
        var o = (al * R.n() - (-2 * Math.cos(w)) * y1 - (1 - al) * y2) / a0;
        y2 = y1; y1 = o; y[i] = o * env[i];
      }
      return mix(scale(y, 0.6 / (peak(y) || 1)), seq([[0.45, fm(note('E6'), 3.5, 3, 2.0)]], 0));
    }],
    chiptune_fanfare: ['Chiptune fanfare', 'Future', 'c', function () {
      var ev = [], t = 0;
      [['C5', 0.1], ['E5', 0.1], ['G5', 0.1], ['C6', 0.3], ['G5', 0.1], ['C6', 0.6]].forEach(function (x) { ev.push([t, square(note(x[0]), x[1], 0.25)]); t += x[1] + 0.01; });
      return seq(ev, 0.2);
    }],
    sine_blip: ['Sine blip', 'Future', 'e', function () { return hann(1600, 0.012); }],
    /* the morse pattern's symbols at 20 wpm: a 60 ms dit, a dah three times it */
    morse_dit: ['Morse dit (750 Hz)', 'Morse', 'm', function () { return sineTone(750, 0.06, 0.004, 0.004); }],
    morse_dah: ['Morse dah (750 Hz)', 'Morse', 'm', function () { return sineTone(750, 0.18, 0.004, 0.004); }]
  };

  function renderSound(id, seed) {
    var c = CATALOG[id];
    if (!c) throw new Error('no catalog sound ' + JSON.stringify(id));
    R = Rng(seed === undefined ? 11 : seed);
    var x = c[3]();
    /* Morse symbols keep their exact length: the pattern's timing is their length. */
    if (c[2] === 'm') return toPcm(highpass(mean0(x)));
    return finish(x);
  }

  /* The kinds a slot takes (§7.1), and which catalog entries fit it. */
  var SLOT_KIND = { chime_q1: 'c', chime_half: 'c', chime_q3: 'c', chime_hour: 'c', strike_hour: 's',
                    strike_qtr: 's', strike_min: 's', jump_minute: 'e', jump_hour: 'e', jump_date: 'e',
                    morse_dit: 'm', morse_dah: 'm' };
  function fits(slot) {
    var k = SLOT_KIND[slot];
    return Object.keys(CATALOG).filter(function (id) { return CATALOG[id][2].indexOf(k) >= 0; });
  }

  /* §7.2: each preset's default. tick: a VOICES id or null; pattern: an hour pattern;
   * slots: slot -> CATALOG id. A slot not named is silent. */
  function W4(extra) {
    var o = { chime_q1: 'westminster_q1', chime_half: 'westminster_q2', chime_q3: 'westminster_q3',
              chime_hour: 'westminster_q4', strike_hour: 'bell_hour' };
    for (var k in extra || {}) o[k] = extra[k];
    return o;
  }
  var PRESETS = {
    skeleton:  { tick: 'classic', pattern: 'count', slots: W4() },
    calendar:  { tick: 'mantel', pattern: 'count', slots: W4({ jump_date: 'teacup_clink' }) },
    modern:    { tick: 'wrist', pattern: 'none', slots: { chime_hour: 'glass_dingdong' } },
    sector:    { tick: 'woodblock', pattern: 'none', slots: { chime_q1: 'marimba_q2', chime_half: 'marimba_q2', chime_q3: 'marimba_q2', chime_hour: 'marimba_hour' } },
    diver:     { tick: 'wrist', pattern: 'ships_bells', slots: { strike_hour: 'ships_bell_single' } },
    orbital:   { tick: 'tourbillon', pattern: 'none', slots: { chime_hour: 'orbit_arp' } },
    aviator:   { tick: 'wrist', pattern: 'none', slots: { chime_half: 'cabin_hilo', chime_hour: 'cabin_3tone' } },
    opentour:  { tick: 'tourbillon', pattern: 'none', slots: { chime_half: 'pit_radio_blip', chime_hour: 'race_start' } },
    kitetour:  { tick: 'tourbillon', pattern: 'none', slots: { chime_q1: 'windchimes_flurry', chime_half: 'windchimes_flurry', chime_q3: 'windchimes_flurry', chime_hour: 'windchimes_flourish' } },
    station:   { tick: null, pattern: 'none', slots: { chime_hour: 'station_dingdong_3tone', jump_minute: 'station_minute_clunk' } },
    luna:      { tick: 'soft', pattern: 'none', slots: { chime_q1: 'small_bowl', chime_half: 'small_bowl', chime_q3: 'small_bowl', chime_hour: 'singing_bowl' } },
    neon:      { tick: 'neon_buzz', pattern: 'none', slots: { chime_half: 'synth_bell', chime_hour: 'synth_pad_bell' } },
    phase:     { tick: 'soft', pattern: 'count', slots: { chime_q1: 'teacup_clink', chime_half: 'teacup_clink', chime_q3: 'teacup_clink', chime_hour: 'glass_bell', strike_hour: 'glass_bell' } },
    regulator: { tick: 'regulator', pattern: 'count', slots: { chime_q1: 'regulator_tingtang_1', chime_half: 'regulator_tingtang_2', chime_q3: 'regulator_tingtang_3', chime_hour: 'regulator_tingtang_4', strike_hour: 'regulator_gong' } },
    perpetual: { tick: 'pocket', pattern: 'count', slots: W4({ jump_date: 'teacup_clink' }) }
  };

  root.SoundEngine = {
    SR: SR, LOOP_S: LOOP_S, RATES: RATES, HP: HP,
    VOICES: Object.keys(VOICES).reduce(function (o, k) { o[k] = { label: VOICES[k][0], rates: voiceRates(k) }; return o; }, {}),
    CATALOG: Object.keys(CATALOG).reduce(function (o, k) { o[k] = { label: CATALOG[k][0], group: CATALOG[k][1], kinds: CATALOG[k][2] }; return o; }, {}),
    SLOT_KIND: SLOT_KIND, PRESETS: PRESETS,
    renderLoop: renderLoop, renderSound: renderSound, loopFromTick: loopFromTick, fits: fits,
    voiceRates: voiceRates, highpass: highpass, finish: finish, finishLoop: finishLoop
  };
})(typeof window !== 'undefined' ? window : (typeof globalThis !== 'undefined' ? globalThis : this));
if (typeof module !== 'undefined' && module.exports) module.exports = (typeof window !== 'undefined' ? window : globalThis).SoundEngine;
