// Does the control row actually OFFER the new controls? renderVals is the single source
// the template reads, so a field missing here is a field the author cannot reach.
require('./headless-stub.js');
const S = require('./headless-stub.js');
const E = global.WatchEngine;
global.React = { createRef: () => ({ current: null }) };
require('./component.js');
let fails = 0, checked = 0;
const ok = (c, n, x) => { checked++; console.log((c ? 'PASS ' : 'FAIL ') + n + (x ? '  [' + x + ']' : '')); if (!c) fails++; };
const DCLogic = function () { };
DCLogic.prototype.setState = function (p) { Object.assign(this.state, typeof p === 'function' ? p(this.state) : p); };
const Comp = global.COMP_CLASS(DCLogic);
const u = (tag, w, h) => { const x = 'data:image/png;base64,' + Buffer.from(tag).toString('base64'); S.SIZES[x] = [w, h]; return x; };
E.assetRestore([{ id: 'ui-art', name: 'art.png', kind: 'png', dataUrl: u('ui', 300, 300) },
  { id: 'ui-big', name: 'big.png', kind: 'png', dataUrl: u('big', 600, 600) }]);
const it = new Comp({});
it.ready = true; it.rendering = false; it.budget = () => ({ fps: 8 });
it.state.dial = 'calendar'; it.state.tab = 'dial'; it.state.playing = false; it.state.hidden = {};
it.state.designs = { calendar: E.canonDesign(JSON.parse(JSON.stringify(E.PRESETS.calendar.design))) };
const labels = (tab) => { it.state.tab = tab; return it.renderVals().fields; };
const byLabel = (fs, l) => fs.find((f) => f.label === l);

