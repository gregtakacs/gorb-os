#!/usr/bin/env node
// D43 P5: the build flow, walked through the real component — from scratch and from a
// preset, Start to Render — and the promise that the steps removed nothing: every field
// every old tab offered is still reachable from a step (with every More fold opened), and
// every binding the pre-P5 template used is still bound, or retired with its replacement
// named. P5b (§4.2) re-laid it as the studio: layers on every step, the inspector.
'use strict';
const fs = require('fs');
const path = require('path');
require('./headless-stub.js');
const E = global.WatchEngine;
global.React = { createRef: () => ({ current: null }) };
require('./component.js');
let fails = 0, checked = 0;
const ok = (c, n, x) => { checked++; if (!c || process.env.VERBOSE) console.log((c ? 'PASS ' : 'FAIL ') + n + (x ? '  [' + x + ']' : '')); if (!c) fails++; };
const DCLogic = function () { };
DCLogic.prototype.setState = function (p) { Object.assign(this.state, typeof p === 'function' ? p(this.state) : p); };
const Comp = global.COMP_CLASS(DCLogic);
const mount = (dial) => {
  const it = new Comp({});
  it.ready = true; it.rendering = false; it.budget = () => ({ fps: 8, dev: { pace: 8, cross: 0, cov: 0, why: '' } });
  it.state.dial = dial; it.state.playing = false; it.state.hidden = {};
  it.state.designs = { [dial]: E.canonDesign(JSON.parse(JSON.stringify(E.PRESETS[dial].design))) };
  return it;
};
const labels = (v) => (v.fields || []).filter((f) => f.isRow && f.label).map((f) => f.label);
const stepOf = (it) => it.stepNow()[0];

