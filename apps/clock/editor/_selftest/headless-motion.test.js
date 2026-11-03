// D41 G3: a hand's motion in the composer. The drive twin must land where the dial's
// own drive function lands (the figures here are tools/host_drive_test.py's hand-worked
// vectors), the bake must encode each path in the columns orb_clock3.c reads, and the
// Hands tab must offer the controls. The two exercise presets ride every path.
require('./headless-stub.js');
const E = global.WatchEngine;
global.React = { createRef: () => ({ current: null }) };
require('./component.js');
let fails = 0, checked = 0;
const ok = (c, n, x) => { checked++; console.log((c ? 'PASS ' : 'FAIL ') + n + (x ? '  [' + x + ']' : '')); if (!c) fails++; };
const near = (a, b, e) => Math.abs(a - b) <= (e || 1e-9);
const at = (h, m, s, ms) => new Date(2026, 10, 1, h, m, s, ms || 0);

// ---- the drive twin, against the firmware's vectors ----
ok(near(E.driveUnits('second', { cadence: 'step', steps: 2 }, at(1, 2, 10, 600)), 10.5), 'step 2: 10.6 s shows 10.5');
ok(near(E.driveUnits('second', { cadence: 'step', steps: 2 }, at(1, 2, 10, 400)), 10), 'step 2: 10.4 s shows 10.0');
ok(near(E.driveUnits('second', { cadence: 'step', steps: 8 }, at(1, 2, 10, 300)), 10.25), 'step 8: 10.3 s shows 10.25');
ok(near(E.driveUnits('minute', { cadence: 'jump' }, at(12, 34, 59, 900)), 34), 'jump: the minute holds at 12:34:59.9');
ok(near(E.driveUnits('minute', { cadence: 'sweep' }, at(12, 34, 30)), 34.5), 'sweep: 12:34:30 is 34.5');
ok(near(E.driveUnits('second', { cadence: 'stop-to-go' }, at(1, 2, 29, 250)), 30), 'stop-to-go: 29.25 s is half way round');
ok(near(E.driveUnits('second', { cadence: 'stop-to-go' }, at(1, 2, 59, 900)), 60), 'stop-to-go: rests at the top past 58.5 s');
ok(near(E.driveUnits('hour', { cadence: 'jump' }, at(18, 59, 59)), 6), 'jump hour: 18:59 still reads 6');

// ---- the paths ----
const deg = (r) => (r * 180) / Math.PI;
ok(near(deg(E.motionPose({ path: 'retrograde', start: 210, span: 120 }, 12, 6).a), 270), 'a fan from 210 over 120: 6 o’clock reads at 270');
ok(near(deg(E.motionPose({ path: 'retrograde', start: 210, span: 120 }, 12, 14).a), 330), 'and stops at its span past the domain');
ok(near(deg(E.motionPose({ path: 'disc', start: 90 }, 12, 3).a), 0), 'a disc indexed at 3 o’clock turns 3 under it');
const sl = E.motionPose({ path: 'slide-x', from: -70, travel: 160 }, 60, 30);
ok(sl.a === 0 && near(sl.d[0], 10) && sl.d[1] === 0, 'a slide from -70 over 160 sits at +10 at half its domain', JSON.stringify(sl));
ok(E.motionPose({ path: 'slide-y', from: -70, travel: 160 }, 60, 60).d[1] === 90, 'a vertical slide travels on y');
ok(!E.motionOn({ path: 'rotate', cadence: 'inherit' }) && E.motionOn({ cadence: 'jump' }) && E.motionOn({ path: 'disc' }),
   'only a path or a cadence of its own counts as motion');

// ---- ease: the same curves the dial's ease_pose draws, read from the wall clock ----
const ezs = E.easeOf('minute', { cadence: 'jump', ease: 120 });
ok(ezs(at(1, 2, 0, 0)) && near(ezs(at(1, 2, 0, 60)).bump, 1), 'a jumping minute settles: half way through 120 ms is the full overshoot');
ok(ezs(at(1, 2, 0, 130)) === null, 'and the settle is over by 120 ms');
ok(E.easeOf('second', { cadence: 'step', steps: 1, ease: 120 })(at(1, 2, 3, 10)) === null, 'a seconds step never settles');
const ezf = E.easeOf('minute', { path: 'retrograde', cadence: 'sweep', ease: 400 });
ok(ezf(at(1, 0, 0, 200)).fly && near(ezf(at(1, 0, 0, 200)).sm, 0.5), 'a minute fan flies back over 400 ms at the top of the hour');
ok(ezf(at(1, 0, 1, 0)) === null, 'and travels plainly after');
ok(E.easeOf('hour', { cadence: 'jump' })(at(1, 0, 0)) === null, 'no ease, no animation');

