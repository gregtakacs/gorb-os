// Engine suite: the decisions watch-engine.js makes about uploaded art, with no browser.
// Pins that an art-free design serializes byte-identically (so no Design ID already in
// the field changes), that a transparent subdial ships no plate and punches nothing, and
// that the registry, the fit gate and the sprite pivot maths behave.
const { E, SIZES } = require('./headless-stub.js');
let fails = 0;
let checked = 0;
const ok = (cond, name, extra) => { checked++; console.log((cond ? 'PASS ' : 'FAIL ') + name + (extra ? '  [' + extra + ']' : '')); if (!cond) fails++; };
const eq = (a, b, name) => ok(JSON.stringify(a) === JSON.stringify(b), name, JSON.stringify(a) + ' vs ' + JSON.stringify(b));
const base = { print: 'roman', color: 'cream', comps: [{ id: 'c1', type: 'subdial', angle: 30, dist: 100, size: 60, scale: 'seconds', style: 'enamel' }], centre: { hour: 1, minute: 1 } };

// ---- canonDesign: an art-free design must be byte-identical to the old one ---------
const clean = E.canonDesign(JSON.parse(JSON.stringify(base)));
const withArt = E.canonDesign(JSON.parse(JSON.stringify(Object.assign({}, base, {
  bgArt: '', bgFit: 'cover', bgX: 0, bgY: 0, bgSize: 442, bgOpacity: 1,
  hands: { 'centre-hour': { art: '', pivotX: 0, pivotY: 0 } },
}))));
eq(withArt, clean, 'canonDesign: empty art fields vanish, so an old Design ID still matches');
const placed = E.canonDesign(JSON.parse(JSON.stringify(Object.assign({}, base, {
  bgArt: 'sky-abcd1234', bgFit: 'contain', bgX: 12, bgSize: 442, hands: { 'centre-hour': { art: 'h-11112222', pivotX: 20, pivotY: 33 } },
}))));
ok(placed.bgArt === 'sky-abcd1234' && placed.bgFit === 'contain' && placed.bgX === 12, 'canonDesign: set bg fields survive');
ok(!('bgSize' in placed), 'canonDesign: the default bgSize 442 is deleted');
ok(placed.hands['centre-hour'].art === 'h-11112222' && placed.hands['centre-hour'].pivotY === 33, 'canonDesign: hand art and authored pivot survive');
const zeroPivot = E.canonDesign(JSON.parse(JSON.stringify(Object.assign({}, base, { hands: { 'centre-hour': { art: 'h-1', pivotX: 0, pivotY: 0 } } }))));
ok(zeroPivot.hands['centre-hour'].pivotX === undefined, 'canonDesign: a 0,0 pivot falls back to hub-ink auto');
const noArt = E.canonDesign(JSON.parse(JSON.stringify(Object.assign({}, base, { hands: { 'centre-hour': { pivotX: 44, pivotY: 9 } } }))));
ok(!noArt.hands || !noArt.hands['centre-hour'] || noArt.hands['centre-hour'].pivotX === undefined, 'canonDesign: a pivot with no art reference vanishes');
// the surface slot is its own decision: filling it must not leave plate keys on the design
const sfOnly = E.canonDesign(JSON.parse(JSON.stringify(Object.assign({}, base, { sfArt: 'face-99990000', sfFit: 'contain', sfX: 7, sfSize: 442, sfOpacity: 1 }))));
ok(sfOnly.sfArt === 'face-99990000' && sfOnly.sfFit === 'contain' && sfOnly.sfX === 7, 'canonDesign: set sf fields survive');
ok(!('sfSize' in sfOnly) && !('sfOpacity' in sfOnly), 'canonDesign: default sf size and opacity are deleted');
ok(!('bgArt' in sfOnly) && !('bgFit' in sfOnly) && !('bgX' in sfOnly), 'canonDesign: a surface-only design carries no plate-backdrop keys at all');
const sfBlank = E.canonDesign(JSON.parse(JSON.stringify(Object.assign({}, base, { sfArt: '', sfFit: 'cover', sfX: 3, sfY: 4, sfSize: 300, sfOpacity: 0.5 }))));
eq(sfBlank, clean, 'canonDesign: an empty sf reference drops the whole group, stale offsets included');
ok(E.artInDial({}, 'no-such-asset', {}) === false, 'artInDial refuses an id with no bytes instead of drawing nothing silently');
ok(!!E.COMP_STYLES.subdial.transparent, 'COMP_STYLES.subdial carries the transparent entry');

