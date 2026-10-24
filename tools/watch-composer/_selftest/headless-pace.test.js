// Pace suite: what the seconds sweep costs the glass, locked as decisions a pixel diff
// cannot see. _devPace is the crossed-plate device-pace model in the Component; the field
// anchors it must reproduce were measured live on the orb (07d3411 and the 2026-10-24
// modern/neon pair), so the numbers below are the trip for the round-to-integer change:
// the old model floor-snapped raw fps to the [15,12,8,4] rungs and called both modern
// (raw 11.4) and neon (raw 11.1) "8"; the device actually held 10 and 12. Rounding to the
// nearest whole fps — clamped to the scheduler floor and the tier ceiling — is the fix.
// The mount mirrors probe_pace.cjs; run it standalone too, but this file is the enforced
// version the headless-test.js runner picks up.
require('./headless-stub.js');
global.React = { createRef: () => ({ current: null }) };
require('./component.js');

let fails = 0, checked = 0;
const ok = (cond, name, extra) => { checked++; console.log((cond ? 'PASS ' : 'FAIL ') + name + (extra ? '  [' + extra + ']' : '')); if (!cond) fails++; };
const eq = (a, b, name) => ok(a === b, name, JSON.stringify(a) + ' vs ' + JSON.stringify(b));

const DCLogic = function () { };
DCLogic.prototype.setState = function (p) { Object.assign(this.state, typeof p === 'function' ? p(this.state) : p); };
const Comp = global.COMP_CLASS(DCLogic);
const E = global.WatchEngine;

// A mounted component reading a real preset, priced at the 15 tier as the probe does.
const mount = (id) => { const i = new Comp({}); i.ready = true; i.state.dial = id; return i; };
const paceAt = (id, b) => mount(id)._devPace(b || { fps: 15, measured: true, car: false });

// ---- the panel anchors, unchanged by the rounding edit -------------------------------
eq(paceAt('aviator').pace, 15, 'aviator (bare ring, cov 0) still holds the full tier');
eq(paceAt('sector').pace, 15, 'sector (bare ring, cov 0) still holds the full tier');
const pp = paceAt('perpetual');
eq(pp.pace, 4, 'perpetual (four banked gilt subdials, cov 0.32) still clamps to the floor 4');
ok(pp.cross === 4 && pp.cov > 0.3, 'perpetual prices four crossed plates at cov ~0.32', pp.cross + ' cov=' + pp.cov.toFixed(4));

// ---- the rounding edit: integer fps, no longer pinned to a rung ----------------------
const mp = paceAt('modern');
eq(mp.pace, 11, 'modern (banked running-seconds subdial) rounds to 11, not the old snapped 8');
ok(mp.pace % 1 === 0 && mp.pace > 8, 'a raw near 11.4 reads as 11, an off-rung whole number the rungs could not say', mp.pace);
eq(paceAt('diver').pace, 15, 'diver (a single thin date window, cov 0.01) rounds to 15, not the snapped 12');

// ---- a caged carrier and a non-sweep tempo are exempt --------------------------------
eq(paceAt('modern', { fps: 15, measured: true, car: true }).pace, 15, 'a caged dial is exempt: _devPace quotes the tier untouched');

// ---- a static single plate under a MOVING twin still pays K_TWIN ---------------------
// The refuted plan step: do NOT narrow K_TWIN to moving plates only. Neon (a static moon
// crossed by a moving hand twin) measured 12 under shadows on, and perpetual's static
// calendar subdials clamp to 4 for the same reason. So the gate is shOn (the runtime
// twin), not the plate's own animation. Build the static-moon dial both ways.
const staticMoon = {
  print: 'baton', centre: { hour: 1, minute: 1, second: 1 },
  comps: [{ id: 'c9', type: 'moon', host: 'dial', angle: 180, dist: 100, rot: 0, size: 64, style: 'classic' }],
};
const iS = mount('modern');
iS.state.designs.modern = staticMoon;
const onSh = iS._devPace({ fps: 15, measured: true, car: false });   // DEF leaves shMove/shOp at 1
iS.setCfg('shMove', 0);
const offSh = iS._devPace({ fps: 15, measured: true, car: false });
ok(onSh.cross === 1 && onSh.cov > 0, 'a static moon crossed by the sweep is priced (cov>0)', 'cov=' + onSh.cov.toFixed(4));
ok(onSh.pace < offSh.pace,
  'a static plate under a moving twin pays the doubled bill: shadows-off is strictly faster',
  onSh.pace + ' vs ' + offSh.pace);
// The gap is a whole re-blit, not a rung skip: both are whole integers in [4,15].
ok(onSh.pace >= 4 && offSh.pace <= 15 && onSh.pace % 1 === 0 && offSh.pace % 1 === 0,
  'both static-plate paces are whole integers inside the floor and ceiling', onSh.pace + ',' + offSh.pace);

// ---- ticked tempos pay only on the beat: no crossed-plate tax ------------------------
const iT = mount('modern');
iT.setCfg('tick', '4');
const tPace = iT._devPace({ fps: 15, measured: true, car: false });
eq(tPace.pace, 15, 'a ticked second hand is exempt from the sweep tax (it holds between beats)');
eq(tPace.cross, 0, 'and the tick path charges no crossed plate');

console.log('\n' + (fails ? fails + ' FAILED' : 'all green (' + checked + ' checks)'));
process.exit(fails ? 1 : 0);