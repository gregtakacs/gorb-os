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
    var setRef = function (el) { own.current = el; if (p.scrollRef) p.scrollRef.current = el; };
    return h(root.React.Fragment, null,
      h('div', { className: 'st-scroll', ref: setRef, onPointerDown: p.onPointerDown },
        h('div', { className: 'st-glass', style: { width: p.size + 'px', height: p.size + 'px', borderRadius: p.round || '50%' } }, p.children)),
      p.zoomTag ? h('div', { className: 'st-ztag' }, p.zoomTag) : null,
      p.status ? h('div', { className: 'st-toast', title: p.status }, p.status) : null);
  };

  /* play: {icon, label, on} · time / view / zoom / render: segmented options (zoom only while
   * magnifying) · wipe: {value, on} while splitting · children: the app's own tool buttons */
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
      p.children);
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
   * tip, on} under a hub · downloads: [{label, note, title, on}] · note: a warning line */
  S.RenderPanel = function (p) {
    var b = p.badge || null;
    return h(root.React.Fragment, null,
      b ? h('div', { className: 'st-badge', title: b.tip || '', style: { borderColor: b.border, background: b.background } }, h('b', { style: { color: b.color } }, b.text), h('span', null, b.note)) : null,
      p.send ? h('button', { className: 'st-big ui-primary', onClick: p.send.on, title: p.send.tip || '' }, h('b', null, 'Send to orb'), h('span', null, p.send.note)) : null,
      list(p.downloads).map(function (d, i) { return h('button', { key: i, className: 'st-big', onClick: d.on, title: d.title || '' }, h('b', null, d.label), h('span', null, d.note)); }),
      p.note ? h('div', { className: 'st-hint ui-mono st-warn' }, p.note) : null);
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
