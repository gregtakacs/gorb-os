// Regression sweep: with no art in play, every preset must still build every layer,
// keep its hand geometry, and leave the procedural sprite path alone.
require('./headless-stub.js');
const E = global.WatchEngine;
let fails = 0;
let checked = 0;
const ok = (c, n, x) => { checked++; if (!c) { fails++; console.log('FAIL ' + n + (x ? '  [' + x + ']' : '')); } };

const OPTS = { finish: 'train', plate: 'train', tick: 'sweep', glow: 1, beats: 1, spokes: 'train', etching: 'none', pinion: 'on', center: 'jewel', temp: 2200, innerAngle: 90, outerLight: 1, outerAngle: 315, outerTemp: 2200, trainAngle: 0, plateAngle: 0, plateScale: 1, hue: 30, hueSat: 0, outerHue: 210, outerSat: 0, orbD: 128, orbTip: 194, cageD: 107, rearFinish: 'train', shAngle: -1, shDist: 1, shBlur: 1, shOp: 1, frames: 8 };
const names = Object.keys(E.PRESETS);
console.log(names.length + ' presets');
names.forEach((k) => {
  const P = E.PRESETS[k];
  const d = E.canonDesign(JSON.parse(JSON.stringify(P.design)));
  let fc;
  try { fc = E.face(Object.assign({}, OPTS, { design: d, train: P.train || 'classic', hands: P.hands, handFinish: P.handFinish })); }
  catch (e) { fails++; console.log('FAIL ' + k + ' threw ' + e.message + '\n' + String(e.stack).split('\n').slice(1, 3).join('\n')); return; }
  ok(!!fc.dial && !!fc.glass && !!fc.outer, k + ': a layer failed to build');
  ok(Array.isArray(fc.hands) && fc.hands.length > 0, k + ': no hands built');
  ok(fc.hands.every((h) => h.sp && h.sp.c && isFinite(h.sp.px) && isFinite(h.sp.py)), k + ': a procedural sprite lost its {c,px,py} contract');
  ok(fc.hands.every((h) => typeof h.art !== 'string' || E.assetReady(h.art)), k + ': a hand claims art that is not there');
  fc.subLayers.forEach((l) => ok(!!l.canvas, k + ': subdial layer ' + l.id + ' has no canvas'));
  const mv = E.movement(Object.assign({}, OPTS, { train: P.train || 'classic' }));
  ok(!!mv.plate && typeof mv.render === 'function', k + ': movement did not build');
  try { mv.render(0); } catch (e) { fails++; console.log('FAIL ' + k + ' render(0) threw ' + e.message); }
});

// the procedural hand contract is exactly what the kit manifest reads
const h = E.face(Object.assign({}, OPTS, { design: E.canonDesign({ print: 'roman', comps: [{ id: 's1', type: 'subdial', angle: 30, dist: 100, size: 60, scale: 'seconds', style: 'guilloche' }], centre: { hour: 1, minute: 1, second: 1 } }), train: 'classic' }));
const keys = (o) => Object.keys(o).sort().join(',');
ok(keys(h.hands[0].sp).indexOf('c,px,py') === 0, 'procedural sprite still answers c,px,py first', keys(h.hands[0].sp));
ok(h.hands.every((x) => !x.art), 'a procedural hand carries no art record');
ok(h.subLayers.every((l) => !l.transparent), 'no plate is transparent in this design');
// an art-backed hand answers the same three fields and nothing the manifest would trip on
require('./headless-stub.js').SIZES['data:image/png;base64,' + Buffer.from('hand').toString('base64')] = [40, 12];
E.assetRestore([{ id: 'hk-1', name: 'hk.png', kind: 'png', dataUrl: 'data:image/png;base64,' + Buffer.from('hand').toString('base64') }]);
setTimeout(() => {
  const d2 = E.canonDesign({ print: 'roman', comps: [], centre: { hour: 1, minute: 1 }, hands: { 'centre-hour': { art: 'hk-1', pivotX: 20, pivotY: 20 } } });
  const f2 = E.face(Object.assign({}, OPTS, { design: d2, train: 'classic' }));
  const hh = f2.hands.find((q) => q.id === 'centre-hour');
  ok(hh && hh.art && hh.art.asset === 'hk-1', 'the art hand is marked as art-backed');
  ok(hh && keys({ c: hh.sp.c, px: hh.sp.px, py: hh.sp.py }).indexOf('c,px,py') === 0, 'the art hand answers the same contract as the procedural one');
  ok(hh && hh.cap === 0, 'no hub cap is drawn over an art hand');
  // A hand sprite and a flare read the SAME registry the composer persists, through the
// legacy IMAGES view. Proving that bridge holds is the point: the flare path predates the
// registry and must keep drawing uploaded art without knowing about it.
const u3 = 'data:image/png;base64,' + Buffer.from('mark').toString('base64');
require('./headless-stub.js').SIZES[u3] = [24, 24];
E.assetRestore([{ id: 'fl-1', name: 'mark.png', kind: 'png', dataUrl: u3 }]);
setTimeout(() => {
  ok(E.IMAGES['fl-1'] !== undefined && !!E.IMAGES['fl-1'], 'a registered asset is visible through the legacy IMAGES view');
  const d4 = E.canonDesign({ print: 'roman', comps: [{ id: 'f1', type: 'flare', host: 'dial', angle: 0, dist: 60, size: 20, style: 'print', icon: 'image', img: 'fl-1', color: 'auto' }], centre: { hour: 1 } });
  const f4 = E.face(Object.assign({}, OPTS, { design: d4, train: 'classic' }));
  ok(!!f4.dial, 'a flare with registry-backed art still renders');
  E.assetDrop('fl-1');
  ok(!E.IMAGES['fl-1'] && !E.assetKnown('fl-1'), 'assetDrop clears both views, so nothing stale can draw');
  // a sweep that checked nothing must not report success: if the preset list or the
  // hand set ever came back empty, this suite would otherwise print "clean" and hide it
  if (checked < 60) { fails++; console.log('FAIL only ' + checked + ' checks ran; the sweep is not exercising the presets'); }
  console.log(fails ? fails + ' FAILED' : 'regression sweep clean (' + checked + ' checks, ' + names.length + ' presets)');
  process.exit(fails ? 1 : 0);
}, 15);
}, 15);