// ---- layout()/buildSubdials(): the plate really is gone ----------------------------
const faceOf = (style, bgId) => {
  const d = E.canonDesign({ print: 'roman',
    comps: [{ id: 'c1', type: 'subdial', angle: 30, dist: 100, size: 60, scale: 'seconds', style: style, open: 0.5 },
      { id: 'c2', type: 'window', host: 'c1', angle: 0, dist: 0, size: 1, shows: 'date' }],
    centre: { hour: 1, minute: 1 } });
  return E.face(Object.assign({ design: d, finish: 'brass', train: 'classic', frames: 16, tick: 'sweep', beats: 1 }, bgId ? { bgArt: bgId, bgSize: 442 } : {}));
};
const solid = faceOf('enamel'), ghost = faceOf('transparent');
ok(solid.lay.dialHoles.filter((h) => h.hidden).length === 1, 'layout: an open enamel subdial still punches its hidden centre');
ok(ghost.lay.dialHoles.filter((h) => h.hidden).length === 0, 'layout: a transparent subdial punches nothing, so art under it survives');
ok(ghost.lay.dialHoles.filter((h) => h.seal).length >= 1, 'layout: a window hosted on a transparent subdial is carried by the dial');
ok(solid.lay.subHoles['c1'] && solid.lay.subHoles['c1'].length === 1, 'layout: a window on a real subdial still cuts that subdial');
ok(ghost.subLayers.length === 1 && ghost.subLayers[0].transparent === true, 'buildSubdials: the transparent plate is flagged');
ok((ghost.handOrder || []).indexOf('c1') >= 0, 'the pointer hand on a transparent subdial is still mounted', (ghost.handOrder || []).join(','));
ok(!!(ghost.hands || []).find((h) => h.id === 'c1'), 'the transparent subdial keeps its hand record for the kit');
console.log('   solid dialHoles=' + JSON.stringify(solid.lay.dialHoles.map((h) => [h.kind, !!h.hidden, !!h.seal])));
console.log('   ghost dialHoles=' + JSON.stringify(ghost.lay.dialHoles.map((h) => [h.kind, !!h.hidden, !!h.seal])));
ok(solid.lay.dialHoles.filter((h) => h.hidden).length - ghost.lay.dialHoles.filter((h) => h.hidden).length === 1,
  'exactly one punch disappears when the plate becomes transparent (the open centre)');
ok(ghost.lay.subHoles['c1'].length === 0, 'nothing is left to cut on a plate that does not ship');

