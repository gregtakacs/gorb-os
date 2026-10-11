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
