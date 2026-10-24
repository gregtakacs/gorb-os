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
setTimeout(async () => {
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

  // ---- the seconds row: the hand-style dropdown there chose nothing at all ----------
  // handParts() ignores `style` once kind is 'second', so the seconds row carries the
  // seconds list in that slot instead. isSec/notSec are what the template branches on,
  // so a row that gets them backwards silently shows a control with no effect.
  const handsBefore = JSON.stringify(it.state.designs.calendar.hands || {});
  const twoRows = (it.state.tab = 'hands', it.renderVals().handRows);
  const secRow = twoRows.find((q) => q.label === 'Centre second');
  const hourRow = twoRows.find((q) => q.label === 'Centre hour');
  ok(!!secRow && secRow.isSec === true && secRow.notSec === false, 'the centre seconds row is flagged as the row whose style slot holds the seconds list', JSON.stringify(secRow && [secRow.isSec, secRow.notSec]));
  ok(hourRow.isSec === false && hourRow.notSec === true, 'every other row keeps its style list', JSON.stringify([hourRow.isSec, hourRow.notSec]));
  ok(secRow.secOpts.length === Object.keys(E.SEC_STYLES).length + 1 && secRow.secOpts.slice(1).every((o) => E.SEC_STYLES[o.v] === o.label), 'that slot is filled from the seconds list, not the hand-style list', JSON.stringify(secRow.secOpts.map((o) => o.v).slice(0, 5)));
  ok(secRow.secOpts.filter((o) => o.sel).length === 1 && secRow.secOpts.find((o) => o.sel).v === 'auto', 'with the preset/default entry selected to begin with', JSON.stringify(secRow.secOpts.filter((o) => o.sel).map((o) => o.v)));
  ok(typeof secRow.onSec === 'function' && typeof secRow.onStyle === 'function', 'both handlers are still on the row, so either branch of the template has something to call');
  secRow.onSec({ target: { value: 'lollipop' } });
  ok(it.design().hands['centre-second'].sec === 'lollipop', 'choosing a seconds style writes what the engine reads', JSON.stringify(it.design().hands['centre-second']));
  const secRow2 = (it.state.tab = 'hands', it.renderVals().handRows).find((q) => q.label === 'Centre second');
  ok(secRow2.secOpts.filter((o) => o.sel).length === 1 && secRow2.secOpts.find((o) => o.sel).v === 'lollipop', 'and the select reads the choice back after a re-render', JSON.stringify(secRow2.secOpts.find((o) => o.sel)));
  hourRow.onStyle({ target: { value: 'poire' } });
  ok(it.design().hands['centre-hour'].style === 'poire' && !it.design().hands['centre-hour'].sec, 'the hour row still takes a hand style and no seconds value', JSON.stringify(it.design().hands['centre-hour']));
  // a seconds SUBDIAL hand is the same kind to the engine, so it gets the same swap.
  // design() hands back a canonDesign COPY, so the change has to land on the stored design.
  const sub = it.state.designs.calendar.comps.find((c) => c.type === 'subdial');
  ok(!!sub, 'the preset has a subdial to turn into a seconds counter', JSON.stringify(!!sub));
  const subScale = sub.scale;
  sub.scale = 'seconds';
  const subRow = (it.state.tab = 'hands', it.renderVals().handRows).find((q) => q.anchor === 'hand-' + sub.id);
  ok(!!subRow && subRow.isSec === true && subRow.notSec === false, 'a subdial counting seconds loses the dead style list too', JSON.stringify(subRow && [subRow.label, subRow.isSec, subRow.notSec]));
  sub.scale = subScale;
  it.state.designs.calendar.hands = JSON.parse(handsBefore);

  // ---- the eye on the hands rows ---------------------------------------------------
  // Centre rows read their label off design.centre alone and buildHands honoured
  // hands[id].on only for subdial hands: the eye on a centre row wrote a flag nobody
  // read, so the hand stayed on the glass and the button never changed. Round-trip it.
  const eyeRow = (label) => (it.state.tab = 'hands', it.renderVals().handRows).find((q) => q.label === label);
  ok(it.face().hands.some((h) => h.id === 'centre-hour'), 'the hour hand starts on the glass');
  eyeRow('Centre hour').toggle();
  ok(it.design().hands['centre-hour'].on === false, 'the eye writes the hide where the engine can read it');
  ok(!it.face().hands.some((h) => h.id === 'centre-hour') && it.face().hands.some((h) => h.id === 'centre-minute'),
    'and that hand is gone from the render while its neighbours keep turning');
  const hidRow = eyeRow('Centre hour');
  ok(hidRow.eye === 'hidden' && hidRow.op === 0.5 && /hidden/.test(hidRow.meta),
    'the row admits it is hidden -- the button that never moved now moves', JSON.stringify([hidRow.eye, hidRow.op, hidRow.meta]));
  hidRow.toggle();
  ok(it.face().hands.some((h) => h.id === 'centre-hour'), 'and a second click puts the hand back');
  ok(!('on' in (it.design().hands['centre-hour'] || {})), 'showing again canonicalises to no flag at all, so the Design ID survives the round trip');
  const anySubRow = (it.state.tab = 'hands', it.renderVals().handRows).find((q) => q.anchor && !/^hand-centre-/.test(q.anchor));
  if (anySubRow) {
    const sid = anySubRow.anchor.slice(5);
    anySubRow.toggle();
    ok(it.face().hands.every((h) => h.id !== sid), 'the eye hides a subdial hand too', sid);
    (it.state.tab = 'hands', it.renderVals().handRows).find((q) => q.anchor === 'hand-' + sid).toggle();
    ok(it.face().hands.some((h) => h.id === sid), 'and shows it again', sid);
  }


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
  const ghostRots = (rot0) => {
    it.state.designs.calendar.hands = { 'centre-hour': { art: 'ui-big', rot0: rot0 } };
    it.state.selKind = 'hand'; it.state.sel = 'centre-hour';
    const c = new S.FakeCanvas(); c.width = 466; c.height = 466;
    it.drawHandGhost(c.getContext('2d'), it.face(), dS, 1, (v) => v);
    return c.getContext('2d').geom.filter((o) => o.op === 'rotate').map((o) => o.a[0]);
  };
  const gr0 = ghostRots(0), gr90 = ghostRots(90);
  ok(gr0.length === 2 && gr90.length === 2 && Math.abs((gr90[0] - gr0[0]) - Math.PI / 2) < 1e-6,
    'the virtual hand\u2019s picture turns with its facing: the same art at 90 degrees is drawn exactly a quarter turn round', JSON.stringify([gr0, gr90]));
  ok(Math.abs((gr90[0] + gr90[1]) - (gr0[0] + gr0[1])) < 1e-9,
    'while the read ray stands still: it marks the angle the clock shows, and the facing swings the art under it instead of running away with it', JSON.stringify([gr0[0] + gr0[1], gr90[0] + gr90[1]]));
  it.state.designs.calendar.hands = { 'centre-hour': { art: 'ui-big', rot0: 90 } };
  const gCan3 = new S.FakeCanvas(); gCan3.width = 466; gCan3.height = 466;
  it.drawHandGhost(gCan3.getContext('2d'), it.face(), dS, 1, (v) => v);
  ok(gCan3.getContext('2d').geom.some((o) => o.op === 'strokeRect'), 'and its sprite is outlined where it really lies, which is what shows a wrong facing');
  it.state.designs.calendar.hands = {}; delete it.state.designs.calendar.sfZoom;
  /* ---- the picked complication is dragged through the hand on top of it -----------
   * Selected on the Complications tab, reached for on the dial: the author means that
   * item, and a hand painted across it must not steal the grab. The same-spot cycle
   * still walks the stack, so the hand over it remains reachable too. */
  let overPt = null, overId = null;
  for (const hh of fcS.lay.hits) {
    const q = fcS.pickAll(hh.x, hh.y, dS);
    if (q.length > 1 && q[0].kind === 'hand' && q.some((r) => r.kind === 'comp' && String(r.id) === String(hh.id))) { overPt = [hh.x, hh.y]; overId = hh.id; break; }
  }
  ok(!!overPt, 'the calendar dial really has a hand lying over a complication to test on', JSON.stringify(overPt));
  it.state.tab = 'comps'; it._clickAt = null; it.drag = null;
  it.state.sel = overId; it.state.selKind = 'comp';
  it.onDown(ev(overPt[0], overPt[1]));
  ok(it.state.selKind === 'comp' && String(it.state.sel) === String(overId) && it.drag && it.drag.kind === 'comp' && String(it.drag.id) === String(overId),
    'a complication picked on the tab is grabbed on the dial straight through the hand painted over it', JSON.stringify([it.state.sel, it.state.selKind, it.drag && it.drag.kind, it.drag && it.drag.id]));
  it.onDown(ev(overPt[0], overPt[1]));
  ok(String(it.state.sel) !== String(overId), 'and a second click on the same spot still steps down the stack, so the hand above it is reachable', String(it.state.sel));
  it.drag = null;
  // the tab row's own pick marks the kind, which is what arms the sticky grab
  it.state.sel = null; it.state.selKind = null;
  const cIdx = it.design().comps.findIndex((c) => String(c.id) === String(overId));
  it.renderVals().compRows[cIdx].pick();
  ok(String(it.state.sel) === String(overId) && it.state.selKind === 'comp', 'picking the row from the Complications list sets it as the active complication, ready to grab on the stage', JSON.stringify([it.state.sel, it.state.selKind]));
  /* ---- a click on the black around the watch lets go of everything -----------------
   * The canvas cannot see a pointer-down outside itself; without the stage handler
   * the only exits from a tab-side selection were Escape and the Deselect button. */
  const stage = {};
  it.state.sel = overId; it.state.selKind = 'comp';
  it.renderVals().stageDown({ target: stage, currentTarget: stage });
  ok(it.state.sel == null && it.state.selKind == null, 'clicking the surround clears the selection', JSON.stringify([it.state.sel, it.state.selKind]));
  it.state.sel = overId; it.state.selKind = 'comp';
  it.renderVals().stageDown({ target: {}, currentTarget: stage });
  ok(String(it.state.sel) === String(overId) && it.state.selKind === 'comp', 'a pointer-down that bubbled up from the canvas keeps its own verdict', String(it.state.sel));
  it.state.sel = null; it.state.selKind = null;
  // ---- the controls themselves -------------------------------------------------------
  const dialZ = labels('dial');
  ok(!!byLabel(dialZ, 'Zoom'), 'each art slot offers a zoom, not only a box size');
  it.state.caseStyle = 'none';
  const caseNone = labels('case');
  ok(!!byLabel(caseNone, 'Opening radius'), 'no case exposes the opening that lighting, shadow and clipping then use');
  // The slider has no pixel effect of its own any more, so the only thing it still owes the
  // author is the truth: a note that promised to clear or cut the content would describe the
  // black ring this option used to paint over their own baked-in case.
  const openNote = (caseNone.find((f) => f.isNote && /carries the case/.test(f.note || '')) || {}).note || '';
  ok(!!openNote && !/clear|clip|cut|rescal/i.test(openNote.replace(/clips nothing|cuts nothing|rescales nothing|clipping/gi, '')),
    'the opening note says what the option does, and promises no cut, no clip and no rescale', JSON.stringify(openNote).slice(0, 150));
  it.state.caseStyle = 'polished';
  ok(!byLabel(labels('case'), 'Opening radius'), 'a real case hides it again: its own rim owns that number');
  const winC = (it.state.designs.calendar.comps || []).find((c) => c.type === 'window');
  ok(!!winC, 'the calendar preset has a window to test on');
  if (winC) { winC.wshape = 'circle'; it.state.sel = winC.id; const cw = labels('comps'); ok(!!byLabel(cw, 'Aperture'), 'a window offers an aperture shape'); const ap = byLabel(cw, 'Aperture'); ok(ap && (ap.opts || []).some((o) => /Round/.test(o.label)), 'with round among its choices', JSON.stringify((ap || {}).opts || []).slice(0, 80)); winC.wshape = 'rect'; }
  /* ---- the gilt frame is a choice now, not a constant ------------------------------
   * A cutout framed in steel is a different watch; the metal row appears exactly
   * when a style actually strikes a raised ring, and the pick lands on the comp. */
  if (winC) {
    it.state.sel = winC.id; it.state.selKind = 'comp';
    const winLive = () => it.state.designs.calendar.comps.find((c) => c.id === winC.id);
    const fr0 = byLabel(labels('comps'), 'Frame colour');
    ok(!!fr0 && fr0.isChips && (fr0.opts || []).some((o) => /Steel/.test(o.label || '')), 'a framed window offers the frame metal, steel among them', JSON.stringify(((fr0 || {}).opts || []).map((o) => o.label).slice(0, 8)));
    const stOpt = fr0 && fr0.opts.find((o) => /Steel/.test(o.label || ''));
    if (stOpt) stOpt.pick();
    ok(winLive().frameColor === 'steel', 'and picking Steel writes the metal on the window', JSON.stringify(winLive().frameColor));
    winLive().style = 'plain';
    ok(!byLabel(labels('comps'), 'Frame colour'), 'a plain cut offers no frame: nothing there draws one');
    winLive().style = 'frame'; winLive().frameColor = 'gilt';
  }
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
  // ---- the reference is the clock's, so it cannot ride along with the art ----------
  // The complaint this answers: every pointer on the overlay moved with the thing being
  // dragged, leaving nothing fixed to line an off-centre graphic up against.
  it.state.selKind = 'dial'; it.state.sel = null; it.state.artSlot = 'sf'; it.state.guide = 'auto'; it.drag = null;
  it.state.designs.calendar.sfArt = 'ui-big'; it.state.designs.calendar.sfSize = 200;
  const gRef = (x, y) => {
    it.state.designs.calendar.sfX = x; it.state.designs.calendar.sfY = y;
    const c = new S.FakeCanvas(); c.width = 466; c.height = 466;
    it.drawGuide(c.getContext('2d'), it.face(), it.now(), 1);
    const G = c.getContext('2d').geom;
    return {
      ref: G.filter((o) => (o.op === 'lineTo' && Math.abs(Math.hypot(o.a[0] - 233, o.a[1] - 233) - 216) < 0.01)
        || (o.op === 'lineTo' && Math.abs(Math.hypot(o.a[0] - 233, o.a[1] - 233) - 13) < 0.01)
        || (o.op === 'arc' && (Math.abs(o.a[2] - 221.5) < 0.01 || (o.a[0] === 233 && o.a[1] === 233 && o.a[2] === 3.5))))
        .map((o) => o.op + ':' + o.a.join(',')).sort().join('|'),
      box: (G.filter((o) => o.op === 'strokeRect').pop() || { a: [] }).a.slice(0, 2),
      n233: G.filter((o) => o.op === 'moveTo' && o.a[0] === 233 && o.a[1] === 233).length,
    };
  };
  const r0 = gRef(0, 0), r1 = gRef(40, -20);
  ok(r0.ref.length > 100 && r0.n233 === 24, 'the reference is drawn out of the hub, with all 24 spokes starting at the centre', 'n233=' + r0.n233);
  ok(r0.ref === r1.ref, 'and it does not budge a single line when the dial art is dragged: it is the thing an off-centre graphic gets aimed at, not another thing to aim', JSON.stringify([r0.ref.length, r1.ref.length]));
  ok(Math.abs(r1.box[0] - r0.box[0] - 40) < 0.01 && Math.abs(r1.box[1] - r0.box[1] + 20) < 0.01,
    'while the art does move, exactly the amount dragged, so placed art and fixed grid read against each other', JSON.stringify([r0.box, r1.box]));
  const gHub = new S.FakeCanvas(); gHub.width = 466; gHub.height = 466;
  it.state.designs.calendar.sfX = 120; it.state.designs.calendar.sfY = 120;
  it.drawGuide(gHub.getContext('2d'), it.face(), it.now(), 1);
  ok(gHub.getContext('2d').geom.some((o) => o.op === 'arc' && o.a[0] === 233 && o.a[1] === 233 && o.a[2] === 3.5),
    'the hub cross stays at the centre of the watch even with the art shoved into a corner');
  // Under a style that scales the face, the ring must still be drawn where the metal cuts
  // (raw panel space), not where the scaled content reaches: a ring that shrinks with the
  // content tells the author the rim is somewhere it is not.
  const gBez = new S.FakeCanvas(); gBez.width = 466; gBez.height = 466;
  const kb = it.face().inset, fcB = Object.assign({}, it.face(), { caseOpen: 193.5, inset: 0.876 });
  it.drawGuide(gBez.getContext('2d'), fcB, it.now(), 0.876);
  const bezArcs = gBez.getContext('2d').geom.filter((o) => o.op === 'arc').map((o) => +o.a[2].toFixed(2));
  ok(bezArcs.indexOf(193.5) >= 0 && bezArcs.indexOf(+(193.5 * 0.876).toFixed(2)) < 0,
    'the opening ring is drawn at the radius the mask cuts at, not at the scaled one', JSON.stringify(bezArcs) + ' inset=' + kb);
  it.state.designs.calendar.sfX = 0; it.state.designs.calendar.sfY = 0; it.state.designs.calendar.sfSize = null;
  // ---- a way out of the selection, which clicking cannot provide ------------------
  it.state.sel = 'cd1'; it.state.selKind = 'comp';
  ok(it.onKey({ key: 'Escape' }) === true, 'Escape answers a selection');
  ok(it.state.sel === null && it.state.selKind === null, 'and clears it: with the dial selected every pixel of the face IS the dial, so there is no empty pixel to click instead', JSON.stringify([it.state.sel, it.state.selKind]));
  it.state.sel = 'cd1';
  ok(it.onKey({ key: 'Escape', target: { tagName: 'INPUT' } }) === false, 'Escape inside a text field is left to the field');
  ok(it.state.sel === 'cd1', 'and leaves the selection alone there');
  it.state.sel = null; it.state.selKind = null; it.state.guide = 'auto';
  ok(it.onKey({ key: 'Escape' }) === false, 'Escape with nothing to undo is not an event');
  it.state.selKind = 'hand'; it.drag = { kind: 'hand', id: 'centre-hour' };
  ok(it.onKey({ key: 'Escape' }) === true && !it.drag && it.state.selKind === null, 'Escape drops an in-flight pivot drag too, which is the moment it is needed most');
  it.state.guide = 'auto';
  ok(it.guideOn() === false, 'and the overlay goes back to auto, so a clean look at the watch is one keypress away');
  it.state.sel = 'centre-hour'; it.state.selKind = 'hand';
  const rvOv = it.renderVals();
  ok(/Deselect hand/.test(rvOv.clearLabel), 'the button under the stage names what it will clear', rvOv.clearLabel);
  ok(rvOv.clearOp === 1, 'and is lit while there is something to clear', String(rvOv.clearOp));
  it.state.sel = null; it.state.selKind = null;
  ok(it.renderVals().clearOp === 0.45, 'faded when there is nothing to clear');
  it.state.guide = 'auto';
  it.cycleGuide(); ok(it.state.guide === 'on', 'the Guide button pins the overlay on');
  it.cycleGuide(); ok(it.state.guide === 'off', 'off');
  it.cycleGuide(); ok(it.state.guide === 'auto', 'and back to auto');
  const rvG = it.renderVals().g;
  ok(rvG.label === 'Guide: auto' && rvG.bd === '#c9a25a', 'and the button says which of the three it is', JSON.stringify(rvG));
  // ---- the pane must scroll to the section, not merely change tab ------------------
  it.state.selKind = 'hand'; it.state.sel = 'centre-second';
  ok(it.scrollTarget() === 'hand-centre-second', 'a selected hand asks for its own row', String(it.scrollTarget()));
  it.state.selKind = 'comp'; it.state.sel = it.design().comps[0].id;
  ok(it.scrollTarget() === 'comp-' + it.design().comps[0].id, 'a selected complication asks for its row in the list', String(it.scrollTarget()));
  it.state.selKind = 'dial'; it.state.artSlot = 'sf'; it.state.designs.calendar.sfArt = 'ui-big';
  ok(it.scrollTarget() === 'art-sf', 'and the dial asks for the section holding the slot being panned', String(it.scrollTarget()));
  it.state.designs.calendar.sfArt = null; it.state.designs.calendar.bgArt = null;
  ok(it.scrollTarget() === '#', 'a dial with no art in either slot asks for the top of the pane instead of nowhere');
  it.state.designs.calendar.sfArt = 'ui-big';
  // a target is only worth having if the pane really emits that anchor: a mismatch is a
  // silent no-op, and a silent no-op is exactly the bug that let the tab change while the
  // view stayed put
  const rvH = (it.state.tab = 'hands', it.renderVals());
  ok(rvH.handRows.some((q) => q.anchor === 'hand-centre-second'), 'the hands pane emits the anchor a selected hand aims at', JSON.stringify(rvH.handRows.map((q) => q.anchor)));
  const rvD = (it.state.tab = 'dial', it.renderVals());
  ok(rvD.fields.some((f) => f.anchor === 'art-sf'), 'and the dial pane anchors its surface-art section', JSON.stringify(rvD.fields.filter((f) => f.anchor).map((f) => f.anchor)));
  const rvC = (it.state.tab = 'comps', it.renderVals());
  ok(rvC.compRows.some((q) => q.anchor === 'comp-' + it.design().comps[0].id), 'and the comps pane anchors each complication', JSON.stringify(rvC.compRows.map((q) => q.anchor)));
  it._clickAt = null; it.state.tab = 'comps'; it.state.sel = null; it.state.selKind = null;
  it.onDown(ev(top.x, top.y));
  ok(it._scrollTo === 'hand-' + top.id, 'a stage click arms the scroll target for whatever it grabbed', String(it._scrollTo));
  const armed = it._scrollTo;
  const pane = {
    scrollTop: 500,
    getBoundingClientRect: () => ({ top: 0, bottom: 400, left: 0, right: 0 }),
    querySelector: (s) => (s === '[data-a="' + armed + '"]' ? { getBoundingClientRect: () => ({ top: 900, bottom: 1000, left: 0, right: 0 }) } : null),
  };
  it.paneRef.current = pane;
  const wasReady = it.ready; it.ready = false;              // the consume runs ahead of the ready guard; the rest of the hook wants the build gate
  it.componentDidUpdate();
  ok(it._scrollTo === null, 'componentDidUpdate spends the armed target instead of leaving it for a render that may never come');
  ok(pane.scrollTop !== 500, 'and that spend moves the pane, which is the whole difference between changing a tab and seeing the field', String(pane.scrollTop));
  it.ready = wasReady;
  it.paneRef.current = null;
  ok(it.scrollToAnchor('art-sf') === false, 'with no pane bound there is nothing to scroll, and it reports that rather than pretending');
  it.paneRef.current = { scrollTop: 0, getBoundingClientRect: () => ({ top: 0, bottom: 400, left: 0, right: 0 }), querySelector: () => null };
  ok(it.scrollToAnchor('#') === true, 'the top anchor needs no element to find');
  ok(it.scrollToAnchor('hand-nothing') === false, 'an anchor the pane does not emit is reported as not scrolled, not as success');
  it.paneRef.current = null;
  it.state.designs.calendar.sfArt = null;
  it.state.designs.calendar.hands = {};
  if (checked < 20) { fails++; console.log('FAIL only ' + checked + ' checks ran'); }
  // every {{ }} binding the template reads must exist on the props object, or the DC
  // runtime renders an empty control with no warning at all
  const ex2 = (it.state.tab = 'export', it.renderVals());
  const rv = it.renderVals();
  const r3 = rv.handRows.find((q) => q.label === 'Centre hour');
  for (const k of ['onPivotX', 'onPivotY', 'onPivotCentre', 'onPivotHub', 'onArtFile', 'onArtClear', 'artRef', 'artLabel', 'artNote', 'onArtDown', 'onArtMove', 'hasArt'])
    ok(r3[k] !== undefined, 'handRows exposes ' + k);
  for (const k of ['exportKit', 'exportTheme', 'themeNote', 'exportBundle', 'exportFrames', 'loadBundle', 'bundleNote', 'artNote', 'kitNote', 'designId', 'loadDesign', 'copyId', 'onIdInput', 'selectAll',
    // FORMAT 2's theme-name field: the input, its live byte count and its placeholder
    'themeNameVal', 'themeNameIn', 'themeNamePh', 'themeNameLen',
    // §31's send-to-orb round: the hub-gated push button and its three bindings
    'isHub', 'sendToOrb', 'sendNote',
    // the three bindings the deselect/scroll row reads: a missing one renders a dead button
    'clearSel', 'clearLabel', 'clearOp', 'pickGuide', 'g', 'paneRef'])
    ok(rv[k] !== undefined, 'the template binding ' + k + ' has a backing property');
  // §31's send-to-orb round: under a hub (http/https) the push button names the door;
  // opened from the filesystem (no location) it hides and the note says so. The headless
  // stub has no location, so the binding must report not-hub, never throw.
  ok(rv.isHub === false && rv.sendNote === 'no hub',
     'no location means no hub: the Send button stays hidden', JSON.stringify([rv.isHub, rv.sendNote]));
  ok(typeof rv.sendToOrb === 'function', 'and the send button is still callable off-hub');
  await (async () => {
    // A press with no hub learned no orb: postTheme3ToOrb must report, not dial blindly.
    const g = global, savedLoc = g.location;
    try {
      g.location = { protocol: 'http:' };
      ok(it.hubSend3() === true, 'with http://location the composer reports hub mode');
      ok(/pushes to/.test(it.renderVals().sendNote),
         'under a hub the send note names the door it will push through', it.renderVals().sendNote);
      // with a hub but no orb answer, the send refuses with an actionable sentence
      it.state.hubIp = null; it.state.hubCheck = true;
      await it.postTheme3ToOrb(new Uint8Array([1, 2, 3]), 'x', 'x', { sentence: 'still' }, 'art 3/9 B');
      ok(/not talking to an orb/.test(it.state.status),
         'a hub with no loaded orb is refused, with the download left as the way out', it.state.status.slice(0, 48));
    } finally {
      if (savedLoc === undefined) delete g.location; else g.location = savedLoc;
      it.state.hubCheck = false; it.state.hubIp = null;
    }
  })();
  ok(typeof rv.clearSel === 'function' && typeof rv.pickGuide === 'function', 'and the two overlay controls are callable', JSON.stringify([typeof rv.clearSel, typeof rv.pickGuide]));
  for (const k of ['label', 'bg', 'bd', 'fg']) ok(rv.g[k] !== undefined, 'the Guide button carries its ' + k);
  // FORMAT 2's identity field: an untouched box must still name the theme after the dial,
  // and what the owner types must win verbatim -- no sanitising stands between them now.
  it.state.themeName = '';
  const tDef = it.themeName();
  ok(typeof tDef === 'string' && tDef.length > 0 && /[\s\S]/.test(tDef.trim()),
     'an untouched name field still yields a name', JSON.stringify(tDef));
  const tPreset = (function () { try { const P = it.preset(); return (P && P.name) || null; } catch (e) { return null; } })();
  ok(tDef === (tPreset || it.state.dial),
     'the default name is the dial title the owner reads, never a sanitized token', JSON.stringify([tDef, tPreset, it.state.dial]));
  it.state.themeName = '  Grand Complication — Noir  ';
  ok(it.themeName() === 'Grand Complication — Noir',
     'the typed name travels trimmed and otherwise untouched', JSON.stringify(it.themeName()));
  it.state.themeName = '';
  // ---- the theme export: it must report, never throw -------------------------------
  // A button that dies silently is worse than no button: the author cannot tell a refused
  // bake from a broken page, and every refusal here is something they can act on.
  ok(typeof rv.exportTheme === 'function', 'the theme button is callable');
  ok(/· measures on export/.test(rv.themeNote) || /comp(s)? bake as/.test(rv.themeNote) || /refuses:/.test(rv.themeNote) || /too many/.test(rv.themeNote),
     'the theme button says what it will ship', JSON.stringify(rv.themeNote));
  // §31 turned the button on: the FORMAT 3 bake carries the moving train as a
  // measured keyframe/carriage bank, and §31's last slice plates the live
  // complications: the default Skeleton's small-seconds rides 60 window plates
  // the dial indexes itself, and the note counts them before the press — the
  // same compPlan3 the export runs, so note and bake cannot disagree.
  ok(/FORMAT 3 · \d+ comps? bake as \d+ plates/.test(rv.themeNote),
     'and it names the comp table it will bake for a dial with live complications',
     JSON.stringify(rv.themeNote));
  await (async () => {
    const name = { got: null }, threw = { err: null };
    it.download = (b, n) => { name.got = n; };
    try {
      // a dial whose layers cannot be found must land as a sentence, not an exception
      try { await it.finishTheme([], { size: 466, frames: 8, fps: 8, hands: [], drawOrder: [] }, 'cal'); }
      catch (e) { threw.err = e; }
      ok(!threw.err, 'an unusable kit throws nothing at the button', threw.err && threw.err.message);
      ok(/^Not baked: /.test(it.state.status) && name.got === null,
         'and it is refused with the reason, without a download', it.state.status.slice(0, 72));
      // FORMAT 2 took the card charset out of the name entirely: the file on the card is
      // sha256(name)[:12], so an awkward name bakes verbatim instead of being sanitised.
      await it.finishTheme([], { size: 466, frames: 8, fps: 8, hands: [], drawOrder: [] }, '.Hidden Name!');
      ok(!/not a theme name/.test(it.state.status), 'an awkward name is a name now, not a refusal', it.state.status.slice(0, 40));
      ok(!/[\/\\]/.test(String(name.got)), 'and however odd the name, the download it produced carries no path', String(name.got));
      let checked31 = null;
      const origOk = global.window.GorbBake.nameOk;
      global.window.GorbBake.nameOk = (s) => { checked31 = s; return origOk(s); };
      await it.finishTheme([], { size: 466, frames: 8, fps: 8, hands: [], drawOrder: [] }, 'a'.repeat(30));
      ok(checked31 === 'a'.repeat(30) && global.window.GorbBake.nameBytes(checked31) <= global.window.GorbBake.NAME_MAX - 1,
         'the name handed to the baker is what was typed, and inside the header field', JSON.stringify(checked31));
      global.window.GorbBake.nameOk = origOk;
      // the one name rule the header still enforces, checked as a refusal this time
      await it.finishTheme([], { size: 466, frames: 8, fps: 8, hands: [], drawOrder: [] }, 'x'.repeat(global.window.GorbBake.NAME_MAX));
      ok(/Not baked/.test(it.state.status) && /measures 56/.test(it.state.status),
         'a name one byte past the field is refused, and says which byte it measured', it.state.status.slice(0, 72));
      global.window.GorbBake = null;
      await it.finishTheme([], { size: 466 }, 'ok');
      ok(/did not load/.test(it.state.status), 'a missing baker names the file that is gone', it.state.status.slice(0, 56));
    } finally {
      global.window.GorbBake = require('../orbtheme-baker.js');
      delete it.download;
      const n = checked; if (n < 1) fails++;
    }
  })();
  ok(rv.bundleNote === ex2.bundleNote, 'the bundle label is stable across renders');
  // ---- the build-gate seatbelt: a tempo flip must always be able to rebuild ----------
  // tick is absent from the art signature. Counting every rebuild against the art alone
  // let a 4/s -> sweep -> 4/s round trip on a Broad arrow spend the budget, so the last
  // flip -- the one that needed a re-render -- was silently refused: the bank length
  // stopped matching the tempo, the timer guard froze the frames, and the movement died
  // on screen. The window now opens per wanted rate.
  const g1 = new Comp({});
  g1.ready = true;
  let gBuilds = 0; g1.buildMovement = () => { gBuilds++; };
  for (let i = 0; i < 7; i++) g1.buildGate();                       // exhaust the per-rate budget
  ok(gBuilds === 5 && g1._bp === 5, 'the seatbelt still caps one wanted rate, at enough passes to walk the whole tier ladder', 'builds=' + gBuilds);
  g1.setCfg('tick', '4');
  g1.buildGate();
  ok(gBuilds === 6 && g1._bp === 1, 'a seconds-rate flip opens a fresh rebuild window on unchanged art, so the movement always re-rates for the new bank', 'builds=' + gBuilds + ' bp=' + g1._bp);
  for (let i = 0; i < 7; i++) g1.buildGate();
  g1.setCfg('tick', '4');                                           // same value: still the same wanted rate
  g1.buildGate();
  ok(g1._bp === 5, 'and the cap still holds while the rate is unchanged -- a pathological dial cannot rebuild forever by moving a slider', 'bp=' + g1._bp);
  // ---- the controls the new author decisions need -----------------------------------
  // rotate, finish blend and its colour live in each art block; the blend exists only
  // where a texture can be pressed through, and Width now belongs to drawn hands alone.
  it.state.designs.calendar.bgArt = 'ui-art'; it.state.designs.calendar.sfArt = 'ui-art';
  const nxRot = labels('dial');
  ok(nxRot.filter((f) => f.label === 'Rotate').length === 2, 'both art blocks gained a Rotate slider', nxRot.filter((f) => f.label === 'Rotate').length);
  const nxRf = nxRot.filter((f) => f.label === 'Rotate')[0];
  ok(nxRf.isRange && nxRf.min === 0 && nxRf.max === 359, 'and it walks the whole circle in degrees');
  ok(!nxRot.some((f) => f.label === 'Finish blend'), 'over enamel there is no texture to blend, so the control withholds itself');
  it.state.designs.calendar.surface = 'guilloche';
  const nxBl = labels('dial');
  const nxChips = nxBl.filter((f) => f.label === 'Finish blend');
  ok(nxChips.length === 2 && nxChips[0].isChips, 'a textured surface offers the blend on both art slots', nxChips.length);
  const nxChipV = nxChips[0].opts.map((o) => o.label);
  ok(['Off', 'Multiply', 'Screen', 'Overlay'].every((w) => nxChipV.some((l) => l.indexOf(w) === 0)), 'the modes offered are off, multiply, screen and overlay', JSON.stringify(nxChipV));
  it.state.designs.calendar.bgFinishBlend = 'multiply';
  const nxCo = labels('dial').filter((f) => f.label === 'Blend colour');
  ok(nxCo.length === 1 && nxCo[0].isColor, 'a live blend offers its target colour picker', nxCo.length);
  it.state.designs.calendar.bgFinishColor = '#223344';
  ok(labels('dial').filter((f) => f.label === 'Blend colour')[0].value === '#223344', 'and the picker reports the colour it holds');
  // both carbon finishes were withheld on the theory that a palette cannot be tinted;
  // their weave and their flake field are geometry, so the control must appear for them
  it.state.designs.calendar.surface = 'carbon';
  ok(labels('dial').filter((f) => f.label === 'Finish blend').length === 2, 'woven carbon offers the blend on both art slots, not just the engine-turned finishes');
  it.state.designs.calendar.surface = 'forged';
  ok(labels('dial').filter((f) => f.label === 'Finish blend').length === 2, 'forged carbon offers it too');
  it.state.designs.calendar.surface = 'enamel';
  ok(!labels('dial').some((f) => f.label === 'Finish blend'), 'and enamel still withholds it: there is genuinely nothing to press through');
  it.state.designs.calendar.surface = 'forged';
  it.state.designs.calendar.bgFinishBlend = 'multiply';
  ok(labels('dial').filter((f) => f.label === 'Blend colour').length === 1, 'with the blend live on a carbon dial the colour picker is there');
  // the colour key is a gate ON TOP of that blend: it appears beside it, and its
  // tolerance only exists while a key colour actually gates something
  const nxKey0 = labels('dial').filter((f) => f.label === 'Key colour');
  ok(nxKey0.length === 1 && nxKey0[0].isColor, 'a live blend offers the key colour beside the blend colour', nxKey0.length);
  ok(!nxKey0[0].opts.some((o) => /^Auto/.test(o.label)) && nxKey0[0].opts.some((o) => o.label === 'None'), 'the key starts at None and has no auto to fall back to', JSON.stringify(nxKey0[0].opts.map((o) => o.label).slice(0, 3)));
  ok(!labels('dial').some((f) => f.label === 'Key tolerance'), 'no key colour, no tolerance slider');
  nxKey0[0].opts.find((o) => o.label === 'Red').pick();
  ok(it.state.designs.calendar.bgFinishKey === '#c8322a', 'a key chip writes the hex it names', it.state.designs.calendar.bgFinishKey);
  const nxTol = labels('dial').filter((f) => f.label === 'Key tolerance');
  ok(nxTol.length === 1 && nxTol[0].isRange && nxTol[0].min === 0 && nxTol[0].max === 128, 'and a live key gains the tolerance slider, 0 to 128', nxTol.length && [nxTol[0].min, nxTol[0].max]);
  nxTol[0].on({ target: { value: '7' } });
  ok(it.state.designs.calendar.bgFinishKeyTol === 7, 'the tolerance slider writes the design', it.state.designs.calendar.bgFinishKeyTol);
  nxKey0[0].opts.find((o) => o.label === 'None').pick();
  ok(it.state.designs.calendar.bgFinishKey === undefined, 'None takes the key off again — canonDesign drops it — without touching the blend');
  ok(it.state.designs.calendar.bgFinishBlend === 'multiply', 'and the whole-image blend the author had is still on');
  ok(!labels('dial').some((f) => f.label === 'Key tolerance'), 'the tolerance slider goes away with the key it belonged to');
  it.state.designs.calendar.bgFinishKey = '#c8322a';
  ok(labels('dial').some((f) => f.label === 'Key tolerance'), 'and it comes back with the key');
  it.state.designs.calendar.bgFinishBlend = 'off';
  ok(!labels('dial').some((f) => f.label === 'Key colour'), 'blend off and the key withholds itself too: there is no pass left to gate');
  it.state.designs.calendar.bgFinishBlend = 'multiply';
  delete it.state.designs.calendar.bgFinishKey; delete it.state.designs.calendar.bgFinishKeyTol;
  const kA = it.mvKey(1);
  it.state.designs.calendar.bgFinishKey = '#c8322a';
  const kB = it.mvKey(1);
  it.state.designs.calendar.bgFinishKeyTol = 30;
  const kC = it.mvKey(1);
  ok(kA !== kB && kB !== kC, 'the plate-slot key and its tolerance re-price the base frames, because they change the pixels baked into them', [kA === kB, kB === kC]);
  delete it.state.designs.calendar.bgFinishKey; delete it.state.designs.calendar.bgFinishKeyTol;
  ok(it.mvKey(1) === kA, 'and dropping the key returns the movement to the signature it had before');
  it.state.designs.calendar.bgFinishKey = '#c8322a';
  it.state.designs.calendar.surface = 'enamel';
  ok(!labels('dial').some((f) => f.label === 'Key colour'), 'enamel withholds all three: no texture means nothing to key');
  delete it.state.designs.calendar.bgFinishColor; delete it.state.designs.calendar.bgFinishBlend; delete it.state.designs.calendar.surface;
  delete it.state.designs.calendar.bgFinishKey; delete it.state.designs.calendar.bgFinishKeyTol;
  it.state.designs.calendar.hands = { 'centre-hour': { art: 'ui-big' } };
  const nxWRow = (it.state.tab = 'hands', it.renderVals().handRows).find((q) => q.label === 'Centre hour');
  ok(nxWRow.hideWid === true && nxWRow.showWid === false, 'the sprite-hand row takes its Width slider away', nxWRow.artLabel);
  delete it.state.designs.calendar.hands['centre-hour'].art;
  const nxDRow = (it.state.tab = 'hands', it.renderVals().handRows).find((q) => q.label === 'Centre hour');
  ok(nxDRow.showWid === true && nxDRow.hideWid === false, 'and the drawn hand keeps it: the slider shapes a silhouette a sprite does not have');
  // ---- §32 the send leg: watched, modal-armed, never a browser dialog --------------
  {
    let confirmed = false;
    global.confirm = () => { confirmed = true; return true; };
    it.state.hubCheck = true; it.state.hubIp = '10.0.0.9';
    // §32.7: a bake big enough to tell the precheck's 128-byte header from the full
    // body. The old precheck posted the WHOLE container, so every plain send moved
    // every byte twice; this array makes the double-transfer and its fix measurable.
    const bytes = Uint8Array.from({ length: 300 }, (_, i) => (i * 7 + 1) & 0xff);
    const sent = [];
    global.fetch = async (url, opt) => {
      sent.push([url, opt && opt.body && opt.body.length]);
      const j = /precheck=1/.test(url) ? { ok: true, data: { duplicate: scen.dup, name: 'orbital-arrow' } }
        : { ok: true, data: { phase: 'uploading', sent: 3, total: 3 } };
      return { ok: true, status: 200, json: async () => j };
    };
    let xhrUrl = null, xhrBody = null, xhrSends = 0;
    global.XMLHttpRequest = function () {
      this.open = (m, u) => { xhrUrl = u; };
      this.setRequestHeader = () => { };
      this.upload = {};
      this.send = (b) => {
        xhrSends++;
        xhrBody = b;
        this.upload.onprogress({ loaded: b.length, total: b.length });
        this.status = 200;
        this.responseText = JSON.stringify({ ok: true, data: { name: 'orbital-arrow', id: 'ab12cd34ef56', store: 'stored' } });
        this.onload();
      };
    };
    const scen = { dup: true };
    // the bindings the template actually reads for the modal's two buttons
    const dupBtns = () => { const v = it.renderVals(); return { open: v.dupOpen, nm: v.dupNm, stem: v.dupStem, pre: v.dupPre, replace: v.dupReplace, cancel: v.dupCancel }; };
    it.state.themeName = 'orbital-arrow';
    it._replaceArmed = null; it.state.dup = null;
    await it.postTheme3ToOrb(bytes, 'orbital-arrow', 'stem', { sentence: 'p' }, 'm', false);
    let dv = dupBtns();
    ok(dv.open === true && dv.nm === 'orbital-arrow' && dv.stem === 'stem', 'a duplicate opens the in-page modal naming the dial', JSON.stringify(dv.open && [dv.nm, dv.stem]));
    ok(dv.pre === true, 'the modal says the precheck caught it before any byte crossed');
    ok(!!it._replaceArmed && it._replaceArmed.bytes === bytes, 'the refused bake is held for the modal Replace');
    ok(sent.length === 1 && /precheck=1/.test(sent[0][0]), 'the refusal came from the precheck alone — no whole-bake body posted', JSON.stringify(sent));
    ok(sent.length === 1 && sent[0][1] <= 128, '§32.7: the precheck leg carried at most the 128-byte header, not the bake', JSON.stringify(sent));
    scen.dup = false;
    await it.sendTheme3ToOrb();
    ok(!confirmed, 'the replace flow never opens a browser dialog');
    ok(/&overwrite=1/.test(xhrUrl || ''), 'the second press sends the held bytes with the firmware flag', xhrUrl);
    ok(xhrBody === bytes, 'and it is the SAME bake, byte-identical, not a re-bake');
    ok(xhrBody && xhrBody.length === 300, '§32.7: the real send ships the full bake once — the precheck was the only short body', xhrBody && xhrBody.length);
    ok(/rebooting onto it/.test(it.state.status), 'the settled line quotes the store verdict', it.state.status);
    scen.dup = true;
    it._replaceArmed = null; it.state.dup = null;
    await it.postTheme3ToOrb(bytes, 'orbital-arrow', 'stem', { sentence: 'p' }, 'm', false);
    ok(!!it._replaceArmed && dupBtns().open, 'the second refusal re-arms and reopens the modal on the same name');
    // the modal Replace ships the held bytes with the flag and closes the overlay
    dv = dupBtns();
    await dv.replace();
    ok(dupBtns().open === false, 'the modal Replace closes the overlay');
    ok(/&overwrite=1/.test(xhrUrl || '') && xhrBody === bytes, 'and the modal Replace ships those held bytes byte-identical, flagged to overwrite', xhrUrl);
    // a Cancel discards the arm so the next press is a fresh bake
    it._replaceArmed = null; it.state.dup = null;
    await it.postTheme3ToOrb(bytes, 'orbital-arrow', 'stem', { sentence: 'p' }, 'm', false);
    const sendsBefore = xhrSends;
    await dupBtns().cancel();
    ok(dupBtns().open === false && it._replaceArmed === null, 'Cancel closes the modal and drops the held arm');
    ok(/Send cancelled/.test(it.state.status), 'and the status line says the card keeps its dial', it.state.status);
    it.state.themeName = 'other-dial';
    const urlBefore = xhrUrl;
    await it.sendTheme3ToOrb();
    ok(it._replaceArmed === null,
       'after a Cancel the arm is gone, so a press has no held bytes to re-ship');
    ok(xhrUrl === urlBefore || !/overwrite=1/.test(xhrUrl || ''),
       'renaming and pressing after a Cancel never flags a re-send of the discarded bake', xhrUrl);
  }

  console.log(fails ? '\n' + fails + ' FAILED' : '\nall green (' + checked + ' checks)');
  process.exit(fails ? 1 : 0);
}, 15);