// ---- the presets that exercise them ----
const OPTS = { finish: 'train', plate: 'train', tick: 'sweep', glow: 1, temp: 2200 };
const faceOf = (k) => { const P = E.PRESETS[k]; return E.face(Object.assign({}, OPTS, { design: E.canonDesign(JSON.parse(JSON.stringify(P.design))), train: P.train || 'classic', hands: P.hands, handFinish: P.handFinish })); };
const flux = faceOf('flux'), orbit = faceOf('orbit');
// D43 P5c: a preset's hands are comps with their own ids; 'centre-<shows>' names the dial's hand of that kind
const fh = (f, id) => f.hands.find((h) => h.id === id) || f.hands.find((h) => !h.sub && 'centre-' + h.kind === id);
// D43 P5d.2: Flux is built from faces -- its hour and minute ride fan sub-dials, its seconds a slider
ok(fh(flux, 'h1').motion.path === 'retrograde' && fh(flux, 'h2').motion.path === 'retrograde' && fh(flux, 'h1').motion.fromFace, 'Flux: hour and minute ride retrograde fans, their motion the fans\u2019');
ok(typeof fh(flux, 'h3').slide === 'function' && fh(flux, 'h3').motion.path === 'slide', 'Flux: the seconds rides its slider');
const fs = fh(flux, 'h3'), xy0 = E.handXY(fs, at(1, 0, 1)), xy30 = E.handXY(fs, at(1, 0, 31));   // past :00, where the bar flies back
ok(near(xy30[0] - xy0[0], 75) && near(xy30[1], xy0[1]), 'Flux: the seconds travels half its 150 px track in half a minute', JSON.stringify([xy0, xy30]));
ok(E.handXY(fs, at(1, 0, 0, 200))[0] > E.handXY(fs, at(1, 0, 0, 300))[0] && E.handXY(fs, at(1, 0, 0, 300))[0] > xy0[0], 'Flux: at :00 the bar flies back along its track');
ok(near(Math.abs(((deg(E.handAngle(fh(flux, 'h1'), at(6, 59, 0))) + 180) % 360) - 180), 0), 'Flux: the jumping hour reads 6 at 06:59, the middle of its 240\u00b0 fan from 240\u00b0');
// D43 P5d.3m: Orbit's turning-disc hands are exposed rings now, read at a laser index at 3 o'clock
const ow = orbit.lay.wheels;
ok(ow.length === 3 && ow.every((w) => w.exposed && w.form === 'ring' && near(w.read, Math.PI / 2)) && !orbit.hands.length, 'Orbit: three exposed rings, read at 3 o\u2019clock, no hands', ow.map((w) => w.spec.n).join(','));
ok(ow.find((w) => w.spec.n === 60 && w.ease > 0).spec.held(at(3, 15, 59)) === 15, 'Orbit: the jumping minute ring holds 15 under the 3 o\u2019clock index');
// P5d.3n: the index is a part of its own, spanning all three rings from inside the hour ring to past the seconds
const oi = orbit.lay.indexes;
ok(oi.length === 1 && oi[0].style === 'lens' && near(oi[0].a, Math.PI / 2) && oi[0].r0 < Math.min(...ow.map((w) => w.ri)) && oi[0].r1 > Math.max(...ow.map((w) => w.r)), 'Orbit: its laser lens is an index part on the crystal, across all three rings', JSON.stringify(oi[0] && [oi[0].r0, oi[0].r1]));