// ---- from scratch, Start to Render -----------------------------------------
{
  const it = mount('skeleton'), V = () => it.renderVals();
  ok(stepOf(it) === 'start' && V().isStart && V().fields.length === 0, 'the composer opens on Start: the gallery, no fields');
  ok(V().stepOpts.map((t) => t.label).join(' | ') === 'Start | Movement | Dial | Parts | Finish | Render', 'six steps, in build order', V().stepOpts.map((t) => t.label).join(' | '));
  ok(V().stepOpts.filter((t) => t.cls === 'on').length === 1 && V().stepOpts[0].cls === 'on', 'one step is marked, the one shown');
  ok(V().stackRows.some((r) => r.isRow), 'the layers panel is there from Start');
  const skel0 = JSON.stringify(it.state.designs.skeleton || null);
  V().fromScratch();
  const nonHand = () => (it.design().comps || []).filter((c) => c.type !== 'hand');
  const handsOn = (host) => (it.design().comps || []).filter((c) => c.type === 'hand' && (c.host || 'dial') === host);
  // owner, 2026-11-06: from scratch is nothing at all -- no dial, no parts, no case, no glass, no light
  ok(stepOf(it) === 'movement' && (it.design().comps || []).length === 0 && it.design().noDial === true, 'Build from scratch: no dial and no parts, on to the movement', JSON.stringify(it.design()));
  ok(it.state.caseStyle === 'none' && it.state.glassStyle === 'none' && +it.cfg().glow === 0 && +it.cfg().outerLight === 0, 'and no case, no glass, no light', JSON.stringify([it.state.caseStyle, it.state.glassStyle, it.cfg().glow, it.cfg().outerLight]));
  ok(it.state.dial === 'scratch' && JSON.stringify(it.state.designs.skeleton || null) === skel0 && it.themeName() === 'Untitled' && !V().dials.some((x) => x.cls === 'on'),
     'it is its own watch, Untitled: Skeleton\u2019s design is left as it was, and no gallery watch is the one open', JSON.stringify([it.state.dial, it.themeName()]));
  ok(V().isMove, 'the Movement step shows the movement cards');
  ok(V().hasSubs && V().subOpts.map((t) => t.label).join() === 'Geartrain,Clock insides', 'Movement has two sub-tabs: the train and the plate behind it', V().subOpts.map((t) => t.label).join());
  V().subOpts[1].pick();
  ok(labels(V()).includes('Plate') && !labels(V()).includes('Metal') && !V().isMove, 'Clock insides holds the plate and its art; the train\u2019s rows and cards stay on Geartrain', labels(V()).slice(0, 4).join(','));
  V().subOpts[0].pick();
  V().stepOpts[2].pick();
  ok(stepOf(it) === 'dial' && labels(V()).includes('Dial') && labels(V()).length < 5, 'the Dial step says there is no dial and offers it back', labels(V()).join(','));
  V().fields.find((f) => f.isChips && (f.opts || []).some((o) => /Put the dial back/.test(o.label))).opts.find((o) => /back/.test(o.label)).pick();
  ok(!it.design().noDial && stepOf(it) === 'dial' && labels(V()).length > 5, 'putting it back opens the dial\u2019s own fields', labels(V()).slice(0, 4).join(','));
  ['hand', 'hand', 'hand'].forEach((t) => { it.setState({ sel: null, selKind: null }); it.addComp(t); });
  ok(handsOn('dial').map((h) => h.shows).join() === 'hour,minute,second', 'three hands added from the catalogue read hour, minute, seconds', handsOn('dial').map((h) => h.shows).join());
  it.setState({ sel: 'dial', selKind: 'dial', step: 'dial', tab: 'dial' });
  ok(V().hasDel && /dial/.test(V().delLabel), 'the dial is a part that can be removed', V().delLabel);
  V().delSel();
  ok(it.design().noDial === true && V().catOpts[0].label === '+ Dial', 'Remove takes it off, and the catalogue offers it back first', V().catOpts[0] && V().catOpts[0].label);
  V().catOpts[0].go();
  ok(!it.design().noDial, 'and + Dial puts it back');
  it.setState({ step: 'dial', tab: 'dial', sel: null, selKind: null });
  V().stepOpts[3].pick();
  const v4 = V();
  // D43 P5c: Parts is one view -- the catalogue, or the selected part's panel -- with no sub-tabs
  ok(stepOf(it) === 'parts' && v4.isComps && !v4.hasSubs && v4.noSel, 'Parts opens on the catalogue, one view with no sub-tabs');
  ok(v4.catOpts.length === Object.keys(E.COMP_TYPES).length + 1 && v4.catOpts.every((c) => c.blurb) && /Hand/.test(v4.catOpts[0].label), 'the catalogue offers every kind of part, a hand first among them, each with a line on what it is', v4.catOpts.map((c) => c.label).join());
  v4.catOpts.find((c) => /Subdial/.test(c.label)).go();
  const sd = nonHand().find((c) => c.type === 'subdial');
  // owner, 2026-11-06: the face prints no scale; the small seconds hand it brings prints its own
  ok(!!sd && !('scale' in sd) && handsOn(sd.id).length === 1 && handsOn(sd.id)[0].shows === 'second' && handsOn(sd.id)[0].scale && handsOn(sd.id)[0].scale.style === 'classic',
     'a new sub-dial is a face that prints nothing, with a seconds hand that prints the scale', JSON.stringify(handsOn(sd && sd.id)));
  it.setState({ sel: null, selKind: null });
  V().catOpts.find((c) => /Rendered display · text/.test(c.label)).go();
  ok(nonHand().length === 2, 'adding from the catalogue adds to the design', nonHand().map((c) => c.type).join());
  // the selected complication's fields are grouped by part
  it.setState({ tab: 'comps', sel: sd.id, selKind: 'comp' });
  const heads = V().fields.filter((f) => f.isHead).map((f) => f.label);
  ok(heads[0] === 'Form' && heads[1] === 'Placement' && heads.includes('Shape'), 'a sub-dial\u2019s fields are grouped: its Form first, then Placement, Shape, Paint', heads.join(','));
  // the inspector names the selection, and Remove takes it off the dial
  ok(V().inspKind === 'Subdial' || /Sub/i.test(V().inspKind), 'the inspector names the kind of the selected part', V().inspKind + ' / ' + V().inspTitle);
  ok(V().hasDel && /subdial/i.test(V().delLabel), 'and offers to remove it', V().delLabel);
  const n0 = nonHand().length; V().delSel();
  ok(nonHand().length === n0 - 1 && !V().hasDel && handsOn(sd.id).length === 0, 'Remove takes the part off the dial, and its own hand with it', n0 + ' -> ' + nonHand().length);
  // the layers panel's Add opens the catalogue on the right, from any step
  V().stepOpts[2].pick(); it.setState({ sel: handsOn('dial')[0].id, selKind: 'hand' });
  V().toggleAdd();
  ok(stepOf(it) === 'parts' && V().isComps && V().noSel && V().catOpts.length > 3 && it.state.sel == null, 'the layers panel\u2019s Add opens the catalogue in the inspector, from any step');
  V().catOpts.find((c) => /Flare/.test(c.label)).go();
  ok(nonHand().some((c) => c.type === 'flare') && it.state.selKind === 'comp', 'picking from it adds the part and selects it');
  // a hand is a part like the rest: added from the catalogue, it opens its own panel
  it.setState({ sel: null, selKind: null });
  const nh0 = handsOn('dial').length;
  V().catOpts.find((c) => /^\+ Hand/.test(c.label)).go();
  const added = handsOn('dial')[nh0];
  ok(!!added && it.state.sel === added.id && it.state.selKind === 'hand' && stepOf(it) === 'parts', 'adding a hand places it on the dial and selects it', JSON.stringify(added));
  const hp = V();
  ok(hp.isHands && hp.handRows.length === 1 && hp.handRows[0].showsOpts.length === Object.keys(E.HAND_SHOWS).length && hp.inspKind === 'Hand', 'its panel alone, opening on what it shows', hp.handRows.map((h) => h.label).join());
  hp.handRows[0].onShows({ target: { value: 'hours24' } });
  ok(handsOn('dial')[nh0].shows === 'hours24' && it.face().hands.some((h) => h.id === added.id && h.kind === 'hours24'), 'choosing what it shows changes the hand on the dial');
  ok(V().hasDel && V().delLabel === 'hand', 'and the inspector offers to remove it like any part');
  // every hand is arbitrary: ten on the dial is a legal design, priced against R6
  for (let k = handsOn('dial').length; k < 10; k++) { it.setState({ sel: null, selKind: null }); V().catOpts.find((c) => /^\+ Hand/.test(c.label)).go(); }
  ok(handsOn('dial').length === 10 && it.face().hands.filter((h) => !h.sub).length === 10, 'ten hands on the main dial draw', handsOn('dial').length + ' / ' + it.face().hands.length);
  ok(/^\d+ of 24 live rows$/.test(V().stackCap) && V().stackRows.filter((r) => r.isRow && /hand/i.test(r.label) && r.arrows === 'visible').length >= 10, 'each is a layer with its own arrows, counted against the 24 live rows', V().stackCap);
  V().stepOpts[4].pick();
  let v6 = V();
  ok(stepOf(it) === 'finish' && v6.subOpts.map((t) => t.label).join() === 'Light,Case & glass,Sound', 'Finish holds light, case & glass and sound');
  const mood = v6.fields.find((f) => f.label === 'Mood');
  ok(mood && mood.opts.length === 6 && /As designed/.test(mood.opts[0].label) && mood.opts.some((o) => o.label === 'No light'), 'Light opens on its moods, No light among them', mood && mood.opts.map((o) => o.label).join());
  mood.opts.find((o) => o.label === 'No light').pick();
  ok(+it.cfg().glow === 0 && +it.cfg().outerLight === 0, 'No light turns the inner glow and the outer shine off');
  ok(v6.fields.some((f) => f.isChips && f.opts && /More \(\d+\)/.test(f.opts[0].label)), 'and the sliders wait under More');
  mood.opts.find((o) => o.label === 'Night').pick();
  ok(+it.cfg().glow === 1.4 && +it.cfg().outerTemp === 7000, 'a mood sets the light', it.cfg().glow + '/' + it.cfg().outerTemp);
  ok(V().fields.find((f) => f.label === 'Mood').opts.find((o) => o.label === 'Night').bd === '#58a6ff', 'and is marked as the mood in use');
  V().stepOpts[5].pick();
  const v7 = V();
  ok(stepOf(it) === 'render' && v7.isExport, 'Render shows the export');
  ok(/Fits the orb/.test(v7.badgeText) && /live rows/.test(v7.badgeNote), 'the badge says whether it fits, with the pace and the rows', v7.badgeText + ' · ' + v7.badgeNote);
  ok(/^Fits/.test(v7.fitShort) && v7.fitDot === '#3fb950', 'the top bar carries the same verdict in two words', v7.fitShort);
  // D43 P7: two files and nothing else, the baked theme and the design it carries
  ok(typeof v7.exportTheme === 'function'
    && !['exportDesign', 'designFileNote', 'exportKit', 'exportFrames', 'exportBundle', 'designId', 'toggleMoreExport', 'designShipOpts'].some((k) => k in v7), 'Render offers the .orbtheme and nothing else (K10: the theme is the one file that leaves the hub)');
  ok(!V().diagOpen && /Diagnostics/.test(V().diagLabel), 'the diagnostics drawer is closed by default');
  V().toggleDiag();
  ok(V().diagOpen, 'and opens on a press');
}

