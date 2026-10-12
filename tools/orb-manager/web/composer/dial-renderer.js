// Procedural steampunk skeleton dial, 466x466. Seamless loop: every wheel advances
// exactly K teeth per loop, and spoke/pinion counts are N/K multiples, so the
// per-loop rotation is a symmetry of every rotating part. Lighting is world-fixed.
// K must stay coprime with every bank length (8/16/24/15 frames), or a step of a
// whole number of teeth makes the rim identical to itself and the teeth read as
// still. K = 4 was the worst case: on a 1:1 bank at 4 fps it stepped exactly one
// tooth and only the spokes crawled. K = 7 is coprime with all four, so every
// frame is distinct. Teeth are therefore free to be as fine as they look best:
// tooth size follows the module, and the module follows the pitch radius m*N/2,
// so K = 7 lets every train keep its size at a much finer tooth (watch-engine
// TRAINS).
// Distinct frames are NOT unambiguous ones, and the first K = 7 build got that
// wrong: coprimality guarantees no frame repeats, but an unmarked rim repeats
// every single tooth, so a tick stepping 0.875 teeth is pixel-identical to one
// stepping -0.125 and the eye takes the slower reading — the rim crawled
// backwards while the spokes (period K) crawled correctly forward. Measured on
// the 21-tooth pinion at the shipped module: at an 8-frame bank the reversed
// reading costs 0 px of mismatch with no mark, and 58 px with the index tooth
// present. So the index tooth is keyed on the STEP, not on coprimality:
// markPeriod engages it whenever one tick turns more than half a tooth, which
// these bank depths leave to the 4 fps tier (7/8) alone. The 8 and 12 fps banks
// are deep enough to step under half a tooth by themselves, and 15 fps sits just
// under at 7/15. loopcheck.py rasterises the rim and asserts both the freeze and
// the direction, so a bank or a K that breaks either one fails the suite.
(function () {
  const S = 466, C = 233, WIN = 160, K = 7, TAU = Math.PI * 2;
  const TEMPOS = [4, 8, 12, 15];
  // Bank depth per tempo. The device advances the background bank exactly one
  // frame per tick (clock_view.c: gif_tick_ms() = clk_gif_delays[0] = 1000/the
  // bank's own fps), so a deeper bank plays the SAME loop more slowly: each tick
  // turns half as far and dirties fewer rows than the 1:1 bank the row caps were
  // measured against. 8/16/24 frames at 4/8/12 fps holds the train at half its
  // old speed, the cadence the watch actually reads best; 15 stays 1:1, where
  // the step is already 7/15 of a tooth.
  const BANKS = { 4: 8, 8: 16, 12: 24, 15: 15 };
  const bankFrames = (fps) => BANKS[fps] || fps;
  // A rim that repeats every `period` pitches reads its motion honestly only
  // while one tick steps at most half a period. Past that the frames are
  // pixel-identical to a step of (step - period) running the other way, and the
  // eye always picks the slower reading. This is why the index tooth exists, and
  // why it is keyed on the STEP and not on coprimality: coprime frames are
  // DISTINCT, which is not the same as UNAMBIGUOUS.
  const aliases = (loopTeeth, frames) => Math.abs(loopTeeth / (frames || 1)) > 0.5;
  let BANKF = 15;                                        // frames in the bank being rendered
  // The index tooth: every K-th tooth is filed short and narrow. The mark may
  // only move INWARD -- a meshing pair leaves just 0.25 * m of tip-to-root
  // clearance, so a taller marked tooth would overlap the wheel it drives.
  // loopcheck.py measures the pixels this changes per tick and fails the kit if
  // it drops to none, which is the frozen-rim bug these numbers exist to stop.
  const MK_DEEP = 0.30, MK_TIP = 0.045, MK_BASE = 0.255;
  const FIN = {
    brass: { hi: '#f6dc96', mid: '#c99b47', lo: '#5e3e14', edge: '#2e1d08' },
    copper: { hi: '#f4bb92', mid: '#b5653a', lo: '#4f2410', edge: '#260f05' },
    gunmetal: { hi: '#d4d7db', mid: '#7f858c', lo: '#2c3034', edge: '#121416' },
    rhodium: { hi: '#f7f9fb', mid: '#b3b9c0', lo: '#4a5057', edge: '#1a1c1f' },
  };
  const BACK = { hi: '#a8743f', mid: '#5f3a1c', lo: '#22130a', edge: '#0d0703' };
  const STEEL = { hi: '#8d939b', mid: '#4a4f56', lo: '#17191c', edge: '#08090a' };
  const ENAMEL = {
    aged: { a: '#efe5cb', b: '#c9b388', ink: '#1f150c', stain: 1 },
    clean: { a: '#f8f4ea', b: '#e0d9c8', ink: '#16130f', stain: 0 },
    black: { a: '#2a2621', b: '#13110e', ink: '#e8d9b0', stain: 0, dark: 1 },
    agedBlack: { a: '#2c2823', b: '#0f0d0a', ink: '#d9c79b', stain: 1, dark: 1 },
  };
  function rng(seed) {
    return function () {
      seed |= 0; seed = (seed + 0x6d2b79f5) | 0;
      let t = Math.imul(seed ^ (seed >>> 15), 1 | seed);
      t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
      return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
  }
  const P = (x, y, r, a) => [x + Math.cos(a) * r, y + Math.sin(a) * r];
  function canvas(w, h) { const c = document.createElement('canvas'); c.width = w; c.height = h || w; return c; }

  function chain(defs, m, dir, rot) {
    rot = rot || 0;
    const out = [];
    defs.forEach((d, i) => {
      const g = { N: d.N, m, rp: (m * d.N) / 2, s: d.N / K, pin: (d.N / K) * (d.N / K < 6 ? 2 : 1) };
      if (i === 0) { const c = Math.cos(rot), s = Math.sin(rot); g.x = C + (d.x - C) * c - (d.y - C) * s; g.y = C + (d.x - C) * s + (d.y - C) * c; g.p = (d.p || 0) + rot; g.v = (dir * TAU * K) / d.N; }
      else {
        const pa = out[d.parent], a = (d.ang * Math.PI) / 180 + rot, dist = (m * (pa.N + d.N)) / 2, r = pa.N / d.N;
        g.x = pa.x + Math.cos(a) * dist; g.y = pa.y + Math.sin(a) * dist;
        g.p = a + Math.PI + Math.PI / d.N - r * (pa.p - a); g.v = -r * pa.v;
      }
      out.push(g);
    });
    return out;
  }
  let FD = [
    { x: 178, y: 193, N: 56, p: 0.1 },
    { parent: 0, ang: -20, N: 42 },
    { parent: 0, ang: 120, N: 35 },
    { parent: 1, ang: 80, N: 28 },
    { parent: 2, ang: 30, N: 42 },
  ], RD = [
    { x: 270, y: 250, N: 56, p: 0.3 },
    { parent: 0, ang: 200, N: 35 },
    { parent: 0, ang: -70, N: 42 },
  ];
  let FM = 2.3, RM = 3.15, TRN = null;
  const FRONT0 = chain(FD, FM, 1), REAR0 = chain(RD, RM, -1), FRONT = chain(FD, FM, 1), REAR = chain(RD, RM, -1);
  const BAL0 = { x: 206, y: 98, r: 34 }, BAL = { x: 206, y: 98, r: 34, rot: 0 };
  let ROT = 0;
  // rigid rotation of the whole train about the dial centre; meshing and loop symmetry are preserved
  function setRot(a) {
    if (a === ROT) return; ROT = a;
    FRONT.splice(0, FRONT.length, ...chain(FD, FM, 1, a)); REAR.splice(0, REAR.length, ...chain(RD, RM, -1, a));
    BAL.r = BAL0.r;
    const c = Math.cos(a), s = Math.sin(a);
    BAL.x = C + (BAL0.x - C) * c - (BAL0.y - C) * s; BAL.y = C + (BAL0.x - C) * s + (BAL0.y - C) * c; BAL.rot = a;
  }
  // swap in a different wheel layout; FRONT/REAR keep their identity so callers holding them stay valid
  function setTrain(t) {
    if (!t || t === TRN) return; TRN = t;
    FD = t.front; FM = t.fm || 4; RD = t.rear || []; RM = t.rm || 5.5;
    FRONT0.splice(0, FRONT0.length, ...chain(FD, FM, 1)); REAR0.splice(0, REAR0.length, ...chain(RD, RM, -1));
    BAL0.x = t.bal.x; BAL0.y = t.bal.y; BAL0.r = t.bal.r || 34;
    const a = ROT; ROT = NaN; setRot(a);
  }
  function hueMul(h, s) {
    s = Math.max(0, Math.min(1, +s || 0)); if (!s) return [1, 1, 1];
    const f = (n) => { const k = (n + (+h || 0) / 30) % 12; return 0.5 - 0.5 * Math.max(-1, Math.min(k - 3, 9 - k, 1)); };
    return [f(0), f(8), f(4)].map((v) => 1 - s + s * v);
  }
  function setTemp(k, h, s) { const hm = hueMul(h, s); LT = kelvin(+k || 2200).map((v, i) => (v / BASEK[i]) * hm[i]); }

  let LT = [1, 1, 1];
  function kelvin(k) {
    const t = k / 100; let r, g, b;
    r = t <= 66 ? 255 : 329.7 * Math.pow(t - 60, -0.1332);
    g = t <= 66 ? 99.47 * Math.log(t) - 161.12 : 288.12 * Math.pow(t - 60, -0.0755);
    b = t >= 66 ? 255 : t <= 19 ? 0 : 138.52 * Math.log(t - 10) - 305.04;
    return [r, g, b].map((v) => Math.max(1, Math.min(255, v)));
  }
  const BASEK = kelvin(2200);
  // re-tint a warm light colour (authored at ~2200K) to the chosen temperature
  function tint(rgb) {
    const c = rgb.split(',').map(Number), out = c.map((v, i) => v * LT[i]), mx = Math.max(255, ...out);
    return out.map((v) => Math.round((v / mx) * 255)).join(',');
  }
  let GLOW = 1, BEATS = 1, SPOKE = 'arched', ETCH = 'none';
  function grad(ctx, x, y, r, pal) {
    const g = ctx.createLinearGradient(x - r, y - r, x + r * 0.8, y + r);
    g.addColorStop(0, pal.hi); g.addColorStop(0.42, pal.mid); g.addColorStop(1, pal.lo);
    return g;
  }
  function shadow(ctx, blur, ox, oy, a) {
    ctx.shadowColor = `rgba(0,0,0,${a})`; ctx.shadowBlur = blur; ctx.shadowOffsetX = ox; ctx.shadowOffsetY = oy;
  }
  // A rim repeating every single tooth is blind to a whole-tooth step, and to a
  // step of any whole number of teeth: the frames come out pixel-identical to a
  // step running the OTHER way by (step - one tooth), and the eye always takes
  // the slower reading. So the index tooth is engaged by the STEP, not by
  // coprimality -- coprime frames are distinct, which was the old freeze bug,
  // but distinct frames can still read as reverse motion, which was the bug that
  // followed it. A mark must repeat a whole number of times both around the gear
  // and over the per-loop advance, or the last frame would not return to the
  // first and the loop would pop once per bank; markPeriod enforces both, and
  // returns 1 (no mark) whenever the step already sits under half a tooth.
  function markPeriod(loopTeeth, copies) {
    if (!aliases(loopTeeth, BANKF)) return 1;
    if (!Number.isInteger(loopTeeth)) return 1;
    for (const d of [K, 2]) if (loopTeeth % d === 0 && copies % d === 0) return d;
    return 1;
  }
  function teethPath(ctx, x, y, N, rr, ra, th, mp) {
    const pitch = TAU / N, lo = ra - MK_DEEP * (ra - rr), mark = (i) => mp > 1 && ((i % mp) === mp - 1);
    for (let i = 0; i < N; i++) {
      const a = th + i * pitch, mk = mark(i), nx = mark(i + 1);
      const tip = mk ? lo : ra, ld = mk ? MK_TIP : 0.13, bs = mk ? MK_BASE : 0.3, nb = nx ? MK_BASE : 0.3;
      const p1 = P(x, y, rr, a - bs * pitch), p2 = P(x, y, tip, a - ld * pitch), p3 = P(x, y, tip, a + ld * pitch), p4 = P(x, y, rr, a + bs * pitch);
      ctx.lineTo(p1[0], p1[1]); ctx.lineTo(p2[0], p2[1]); ctx.lineTo(p3[0], p3[1]); ctx.lineTo(p4[0], p4[1]);
      ctx.arc(x, y, rr, a + bs * pitch, a + pitch - nb * pitch);
    }
    ctx.closePath();
  }
  function jewel(ctx, x, y, gold) {
    ctx.save(); shadow(ctx, 3, 1, 1.5, 0.6);
    let g = ctx.createRadialGradient(x - 2, y - 2, 1, x, y, 8);
    g.addColorStop(0, gold.hi); g.addColorStop(1, gold.lo);
    ctx.fillStyle = g; ctx.beginPath(); ctx.arc(x, y, 7, 0, TAU); ctx.fill(); ctx.restore();
    g = ctx.createRadialGradient(x - 1.2, y - 1.2, 0.3, x, y, 4.2);
    g.addColorStop(0, '#ff8a8a'); g.addColorStop(0.5, '#b4151f'); g.addColorStop(1, '#4a0509');
    ctx.fillStyle = g; ctx.beginPath(); ctx.arc(x, y, 4, 0, TAU); ctx.fill();
    ctx.fillStyle = 'rgba(255,240,240,.85)'; ctx.beginPath(); ctx.arc(x - 1.3, y - 1.3, 0.9, 0, TAU); ctx.fill();
  }
  function screw(ctx, x, y, r, slot) {
    ctx.save(); shadow(ctx, 2, 1, 1, 0.6);
    const g = ctx.createRadialGradient(x - r * 0.4, y - r * 0.4, 0.5, x, y, r);
    g.addColorStop(0, '#7aa0d8'); g.addColorStop(0.6, '#2c4c86'); g.addColorStop(1, '#0d1a33');
    ctx.fillStyle = g; ctx.beginPath(); ctx.arc(x, y, r, 0, TAU); ctx.fill(); ctx.restore();
    const a = P(x, y, r * 0.8, slot), b = P(x, y, r * 0.8, slot + Math.PI);
    ctx.strokeStyle = '#06080d'; ctx.lineWidth = 1.4; ctx.beginPath(); ctx.moveTo(a[0], a[1]); ctx.lineTo(b[0], b[1]); ctx.stroke();
  }

  const OFF = 3000;
  function lobe(t, c, sg) { let d = Math.abs(t - c) % TAU; if (d > Math.PI) d = TAU - d; return Math.exp(-0.5 * (d / sg) * (d / sg)); }
  let SPEC = null;
  // soft specular on a rotating gear: world-fixed angular lobes (3 & 9 o'clock) x radial profile, masked by the gear's face
  function gearSpec(ctx, g, th, pal, rear) {
    if (!SPEC) SPEC = canvas(S);
    const lx = SPEC.getContext('2d'), { x, y, m, rp } = g, ra = rp + m, rr = rp - 1.25 * m, ri = rr - 2.2 * m, rh = Math.max(7, rp * 0.2);
    const wl = sideW(x), wr = 1 - wl, amt = (rear ? 0.35 : 0.85) * Math.min(1.8, GLOW);
    lx.setTransform(1, 0, 0, 1, 0, 0); lx.globalCompositeOperation = 'source-over'; lx.globalAlpha = 1; lx.filter = 'none';
    lx.clearRect(0, 0, S, S);
    const cg = lx.createConicGradient(0, x, y);
    for (let i = 0; i <= 120; i++) {
      const t = (i / 120) * TAU;
      const v = (0.25 + 0.75 * wr) * lobe(t, 0, 0.36) + (0.25 + 0.75 * wl) * lobe(t, Math.PI, 0.36) + 0.22 * lobe(t, Math.PI * 1.3, 0.3);
      cg.addColorStop(i / 120, 'rgba(' + tint('255,' + Math.round(196 + 30 * lobe(t, Math.PI * 1.3, 0.3)) + ',122') + ',' + Math.min(1, v * amt) + ')');
    }
    lx.fillStyle = cg; lx.beginPath(); lx.arc(x, y, ra + 3, 0, TAU); lx.fill();
    lx.globalCompositeOperation = 'destination-in';
    const rg = lx.createRadialGradient(x, y, 0, x, y, ra + 1);
    const rim = (ri + rr) / 2 / (ra + 1), hub = rh / (ra + 1);
    [[0, 0.25], [hub * 0.6, 0.85], [hub, 0.45], [hub + 0.04, 0.2], [ri / (ra + 1), 0.35], [rim, 1], [rr / (ra + 1), 0.75], [1, 0.35]]
      .forEach(([p, a]) => rg.addColorStop(Math.min(1, p), 'rgba(0,0,0,' + a + ')'));
    lx.fillStyle = rg; lx.fillRect(0, 0, S, S);
    gearPathFn(lx, g, th)(0, 0); lx.fillStyle = '#000'; lx.fill();
    lx.globalCompositeOperation = 'destination-out';
    const R = rng(Math.round(x * 31 + y * 7)); lx.lineWidth = 0.55;
    for (let r = 3; r < ra + 1; r += 0.6) {
      lx.strokeStyle = 'rgba(0,0,0,' + R() * 0.5 + ')'; const a = R() * TAU;
      lx.beginPath(); lx.arc(x, y, r, a, a + 0.6 + R() * 4.5); lx.stroke();
    }
    lx.globalCompositeOperation = 'source-over';
    ctx.save(); ctx.globalCompositeOperation = 'lighter';
    ctx.filter = 'blur(' + (rear ? 4 : 3) + 'px)'; ctx.globalAlpha = 0.65; ctx.drawImage(SPEC, 0, 0);
    ctx.filter = 'blur(1.2px)'; ctx.globalAlpha = 0.5; ctx.drawImage(SPEC, 0, 0);
    ctx.filter = 'blur(0.35px)'; ctx.globalAlpha = 0.9; ctx.drawImage(SPEC, 0, 0);
    ctx.filter = 'none'; ctx.restore();
  }
  // two warm sources hidden under the dial ring at 3 and 9 o'clock
  const sideW = (x) => cl((C - x) / (WIN * 1.4) + 0.5);
  function cl(v) { return Math.max(0, Math.min(1, v)); }
  function sideShadows(x, k) {
    const wl = sideW(x), wr = 1 - wl, L = [];
    [[3, 1.5, 3, 0.8], [11, 3, 10, 0.55], [24, 5, 22, 0.4], [40, 7, 34, 0.25]].forEach(([d, oy, b, a]) => {
      const dd = d * k;
      L.push([dd, oy, b * k, a * (0.35 + 0.65 * wl)], [-dd, oy, b * k, a * (0.35 + 0.65 * wr)]);
    });
    return L;
  }
  function sideWash(ctx, ry, reach, rgb, a, clipR) {
    rgb = tint(rgb);
    [[-1, sideW(C - WIN)], [1, 1 - sideW(C - WIN)]].forEach(([sd]) => {
      ctx.save();
      if (clipR) { ctx.beginPath(); ctx.arc(C, C, clipR, 0, TAU); ctx.clip(); }
      ctx.translate(C + sd * (WIN + 6), C); ctx.scale(1, ry);
      const q = ctx.createRadialGradient(0, 0, 0, 0, 0, reach);
      q.addColorStop(0, 'rgba(' + rgb + ',' + Math.min(1, a) + ')'); q.addColorStop(0.35, 'rgba(' + rgb + ',' + Math.min(1, a * 0.45) + ')'); q.addColorStop(1, 'rgba(' + rgb + ',0)');
      ctx.fillStyle = q; ctx.fillRect(-reach, -reach, reach * 2, reach * 2); ctx.restore();
    });
  }
  function castShadow(ctx, pathFn, layers, stroke) {
    for (const [ox, oy, b, a] of layers) {
      ctx.save(); ctx.shadowColor = 'rgba(0,0,0,' + a + ')'; ctx.shadowBlur = b; ctx.shadowOffsetX = OFF + ox; ctx.shadowOffsetY = oy;
      pathFn(-OFF / ctx.getTransform().a, 0);
      if (stroke) { ctx.strokeStyle = '#000'; ctx.stroke(); } else { ctx.fillStyle = '#000'; ctx.fill(); }
      ctx.restore();
    }
  }
  function sheen(ctx, x, y, r, k) {
    const wl = sideW(x), wr = 1 - wl, g = Math.min(1.6, GLOW);
    const cg = ctx.createConicGradient(0, x, y);
    for (let i = 0; i <= 96; i++) {
      const t = (i / 96) * TAU, lit = (0.18 + 0.5 * wr) * lobe(t, 0, 0.42) + (0.18 + 0.5 * wl) * lobe(t, Math.PI, 0.42);
      const dk = 0.38 * (lobe(t, Math.PI / 2, 0.5) + lobe(t, Math.PI * 1.5, 0.5));
      cg.addColorStop(i / 96, lit * g >= dk ? 'rgba(' + tint('255,190,110') + ',' + Math.min(1, (lit * g - dk * 0.3) * k) + ')' : 'rgba(0,0,0,' + Math.min(1, (dk - lit * g * 0.3) * k) + ')');
    }
    ctx.fillStyle = cg; ctx.fillRect(x - r - 6, y - r - 6, 2 * r + 12, 2 * r + 12);
    ctx.lineWidth = 0.7;
    for (let q = 4, i = 0; q < r; q += 1.7, i++) {
      ctx.strokeStyle = i % 2 ? 'rgba(255,240,210,.045)' : 'rgba(0,0,0,.05)';
      ctx.beginPath(); ctx.arc(x, y, q, 0, TAU); ctx.stroke();
    }
  }
  // a solid metal part: cast shadows, extruded thickness, face, brushed sheen, chamfer bevel
  function solid(ctx, pathFn, x, y, r, pal, o) {
    castShadow(ctx, pathFn, o.shadows);
    const side = ctx.createLinearGradient(x - r, y - r, x + r, y + r);
    side.addColorStop(0, pal.lo); side.addColorStop(1, pal.edge);
    for (let k = o.depth; k >= 1; k--) { pathFn(k * 0.75, k * 1.05); ctx.fillStyle = side; ctx.fill(); }
    pathFn(0, 0); ctx.fillStyle = grad(ctx, x, y, r, pal); ctx.fill();
    ctx.save(); pathFn(0, 0); ctx.clip();
    if (o.etch) o.etch();
    sheen(ctx, x, y, r, o.sheen);
    ctx.lineJoin = 'round';
    const wl = sideW(x), g = Math.min(1.6, GLOW);
    pathFn(0, 1.6); ctx.strokeStyle = 'rgba(0,0,0,.5)'; ctx.lineWidth = 2.2; ctx.stroke();
    pathFn(0, -1.6); ctx.strokeStyle = 'rgba(0,0,0,.45)'; ctx.stroke();
    ctx.lineWidth = 1.8; ctx.filter = 'blur(0.6px)';
    const CH = tint('255,205,130');
    pathFn(1.4, 0); ctx.strokeStyle = 'rgba(' + CH + ',' + Math.min(1, (0.25 + 0.6 * wl) * o.sheen * g) + ')'; ctx.stroke();
    pathFn(-1.4, 0); ctx.strokeStyle = 'rgba(' + CH + ',' + Math.min(1, (0.25 + 0.6 * (1 - wl)) * o.sheen * g) + ')'; ctx.stroke();
    ctx.filter = 'none';
    ctx.restore();
    pathFn(0, 0); ctx.strokeStyle = pal.edge; ctx.lineWidth = 0.7; ctx.stroke();
  }
  function gearPathFn(ctx, g, th) {
    const { N, m, rp } = g, ra = rp + m, rr = rp - 1.25 * m, ri = rr - 2.2 * m, rh = Math.max(7, rp * 0.2);
    const sw = Math.max(3, m * 1.05), curve = 0.38, mid = (rh + ri) / 2;
    return (dx, dy) => {
      const x = g.x + dx, y = g.y + dy;
      ctx.beginPath();
      teethPath(ctx, x, y, N, rr, ra, th, markPeriod(K, N));
      ctx.moveTo(x + ri, y); ctx.arc(x, y, ri, 0, TAU, true);
      if (g.sp === 'tapered') {
        const rh2 = rh * 1.3, rht = rh2 * 1.15, wh = Math.max(16, m * 6), wr = 0.84 * Math.max(5, m * 2.3);
        for (let i = 0; i < g.s; i++) {
          const b = th + (i / g.s) * TAU + Math.PI / N, hw = Math.min(Math.PI / g.s * 0.92, wh / 2 / rht), rw = wr / 2 / ri;
          let q = P(x, y, rht * 0.92, b - hw); ctx.moveTo(q[0], q[1]);
          q = P(x, y, ri + 1.5, b - rw); ctx.lineTo(q[0], q[1]); q = P(x, y, ri + 1.5, b + rw); ctx.lineTo(q[0], q[1]);
          q = P(x, y, rht * 0.92, b + hw); ctx.lineTo(q[0], q[1]); ctx.closePath();
        }
        ctx.moveTo(x + rht, y); ctx.arc(x, y, rht, 0, TAU);
        return;
      }
      if (g.sp === 'holes') {
        // lightened disc: a solid web pierced by round holes on the spoke symmetry
        ctx.moveTo(x + ri + 1.5, y); ctx.arc(x, y, ri + 1.5, 0, TAU);
        const r0 = rh * 1.3, mr = (r0 + ri) / 2, hr = Math.min((ri - r0) / 2 - 2.5, mr * Math.sin(Math.PI / g.s) - 2.5);
        if (hr > 2) for (let i = 0; i < g.s; i++) { const q = P(x, y, mr, th + (i / g.s) * TAU + Math.PI / N); ctx.moveTo(q[0] + hr, q[1]); ctx.arc(q[0], q[1], hr, 0, TAU, true); }
        return;
      }
      if (g.sp === 'thin') {
        // fine constant-width spokes, small hub
        const hw = Math.max(0.9, m * 0.42), rh2 = rh * 1.05;
        for (let i = 0; i < g.s; i++) {
          const b = th + (i / g.s) * TAU + Math.PI / N, a0 = hw / rh2, a1 = hw / ri;
          let q = P(x, y, rh2 * 0.9, b - a0); ctx.moveTo(q[0], q[1]);
          q = P(x, y, ri + 1.5, b - a1); ctx.lineTo(q[0], q[1]); q = P(x, y, ri + 1.5, b + a1); ctx.lineTo(q[0], q[1]);
          q = P(x, y, rh2 * 0.9, b + a0); ctx.lineTo(q[0], q[1]); ctx.closePath();
        }
        ctx.moveTo(x + rh2, y); ctx.arc(x, y, rh2, 0, TAU);
        return;
      }
      if (g.sp === 'closed') {
        ctx.moveTo(x + ri + 1.5, y); ctx.arc(x, y, ri + 1.5, 0, TAU);
        const ro = ri - Math.max(4, m * 1.6), rin = rh * 1.3 + Math.max(4, m * 1.6), half = Math.max(4, m * 2.1);
        if (ro - rin > 4) for (let i = 0; i < g.s; i++) {
          const b = th + (i / g.s) * TAU + Math.PI / N, b2 = b + TAU / g.s;
          const a1 = b + half / ro, a2 = b2 - half / ro, i1 = b + half / rin, i2 = b2 - half / rin;
          if (a2 <= a1 || i2 <= i1) continue;
          let q = P(x, y, ro, a2); ctx.moveTo(q[0], q[1]);
          ctx.arc(x, y, ro, a2, a1, true);
          q = P(x, y, rin, i1); ctx.lineTo(q[0], q[1]);
          ctx.arc(x, y, rin, i1, i2, false); ctx.closePath();
        }
        return;
      }
      if (g.sp === 'straight') {
        const w = Math.max(5, m * 2.3), rh2 = rh * 1.3;
        for (let i = 0; i < g.s; i++) {
          const b = th + (i / g.s) * TAU + Math.PI / N, hw = (w * 0.6) / rh2, rw = (w * 0.42) / ri;
          let q = P(x, y, rh2 * 0.9, b - hw); ctx.moveTo(q[0], q[1]);
          q = P(x, y, ri + 1.5, b - rw); ctx.lineTo(q[0], q[1]); q = P(x, y, ri + 1.5, b + rw); ctx.lineTo(q[0], q[1]);
          q = P(x, y, rh2 * 0.9, b + hw); ctx.lineTo(q[0], q[1]); ctx.closePath();
        }
        ctx.moveTo(x + rh2, y); ctx.arc(x, y, rh2, 0, TAU);
        return;
      }
      for (let i = 0; i < g.s; i++) {
        const b = th + (i / g.s) * TAU + Math.PI / N;
        const hw = (sw * 0.85) / rh, mw = (sw * 0.5) / mid, rw = (sw * 0.75) / ri;
        let q = P(x, y, rh * 0.9, b - hw); ctx.moveTo(q[0], q[1]);
        let c1 = P(x, y, mid, b + curve * 0.45 - mw), e1 = P(x, y, ri + 1.5, b + curve - rw);
        ctx.quadraticCurveTo(c1[0], c1[1], e1[0], e1[1]);
        e1 = P(x, y, ri + 1.5, b + curve + rw); ctx.lineTo(e1[0], e1[1]);
        c1 = P(x, y, mid, b + curve * 0.45 + mw); q = P(x, y, rh * 0.9, b + hw);
        ctx.quadraticCurveTo(c1[0], c1[1], q[0], q[1]); ctx.closePath();
      }
      ctx.moveTo(x + rh, y); ctx.arc(x, y, rh, 0, TAU);
    };
  }
  function etch(ctx, g, th, rear) {
    const E = g.et || ETCH;
    if (E === 'none') return;
    const { x, y, m, rp } = g, rr = rp - 1.25 * m, rh = Math.max(7, rp * 0.2), k = rear ? 0.6 : 1;
    if (E === 'rings') {
      ctx.lineWidth = 1.1;
      for (let r = 3, i = 0; r < rr; r += 2.8, i++) {
        ctx.strokeStyle = 'rgba(0,0,0,' + 0.5 * k + ')'; ctx.beginPath(); ctx.arc(x, y + 0.5, r, 0, TAU); ctx.stroke();
        ctx.strokeStyle = 'rgba(255,240,205,' + 0.35 * k + ')'; ctx.beginPath(); ctx.arc(x, y - 0.5, r + 0.3, 0, TAU); ctx.stroke();
      }
      return;
    }
    // perlage: overlapping swirl dots on rings, count a multiple of the spoke symmetry so the loop stays exact
    const pr = Math.max(3, m * 0.95), step = pr * 1.15;
    for (let r = rh * 0.9 + pr * 0.6, ring = 0; r < rr + pr * 0.4; r += step, ring++) {
      const n = g.s * Math.max(1, Math.ceil((TAU * r) / (step * g.s)));
      const j0 = Math.round(th / (TAU / n));
      for (let jj = 0; jj < n; jj++) {
        const j = jj - j0, q = P(x, y, r, th + (j + (ring % 2) * 0.5) * (TAU / n));
        const gg = ctx.createRadialGradient(q[0] - pr * 0.35, q[1] - pr * 0.35, 0, q[0], q[1], pr);
        gg.addColorStop(0, 'rgba(255,240,205,' + 0.4 * k + ')'); gg.addColorStop(0.5, 'rgba(255,240,205,0)');
        gg.addColorStop(0.82, 'rgba(0,0,0,' + 0.38 * k + ')'); gg.addColorStop(1, 'rgba(0,0,0,0)');
        ctx.fillStyle = gg; ctx.beginPath(); ctx.arc(q[0], q[1], pr, 0, TAU); ctx.fill();
      }
    }
  }
  function drawGear(ctx, g, th, pal, rear, gold) {
    const { x, y, m, rp } = g, ra = rp + m, rr = rp - 1.25 * m, ri = rr - 2.2 * m, rh = Math.max(7, rp * 0.2) * (g.sp === 'arched' ? 1 : 1.3);
    const et = () => etch(ctx, g, th, rear);
    solid(ctx, gearPathFn(ctx, g, th), x, y, ra, pal, rear
      ? { shadows: sideShadows(x, 0.7), depth: 3, sheen: 0.55, etch: et }
      : { shadows: sideShadows(x, 1), depth: 4, sheen: 1, etch: et });
    gearSpec(ctx, g, th, pal, rear);
    if (g.pinOn) {
      const n = g.pin, pr = rh * 1.25, pm = (2 * pr) / (n + 2.5);
      const pin = (dx, dy) => { ctx.beginPath(); teethPath(ctx, x + dx, y + dy, n, pr - 1.25 * pm, pr + pm, th + 0.2, markPeriod((K * n) / g.N, n)); };
      solid(ctx, pin, x, y, pr + pm, pal, { shadows: sideShadows(x, 0.35), depth: 3, sheen: rear ? 0.5 : 0.9 });
    }
    if (rear) {
      ctx.fillStyle = '#0b0805'; ctx.beginPath(); ctx.arc(x, y, 3.2, 0, TAU); ctx.fill();
      ctx.fillStyle = 'rgba(255,220,170,.35)'; ctx.beginPath(); ctx.arc(x - 0.8, y - 0.8, 1, 0, TAU); ctx.fill();
    } else post(ctx, g, gold);
  }
  function post(ctx, g, gold) { if (g.center === 'screw') screw(ctx, g.x, g.y, 6, -0.6); else jewel(ctx, g.x, g.y, gold); }

  function bridge(ctx, pts, w, pal) {
    const line = (dx, dy) => { ctx.beginPath(); pts.forEach((p, i) => (i ? ctx.lineTo(p[0] + dx, p[1] + dy) : ctx.moveTo(p[0] + dx, p[1] + dy))); };
    ctx.save(); ctx.lineCap = 'round'; ctx.lineJoin = 'round'; ctx.lineWidth = w;
    castShadow(ctx, line, sideShadows(pts.reduce((a, p) => a + p[0], 0) / pts.length, 1), true);
    const xs = pts.map((p) => p[0]), ys = pts.map((p) => p[1]);
    const x0 = Math.min(...xs), y0 = Math.min(...ys) - w, x1 = Math.max(...xs), y1 = Math.max(...ys) + w;
    for (let k = 4; k >= 1; k--) { line(k * 0.75, k * 1.05); ctx.strokeStyle = pal.edge; ctx.stroke(); }
    const g = ctx.createLinearGradient(x0, y0, x1, y1);
    g.addColorStop(0, pal.hi); g.addColorStop(0.5, pal.mid); g.addColorStop(1, pal.lo);
    line(0, 0); ctx.strokeStyle = g; ctx.stroke();
    ctx.lineWidth = w - 3; line(1.1, 1.4); ctx.strokeStyle = 'rgba(0,0,0,.35)'; ctx.stroke();
    ctx.lineWidth = w - 4.5; line(0, 0); ctx.strokeStyle = g; ctx.stroke();
    ctx.lineWidth = w - 6; line(-0.6, -0.8); ctx.strokeStyle = 'rgba(255,255,255,.1)'; ctx.stroke();
    ctx.restore();
  }

  function drawBalance(ctx, t, pal, gold) {
    const { x, y, r } = BAL, osc = 0.95 * ({ 1: 1, 2: 2 / 3, 4: 1 / 3 })[BEATS] * Math.sin(TAU * t * BEATS), beta = BAL.rot + osc;
    const turns = 7, r0 = 6, r1 = 25, tot = turns * TAU;
    const spring = (dx, dy) => {
      ctx.beginPath();
      for (let i = 0; i <= 420; i++) {
        const ph = (i / 420) * tot, rad = r0 + ((r1 - r0) * ph) / tot, a = BAL.rot + ph + osc * (1 - ph / tot);
        const q = P(x + dx, y + dy, rad, a); i ? ctx.lineTo(q[0], q[1]) : ctx.moveTo(q[0], q[1]);
      }
    };
    ctx.save(); ctx.lineWidth = 0.9; castShadow(ctx, spring, [[2, 4, 3, 0.7]], true);
    spring(0, 0); ctx.strokeStyle = '#5577ad'; ctx.stroke();
    spring(-0.4, -0.4); ctx.strokeStyle = 'rgba(170,200,255,.35)'; ctx.lineWidth = 0.4; ctx.stroke(); ctx.restore();
    const wheel = (dx, dy) => {
      const X = x + dx, Y = y + dy; ctx.beginPath();
      ctx.moveTo(X + r, Y); ctx.arc(X, Y, r, 0, TAU); ctx.moveTo(X + r - 4.5, Y); ctx.arc(X, Y, r - 4.5, 0, TAU, true);
      for (let i = 0; i < 3; i++) {
        const b = beta + (i / 3) * TAU, w = 0.09;
        let q = P(X, Y, 4, b - 0.5); ctx.moveTo(q[0], q[1]);
        q = P(X, Y, r - 3.5, b - w); ctx.lineTo(q[0], q[1]); q = P(X, Y, r - 3.5, b + w); ctx.lineTo(q[0], q[1]);
        q = P(X, Y, 4, b + 0.5); ctx.lineTo(q[0], q[1]); ctx.closePath();
      }
    };
    solid(ctx, wheel, x, y, r, pal, { shadows: sideShadows(x, 1), depth: 3, sheen: 1 });
    for (let i = 0; i < 12; i++) {
      const q = P(x, y, r + 1.5, beta + (i / 12) * TAU);
      ctx.save(); shadow(ctx, 2, 1, 1.5, 0.7);
      const g = ctx.createRadialGradient(q[0] - 0.8, q[1] - 0.8, 0.2, q[0], q[1], 2.6);
      g.addColorStop(0, gold.hi); g.addColorStop(1, gold.lo);
      ctx.fillStyle = g; ctx.beginPath(); ctx.arc(q[0], q[1], 2.4, 0, TAU); ctx.fill(); ctx.restore();
    }
  }

  function gauss(d, sg) { return Math.exp(-0.5 * (d / sg) * (d / sg)); }
  // soft anisotropic highlight on a circular ridge: angular lobes x radial profile x brushed breakup, plus bloom
  function specular(ctx, rc, w, lobes, rgb, amt, bloom, seed) {
    rgb = tint(rgb);
    const L = canvas(S), lx = L.getContext('2d'), cg = lx.createConicGradient(0, C, C);
    for (let i = 0; i <= 180; i++) {
      const th = (i / 180) * TAU; let v = 0;
      lobes.forEach(([c, sg, a]) => { let d = Math.abs(th - c) % TAU; if (d > Math.PI) d = TAU - d; v += a * gauss(d, sg); });
      cg.addColorStop(i / 180, 'rgba(' + rgb + ',' + Math.min(1, v * amt) + ')');
    }
    lx.fillStyle = cg; lx.beginPath(); lx.arc(C, C, rc + w * 2.2, 0, TAU); lx.arc(C, C, Math.max(0, rc - w * 2.2), 0, TAU, true); lx.fill();
    lx.globalCompositeOperation = 'destination-in';
    const rg = lx.createRadialGradient(C, C, rc - w * 2.2, C, C, rc + w * 2.2);
    for (let i = 0; i <= 12; i++) { const u = i / 12, d = (u - 0.5) * 4.4; rg.addColorStop(u, 'rgba(0,0,0,' + Math.pow(gauss(d, 0.75), 1.3) + ')'); }
    lx.fillStyle = rg; lx.fillRect(0, 0, S, S);
    lx.globalCompositeOperation = 'destination-out';
    const R = rng(seed); lx.lineWidth = 0.6;
    for (let r = rc - w * 2.2; r < rc + w * 2.2; r += 0.7) {
      lx.strokeStyle = 'rgba(0,0,0,' + (R() * 0.45) + ')'; const a = R() * TAU;
      lx.beginPath(); lx.arc(C, C, r, a, a + 0.5 + R() * 4); lx.stroke();
    }
    ctx.save(); ctx.globalCompositeOperation = 'lighter';
    ctx.filter = 'blur(' + bloom + 'px)'; ctx.globalAlpha = 0.7; ctx.drawImage(L, 0, 0);
    ctx.filter = 'blur(0.4px)'; ctx.globalAlpha = 1; ctx.drawImage(L, 0, 0);
    ctx.filter = 'none'; ctx.restore();
  }
  // grime, crazing, scratches, chips — applied after printing so they cross the ink
  function wear(ctx, x, y, rIn, rOut, seed, en, k) {
    const R = rng(seed), dark = !!en.dark;
    const GR = dark ? '120,104,78' : '48,28,8', BL = dark ? '140,120,88' : '70,44,16', AR = dark ? '150,132,100' : '50,30,10';
    ctx.save(); ctx.beginPath(); ctx.arc(x, y, rOut, 0, TAU); if (rIn > 0) ctx.arc(x, y, rIn, 0, TAU, true); ctx.clip('evenodd');
    let g = ctx.createRadialGradient(x, y, rIn, x, y, rOut);
    const ga = dark ? 0.35 : 1;
    g.addColorStop(0, 'rgba(' + GR + ',' + 0.55 * k * ga + ')'); g.addColorStop(0.16, 'rgba(' + GR + ',0)');
    g.addColorStop(0.8, 'rgba(' + GR + ',0)'); g.addColorStop(1, 'rgba(' + GR + ',' + 0.6 * k * ga + ')');
    ctx.fillStyle = g; ctx.fillRect(0, 0, S, S);
    const pt = (rr) => { const a = R() * TAU, d = rr != null ? rr : rIn + Math.sqrt(R()) * (rOut - rIn); return [x + Math.cos(a) * d, y + Math.sin(a) * d, a, d]; };
    for (let i = 0; i < 9 * k; i++) {
      const [px, py] = pt(), sr = 14 + R() * 40;
      ctx.save(); ctx.translate(px, py); ctx.rotate(R() * TAU); ctx.scale(1, 0.35 + R() * 0.5);
      const q = ctx.createRadialGradient(0, 0, 0, 0, 0, sr);
      q.addColorStop(0, 'rgba(' + BL + ',' + (0.08 + R() * 0.12) * k * (dark ? 0.6 : 1) + ')'); q.addColorStop(1, 'rgba(' + BL + ',0)');
      ctx.fillStyle = q; ctx.fillRect(-sr, -sr, sr * 2, sr * 2); ctx.restore();
    }
    if (!dark || en.stain) {
      ctx.lineCap = 'round';
      for (let i = 0; i < 70 * k; i++) {
        let [px, py, a] = pt(); let dir = R() * TAU;
        ctx.beginPath(); ctx.moveTo(px, py);
        const n = 3 + ((R() * 9) | 0);
        for (let j = 0; j < n; j++) { dir += (R() - 0.5) * 1.6; px += Math.cos(dir) * (3 + R() * 7); py += Math.sin(dir) * (3 + R() * 7); ctx.lineTo(px, py); }
        ctx.strokeStyle = dark ? 'rgba(190,172,140,' + (0.05 + R() * 0.1) * k + ')' : 'rgba(60,38,14,' + (0.12 + R() * 0.22) * k + ')'; ctx.lineWidth = 0.45; ctx.stroke();
      }
    }
    for (let i = 0; i < 140 * k; i++) {
      const [px, py] = pt(), dir = R() * TAU, len = 3 + Math.pow(R(), 2) * 38, cv = (R() - 0.5) * 0.6;
      const ex = px + Math.cos(dir) * len, ey = py + Math.sin(dir) * len, mx = (px + ex) / 2 + Math.cos(dir + 1.57) * len * cv, my = (py + ey) / 2 + Math.sin(dir + 1.57) * len * cv;
      ctx.lineWidth = 0.35 + R() * 0.45;
      ctx.strokeStyle = dark ? 'rgba(255,240,210,' + (0.06 + R() * 0.12) * k + ')' : 'rgba(40,24,8,' + (0.1 + R() * 0.2) * k + ')';
      ctx.beginPath(); ctx.moveTo(px, py); ctx.quadraticCurveTo(mx, my, ex, ey); ctx.stroke();
      ctx.strokeStyle = dark ? 'rgba(0,0,0,' + (0.2 + R() * 0.3) * k + ')' : 'rgba(255,252,240,' + (0.1 + R() * 0.2) * k + ')';
      ctx.beginPath(); ctx.moveTo(px + 0.5, py + 0.5); ctx.quadraticCurveTo(mx + 0.5, my + 0.5, ex + 0.5, ey + 0.5); ctx.stroke();
    }
    for (let i = 0; i < 40 * k; i++) {
      const [, , a, d] = pt(), span = 0.05 + R() * 0.35;
      ctx.strokeStyle = 'rgba(' + AR + ',' + (0.06 + R() * 0.12) * k * (dark ? 0.6 : 1) + ')'; ctx.lineWidth = 0.4;
      ctx.beginPath(); ctx.arc(x, y, d, a, a + span); ctx.stroke();
    }
    for (let i = 0; i < 14 * k; i++) {
      const [px, py] = pt(), cr = 0.6 + Math.pow(R(), 2) * 2.6;
      ctx.beginPath();
      for (let j = 0; j < 7; j++) { const a = (j / 7) * TAU, rr = cr * (0.6 + R() * 0.7); j ? ctx.lineTo(px + Math.cos(a) * rr, py + Math.sin(a) * rr) : ctx.moveTo(px + Math.cos(a) * rr, py + Math.sin(a) * rr); }
      ctx.closePath(); ctx.fillStyle = dark ? 'rgba(128,116,96,' + (0.5 + R() * 0.35) + ')' : 'rgba(28,16,5,' + (0.45 + R() * 0.35) + ')'; ctx.fill();
      ctx.strokeStyle = dark ? 'rgba(0,0,0,.5)' : 'rgba(255,250,235,.35)'; ctx.lineWidth = 0.5; ctx.stroke();
    }
    for (let i = 0; i < 1400 * k; i++) {
      const [px, py] = pt();
      ctx.fillStyle = R() < 0.5 ? (dark ? 'rgba(0,0,0,' + R() * 0.35 + ')' : 'rgba(50,30,10,' + R() * 0.25 + ')') : 'rgba(255,250,235,' + R() * (dark ? 0.1 : 0.18) + ')';
      ctx.fillRect(px, py, 0.6 + R() * 0.8, 0.6 + R() * 0.8);
    }
    ctx.restore();
  }
  function enamelFill(ctx, x, y, r, en, seed) {
    const g = ctx.createRadialGradient(x - r * 0.3, y - r * 0.35, r * 0.1, x, y, r * 1.05);
    g.addColorStop(0, en.a); g.addColorStop(1, en.b);
    ctx.fillStyle = g; ctx.fill();
    if (!en.stain) return;
    const R = rng(seed);
    ctx.save(); ctx.clip();
    for (let i = 0; i < 22; i++) {
      const a = R() * TAU, d = R() * r, sx = x + Math.cos(a) * d, sy = y + Math.sin(a) * d, sr = 8 + R() * 38;
      const s = ctx.createRadialGradient(sx, sy, 0, sx, sy, sr);
      const sc = en.dark ? '130,112,82' : '110,72,30'; s.addColorStop(0, `rgba(${sc},${(0.05 + R() * 0.12) * (en.dark ? 0.55 : 1)})`); s.addColorStop(1, `rgba(${sc},0)`);
      ctx.fillStyle = s; ctx.fillRect(sx - sr, sy - sr, sr * 2, sr * 2);
    }
    for (let i = 0; i < 500; i++) {
      const a = R() * TAU, d = Math.sqrt(R()) * r;
      ctx.fillStyle = en.dark ? `rgba(170,150,115,${0.05 + R() * 0.12})` : `rgba(60,40,18,${0.08 + R() * 0.2})`;
      ctx.fillRect(x + Math.cos(a) * d, y + Math.sin(a) * d, 0.5 + R() * 1.2, 0.5 + R());
    }
    ctx.strokeStyle = en.dark ? 'rgba(170,150,115,.16)' : 'rgba(70,45,20,.28)'; ctx.lineWidth = 0.6;
    for (let i = 0; i < 5; i++) {
      let a = R() * TAU, d = r * (0.5 + R() * 0.5); ctx.beginPath();
      let q = P(x, y, d, a); ctx.moveTo(q[0], q[1]);
      for (let j = 0; j < 6; j++) { a += (R() - 0.5) * 0.08; d += (R() - 0.5) * 9; q = P(x, y, d, a + j * 0.02); ctx.lineTo(q[0], q[1]); }
      ctx.stroke();
    }
    ctx.restore();
  }

  function buildStatic(o) {
    const en = ENAMEL[o.enamel] || ENAMEL.aged, pal = FIN[o.finish] || FIN.brass;
    const base = canvas(S), top = canvas(S);
    let ctx = base.getContext('2d');
    ctx.fillStyle = '#000'; ctx.fillRect(0, 0, S, S);
    let g = ctx.createRadialGradient(175, 160, 10, C, C, WIN);
    g.addColorStop(0, '#3b2816'); g.addColorStop(1, '#0a0604');
    ctx.fillStyle = g; ctx.beginPath(); ctx.arc(C, C, WIN + 2, 0, TAU); ctx.fill();
    ctx.lineWidth = 1;
    for (let r = 8; r < WIN; r += 5) { ctx.strokeStyle = `rgba(255,210,150,${r % 10 ? 0.025 : 0.045})`; ctx.beginPath(); ctx.arc(C, C, r, 0, TAU); ctx.stroke(); }
    ctx.save(); ctx.globalCompositeOperation = 'lighter';
    sideWash(ctx, 0.62, 230, '255,135,40', 0.75 * GLOW, WIN + 2);
    sideWash(ctx, 0.3, 120, '255,200,120', 0.4 * GLOW, WIN + 2);
    ctx.restore();
    const R = rng(7);
    for (let i = 0; i < 40; i++) {
      const a = R() * TAU, d = Math.sqrt(R()) * WIN, x = C + Math.cos(a) * d, y = C + Math.sin(a) * d;
      ctx.strokeStyle = 'rgba(255,200,140,.035)'; ctx.beginPath(); ctx.arc(x, y, 9, 0, TAU); ctx.stroke();
    }

    ctx = top.getContext('2d');
    [[0, 5, 8, 0.85], [0, -5, 8, 0.85], [0, 16, 28, 0.75], [0, -16, 28, 0.75], [0, 30, 50, 0.5], [0, -30, 50, 0.5], [0, 0, 6, 0.5]].forEach(([ox, oy, b, a]) => {
      ctx.save(); ctx.beginPath(); ctx.arc(C, C, WIN + 1, 0, TAU); ctx.clip();
      ctx.shadowColor = 'rgba(0,0,0,' + a + ')'; ctx.shadowBlur = b; ctx.shadowOffsetX = ox; ctx.shadowOffsetY = oy;
      ctx.beginPath(); ctx.rect(-80, -80, S + 160, S + 160); ctx.arc(C, C, WIN + 1, 0, TAU, true);
      ctx.fillStyle = '#000'; ctx.fill(); ctx.restore();
    });
    g = ctx.createRadialGradient(C, C, WIN - 22, C, C, WIN);
    g.addColorStop(0, 'rgba(0,0,0,0)'); g.addColorStop(1, 'rgba(0,0,0,.4)');
    ctx.fillStyle = g; ctx.beginPath(); ctx.arc(C, C, WIN, 0, TAU); ctx.fill();
    ctx.save(); shadow(ctx, 10, 0, 3, 0.8);
    ctx.beginPath(); ctx.arc(C, C, 221, 0, TAU); ctx.arc(C, C, WIN, 0, TAU, true);
    ctx.fillStyle = en.a; ctx.fill(); ctx.restore();
    ctx.beginPath(); ctx.arc(C, C, 221, 0, TAU); ctx.arc(C, C, WIN, 0, TAU, true);
    enamelFill(ctx, C, C, 221, en, 11);
    ctx.save(); ctx.beginPath(); ctx.arc(C, C, 221, 0, TAU); ctx.arc(C, C, WIN, 0, TAU, true); ctx.clip('evenodd');
    g = ctx.createRadialGradient(C, C, WIN, C, C, 221);
    g.addColorStop(0, 'rgba(60,35,10,.45)'); g.addColorStop(0.12, 'rgba(60,35,10,0)'); g.addColorStop(0.55, 'rgba(255,250,235,.1)'); g.addColorStop(0.9, 'rgba(60,35,10,0)'); g.addColorStop(1, 'rgba(40,22,6,.5)');
    ctx.fillStyle = g; ctx.fillRect(0, 0, S, S);
    g = ctx.createLinearGradient(60, 60, 410, 410);
    g.addColorStop(0, 'rgba(255,248,225,.22)'); g.addColorStop(0.5, 'rgba(0,0,0,0)'); g.addColorStop(1, 'rgba(40,22,6,.32)');
    ctx.fillStyle = g; ctx.fillRect(0, 0, S, S); ctx.restore();
    ctx.strokeStyle = 'rgba(0,0,0,.65)'; ctx.lineWidth = 2.5; ctx.beginPath(); ctx.arc(C, C, WIN + 0.5, 0, TAU); ctx.stroke();
    ctx.strokeStyle = 'rgba(255,250,235,.35)'; ctx.lineWidth = 1; ctx.beginPath(); ctx.arc(C, C, WIN + 2.6, 0, TAU); ctx.stroke();
    ctx.strokeStyle = en.ink; ctx.globalAlpha = 0.85;
    ctx.lineWidth = 0.8; [203, 215].forEach((r) => { ctx.beginPath(); ctx.arc(C, C, r, 0, TAU); ctx.stroke(); });
    for (let i = 0; i < 60; i++) {
      const a = (i / 60) * TAU - Math.PI / 2, big = i % 5 === 0;
      const p1 = P(C, C, big ? 201 : 205, a), p2 = P(C, C, 215, a);
      ctx.lineWidth = big ? 2.4 : 0.9; ctx.beginPath(); ctx.moveTo(p1[0], p1[1]); ctx.lineTo(p2[0], p2[1]); ctx.stroke();
    }
    ctx.globalAlpha = 0.92; ctx.fillStyle = en.ink; ctx.textAlign = 'center'; ctx.textBaseline = 'middle';
    ctx.font = '700 31px "Cormorant Garamond", Georgia, serif';
    const ROM = ['XII', 'I', 'II', 'III', 'IIII', 'V', 'VI', 'VII', 'VIII', 'IX', 'X', 'XI'];
    ROM.forEach((s, h) => {
      const a = (h / 12) * TAU; ctx.save(); ctx.translate(C + Math.sin(a) * 181, C - Math.cos(a) * 181); ctx.rotate(a);
      ctx.shadowColor = 'rgba(0,0,0,.45)'; ctx.shadowBlur = 1.5;
      ctx.shadowOffsetX = Math.cos(a) * 0.9 + Math.sin(a) * 1.3; ctx.shadowOffsetY = -Math.sin(a) * 0.9 + Math.cos(a) * 1.3;
      ctx.fillText(s, 0, 1); ctx.restore();
    });
    ctx.globalAlpha = 1;
    if (en.stain) {
      const R2 = rng(91);
      for (let i = 0; i < 900; i++) {
        const a = R2() * TAU, d = WIN + 1 + R2() * (219 - WIN);
        ctx.fillStyle = (en.dark ? 'rgba(32,29,25,' : 'rgba(232,220,192,') + (0.35 + R2() * 0.5) + ')';
        ctx.fillRect(C + Math.cos(a) * d, C + Math.sin(a) * d, 0.6 + R2() * 1.4, 0.6 + R2() * 1.2);
      }
    }
    wear(ctx, C, C, WIN, 221, 57, en, en.stain ? 1.5 : 0.35);
    ctx.strokeStyle = 'rgba(0,0,0,.6)'; ctx.lineWidth = 1.5; ctx.beginPath(); ctx.arc(C, C, 221, 0, TAU); ctx.stroke();
    // bezel
    ctx.beginPath(); ctx.arc(C, C, 233, 0, TAU); ctx.arc(C, C, 221.5, 0, TAU, true);
    g = ctx.createRadialGradient(C, C, 221.5, C, C, 233);
    g.addColorStop(0, '#2c1d0b'); g.addColorStop(0.3, '#9c7434'); g.addColorStop(0.55, '#f0d79a'); g.addColorStop(0.8, '#7d5a24'); g.addColorStop(1, '#120c05');
    ctx.fillStyle = g; ctx.fill();
    ctx.save(); ctx.clip('evenodd'); sheen(ctx, C, C, 233, 0.9);
    { const R3 = rng(77);
      for (let i = 0; i < 260; i++) {
        const a = R3() * TAU, d = 222 + R3() * 11, span = 0.004 + Math.pow(R3(), 2) * 0.06, tilt = (R3() - 0.5) * 3;
        ctx.strokeStyle = R3() < 0.55 ? 'rgba(20,12,4,' + (0.2 + R3() * 0.35) + ')' : 'rgba(255,240,200,' + (0.12 + R3() * 0.3) + ')';
        ctx.lineWidth = 0.4 + R3() * 0.4; const p1 = P(C, C, d, a), p2 = P(C, C, d + tilt, a + span);
        ctx.beginPath(); ctx.moveTo(p1[0], p1[1]); ctx.lineTo(p2[0], p2[1]); ctx.stroke();
      }
      for (let i = 0; i < 18; i++) {
        const a = R3() * TAU, d = 223 + R3() * 9, q = P(C, C, d, a), sr = 3 + R3() * 10;
        const gg = ctx.createRadialGradient(q[0], q[1], 0, q[0], q[1], sr);
        gg.addColorStop(0, 'rgba(30,20,6,' + (0.2 + R3() * 0.3) + ')'); gg.addColorStop(1, 'rgba(30,20,6,0)');
        ctx.fillStyle = gg; ctx.fillRect(q[0] - sr, q[1] - sr, sr * 2, sr * 2);
      }
      const gr = ctx.createRadialGradient(C, C, 221.5, C, C, 233);
      gr.addColorStop(0, 'rgba(20,10,2,.75)'); gr.addColorStop(0.18, 'rgba(20,10,2,0)'); gr.addColorStop(0.85, 'rgba(20,10,2,0)'); gr.addColorStop(1, 'rgba(20,10,2,.7)');
      ctx.fillStyle = gr; ctx.fillRect(0, 0, S, S);
    }
    ctx.restore();
    ctx.beginPath(); ctx.arc(C, C, 233, 0, TAU); ctx.arc(C, C, 221.5, 0, TAU, true);
    g = ctx.createLinearGradient(0, 0, S, S);
    g.addColorStop(0, 'rgba(255,240,200,.25)'); g.addColorStop(0.5, 'rgba(0,0,0,0)'); g.addColorStop(1, 'rgba(0,0,0,.45)');
    ctx.fillStyle = g; ctx.fill();
    // small-seconds subdial
    if (o.subdial) {
      const r = (WIN - 3) / 2, x = 290, y = 290, k = r / 46;
      [[2, 3, 4, 0.8], [8, 13, 20, 0.7], [14, 22, 34, 0.5]].forEach(([ox, oy, b, a]) => { ctx.save(); shadow(ctx, b, ox, oy, a); ctx.beginPath(); ctx.arc(x, y, r + 2.5, 0, TAU); ctx.fillStyle = '#000'; ctx.fill(); ctx.restore(); });
      ctx.save(); ctx.beginPath(); ctx.arc(x, y, r + 2.5, 0, TAU);
      ctx.fillStyle = grad(ctx, x, y, r + 3, pal); ctx.fill(); ctx.restore();
      ctx.beginPath(); ctx.arc(x, y, r, 0, TAU); enamelFill(ctx, x, y, r, en, 23);
      ctx.strokeStyle = en.ink; ctx.globalAlpha = 0.85; ctx.lineWidth = 0.7;
      ctx.beginPath(); ctx.arc(x, y, r - 4 * k, 0, TAU); ctx.stroke();
      ctx.beginPath(); ctx.arc(x, y, r - 11.5 * k, 0, TAU); ctx.lineWidth = 0.5; ctx.stroke();
      for (let i = 0; i < 60; i++) {
        const a = (i / 60) * TAU - Math.PI / 2, big = i % 5 === 0, p1 = P(x, y, big ? r - 11.5 * k : r - 8 * k, a), p2 = P(x, y, r - 4 * k, a);
        ctx.lineWidth = big ? 2 : 0.8; ctx.beginPath(); ctx.moveTo(p1[0], p1[1]); ctx.lineTo(p2[0], p2[1]); ctx.stroke();
      }
      ctx.font = '700 ' + Math.round(12 * k) + 'px "Cormorant Garamond", Georgia, serif'; ctx.fillStyle = en.ink;
      [60, 10, 20, 30, 40, 50].forEach((n, i) => { const a = (i / 6) * TAU - Math.PI / 2, q = P(x, y, r - 20 * k, a); ctx.fillText(String(n), q[0], q[1] + 0.5); });
      ctx.globalAlpha = 1;
      wear(ctx, x, y, 0, r, 63, en, en.stain ? 0.7 : 0.15);
      g = ctx.createRadialGradient(x, y, 2, x, y, 12); g.addColorStop(0, '#120c06'); g.addColorStop(1, 'rgba(0,0,0,.15)');
      ctx.fillStyle = g; ctx.beginPath(); ctx.arc(x, y, 12, 0, TAU); ctx.fill();
    }
    // light + vignette
    g = ctx.createRadialGradient(150, 120, 0, 150, 120, 260);
    g.addColorStop(0, 'rgba(255,214,150,.13)'); g.addColorStop(1, 'rgba(255,214,150,0)');
    ctx.fillStyle = g; ctx.beginPath(); ctx.arc(C, C, 233, 0, TAU); ctx.fill();
    g = ctx.createRadialGradient(C, C, 120, C, C, 233);
    g.addColorStop(0, 'rgba(0,0,0,0)'); g.addColorStop(1, 'rgba(0,0,0,.38)');
    ctx.fillStyle = g; ctx.beginPath(); ctx.arc(C, C, 233, 0, TAU); ctx.fill();
    const glow = canvas(S); ctx = glow.getContext('2d');
    ctx.globalCompositeOperation = 'lighter';
    sideWash(ctx, 0.5, 200, '255,150,60', 0.22 * GLOW, WIN + 2);
    ctx = top.getContext('2d');
    ctx.save(); ctx.globalCompositeOperation = 'lighter';
    g = ctx.createRadialGradient(C, C, WIN - 6, C, C, WIN + 22);
    { const t1 = tint('255,150,60'); g.addColorStop(0, `rgba(${t1},0)`); g.addColorStop(0.3, `rgba(${t1},${Math.min(1, 0.08 * GLOW)})`); g.addColorStop(1, `rgba(${t1},0)`); }
    ctx.fillStyle = g; ctx.beginPath(); ctx.arc(C, C, WIN + 22, 0, TAU); ctx.arc(C, C, WIN + 1.5, 0, TAU, true); ctx.fill();
    sideWash(ctx, 0.75, 70, '255,170,70', 0.55 * GLOW, WIN + 1);
    specular(ctx, WIN - 1.5, 3.2, [[0, 0.42, 1], [Math.PI, 0.42, 1]], '255,165,70', 0.95 * GLOW, 4, 21);
    specular(ctx, WIN + 2.4, 1.8, [[0, 0.3, 0.8], [Math.PI, 0.3, 0.8], [Math.PI * 1.3, 0.35, 0.35]], '255,232,190', 0.7 * GLOW, 2, 22);
    specular(ctx, 227.2, 4.2, [[0, 0.38, 0.9], [Math.PI, 0.38, 0.9], [Math.PI * 1.28, 0.32, 0.75], [Math.PI * 1.52, 0.1, 0.4], [Math.PI * 0.22, 0.18, 0.3]], '255,228,175', 0.85 * GLOW, 3, 23);
    specular(ctx, 222.8, 1.4, [[0, 0.5, 0.6], [Math.PI, 0.5, 0.6]], '255,200,130', 0.5 * GLOW, 1.5, 24);
    g = ctx.createRadialGradient(C, C, 230, C, C, 330);
    { const t2 = tint('255,160,70'); g.addColorStop(0, `rgba(${t2},${Math.min(1, 0.2 * GLOW)})`); g.addColorStop(1, `rgba(${t2},0)`); }
    ctx.fillStyle = g; ctx.beginPath(); ctx.rect(0, 0, S, S); ctx.arc(C, C, 233, 0, TAU, true); ctx.fill('evenodd');
    ctx.restore();
    ctx.save(); ctx.beginPath(); ctx.arc(C, C, 233, 0, TAU); ctx.arc(C, C, 221.5, 0, TAU, true); ctx.clip('evenodd');
    g = ctx.createLinearGradient(80, 80, 390, 390);
    g.addColorStop(0, 'rgba(255,200,120,.18)'); g.addColorStop(0.55, 'rgba(0,0,0,0)'); g.addColorStop(1, 'rgba(0,0,0,.35)');
    ctx.fillStyle = g; ctx.fillRect(0, 0, S, S); ctx.restore();
    return { base, top, glow };
  }

  function configure(o) {
    o = Object.assign({ finish: 'brass', enamel: 'aged', subdial: true, frames: 15, glow: 1 }, o);
    // The bank being rendered decides whether a rim can alias, so the mark
    // decision has to see it. A zeroed/absent count falls back to the top tier.
    BANKF = +o.frames > 0 ? +o.frames : 15;
    GLOW = +o.glow; setRot(+o.trainRot || 0);
    setTemp(o.temp, o.hue, o.hueSat);
    const SD = o.seeds || {};
    const pick = (v, list, R) => (v === 'random' ? list[Math.floor(R() * list.length)] : v);
    const NAT = [...FRONT0, ...REAR0];
    [...FRONT, ...REAR].forEach((g, i) => {
      const base = 1000 + i * 97 + Math.round(NAT[i].x * 13 + NAT[i].y);
      g.sp = pick(o.spokes || 'arched', ['arched', 'straight', 'closed', 'tapered', 'holes', 'thin'], rng(base + (SD.spokes || 0) * 7919));
      g.pinOn = pick(o.pinion == null ? 'on' : o.pinion, ['on', 'off'], rng(base + 31 + (SD.pinion || 0) * 6271)) === 'on';
      g.center = pick(o.center || 'jewel', ['jewel', 'screw'], rng(base + 67 + (SD.center || 0) * 4409));
      g.et = pick(o.etching || 'none', ['none', 'rings', 'perlage'], rng(base + 113 + (SD.etching || 0) * 3301));
      const fk = pick(o.finish || 'brass', ['brass', 'copper', 'gunmetal', 'rhodium'], rng(base + 151 + (SD.finish || 0) * 2203));
      g.pal = FIN[fk] || FIN.brass;
    }); ETCH = o.etching || 'none'; BEATS = [1, 2, 4].includes(+o.beats) ? +o.beats : 1;
    return o;
  }
  function create(o) {
    o = configure(o);
    const pal = FIN[o.finish] || FIN.brass, gold = o.finish === 'brass' ? FIN.copper : FIN.brass;
    const st = buildStatic(o), rear = canvas(S);
    function renderAt(ctx, t) {
      ctx.clearRect(0, 0, S, S);
      ctx.drawImage(st.base, 0, 0);
      ctx.save(); ctx.beginPath(); ctx.arc(C, C, WIN + 1, 0, TAU); ctx.clip();
      const rx = rear.getContext('2d'); rx.clearRect(0, 0, S, S);
      REAR.forEach((g) => drawGear(rx, g, g.p + g.v * t, BACK, true, gold));
      ctx.filter = 'blur(1.1px)'; ctx.drawImage(rear, 0, 0); ctx.filter = 'none';
      ctx.fillStyle = 'rgba(14,7,2,.12)'; ctx.fillRect(0, 0, S, S);
      ctx.globalCompositeOperation = 'lighter'; ctx.globalAlpha = 0.9; ctx.drawImage(st.glow, 0, 0); ctx.drawImage(st.glow, 0, 0); ctx.globalAlpha = 1; ctx.globalCompositeOperation = 'source-over';
      bridge(ctx, [[330, 30], [323, 105], [395, 160]], 20, STEEL);
      screw(ctx, 323, 105, 5.5, 0.6);
      FRONT.forEach((g) => drawGear(ctx, g, g.p + g.v * t, g.pal, false, gold));
      bridge(ctx, [[40, 330], [126, 283]], 22, STEEL);
      post(ctx, FRONT[2], gold); screw(ctx, 82, 307, 5.5, 2.1);
      drawBalance(ctx, t, pal, gold);
      bridge(ctx, [[108, 52], [160, 70], [206, 98]], 18, STEEL);
      jewel(ctx, BAL.x, BAL.y, gold); screw(ctx, 152, 67, 5.5, 1.2);
      ctx.globalCompositeOperation = 'lighter'; ctx.drawImage(st.glow, 0, 0); ctx.globalCompositeOperation = 'source-over';
      ctx.restore();
      ctx.drawImage(st.top, 0, 0);
    }
    return {
      size: S, frames: o.frames,
      renderFrame(ctx, f) { renderAt(ctx, f / o.frames); },
      renderAll() { return Array.from({ length: o.frames }, (_, f) => { const c = canvas(S); renderAt(c.getContext('2d'), f / o.frames); return c; }); },
      newCanvas: () => canvas(S),
    };
  }
  window.DialRenderer = { create, SIZE: S, TEETH_PER_LOOP: K,
    TEMPOS, BANKS, bankFrames, aliases, markPeriod,
    _: { configure, setTemp, kelvin, setTrain, FRONT0, REAR0, BAL0, rng, P, canvas, FRONT, REAR, BAL, tint, grad, shadow, jewel, screw, castShadow, sideShadows, sideWash, sheen, solid, drawGear, post, bridge, drawBalance, specular, wear, enamelFill, FIN, BACK, STEEL, ENAMEL } };
})();
