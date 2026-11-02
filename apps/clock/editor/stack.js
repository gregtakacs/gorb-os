/* stack.js — the composer's layer stack (D43 §2, §3), as data.
 *
 * A watch is one stack between a pinned crystal (the glass, the case) and a pinned
 * plate (the movement, the geartrain). Between them every visible layer of the design
 * has one place: the hands and the sub-dial pointers, each sub-dial face with the
 * discs it frames, the dial with what is printed on it and cut through it, and the
 * discs the dial frames. This file builds that list from a design and moves a layer
 * through it under the stacking rules; it never draws and never touches a canvas, so
 * the node suites hold every rule directly (_selftest/headless-stack.test.js).
 *
 * The stack is a VIEW of the design's own fields, not a second copy of them: depth among
 * the hands is design.order (bottom to top, the order WatchEngine draws and the bake
 * ships as depthOrder), and depth among sub-dials, and among the discs one host frames,
 * is their order in design.comps. move() answers with the new values of those fields,
 * so a preset's stack bakes exactly what the preset baked before the stack existed.
 *
 * Rules (D43 §3). R1-R7 refuse a move, by name; R8 only warns, and only when the design
 * carries the Mechanical switch (ruling 1):
 *   R1  nothing above the crystal or below the plate
 *   R2  a shadow is its hand's, never a layer of its own
 *   R3  a child stays above its host's face (a sub-dial pointer over its sub-dial, a
 *       sub-dial over the dial)
 *   R4  a disc stays below the face that frames it
 *   R5  only still layers fold into the plate (the badge each row carries)
 *   R6  at most 24 live rows (rowCount)
 *   R7  the dial is the face the rest is placed against; it does not move
 *   R8  coaxial order and arbors swept through: warnings
 *
 * D43 P4b: a sub-dial face may be LIFTED among the hands. It then ships as a live still
 * row at its depth instead of folding into the plate (R5's promotion), and the hands
 * below it pass under it. design.order carries the live band: hand ids and
 * 'comp:<id>' for a lifted face, bottom first; the engine's liveSeq reads the same. */
