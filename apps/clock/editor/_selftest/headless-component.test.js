// Component-level headless test: drives the new art methods on the real class body.
// Phase 1 here; phase 2 (manifest/bundle) continues in headless-component.kit.js via global.__T.
require('./headless-stub.js');
const S = require('./headless-stub.js');
const E = global.WatchEngine;
global.React = { createRef: () => ({ current: null }) };
require('./component.js');
let fails = 0, checked = 0;
const ok = (c, n, x) => { checked++; console.log((c ? 'PASS ' : 'FAIL ') + n + (x ? '  [' + x + ']' : '')); if (!c) fails++; };
const eq = (a, b, n) => ok(JSON.stringify(a) === JSON.stringify(b), n, JSON.stringify(a) + ' vs ' + JSON.stringify(b));
const wait = (ms) => new Promise((r) => setTimeout(r, ms));
const DCLogic = function () { };
DCLogic.prototype.setState = function (p) { Object.assign(this.state, typeof p === 'function' ? p(this.state) : p); };
const Comp = global.COMP_CLASS(DCLogic);
const mk = () => { const i = new Comp({}); i.ready = true; i.state.dial = 't'; i.state.designs.t = { print: 'roman', comps: global.hands3({ shows: 'hour', id: 'centre-hour' }, { shows: 'minute', id: 'centre-minute' }) }; return i; };
const urlFor = (tag, w, h) => { const u = 'data:image/png;base64,' + Buffer.from(tag).toString('base64'); S.SIZES[u] = [w, h]; return u; };
const txt = (s) => ({ name: 'x', _url: 'data:text/plain;base64,' + Buffer.from(s).toString('base64') });

