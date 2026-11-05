#!/usr/bin/env node
/* estimate_spans.js — the span compositor's price of a baked clock theme (plan §49.6).
 *
 * estimate3 (the baker's) prices the shipped swept-box path. This prices the path the
 * §49 bench measured: ink-span marks and paint, nearest shadows, 80 MHz, from the bake
 * alone, by doing what the firmware does:
 *
 *   - every AUX row's alpha plane is reduced to [umin, umax] per 8-row strip and each
 *     strip forward-mapped to a quad at the pose, cut into one padded [x0, x1] per screen
 *     row (orb_gfx.c gfx_spans_of, padded 3 texels, +1 px);
 *   - a tick restores, per screen row, the union of every mover's old and new spans and
 *     the bank rect that lands that tick (one [x0, x1] per row, as gfx_mark_rows keeps it);
 *   - every rotated row then walks its own spans inside the restored rows.
 *
 * The cost per commit (compositing + dma + TE), fitted on THIS file's own restored and
 * walk figures over all 18 templates x 2 arms at 80 MHz (§49.6; median error 9%, p90
 * 20%, Flux's ~15 ms commits the one outlier). A carriage dial restores more than its
 * spans (the stage marks it by chords of both frames), hence its own term:
 *   commit_ms = 0.734 R + 0.323 W + 0.676 R (carriage dials) + 20.85
 * 20.85 carries the unpaced TE wait (mean 8.7 ms); a paced present pays its own.
 *
 * Usage: estimate_spans.js BAKER THEME [--step deg] [--gear-every N] [--json]
 *   --step        the seconds row's turn per tick, degrees (default 6 = a 1 bps step)
 *   --gear-every  a bank frame lands every N ticks (default: priced as its own tick)
 * Prints the hand tick and the gear tick separately: the grid bench needs both.
 */
'use strict';
const fs = require('fs');

const W = 466, H = 466, STRIP = 8, PAD = 3;
const K = { R: 0.734, WALK: 0.323, CAR: 0.676, FIXED: 20.85, TE: 8.7 };
const SRC = { none: 0, hour12: 4, hour24: 5, minute: 6, second: 7 };
const AUX = 8, PLATE = 3, SHADOW = 1 << 4, HIDDEN = 8, DRUM = 3;

function inkStrips(art, sp) {
  const a0 = sp.off + sp.w * sp.h * 2, ns = Math.ceil(sp.h / STRIP), out = [];
  for (let s = 0; s < ns; s++) {
    let lo = 1e9, hi = -1;
    for (let v = s * STRIP; v < Math.min(sp.h, (s + 1) * STRIP); v++)
      for (let u = 0; u < sp.w; u++) if (art[a0 + v * sp.w + u]) { if (u < lo) lo = u; if (u > hi) hi = u; }
    out.push([lo, hi]);
  }
  return out;
}

/* gfx_rot_sin_cos's angle convention: screen = centre + R(ang)^-1 (src - pivot). */
function spans(row, ang10) {
  const x0 = new Int16Array(H).fill(32767), x1 = new Int16Array(H).fill(-1);
  const t = -(ang10 % 3600) / 10 * Math.PI / 180, sn = Math.sin(t), cs = Math.cos(t);
  const cx = row.e.x + row.e.pivot_x, cy = row.e.y + row.e.pivot_y, px = row.e.pivot_x, py = row.e.pivot_y;
  row.ink.forEach(([lo, hi], s) => {
    if (hi < lo) return;
    const u0 = lo - PAD - px, u1 = hi + 1 + PAD - px;
    const v0 = s * STRIP - PAD - py, v1 = Math.min(row.sp.h, (s + 1) * STRIP) + PAD - py;
    const q = [[u0, v0], [u1, v0], [u1, v1], [u0, v1]].map(([u, v]) => [cx + cs * u + sn * v, cy - sn * u + cs * v]);
    const ys = q.map((p) => p[1]);
    const ya = Math.max(0, Math.floor(Math.min(...ys))), yb = Math.min(H - 1, Math.floor(Math.max(...ys)));
    for (let y = ya; y <= yb; y++) {
      let mn = 1e9, mx = -1e9;
      for (let e = 0; e < 4; e++) {
        const [ax, ay] = q[e], [bx, by] = q[(e + 1) & 3];
        const lo2 = Math.max(Math.min(ay, by), y), hi2 = Math.min(Math.max(ay, by), y + 1);
        if (lo2 > hi2) continue;
        const k = by !== ay ? (bx - ax) / (by - ay) : 0;
        const xa = by !== ay ? ax + (lo2 - ay) * k : ax, xb = by !== ay ? ax + (hi2 - ay) * k : bx;
        mn = Math.min(mn, xa, xb); mx = Math.max(mx, xa, xb);
      }
      if (mn > mx) continue;
      const a = Math.max(0, Math.floor(mn) - 1), b = Math.min(W - 1, Math.floor(mx) + 2);
      if (a < x0[y]) x0[y] = a;
      if (b > x1[y]) x1[y] = b;
    }
  });
  return { x0, x1 };
}

