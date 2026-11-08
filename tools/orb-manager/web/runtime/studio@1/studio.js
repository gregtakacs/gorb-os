/* studio@1 — the editor shell every app's theme editor can stand in (theme-editor-kit-plan
 * K6; owner ruling §0.1.1: the shell takes as much as it can).
 *
 * React components on window.OrbStudio, placed from an app's dc template with x-import:
 *
 *   <x-import component-from-global-scope="OrbStudio.Frame">
 *     <x-import component-from-global-scope="OrbStudio.TopBar" dc-props="{{ studioTop }}"></x-import>
 *     <x-import component-from-global-scope="OrbStudio.Pane" side="left"> …the app's list… </x-import>
 *     <x-import component-from-global-scope="OrbStudio.Stage">
 *       <x-import component-from-global-scope="OrbStudio.Glass" dc-props="{{ studioGlass }}"><canvas …></x-import>
 *       <x-import component-from-global-scope="OrbStudio.Toolbar" dc-props="{{ studioTool }}"> …extra buttons… </x-import>
 *     </x-import>
 *     <x-import component-from-global-scope="OrbStudio.Pane" side="right"> …the inspector… </x-import>
 *     <x-import component-from-global-scope="OrbStudio.SendModal" dc-props="{{ studioSend }}"></x-import>
 *   </x-import>
 *
 * The shell owns the frame, the top bar (back, crumb, name, steps, undo and redo, the fit
 * dot, the primary action), the stage (the scrolling glass, its zoom tag, the status
 * toast, the toolbar of time, view, zoom and render), the calibration panel (a ruler and a
 * bank card), the send and duplicate modals, and the Render panel (the fit badge, Send and
 * the downloads). Builders below turn the theme@1 services' answers into these props, so an
 * app passes data and never markup. Its layout classes are studio.css's (st-*), in ui@1's
 * tokens. Plain React.createElement: no JSX, no Babel.
 */