// ---- the art registry --------------------------------------------------------------
const mk = (id, w, h, kind) => { const u = 'data:image/' + (kind === 'svg' ? 'svg+xml' : 'png') + ';base64,' + Buffer.from(id).toString('base64'); SIZES[u] = [w, h]; return u; };
const urlA = mk('alpha-1', 90, 40, 'png');
E.assetRestore([{ id: 'alpha-1', name: 'alpha.png', mime: 'image/png', kind: 'png', dataUrl: urlA }]);
E.assetRestore([{ id: 'beta-svg', name: 'beta.svg', mime: 'image/svg+xml', kind: 'svg', dataUrl: mk('beta-svg', 512, 512, 'svg') }]);
setTimeout(() => {
  ok(E.assetReady('alpha-1') && E.assetGet('alpha-1').w === 90, 'a restored png decodes and records its natural size');
  ok(E.assetReady('beta-svg') && E.assetGet('beta-svg').w === 512, 'a restored svg decodes at its stamped size');
  ok(!!E.assetGet('alpha-1').sha, 'a sha32 identity is computed at rest');
  ok(E.assetFits('alpha-1', 60, 60, 'contain').ok === true, 'assetFits: contain into a smaller box is allowed');
  const cov = E.assetFits('alpha-1', 60, 60, 'cover');
  ok(cov.ok === false && cov.scale > 1.4 && cov.scale < 1.6, 'assetFits: cover prices the real fill scale, not the box', JSON.stringify(cov.scale));
  const up = E.assetFits('alpha-1', 200, 200, 'cover');
  ok(up.ok === false && up.upscale === true, 'assetFits: an upsample is refused with the numbers named', up.why);
  ok(up.why.indexOf('90×40') > 0 && up.why.indexOf('200×200') > 0, 'assetFits names source and box');
  ok(E.assetFits('alpha-1', 20, 9, 'stretch').ok === true, 'assetFits: stretch shrinks both axes, so nothing is magnified');
  ok(E.assetFits('beta-svg', 900, 900).ok === true, 'assetFits: vector art is never an upsample');
  ok(E.assetFits('nope-1', 10, 10).ok === false, 'assetFits: a dangling reference reports as missing');
  ok(E.assetDump(true).filter((r) => r.dataUrl).length === 2, 'assetDump(true) round-trips the bytes');
  ok(E.assetDump(false).every((r) => !r.dataUrl), 'assetDump(false) is references only, which is what a Design ID carries');
  ok(E.assetBytes() > 0, 'assetBytes reports the store size');
  ok(E.assetRestore([{ id: 'alpha-1', kind: 'png', name: 'alpha.png', dataUrl: urlA }]).added === 0, 're-restoring an id already decoded is a no-op');
  const needy = E.assetRestore([{ id: 'gamma-9', kind: 'png', name: 'gamma.png' }]);
  ok(needy.added === 0 && needy.needBytes === 1, 'a reference with no bytes is counted as needing them');
  const sp = E.handSpriteAsset('alpha-1', { maxW: 200, maxH: 200 });
  ok(sp && sp.c && isFinite(sp.px) && isFinite(sp.py), 'handSpriteAsset answers the {c,px,py} contract');
  ok(sp && sp.snapped === true, 'no authored pivot takes the hub-ink centroid');
  const sp2 = E.handSpriteAsset('alpha-1', { maxW: 200, maxH: 200, pivotX: 45, pivotY: 20 });
  ok(sp2 && Math.abs(sp2.px - 45 * sp2.scale) < 1e-6 && Math.abs(sp2.py - 20 * sp2.scale) < 1e-6, 'an authored source pivot scales with the sprite');
  ok(E.handSpriteAsset('nope-1') === null, 'a missing asset yields no sprite rather than a blank hand');
  const big = E.handSpriteAsset('beta-svg', { maxW: 30, maxH: 30 });
  ok(big && big.c.width <= 30 && big.c.height <= 30, 'a sprite is capped to the box the hand could sweep');
  // a dangling reference must not take the render down with it
  const dangling = E.face({ design: E.canonDesign({ print: 'roman', bgArt: 'gone-1', comps: [], centre: { hour: 1 } }), finish: 'brass', train: 'classic', frames: 16, tick: 'sweep', beats: 1 });
  ok(!!dangling.dial && !!dangling.outer, 'face() with a dangling art reference still builds every layer');
  ok(E.drawAsset(dangling.dial.getContext('2d'), 'gone-1', { x: 233, y: 233, w: 100, h: 100 }) === false, 'drawAsset says false for art that cannot draw');
  ok(E.drawAsset(dangling.dial.getContext('2d'), 'alpha-1', { x: 233, y: 233, w: 100, h: 100 }) === true, 'drawAsset draws held art');

  // ---- the backdrop really lands at the plate slot, under the geartrain -----------
  const { CANVASES, ALPHAS } = require('./headless-stub.js');
  const before = CANVASES.length;
  E.movement({ design: { comps: [] }, finish: 'brass', train: 'classic', frames: 16, tick: 'sweep', beats: 1, plate: 'classic', bgArt: 'alpha-1', bgSize: 300, bgX: 10, bgY: -5, bgFit: 'contain', bgOpacity: 0.5 });
  const painted = CANVASES.slice(before).some((c) => (c._ctx && c._ctx.draws || []).some((d) => d.src && d.src === urlA));
  ok(painted, 'movement() draws the held backdrop into the plate canvas');
  const before2 = CANVASES.length;
  E.movement({ design: { comps: [] }, finish: 'brass', train: 'classic', frames: 16, tick: 'sweep', beats: 1, plate: 'classic' });
  ok(!CANVASES.slice(before2).some((c) => (c._ctx && c._ctx.draws || []).some((d) => d.src && d.src === urlA)), 'no backdrop, no draw: the plate stays procedural');

  /* ---- ordering: the dial-surface art must be laid BEFORE the print -----------------
   * A stub cannot show colour, but it can show sequence. On the dial canvas the art's
   * drawImage has to come before the first numeral fillText; art drawn after the print
   * would bury the numerals, markers and chapter ring the user sets themselves. */
  const SF = { print: 'roman', numerals: 'all', comps: [], centre: { hour: 1, minute: 1 } };
  const sfFace = E.face({ design: E.canonDesign(Object.assign({ sfArt: 'alpha-1' }, JSON.parse(JSON.stringify(SF)))), finish: 'brass', train: 'classic', frames: 16, tick: 'sweep', beats: 1 });
  const sfOps = sfFace.dial.getContext('2d').ops || [];
  const iArt = sfOps.findIndex((o) => o.op === 'drawImage' && o.src === urlA);
  const iTxt = sfOps.findIndex((o) => o.op === 'fillText');
  ok(iArt >= 0, 'face() draws the surface art into the dial canvas', 'ops=' + sfOps.length);
  ok(iTxt >= 0, 'that dial really prints numerals for the art to sit under', 'ops=' + sfOps.length);
  ok(iArt >= 0 && iTxt >= 0 && iArt < iTxt, 'the art is under the print, not over it', 'art@' + iArt + ' first text@' + iTxt);
  ok(iArt > 0 && sfOps.slice(Math.max(0, iArt - 3), iArt).some((o) => o.op === 'clip'), 'the art is clipped to the dial circle as it is laid');
  const noSf = E.face({ design: E.canonDesign(JSON.parse(JSON.stringify(SF))), finish: 'brass', train: 'classic', frames: 16, tick: 'sweep', beats: 1 });
  ok(!(noSf.dial.getContext('2d').ops || []).some((o) => o.op === 'drawImage' && o.src === urlA), 'a design with no surface art leaves the dial procedural');
  // both slots at once: one image, two different canvases
  const both = E.movement({ design: { comps: [] }, finish: 'brass', train: 'classic', frames: 16, tick: 'sweep', beats: 1, plate: 'classic', bgArt: 'alpha-1' });
  const bothFace = E.face({ design: E.canonDesign(Object.assign({ sfArt: 'alpha-1' }, JSON.parse(JSON.stringify(SF)))), finish: 'brass', train: 'classic', frames: 16, tick: 'sweep', beats: 1, bgArt: 'alpha-1' });
  const plateHit = (both.plate._ctx || both.plate.getContext('2d')).draws.some((d) => d.src === urlA);
  const dialHit = (bothFace.dial._ctx || bothFace.dial.getContext('2d')).draws.some((d) => d.src === urlA);
  ok(plateHit && dialHit, 'the same stored image draws into the plate canvas and the dial face separately, so the slots cannot overwrite each other', 'plate=' + plateHit + ' dial=' + dialHit);

  // ---- the alpha<6 kill runs once, at ingest, and re-encodes ----------------------
  const w = 4, h = 4, data = new Uint8ClampedArray(w * h * 4);
  for (let i = 3; i < data.length; i += 4) data[i] = 200;                // solid ink
  data[3] = 3; data[7 * 4 + 3] = 5; data[11 * 4 + 3] = 0;                // two sub-threshold, one already clear
  const faintUrl = 'data:image/png;base64,' + Buffer.from('faint.png').toString('base64');
  SIZES[faintUrl] = [w, h]; ALPHAS[faintUrl] = data;
  E.assetRestore([{ id: 'faint-1', name: 'faint.png', kind: 'png', dataUrl: faintUrl }]);
  setTimeout(() => {
    const f = E.assetGet('faint-1');
    ok(!!f && f.dataUrl !== faintUrl, 'sub-threshold texels forced a re-encode at ingest');
    const body = JSON.parse(Buffer.from(f.dataUrl.split(',')[1], 'base64').toString('utf8'));
    ok(body.faint === 0, 'the stored bytes hold no alpha in 1..5 (prep_watch_kit INVISIBLE agrees)', JSON.stringify(body));
    ok(body.ink === w * h - 3, 'real ink survived the clean (16 texels, 2 sub-threshold, 1 already clear)', JSON.stringify(body));
    // centroid maths, on ink the stub hands back directly
    const c2 = new (require('./headless-stub.js').FakeCanvas)(); c2.width = 10; c2.height = 10;
    const d2 = new Uint8ClampedArray(10 * 10 * 4);
    d2[(0 * 10 + 0) * 4 + 3] = 255;                                       // ink at the top-left corner
    d2[(0 * 10 + 9) * 4 + 3] = 255;                                       // and the top-right
    c2._alpha = d2;
    const ct = E.inkCentroidOf(c2);
    ok(Math.abs(ct[0] - 4.5) < 1e-6 && ct[1] === 0, 'inkCentroidOf weighs the ink, not the box', JSON.stringify(ct));
    // ---- art zoom: the box that is drawn must be the box that is priced -------------
    const FC2 = require('./headless-stub.js').FakeCanvas;
    const zoomDraw = (z) => { const c = new FC2(); c.width = 466; c.height = 466; E.drawAsset(c.getContext('2d'), 'alpha-1', { x: 233, y: 233, w: 100, h: 100, fit: 'stretch', zoom: z }); return c.getContext('2d').draws.slice(-1)[0].args; };
    const zb1 = zoomDraw(1), zb25 = zoomDraw(2.5);
    ok(Math.abs(zb25[2] - zb1[2] * 2.5) < 1e-6 && Math.abs(zb25[3] - zb1[3] * 2.5) < 1e-6, 'drawAsset multiplies the drawn box by zoom', JSON.stringify([zb1.slice(2), zb25.slice(2)]));
    ok(zb1[0] === -zb1[2] / 2 && zb1[1] === -zb1[3] / 2 && zb25[0] === -zb25[2] / 2 && zb25[1] === -zb25[3] / 2, 'the box stays centred on the anchor at every zoom, so scaling never slides the art off its offset', JSON.stringify([zb1.slice(0, 2), zb25.slice(0, 2)]));
    ok(E.assetFits('alpha-1', 60, 60, 'contain', 1).ok === true, 'contain at zoom 1 is allowed');
    const zup = E.assetFits('alpha-1', 60, 60, 'contain', 2);
    ok(zup.ok === false && zup.upscale === true, 'that same contain at zoom 2 is an upscale and is refused, not silently resampled', zup.why);
    const abz = E.artBox('alpha-1', { x: 253, y: 223, w: 100, h: 100, fit: 'stretch', zoom: 1.5 });
    ok(abz.w === 150 && abz.x === 253, 'artBox reports the zoomed box in dial space, which is what the guide frames', JSON.stringify(abz));
    const drawn = new FC2(); drawn.width = 466; drawn.height = 466;
    E.drawAsset(drawn.getContext('2d'), 'alpha-1', { x: 253, y: 223, w: 100, h: 100, fit: 'stretch', zoom: 1.5 });
    const dArgs = drawn.getContext('2d').draws.slice(-1)[0].args;
    ok(Math.abs(dArgs[2] - abz.w) < 1e-6 && Math.abs(dArgs[3] - abz.h) < 1e-6, 'and the guide box is the box drawAsset paints: one implementation, no drift', JSON.stringify([dArgs, abz.w, abz.h]));
    ok(!('sfZoom' in E.canonDesign({ sfArt: 'alpha-1', sfZoom: 1 })), 'a zoom of 1 is the identity and does not reach the Design ID');
    ok(E.canonDesign({ sfArt: 'alpha-1', sfZoom: 1.4 }).sfZoom === 1.4, 'a real zoom is authored and shipped');
    /* ---- the arbour outside the ink ----------------------------------------------
     * A hand whose hub hides under a subdial has a pivot with no ink under it. The
     * old code threw that number away and used the centroid, so the author could never
     * say so. The plane must grow to hold the pivot instead, and the ink must not move
     * relative to the arbour while it does. */
    const tall = mk('tall-1', 20, 100, 'png'); E.assetRestore([{ id: 'tall-1', name: 'tall.png', kind: 'png', dataUrl: tall }]);
    setTimeout(() => {
      const inb = E.handSpriteAsset('tall-1', { maxW: 466, maxH: 466, pivotX: 10, pivotY: 95 });
      ok(inb && inb.padded === false && inb.c.height === 100, 'a pivot on the ink leaves the plane the size of the art');
      const offp = E.handSpriteAsset('tall-1', { maxW: 466, maxH: 466, pivotX: 10, pivotY: 140 });
      ok(offp && offp.padded === true && offp.snapped === false, 'a pivot beyond the ink is accepted, not discarded', JSON.stringify(offp && [offp.py, offp.snapped]));
      ok(offp && offp.py >= 2 && offp.py <= offp.c.height - 2 && offp.c.height > 100, 'the grown plane holds that pivot clear of every edge', JSON.stringify(offp && [offp.c.width, offp.c.height, offp.px, offp.py]));
      ok(offp && offp.px === 10, 'the pad is added only where the arbour is, so the ink itself never shifts');
      ok(E.handSpriteAsset('tall-1', { maxW: 466, maxH: 466, pivotX: null, pivotY: null }).snapped === true, 'the centroid fallback is reserved for a pivot that was never set');
      ok(E.handSpriteAsset('tall-1', { maxW: 466, maxH: 466, pivotX: 10 }).snapped === true, 'a lone pivot with no partner falls back rather than turning about half a point');
      const zz = E.handSpriteAsset('tall-1', { maxW: 466, maxH: 466, zoom: 2 });
      ok(zz && zz.c.height === 200 && zz.scale === 2, 'a hand zoom rescales the plane, so a small upload still reaches a long hand');
      /* ---- the facing of art that was not drawn pointing north ---------------------
       * Two probes 40 px from the arbour: straight up, and straight to the right. The
       * art is 20×100 with its arbour on the bottom edge, so 40 is past the 10 px
       * half-width and inside the 95 px reach — a probe answers only along whichever
       * axis the ink actually lies on, which is exactly the thing Facing sets, so this
       * pair fails if rot0 is parsed but never actually turns anything. */
      const HF = (rot0) => E.face({ design: E.canonDesign({ print: 'none', numerals: 'none', comps: [], centre: { hour: 0, minute: 0, second: 1 }, hands: { 'centre-second': { art: 'tall-1', rot0: rot0, pivotX: 10, pivotY: 95 } } }), finish: 'brass', train: 'classic', frames: 16, tick: 'sweep', beats: 1 });
      const t0 = new Date(2026, 0, 1, 10, 9, 0);
      const hd0 = HF(0).hands.find((q) => q.id === 'centre-second');
      const hd90 = HF(90).hands.find((q) => q.id === 'centre-second');
      ok(hd0 && hd0.artRot === 0, 'art with no facing set turns with no correction');
      ok(hd90 && Math.abs(hd90.artRot - Math.PI / 2) < 1e-9, 'the facing rides the hand record in radians', String(hd90 && hd90.artRot));
      const tipX = (hd, s) => E.handHit(hd, hd.x + s, hd.y, t0), tipY = (hd, s) => E.handHit(hd, hd.x, hd.y - s, t0);
      ok(tipY(hd0, 40) && !tipX(hd0, 40), 'at facing 0 the art reads up out of the arbour, the way it was drawn', JSON.stringify([tipY(hd0, 40), tipX(hd0, 40)]));
      ok(!tipY(hd90, 40) && tipX(hd90, 40), 'at facing 90 the same art reads to the right: the correction really turns it', JSON.stringify([tipY(hd90, 40), tipX(hd90, 40)]));
      ok(E.handZeroDeg(hd0) === 0 && E.handZeroDeg(hd90) === 90, 'zeroRotationDeg ships the facing, so the device turns the sprite the same way', JSON.stringify([E.handZeroDeg(hd0), E.handZeroDeg(hd90)]));
      const cal = { print: 'none', numerals: 'none', comps: [{ id: 'c1', type: 'subdial', host: 'dial', angle: 90, dist: 100, rot: 30, size: 60, scale: 'seconds', style: 'azure' }], centre: { hour: 0, minute: 0, second: 0 }, hands: { c1: { art: 'tall-1', rot0: 45 } } };
      const hdSub = E.face({ design: E.canonDesign(JSON.parse(JSON.stringify(cal))), finish: 'brass', train: 'classic', frames: 16, tick: 'sweep', beats: 1 }).hands.find((q) => q.id === 'c1');
      ok(hdSub && Math.abs(E.handZeroDeg(hdSub) - 75) < 0.01, 'a tilted subdial hand ships its host tilt AND its own facing as one angle', String(hdSub && E.handZeroDeg(hdSub)));
      const noArt = E.canonDesign({ hands: { 'centre-hour': { rot0: 90, pivotX: 4, pivotY: 4 } } });
      ok(!noArt.hands, 'a facing and pivot on a hand with no art under them are stripped, not shipped as a no-op');
      /* ---- what a click lands on ---------------------------------------------------
       * The face answered for complications alone, so grabbing a hand or the dial read
       * as empty space and deselected whatever the author had picked. The complication
       * here is a SUBDIAL: its hit radius is geometric, while a window's is measured
       * from its text -- which a stub cannot measure, and an assertion built on it
       * would be about the stub rather than about the engine. */
      const PF = { print: 'none', numerals: 'none', comps: [{ id: 'c1', type: 'subdial', host: 'dial', angle: 0, dist: 110, rot: 0, size: 60, scale: 'seconds', style: 'azure' }], centre: { hour: 1, minute: 1, second: 1 } };
      const fp = E.face({ design: E.canonDesign(JSON.parse(JSON.stringify(PF))), finish: 'brass', train: 'classic', frames: 16, tick: 'sweep', beats: 1 });
      const hdS = fp.hands.find((q) => q.id === 'centre-second');
      const sub = fp.lay.subs[0];
      const edge = fp.hands.reduce((m, q) => Math.max(m, Math.hypot(q.x, q.y)), 0);
      ok(fp.pick(465, 465, t0) === null, 'a corner beyond the dial and clear of every hand picks nothing, as it always did', JSON.stringify([edge, fp.pick(465, 465, t0)]));
      /* At 10:09:00 the hour hand reads 10, the minute 9 and the seconds 12, so the
       * lower-right diagonal is the one stretch of face with nothing drawn on it. */
      const bare = fp.pick(318, 318, t0);
      ok(bare && bare.kind === 'dial', 'a bare stretch of face picks the dial, which is what makes its art draggable at all', JSON.stringify(bare));
      const stack = fp.pickAll(sub.x, sub.y, t0);
      ok(stack.length > 1 && stack[0].kind === 'hand', 'the subdial hand lies over its own plate, so the stack names both with the hand on top', JSON.stringify(stack));
      ok(stack.some((q) => q.kind === 'comp' && q.id === 'c1'), 'and the plate under it is in that stack, which is what lets a second click reach it', JSON.stringify(stack));
      ok(fp.hit(sub.x, sub.y) === 'c1', 'F.hit is untouched, so every caller that wanted a complication still gets exactly one');
      const onHand = fp.pick(hdS.x, hdS.y - 8, t0);
      ok(onHand && onHand.kind === 'hand' && onHand.id === 'centre-second', 'a hand is clickable now, which is the whole basis for dragging one', JSON.stringify(onHand));
      /* ---- no case: nothing drawn at all, and the opening fits the drawn face --------
       * The device owns its own metal and the author's own image carries it, so the case
       * layer must be empty. It used to ship an opaque mask instead: a black circle that
       * sat at the rim at full width and marched inward as the opening shrank, painting
       * their printed case out. The opening is now the ring the guide paints -- and the
       * lens the DRAWN content is eased through (dynScale), never the author's art. */
      const NC = { print: 'none', numerals: 'none', comps: [], centre: { hour: 1, minute: 1 } };
      const mkFace = (o) => E.face(Object.assign({ design: E.canonDesign(JSON.parse(JSON.stringify(NC))), finish: 'brass', train: 'classic', frames: 16, tick: 'sweep', beats: 1 }, o));
      ok(mkFace({ caseStyle: 'polished' }).inset === 1, 'a plain case leaves the content at full size');
      ok(Math.abs(mkFace({ caseStyle: 'tachy' }).inset - 193.5 / 221) < 1e-9, 'a bezel style keeps its own fixed opening, exactly as before');
      ok(mkFace({ caseStyle: 'none' }).inset === 1, 'no case at the default opening matches the old blank rim');
      const none1 = mkFace({ caseStyle: 'none', caseOpen: 160 });
      ok(none1.caseOpen === 160 && none1.inset === 1 && Math.abs(none1.dynK - 160 / 221) < 1e-9, 'a smaller authored opening changes the author\'s pixels NOT at all: it is the hardware\'s rim, quoted for the guide, and the handle that eases the drawn face to fit it', JSON.stringify([none1.caseOpen, none1.inset, none1.dynK]));
      ok(E.dynScale({ caseStyle: 'none', caseOpen: 90 }) === 90 / 221 && E.dynScale({ caseStyle: 'none', caseOpen: 233 }) === 1 && E.dynScale({ caseStyle: 'polished', caseOpen: 90 }) === 1,
        'dynScale is the opening\'s other half: under None the drawn content fits the ring, and only the authorable opening moves it', JSON.stringify([E.dynScale({ caseStyle: 'none', caseOpen: 90 }), E.dynScale({ caseStyle: 'none', caseOpen: 233 }), E.dynScale({ caseStyle: 'polished', caseOpen: 90 })]));
      const fLens = mkFace({ caseStyle: 'none', caseOpen: 160 });
      const bLens = E.movement({ design: { comps: [] }, finish: 'brass', train: 'classic', frames: 16, tick: 'sweep', beats: 1, plate: 'classic' });
      const cLens = document.createElement('canvas'); cLens.width = cLens.height = 466; const cxL = cLens.getContext('2d');
      E.compose(cxL, bLens, {}, fLens, new Date(2026, 0, 1, 10, 9, 0), {});
      const nLens = cxL.geom.filter((g) => g.op === 'scale' && Math.abs(g.a[0] - 160 / 221) < 1e-9).length;
      ok(nLens >= 5, 'compose turns the plate, train, haze, complications, subdials, hands and glass through the opening scale -- and blends the dial and the backdrop art as baked', 'scaled layers=' + nLens);
      /* ---- the hands row eye on a CENTRE hand ---------------------------------------
       * The eye wrote hands['centre-hour'].on = false and nothing read it: buildHands
       * skipped only the subdial rows, and the row read its label off design.centre alone.
       * The button moved on the centre rows too -- and did nothing. */
      const mkHidden = (hcfg) => { const d = JSON.parse(JSON.stringify(NC)); d.hands = hcfg; return E.face({ design: E.canonDesign(d), finish: 'brass', train: 'classic', frames: 16, tick: 'sweep', beats: 1, caseStyle: 'polished' }); };
      const fHid = mkHidden({ 'centre-hour': { on: false } });
      ok(fHid.hands.some((h) => h.id === 'centre-minute') && !fHid.hands.some((h) => h.id === 'centre-hour'),
        'the eye hides a CENTRE hand: the engine drops it from the drawn list, as it always did for a subdial hand', (fHid.hands || []).map((h) => h.id).join(','));
      ok(fHid.handSlots.some((s) => s.id === 'centre-hour'), 'and the hidden hand keeps its row -- an author who hides a hand must still be able to show it again', (fHid.handSlots || []).map((s) => s.id).join(','));
      ok(mkHidden({ 'centre-hour': { on: true } }).hands.some((h) => h.id === 'centre-hour'), 'on:true is showing, not a second kind of hidden');
      ok(E.faceScale({ caseStyle: 'none', caseOpen: 90 }) === 1 && E.faceScale({ caseStyle: 'tachy' }) === 193.5 / 221,
        'faceScale agrees: the authorable opening never shrinks the face, a bezel style always does', JSON.stringify([E.faceScale({ caseStyle: 'none', caseOpen: 90 }), E.faceScale({ caseStyle: 'tachy' })]));
      ok(mkFace({ caseStyle: 'polished', caseOpen: 120 }).inset === 1, 'and an opening typed in beside a style that cannot use it changes nothing at all');
      ok(mkFace({ caseStyle: 'none', caseOpen: 5 }).caseOpen === 90 && mkFace({ caseStyle: 'none', caseOpen: 9999 }).caseOpen === 233, 'a wild opening is clamped into the range the panel can show');
      ok(E.caseOpenR({ caseStyle: 'tachy', caseOpen: 100 }) === 193.5, 'a bezel style ignores the authored opening it cannot use');
      const noneG = E.caseLayer({ caseStyle: 'none', caseOpen: 160, outerTemp: 2200 }).getContext('2d').geom;
      ok(noneG.filter((o) => o.op === 'stroke').length === 0, 'the no-case layer strokes no rim, groove or specular line', 'strokes=' + noneG.filter((o) => o.op === 'stroke').length);
      const polG = E.caseLayer({ caseStyle: 'polished', caseOpen: 160, outerTemp: 2200 }).getContext('2d').geom.filter((o) => o.op === 'stroke').length;
      ok(polG > 0, 'the same probe on a real case finds its rim lines, so the check above is not vacuous', 'strokes=' + polG);
      ok(noneG.length === 0, 'the no-case layer draws NOTHING: no mask, no ring, no shade -- any pixel it painted would sit on top of the case in the author\'s own image, which is what "None" promises', JSON.stringify(noneG.slice(0, 3).map((o) => o.op)));
      ok(E.caseLayer({ caseStyle: 'polished', caseOpen: 160, outerTemp: 2200 }).getContext('2d').geom.length > 0, 'and a drawn case still paints its geometry, so the empty layer above is the option and not a broken probe');
      ok(E.artClipR({ caseStyle: 'none' }) === E.PANEL_R && E.artClipR({ caseStyle: 'polished' }) === 221 && E.artClipR({}) === 221,
        'the author\'s art is clipped to the panel under None, where their image carries the case, and to the dial circle under a drawn one, where the metal covers the rim',
        JSON.stringify([E.artClipR({ caseStyle: 'none' }), E.artClipR({ caseStyle: 'polished' }), E.artClipR({})]));
      /* ---- what actually shapes a seconds hand --------------------------------------
       * The hands pane put the hand-style dropdown on the seconds row as well, and it
       * changed nothing: that branch of handParts() reads `style` in exactly one place,
       * to decide whether the counterweight is hollow. So the row now offers the seconds
       * list. If a hand style ever starts shaping a seconds hand for real, the first
       * check fails and the row has to give the control back. */
      const sig = (f) => { const log = []; const g = { moveTo: (...a) => log.push('M' + a), lineTo: (...a) => log.push('L' + a), arc: (...a) => log.push('A' + a), closePath: () => log.push('Z'), quadraticCurveTo: (...a) => log.push('Q' + a), bezierCurveTo: (...a) => log.push('B' + a), rect: (...a) => log.push('R' + a) }; ['save', 'restore', 'beginPath', 'translate', 'scale', 'rotate', 'transform', 'clip', 'fill', 'stroke', 'setLineDash'].forEach((k) => { g[k] = () => { }; }); f(g); return log.join(';'); };
      const partsOf = (style, kind, sec) => { const p = E.handParts(style, kind, 200, 3, kind === 'second' ? 52 : 32, sec); return { body: p.Pp.map(sig).join('|'), holes: p.cut.map(sig).join('|'), span: Math.round(p.span * 100) }; };
      const base = partsOf('dauphine', 'second', 'classic');
      const off = Object.keys(E.HAND_STYLES).filter((s) => { const p = partsOf(s, 'second', 'classic'); return p.body !== base.body || p.holes !== base.holes || p.span !== base.span; });
      ok(off.length === 2 && off.every((s) => s === 'breguet' || s === 'spade'),
        'only two entries in the whole hand-style list reach a seconds sprite at all, so the style dropdown on that row chose nothing', JSON.stringify(off));
      ok(off.every((s) => partsOf(s, 'second', 'classic').body === base.body),
        'and even those two leave the seconds hand shaped identically: the style only punched the counterweight hole', JSON.stringify(off.map((s) => partsOf(s, 'second', 'classic').holes.length)));
      const holesClassic = partsOf('breguet', 'second', 'classic').holes, holesLolly = partsOf('breguet', 'second', 'lollipop').holes;
      ok(holesClassic !== holesLolly && partsOf('dauphine', 'second', 'lollipop').body !== base.body,
        'while the seconds list moves the ink a hand style never touched', JSON.stringify([holesClassic.split(';').length, holesLolly.split(';').length]));
      ok(partsOf('poire', 'hour', null).body !== partsOf('leaf', 'hour', null).body, 'the same probe on an hour hand finds the style does shape it, so the seconds checks above are not vacuous');
      /* ---- a round window -----------------------------------------------------------
       * The shape has to reach the hole, or the device ships a square disc inside a
       * round punch and the four corners sit over the dial. */
      const CW = { print: 'none', numerals: 'none', centre: { hour: 1, minute: 1 }, comps: [{ id: 'w9', type: 'window', shows: 'date', host: 'dial', angle: 0, dist: 120, size: 30, wshape: 'circle' }] };
      const fcw = E.face({ design: E.canonDesign(JSON.parse(JSON.stringify(CW))), finish: 'brass', train: 'classic', frames: 16, tick: 'sweep', beats: 1 });
      const hole9 = fcw.lay.comps.find((c) => c.id === 'w9').hole;
      ok(hole9.kind === 'circle' && isFinite(hole9.r) && hole9.r > 0, 'a window asked for round is laid out round', JSON.stringify(hole9));
      const dRound = E.canonDesign(JSON.parse(JSON.stringify(CW)));
      const dSquare = E.canonDesign(JSON.parse(JSON.stringify(Object.assign({}, CW, { comps: [{ id: 'w9', type: 'window', shows: 'date', host: 'dial', angle: 0, dist: 120, size: 30 }] }))));
      ok(dRound.comps[0].wshape === 'circle' && !('wshape' in dSquare.comps[0]), 'round is authored; square is the identity and stays out of the Design ID');
      const dg = E.canonDesign({ comps: [{ id: 'x', type: 'subdial', wshape: 'circle' }] });
      ok(!('wshape' in dg.comps[0]), 'a shape on a complication that has no aperture is dropped');
      const frect = E.face({ design: dSquare, finish: 'brass', train: 'classic', frames: 16, tick: 'sweep', beats: 1 });
      ok(frect.lay.comps.find((c) => c.id === 'w9').hole.kind === 'rect', 'an unmarked window stays square, as every design made before this keeps it');
      ok(fcw.lay.hits.some((h) => h.id === 'w9'), 'a round window is still clickable');
      if (checked < 72) { fails++; console.log('FAIL only ' + checked + ' checks ran'); }
  console.log(fails ? '\n' + fails + ' FAILED' : '\nall green (' + checked + ' checks)');
    process.exit(fails ? 1 : 0);
    }, 20);
  }, 20);
}, 20);

