#!/usr/bin/env node
// D43 P5: the build flow, walked through the real component — from scratch and from a
// preset, Start to Render — and the promise that the steps removed nothing: every field
// every old tab offered is still reachable from a step (with every More fold opened), and
// every binding the pre-P5 template used is still bound.
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
  ok(V().stepOpts.map((t) => t.label).join(' | ') === 'Start | Movement | Dial | Complications & hands | Arrange | Finish | Render', 'seven steps, in build order', V().stepOpts.map((t) => t.label).join(' | '));
  V().fromScratch();
  ok(stepOf(it) === 'movement' && (it.design().comps || []).length === 0, 'Build from scratch: a plain dial, no complications, on to the movement');
  ok(V().isMove, 'the Movement step shows the movement cards');
  V().stepOpts[2].pick();
  ok(stepOf(it) === 'dial' && labels(V()).length > 5, 'the Dial step shows the dial’s fields', labels(V()).slice(0, 4).join(','));
  V().stepOpts[3].pick();
  const v4 = V();
  ok(stepOf(it) === 'parts' && v4.isComps && v4.hasSubs && v4.subOpts.map((t) => t.label).join() === 'Complications,Hands', 'Complications & hands shows both views');
  ok(v4.catOpts.length === Object.keys(E.COMP_TYPES).length + 1 && v4.catOpts.every((c) => c.blurb), 'the catalogue offers every kind of complication and a hand, each with a line on what it is');
  v4.catOpts.find((c) => /Subdial/.test(c.label)).go();
  V().catOpts.find((c) => /Window/.test(c.label)).go();
  ok(it.design().comps.length === 2, 'adding from the catalogue adds to the design', it.design().comps.map((c) => c.type).join());
  // the selected complication's fields are grouped by part
  it.setState({ tab: 'comps', sel: it.design().comps[0].id, selKind: 'comp' });
  const heads = V().fields.filter((f) => f.isHead).map((f) => f.label);
  ok(heads[0] === 'Placement' && heads.includes('Shape'), 'a complication’s fields are grouped: Placement, Shape, Paint', heads.join(','));
  V().stepOpts[4].pick();
  const v5 = V();
  ok(stepOf(it) === 'arrange' && v5.isArrange && v5.stackRows.some((r) => r.isRow), 'Arrange shows the stack in the pane');
  ok(!v5.isComps, 'and not the add list (Arrange arranges)');
  V().stepOpts[5].pick();
  let v6 = V();
  ok(stepOf(it) === 'finish' && v6.subOpts.map((t) => t.label).join() === 'Light,Case & glass,Sound', 'Finish holds light, case & glass and sound');
  const mood = v6.fields.find((f) => f.label === 'Mood');
  ok(mood && mood.opts.length === 5 && /As designed/.test(mood.opts[0].label), 'Light opens on its moods');
  ok(v6.fields.some((f) => f.isChips && f.opts && /More \(\d+\)/.test(f.opts[0].label)), 'and the sliders wait under More');
  mood.opts.find((o) => o.label === 'Night').pick();
  ok(+it.cfg().glow === 1.4 && +it.cfg().outerTemp === 7000, 'a mood sets the light', it.cfg().glow + '/' + it.cfg().outerTemp);
  ok(V().fields.find((f) => f.label === 'Mood').opts.find((o) => o.label === 'Night').bd === '#c9a25a', 'and is marked as the mood in use');
  V().stepOpts[6].pick();
  const v7 = V();
  ok(stepOf(it) === 'render' && v7.isExport, 'Render shows the export');
  ok(/Fits the orb/.test(v7.badgeText) && /live rows/.test(v7.badgeNote), 'the badge says whether it fits, with the pace and the rows', v7.badgeText + ' · ' + v7.badgeNote);
  ok(!v7.moreExportOpen && /More export options/.test(v7.moreExportLabel), 'the other exports wait under More export options');
  v7.toggleMoreExport();
  ok(V().moreExportOpen, 'and open on a press');
  ok(!V().diagOpen && /Diagnostics/.test(V().diagLabel), 'the diagnostics drawer is closed by default');
  V().toggleDiag();
  ok(V().diagOpen, 'and opens on a press');
}

// ---- from a preset ------------------------------------------------------------
{
  const it = mount('skeleton'), V = () => it.renderVals();
  const card = V().dials.find((d) => /Perpetual/.test(d.name));
  card.pick();
  ok(it.state.dial === 'perpetual', 'picking a preset on Start loads it');
  V().stepOpts[4].pick();
  ok(V().stackRows.filter((r) => r.isRow && /^Sub-dial/.test(r.label)).length === 4, 'Arrange shows the preset’s four sub-dials');
  // a selection on the stage while in Arrange stays in Arrange
  it.setState({ sel: 'c1', selKind: 'comp', tab: 'comps' });
  ok(stepOf(it) === 'arrange' && V().fields.some((f) => f.isHead && f.label === 'Placement'), 'selecting a complication in Arrange keeps Arrange and shows its placement');
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
  const RENAMED = { tabs: 'stepOpts', isDial: 'isStart' };
  const html = fs.readFileSync(path.join(__dirname, '..', 'Watch Composer.dc.html'), 'utf8');
  const tpl = html.slice(html.indexOf('<x-dc>'), html.indexOf('</x-dc>'));
  const bound = new Set((tpl.match(/\{\{\s*([A-Za-z_][A-Za-z0-9_]*)/g) || []).map((m) => m.replace(/\{\{\s*/, '')));
  const lost = PRE.filter((k) => !bound.has(RENAMED[k] || k));
  ok(lost.length === 0, 'every binding of the pre-P5 template is still in the page', lost.join(','));
  const v = it.renderVals();
  const unbound = [...bound].filter((k) => !['a', 'c', 'd', 'f', 'g', 'h', 'o', 'r', 't', 'w', 'true', 'false'].includes(k) && v[k] === undefined);
  ok(unbound.length === 0, 'every binding in the page has a value', unbound.join(','));
}

console.log(fails ? 'RESULT: FAIL — ' + fails + ' of ' + checked + ' checks' : 'all green (' + checked + ' checks)');
process.exit(fails ? 1 : 0);