(function (root) {
  'use strict';
  var S = root.OrbStudio = root.OrbStudio || {};
  var h = function () { return root.React.createElement.apply(root.React, arguments); };
  var cx = function () { return Array.prototype.filter.call(arguments, Boolean).join(' '); };
  var list = function (a) { return Array.isArray(a) ? a : []; };

  // a segmented control: [{label, tip, cls, pick}]
  function Seg(p) {
    return h('div', { className: 'ui-seg' }, list(p.opts).map(function (o, i) {
      return h('button', { key: i, className: o.cls || '', onClick: o.pick, title: o.tip || o.title || '' }, o.label);
    }));
  }
  S.Seg = Seg;

  S.Frame = function (p) { return h('div', { className: 'st' }, p.children); };

  /* back: fn or null · crumb · name: {value, placeholder, title, onChange} · steps: [{label, tip,
   * cls, pick}] · undo / redo: {on, tip, op} · extra: [{label, title, on}] · fit: {dot, text, tip}
   * · primary: {label, title, on} */
  S.TopBar = function (p) {
    var nm = p.name || null, fit = p.fit || null, pr = p.primary || null;
    return h('header', { className: 'st-top' },
      h('div', { className: 'st-crumb' },
        p.back ? h('button', { className: 'ui-ghost', onClick: p.back, title: p.backTip || 'Back' }, '←') : null,
        h('span', { className: 'ui-mut' }, p.crumb || ''), h('span', { className: 'ui-mut' }, '/'),
        nm ? h('input', { className: 'st-name', type: 'text', value: nm.value, placeholder: nm.placeholder, title: nm.title, onChange: nm.onChange }) : null),
      h('nav', { className: 'st-steps' }, list(p.steps).map(function (t, i) {
        return h('button', { key: i, className: t.cls || '', onClick: t.pick, title: t.tip || '' }, t.label);
      })),
      p.undo ? h('button', { className: 'ui-ghost st-sm', onClick: p.undo.on, title: p.undo.tip, style: { opacity: p.undo.op } }, '↶') : null,
      p.redo ? h('button', { className: 'ui-ghost st-sm', onClick: p.redo.on, title: p.redo.tip, style: { opacity: p.redo.op } }, '↷') : null,
      list(p.extra).map(function (x, i) { return h('button', { key: 'x' + i, className: 'ui-ghost st-sm', onClick: x.on, title: x.title || '' }, x.label); }),
      fit ? h('div', { className: 'st-fit', title: fit.tip || '' }, h('span', { className: 'st-dot', style: { background: fit.dot } }), fit.text) : null,
      pr ? h('button', { className: 'ui-primary', onClick: pr.on, title: pr.title || '' }, pr.label) : null);
  };

  S.Pane = function (p) { return h('aside', { className: cx('st-side', p.side === 'right' ? 'st-right' : 'st-left') }, p.children); };
  S.Stage = function (p) { return h('main', { className: 'st-stage' }, p.children); };

  /* the round glass the app's canvas sits in: size (css px), round (border-radius), scrollRef
   * (a ref the app centres a magnified scroll with), onPointerDown; then the zoom tag and the
   * status toast over it. onFit(px): the size Fit should draw at, the pane's smaller side less
   * a margin for the case ring (never under 160), reported when it moves by 2 px or more. The
   * glass measures itself after every render: one rect read, so a stylesheet that lands after
   * the first paint, or a window whose resize events are throttled, still fits. */
  S.FIT_MARGIN = 40; S.FIT_MIN = 160;
  S.fitOf = function (w, h) { return Math.max(S.FIT_MIN, Math.floor(Math.min(w, h) - S.FIT_MARGIN)); };
  S.Glass = function (p) {
    var R = root.React, own = R.useRef(null), last = R.useRef(0);
    R.useLayoutEffect(function () {
      var el = own.current;
      if (!el || !p.onFit || typeof el.getBoundingClientRect !== 'function') return;
      var r = el.getBoundingClientRect(), px = S.fitOf(r.width, r.height);
      if (r.width > 0 && Math.abs(px - last.current) >= 2) { last.current = px; p.onFit(px); }
    });
    // a view that changed size opens centred: a magnified face scrolls from its middle
    var sized = R.useRef('');
    R.useLayoutEffect(function () {
      var el = own.current, key = p.size + ':' + (p.zoomTag || '');
      if (!el || key === sized.current || typeof el.scrollTo !== 'function') return;
      sized.current = key;
      var cx = (el.scrollWidth - el.clientWidth) / 2, cy = (el.scrollHeight - el.clientHeight) / 2;
      if (cx > 0 || cy > 0) el.scrollTo(cx, cy);
    });
    var setRef = function (el) { own.current = el; if (p.scrollRef) p.scrollRef.current = el; };
    return h(root.React.Fragment, null,
      h('div', { className: 'st-scroll', ref: setRef, onPointerDown: p.onPointerDown },
        h('div', { className: 'st-glass', style: { width: p.size + 'px', height: p.size + 'px', borderRadius: p.round || '50%' } }, p.children)),
      p.zoomTag ? h('div', { className: 'st-ztag' }, p.zoomTag) : null,
      p.status ? h('div', { className: 'st-toast', title: p.status }, p.status) : null);
  };

  /* play: {icon, label, on} · time / view / zoom / render: segmented options (zoom only while
   * magnifying) · wipe: {value, on} while splitting · screen: {open, on} (the screen panel's
   * button, S.viewer's toolScreen) · children: the app's own tool buttons */
  S.Toolbar = function (p) {
    return h('div', { className: 'st-tool' },
      p.play ? h('button', { className: 'st-play', onClick: p.play.on, title: p.play.label }, p.play.icon) : null,
      p.time ? h(Seg, { opts: p.time }) : null,
      p.view ? h('span', { className: 'ui-mut st-lab' }, 'View') : null,
      p.view ? h(Seg, { opts: p.view }) : null,
      p.zoom ? h(Seg, { opts: p.zoom }) : null,
      p.render ? h('span', { className: 'ui-mut st-lab' }, 'Render') : null,
      p.render ? h(Seg, { opts: p.render }) : null,
      p.wipe ? h('input', { type: 'range', min: 0, max: 466, step: 1, value: p.wipe.value, onChange: p.wipe.on, title: 'Where the split sits: Panel on the left, Design on the right', style: { width: '120px' } }) : null,
      p.children,
      p.screen ? h('button', { className: cx('ui-ghost', p.screen.open ? 'on' : ''), onClick: p.screen.on, title: 'This screen: the true-size ruler and bank card, set once for every app on this browser' }, '⚙') : null);
  };

  /* True size's two instruments (OrbTheme.screen): the ruler, its factor and what a real ruler
   * reads against it; the bank card outline and how wide it really measures. text: the view's
   * caption. show: false hides the instruments (they belong to True size only). */
  S.Calibrate = function (p) {
    var r = p.ruler || {}, c = p.cal || {}, card = p.card || null;
    return h('div', { className: 'st-cal' },
      p.text ? h('div', { className: 'ui-mono ui-mut st-cap' }, p.text) : null,
      p.show ? h(root.React.Fragment, null,
        h('div', { className: 'st-ruler', style: { width: r.px + 'px' } },
          list(r.ticks).map(function (t, i) { return h('div', { key: 't' + i, className: 'st-tick', style: { left: t.left + '%', opacity: t.op, height: t.h + 'px' } }); }),
          list(r.ticks).map(function (t, i) { return t.lab ? h('div', { key: 'l' + i, className: 'st-tlab', style: { left: t.left + '%' } }, t.lab) : null; })),
        h('div', { className: 'st-row' },
          h('span', { className: 'ui-mut', title: 'Hold a real ruler against the bar: True size is only true once this matches' }, 'Screen ruler'),
          h('button', { onClick: c.down, title: 'Nudge the calibration down' }, '−'),
          h('input', { type: 'range', min: c.min, max: c.max, step: c.step, value: c.value, onChange: c.onChange, style: { width: '150px' } }),
          h('button', { onClick: c.up, title: 'Nudge the calibration up' }, '+'),
          h('span', { className: 'ui-mono' }, c.text)),
        h('div', { className: 'st-row' },
          h('span', { className: 'ui-mut' }, 'It measures'),
          h('input', { type: 'text', onChange: c.solve, placeholder: 'mm', title: 'Read the bar against a real ruler and type what it measures: this solves the calibration outright', style: { width: '56px', textAlign: 'right' } }),
          h('span', { className: 'ui-mut' }, 'mm'),
          h('button', { onClick: c.reset, title: 'Back to the 96 px/in standard' }, 'Reset')),
        card ? h('div', { className: 'st-card', title: 'Hold a bank card over the outline: if the edges match, True size is exact', style: { width: card.w + 'px', height: card.h + 'px', borderRadius: card.r + 'px' } }, 'bank card') : null,
        card ? h('div', { className: 'st-row' },
          h('span', { className: 'ui-mut' }, 'Or the card outline measures'),
          h('input', { type: 'text', onChange: card.solve, placeholder: 'mm', title: 'Measure the outline above with a real card or ruler: a card is 85.6 mm wide', style: { width: '56px', textAlign: 'right' } }),
          h('span', { className: 'ui-mut' }, 'mm wide (a card is 85.6)')) : null) : null,
      p.children);
  };

  /* ---- the screen (owner, §12.5: set once for every app, the clock's way). One controller
   * over theme@1's screen and view: the view and zoom, Fit's measured size, the true-size
   * calibration (one per browser, shared by every app's editor) and whether this page set or
   * read one, the two instruments' readings and what they say. The app keeps the numbers in
   * its own state, under its own names, and hands the controller a getter and a setter:
   *   host.get() -> {view, zoom, fitPx, cal, open}   host.set(patch)   host.say(text)
   * Its answers are the shell's props: glass(), tool() (view and zoom), toolScreen() (the ⚙
   * button) and panel() (ScreenPanel's). init() is the state an editor opens with. */
  S.viewer = function (host) {
    var T = root.OrbTheme, scr = T.screen, loaded = scr.load(), calSet = loaded.set, scrollRef = { current: null };
    var say = function (t) { if (host.say) host.say(t); };
    var api = {
      scrollRef: scrollRef,
      init: function (view) { return { view: view || 'fit', zoom: 2, fitPx: 466, cal: loaded.cal, open: false }; },
      cal: function () { var c = +host.get().cal; return c >= scr.MIN && c <= scr.MAX ? c : 1; },
      calibrated: function () { return calSet; },
      setCal: function (v) { calSet = true; host.set({ cal: scr.save(v) }); },
      view: function () { var g = host.get(); return T.view({ view: g.view, zoom: g.zoom, fitPx: g.fitPx, cal: api.cal(), calibrated: calSet }); },
      setView: function (v) { var g = host.get(); host.set(v === 'zoom' && !(+g.zoom >= 1) ? { view: v, zoom: 2 } : { view: v }); },
      setZoom: function (z) { host.set({ view: 'zoom', zoom: Math.min(4, Math.max(1, +z || 1)) }); },
      // what a real ruler measures against the on-screen bar solves the factor outright
      solveRuler: function (mm) {
        if (mm == null || String(mm).trim() === '') return;
        var c = scr.fromRuler(api.cal(), mm);
        if (c == null) { say('Type what the ruler really measures, in mm — between 5 and 300'); return; }
        api.setCal(c); say('Ruler matched at ' + (+mm) + ' mm · true size now draws the face ' + scr.PANEL_MM + ' mm across on this screen');
      },
      solveCard: function (mm) {
        if (mm == null || String(mm).trim() === '') return;
        var c = scr.fromCard(api.cal(), mm);
        if (c == null) { say('Type how wide the card outline really measures, in mm — between 20 and 400'); return; }
        api.setCal(c); say('Card matched at ' + (+mm) + ' mm · true size now draws the face ' + scr.PANEL_MM + ' mm across on this screen');
      },
      reset: function () { api.setCal(1); say('Screen calibration reset to the 96 px/in standard'); },
      onFit: function (px) { if (Math.abs(px - (+host.get().fitPx || 0)) >= 2) host.set({ fitPx: px }); },
      glass: function () { var V = api.view(); return { size: V.css, scrollRef: scrollRef, zoomTag: V.tag, onFit: api.onFit }; },
      tool: function () {
        var g = host.get(), V = api.view();
        return { view: T.VIEWS.map(function (v) { return { label: v[1], tip: v[2], cls: v[0] === g.view ? 'on' : '', pick: function () { api.setView(v[0]); } }; }),
          zoom: g.view === 'zoom' ? T.ZOOMS.map(function (z) { return { label: z + '×', cls: z === V.k ? 'on' : '', pick: function () { api.setZoom(z); } }; }) : null };
      },
      toolScreen: function () { var g = host.get(); return { open: !!g.open, on: function () { host.set({ open: !host.get().open }); } }; },
      calProps: function () {
        var c = api.cal(), st = scr.step(c);
        return S.calProps(api.view(), { min: scr.MIN, max: scr.MAX, step: st, value: c,
          onChange: function (e) { api.setCal(+e.target.value); }, up: function () { api.setCal(c + st); }, down: function () { api.setCal(c - st); },
          solve: function (e) { api.solveRuler(e.target.value); }, solveCard: function (e) { api.solveCard(e.target.value); }, reset: api.reset });
      },
      panel: function () { var g = host.get(); return { open: !!g.open, cal: api.calProps(), close: function () { host.set({ open: false }); } }; },
    };
    return api;
  };
  /* the screen panel over the stage: the view's caption, and in True size the ruler and the
   * bank card. children: an app's own diagnostics under them (the clock's frame stepping) */
  S.ScreenPanel = function (p) {
    if (!p.open) return null;
    return h('div', { className: 'st-diag' },
      h('div', { className: 'st-diag-h' }, h('span', { className: 'ui-label' }, 'This screen'),
        p.close ? h('button', { className: 'ui-ghost st-sm', onClick: p.close, title: 'Close' }, '✕') : null),
      h(S.Calibrate, p.cal || {}),
      p.cal && !p.cal.show ? h('div', { className: 'ui-mut st-cap' }, 'Pick True size to calibrate: the ruler and the bank card show there, and the calibration holds for every app’s editor on this browser.') : null,
      p.children);
  };

  /* ---- the start sheet (owner, §12.5: the clock's way for every app): over the stage, the
   * app's starting points as pictures, the library under them, and Continue. Picking a
   * tile chooses it; only Continue moves on.
   *   title · tiles: [{key, name, short, tip, on, cls, draw(canvas)}] · scratch: {label, short, tip, on}
   *   · library: {openFile, accept, rows (libraryRows), empty}. Saving is the Render step's
   *     (RenderPanel's save), under the theme's name: the sheet only opens. A hosted editor
   *     (host@1) passes no openFile: its file comes in through importFile, below the library.
   *   · importFile: {accept, on}: Import a file, which puts it in the library and opens it
   *   · foot: [{label, on, primary, tip}] */
  function Tile(p) {
    var R = root.React, ref = R.useRef(null), t = p.t;
    R.useEffect(function () { if (ref.current && t.draw) t.draw(ref.current); });
    return h('div', { className: cx('st-gc', t.cls), onClick: t.on, title: t.tip || '' },
      h('canvas', { ref: ref, width: 192, height: 192 }), h('b', null, t.name), h('span', null, t.short || ''));
  }
  S.StartSheet = function (p) {
    var L = p.library || null, I = p.importFile || null, fileRef = root.React.useRef(null), impRef = root.React.useRef(null);
    return h('div', { className: 'st-sheet' },
      h('div', { className: 'st-sheet-in' },
        h('h2', null, p.title || 'Start a theme'),
        h('div', { className: 'st-gal' },
          p.scratch ? h('button', { className: cx('st-gc', p.scratch.cls), onClick: p.scratch.on, title: p.scratch.tip || '' }, h('span', { className: 'st-blank' }, '＋'), h('b', null, p.scratch.label), h('span', null, p.scratch.short || '')) : null,
          list(p.tiles).map(function (t) { return h(Tile, { key: t.key, t: t }); })),
        L ? h(root.React.Fragment, null,
          h('h2', { style: { marginTop: '18px' } }, 'Library'),
          L.openFile ? h('div', { className: 'st-lib-bar' },
            h('button', { className: 'ui-ghost st-sm', onClick: function () { fileRef.current && fileRef.current.click(); }, title: 'Open a theme (.orbtheme) or a design file (.orbdesign) in the editor' }, 'Open a file…'),
            h('input', { ref: fileRef, type: 'file', accept: L.accept || '.orbtheme,.orbdesign', style: { display: 'none' }, onChange: L.openFile })) : null,
          list(L.rows).length ? null : h('div', { className: 'ui-mut st-lib-empty' }, L.empty || 'Nothing in the library yet. Save to library on the Render step keeps a design on the hub as a draft; Send to orb bakes it into a theme under the same name.'),
          list(L.rows).map(function (r) {
            return h('div', { key: r.name, className: 'st-lib-row' },
              r.thumb ? h('img', { className: 'st-lib-pv', src: r.thumb, alt: '' }) : null,
              h('b', null, r.name), h('span', { className: 'ui-mut', title: r.tagTip }, r.tag), h('span', { className: 'ui-mut ui-mono' }, r.meta),
              h('button', { className: 'ui-ghost st-sm', onClick: r.load, title: r.loadTip }, 'Open'),
              r.changed ? h('button', { className: 'ui-ghost st-sm', onClick: r.discard, title: 'Drop the unsent changes and keep the theme as it was baked' }, 'Discard changes') : null,
              h('button', { className: 'ui-ghost st-sm', onClick: r.del, title: r.delTip }, r.delLabel));
          })) : null,
        I ? h(root.React.Fragment, null,
          h('h2', { style: { marginTop: '18px' } }, 'Import'),
          h('div', { className: 'st-lib-bar' },
            h('button', { className: 'ui-ghost st-sm', onClick: function () { impRef.current && impRef.current.click(); }, title: 'Add a theme (.orbtheme) or a design file (.orbdesign) to the library, then open it' }, 'Import a file…'),
            h('span', { className: 'ui-mut' }, 'An .orbtheme or a design file: it goes into the library, then opens here.'),
            h('input', { ref: impRef, type: 'file', accept: I.accept || '.orbtheme,.orbdesign', style: { display: 'none' }, onChange: I.on }))) : null),
      h('div', { className: 'st-sheet-foot' }, list(p.foot).map(function (f, i) {
        return h('button', { key: i, className: f.primary ? 'ui-primary' : 'ui-ghost', onClick: f.on, title: f.tip || '' }, f.label);
      })));
  };
  /* the library's rows for StartSheet, from a session: o.version (the editor's), o.meta(entry),
   * o.open(name), o.forget(name, what), o.armed (the name whose Delete was pressed once), o.arm(name) */
  S.libraryRows = function (lib, o) {
    return list(lib).map(function (m) {
      var th = m.theme || {}, armed = o.armed === m.name, v = o.version || 'dev';
      var older = th.made_with && v !== 'dev' && th.made_with.split('+')[0] !== v.split('+')[0] ? th.made_with : '';
      var tag = th.stale ? 'theme · needs a rebake' : m.state === 'changed' ? 'theme · unsent changes' : m.state === 'theme' ? 'theme' : 'draft';
      var tb = typeof m.thumb === 'string' && /^data:image\/(jpeg|png);base64,/.test(m.thumb) ? m.thumb : null;
      return { name: m.name, thumb: tb, tag: tag + (older ? ' · made with ' + older : ''),
        tagTip: th.stale ? 'An orb on the current firmware would refuse this bake (' + th.stale + '). Rebake all on the Themes tab rebakes it from its design.' : m.state === 'draft' ? 'Saved, never baked: Send to orb makes it a theme' : m.state === 'changed' ? 'A baked theme with changes saved beside it: Open shows the changes, Send to orb bakes them in' : 'Baked: on the hub shelf, ready to install',
        meta: (o.meta ? o.meta(m) : '') + new Date(+m.at || 0).toLocaleDateString(),
        load: function () { o.open(m.name); }, loadTip: m.state === 'changed' ? 'Open it with its unsent changes' : 'Open it in the editor',
        changed: m.state === 'changed', discard: function () { o.forget(m.name, 'draft'); },
        delLabel: armed ? 'Press again' : 'Delete', delTip: 'Take it out of the hub’s library, baked theme and all (asks twice). No orb is touched.',
        del: function () { if (armed) { o.arm(null); o.forget(m.name, 'all'); } else o.arm(m.name); } };
    });
  };

  /* line: the session's opening line (session.opening), null when nothing is on its way in.
   * Covers the whole editor, the start sheet's presets first, until the open lands or fails. */
  S.OpeningModal = function (p) {
    if (!p.line) return null;
    return h('div', { className: 'st-modal', style: { zIndex: 70, cursor: 'progress' } },
      h('div', null,
        h('div', { className: 'st-mtitle' }, 'Opening a design'),
        h('div', { className: 'st-mbody' }, p.line),
        h('div', { className: 'st-hint' }, 'A theme’s design comes with its art, and from an orb’s card that can take a while. The editor opens on the theme when it arrives.')));
  };
  // open, line, hint, noCancel, cancelLabel, onCancel
  S.SendModal = function (p) {
    if (!p.open) return null;
    return h('div', { className: 'st-modal', style: { zIndex: 60 } },
      h('div', null,
        h('div', { className: 'st-mtitle' }, 'Sending to the orb'),
        h('div', { className: 'st-mbody' }, p.line),
        p.hint ? h('div', { className: 'st-hint' }, p.hint) : null,
        h('div', { className: 'st-mfoot' }, h('button', { onClick: p.onCancel, disabled: !!p.noCancel }, p.cancelLabel || 'Cancel'))));
  };
  // open, name, stem, note, onReplace, onCancel: the bake is held; Replace ships these bytes
  S.DupModal = function (p) {
    if (!p.open) return null;
    return h('div', { className: 'st-modal', onClick: p.onCancel },
      h('div', { onClick: function (e) { e.stopPropagation(); } },
        h('div', { className: 'st-mtitle' }, 'That name is already on the card'),
        h('div', { className: 'st-mbody' }, 'A different theme already lives under ', h('span', { className: 'ui-mono', style: { color: 'var(--fg)' } }, p.name),
          h('span', { className: 'ui-mono' }, ' (' + (p.stem || '') + ')'), '. ' + (p.note || '')),
        h('div', { className: 'st-hint' }, 'Replace overwrites that theme with this bake: these exact bytes, no re-bake.'),
        h('div', { className: 'st-mfoot' },
          h('button', { onClick: p.onCancel }, 'Cancel'),
          h('button', { className: 'ui-primary', onClick: p.onReplace }, 'Replace on the card'))));
  };

  /* badge: the OrbTheme.fit verdict {text, note, tip, color, border, background} · send: {note,
   * tip, on} under a hub · save: {note, tip, on} under a hub, the one place a design is saved
   * to the library · downloads: [{label, note, title, on}] · note: a warning line */
  S.RenderPanel = function (p) {
    var b = p.badge || null;
    return h(root.React.Fragment, null,
      b ? h('div', { className: 'st-badge', title: b.tip || '', style: { borderColor: b.border, background: b.background } }, h('b', { style: { color: b.color } }, b.text), h('span', null, b.note)) : null,
      p.save ? h('button', { className: 'st-big', onClick: p.save.on, title: p.save.tip || '' }, h('b', null, 'Save to library'), h('span', null, p.save.note)) : null,
      p.send ? h('button', { className: 'st-big ui-primary', onClick: p.send.on, title: p.send.tip || '' }, h('b', null, 'Send to orb'), h('span', null, p.send.note)) : null,
      list(p.downloads).map(function (d, i) { return h('button', { key: i, className: 'st-big', onClick: d.on, title: d.title || '' }, h('b', null, d.label), h('span', null, d.note)); }),
      p.note ? h('div', { className: 'st-hint ui-mono st-warn' }, p.note) : null);
  };

  /* ---- the inspector's fields (K7): a kit of row builders and the component that draws them.
   * A field is one plain object; the kit's builders are the clock editor's own, moved, with
   * the two things that were the clock's made options: where a colour's typed draft is kept
   * (scope), and which choice of a select means "follow the source" (inherit). */
  S.INKS = [['#f6f3ec', 'White'], ['#1a140c', 'Black'], ['#c9a25a', 'Gold'], ['#d7dbe0', 'Silver'], ['#cfe8b0', 'Lume green'], ['#9fd8ff', 'Lume blue'], ['#c8322a', 'Red'], ['#ff7a1a', 'Orange'], ['#2b59c3', 'Blue'], ['#ff2bd6', 'Magenta'], ['#22e6ff', 'Cyan'], ['#b6ff2e', 'Acid']];
  /* the colour picker's palettes (owner, §12.5: one picker, served by the hub, the same in
   * every editor). Classic: the inks and period colours a dial or chart is printed in; Vivid:
   * the saturated ones an OLED shows off; Any: the system picker. Which one shows is one
   * choice per browser, made on any colour row, kept for every row and every app. */
  S.PALETTES = {
    classic: S.INKS.concat([['#1d2b4a', 'Navy'], ['#5b6b3a', 'Olive'], ['#6e1f2b', 'Burgundy'], ['#efe6cf', 'Cream'], ['#5d6b78', 'Slate'], ['#a0703c', 'Bronze']]),
    vivid: [['#ffffff', 'White'], ['#ff2d2d', 'Red'], ['#ff8a00', 'Orange'], ['#ffd400', 'Yellow'], ['#b6ff2e', 'Lime'], ['#39ff14', 'Phosphor'],
      ['#00e5a0', 'Mint'], ['#22e6ff', 'Cyan'], ['#2f7bff', 'Blue'], ['#7a3cff', 'Violet'], ['#ff2bd6', 'Magenta'], ['#ff4f8b', 'Pink']],
  };
  S.PALETTE_TABS = [['classic', 'Classic'], ['vivid', 'Vivid'], ['any', 'Any']];
  S.PALETTE_KEY = 'orb.colour.palette';
  S.palette = (function () { try { var v = root.localStorage.getItem(S.PALETTE_KEY); return v === 'vivid' || v === 'any' ? v : 'classic'; } catch (e) { return 'classic'; } })();
  S.setPalette = function (v) { S.palette = v; try { root.localStorage.setItem(S.PALETTE_KEY, v); } catch (e) { /* this page only */ } };
  // a typed colour: #rrggbb, #rgb, or r, g, b; null when it is none of them
  S.parseColor = function (t) {
    t = String(t || '').trim().toLowerCase(); var m;
    if ((m = t.match(/^#?([0-9a-f]{6})$/))) return '#' + m[1];
    if ((m = t.match(/^#?([0-9a-f])([0-9a-f])([0-9a-f])$/))) return '#' + m[1] + m[1] + m[2] + m[2] + m[3] + m[3];
    if ((m = t.match(/^(?:rgb\s*\()?\s*(\d{1,3})[\s,]+(\d{1,3})[\s,]+(\d{1,3})\s*\)?$/))) { var v = [m[1], m[2], m[3]].map(Number); if (v.every(function (x) { return x <= 255; })) return '#' + v.map(function (x) { return x.toString(16).padStart(2, '0'); }).join(''); }
    return null;
  };
  /* o: drafts (an object the app keeps, so a half-typed hex survives a render), refresh() (redraw
   * after a draft changes), scope() (the draft key's prefix, e.g. the selected part), inherit:
   * [{re, src}] (a select's option whose label matches re is "follow src"; first rule first) */
  S.fieldKit = function (o) {
    o = o || {};
    var drafts = o.drafts || {}, refresh = o.refresh || function () {}, scopeOf = o.scope || function () { return ''; };
    var inherit = o.inherit || [{ re: /^Default\b/, src: 'the defaults' }];
    var on = function (b) { return { cls: b ? 'on' : '', bg: b ? '#1f3a5f' : '#1c2330', bd: b ? '#58a6ff' : '#2b3540', fg: b ? '#e6edf3' : '#e6edf3' }; };
    var mark = function (list, v) { return list.map(function (x) { return Object.assign({}, x, { sel: String(x.v) === String(v) }); }); };
    var mk = function (f) {
      return Object.assign({ label: '', isHead: false, isRow: true, isRange: false, isSelect: false, isChips: false, isNote: false, isColor: false, isText: false, isFile: false, min: 0, max: 1, step: 1, value: '', on: null, opts: [], valText: '', note: '', anchor: null,
        hasLink: false, linkText: '', linkTip: '', linkFg: '', linkCursor: 'default', relink: null }, f);
    };
    /* D43 §2.3: a property that can follow a source says which it is doing. Linked: it follows,
     * and changing it here unlinks it. Overridden: one click re-links it. */
    var linkOf = function (opts, value, onv) {
      var inh = null, src = '';
      for (var i = 0; i < inherit.length && !inh; i++) {
        inh = opts.find(function (x) { return inherit[i].re.test(String(x.label || '')); }) || null;
        if (inh) src = inherit[i].src;
      }
      if (!inh) return {};
      var linked = String(value) === String(inh.v);
      return { hasLink: true, linkText: linked ? '⛓ ' + src : '↺ re-link', linkFg: linked ? '#8b98a5' : '#58a6ff', linkCursor: linked ? 'default' : 'pointer',
        linkTip: linked ? 'Follows ' + src + '; choose another value to set it here' : 'Set here, not from ' + src + ' — click to follow ' + src + ' again',
        relink: linked ? function () {} : function () { onv(inh.v); } };
    };
    // anchor: the pane's scroll destination for a selection
    // fold: {open, toggle} makes the section collapsible; a closed one hides its rows
    var H = function (label, anchor, fold) { return mk({ label: label, isHead: true, isRow: false, anchor: anchor, fold: fold || null }); };
    // a slider snaps to 0 within 1.5% of its span and, on a fine one, to a whole number within 0.6%
    var snapV = function (v, min, max, step) {
      var sp = Math.abs(max - min) || 1;
      if (min <= 0 && max >= 0 && Math.abs(v) <= sp * 0.015) return 0;
      if (step < 1 && sp > 2 && Math.abs(v - Math.round(v)) <= sp * 0.006) return Math.round(v);
      return v;
    };
    var Rg = function (label, value, min, max, step, onv, valText) {
      return mk({ label: label, isRange: true, min: min, max: max, step: step, value: value, valText: valText,
        on: function (e) { onv(snapV(+e.target.value, min, max, step)); },
        onNum: function (e) { var v = e.target.value; if (v === '' || !isFinite(+v)) return; onv(Math.max(min, Math.min(max, +v))); } });
    };
    var Sl = function (label, value, opts, onv) { return mk(Object.assign({ label: label, isSelect: true, value: value, opts: mark(opts, value), on: function (e) { onv(e.target.value); } }, linkOf(opts, value, onv))); };
    // a choice: pills for up to three, a dropdown past that (four pills wrap into a ragged row)
    var Ch = function (label, value, opts, onv) {
      // each option keeps its pick, as a pill's has, so code that drives the panel reads it alike
      var picks = opts.map(function (x) { return Object.assign({}, x, { pick: function () { onv(x.v); } }); });
      if (opts.length > 3) return mk({ label: label, isSelect: true, value: value == null ? '' : value,
        opts: mark(String(value) === '' || !opts.some(function (x) { return String(x.v) === String(value); }) ? [{ v: '', label: '—' }].concat(picks) : picks, value),
        on: function (e) { if (e.target.value !== '') onv(e.target.value); } });
      return mk({ label: label, isChips: true, opts: opts.map(function (x) { return Object.assign({}, on(String(x.v) === String(value)), { label: x.label, pick: function () { onv(x.v); } }); }) });
    };
    var Nt = function (label, note) { return mk({ label: label, isNote: true, note: note }); };
    // swatches: [{k, label, bg}]; extra: leading options; vOf(option): the value an extra option
    // stands for, which the inherit link reads (default: its v, else its label)
    var Sw = function (label, value, swatches, onv, extra, vOf) {
      var named = swatches.find(function (w) { return w.k === value; });
      return mk(Object.assign({ label: label, isSwatch: true, valText: named ? named.label : '',
        opts: (extra || []).concat(swatches.map(function (w) { return { label: w.label, text: '', bg: w.bg, bd: w.k === value ? '#e6edf3' : '#232c36', pick: function () { onv(w.k); } }; })) },
        linkOf((extra || []).map(function (x) { return { v: vOf ? vOf(x) : x.v != null ? x.v : x.label, label: x.label }; }), value, onv)));
    };
    var hexRGB = function (v) { var n = parseInt(v.slice(1), 16); return (n >> 16) + ', ' + ((n >> 8) & 255) + ', ' + (n & 255); };
    /* a colour: the preset's own, a palette (Classic, Vivid, or Any's picker), and a hex box
     * that keeps a half-typed draft per (scope, label). X: {none: offer None, noAuto: no Auto,
     * preset: the value the design's preset gives this colour, offered first} */
    var Co = function (label, value, onv, autoText, scope, X) {
      X = X || {};
      var key = (scope || scopeOf()) + ':' + label, hex = value && /^#[0-9a-f]{6}$/i.test(value) ? value : null, dr = drafts[key];
      var pal = S.palette, ink = pal === 'any' ? [] : S.PALETTES[pal] || S.INKS, pre = X.preset && /^#[0-9a-f]{6}$/i.test(X.preset) ? X.preset : null;
      var same = function (a, b) { return !!a && !!b && String(a).toLowerCase() === String(b).toLowerCase(); };
      return mk({ label: label, isColor: true, value: hex || '#ffffff', on: function (e) { delete drafts[key]; onv(e.target.value); },
        picker: pal === 'any',
        palTabs: S.PALETTE_TABS.map(function (t) { return { label: t[1], on: t[0] === pal, pick: function () { S.setPalette(t[0]); refresh(); } }; }),
        hexText: dr != null ? dr : hex || '', hexTitle: hex ? 'rgb ' + hexRGB(hex) : 'Type a hex code or r, g, b',
        onHex: function (e) { var v = e.target.value, p = S.parseColor(v); if (p) { delete drafts[key]; onv(p); } else { drafts[key] = v; refresh(); } },
        onHexBlur: function () { if (drafts[key] != null) { delete drafts[key]; refresh(); } },
        valText: value === 'none' ? 'None' : !value || value === 'auto' ? (X.noAuto ? 'None' : autoText || 'Auto') : value,
        pre: pre ? { label: 'Back to the preset’s ' + pre, text: 'P', cls: 'st-sw-pre', bg: pre, bd: same(pre, value) ? '#e6edf3' : '#2b3540', pick: function () { delete drafts[key]; onv(pre); } } : null,
        cur: { label: 'Choose a colour', text: value === 'none' ? '∅' : !hex ? 'A' : '', cls: 'st-sw-cur', bg: hex || '#1c2330', bd: '#8b98a5' },
        opts: [].concat(X.none ? [{ label: 'None', text: '∅', bg: '#1c2330', bd: value === 'none' ? '#e6edf3' : '#2b3540', pick: function () { onv('none'); } }] : [])
          .concat(X.noAuto ? [] : [{ label: autoText || 'Auto', text: 'A', bg: '#1c2330', bd: !value || value === 'auto' ? '#e6edf3' : '#2b3540', pick: function () { onv('auto'); } }])
          .concat(ink.map(function (c) { return { label: c[1], text: '', bg: c[0], bd: same(c[0], value) ? '#e6edf3' : '#232c36', pick: function () { onv(c[0]); } }; })) });
    };
    // a sample: `text` as it will read, style {color, background, fontFamily, fontSize, border}
    var Sa = function (label, text, style, note) { return mk({ label: label, isSample: true, value: text, style: style || {}, valText: note || '' }); };
    var Tx = function (label, value, onv) { return mk({ label: label, isText: true, value: value == null ? '' : String(value), on: function (e) { onv(e.target.value); } }); };
    var Fi = function (label, onv, valText, accept) { return mk({ label: label, isFile: true, on: onv, valText: valText || '', accept: accept || 'image/png,image/svg+xml,image/jpeg,image/webp' }); };
    // a canvas the app draws into (a ref callback): a thumbnail to click or drag on
    var Cv = function (label, ref, size, css, onDown, onMove, tip) { return mk({ label: label, isCanvas: true, ref: ref, w: size, h: size, css: css || size, onDown: onDown, onMove: onMove, tip: tip || '' }); };
    // a reading: a value the author watches (a rate, a fit, a cost), always printed; its why rides as the hover
    var Rd = function (label, value, why) { return mk({ label: label, valText: value == null ? '' : String(value), tip: why || '' }); };
    // a run of rows folded to its first `keep`, with a More / Less chip; open and toggle are the app's
    var fold = function (rows, keep, open, toggle) {
      if (rows.length <= keep) return rows;
      var hid = rows.length - keep;
      var chip = Object.assign({}, rows[0], { label: '', valText: '', isHead: false, isRow: true, anchor: null, isRange: false, isSelect: false, isSwatch: false, isColor: false, isText: false, isFile: false, isNote: false, hasLink: false, isChips: true,
        opts: [{ label: open ? 'Less' : 'More (' + hid + ')', bg: '#1c2330', bd: '#2b3540', fg: '#8b98a5', pick: toggle }] });
      return (open ? rows : rows.slice(0, keep)).concat([chip]);
    };
    return { on: on, mark: mark, mk: mk, linkOf: linkOf, H: H, snapV: snapV, Rg: Rg, Sl: Sl, Ch: Ch, Nt: Nt, Sw: Sw, Co: Co, Sa: Sa, Tx: Tx, Fi: Fi, Cv: Cv, Rd: Rd, fold: fold };
  };

  // one field row: its label (with the inherit link), its control, and its value
  function FieldRow(p) {
    var openS = root.React.useState(false), open = openS[0];
    var f = p.f, sw = function (x, i) { return h('button', { key: i, className: cx('st-sw', x.cls), onClick: x.pick, title: x.label, style: { background: x.bg, borderColor: x.bd } }, x.text); };
    /* a slider's reading: the number box already says the number (in px, degrees, a multiple
     * or a percentage of it), so only words ride, under the slider; nothing repeats it in a
     * column of its own, and every slider in a pane is the same width */
    var said = String(f.valText == null ? '' : f.valText), bare = said.replace(/\s*(px|°|×|x)$/, '');
    var pct = /^-?[\d.]+\s*%$/.test(said) && Math.abs(parseFloat(said) / 100 - +f.value) < 0.006;
    var echo = f.isRange && (said === '' || pct || (isFinite(+bare) && Math.abs(+bare - +f.value) < 1e-9));
    var capUnder = f.isRange && !echo ? said : '';
    var ctl =
      f.isRange ? h('div', { className: 'st-rng' }, h('input', { type: 'range', min: f.min, max: f.max, step: f.step, value: f.value, onChange: f.on }),
        h('input', { className: 'st-num', type: 'number', min: f.min, max: f.max, step: f.step, value: f.value, onChange: f.onNum, title: said ? said + ' · type an exact value' : 'Type an exact value' }))
      : f.isSelect ? h('select', { onChange: f.on, value: String(f.value) }, list(f.opts).map(function (x, i) { return h('option', { key: i, value: x.v }, x.label); }))
      : f.isChips ? h('div', { className: 'ui-chips st-pills' }, list(f.opts).map(function (x, i) { return h('button', { key: i, onClick: x.pick, style: { background: x.bg, borderColor: x.bd, color: x.fg } }, x.label); }))
      : f.isSwatch ? h('div', { className: 'ui-chips' }, list(f.opts).map(sw))
      : f.isColor && f.cur ? h('div', null,
          // the hub's one colour picker (§12.5): the preset's colour, this one (it opens the
          // palette), the hex; under it the palette: Classic, Vivid or Any's system picker
          h('div', { className: 'ui-chips', style: { alignItems: 'center', flexWrap: 'nowrap' } },
            f.pre ? sw(f.pre, 'pre') : null,
            h('button', { className: cx('st-sw', f.cur.cls, open ? 'on' : ''), onClick: function () { openS[1](!open); }, title: open ? 'Close the palette' : f.cur.label, style: { background: f.cur.bg, borderColor: f.cur.bd } }, f.cur.text),
            h('input', { type: 'text', value: f.hexText, onChange: f.onHex, onBlur: f.onHexBlur, placeholder: '#rrggbb', title: f.hexTitle, style: { flex: '1 1 80px', fontFamily: 'var(--mono)' } }),
            h('button', { className: 'ui-ghost st-pal-tog', onClick: function () { openS[1](!open); }, title: open ? 'Close the palette' : 'Open the palette' }, open ? '▴' : '▾')),
          open ? h('div', { className: 'st-pal-box' },
            h('div', { className: 'st-pal' }, list(f.palTabs).map(function (t, i) {
              return h('button', { key: i, className: t.on ? 'on' : '', onClick: t.pick, title: 'Show the ' + t.label.toLowerCase() + ' colours, here and on every colour row after' }, t.label);
            })),
            h('div', { className: 'ui-chips', style: { alignItems: 'center' } }, list(f.opts).map(sw),
              f.picker ? h('input', { type: 'color', value: f.value, onChange: f.on, title: 'Any colour' }) : null)) : null)
      : f.isColor ? h('div', { className: 'ui-chips', style: { alignItems: 'center' } }, list(f.opts).map(sw),
          h('input', { type: 'color', value: f.value, onChange: f.on, title: 'Custom colour' }),
          h('input', { type: 'text', value: f.hexText, onChange: f.onHex, onBlur: f.onHexBlur, placeholder: '#rrggbb', title: f.hexTitle, style: { flex: '1 1 80px', fontFamily: 'var(--mono)' } }))
      : f.isSample ? h('div', { className: 'st-sample', style: f.style }, f.value)
      : f.isText ? h('input', { type: 'text', value: f.value, onChange: f.on })
      : f.isFile ? h('input', { type: 'file', accept: f.accept, onChange: f.on })
      : f.isCanvas ? h('canvas', { ref: f.ref, width: f.w, height: f.h, onMouseDown: f.onDown, onMouseMove: f.onMove, title: f.tip, className: 'st-cv', style: { width: f.css + 'px', height: f.css + 'px' } })
      : f.isNote ? h('span', { className: 'st-tip', title: f.note }, 'ⓘ') : null;
    return h('div', { className: cx('st-row', f.rowCls), 'data-a': f.anchor },
      h('label', { title: f.tip || f.label }, f.label, f.tip ? h('span', { className: 'st-tip' }, 'ⓘ') : null,
        f.hasLink ? h('span', { onClick: f.relink, title: f.linkTip, style: { color: f.linkFg, cursor: f.linkCursor } }, f.linkText) : null),
      h('div', { style: { minWidth: 0 } }, ctl, h('div', { className: 'st-cname' }, f.isRange ? capUnder : f.isColor && f.valText === f.hexText ? '' : f.valText)),
      h('div', { className: 'st-val', title: f.valText }, f.valText));
  }
  // fields: the kit's rows; a slider keeps the value column, every other row reads its value under it.
  // A note is not printed (owner, 2026-11-04: the explanations were clutter): it becomes the
  // hover text of the row or section above it, marked ⓘ; a note with a label of its own, or
  // with nothing above it, is a row holding only the ⓘ.
  S.Fields = function (p) {
    var out = [];
    list(p.fields).forEach(function (f) {
      var prev = out[out.length - 1];
      if (f.isNote && !f.label && prev && !prev.isNote) {
        out[out.length - 1] = Object.assign({}, prev, { tip: (prev.tip ? prev.tip + '\n\n' : '') + f.note });
        return;
      }
      out.push(f);
    });
    var shut = false;
    return h('div', { className: 'st-fields' }, out.map(function (f, i) {
      if (f.isHead) shut = !!(f.fold && !f.fold.open);
      if (f.isHead) return h('div', { key: i, className: cx('st-sec', f.fold ? 'st-fold' : '', f.fold && !f.fold.open ? 'shut' : ''), 'data-a': f.anchor, title: f.tip || '',
        onClick: f.fold ? f.fold.toggle : undefined }, f.fold ? h('i', null, f.fold.open ? '▾' : '▸') : null, f.label, f.tip ? h('span', { className: 'st-tip' }, 'ⓘ') : null);
      if (shut) return null;
      if (!f.isRow) return null;
      return h(FieldRow, { key: i, f: Object.assign({}, f, { rowCls: f.isRange ? 'rng' : f.rowCls != null ? f.rowCls : 'wide' }) });
    }));
  };

  /* ---- the layers panel (K7). The app decides what each layer is and how it may move; the
   * shell lays the rows out: a band head per band, a row indented by its depth under its
   * parent, the selection, the arrows, the eye, the row's explanation as its tooltip.
   * items: [{id, parent, band, label, movable}] in display order. o:
   *   sel (the selected row id), heads ({band: label}), dragging (the dragged row id),
   *   icon(it), note(it), tag(it), warn(it), num(it) -> text (num: the part's '#n', as the
   *   inspector's pickers name it, printed before the label), hidden(it) -> bool,
   *   move(id, dir) -> {ok, reason}, onMove(id, dir), pick(it), eyeTip(it), toggleEye(it),
   *   drag: {start(it, e), over(e, it), drop(it, e), end()} */
  S.layerRows = function (items, o) {
    o = o || {};
    var rows = [], band = null, byRow = {}, call = function (f, it, d) { return f ? f(it) : d; };
    items.forEach(function (x) { byRow[x.id] = x; });
    var depthOf = function (x) { var n = 0; for (var p = x.parent; p && byRow[p] && n < 6; p = byRow[p].parent) n++; return n; };
    var none = { ok: false, reason: '' };
    items.forEach(function (it) {
      if (it.band !== band) { band = it.band; rows.push({ isHead: true, isRow: false, label: (o.heads || {})[band] }); }
      var up = it.movable ? o.move(it.id, -1) : none, dn = it.movable ? o.move(it.id, 1) : none;
      var hidden = !!call(o.hidden, it, false), on = o.sel === it.id, warn = call(o.warn, it, ''), depth = depthOf(it);
      rows.push({ id: it.id, parent: it.parent || null, isHead: false, isRow: true, label: it.label, num: call(o.num, it, ''), warn: warn ? ' ⚠' : '', icon: call(o.icon, it, '•'),
        cls: (on ? 'on' : '') + (it.parent ? ' child' : '') + (o.dragging === it.id ? ' dragging' : ''), indent: 14 * depth, tag: call(o.tag, it, ''),
        note: call(o.note, it, ''),
        tip: it.movable ? 'Drag onto another layer, or use the arrows' : (o.move(it.id, 1).reason || ''),
        pad: 6 + 16 * depth, weight: 400, op: hidden ? 0.45 : 1, bg: on ? '#1f3a5f' : 'transparent', edge: on ? '#58a6ff' : 'transparent',
        arrows: it.movable ? 'visible' : 'hidden',
        upOp: up.ok ? 1 : 0.35, downOp: dn.ok ? 1 : 0.35, upTip: up.ok ? 'Up one' : up.reason, downTip: dn.ok ? 'Down one' : dn.reason,
        up: function (e) { e.stopPropagation(); o.onMove(it.id, -1); }, down: function (e) { e.stopPropagation(); o.onMove(it.id, 1); },
        drag: it.movable ? 'true' : 'false',
        dragStart: function (e) { o.drag.start(it, e); },
        dragOver: function (e) { o.drag.over(e, it); },
        drop: function (e) { e.preventDefault(); o.drag.drop(it, e); },
        dragEnd: function () { o.drag.end(); },
        pick: function () { o.pick(it); },
        eye: hidden ? 'hidden' : 'shown', eyeIcon: hidden ? '○' : '◉', eyeFg: hidden ? '#5d6b78' : '#8b98a5',
        eyeTip: call(o.eyeTip, it, ''),
        eyeToggle: function (e) { e.stopPropagation(); o.toggleEye(it); } });
    });
    // the row's explanation (its note, what it costs, why it cannot move) is its tooltip
    rows.forEach(function (r) { if (r.isRow) r.title = [r.label, r.note, r.tip].filter(Boolean).join(' · '); });
    return rows;
  };
  // a drop gap the app places among the rows: {label, ok, indent, drop}
  S.layerGap = function (g) {
    return { isGap: true, isRow: false, isHead: false, cls: g.ok ? '' : 'no', indent: g.indent || 0, label: g.label,
      dragOver: function (e) { if (e && e.preventDefault) e.preventDefault(); }, drop: function (e) { if (e && e.preventDefault) e.preventDefault(); g.drop(); } };
  };
  /* title, cap: {text, color, tip}, rows (layerRows, with any gap), warnings: [{text}], add:
   * {label, tip, on}, children: the footer (the app's switches) */
  S.Layers = function (p) {
    var cap = p.cap || {};
    return h(root.React.Fragment, null,
      h('div', { className: 'st-ph' }, h('span', { className: 'ui-label' }, p.title || 'Layers'),
        cap.text ? h('span', { className: 'ui-mono', style: { fontSize: '11px', color: cap.color }, title: cap.tip || '' }, cap.text) : null),
      h('div', { className: 'st-list' },
        list(p.rows).map(function (r, i) {
          if (r.isHead) return h('div', { key: i, className: 'st-band' }, r.label);
          if (r.isGap) return h('div', { key: i, className: cx('st-gap', r.cls), onDragOver: r.dragOver, onDrop: r.drop, style: { marginLeft: r.indent + 'px' } }, r.label);
          if (!r.isRow) return null;
          return h('div', { key: r.id || i, className: cx('st-lr', r.cls), draggable: r.drag, onDragStart: r.dragStart, onDragOver: r.dragOver, onDrop: r.drop, onDragEnd: r.dragEnd, onClick: r.pick, title: r.title, style: { marginLeft: r.indent + 'px', opacity: r.op } },
            h('span', { className: 'st-ic' }, r.icon),
            h('span', { className: 'st-nm', style: { fontWeight: r.weight } }, r.num ? h('b', { className: 'st-lnum' }, r.num) : null, r.label, h('span', { className: 'st-warn' }, r.warn), h('small', null, r.tag)),
            h('span', { className: 'st-acts' },
              h('button', { className: 'st-mini', onClick: r.up, title: r.upTip, style: { opacity: r.upOp, visibility: r.arrows } }, '↑'),
              h('button', { className: 'st-mini', onClick: r.down, title: r.downTip, style: { opacity: r.downOp, visibility: r.arrows } }, '↓'),
              h('button', { className: 'st-eye', onClick: r.eyeToggle, title: r.eyeTip, style: { color: r.eyeFg } }, r.eyeIcon)));
        }),
        list(p.warnings).map(function (w, i) { return h('div', { key: 'w' + i, className: 'st-warnrow' }, '⚠ ' + w.text); }),
        p.add ? h('button', { className: 'st-add', onClick: p.add.on, title: p.add.tip || '' }, p.add.label) : null),
      p.children ? h('div', { className: 'st-foot' }, p.children) : null);
  };

  /* ---- builders: the services' answers as the components' props ---- */
  // a sender's state (OrbTheme.sender's `sending`) and the status line as SendModal's props
  S.sendProps = function (sending, status, onCancel) {
    return { open: !!sending, line: status || 'Building the theme…',
      hint: !sending ? '' : sending.cancelling ? 'Cancelling: the send stops before anything reaches the orb.'
        : sending.canCancel ? 'Nothing has reached the orb yet, so Cancel stops it cleanly.'
        : 'The orb is storing it and rebooting onto it. That cannot be stopped part way, so this waits until the orb is back.',
      noCancel: !(sending && sending.canCancel), cancelLabel: sending && sending.cancelling ? 'Cancelling…' : 'Cancel', onCancel: onCancel };
  };
  // the sender's duplicate question as DupModal's props
  S.dupProps = function (dup, onReplace, onCancel) {
    return { open: !!dup, name: dup ? dup.nm : '', stem: dup ? dup.stem : '', note: dup ? dup.note : '', onReplace: onReplace, onCancel: onCancel };
  };
  // an OrbTheme.fit verdict as the top bar's dot and the Render panel's badge
  S.fitProps = function (V, tip) { return { dot: V.color, text: V.short, tip: tip || '' }; };
  S.badgeProps = function (V, tip) { return { text: V.text, note: V.note, tip: tip || '', color: V.color, border: V.border, background: V.background }; };
  // an OrbTheme.view answer, the calibration and its handlers as Calibrate's props
  S.calProps = function (V, c) {
    return { text: V.text, show: V.showRuler, ruler: { px: V.rulerPx, ticks: V.rulerTicks },
      cal: { min: c.min, max: c.max, step: c.step, value: c.value, text: V.calText, onChange: c.onChange, up: c.up, down: c.down, solve: c.solve, reset: c.reset },
      card: c.solveCard ? { w: V.card.w, h: V.card.h, r: V.card.r, solve: c.solveCard } : null };
  };
})(typeof window !== 'undefined' ? window : globalThis);
