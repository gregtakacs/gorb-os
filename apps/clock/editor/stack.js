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
 * below it pass under it. design.order carries the live band: the dial's own hand ids
 * and 'comp:<id>' for a lifted face, bottom first; the engine's liveSeq reads the same.
 *
 * D43 P5d: a face is a group. A sub-dial's hands are its children, like its windows:
 * they turn above its face and travel with it, lifted or not, and their order among
 * themselves is their order in design.comps. place() is the drag: it puts a layer above
 * or below any other and answers the design fields that says, or the rule that refuses
 * it -- a hand dropped into another face's group is re-hosted there. */
(function (root) {
  'use strict';

  var ROW_CAP = 24;
  // Turns per day, for coaxial order: a slower hand belongs lower on a shared arbor.
  var SPEED = { second: 1440, minute: 24, hour: 2, hours24: 1, gmt: 1, day: 1 / 7, date: 1 / 30.4, month: 1 / 365 };
  var HAND_NAME = { hour: 'Hour hand', minute: 'Minute hand', second: 'Seconds hand',
                    hours24: '24-hour hand', gmt: 'GMT hand', day: 'Day hand', date: 'Date hand', month: 'Month hand' };

  function compTitle(c, E) {
    if (c.type === 'wheel') return ((E && E.WHEEL_SHOWS && E.WHEEL_SHOWS[c.shows]) || 'Date') + (c.digit === 'tens' ? ' tens' : c.digit === 'units' ? ' units' : '') + ' wheel';
    if (c.type === 'cutout') return 'Cutout' + (c.shape && c.shape !== 'window' ? ' · ' + c.shape : '');
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
    // D43 P5d.3: a wheel is a disc under its face; its cutouts are its children
    var isDisc = function (c) { return c.type === 'window' || c.type === 'moon' || c.type === 'wheel'; };
    var cutoutsOf = function (w, hostKey) {
      comps.forEach(function (c) {
        if (c.type === 'cutout' && c.host === w.id)
          push({ id: 'comp:' + c.id, ref: c.id, kind: 'cutout', band: 'rows', label: compTitle(c, E), parent: 'comp:' + w.id, host: hostKey,
                 hide: 'complications', sel: { kind: 'comp', id: c.id }, badge: 'cut through ' + (hostKey === 'dial' ? 'the dial' : 'its face'), movable: true, group: 'cuts:' + w.id });
      });
    };
    var hostOf = function (c) { return c.host && c.host !== 'dial' && byId[c.host] ? c.host : (c.host === 'plate' ? 'plate' : 'dial'); };
    var items = [];
    var push = function (it) { items.push(it); return it; };

    push({ id: 'outer', kind: 'case', label: 'Case & outer shine', band: 'crystal', hide: 'outer', badge: 'crystal overlay', movable: false });
    push({ id: 'glass', kind: 'glass', label: 'Glass', band: 'crystal', hide: 'glass', badge: 'crystal overlay', movable: false });

    var order = liveKeys(input);
    var drawnHand = {}; hands.forEach(function (h) { drawnHand[h.id] = h; });
    var lifted = {};
    order.forEach(function (e) { if (e.indexOf('comp:') === 0 && byId[e.slice(5)] && byId[e.slice(5)].type === 'subdial') lifted[e.slice(5)] = 1; });
    var subBlock = function (sb, live) {
      push({ id: 'comp:' + sb.id, ref: sb.id, kind: 'subdial', group: live ? 'live' : 'subdials', band: 'rows', label: compTitle(sb, E),
             host: 'dial', hide: 'subdials', sel: { kind: 'comp', id: sb.id }, lifted: !!live,
             badge: live ? 'live · still row' : (sb.transparent ? 'baked in · open' : 'baked in'), movable: true });
      // its hands, top first: they turn above its face and ride it wherever it goes
      for (var q = hands.length - 1; q >= 0; q--) {
        var ph = hands[q];
        if (!ph.sub || ph.host !== sb.id) continue;
        push({ id: 'hand:' + ph.id, ref: ph.id, kind: 'pointer', group: 'ptr:' + sb.id, band: 'rows', label: HAND_NAME[ph.kind] || 'Hand',
               parent: 'comp:' + sb.id, handKind: ph.kind, host: 'comp:' + sb.id, shadow: !!input.shadows,
               hide: 'hands', sel: { kind: 'hand', id: ph.id }, badge: 'live', movable: true, pivot: [ph.x, ph.y], reach: ph.len });
      }
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
        if (c.type === 'wheel' && hostOf(c) === sb.id) cutoutsOf(c, 'comp:' + sb.id);
      });
    };
    // the live band, top of the stack first (the engine draws it bottom first): the
    // hands and any sub-dial face lifted among them
    for (var i = order.length - 1; i >= 0; i--) {
      var e = order[i];
      if (e.indexOf('comp:') === 0) { if (lifted[e.slice(5)]) subBlock(byId[e.slice(5)], true); continue; }
      var h = drawnHand[e]; if (!h || h.sub) continue;
      push({ id: 'hand:' + h.id, ref: h.id, kind: 'hand', group: 'live', band: 'rows', label: HAND_NAME[h.kind] || 'Hand',
             handKind: h.kind, host: 'dial', shadow: !!input.shadows,
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
      if (dc.type === 'wheel') cutoutsOf(dc, 'dial');
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
    if (it.kind === 'pointer') return 'R3: a sub-dial\u2019s hand rides its face \u2014 drag it onto another face to move it there';
    if (it.kind === 'hand' && dir > 0) return 'R3: a hand turns above the dial, so it stays above the dial’s face';
    if (it.kind === 'subdial' && dir > 0) return 'R3: a sub-dial sits on the dial, so it stays above the dial’s face';
    if (it.kind === 'cutout') return 'R4: a cutout reveals its wheel \u2014 drag it onto another wheel to read that one';
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
    var full = liveKeys(input);
    var key = function (x) { return x.kind === 'subdial' ? 'comp:' + x.ref : x.ref; };
    if (it.group === 'live' && nb.group === 'live') {
      var a = full.indexOf(key(it)), b = full.indexOf(key(nb));
      if (a < 0 || b < 0) return { ok: false, reason: 'a layer missing from the order' };
      full[a] = key(nb); full[b] = key(it);            // the two trade slots
      return { ok: true, order: full };
    }
    if (it.group === 'live' && it.kind === 'subdial' && dir > 0) {
      // a lifted face stepping below the live band settles back on the dial
      return { ok: true, order: full.filter(function (x) { return x !== key(it); }) };
    }
    if (it.group === 'live' && dir > 0 && nb.kind === 'subdial') {
      // the lowest hand steps under a face: the face is lifted just above it
      full.splice(full.indexOf(key(it)) + 1, 0, 'comp:' + nb.ref);
      return { ok: true, order: full, lift: nb.ref };
    }
    if (it.group === 'subdials' && dir < 0 && nb.group === 'live') {
      // the top face on the dial steps up among the hands: lifted just above the lowest
      full.splice(full.indexOf(key(nb)) + 1, 0, 'comp:' + it.ref);
      return { ok: true, order: full, lift: it.ref };
    }
    // sub-dials and discs: trade the two comps' places in design.comps
    var comps = (input.comps || []).slice();
    var ca = -1, cb = -1;
    comps.forEach(function (c, n) { if (c.id === it.ref) ca = n; if (c.id === nb.ref) cb = n; });
    if (ca < 0 || cb < 0) return { ok: false, reason: 'no comp to trade with' };
    var tc = comps[ca]; comps[ca] = comps[cb]; comps[cb] = tc;
    return { ok: true, comps: comps };
  }

  /* The live band, bottom first: the dial's own hands and 'comp:<id>' for each lifted
   * face. A sub-dial's hands are not in it: they ride their face. */
  function liveKeys(input) {
    var sub = {}; (input.hands || []).forEach(function (h) { if (h.sub) sub[h.id] = 1; });
    var subs = {}; (input.comps || []).forEach(function (c) { if (c.type === 'subdial') subs[c.id] = 1; });
    var out = [], seen = {};
    (input.order || (input.hands || []).map(function (h) { return h.id; })).forEach(function (e) {
      e = String(e);
      if (seen[e]) return;
      if (e.indexOf('comp:') === 0 ? subs[e.slice(5)] : !sub[e]) { out.push(e); seen[e] = 1; }
    });
    (input.hands || []).forEach(function (h) { if (!h.sub && !seen[h.id]) { out.push(h.id); seen[h.id] = 1; } });
    return out;
  }

  /* D43 P5d: the drag. Put `id` directly above or below `targetId` as the panel shows
   * them (top first), and answer the design fields that say so -- { ok, order, comps } --
   * or { ok: false, reason } naming the rule. The panel asks this for every row the
   * drag passes over, and opens a gap only where it answers ok.
   *   a hand: anywhere above the dial. Into a sub-dial's group it is re-hosted there; onto
   *     the dial's own band it turns on the dial. Dropped below a face left on the dial,
   *     every face displayed above the drop is lifted over it (P4b's lift, generalized).
   *   a sub-dial: among the hands (lifted there) or among the faces on the dial.
   *   a disc: among the discs its own face frames (R4).
   * The crystal and the plate are pinned (R1), and nothing turns under the dial (R3). */
  function place(input, id, targetId, where) {
    var items = build(input), i = find(items, id), t = find(items, targetId);
    if (i < 0 || t < 0) return { ok: false, reason: 'no such layer' };
    if (i === t) return { ok: true, same: true };
    var it = items[i], tg = items[t], above = where !== 'below';
    if (!it.movable) return { ok: false, reason: refusal(it, null, 1) };
    if (tg.band === 'crystal' && !(tg.id === 'glass' && !above)) return { ok: false, reason: 'R1: nothing goes above the glass and the case' };
    if (tg.band === 'crystal') { tg = items[t + 1]; above = true; }        // just under the glass is the top of the stack
    if (tg.band === 'plate') return { ok: false, reason: 'R1: nothing goes below the movement plate' };
    var live = liveKeys(input), comps = (input.comps || []).map(function (c) { return Object.assign({}, c); });
    var byId = {}; comps.forEach(function (c) { byId[c.id] = c; });
    var faceOf = function (x) { return x.kind === 'subdial' ? x.ref : (x.parent && x.parent.indexOf('comp:') === 0 ? x.parent.slice(5) : null); };
    var lifted = function (sid) { return live.indexOf('comp:' + sid) >= 0; };
    var onDial = function (x) { return x.kind === 'dial' || x.parent === 'dial' || x.group === 'discs:dial'; };
    var face = faceOf(tg), tLive = tg.group === 'live' || (face && lifted(face));
    var keyOf = function (x) { return x.kind === 'subdial' ? 'comp:' + x.ref : x.ref; };
    // the unlifted faces displayed above a target on the dial side, top first
    var facesAbove = function (stop) { var out = []; for (var k = 0; k < stop; k++) if (items[k].kind === 'subdial' && !lifted(items[k].ref)) out.push(items[k].ref); return out; };
    var moveComp = function (cid, refId, after) {
      var c = comps.filter(function (x) { return x.id === cid; })[0];
      comps = comps.filter(function (x) { return x.id !== cid; });
      var at = -1; comps.forEach(function (x, n) { if (x.id === refId) at = n; });
      comps.splice(at < 0 ? comps.length : at + (after ? 1 : 0), 0, c);
    };
    var tIdx = function (x) { return x ? live.indexOf(keyOf(x)) : -1; };

    if (it.kind === 'hand' || it.kind === 'pointer') {
      var hid = it.ref, hc = byId[hid];
      if (!hc) return { ok: false, reason: 'no hand ' + hid };
      live = live.filter(function (e) { return e !== hid; });
      // into a face's group: the hand is re-hosted on that sub-dial
      var into = face && (tg.kind === 'pointer' || tg.kind === 'flare' || (tg.kind === 'subdial' && !above)) ? face : null;
      if (into) {
        if (byId[into].type !== 'subdial') return { ok: false, reason: 'R3: a hand turns on the dial or on a sub-dial' };
        hc.host = into;
        // display is top first and design.comps bottom first: above a sibling is after it
        var sibs = comps.filter(function (x) { return x.type === 'hand' && x.host === into && x.id !== hid; });
        if (tg.kind === 'pointer') moveComp(hid, tg.ref, above);
        else if (sibs.length) moveComp(hid, sibs[sibs.length - 1].id, true);
        return { ok: true, order: live, comps: comps, host: into };
      }
      if (tg.kind === 'disc' || (onDial(tg) && !(tg.kind === 'dial' && above)))
        return { ok: false, reason: onDial(tg) ? 'R3: a hand turns above the dial, never under it' : 'R3: a hand turns above its face, never under what the face frames' };
      hc.host = 'dial';
      if (tLive) {
        var anchor = tg.kind === 'subdial' || tg.kind === 'hand' ? tg : items[find(items, 'comp:' + face)];
        var n = live.indexOf(keyOf(anchor));
        live.splice(above ? n + 1 : n, 0, hid);
        return { ok: true, order: live, comps: comps, host: 'dial' };
      }
      // below every lifted layer, on the dial side: the hand goes to the bottom of the live
      // band, and the faces displayed above the drop point are lifted over it
      var up = facesAbove(tg.kind === 'subdial' && above ? t : (tg.kind === 'dial' ? t : t)).reverse();
      live = [hid].concat(up.map(function (sid) { return 'comp:' + sid; })).concat(live);
      return { ok: true, order: live, comps: comps, host: 'dial', lift: up[0] };
    }
    if (it.kind === 'subdial') {
      var sid = it.ref;
      if (face === sid) return { ok: true, same: true };
      live = live.filter(function (e) { return e !== 'comp:' + sid; });
      if (tLive) {
        var anc = tg.kind === 'hand' ? tg : items[find(items, 'comp:' + face)];
        var m = live.indexOf(keyOf(anc));
        live.splice(above ? m + 1 : m, 0, 'comp:' + sid);
        return { ok: true, order: live, lift: sid };
      }
      if (face) { moveComp(sid, face, tg.kind === 'subdial' && above); return { ok: true, order: live, comps: comps }; }
      if (tg.kind === 'dial' && above) {
        var subsOnDial = comps.filter(function (x) { return x.type === 'subdial' && !lifted(x.id) && x.id !== sid; });
        if (subsOnDial.length) moveComp(sid, subsOnDial[0].id, false);
        return { ok: true, order: live, comps: comps };
      }
      return { ok: false, reason: 'R3: a sub-dial sits on the dial, so it stays above the dial’s face' };
    }
    if (it.kind === 'disc') {
      // the face it lands under: the one whose discs, cutouts or prints it is dropped among, or
      // a face dropped on from below (into it); a disc stays under a face, never among the hands
      var hostKey = function (x) { return x.host === 'dial' || !x.host ? 'dial' : x.host.slice(5); };
      var dest = tg.kind === 'disc' || tg.kind === 'cutout' ? hostKey(tg)
        : tg.kind === 'subdial' && !above && !lifted(tg.ref) ? tg.ref
        : tg.kind === 'dial' && !above ? 'dial'
        : (tg.kind === 'flare' || tg.kind === 'opening' || tg.kind === 'pointer') ? (tg.parent === 'dial' ? 'dial' : faceOf(tg)) : null;
      if (!dest) return { ok: false, reason: 'R4: a disc turns under a face \u2014 drop it among that face\u2019s discs, or on the face itself' };
      var dc = byId[it.ref], from = dc.host && dc.host !== 'dial' ? dc.host : 'dial';
      if (dest !== from) {
        if (dest !== 'dial' && lifted(dest)) return { ok: false, reason: 'R4: that face is lifted among the hands \u2014 a disc turns only under a face on the dial' };
        // re-hosted where it already stands on the dial: its polar place recomputed about the new face
        var centre = function (h) { if (h === 'dial' || !byId[h]) return [233, 233]; var q = byId[h], a = (+q.angle || 0) * Math.PI / 180, d = +q.dist || 0; return [233 + Math.sin(a) * d, 233 - Math.cos(a) * d]; };
        var c0 = centre(from), a0 = (+dc.angle || 0) * Math.PI / 180, d0 = +dc.dist || 0, px = c0[0] + Math.sin(a0) * d0, py = c0[1] - Math.cos(a0) * d0, c1 = centre(dest);
        var dx = px - c1[0], dy = py - c1[1], dd = Math.sqrt(dx * dx + dy * dy);
        dc.host = dest; dc.dist = Math.round(dd * 2) / 2; dc.angle = dd < 0.5 ? 0 : Math.round(((Math.atan2(dx, -dy) * 180 / Math.PI) + 360) % 360);
      }
      if (tg.kind === 'disc') moveComp(it.ref, tg.ref, above);
      else if (tg.kind === 'cutout') moveComp(it.ref, byId[tg.ref].host, true);
      return { ok: true, comps: comps, host: dest };
    }
    if (it.kind === 'cutout') {
      // a cutout reveals the wheel it is dropped on (or the wheel of the cutout it lands beside)
      var wid = tg.kind === 'disc' && byId[tg.ref] && byId[tg.ref].type === 'wheel' ? tg.ref : tg.kind === 'cutout' ? byId[tg.ref].host : null;
      if (!wid) return { ok: false, reason: 'R4: a cutout reveals a wheel \u2014 drop it on a wheel, or beside one of its cutouts' };
      byId[it.ref].host = wid;
      if (tg.kind === 'cutout') moveComp(it.ref, tg.ref, !above);
      return { ok: true, comps: comps, host: wid };
    }
    return { ok: false, reason: refusal(it, null, 1) };
  }
  // the old drop: onto a row, landing where the row is (above it when moving up)
  function moveTo(input, id, targetId) {
    var items = build(input), from = find(items, id), to = find(items, targetId);
    if (from < 0 || to < 0) return { ok: false, reason: 'no such layer' };
    return place(input, id, targetId, to < from ? 'above' : 'below');
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
    (input.comps || []).forEach(function (c) { if (c.type === 'window' || c.type === 'moon' || c.type === 'wheel') n += 1; });
    if (input.carriage) n += 1;
    // a lifted sub-dial face ships as one still row (D43 P4b)
    liveKeys(input).forEach(function (e) { if (e.indexOf('comp:') === 0) n += 1; });
    return { rows: n, cap: ROW_CAP, over: n > ROW_CAP };
  }

  var api = { compTitle: compTitle, build: build, move: move, moveTo: moveTo, place: place, liveKeys: liveKeys, warnings: warnings, rowCount: rowCount, ROW_CAP: ROW_CAP };
  root.OrbStack = api;
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
})(typeof window !== 'undefined' ? window : globalThis);