function main() {
  const argv = process.argv.slice(2);
  const G = require(argv[0]);
  const opt = (n, d) => { const i = argv.indexOf(n); return i < 0 ? d : +argv[i + 1]; };
  const step = opt('--step', 6), every = opt('--gear-every', 0);
  const sec = G.verifyBundle3(new Uint8Array(fs.readFileSync(argv[1]))).sections[0];
  const els = sec.blob.elements, spr = sec.blob.sprites, art = sec.art;

  /* The pose at 10:08:30 (the hands apart, the classic display time), the seconds swept. */
  const T = { hour: (10 + 8.5 / 60) / 12, minute: 8.5 / 60, second: 30 / 60 };
  const rows = [];
  let lastSrc = 0;
  for (const e of els) {
    if (e['class'] !== AUX || (e.flags & HIDDEN)) continue;
    const twin = !!(e.flags & SHADOW);
    const src = twin ? lastSrc : e.src;
    if (!twin) lastSrc = e.src;
    const sp = spr[e.frame0];
    const frac = src === SRC.second ? T.second : src === SRC.minute ? T.minute
               : (src === SRC.hour12 || src === SRC.hour24) ? T.hour : 0;
    const ang = ((e.zero | 0) + Math.round(frac * (e.span | 0))) % 3600;
    rows.push({ e, sp, src, twin, ang, drum: e.kind === DRUM, ink: e.kind === DRUM ? [] : inkStrips(art, sp),
                carriage: e.n_frames > 1 && e.delay > 0 });
  }
  const plate = els.find((e) => e['class'] === PLATE && !(e.slot & 0xffff));
  const bank = plate && plate.n_frames > 1 ? Array.from({ length: plate.n_frames - 1 }, (_, i) => spr[plate.frame0 + 1 + i]) : [];
  const bankFps = plate && plate.delay ? 1000 / plate.delay : 0;

  const priceTick = (withBank) => {
    let R = 0, Wk = 0, n = 0;
    const samples = 12;
    for (let k = 0; k < samples; k++) {
      const rx0 = new Int16Array(H).fill(32767), rx1 = new Int16Array(H).fill(-1);
      const mark = (s) => { for (let y = 0; y < H; y++) if (s.x0[y] <= s.x1[y]) { if (s.x0[y] < rx0[y]) rx0[y] = s.x0[y]; if (s.x1[y] > rx1[y]) rx1[y] = s.x1[y]; } };
      const secAng = (k * 300) % 3600;                 // the seconds round the dial
      const pose = rows.map((r) => r.src === SRC.second ? (r.ang + secAng) % 3600 : r.ang);
      rows.forEach((r, i) => {
        if (r.drum) { const w2 = r.e.radius | 0; const y0 = r.e.y + r.e.pivot_y - w2;
          for (let y = Math.max(0, y0); y < Math.min(H, y0 + 2 * w2); y++) { rx0[y] = Math.min(rx0[y], r.e.x); rx1[y] = Math.max(rx1[y], r.e.x + r.e.w - 1); } return; }
        if (r.src !== SRC.second) return;
        mark(spans(r, pose[i]));
        mark(spans(r, (pose[i] + Math.round(step * 10)) % 3600));
      });
      if (withBank && bank.length) {
        const b = bank[k % bank.length];
        for (let y = b.diffy; y < b.diffy + b.h && y < H; y++) { rx0[y] = Math.min(rx0[y], b.diffx); rx1[y] = Math.max(rx1[y], b.diffx + b.w - 1); }
      }
      let r = 0;
      for (let y = 0; y < H; y++) if (rx0[y] <= rx1[y]) r += rx1[y] - rx0[y] + 1;
      let w = 0;
      rows.forEach((row, i) => {
        if (row.drum || pose[i] % 3600 === 0) return;  // identity rows take the static path
        const s = spans(row, row.src === SRC.second ? (pose[i] + Math.round(step * 10)) % 3600 : pose[i]);
        for (let y = 0; y < H; y++) {
          const a = Math.max(s.x0[y], rx0[y]), b = Math.min(s.x1[y], rx1[y]);
          if (b >= a) w += b - a + 1;
        }
      });
      R += r; Wk += w; n++;
    }
    R /= n * 1000; Wk /= n * 1000;
    const car = rows.some((r) => r.carriage) ? K.CAR * R : 0;
    const ms = K.R * R + K.WALK * Wk + car + K.FIXED;
    return { restored_kpx: +R.toFixed(1), walk_kpx: +Wk.toFixed(1),
             commit_ms: +ms.toFixed(1), paced_ms: +(ms - K.TE).toFixed(1) };
  };
  const out = { hand: priceTick(false), gear: bank.length ? priceTick(true) : null,
                bank_fps: +bankFps.toFixed(2), bank_frames: bank.length, carriage: rows.some((r) => r.carriage),
                movers: rows.filter((r) => r.src === SRC.second).length, rows: rows.length };
  if (argv.includes('--json')) process.stdout.write(JSON.stringify(out));
  else console.log(JSON.stringify(out, null, 1));
}
main();