(function (root) {
  'use strict';

  var ROW_CAP = 24;
  // Turns per day, for coaxial order: a slower hand belongs lower on a shared arbor.
  var SPEED = { second: 1440, minute: 24, hour: 2, hours24: 1, gmt: 1, day: 1 / 7, date: 1 / 30.4, month: 1 / 365 };
  var HAND_NAME = { hour: 'Hour hand', minute: 'Minute hand', second: 'Seconds hand',
                    hours24: '24-hour hand', gmt: 'GMT hand', day: 'Day hand', date: 'Date hand', month: 'Month hand' };

  function compTitle(c, E) {
    if (c.type === 'subdial') return 'Sub-dial' + (c.scale && c.scale !== 'none' && E && E.SCALES && E.SCALES[c.scale] ? ' · ' + E.SCALES[c.scale] : '');
    if (c.type === 'window') return ((E && E.SHOWS && E.SHOWS[c.shows]) || 'Date') + ' window';
    if (c.type === 'moon') return 'Moon phase';
    if (c.type === 'flare') return 'Flare' + (E && E.FLARE_ICONS && E.FLARE_ICONS[c.icon] ? ' · ' + E.FLARE_ICONS[c.icon] : '');
    if (c.show === 'balance') return 'Escapement opening';
    return 'Opening';
  }

  /* input: {
   *   comps:   design.comps (in order),
   *   hands:   [{ id, kind, sub, host, x, y, len }] the hands drawn, bottom first (WatchEngine's F.hands),
 *   order:   the full live band, bottom first: WatchEngine's handOrder with any lifted
 *            sub-dial as 'comp:<id>' in its place (design.order). It can name hands that
 *            are not drawn (a seconds hand the carriage replaces, a hand switched off),
 *            and a move keeps those in their slots so the bake's depth numbers do not shift,
   *   shadows: whether hands cast shadow twins,
   *   carriage: whether the movement has an animated carriage row,
   *   E:       WatchEngine, for names (optional)
   * } */
  function build(input) {
    var comps = input.comps || [], hands = input.hands || [], E = input.E;
    var byId = {};
    comps.forEach(function (c) { byId[c.id] = c; });
    var subs = comps.filter(function (c) { return c.type === 'subdial'; });
    var isDisc = function (c) { return c.type === 'window' || c.type === 'moon'; };
    var hostOf = function (c) { return c.host && c.host !== 'dial' && byId[c.host] ? c.host : (c.host === 'plate' ? 'plate' : 'dial'); };
    var items = [];
    var push = function (it) { items.push(it); return it; };

    push({ id: 'outer', kind: 'case', label: 'Case & outer shine', band: 'crystal', hide: 'outer', badge: 'crystal overlay', movable: false });
    push({ id: 'glass', kind: 'glass', label: 'Glass', band: 'crystal', hide: 'glass', badge: 'crystal overlay', movable: false });

    var order = (input.order || hands.map(function (h) { return h.id; })).map(String);
    var drawnHand = {}; hands.forEach(function (h) { drawnHand[h.id] = h; });
    var lifted = {};
    order.forEach(function (e) { if (e.indexOf('comp:') === 0 && byId[e.slice(5)] && byId[e.slice(5)].type === 'subdial') lifted[e.slice(5)] = 1; });
    var subBlock = function (sb, live) {
      push({ id: 'comp:' + sb.id, ref: sb.id, kind: 'subdial', group: live ? 'live' : 'subdials', band: 'rows', label: compTitle(sb, E),
             host: 'dial', hide: 'subdials', sel: { kind: 'comp', id: sb.id }, lifted: !!live,
             badge: live ? 'live · still row' : (sb.transparent ? 'baked in · open' : 'baked in'), movable: true });
      comps.forEach(function (c) {
        if (c.type === 'flare' && hostOf(c) === sb.id)
          push({ id: 'comp:' + c.id, ref: c.id, kind: 'flare', band: 'rows', label: compTitle(c, E), parent: 'comp:' + sb.id,
                 host: 'comp:' + sb.id, hide: 'subdials', sel: { kind: 'comp', id: c.id }, badge: 'printed on it', movable: false });
      });
      comps.forEach(function (c) {
        if (isDisc(c) && hostOf(c) === sb.id)
          push({ id: 'comp:' + c.id, ref: c.id, kind: 'disc', group: 'discs:' + sb.id, band: 'rows', label: compTitle(c, E),
                 parent: 'comp:' + sb.id, host: 'comp:' + sb.id, hide: 'complications', sel: { kind: 'comp', id: c.id },
                 badge: 'live · value plate', movable: true });
      });
    };
    // the live band, top of the stack first (the engine draws it bottom first): the
    // hands and any sub-dial face lifted among them
    for (var i = order.length - 1; i >= 0; i--) {
      var e = order[i];
      if (e.indexOf('comp:') === 0) { if (lifted[e.slice(5)]) subBlock(byId[e.slice(5)], true); continue; }
      var h = drawnHand[e]; if (!h) continue;
      var host = h.sub ? (h.host && h.host !== 'dial' ? h.host : h.id) : null;   // D43 P5c: a hand names its host
      push({ id: 'hand:' + h.id, ref: h.id, kind: h.sub ? 'pointer' : 'hand', group: 'live', band: 'rows',
             label: h.sub ? ((HAND_NAME[h.kind] || 'Hand') + ' · on ' + compTitle(byId[host] || { type: 'subdial' }, E))
                          : (HAND_NAME[h.kind] || 'Hand'),
             handKind: h.kind, host: host ? 'comp:' + host : 'dial', shadow: !!input.shadows,
             hide: 'hands', sel: { kind: 'hand', id: h.id }, badge: 'live', movable: true,
             pivot: [h.x, h.y], reach: h.len });
    }
    // the sub-dial faces still on the dial, top first, each with what it prints and frames
    for (var s2 = subs.length - 1; s2 >= 0; s2--) if (!lifted[subs[s2].id]) subBlock(subs[s2], false);
    // the dial: what is printed on it and cut through it rides it
    push({ id: 'dial', kind: 'dial', band: 'rows', label: 'Dial', hide: 'dial', sel: { kind: 'dial', id: 'dial' },
           badge: 'baked in', movable: false });
    comps.forEach(function (c) {
      if (c.type === 'flare' && hostOf(c) === 'dial')
        push({ id: 'comp:' + c.id, ref: c.id, kind: 'flare', band: 'rows', label: compTitle(c, E), parent: 'dial', host: 'dial',
               hide: 'dial', sel: { kind: 'comp', id: c.id }, badge: 'printed on it', movable: false });
      if (c.type === 'opening')
        push({ id: 'comp:' + c.id, ref: c.id, kind: 'opening', band: 'rows', label: compTitle(c, E), parent: 'dial', host: 'dial',
               hide: 'dial', sel: { kind: 'comp', id: c.id }, badge: 'cut through it', movable: false });
    });
    // the discs the dial frames, top first
    var dDiscs = comps.filter(function (c) { return isDisc(c) && hostOf(c) === 'dial'; });
    for (var k = dDiscs.length - 1; k >= 0; k--) {
      var dc = dDiscs[k];
      push({ id: 'comp:' + dc.id, ref: dc.id, kind: 'disc', group: 'discs:dial', band: 'rows', label: compTitle(dc, E) + ' · under the dial',
             host: 'dial', hide: 'complications', sel: { kind: 'comp', id: dc.id }, badge: 'live · value plate', movable: true });
    }
    push({ id: 'geartrain', kind: 'geartrain', band: 'plate', label: input.carriage ? 'Geartrain & carriage' : 'Geartrain',
           hide: 'geartrain', sel: { kind: 'move', id: 'geartrain' }, badge: input.carriage ? 'plate bank · carriage live' : 'plate bank', movable: false });
    push({ id: 'movement', kind: 'movement', band: 'plate', label: 'Clock insides', hide: 'movement', sel: { kind: 'move', id: 'movement' }, badge: 'plate', movable: false });
    comps.forEach(function (c) {
      if (c.type === 'flare' && hostOf(c) === 'plate')
        push({ id: 'comp:' + c.id, ref: c.id, kind: 'flare', band: 'plate', label: compTitle(c, E), parent: 'movement', host: 'movement',
               hide: 'movement', sel: { kind: 'comp', id: c.id }, badge: 'printed on the plate', movable: false });
    });
    return items;
  }

  var find = function (items, id) { for (var i = 0; i < items.length; i++) if (items[i].id === id) return i; return -1; };

  /* Why the move of `it` across `nb` (the layer it would pass, toward `dir`: -1 up,
   * +1 down) is refused, or null if the two share a group and may trade places. */
  function refusal(it, nb, dir) {
    if (!it.movable) {
      if (it.kind === 'dial') return 'R7: the dial is the face every other layer is placed against, so it does not move';
      if (it.kind === 'flare' || it.kind === 'opening') return 'R3: ' + it.label + ' is part of ' + (it.parent === 'dial' ? 'the dial' : 'its host') + ' — move the host instead';
      return 'R1: the crystal and the plate are pinned';
    }
    if (!nb) return dir < 0 ? 'R1: nothing goes above the glass and the case' : 'R1: nothing goes below the movement plate';
    if (nb.band === 'crystal') return 'R1: nothing goes above the glass and the case';
    if (nb.band === 'plate') return 'R1: nothing goes below the movement plate';
    if (nb.group && nb.group === it.group) return null;
    // leaving its group: name the rule the neighbour stands for
    // the live band and the sub-dial faces trade through lifting (move() handles it)
    if (it.group === 'live' && dir > 0 && nb.kind === 'subdial') return null;
    if (it.group === 'subdials' && dir < 0 && nb.group === 'live') return null;
    if (it.kind === 'subdial' && it.lifted && dir > 0) return null;
    if ((it.kind === 'hand' || it.kind === 'pointer') && dir > 0) return 'R3: a hand turns above the dial, so it stays above the dial’s face';
    if (it.kind === 'subdial' && dir > 0) return 'R3: a sub-dial sits on the dial, so it stays above the dial’s face';
    if (it.kind === 'disc' && dir < 0) return 'R4: a disc stays under the face that frames it' + (it.host === 'dial' ? ' (the dial)' : '');
    if (it.kind === 'disc' && dir > 0) return 'R4: a disc stays with the face that frames it';
    return 'R3: ' + it.label + ' stays with its host';
  }

  /* One step of `id` up (dir -1) or down (+1). Returns { ok, reason } or
   * { ok: true, order?: design.order, comps?: design.comps } — the new field values. */
  function move(input, id, dir) {
    var items = build(input), i = find(items, id);
    if (i < 0) return { ok: false, reason: 'no layer ' + id };
    var it = items[i];
    if (it.kind === 'shadow') return { ok: false, reason: 'R2: a shadow is its hand’s — move the hand' };
    // the neighbour it would pass: a child layer outside the mover's own group (what is
    // printed on a sub-dial, the discs it frames — the mover's own or a neighbour's)
    // travels with its host, so the move passes over it to the next layer that counts
    var j = i + dir;
    while (j >= 0 && j < items.length && items[j].parent && items[j].group !== it.group) j += dir;
    var nb = items[j] || null, why = refusal(it, nb, dir);
    if (why) return { ok: false, reason: why };
    var full = (input.order || (input.hands || []).map(function (h) { return h.id; })).map(String);
    var key = function (x) { return x.kind === 'subdial' ? 'comp:' + x.ref : x.ref; };
    if (it.group === 'live' && nb.group === 'live') {
      var a = full.indexOf(key(it)), b = full.indexOf(key(nb));
      if (a < 0 || b < 0) return { ok: false, reason: 'a layer missing from the order' };
      full[a] = key(nb); full[b] = key(it);            // the two trade slots; hidden hands keep theirs
      return r3(full, input) || { ok: true, order: full };
    }
    if (it.group === 'live' && it.kind === 'subdial' && dir > 0) {
      // a lifted face stepping below the live band settles back on the dial
      return { ok: true, order: full.filter(function (x) { return x !== key(it); }) };
    }
    if (it.group === 'live' && dir > 0 && nb.kind === 'subdial') {
      // the lowest hand steps under a face: the face is lifted just above it
      full.splice(full.indexOf(key(it)) + 1, 0, 'comp:' + nb.ref);
      return r3(full, input) || { ok: true, order: full, lift: nb.ref };
    }
    if (it.group === 'subdials' && dir < 0 && nb.group === 'live') {
      // the top face on the dial steps up among the hands: lifted just above the lowest
      full.splice(full.indexOf(key(nb)) + 1, 0, 'comp:' + it.ref);
      return r3(full, input) || { ok: true, order: full, lift: it.ref };
    }
    // sub-dials and discs: trade the two comps' places in design.comps
    var comps = (input.comps || []).slice();
    var ca = -1, cb = -1;
    comps.forEach(function (c, n) { if (c.id === it.ref) ca = n; if (c.id === nb.ref) cb = n; });
    if (ca < 0 || cb < 0) return { ok: false, reason: 'no comp to trade with' };
    var tc = comps[ca]; comps[ca] = comps[cb]; comps[cb] = tc;
    return { ok: true, comps: comps };
  }

  /* R3 over the live band: a lifted face's own pointer must stay above it. */
  function r3(full, input) {
    // D43 P5c: a sub-dial's own pointers are the hands it hosts, whatever their ids
    var own = {}; (input.hands || []).forEach(function (h) { if (h.sub && h.host) (own[h.host] = own[h.host] || []).push(h.id); });
    for (var i = 0; i < full.length; i++) {
      var e = full[i];
      if (e.indexOf('comp:') !== 0) continue;
      var ptrs = own[e.slice(5)] || [];
      for (var k = 0; k < ptrs.length; k++) {
        var p = full.indexOf(ptrs[k]);
        if (p >= 0 && p < i) return { ok: false, reason: 'R3: a sub-dial\u2019s own pointer rides it, so the face stays under that pointer' };
      }
    }
    return null;
  }

  /* Move `id` to sit where `targetId` sits (drag and drop), step by step, stopping at
   * the first refusal so the reason is the one that bites. */
  function moveTo(input, id, targetId) {
    var items = build(input), from = find(items, id), to = find(items, targetId);
    if (from < 0 || to < 0) return { ok: false, reason: 'no such layer' };
    if (from === to) return { ok: true };
    var dir = to < from ? -1 : 1, cur = Object.assign({}, input), out = null;
    for (var guard = 0; guard < items.length; guard++) {
      var r = move(cur, id, dir);
      if (!r.ok) return out ? Object.assign({}, out, { partial: r.reason }) : r;
      out = r;
      if (r.order) cur = Object.assign({}, cur, { order: r.order, hands: reorderHands(cur.hands, r.order) });
      if (r.comps) cur = Object.assign({}, cur, { comps: r.comps });
      var now = build(cur);
      if (find(now, id) === find(now, targetId) || (dir < 0 ? find(now, id) <= to : find(now, id) >= to)) return out;
    }
    return out || { ok: false, reason: 'no move' };
  }
  function reorderHands(hands, order) {
    var by = {}; hands.forEach(function (h) { by[h.id] = h; });
    return order.map(function (id) { return by[id]; }).filter(Boolean);
  }

  /* R8: the mechanical warnings, only for a design with the Mechanical switch on. A
   * warning never blocks a move or a bake (D43 ruling 1). */
  function warnings(input) {
    var hands = input.hands || [], out = [];
    for (var lo = 0; lo < hands.length; lo++) {
      for (var hi = lo + 1; hi < hands.length; hi++) {
        var a = hands[lo], b = hands[hi];          // a is drawn below b
        var same = Math.abs(a.x - b.x) < 1 && Math.abs(a.y - b.y) < 1;
        if (same && (SPEED[a.kind] || 0) > (SPEED[b.kind] || 0))
          out.push({ rule: 'R8', id: 'hand:' + b.id, text: 'Coaxial order: the ' + (HAND_NAME[b.kind] || b.kind).toLowerCase() + ' sits above the faster ' +
                     (HAND_NAME[a.kind] || a.kind).toLowerCase() + ' on one arbor; a tube-in-tube movement stacks the slower hand lower' });
        if (!same && a.len && Math.hypot(a.x - b.x, a.y - b.y) < a.len)
          out.push({ rule: 'R8', id: 'hand:' + a.id, text: 'Through an arbor: the ' + (HAND_NAME[a.kind] || a.kind).toLowerCase() + ' sweeps through the arbor of the ' +
                     (HAND_NAME[b.kind] || b.kind).toLowerCase() + ' stacked above it' });
      }
    }
    return out;
  }

  /* R6: the live rows the bake will ship — hands with their shadow twins, the discs'
   * value rows, the carriage — against the dial's cap. */
  function rowCount(input) {
    var n = 0;
    (input.hands || []).forEach(function () { n += input.shadows ? 2 : 1; });
    (input.comps || []).forEach(function (c) { if (c.type === 'window' || c.type === 'moon') n += 1; });
    if (input.carriage) n += 1;
    // a lifted sub-dial face ships as one still row (D43 P4b)
    (input.order || []).forEach(function (e) { if (String(e).indexOf('comp:') === 0) n += 1; });
    return { rows: n, cap: ROW_CAP, over: n > ROW_CAP };
  }

  var api = { build: build, move: move, moveTo: moveTo, warnings: warnings, rowCount: rowCount, ROW_CAP: ROW_CAP };
  root.OrbStack = api;
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
})(typeof window !== 'undefined' ? window : globalThis);
