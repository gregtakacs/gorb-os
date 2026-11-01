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

// ---- the presets that exercise them ----
const OPTS = { finish: 'train', plate: 'train', tick: 'sweep', glow: 1, temp: 2200 };
const faceOf = (k) => { const P = E.PRESETS[k]; return E.face(Object.assign({}, OPTS, { design: E.canonDesign(JSON.parse(JSON.stringify(P.design))), train: P.train || 'classic', hands: P.hands, handFinish: P.handFinish })); };
const flux = faceOf('flux'), orbit = faceOf('orbit');
const fh = (f, id) => f.hands.find((h) => h.id === id);
ok(fh(flux, 'centre-hour').motion.path === 'retrograde' && fh(flux, 'centre-minute').motion.path === 'retrograde', 'Flux: hour and minute are retrograde fans');
ok(typeof fh(flux, 'centre-second').slide === 'function', 'Flux: the seconds slide');
const fs = fh(flux, 'centre-second'), xy0 = E.handXY(fs, at(1, 0, 0)), xy30 = E.handXY(fs, at(1, 0, 30));
ok(near(xy30[0] - xy0[0], 80) && xy30[1] === xy0[1], 'Flux: the seconds bar travels 80 px across half a minute', JSON.stringify([xy0, xy30]));
ok(near(deg(E.handAngle(fh(flux, 'centre-hour'), at(6, 59, 0))), 270), 'Flux: the jumping hour reads 6 at 06:59, on its fan');
ok(['centre-hour', 'centre-minute', 'centre-second'].every((id) => fh(orbit, id).disc && fh(orbit, id).noShadow), 'Orbit: three discs, none casting');
const od = fh(orbit, 'centre-minute');
ok(near(deg(E.handAngle(od, at(3, 15, 59))) % 360, 0), 'Orbit: the jumping minute disc holds 15 under the 3 o’clock index');

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
const pp = it.motionRows3(it.design(), { rows: [], refuses: [] });
ok(pp.rows.length === 1 && pp.rows[0].id === 'centre-second' && pp.rows[0].motion === 'slide-x' && pp.rows[0].freeze,
   'Flux: the sliding seconds joins the pointer table, lifted at rest', JSON.stringify(pp.rows.map((r) => r.id)));
const rows = it.renderVals().handRows;
const hrRow = rows.find((q) => q.label === 'Centre hour');
ok(hrRow && hrRow.canMove && hrRow.pathOpts.some((o) => o.v === 'retrograde' && o.sel), 'the Hands tab offers the path, set to the fan', hrRow && hrRow.motionNote);
ok(hrRow.cadOpts.some((o) => o.v === 'jump' && o.sel) && !hrRow.cadOpts.some((o) => o.v === 'stop-to-go'), 'and the cadence; stop-to-go only on a seconds hand');
hrRow.onPath({ target: { value: 'disc' } });
ok(it.design().hands['centre-hour'].motion.path === 'disc' && it.design().hands['centre-hour'].motion.cadence === 'jump', 'picking a path keeps the cadence');
const secRow = it.renderVals().handRows.find((q) => q.label === 'Centre second');
ok(/pointer row/.test(secRow.motionNote) && secRow.aLabel === 'From' && secRow.bLabel === 'Travel', 'a slide says it bakes as a pointer and asks for its travel', secRow.motionNote);

console.log(fails ? fails + ' FAILED of ' + checked : 'all green (' + checked + ' checks)');
process.exit(fails ? 1 : 0);