// ---- the bake's encoding and the Hands tab ----
const DCLogic = function () { };
DCLogic.prototype.setState = function (p) { Object.assign(this.state, typeof p === 'function' ? p(this.state) : p); };
const it = new (global.COMP_CLASS(DCLogic))({});
it.ready = true; it.rendering = false; it.budget = () => ({ fps: 8, dev: { pace: 8, cross: 0, cov: 0, why: '' } });
it.state.dial = 'flux'; it.state.tab = 'hands'; it.state.playing = false; it.state.hidden = {};
it.state.designs = { flux: E.canonDesign(JSON.parse(JSON.stringify(E.PRESETS.flux.design))) };
const hr = it.driveCols3('hour', { path: 'retrograde', start: 210, span: 120, cadence: 'jump' });
ok(hr.motion === 'retrograde' && hr.span === 1200 && hr.zero === 2100 - 3600 && hr.cadence === 3 && hr.src === 'hour12' && hr.vmax === 12,
   'a fan from 210 encodes zero -1500: under the span, so the dial’s absolute clamp never fires', JSON.stringify(hr));
// the firmware's arithmetic over that encoding lands where the preview drew it
const fw = (c, u) => { let t = c.zero + (u * c.span) / c.vmax; if (c.motion === 'retrograde' && ((c.span >= 0 && t > c.span) || (c.span < 0 && t < c.span))) t = c.span; return ((t % 3600) + 3600) % 3600 / 10; };
ok([0, 3, 6, 9, 12].every((u) => near(fw(hr, u), deg(E.motionPose({ path: 'retrograde', start: 210, span: 120 }, 12, u).a) % 360, 1e-6)),
   'the dial and the preview agree along the whole fan');
const dc = it.driveCols3('minute', { path: 'disc', start: 90, cadence: 'jump' });
ok(dc.motion === 'rotate' && dc.span === -3600 && dc.zero === 900, 'a disc encodes a whole turn backwards from its index', JSON.stringify(dc));
ok([0, 15, 30, 59].every((u) => near(fw(dc, u), ((deg(E.motionPose({ path: 'disc', start: 90 }, 60, u).a) % 360) + 360) % 360, 1e-6)), 'the dial and the preview agree around the disc');
const sc = it.driveCols3('second', { path: 'slide-x', from: -70, travel: 160, cadence: 'step', steps: 1 });
ok(sc.motion === 'slide-x' && sc.span === 160 && sc.zero === -70 && sc.cadence === 2 && sc.cad_arg === 1, 'a slide encodes its start as zero and its travel as span', JSON.stringify(sc));
ok(it.driveCols3('minute', { path: 'retrograde', start: 30, span: 120 }).cadence === 1, 'an inherited cadence on a path of its own is written as sweep');
ok(it.driveCols3('hour', { path: 'rotate' }) === null, 'a plain rotation writes nothing');
ok(it.driveCols3('hour', { path: 'retrograde', start: 210, span: 120, cadence: 'jump', ease: 200 }).ease === 200, 'the bake carries the ease');
// D43 P5c: Flux's hands are comps; name them by what they show
const hid = (k) => it.design().comps.find((c) => c.type === 'hand' && c.shows === k).id;
const pp = it.pointerRows3(it.design(), { rows: [] }), pr = (k) => pp.rows.find((r) => r.id === hid(k)) || {};
ok(pp.rows.length === 3 && pr('hour').motion === 'retrograde' && pr('minute').motion === 'retrograde' && pr('second').motion === 'slide-x' && pr('second').angle === 0 && pp.rows.every((r) => r.freeze),
   'Flux: every hand rides the pointer table -- two fans and a slide -- lifted at rest', JSON.stringify(pp.rows.map((r) => r.id + ':' + r.motion)));