// ---- the hand panels: Shape, Paint, Motion, artwork folded --------------------
{
  const it = mount('skeleton'), V = () => it.renderVals();
  const ids = it.design().comps.filter((c) => c.type === 'hand').map((c) => c.id);
  const pan = (id) => { it.setState({ sel: id, selKind: 'hand', step: 'parts', tab: 'hands' }); return V().handRows; };
  ok(ids.length > 0 && ids.every((id) => { const r = pan(id); return r.length === 1 && r[0].anchor === 'hand-' + id && r[0].artOpen === false && /Artwork/.test(r[0].artToggleLabel); }), 'every hand\u2019s panel stands alone, its artwork upload folded', ids.length + ' hands');
  pan(ids[0])[0].toggleArt();
  ok(pan(ids[0])[0].artOpen === true && ids.slice(1).every((id) => !pan(id)[0].artOpen), 'opening one hand\u2019s artwork opens that hand\u2019s only');
  // owner, 2026-11-05: a hand's pane is the kit's rows, like every part's: its Name, then what it
  // shows, where it stands, how it moves, how it looks, and its artwork, each section folding
  const heads = () => V().fields.filter((f) => f.isHead).map((f) => f.label).join();
  ok(!V().handGrid && V().inspKind === 'Hand' && V().fields[0].label === 'Name' && heads() === 'Reading,Placement,Motion,Scale,Look,Artwork', 'a selected hand shows its own pane in the kit\u2019s rows: Name, Reading, Placement, Motion, Scale, Look, Artwork', heads());
  ok(V().fields.filter((f) => f.isHead).every((f) => f.fold) && V().fields.filter((f) => f.isHead && f.fold.open).length === 1, 'its sections fold, one open at a time');
  const look = V().fields.find((f) => f.isHead && f.label === 'Look'); look.fold.toggle();
  ok(V().fields.find((f) => f.isHead && f.label === 'Look').fold.open && !V().fields.find((f) => f.isHead && f.label === 'Reading').fold.open, 'opening Look closes the others');
  it.setState({ sel: ids[1] || ids[0] });
  ok(V().fields.find((f) => f.isHead && f.label === 'Look').fold.open, 'and the next hand opens where the last was left');
  // the style every hand starts from is the dial's: the Dial step's Hand style view
  it.setState({ sel: null, selKind: null, step: 'dial', tab: 'hands' });
  ok(V().handGrid && !V().handRows.length && V().fields.some((f) => f.label === 'Cast shadows'), 'the Dial step\u2019s Hand style shows the default-style grid and the shared settings, and no hand panels');
  ok(V().subOpts.map((t) => t.label).join() === 'Dial,Hand set,Timing', 'the Dial step offers Dial, Hand set and Timing', V().subOpts.map((t) => t.label).join());
  it.setState({ sel: null, selKind: null });
  const html = fs.readFileSync(path.join(__dirname, '..', 'Watch Composer.dc.html'), 'utf8');
  ok(html.indexOf('<sc-for list="{{ handRows }}"') < 0 && html.indexOf('class="wc-row') < 0, 'no hand-written hand pane is left: one kind of element, one kind of pane');
}