const dial = labels('dial');
const bgHead = dial.find((f) => f.isHead && /Background art/.test(f.label));
ok(!!bgHead, 'the Dial tab has a Background art section');
const fi0 = byLabel(dial, 'Image');
ok(!!fi0 && fi0.isFile, 'the file input is offered before anything is chosen', JSON.stringify(!!fi0));
ok(dial.some((f) => f.isNote && /No backdrop/.test(f.note || '')), 'and the row says the plate is procedural today');
const after = labels('dial');
it.state.designs.calendar.bgArt = 'ui-art';
setTimeout(() => {
  const h2 = labels('dial').find((f) => f.isNote && /one-time store|No bytes|re-export|fills a/.test(f.note || ''));
  ok(/one-time store|re-export/.test((h2 || {}).note || ''), 'the line prices the art or refuses it, with numbers', (h2 || {}).note);
  const held = labels('dial');
  for (const l of ['Fit', 'Offset X', 'Offset Y', 'Size', 'Opacity']) ok(!!byLabel(held, l), 'held art reveals the ' + l + ' control');
  ok(held.some((f) => f.isChips && (f.opts || []).some((o) => /Remove this image/.test(o.label))), 'and a way back to the procedural plate');
  const rm = held.find((f) => f.isChips && (f.opts || []).some((o) => /Remove this image/.test(o.label)));
  rm.opts.find((o) => /Remove/.test(o.label)).pick();
  ok(!it.design().bgArt, 'Remove clears the reference from the design');
  it.state.designs.calendar.bgArt = 'nowhere';
  const miss = labels('dial').find((f) => f.isNote && /No bytes for/.test(f.note || ''));
  ok(!!miss, 'a dangling reference is reported on the control row', miss && miss.note);

  // ---- slot two: the dial face, next to the plate slot, behaving the same way ------
  ok(!!labels('dial').find((f) => f.isHead && /Dial surface art/.test(f.label)), 'the Dial tab offers a second section for the dial face');
  ok(!/geartrain/.test(labels('dial').find((f) => f.isHead && /Dial surface art/.test(f.label)).label), 'the surface section does not claim to sit behind the train');
  // large enough to fill the 442 box without upsampling, so what is on the line is
  // the slot's own price text rather than a refusal to place it
  it.state.designs.calendar.bgArt = 'ui-big';
  it.state.designs.calendar.sfArt = 'ui-big';
  const heldSf = labels('dial');
  ok(heldSf.filter((f) => f.isFile && f.label === 'Image').length === 2, 'each backdrop slot keeps its own upload, so two images can be authored independently');
  const sfNote = heldSf.find((f) => f.isNote && /dial sprite/.test(f.note || ''));
  ok(!!sfNote, 'the surface line says the art ships inside the dial sprite, not as a new layer', sfNote && sfNote.note);
  const bgNote = heldSf.find((f) => f.isNote && /base frames/.test(f.note || ''));
  ok(!!bgNote, 'the plate line still prices the plate slot, unchanged by the new section');
  const chips = heldSf.filter((f) => f.isChips && (f.opts || []).some((o) => /Remove this image/.test(o.label)));
  ok(chips.length === 2, 'each slot gets its own Remove, so clearing one leaves the other', chips.length);
  chips[1].opts.find((o) => /Remove/.test(o.label)).pick();
  ok(!it.design().sfArt && it.design().bgArt === 'ui-big', 'removing the dial-surface art leaves the plate backdrop referenced');
  it.state.designs.calendar.sfArt = 'ui-big';
  // leave the plate slot dangling again: the export-tab assertion further down proves the
  // tab warns before shipping a kit whose art is not here, and it can only do that if a
  // reference really is missing once this block finishes
  it.state.designs.calendar.bgArt = 'nowhere';

  // ---- the hands tab: sprite slot, pivot fields, hub snap --------------------------
  const hands = labels('hands');
  ok(hands.length > 0, 'hands tab renders');
  const rows = (it.state.tab = 'hands', it.renderVals().handRows);
  ok(rows.length >= 3, 'rows built: ' + rows.length);
  const r = rows.find((q) => q.label === 'Centre hour');
  ok(!!r.onArtFile && !!r.onArtClear, 'each hand row offers an art upload and a way back to procedural');
  ok(!!r.artRef && !!r.onArtDown && !!r.onArtMove, 'each row carries the pivot thumbnail with a click/drag handler');
  ok(/antialias/i.test(r.artNote), 'a procedural hand is told why final-size art matters', r.artNote.slice(0, 60));
  it.state.designs.calendar.hands = { 'centre-hour': { art: 'ui-art', pivotX: 120, pivotY: 140 } };
  const r2 = it.renderVals().handRows.find((q) => q.label === 'Centre hour');
  ok(r2.hasArt === true && r2.artLabel === 'art.png', 'the row reports the sprite it is using', r2.artLabel);
  ok(r2.pivotX === 120 && r2.pivotY === 140, 'the authored pivot reads back into the fields');
  ok(/hub ink \(auto\)/.test(r2.artNote) === false && /pivot 120,140/.test(r2.artNote), 'the note reports the authored pivot', r2.artNote);
  r2.onPivotHub();
  ok(!it.state.designs.calendar.hands['centre-hour'].pivotX, 'Snap to hub ink clears the authored pivot');
  it.state.designs.calendar.hands['centre-hour'].pivotX = null; it.state.designs.calendar.hands['centre-hour'].pivotY = null;
  ok(/hub ink \(auto\)/.test(it.renderVals().handRows.find((q) => q.label === 'Centre hour').artNote), 'and the note says it is on hub ink');

  // ---- the subdial style list and the export row ----------------------------------
  it.state.designs.calendar.comps.forEach((c) => { if (c.type === 'subdial') c.style = 'transparent'; });
  const cs = labels('comps');
  ok(!!it.design().comps.find((c) => c.style === 'transparent'), 'the layout accepts a transparent subdial style');
  const ex = (it.state.tab = 'export', it.renderVals());
  ok(/art asset/.test(ex.bundleNote), 'the bundle button counts the art it will carry', ex.bundleNote);
  ok(/MISSING/.test(ex.artNote), 'the export tab warns before a kit ships without its art', ex.artNote);
  // ---- selecting on the stage: the pane must follow what was grabbed -----------------
  global.requestAnimationFrame = () => 1;                                  // onMove defers; the test flushes by hand
  it.forceUpdate = () => { };
  const stageEl = { getBoundingClientRect: () => ({ left: 0, top: 0, width: 466, height: 466 }), setPointerCapture() { } };
  const ev = (x, y) => ({ clientX: x, clientY: y, pointerId: 1, currentTarget: stageEl });
  const fcS = it.face(), dS = it.now();
  const top = fcS.hands[fcS.hands.length - 1];
  it.state.tab = 'comps'; it.state.sel = null; it.state.selKind = null;
  it.onDown(ev(top.x, top.y));                                             // the arbour is inside every hand's own plane
  ok(it.state.selKind === 'hand' && it.state.sel === top.id, 'grabbing the top hand selects that hand', JSON.stringify([it.state.selKind, it.state.sel, top.id]));
  ok(it.state.tab === 'hands', 'and the Editing Pane jumps to Hands, where its fields are', it.state.tab);
  ok(!(it.drag && it.drag.kind === 'hand'), 'a procedural hand selects but arms no pivot drag: its arbour is built from the style, not authored', JSON.stringify(it.drag && it.drag.kind));
  it.state.designs.calendar.hands = Object.assign({}, it.state.designs.calendar.hands, { [top.id]: { art: 'ui-big', pivotX: 150, pivotY: 150 } });
  const fcA = it.face(), topA = fcA.hands[fcA.hands.length - 1];
  it.state.tab = 'comps'; it._clickAt = null; it.onDown(ev(topA.x, topA.y));
  ok(it.drag && it.drag.kind === 'hand' && it.drag.id === topA.id, 'an image-backed hand arms the pivot drag, which is the whole point of selecting one', JSON.stringify(it.drag && [it.drag.kind, it.drag.id, topA.id]));
  ok(isFinite(it.drag.k) && it.drag.k > 0 && isFinite(it.drag.ang), 'the drag carries the sprite scale and the held angle, so the cursor maps into the frame of the art', JSON.stringify(it.drag && [it.drag.k, it.drag.ang]));
  const p0 = it.design().hands[topA.id].pivotX;
  it.onMove(ev(topA.x + 20, topA.y));
  it.flushDrag();
  ok(+it.design().hands[topA.id].pivotX !== p0, 'dragging the hand across the face moves its arbour', JSON.stringify([p0, it.design().hands[topA.id].pivotX]));
  it.setHand(topA.id, { pivotX: -40, pivotY: -12 });
  const neg = it.design().hands[topA.id];
  ok(neg.pivotX === -40 && neg.pivotY === -12, 'a pivot outside the sprite survives, because an arbour hidden under a subdial is a real design', JSON.stringify([neg.pivotX, neg.pivotY]));
  const want = 'centre ' + String(topA.id).slice(7);
  const negRow = (it.state.tab = 'hands', it.renderVals().handRows.find((q) => q.label.toLowerCase() === want));
  ok(negRow && /off the artwork/.test(negRow.artNote), 'and the row says so in words, rather than showing a number the author cannot interpret', negRow && negRow.artNote.slice(-70));
  it.state.designs.calendar.hands = {};
  let barePt = null;
  for (let a = 0; a < 360 && !barePt; a += 15) for (const r of [190, 150, 60]) {
    const s0 = fcS.pickAll(233 + Math.sin(a * Math.PI / 180) * r, 233 - Math.cos(a * Math.PI / 180) * r, dS);
    if (s0.length && s0[0].kind === 'dial') { barePt = [233 + Math.sin(a * Math.PI / 180) * r, 233 - Math.cos(a * Math.PI / 180) * r]; break; }
  }
  ok(!!barePt, 'a bare stretch of the face exists to click at all', JSON.stringify(barePt));
  // ---- overlapping stack: a second click on the same pixel steps down ---------------
  // Without this a date window under the seconds hand is unreachable for the whole
  // life of that hand, because the hand is drawn over it and always wins the pixel.
  const at = [top.x, top.y];
  const stackAt = fcS.pickAll(at[0], at[1], dS);
  it._clickAt = null;
  it.onDown(ev(at[0], at[1]));
  const seq = [it.state.selKind + ':' + it.state.sel];
  for (let n = 1; n < stackAt.length; n++) { it.onDown(ev(at[0], at[1])); seq.push(it.state.selKind + ':' + it.state.sel); }
  ok(seq.length === stackAt.length && seq.every((s, n) => s === stackAt[n].kind + ':' + stackAt[n].id),
    'repeated clicks on one pixel walk the stack top-down, so nothing under the top layer is ever unselectable', JSON.stringify(seq));
  it.onDown(ev(at[0], at[1]));
  ok(it.state.sel === stackAt[0].id, 'and the cycle wraps back to the top rather than dead-ending', JSON.stringify([it.state.sel, stackAt[0].id]));
  it.onDown(ev(at[0] + 90, at[1] + 40));
  const fresh = (it.face().pickAll(at[0] + 90, at[1] + 40, it.now()) || [])[0];
  ok(it._clickAt && (fresh ? it._clickAt.id === fresh.id : !it._clickAt.id), 'a click somewhere else starts a fresh cycle instead of continuing the old one', JSON.stringify([it._clickAt && it._clickAt.id, fresh && fresh.id]));
  it.state.tab = 'hands'; it.state.sel = 'nothing'; it.state.selKind = 'comp';
  it.state.designs.calendar.sfArt = 'ui-big';
  it.onDown(ev(barePt[0], barePt[1]));
  ok(it.state.selKind === 'dial' && it.state.tab === 'dial', 'clicking the face selects the dial and opens the Dial tab', JSON.stringify([it.state.selKind, it.state.tab]));
  ok(it.drag && it.drag.kind === 'dial' && it.drag.slot === 'sf', 'the dial drag is armed against the surface slot, the one holding art', it.drag && it.drag.slot);
  const sx0 = +it.design().sfX || 0, sy0 = +it.design().sfY || 0;
  it.onMove(ev(barePt[0] + 30, barePt[1] + 12));
  it.flushDrag();
  ok(Math.round((+it.design().sfX) - sx0) === 30 && Math.round((+it.design().sfY) - sy0) === 12, 'dragging the face pans that art by exactly the cursor move', JSON.stringify([sx0, sy0, it.design().sfX, it.design().sfY]));
  // ---- the alignment overlay: on when it answers a question, off otherwise -----------
  it.state.selKind = null; it.state.sel = null; it.drag = null; it.state.guide = 'auto';
  ok(it.guideOn() === false, 'a plain look at the watch carries no overlay', String(it.guideOn()));
  it.state.selKind = 'dial';
  ok(it.guideOn() === true, 'aligning the dial art brings the guide up');
  it.state.selKind = 'hand';
  ok(it.guideOn() === true, 'aligning a hand brings it up too');
  it.state.selKind = 'comp';
  ok(it.guideOn() === false, 'a complication of its own does not need it');
  it.state.selKind = null; it.state.guide = 'on';
  ok(it.guideOn() === true, 'and the author can pin it on');
  it.state.guide = 'off'; it.drag = { kind: 'dial' };
  ok(it.guideOn() === false, 'off means off, even mid-drag', String(it.guideOn()));
  it.drag = null; it.state.guide = 'auto';
  const gCan = new S.FakeCanvas(); gCan.width = 466; gCan.height = 466;
  it.state.selKind = 'dial'; it.state.artSlot = 'sf'; it.state.designs.calendar.sfZoom = 1.4;
  it.drawGuide(gCan.getContext('2d'), fcS, dS, 1);
  const gGeo = gCan.getContext('2d').geom;
  const arcs = gGeo.filter((o) => o.op === 'arc');
  ok(arcs.some((o) => Math.abs(o.a[0] - 233) < .01 && Math.abs(o.a[2] - 221.5) < .01), 'the guide circles the case opening the content must stay inside', JSON.stringify(arcs.map((o) => +o.a[2].toFixed(1))).slice(0, 60));
  const spokes = gGeo.filter((o) => o.op === 'lineTo' && Math.abs(Math.hypot(o.a[0] - 233, o.a[1] - 233) - 216) < 0.01).length;
  ok(spokes === 24, 'and lays exactly the 24 fifteen-degree spokes, no more and no fewer', 'spokes=' + spokes + ' arcs=' + arcs.length);
  ok(gGeo.some((o) => o.op === 'moveTo' && Math.abs(o.a[0] - 233) < 0.01), 'with a ray through the centre, so vertical alignment is read against the dial axis');
  ok(gGeo.some((o) => o.op === 'strokeRect'), 'the zoomed art is framed by a box, so the author sees what the crop will keep');
  const ghostRot = (rot0) => {
    it.state.designs.calendar.hands = { 'centre-hour': { art: 'ui-big', rot0: rot0 } };
    it.state.selKind = 'hand'; it.state.sel = 'centre-hour';
    const c = new S.FakeCanvas(); c.width = 466; c.height = 466;
    it.drawHandGhost(c.getContext('2d'), it.face(), dS, 1, (v) => v);
    const r = c.getContext('2d').geom.filter((o) => o.op === 'rotate');
    return r.length ? r[r.length - 1].a[0] : null;
  };
  const gA = ghostRot(0), gB = ghostRot(90);
  ok(gA !== null && gB !== null && Math.abs((gB - gA) - Math.PI / 2) < 1e-6,
    'the virtual hand turns with its facing: the same hand at 90 degrees is drawn exactly a quarter turn round', JSON.stringify([gA, gB, gB - gA]));
  it.state.designs.calendar.hands = { 'centre-hour': { art: 'ui-big', rot0: 90 } };
  const gCan3 = new S.FakeCanvas(); gCan3.width = 466; gCan3.height = 466;
  it.drawHandGhost(gCan3.getContext('2d'), it.face(), dS, 1, (v) => v);
  ok(gCan3.getContext('2d').geom.some((o) => o.op === 'strokeRect'), 'and its sprite is outlined where it really lies, which is what shows a wrong facing');
  it.state.designs.calendar.hands = {}; delete it.state.designs.calendar.sfZoom;
  // ---- the controls themselves -------------------------------------------------------
  const dialZ = labels('dial');
  ok(!!byLabel(dialZ, 'Zoom'), 'each art slot offers a zoom, not only a box size');
  it.state.caseStyle = 'none';
  const caseNone = labels('case');
  ok(!!byLabel(caseNone, 'Opening radius'), 'no case exposes the opening that lighting, shadow and clipping then use');
  it.state.caseStyle = 'polished';
  ok(!byLabel(labels('case'), 'Opening radius'), 'a real case hides it again: its own rim owns that number');
  const winC = (it.state.designs.calendar.comps || []).find((c) => c.type === 'window');
  ok(!!winC, 'the calendar preset has a window to test on');
  if (winC) { winC.wshape = 'circle'; it.state.sel = winC.id; const cw = labels('comps'); ok(!!byLabel(cw, 'Aperture'), 'a window offers an aperture shape'); const ap = byLabel(cw, 'Aperture'); ok(ap && (ap.opts || []).some((o) => /Round/.test(o.label)), 'with round among its choices', JSON.stringify((ap || {}).opts || []).slice(0, 80)); winC.wshape = 'rect'; }
  const hRows = (it.state.designs.calendar.hands = { 'centre-hour': { art: 'ui-big' } }, it.state.tab = 'hands', it.renderVals().handRows);
  const hr = hRows.find((q) => q.label === 'Centre hour');
  for (const k of ['onRot0', 'onZoom', 'onFacingUp', 'onFacingDown', 'onFacingReset', 'rot0', 'zoom'])
    ok(hr[k] !== undefined, 'a hand row exposes ' + k);
  hr.onFacingUp();
  ok(it.design().hands['centre-hour'].rot0 === 15, 'the facing stepper turns the art 15 degrees clockwise', String(it.design().hands['centre-hour'].rot0));
  hr.onFacingDown(); hr.onFacingDown(); hr.onFacingDown();
  ok(it.design().hands['centre-hour'].rot0 === 330, 'and keeps stepping off the live design (15 down to 0, then 345, then 330) instead of sticking on the value the row was drawn with', String(it.design().hands['centre-hour'].rot0));
  hr.onFacingReset();
  ok(!it.design().hands['centre-hour'].rot0, 'Facing to 12 clears it, which is the identity the Design ID already had');
  it.state.designs.calendar.hands = {};
  if (checked < 20) { fails++; console.log('FAIL only ' + checked + ' checks ran'); }
  // every {{ }} binding the template reads must exist on the props object, or the DC
  // runtime renders an empty control with no warning at all
  const ex2 = (it.state.tab = 'export', it.renderVals());
  const rv = it.renderVals();
  const r3 = rv.handRows.find((q) => q.label === 'Centre hour');
  for (const k of ['onPivotX', 'onPivotY', 'onPivotCentre', 'onPivotHub', 'onArtFile', 'onArtClear', 'artRef', 'artLabel', 'artNote', 'onArtDown', 'onArtMove', 'hasArt'])
    ok(r3[k] !== undefined, 'handRows exposes ' + k);
  for (const k of ['exportKit', 'exportBundle', 'exportFrames', 'loadBundle', 'bundleNote', 'artNote', 'kitNote', 'designId', 'loadDesign', 'copyId', 'onIdInput', 'selectAll'])
    ok(rv[k] !== undefined, 'the template binding ' + k + ' has a backing property');
  ok(rv.bundleNote === ex2.bundleNote, 'the bundle label is stable across renders');
  console.log(fails ? '\n' + fails + ' FAILED' : '\nall green (' + checked + ' checks)');
  process.exit(fails ? 1 : 0);
}, 15);
