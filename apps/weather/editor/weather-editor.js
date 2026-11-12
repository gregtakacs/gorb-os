/* weather-editor.js — the weather app's part of its theme editor (docs/weather-plan.md §2.6;
 * theme-editor-kit-plan §8, E3).
 *
 * Orb Manager owns the page (host@1): the start step, the library, import, the Render step,
 * the stage with its frame clock and its controls, Reset, the fit, history and the send.
 * What is the weather's is here, registered with OrbHost: the steps (Ground, Map, Icons,
 * Temperature, Words, Layout) and their fields, and the preview. The app's places are its
 * settings, not its theme: they live on the hub's App settings page (app.json declares them,
 * §57; owner 2026-11-06), never here.
 * The preview draws the app's four screens (Now, Places, Hourly, 5 days) from the very
 * frames the bake stores, laid out as apps/weather/main.c poses them. Now moves as the
 * glass does: fx.js (fx.c's twin, held to it by tools/host_weather_fx_test.py) runs the
 * rain, snow, mist, wisps and stars, the icon bobs and the clock ticks; the toolbar's
 * weather picks the condition, so every theme's every effect can be seen before it is
 * sent. On the Layout step Now's elements are picked and dragged on the glass. */
(function (root) {
  'use strict';
  var B = root.WeatherBake, FX = root.WeatherFx, M = root.WeatherMap;
  var word = function (hub, web) { return root.OrbTheme && root.OrbTheme.hostText ? root.OrbTheme.hostText(hub, web) : hub; };
  var G = 466, CX = 233, ROW_CAP = (root.GorbBake && root.GorbBake.CLK_ROW_MAX) || 64;
  var VIEWS = [['now', 'Now', 'Home on the glass, unarmed'], ['places', 'Places', 'Armed: turning steps the places'],
    ['hours', 'Hourly', 'A press: the next twelve hours on a clock face, the cursor on one'], ['days', '5 days', 'Another press: the next five days']];
  // the preview's made-up forecast (°C), the mockups' own
  var NOW = { name: 'Home', kind: 'partly', t: 17, feels: 15, wind: 14, hi: 18, lo: 10 };
  // the Now preview's condition, in the bake's KINDS order (main.c's COND words)
  var WEATHER = [['clear', 'Clear day', 'Clear'], ['clearN', 'Clear night', 'Clear'], ['partly', 'Partly cloudy', 'Partly cloudy'],
    ['partlyN', 'Partly cloudy, night', 'Partly cloudy'], ['cloudy', 'Cloudy', 'Cloudy'], ['fog', 'Fog', 'Fog'], ['drizzle', 'Drizzle', 'Drizzle'],
    ['rain', 'Rain', 'Rain'], ['snow', 'Snow', 'Snow'], ['thunder', 'Thunder', 'Thunder']];
  var PLACE2 = { name: 'Lisbon', kind: 'clear', t: 22, feels: 22, wind: 9, hi: 24, lo: 15 };
  var HOURS = [[14, 17, 'partly', 10], [15, 18, 'partly', 10], [16, 18, 'cloudy', 20], [17, 17, 'cloudy', 40], [18, 16, 'rain', 70], [19, 15, 'rain', 80],
    [20, 14, 'drizzle', 55], [21, 13, 'cloudy', 30], [22, 12, 'partlyN', 10], [23, 12, 'clearN', 5], [0, 11, 'clearN', 0], [1, 11, 'clearN', 0]];
  var DAYS = [['Fri', 'rain', 10, 18, 80], ['Sat', 'partly', 10, 18, 10], ['Sun', 'clear', 7, 17, 0], ['Mon', 'cloudy', 9, 15, 40], ['Tue', 'thunder', 11, 16, 90]];

  var clone = function (o) { return JSON.parse(JSON.stringify(o)); };
  function checkDesign(x) {
    if (!x || !x.plate || !x.words || !x.temps || !x.icons) return 'that design has no weather theme in it';
    if (!B.PACKS[x.icons.pack]) return 'that design names an icon pack this editor does not carry (' + x.icons.pack + ')';
    if (!Array.isArray(x.temps.ramp) || x.temps.ramp.length !== 3) return 'that design’s temperature colours are not three stops';
    return null;
  }

  /* ---- the preview: one screen, drawn from the bake's own frames, each part drawn again
   * only when its own part of the design changes (the host keeps them, c.part) ---- */
  // the place a screen shows: Places the place control's (none while the orb has none set),
  // every other screen home
  function shownPlace(c) { return c.view.show === 'places' ? c.place : c.home; }
  function partsOf(d, c) {
    var plate = c.part('plate', JSON.stringify(d.plate), function () { return B.plateCanvas(d); });
    var icons = c.part('icons', JSON.stringify(d.icons) + d.plate.c1 + B.sizeOf(d, 'icon'), async function () {
      var big = await B.iconFrames(d, B.sizeOf(d, 'icon'));
      // Hourly's own, at the default size whatever Now's is (§2.10)
      return { big: big, hour: B.sizeOf(d, 'icon') === B.ICON_PX ? big : await B.iconFrames(d, B.ICON_PX), small: await B.iconFrames(d, B.SICON_PX) };
    });
    var temps = c.part('temps', JSON.stringify(d.temps) + JSON.stringify(d.words) + JSON.stringify(d.icons.c), async function () { return { bars: B.barFrames(d), dots: B.dotFrames(d), parts: B.partFrames(d), bolt: B.boltFrame(d) }; });
    // the words at their sizes, and what is measured in them: the days' columns, the day
    // bar they leave room for, and its halo
    var words = c.part('words', JSON.stringify(d.words) + JSON.stringify(d.temps) + JSON.stringify(d.size || {}), async function () {
      var W = d.words;
      try {
        await B.loadFace(W.text, B.sizeOf(d, 'text')); await B.loadFace(W.label, B.sizeOf(d, 'label')); await B.loadFace(W.num, B.sizeOf(d, 'num'));
        await B.loadFace(W.text, B.SIZES.text[2]); await B.loadFace(W.label, B.SIZES.label[2]); await B.loadFace(W.num, B.SIZES.num[2]);
      } catch (e) { /* fallback */ }
      var dl = B.dayLayout(d);
      return { dl: dl, days: B.dayFrames(d, dl.dw), halo: B.haloFrame(d, dl.dw) };
    });
    // the shown place's map at the design's area (maps@1), drawn as the orb draws it: home's
    // on its screens, the place control's on Places
    var at = shownPlace(c), map = null;
    if (at && d.map && d.map.on && root.GorbMaps) {
      var mz = M.levelOf(at.lat, d.map.area).z, lk = at.lat + '|' + at.lon + '|' + mz;
      var bytes = c.part('level', lk, function () { return root.GorbMaps.fetchLevel(at.lat, at.lon, mz); }, { fresh: true });
      if (bytes) map = c.part('map', JSON.stringify(d.map) + '|' + lk, function () { return M.view(d.map, bytes, at.lat); });
    }
    return { plate: plate, icons: icons, temps: temps, words: words, map: map };
  }
  var KIND = function (k) { return B.KINDS.indexOf(k); };
  /* The Now screen's life, as main.c's stage_life poses it: the particles (each its frame,
   * at its place plus snow's sway, at its brightness), the icon's 3 px bob over 4 s and the
   * clock. `L` is {fx, t (ms), clock}; without one the screen stands still. */
  function drawLife(x, d, P, L) {
    if (!L || d.fx && d.fx.on === false) return;
    for (var i = 0; i < L.fx.n; i++) {
      var o = FX.opa(L.fx, i); if (!o) continue;
      var q = L.fx.p[i], c = P.temps.parts[q.f];
      x.globalAlpha = o / 255;
      x.drawImage(c, Math.round(q.x + FX.sway(L.fx, i)) - (c.width >> 1), Math.round(q.y) - (c.height >> 1));
    }
    var bo = FX.bolt(L.fx);                                       // thunder's strike
    if (bo.opa && P.temps.bolt) { x.globalAlpha = bo.opa / 255; x.drawImage(P.temps.bolt, Math.round(bo.x) - (P.temps.bolt.width >> 1), Math.round(bo.y) - (P.temps.bolt.height >> 1)); }
    x.globalAlpha = 1;
  }
  /* The places Now's elements were hit at, for the Layout step's picking: [{k, x0, y0, x1, y1}]. */
  var HITS = [];
  // pname, poff: the place Places shows and its zone's offset now (minutes), else the made-up Lisbon
  function drawView(x, d, P, view, L, sel, pname, poff) {
    x.clearRect(0, 0, G, G);
    if (P.plate) x.drawImage(P.plate, 0, 0); else { x.fillStyle = '#000'; x.fillRect(0, 0, G, G); }
    // the shown place's map (flights-map-plan §5): home's, or on Places the chosen place's
    if (P.map && d.map && d.map.on) x.drawImage(P.map, 0, 0);
    if (!P.icons || !P.temps || !P.words) return;
    var W = d.words, caps = function (s) { return W.caps ? s.toUpperCase() : s; };
    var tx = B.sizeOf(d, 'text'), lb = B.sizeOf(d, 'label'), nu = B.sizeOf(d, 'num'), ip = B.sizeOf(d, 'icon');
    var hit = function (k, cx, cy, w, hh) { HITS.push({ k: k, x0: cx - w / 2, y0: cy - hh / 2, x1: cx + w / 2, y1: cy + hh / 2 }); };
    var text = function (s, y, face, px, col, al, xx, op, k) {
      x.globalAlpha = op == null ? 1 : op;
      x.font = px + 'px "' + face + '", sans-serif'; x.fillStyle = col; x.textAlign = al || 'center'; x.textBaseline = 'middle';
      x.letterSpacing = (face === W.label ? (W.track | 0) : 1) + 'px';   // the bake's track: the Titles face's, 1 px the rest
      x.fillText(s, xx == null ? CX : xx, y);
      x.globalAlpha = 1;
      if (k) hit(k, xx == null ? CX : xx, y, Math.max(24, x.measureText(s).width + 8), px * 1.3);
    };
    var blit = function (c, cx, cy, op) { x.globalAlpha = op == null ? 1 : op; x.drawImage(c, cx - c.width / 2, cy - c.height / 2); x.globalAlpha = 1; };
    HITS = [];
    if (view === 'now' || view === 'places') {
      var p = view === 'now' ? Object.assign({}, NOW, { kind: L ? L.kind : NOW.kind }) : pname ? Object.assign({}, PLACE2, { name: pname }) : PLACE2;
      var wx = WEATHER[KIND(p.kind)] || WEATHER[2], A = B.layoutNow(d);
      if (view === 'now') drawLife(x, d, P, L);
      text(view === 'places' ? p.name : caps(p.name), A.title.y, W.label, lb, W.quiet, 'center', A.title.x, null, 'title');
      blit(P.temps.dots[6 + (view === 'places' ? 1 : 0)], A.dots.x, A.dots.y); hit('dots', A.dots.x, A.dots.y, 64, 14);
      blit(P.icons.big[KIND(p.kind)], A.icon.x, A.icon.y + (view === 'now' && L ? Math.round(3 * Math.sin((L.t % 4000) * 2 * Math.PI / 4000)) : 0));
      hit('icon', A.icon.x, A.icon.y, ip, ip);
      text(p.t + '°', A.big.y, W.num, nu, W.ink, 'center', A.big.x, null, 'big');
      text(wx[2], A.line.y, W.text, tx, W.ink, 'center', A.line.x, null, 'line');
      var lh = Math.round(lb * 1.3);
      text(caps('Feels ' + p.feels + '°  Wind ' + p.wind + ' km/h'), A.detail.y - lh, W.label, lb, W.quiet, 'center', A.detail.x, null, 'detail');
      text(caps('H ' + p.hi + '°  L ' + p.lo + '°  1016 hPa \u2193'), A.detail.y, W.label, lb, W.quiet, 'center', A.detail.x, null, 'detail');   /* and the pressure's way */
      text(caps('Sunset 17:03'), A.detail.y + lh, W.label, lb, W.quiet, 'center', A.detail.x, null, 'detail');   /* the next dawn or dusk */
      // the clock: Home's is the orb's, a place's its own local time from its zone's offset
      // (the orb's places carry their zones from OS v1.2.38); the made-up Lisbon keeps UTC
      var there = function () {
        var off = poff == null ? 0 : poff, n = new Date(Date.now() + off * 60000);
        return String(n.getUTCHours()).padStart(2, '0') + ':' + String(n.getUTCMinutes()).padStart(2, '0');
      };
      text(view === 'now' ? (L ? L.clock : '10:42') : there(), A.clock.y, W.text, tx, W.ink, 'center', A.clock.x, null, 'clock');
      // the Layout step's pick: a dashed frame round the element
      var on = HITS.filter(function (q) { return q.k === sel; });
      if (on.length) {
        var bx = { x0: Math.min.apply(null, on.map(function (q) { return q.x0; })), y0: Math.min.apply(null, on.map(function (q) { return q.y0; })),
          x1: Math.max.apply(null, on.map(function (q) { return q.x1; })), y1: Math.max.apply(null, on.map(function (q) { return q.y1; })) };
        // round, as the glass is: a ring round the icon, a pill round a line of words
        var cxh = (bx.x0 + bx.x1) / 2, cyh = (bx.y0 + bx.y1) / 2, ww = bx.x1 - bx.x0 + 12, hh = bx.y1 - bx.y0 + 8;
        x.save(); x.setLineDash([5, 4]); x.strokeStyle = '#58a6ff'; x.lineWidth = 1.5; x.beginPath();
        if (sel === 'icon') x.arc(cxh, cyh, Math.max(ww, hh) / 2 * 1.05, 0, 2 * Math.PI);
        else x.roundRect(cxh - ww / 2, cyh - hh / 2, ww, hh, hh / 2);
        x.stroke(); x.restore();
      }
    } else if (view === 'hours') {
      // Hourly and the 5 days draw at the fixed sizes, whatever Now's (§2.10)
      tx = B.SIZES.text[2]; lb = B.SIZES.label[2]; nu = B.SIZES.num[2]; ip = B.ICON_PX;
      var lo = 11, hi = 18, sel2 = 3;
      HOURS.forEach(function (hr, k) {
        var Lv = Math.max(0, Math.min(B.BAR_LEN - 1, Math.round((hr[1] - lo) / (hi - lo) * (B.BAR_LEN - 1))));
        var rain = hr[3] < 30 ? 0 : hr[3] < 70 ? 1 : 2, f = P.temps.bars[Lv * B.BAR_RAIN + rain];
        var a = (hr[0] % 12) * 30 * Math.PI / 180;
        x.save(); x.globalAlpha = k === sel2 ? 1 : 120 / 255;
        x.translate(CX + 157 * Math.sin(a), CX - 157 * Math.cos(a)); x.rotate(a);
        x.drawImage(f, -f.width / 2, -f.height / 2); x.restore(); x.globalAlpha = 1;
      });
      // main.c stage_hours: the stack by the theme's sizes, a small icon where a large one
      // would reach the bars
      var s = HOURS[sel2], ht = Math.round(lb * 1.45) + 2, hb = Math.round(nu * 0.85) + 2, hl = Math.max(28, Math.ceil(tx * 1.5)) + 2;
      var useSmall = ht + ip + hb + hl + 20 > 200, hi_ = useSmall ? B.SICON_PX : ip;
      var y = 227 - Math.round((ht + 4 + hi_ + 1 + hb + 15 + hl) / 2);
      text('17:00', y + ht / 2, W.label, lb, W.quiet); y += ht + 4;
      blit((useSmall ? P.icons.small : P.icons.hour)[KIND(s[2])], CX, y + hi_ / 2); y += hi_ + 1;
      text(s[1] + '°', y + hb / 2, W.num, nu, W.ink, 'center', CX + 6); y += hb + 15;
      text(caps('Rain ' + s[3] + '%   Cloudy'), y + hl / 2, W.label, lb, W.quiet);
    } else {
      // main.c stage_days: the columns from the measured widths, the halo on the day under
      // the cursor, that day told below the list
      tx = B.SIZES.text[2]; lb = B.SIZES.label[2];
      var dl = P.words.dl, c = dl.cols, dw = dl.dw, si = B.SICON_PX, pitch = 2 * Math.max(28, Math.ceil(tx * 1.5)), Y0 = 96 - 2 * (pitch - 56), cur = 1;
      var span = c[0] + si - 2 + c[1] - 8 + 4 + dw + 2 + c[2] + c[3] - 2;
      var aL = CX - Math.floor(span / 2), iconX = aL + c[0] + si / 2 - 2, bR = iconX + si / 2 + c[1] - 8, barX = bR + 4 + dw / 2, cL = barX + dw / 2 + 2, dR = cL + c[2] + c[3] - 2;
      text(caps('5 days'), 50, W.label, lb, W.quiet);
      var wlo = 7, whi = 18, pos = function (v) { return Math.round((v - wlo) / (whi - wlo) * (B.DAY_POS - 1)); };
      DAYS.forEach(function (dy, i) {
        var y = Y0 + pitch * i, a = pos(dy[2]), b = pos(dy[3]), fi = B.DAY_POS * a - a * (a - 1) / 2 + (b - a);
        if (i === cur) blit(P.words.halo, barX, y);
        text(dy[0], y, W.text, tx, W.ink, 'left', aL);
        blit(P.icons.small[KIND(dy[1])], iconX, y);
        text(dy[2] + '°', y, W.text, tx, W.ink, 'right', bR, 170 / 255);
        blit(P.words.days[fi], barX, y);
        text(dy[3] + '°', y, W.text, tx, W.ink, 'left', cL);
        text(dy[4] + '%', y, W.text, tx, W.ink, 'right', dR, 170 / 255);
      });
      var lineY = Y0 + 4 * pitch + pitch / 2 + 4, lh2 = Math.round(lb * 1.3), hl2 = Math.max(28, Math.ceil(tx * 1.5)) + 2;
      text(WEATHER[KIND(DAYS[cur][1])][2], lineY, W.text, tx, W.ink);
      var dy0 = lineY + hl2 / 2 + lh2 + 2;
      text(caps('0.4 mm  18 km/h  UV 3'), dy0 - lh2 / 2, W.label, lb, W.quiet);
      text('↑ 07:14  ↓ 16:58', dy0 + lh2 / 2, W.label, lb, W.quiet);
    }
  }

  /* ---- Now's life: fx.js stepped against the frame clock, as the orb's stage_life does (the
   * pool sized by the theme's own rows, re-seeded when the condition changes) ---- */
  function lifeOf(d, c) {
    var L = c.memo.life || (c.memo.life = { fx: FX.create(12345), t: 0, kind: '', n: -1, clock: '10:42' });
    var n = d.fx && d.fx.on === false ? 0 : B.N_PART;
    if (L.kind !== c.view.weather || L.n !== n) { L.kind = c.view.weather; L.n = n; FX.set(L.fx, KIND(L.kind), n); }
    if (c.dt) FX.step(L.fx, c.dt / 1000);
    L.t = c.t;
    var now = new Date(); L.clock = String(now.getHours()).padStart(2, '0') + ':' + String(now.getMinutes()).padStart(2, '0');   // the OS's 24-hour form
    return L;
  }

  /* ---- the Layout step: Now's elements picked and dragged on the glass (the clock editor's
   * way: a drag sets the element's Angle and Distance from its own place) ---- */
  function placed(d, k, a, r) {
    var n = clone(d); n.place = Object.assign({}, n.place || {});
    n.place[k] = { a: Math.round(((a % 360) + 360) % 360), r: Math.max(0, Math.min(220, Math.round(r))) };
    return n;
  }
  var homeOf = function (d, k) { var z = clone(d); z.place = {}; return B.layoutNow(z)[k]; };
  var drag = null;
  var pointer = {
    down: function (pt, c) {
      if (c.step !== 'layout' || (c.view.show !== 'now' && c.view.show !== 'places')) return false;
      var hs = HITS.filter(function (q) { return pt[0] >= q.x0 && pt[0] <= q.x1 && pt[1] >= q.y0 && pt[1] <= q.y1; });
      if (!hs.length) return false;
      var k = hs[hs.length - 1].k, at = B.layoutNow(c.d)[k];
      c.setUi({ picked: k });
      drag = { k: k, ox: pt[0] - at.x, oy: pt[1] - at.y, home: homeOf(c.d, k) };
      return true;
    },
    move: function (pt, c) {
      if (!drag) return;
      var dr = drag, dx = pt[0] - dr.ox - dr.home.x, dy = pt[1] - dr.oy - dr.home.y;
      c.set(function (o) { return placed(o, dr.k, Math.atan2(dx, -dy) * 180 / Math.PI, Math.hypot(dx, dy)); });
    },
    up: function () { drag = null; },
    cursor: function (c) { return c.step === 'layout' ? (drag ? 'grabbing' : 'grab') : 'default'; },
  };

  /* ---- the fields of each step ---- */
  function fields(step, c) {
    var d = c.d, K = c.K, upd = c.upd, setD = c.set, base = c.base, say = c.say, getIn = c.getIn, home = c.home, picked = c.ui.picked;
    var opt = function (pairs) { return pairs.map(function (p) { return { v: p[0], label: p[1] }; }); };
    var C = function (label, path, presetPath) {
      return K.Co(label, getIn(d, path), function (nv) { upd(path, nv); }, '', path, { noAuto: true, preset: getIn(base, presetPath || path) });
    };
    var setPlace = function (k, a, r) { setD(function (o) { return placed(o, k, a, r); }); };
    var faces = B.FACES.map(function (k) { return { v: k, label: k }; });
    var fields = [];
    var pctOf = function (v) { return Math.round(v * 100) + '%'; };
    var mRg = function (label, path, min, max, stp, fmt) { var v = +getIn(d, path); return K.Rg(label, v, min, max, stp, function (nv) { upd(path, nv); }, fmt(v)); };
    var mPick = function (label, path, pairs) { return K.Ch(label, getIn(d, path), opt(pairs), function (v) { upd(path, v); }); };
    var mOn = function (label, path, a, b) { return K.Ch(label, getIn(d, path) ? 'on' : 'off', opt([['on', a], ['off', b]]), function (v) { upd(path, v === 'on'); }); };
    if (step === 'map') {
      var mp = d.map;
      fields.push(K.H('Map'));
      fields.push(mOn('Map', 'map.on', 'Behind the reading', 'None'));
      if (!mp.on) fields.push(K.Nt('', 'A hint of the place behind the weather: its roads, water and towns, from the maps Orb Manager makes for home and the places. Each place shows its own.'));
      if (mp.on) {
        fields.push(K.Ch('Look', mp.look, opt(M.LOOKS), function (v) { setD(function (o) { var n = clone(o); n.map = M.applyLook(o.map, v, o.base); return n; }); }));
        fields.push(mRg('Strength', 'map.strength', 0.1, 1, 0.05, function (v) { return Math.abs(v - 0.5) < 0.001 ? 'the look’s own' : Math.round(v / 0.5 * 100) + '% of the look'; }));
        fields.push(K.Nt('', 'Every opacity at once: lower keeps the reading in front.'));
        fields.push(mPick('Reach', 'map.area', M.AREAS));
        fields.push(K.H('What is drawn'));
        fields.push(mPick('Water', 'map.water', [['all', 'Lakes, sea and rivers'], ['lakes', 'Lakes and sea'], ['off', 'None']]));
        fields.push(mPick('Roads', 'map.roads', [['motorways', 'Motorways'], ['main', 'Main roads too'], ['off', 'None']]));
        fields.push(mOn('Airports', 'map.airports', 'A symbol', 'None'));
        if (mp.airports) fields.push(mPick('Symbol', 'map.symbol', M.SYMBOLS));
        fields.push(mOn('Town dots', 'map.dots', 'Drawn', 'None'));
        fields.push(mOn('Built-up areas', 'map.urban', 'Tinted', 'Not tinted'));
        fields.push(mOn('Borders', 'map.borders', 'State and country lines', 'None'));
        fields.push(K.H('Colours'));
        M.INKS.forEach(function (ik) {
          var k = ik[0];
          if ((k === 'water' || k === 'river') && mp.water === 'off') return;
          if (k === 'river' && mp.water !== 'all') return;
          if ((k === 'road1' || k === 'road2') && mp.roads === 'off') return;
          if (k === 'road2' && mp.roads !== 'main') return;
          if ((k === 'airport' && !mp.airports) || (k === 'town' && !mp.dots) || (k === 'urban' && !mp.urban) || (k === 'border' && !mp.borders)) return;
          fields.push(K.Co(ik[1], mp.ink[k], function (nv) { upd('map.ink.' + k, nv); }, '', 'map.ink.' + k, { noAuto: true, preset: getIn(base, 'map.ink.' + k) }));
          fields.push(mRg('Opacity', 'map.alpha.' + k, 0, 1, 0.05, pctOf));
        });
        fields.push(K.H('Lines'));
        M.LINES.forEach(function (ln) {
          var k = ln[0], L = mp.line[k];
          if ((k === 'road1' && mp.roads === 'off') || (k === 'road2' && mp.roads !== 'main') || (k === 'river' && mp.water !== 'all') || (k === 'border' && !mp.borders)) return;
          fields.push(K.Ch(ln[1], L[1], opt([['solid', 'Solid'], ['long', 'Dashed'], ['short', 'Short dashes'], ['dot', 'Dotted'], ['dashdot', 'Dash-dot']]),
            function (v) { upd('map.line.' + k, [L[0], v]); }));
          fields.push(K.Rg('Weight', L[0], 1, 4, 1, function (v) { upd('map.line.' + k, [v, L[1]]); }, L[0] + ' px'));
        });
        fields.push(K.Nt('', home ? 'The preview is your own home map, drawn the way the orb draws it; on the orb each place shows its own.' : word('Open this editor from the hub, with home set on the Orb tab, to preview the map on your own home.', 'Open this editor with home set on the Orb tab, to preview the map on your own home.')));
      }
    }
    if (step === 'ground') {
      var P = d.plate, GD = root.WeatherGround, KEEP = ['finish', 'tex', 'weaveAng', 'weaveSc', 'marks', 'img', 'imgScale', 'imgX', 'imgY', 'imgDim', 'age', 'stain', 'dust'];
      var upP = function (o) { setD(function (x) { var n = clone(x); n.plate = Object.assign({}, n.plate, o); return n; }); };
      fields.push(K.H('Ground'));
      fields.push(K.Ch('Look', P.style, opt(Object.keys(B.GROUNDS).map(function (k) { return [k, B.GROUNDS[k]]; }).concat([['plain', 'Plain']])), function (v) {
        // a look brings its colours and markings; the finish, the picture and the wear stay
        setD(function (o) { var n = clone(o), src = B.PRESETS_DEF[v], keep = {};
          KEEP.forEach(function (k) { if (o.plate[k] !== undefined) keep[k] = o.plate[k]; });
          n.plate = Object.assign(src ? clone(src.plate) : Object.assign({}, o.plate, { style: v }), keep); return n; });
      }));
      fields.push(K.Ch('Markings', P.marks === false ? 'off' : 'on', opt([['on', 'The look’s'], ['off', 'None']]), function (v) { upP({ marks: v === 'on' }); }));
      fields.push(K.Nt('', 'The look’s rings, ticks and bezel, drawn over the face (or over your picture).'));
      // the clock dial's colours: a pick sets the face's three stops from its pair
      var sw = function (grp) { return GD.GROUPS[grp].map(function (k) { return { k: k, label: k[0].toUpperCase() + k.slice(1), bg: GD.COLOURS[k][0] }; }); };
      var pickCol = function (k) { var c = GD.COLOURS[k]; upP({ c0: c[0], c1: B.mix(c[0], c[1], 0.5), c2: c[1], colour: k }); };
      fields.push(K.H('Colour'));
      if (GD) { fields.push(K.Sw('Classic', P.colour, sw('classic'), pickCol)); fields.push(K.Sw('Vivid', P.colour, sw('vivid'), pickCol)); }
      fields.push(C('Centre', 'plate.c0')); fields.push(C('Face', 'plate.c1')); fields.push(C('Rim', 'plate.c2'));
      fields.push(C(P.style === 'brass' ? 'Bezel metal' : P.style === 'celestial' ? 'Nebula' : 'Detail', 'plate.detail'));
      fields.push(C('Ticks', 'plate.tick'));
      if (GD) {
        var fin = P.finish || 'plain';
        fields.push(K.H('Surface'));
        fields.push(K.Sl('Finish', fin, opt(Object.keys(GD.FINISHES).map(function (k) { return [k, GD.FINISHES[k]]; })), function (v) { upP({ finish: v }); }));
        if (fin !== 'plain' && fin !== 'enamel') fields.push(K.Rg('Texture', P.tex == null ? 1 : +P.tex, 0, 2.5, 0.05, function (v) { upP({ tex: +v }); }, (P.tex == null ? 1 : +P.tex).toFixed(2) + '×'));
        if (fin === 'carbon') fields.push(K.Rg('Weave angle', +P.weaveAng || 0, 0, 90, 1, function (v) { upP({ weaveAng: +v }); }, (+P.weaveAng || 0) + '°'));
        if (fin === 'carbon' || fin === 'forged') { var wsv = P.weaveSc == null ? 1 : +P.weaveSc; fields.push(K.Rg(fin === 'carbon' ? 'Weave size' : 'Flake size', wsv, 0.4, 3, 0.05, function (v) { upP({ weaveSc: +v }); }, wsv.toFixed(2) + '×')); }
        fields.push(K.Nt('', 'The clock dial’s finishes, in the face colours: Gradient is the look’s own.'));
      }
      fields.push(K.H('Picture'));
      fields.push(K.Fi(P.img ? 'Replace picture' : 'Your picture', function (e) {
        var fl = e.target.files && e.target.files[0]; if (!fl) return;
        var rd = new FileReader();
        rd.onload = function () {
          var im = new Image();
          im.onload = function () {
            // kept at most 932 px (twice the glass) as a JPEG, so a pan or zoom has pixels to spare
            var k = Math.min(1, 932 / Math.max(im.width, im.height)), cv = document.createElement('canvas');
            cv.width = Math.round(im.width * k); cv.height = Math.round(im.height * k);
            cv.getContext('2d').drawImage(im, 0, 0, cv.width, cv.height);
            upP({ img: cv.toDataURL('image/jpeg', 0.88), imgScale: 1, imgX: 0, imgY: 0 });
            say('The picture is the ground: the look’s markings and the wear draw over it');
          };
          im.onerror = function () { say('That file is not a picture this browser can read'); };
          im.src = rd.result;
        };
        rd.readAsDataURL(fl);
      }, P.img ? 'on the ground' : '', 'image/png,image/jpeg,image/webp,image/svg+xml'));
      if (P.img) {
        var isc = P.imgScale == null ? 1 : +P.imgScale;
        fields.push(K.Rg('Zoom', isc, 1, 4, 0.05, function (v) { upP({ imgScale: +v }); }, isc.toFixed(2) + '×'));
        fields.push(K.Rg('Across', +P.imgX || 0, -233, 233, 1, function (v) { upP({ imgX: +v }); }, (+P.imgX || 0) + ' px'));
        fields.push(K.Rg('Up and down', +P.imgY || 0, -233, 233, 1, function (v) { upP({ imgY: +v }); }, (+P.imgY || 0) + ' px'));
        fields.push(K.Rg('Darken', +P.imgDim || 0, 0, 0.8, 0.05, function (v) { upP({ imgDim: +v }); }, Math.round((+P.imgDim || 0) * 100) + '%'));
        fields.push(K.Ch('', '', [{ v: 'x', label: 'Remove the picture' }], function () { upP({ img: null }); }));
      }
      fields.push(K.Nt('', 'A photo or drawing of your own as the ground, filling the glass. Darken it until the words read over it. It is baked into the ground picture, so it costs the orb nothing.'));
      if (GD) {
        var Wv = { age: +P.age || 0, stain: +P.stain || 0, dust: +P.dust || 0 };
        var cur = Object.keys(B.WEAR).find(function (k) { var w = B.WEAR[k]; return w[0] === Wv.age && w[1] === Wv.stain && w[2] === Wv.dust; }) || '';
        fields.push(K.H('Wear'));
        fields.push(K.Ch('Quick set', cur, Object.keys(B.WEAR).map(function (k) { return { v: k, label: k }; }), function (k) { var w = B.WEAR[k]; upP({ age: w[0], stain: w[1], dust: w[2] }); }));
        fields.push(K.Rg('Ageing', Wv.age, 0, 1.5, 0.05, function (v) { upP({ age: +v }); }, Math.round(Wv.age * 100) + '%'));
        fields.push(K.Rg('Stains', Wv.stain, 0, 1.5, 0.05, function (v) { upP({ stain: +v }); }, Math.round(Wv.stain * 100) + '%'));
        fields.push(K.Rg('Dust & scratches', Wv.dust, 0, 1.5, 0.05, function (v) { upP({ dust: +v }); }, Math.round(Wv.dust * 100) + '%'));
      }
      fields.push(K.Nt('', 'The ground is one picture under every screen; the icons and words draw over it.'));
      fields.push(K.H("The OS's overlay"));
      fields.push(K.Co("The OS's overlay", B.overlayOf(d), function (nv) { upd('overlay', nv); }, '', 'overlay',
        { noAuto: true, preset: B.packStart(d.icons.pack)[B.accentKey(d.icons.pack)] }));
      fields.push(K.Nt('', 'The colour the orb’s own overlays — the ring, the battery, the notices — are drawn in over this look. It starts as the pack’s accent; set it to something else to make the OS’s marks stand apart.'));
    } else if (step === 'icons') {
      var I = d.icons, pk = B.PACKS[I.pack];
      fields.push(K.H('Icon pack'));
      fields.push(K.Ch('Pack', I.pack, opt(Object.keys(B.PACKS).map(function (k) { return [k, B.PACKS[k].title]; })), function (v) {
        setD(function (o) { var n = clone(o); n.icons = { pack: v, c: B.packStart(v), shade: o.icons.shade, shadow: o.icons.shadow }; return n; });
      }));
      fields.push(K.Nt('', pk.note + '. The theme keeps its own copy of the pack, in its own colours.'));
      fields.push(K.Ch('Shading', I.shade === '3d' ? '3d' : 'flat', opt([['flat', 'Flat'], ['3d', '3D']]), function (v) { upd('icons.shade', v); }));
      fields.push(K.Ch('Drop shadow', I.shadow || 'off', opt([['off', 'None'], ['soft', 'Soft'], ['strong', 'Strong']]), function (v) { upd('icons.shadow', v); }));
      fields.push(K.Ch('Weather effects', d.fx && d.fx.on === false ? 'off' : 'on', opt([['on', 'On'], ['off', 'Off']]), function (v) { upd('fx', { on: v === 'on' }); }));
      fields.push(K.Nt('', 'Rain falls, snow drifts, mist and cloud wisps pass behind the weather now, and stars come out on a clear night: drawn live on the orb, in the rain colour and the words\u2019 colours.'));
      fields.push(K.H('Colours'));
      pk.roles.forEach(function (r) {
        fields.push(K.Co(r[1], I.c[r[0]], function (nv) { upd('icons.c.' + r[0], nv); }, '', 'icons.' + r[0], { noAuto: true, preset: B.packStart(I.pack)[r[0]] }));
      });
      fields.push(K.Nt('', 'Its size is on the Layout step, with its place. The days’ icons stay ' + B.SICON_PX + ' px. Small icons carry two of shape, colour and detail, never all three.'));
    } else if (step === 'temps') {
      fields.push(K.H('Temperature colours'));
      fields.push(K.Co('Cold', d.temps.ramp[0], function (v) { var s = d.temps.ramp.slice(); s[0] = v; upd('temps.ramp', s); }, '', 'ramp0', { noAuto: true, preset: base.temps.ramp[0] }));
      fields.push(K.Co('Mild', d.temps.ramp[1], function (v) { var s = d.temps.ramp.slice(); s[1] = v; upd('temps.ramp', s); }, '', 'ramp1', { noAuto: true, preset: base.temps.ramp[1] }));
      fields.push(K.Co('Hot', d.temps.ramp[2], function (v) { var s = d.temps.ramp.slice(); s[2] = v; upd('temps.ramp', s); }, '', 'ramp2', { noAuto: true, preset: base.temps.ramp[2] }));
      var lum = function (c) { var n = parseInt(c.slice(1), 16), f = function (v) { v /= 255; return v <= 0.03928 ? v / 12.92 : Math.pow((v + 0.055) / 1.055, 2.4); }; return 0.2126 * f(n >> 16) + 0.7152 * f((n >> 8) & 255) + 0.0722 * f(n & 255); };
      var Ls = d.temps.ramp.map(lum), spread = Math.max.apply(null, Ls) - Math.min.apply(null, Ls);
      fields.push(K.Nt('', spread < 0.08 ? 'These three are close in lightness: a colour-blind eye may not tell cold from hot. Spread them.' : 'The hourly bars and the day bars run cold to hot through these.'));
      fields.push(C('Rain', 'temps.rain'));
      fields.push(C('Bar track', 'temps.track'));
      fields.push(K.Ch('Bar ends', d.temps.cap, opt([['round', 'Round'], ['butt', 'Square']]), function (v) { upd('temps.cap', v); }));
      fields.push(K.Rg('Bar width', d.temps.width || 6, 2, 14, 1, function (v) { upd('temps.width', v); }, (d.temps.width || 6) + ' px'));
      fields.push(K.Ch('Bar fill', d.temps.fill || 'flat', opt([['flat', 'One colour'], ['gradient', 'Cold to its colour']]), function (v) { upd('temps.fill', v); }));
    } else if (step === 'words') {
      var W = d.words;
      fields.push(K.H('Typefaces'));
      fields.push(K.Sl('Figures', W.num, faces, function (v) { upd('words.num', v); }));
      fields.push(K.Sl('Words', W.text, faces, function (v) { upd('words.text', v); }));
      fields.push(K.Sl('Titles', W.label, faces, function (v) { upd('words.label', v); }));
      fields.push(K.Ch('Titles in', W.caps ? 'caps' : 'as written', opt([['caps', 'Capitals'], ['as written', 'As written']]), function (v) { upd('words.caps', v === 'caps'); }));
      fields.push(K.Rg('Letter spacing', W.track | 0, 0, 6, 1, function (v) { upd('words.track', v); }, (W.track | 0) + ' px'));
      fields.push(K.Nt('', 'Letter spacing opens up the titles and the details (the Titles face). Sizes are on the Layout step, beside each element’s place.'));
      fields.push(K.H('Colours'));
      fields.push(C('Words', 'words.ink')); fields.push(C('Quiet', 'words.quiet'));
      fields.push(K.Nt('', 'Quiet is the place name, the time and the details under the temperature; the lows and the rain column of the days are the words, dimmed.'));
    } else if (step === 'layout') {
      var pl = B.placeOf(d, picked);
      fields.push(K.H('Now / Places elements'));
      fields.push(K.Sl('Element', picked, opt(B.ELEMENTS), function (v) { c.setUi({ picked: v }); }));
      // its size: the icon's own, or its font's (one size a font: the orb keeps one set of
      // glyphs each), and what else that font draws
      var SZK = { icon: 'icon', big: 'num', line: 'text', clock: 'text', title: 'label', detail: 'label' }[picked];
      if (SZK) {
        var zr = B.SIZES[SZK], zv = B.sizeOf(d, SZK);
        fields.push(K.Rg('Size', zv, zr[0], zr[1], SZK === 'icon' ? 2 : 1, function (nv) { var o = Object.assign({}, d.size); o[SZK] = +nv; upd('size', o); }, zv + ' px'));
        fields.push(K.Nt('', {
          icon: 'The large icon on Now and Places. Hourly has its own, at ' + B.ICON_PX + ' px, so this moves nothing there.',
          num: 'The big temperature\u2019s font on Now and Places. Hourly\u2019s is its own, at ' + B.SIZES.num[2] + ' px.',
          text: 'The Words font on Now and Places: the condition and the clock share this size. Hourly and the 5 days have their own, at ' + B.SIZES.text[2] + ' px.',
          label: 'The Titles font on Now and Places: the place name and the details share this size. Hourly and the 5 days have their own, at ' + B.SIZES.label[2] + ' px.' }[SZK]));
      }
      fields.push(K.Rg('Angle', pl.a, 0, 359, 1, function (v) { setPlace(picked, +v, pl.r); }, pl.a + '°'));
      fields.push(K.Rg('Distance', pl.r, 0, 220, 1, function (v) { setPlace(picked, pl.a, +v); }, pl.r + ' px'));
      fields.push(K.Ch('', '', [{ v: 'reset', label: 'Back to its place' }], function () { setPlace(picked, 0, 0); }));
      fields.push(K.Nt('', 'Pick an element on the glass and drag it, or set its Angle (the direction, 0 up, turning clockwise) and Distance from its own place, as the clock editor places a part. A size moves nothing else. Now and Places only: Hourly and the 5 days keep the app’s layout at their own fixed sizes, so nothing here moves them.'));
      if (c.view.show !== 'now' && c.view.show !== 'places') fields.push(K.Nt('', 'Show Now to move the elements.'));
    }

    return fields;
  }

  var app = {
    slug: 'weather', title: 'Weather', schema: B.SCHEMA,
    check: checkDesign, complete: B.complete,
    blank: { note: 'Plain greys, nothing preset', make: function () { return B.preset('blank'); } },
    templates: B.PRESETS.map(function (k) {
      var pd = B.PRESETS_DEF[k];
      return { key: k, name: pd.name, note: B.PACKS[pd.icons.pack].title + ' icons · ' + pd.match, make: function () { return B.preset(k); } };
    }),
    start: 'scope',
    startNote: word('The places the app shows are on the hub’s App settings page.', 'The places the app shows are on your account’s App settings page.'),
    picture: function (d, px) { return B.thumb(d, px); },
    steps: [['ground', 'Ground'], ['map', 'Map'], ['icons', 'Icons'], ['temps', 'Temperature'], ['words', 'Words'], ['layout', 'Layout']],
    parts: [
      { id: 'ground', label: 'Ground', icon: '◎', note: function (d) { return B.GROUNDS[d.plate.style] || 'Plain'; } },
      { id: 'map', label: 'Map', icon: '▦', note: function (d) { return d.map && d.map.on ? 'behind the reading' : 'none'; } },
      { id: 'icons', label: 'Icons', icon: '☀', note: function (d) { return B.PACKS[d.icons.pack].title; } },
      { id: 'temps', label: 'Temperature', icon: '▮', note: function () { return 'cold to hot'; } },
      { id: 'words', label: 'Words', icon: 'A', note: function (d) { return d.words.text; }, also: ['layout'] },
    ],
    eyes: false,
    ui: { picked: 'icon' },
    fields: fields,
    views: [
      { key: 'show', label: 'Screen', options: VIEWS },
      { key: 'weather', kind: 'select', label: 'Weather', tip: 'The condition Now shows, its icon and its effects', idle: 'Show Now to pick the weather',
        options: WEATHER.map(function (w) { return [w[0], w[1]]; }), init: 'partly', active: function (v) { return v.show === 'now'; } },
      { key: 'place', kind: 'place', home: false, label: 'Place', tip: 'The place Places shows, from the orb’s own; with none set it shows a made-up one', idle: 'Show Places to pick the place',
        active: function (v) { return v.show === 'places'; } },
    ],
    // the preview plays at 30; on the orb Now bursts at the panel's ceiling, the grid's 20
    play: { fps: 30, want: 20, on: function (v) { return v.show === 'now'; }, what: 'the weather', idle: 'The weather plays on Now' },
    paceBy: [{ key: 'show', label: 'Screen' }, { key: 'weather', with: { show: 'now' }, label: 'Weather, on Now' }],
    draw: function (x, d, c) {
      var view = c.view.show, P = partsOf(d, c);
      var sp = view === 'places' ? shownPlace(c) : null;
      drawView(x, d, P, view, view === 'now' ? lifeOf(d, c) : null, c.step === 'layout' ? c.ui.picked : null, sp ? sp.name : null, sp ? sp.offset_min : null);
    },
    pointer: pointer,
    bake: function (d, o) { return B.bake(d, o); },
    fit: function () { return { rows: B.LIVE_ROWS, cap: ROW_CAP }; },
  };

  root.WeatherEditor = { app: app, drawView: drawView, checkDesign: checkDesign };
  if (root.OrbHost) root.OrbHost.register(app);
})(typeof window !== 'undefined' ? window : globalThis);