(async () => {
  const U_BG = urlFor('bg-src', 500, 500), U_H = urlFor('hand-src', 120, 40), U_F = urlFor('flare-src', 64, 64);
  E.assetRestore([
    { id: 'bg-1', name: 'bg.png', kind: 'png', dataUrl: U_BG },
    { id: 'hand-1', name: 'hand.png', kind: 'png', dataUrl: U_H },
    { id: 'flare-1', name: 'mark.svg', kind: 'svg', dataUrl: U_F },
    { id: 'sf-1', name: 'face.png', kind: 'png', dataUrl: U_BG }]);
  await wait(20);
  const i1 = mk(), d = i1.state.designs.t;
  d.bgArt = 'bg-1';
  d.sfArt = 'sf-1';
  Object.assign(global.hand3(d, 'centre-minute'), { art: 'hand-1', pivotX: 60, pivotY: 20 });
  d.comps = d.comps.concat([{ id: 'c9', type: 'flare', icon: 'image', img: 'flare-1' }]);
  const refs = i1.artRefs(d);
  eq(refs.map((r) => r.slot).sort(), ['bg', 'comp:c9', 'hand:centre-minute', 'sf'], 'artRefs finds both backdrop slots, the hand sprite and the flare art');
  eq(refs.map((r) => r.usedBy).sort(), ['flare #c9', 'hand centre-minute', 'painted surface', 'plate backdrop'], 'each reference names its slot in the words the UI uses');
  ok(i1.artNote(refs).indexOf('MISSING') < 0, 'artNote is quiet while every reference is held', i1.artNote(refs));
  ok(/4\/4 art assets held/.test(i1.artNote(refs)), 'artNote counts what it holds');
  // one image filling the plate AND the dial face is one stored asset, not two
  d.sfArt = 'bg-1';
  const rShared = i1.artRefs(d);
  ok(rShared.length === 4 && /3\/3 art assets held/.test(i1.artNote(rShared)),
    'artNote counts distinct assets when two slots share one file: ' + i1.artNote(rShared));
  d.sfArt = 'sf-1';
  d.comps.find((c) => c.id === 'c9').img = 'gone-77';
  const r2 = i1.artRefs(d);
  ok(/MISSING.*flare #c9/.test(i1.artNote(r2)), 'artNote names the slot whose bytes are not here', i1.artNote(r2));
  d.comps.find((c) => c.id === 'c9').img = 'flare-1';

  // the browser keeps no copy of the art (owner ruling 2026-11-04): a library draft and the
  // theme carry it, and the readout says it lasts this session only
  ok(typeof i1.saveArt === 'undefined' && typeof i1.loadArt === 'undefined', 'the editor has no art store to save to or load from');
  ok(/this session only — save it to the library, or send or download the theme, to keep it/.test(i1.artNote(refs)), 'artNote says the art lasts this session only', i1.artNote(refs));
  global.localStorage.removeItem('wc.assets');
  const r0 = i1.applyDesignContent({ dial: 'skeleton', design: E.canonDesign(JSON.parse(JSON.stringify(E.PRESETS.skeleton.design))),
    assets: [{ id: 'bg-9', name: 'bg9.png', kind: 'png', dataUrl: U_BG }] }, 'x.orbdesign');
  ok(E.assetKnown('bg-9') && global.localStorage.getItem('wc.assets') === null, 'opening a design takes its art into the session and writes none of it to the browser', String(r0));
  // ---- the two shadow switches, and which one the pace readout answers to --------
  const iS = mk();
  iS.castShadowsOn(false);
  eq([iS.cfg().shMove, iS.cfg().shOp], [0, 1], 'Cast shadows off writes the moving gate and leaves the art alone');
  iS.castShadowsOn(true);
  eq(iS.cfg().shMove, 1, 'and back on restores the twins without disturbing anything else');
  iS.setCfg('shOp', 0.5);
  iS.staticShadowsOn(false);
  // P5d.3j: relief has its own gate; it no longer zeroes the opacity the cast shadows share
  eq([iS.cfg().shRelief, iS.cfg().shOp, iS.cfg().shMove], [0, 0.5, 1], 'Plate relief off closes its own gate and leaves the cast shadows on');
  iS.staticShadowsOn(true);
  eq([iS.cfg().shRelief, iS.cfg().shOp], [1, 0.5], 'and back on, the strength the author left is untouched');
  eq(iS.opts(null, true).shRelief, 1, 'opts carries the relief gate to the engine');
  eq(iS.opts(null, true).shMove, 1, 'opts carries the moving gate, so the preview and the bake see it');
  ok(iS.mvKey(true).shMove === undefined, 'the moving gate is no reason to resample the bank');
  // ---- the baked preview plane (docs/history/theme-preview-plan.md) ------------------
  // Every bake carries an ID photo: the dial at the design pose, nearest halved
  // to 232². The picker page, the glass, and the hub all paint this one plane, so
  // the plane's shape, pose, matte, determinism, and serial fold are pinned here.
  {
    const iv = mk();
    const SRC = new Uint8ClampedArray(466 * 466 * 4);
    for (let y = 0; y < 466; y++) for (let x = 0; x < 466; x++) {
      const i = (y * 466 + x) * 4;
      SRC[i] = x & 255; SRC[i + 1] = y & 255; SRC[i + 2] = (x + y) & 255; SRC[i + 3] = 255;
    }
    for (let y = 0; y < 466; y++) for (let x = 400; x < 466; x++) SRC[(y * 466 + x) * 4 + 3] = 0;
    iv.frameCanvas = (f, hidden, date) => { iv._pose = [f, date]; return {
      width: 466, height: 466, getContext: () => ({ getImageData: () => ({ data: SRC }) }) }; };
    const plane = iv.previewPlane3();
    eq([plane.w, plane.h, plane.rgba.length], [232, 232, 232 * 232 * 4], 'previewPlane3 ships a 232² RGBA plane');
    eq(iv._pose[0], 0, 'the plane composes at keyframe 0, never the live transport');
    ok(iv._pose[1].getHours() === 10 && iv._pose[1].getMinutes() === 9 && iv._pose[1].__moon === 0.34,
       'the plane composes at the design pose: 10:09:36, moon 0.34');
    const oi = (13 * 232 + 7) * 4;
    ok(plane.rgba[oi] === 15 && plane.rgba[oi + 1] === 27 && plane.rgba[oi + 3] === 255,
       'nearest-half samples the odd texel centre of each 2x2 cell');
    const bi = (5 * 232 + 210) * 4;   // source x 421: inside the transparent band
    ok(plane.rgba[bi] === 0 && plane.rgba[bi + 1] === 0 && plane.rgba[bi + 2] === 0 && plane.rgba[bi + 3] === 255,
       'transparent art mattes to black, never to alpha');
    const plane2 = iv.previewPlane3();
    let same = plane.rgba.length === plane2.rgba.length;
    for (let i = 0; same && i < plane.rgba.length; i += 601) if (plane.rgba[i] !== plane2.rgba[i]) same = false;
    ok(same, 'the same dial hands back identical bytes, so the serial stays a quiet no-op');
    const BKR = require(require('./runtime-paths.js').BAKER);
    const baseOpts = () => ({ plate: { rgba: new Uint8Array([1, 2, 3, 4, 5, 6, 7, 8]), w: 2, h: 2 }, x: 0, y: 0, w: 2, h: 2 });
    const s1 = iv.serialContent3(BKR, 'nm', 16, [], { carrier: 'still' }, baseOpts());
    const s2 = iv.serialContent3(BKR, 'nm', 16, [], { carrier: 'still' }, Object.assign(baseOpts(), { preview: plane }));
    const s3 = iv.serialContent3(BKR, 'nm', 16, [], { carrier: 'still' },
      Object.assign(baseOpts(), { preview: { rgba: new Uint8Array(232 * 232 * 4), w: 232, h: 232 } }));
    ok(s1 !== s2, 'a bake with a preview serializes apart from one without it');
    ok(s2 !== s3, 'and preview pixels alone move the witness: a re-posed dial re-stores');
  }

  global.__T = { ok, eq, wait, mk, E, S, txt, i1, d, refs, fails: () => fails, checked: () => checked };
  require('./headless-component.kit.js');
})();