// a hand's panel shows when that hand is selected, on Parts
const panel = (k) => { it.setState({ sel: hid(k), selKind: 'hand', step: 'parts', tab: 'hands' }); return it.renderVals().handRows[0]; };
const hrRow = panel('hour');
ok(hrRow && hrRow.canMove && hrRow.pathOpts.length === 1 && /from its fan/.test(hrRow.pathOpts[0].label), 'a fan\u2019s hand shows its path as the fan\u2019s', hrRow && hrRow.pathOpts.map((o) => o.label).join('|'));
ok(hrRow.cadOpts.some((o) => o.v === 'jump' && o.sel) && !hrRow.cadOpts.some((o) => o.v === 'stop-to-go'), 'and the cadence; stop-to-go only on a seconds hand');
hrRow.onPath({ target: { value: 'disc' } });
ok(!(global.hand3(it.design(), hid('hour')).motion || {}).path && global.hand3(it.design(), hid('hour')).motion.cadence === 'jump', 'a face-driven hand cannot be given a path of its own; its cadence stays');
const secRow = panel('second');
ok(/along its slider/.test(secRow.pathOpts[0].label) && secRow.showSpan && secRow.spanLabel === 'Travel', 'a slider\u2019s hand takes its path from it and offers its travel as an override', secRow.pathOpts[0].label);
// a hand that names a fan path of its own (an older design) keeps it, and may still change it
it.setState({ designs: { flux: E.canonDesign({ print: 'none', comps: [{ id: 'old', type: 'hand', host: 'dial', shows: 'hour', motion: { path: 'retrograde', start: 210, span: 120, cadence: 'jump' } }] }) }, sel: 'old', selKind: 'hand', step: 'parts', tab: 'hands' });
const oldRow = it.renderVals().handRows[0];
ok(oldRow.pathOpts.some((o) => o.v === 'retrograde' && o.sel && /its own scale/.test(o.label)) && oldRow.pathOpts.some((o) => o.v === 'disc'), 'an older hand keeps its own fan path, marked, beside rotate and disc');
oldRow.onPath({ target: { value: 'disc' } });
ok(global.hand3(it.design(), 'old').motion.path === 'disc' && global.hand3(it.design(), 'old').motion.cadence === 'jump', 'picking a path keeps the cadence');

// ---- D42: the v6 row the bake writes lands where the preview draws ----
// The bake turns each authored motion into a v6 row (GorbBake.v6Drive: two poses and an
// end rule); the dial poses that row by orb_clock3.c's row_pose, whose arithmetic
// tools/host_drive_test.py pins against hand-worked vectors: t = (u - vmin) / (vmax -
// vmin), a wrap takes t modulo one and a flyback clamps it, the angle is zero + t * span
// and the pivot's travel t * (dx, dy), plus the placement shift a slide's start became.
// Every motion hand of the motion presets, sampled across its whole domain.
{
  const B = require(require('./runtime-paths.js').BAKER);
  const v6Pose = (r, u) => {
    let t = (u - r.vmin) / (r.vmax - r.vmin);
    if (r.end === 0) t -= Math.floor(t); else t = Math.min(1, Math.max(0, t));
    const a = r.zero + t * r.span;
    return { deg: (((a / 10) % 360) + 360) % 360, x: r.x + t * r.dx, y: r.y + t * r.dy };
  };
  const DOM = { hour: 12, minute: 60, second: 60 };
  let rows = 0, bad = [];
  // the engine's own motion for each hand: a face-made fan or slide included (D43 P5d.2)
  for (const key of ['flux', 'orbit', 'station']) {
    for (const hd of faceOf(key).hands) {
      const id = hd.id, m = hd.motion, role = DOM[hd.kind] ? hd.kind : null;
      if (!m || !role || !E.motionOn(m)) continue;
      const cols = it.driveCols3(role, m);
      if (!cols) continue;                       // a cadence-only hand keeps its slot's turn
      const r = B.v6Drive(cols.motion, cols, 0, 0);
      rows++;
      const n = DOM[role];
      for (let k = 0; k <= 4 * n; k++) {
        const u = k / 4;
        const want = E.motionPose(m, n, u), got = v6Pose(r, u);
        const wdeg = ((((want.a || 0) * 180 / Math.PI) % 360) + 360) % 360;
        const dd = Math.abs(((got.deg - wdeg + 540) % 360) - 180);
        const wx = (want.d && want.d[0]) || 0, wy = (want.d && want.d[1]) || 0;
        if (dd > 1e-6 || Math.abs(got.x - wx) > 1e-6 || Math.abs(got.y - wy) > 1e-6) {
          bad.push(key + ' ' + id + ' u=' + u + ' preview ' + wdeg.toFixed(3) + '/(' + wx + ',' + wy +
                   ') row ' + got.deg.toFixed(3) + '/(' + got.x + ',' + got.y + ')');
          break;
        }
      }
    }
  }
  ok(rows >= 5 && !bad.length, 'D42: every motion hand of Flux, Orbit and Station bakes to a v6 row that poses where the preview draws, across its whole domain',
     rows + ' rows' + (bad.length ? '; ' + bad[0] : ''));
}

console.log(fails ? fails + ' FAILED of ' + checked : 'all green (' + checked + ' checks)');
process.exit(fails ? 1 : 0);
