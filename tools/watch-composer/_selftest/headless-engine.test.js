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
    if (checked < 55) { fails++; console.log('FAIL only ' + checked + ' checks ran'); }
  console.log(fails ? '\n' + fails + ' FAILED' : '\nall green (' + checked + ' checks)');
    process.exit(fails ? 1 : 0);
  }, 20);
}, 20);