// ---- from a preset ------------------------------------------------------------
{
  const it = mount('skeleton'), V = () => it.renderVals();
  const card = V().dials.find((d) => /Perpetual/.test(d.name));
  card.pick();
  ok(it.state.dial === 'perpetual', 'picking a preset on Start loads it');
  V().stepOpts[2].pick();
  ok(V().stackRows.filter((r) => r.isRow && /^Sub-dial/.test(r.label)).length === 4, 'the layers panel shows the preset’s four sub-dials on any step');
  // a part picked while on the Dial step opens where its fields live
  it.setState({ sel: 'c1', selKind: 'comp', tab: 'comps' });
  ok(stepOf(it) === 'parts' && V().fields.some((f) => f.isHead && f.label === 'Placement'), 'selecting a complication shows its placement on Parts');
  // a selection that needs another step moves there
  it.setState({ tab: 'light' });
  ok(stepOf(it) === 'finish', 'a view the step does not show moves to the step that does');
}

// ---- nothing removed ----------------------------------------------------------
{
  const it = mount('calendar');
  const OLD = it.TABS.map((t) => t[0]);
  const ALL = { light: 1, place: 1, shape: 1, paint: 1 };
  const missing = [];
  const compare = (what) => {
    const old = labels(it.renderVals0());
    it.state.more = ALL;
    const st = it.STEPS.find((x) => x[0] !== 'start' && x[2].includes(it.state.tab));
    it.state.step = st[0];
    const now = labels(it.renderVals());
    old.forEach((l) => { if (!now.includes(l)) missing.push(what + ': ' + l); });
    it.state.more = {};
  };
  OLD.forEach((tab) => { it.state.tab = tab; it.state.sel = null; it.state.selKind = null; compare(tab); });
  // every complication type's own fields
  it.design().comps.forEach((c) => { it.state.tab = 'comps'; it.state.sel = c.id; it.state.selKind = 'comp'; compare('comps/' + c.type); });
  ok(missing.length === 0, 'every field every old tab offered is reachable from a step', missing.slice(0, 6).join(' | '));
  ok(OLD.every((t) => it.STEPS.some((x) => x[2].includes(t))), 'every old tab belongs to a step');

  // every binding the pre-P5 template used is still bound (two renamed on purpose)
  const PRE = 'addOpts artNote bundleNote calDown calMax calMin calStep calText calUp calVal caseOpts clearLabel clearOp clearSel compRows copyId cursor dateKitNote designId dials dupCancel dupNm dupNote dupOpen dupReplace dupStem dupStop exportBundle exportFrames exportKit exportTheme fields frameLabel glassOpts handOpts handRows hasSelHead hasTabReset hasZoom idInput isCase isCompare isComps isDial isExport isHands isHub isMove kitNote loadBundle loadDesign mainRef mech metaLabel next noSel onDown onIdInput onMove onUp paneRef pickGuide playLabel prev renderOpts resetCal resetDesign resetHands rulerPx rulerTicks seamText selTitle selectAll sendNote sendToOrb setCal setWipe showRuler slowLabel solve stackCap stackCapFg stackRows stackWarns stageCv stageDown stageFlow stageRing stageRing2 stageRound stageWin status tabReset tabResetLabel tabs themeNameIn themeNameLen themeNamePh themeNameVal themeNote timeOpts toggle toggleMech toggleSlow trainOpts viewOpts viewText wipeVal zipNote zoomOpts'.split(' ');
  // D43 P7 retired the layer kit, the flattened frames, the .wckit and the Design ID: the
  // .orbtheme is the one file the editor writes (K10 retired the design-file download too)
  const RETIRED = new Set(['bundleNote', 'copyId', 'dateKitNote', 'designId', 'exportBundle', 'exportFrames', 'exportKit', 'idInput', 'kitNote', 'loadBundle', 'loadDesign', 'onIdInput', 'selectAll', 'zipNote']);
  const RENAMED = { tabs: 'stepOpts', isDial: 'isStart',
    // P5b (§4.2): the layers panel arranges and the inspector removes, so the old
    // complication list (and its hidden add row) went; the stage box became the pane
    compRows: 'stackRows', addOpts: 'catOpts', hasSelHead: 'inspKind', selTitle: 'inspTitle',
    stageWin: 'stageCv', stageFlow: 'stageRef', stageRing: 'stageRound', stageRing2: 'stageRound',
    mech: 'mechCls', stackCap: 'stackCapShort', themeNameLen: 'themeNameTip',
    // owner, 2026-11-05: a hand's pane is the kit's rows, so it rides the inspector's fields
    handRows: 'studioFields', isHands: 'studioFields' };
  const html = fs.readFileSync(path.join(__dirname, '..', 'Watch Composer.dc.html'), 'utf8');
  const tpl = html.slice(html.indexOf('<x-dc>'), html.indexOf('</x-dc>'));
  const bound = new Set((tpl.match(/\{\{\s*([A-Za-z_][A-Za-z0-9_]*)/g) || []).map((m) => m.replace(/\{\{\s*/, '')));
  // K6: the chrome is the OS's shell (studio@1). A value the clock hands it through
  // studioVals (R.<name>) is still bound; the duplicate question and the true-size caption
  // and ruler the shell builds from the services' own answers, so they ride the studio
  // object that carries them, which the template must bind.
  const sv = html.slice(html.indexOf('  studioVals(R) {'), html.indexOf('\n  }\n', html.indexOf('  studioVals(R) {')));
  (sv.match(/\bR\.([A-Za-z_][A-Za-z0-9_]*)/g) || []).forEach((m) => bound.add(m.slice(2)));
  const SHELL = { dupOpen: 'studioDup', dupNm: 'studioDup', dupNote: 'studioDup', dupStem: 'studioDup', dupStop: 'studioDup',
    viewText: 'studioCal', showRuler: 'studioCal', rulerPx: 'studioCal', rulerTicks: 'studioCal', calText: 'studioCal' };
  Object.keys(SHELL).forEach((k) => { if (bound.has(SHELL[k])) bound.add(k); });
  const lost = PRE.filter((k) => !RETIRED.has(k) && !bound.has(RENAMED[k] || k));
  ok(lost.length === 0, 'every binding of the pre-P5 template is still in the page', lost.join(','));
  const v = it.renderVals();
  const unbound = [...bound].filter((k) => !['a', 'c', 'd', 'f', 'g', 'h', 'o', 'r', 't', 'w', 'true', 'false'].includes(k) && v[k] === undefined);
  ok(unbound.length === 0, 'every binding in the page has a value', unbound.join(','));
}

// ---- P5b: the studio's stage and chrome --------------------------------------
{
  const it = mount('skeleton'), V = () => it.renderVals();
  ok(it.state.view === 'fit' && /^Fit/.test(V().zoomTag) && V().stageCv === 466, 'the stage opens on Fit, sized by the pane (466 off-screen)', V().zoomTag + ' ' + V().stageCv);
  it.setState({ fitPx: 700 });
  ok(V().stageCv === 700 && /1\.50×/.test(V().zoomTag), 'Fit follows the measured pane', V().stageCv + ' ' + V().zoomTag);
  V().viewOpts.find((o) => o.label === 'Magnify').pick();
  ok(V().hasZoom && V().stageRound === '50%' && V().stageCv === 932, 'Magnify is a zoom stop: the glass stays round and the pane scrolls it', V().stageCv);
  // a colour's name sits under its swatches, never in a column its length resizes
  it.setState({ step: 'dial', tab: 'dial', sel: null, selKind: null });
  const rowsD = V().fields.filter((f) => f.isRow);
  ok(rowsD.some((f) => f.isColor || f.isSwatch) && rowsD.every((f) => (f.rowCls === 'wide') === !f.isRange), 'only sliders keep the value column; every other row reads its value underneath',
     rowsD.filter((f) => (f.rowCls === 'wide') === !!f.isRange).map((f) => f.label).join());
  ok(V().viewOpts.map((o) => o.label).join() === 'Fit,1:1,True size,Magnify', 'the four view stops', V().viewOpts.map((o) => o.label).join());
  ok(V().inFrame === false && V().notHub === true && /Clock/.test(V().crumbLabel), 'standalone: no back arrow, the download in the top bar', V().crumbLabel);
  const html = fs.readFileSync(path.join(__dirname, '..', 'Watch Composer.dc.html'), 'utf8');
  // K6: the shell's parts carry the clock's own values: the send modal says what the clock's
  // send lines say in every state, and the top bar holds the clock's steps and primary action
  [null, { canCancel: true, cancelling: false }, { canCancel: false, cancelling: false }, { canCancel: false, cancelling: true }].forEach((sd) => {
    it.setState({ sending: sd, status: sd ? 'Sending' : '' });
    const R = V();
    ok(R.studioSend.open === R.sendOpen && R.studioSend.hint === R.sendHint && R.studioSend.cancelLabel === R.sendCancelLabel && R.studioSend.noCancel === R.sendNoCancel && R.studioSend.line === R.sendLine,
       'the shell’s send modal and the clock’s send lines agree: ' + JSON.stringify(sd), R.studioSend.hint + ' | ' + R.sendHint);
  });
  it.setState({ sending: null, status: '' });
  { const R = V(); ok(R.studioTop.steps === R.stepOpts && R.studioTop.primary.label === 'Download .orbtheme' && R.studioGlass.size === R.stageCv && R.studioTool.view === R.viewOpts && R.studioRender.downloads.length === 1, 'the top bar, glass, toolbar and Render panel carry the clock’s own values'); }
  // K6: the one title bar is the shell's TopBar; the clock's page draws none of its own
  const studioJs = fs.readFileSync(require('./runtime-paths.js').STUDIO, 'utf8');
  ok(html.indexOf('Watch composer') < 0 && !(html.match(/<header/g) || []).length && (studioJs.match(/h\('header'/g) || []).length === 1 && html.includes('OrbStudio.TopBar'),
     'one title bar: the studio\u2019s own, no second heading');
  ok(/<link href="ui.css" rel="stylesheet">/.test(html), 'the look is the runtime\u2019s ui@1 sheet');
  ok(!/#c9a25a|#f0d79a|#2a2316/.test(html.slice(html.indexOf('<x-dc>'), html.indexOf('</x-dc>'))), 'no gold left in the studio\u2019s chrome');
}

// ---- a sub-dial prints nothing; the hand aligned to it prints in its own units ---------
{
  const it = mount('modern'), D = () => it.design();
  const sub = D().comps.find((c) => c.type === 'subdial'), hid = D().comps.find((c) => c.type === 'hand' && c.host === sub.id).id, hc = () => D().comps.find((c) => c.id === hid);
  const style0 = (hc().scale || {}).style;
  it.setComp(sub.id, { scale: 'date', tstyle: 'bold' });
  ok(!('scale' in D().comps.find((c) => c.id === sub.id)) && hc().shows === 'second', 'a face keeps no scale of its own, and its hand reads what it read', JSON.stringify(D().comps.find((c) => c.id === sub.id)));
  it.setHand(hid, { shows: 'date' });
  const hd = (it.face().hands || []).find((h) => h.id === hid);
  ok(!!style0 && hc().scale && hc().scale.style === style0 && hd && hd.kind === 'date' && hd.scale && hd.scale.style === style0,
     'the hand set to the date keeps its look, and its scale now counts the date', JSON.stringify(hc().scale));
}

// ---- the live price asks again by itself (a fresh Diver read 4 fps until its beat was touched) ----
(async () => {
  const it = mount('diver'), wait = (ms) => new Promise((r) => setTimeout(r, ms));
  it.settled3 = () => true;
  const walked = [];
  it.livePlan4 = (k) => { walked.push(k); return new Promise((r) => setTimeout(() => r(null), 400)); };
  it.liveSoon(); await wait(300);
  ok(walked.length === 1 && it._liveBusy, 'a price is walking');
  it.setCfg('tick', '1'); it.liveSoon();                       // the author moves on mid-walk
  await wait(2000);                                            // no update from the editor in between
  ok(walked.length >= 2 && walked[walked.length - 1] === it.live4Key(), 'a request made while a walk ran is priced when it ends, without another edit', walked.length);
  const n0 = walked.length;
  it.settled3 = () => false; it.setCfg('tick', 'sweep'); it.liveSoon(); await wait(500);
  ok(walked.length === n0, 'nothing is priced while the dial is still building');
  it.settled3 = () => true; await wait(2000);
  ok(walked.length > n0 && walked[walked.length - 1] === it.live4Key(), 'and the request is priced once it settles, without another edit', walked.length - n0);
  // ---- the crystal's rows open where they are set; a template after From scratch comes as designed ----
  {
    const it = mount('wave'), V = () => it.renderVals();
    const row = (id) => V().stackRows.find((r) => r.isRow && r.id === id);
    row('glass').pick();
    ok(it.state.step === 'finish' && it.state.tab === 'case' && it._scrollTo === 'case-glass', 'the Glass layer opens Case & glass at its Glass tiles', JSON.stringify([it.state.step, it.state.tab, it._scrollTo]));
    it.setState({ step: 'parts', tab: 'comps' }); row('outer').pick();
    ok(it.state.step === 'finish' && it.state.tab === 'case', 'the Case & outer shine layer opens Case & glass', JSON.stringify([it.state.step, it.state.tab]));
    it.setState({ step: 'start', tab: 'start' });
    V().dials.find((x) => /Wave/.test(x.name)).pick();
    const waveCase = it.state.caseStyle;
    V().fromScratch();
    ok(it.state.caseStyle === 'none' && !it.design().bezelInk, 'From scratch leaves no case behind from the template before it (Wave\u2019s bezel came in black)', it.state.caseStyle + ' after ' + waveCase);
    V().dials.find((x) => /Triple calendar/.test(x.name)).pick();
    ok(it.state.caseStyle === 'polished' && it.state.glassStyle === 'domed' && +it.cfg().glow > 0 && +it.cfg().outerLight > 0, 'and the next template comes with its own case, glass and light', JSON.stringify([it.state.caseStyle, it.state.glassStyle, it.cfg().glow, it.cfg().outerLight]));
  }
  console.log(fails ? 'RESULT: FAIL — ' + fails + ' of ' + checked + ' checks' : 'all green (' + checked + ' checks)');
  process.exit(fails ? 1 : 0);
})();
