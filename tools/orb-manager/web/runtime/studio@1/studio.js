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

  /* ---- the inspector's fields (K7): a kit of row builders and the component that draws them.
   * A field is one plain object; the kit's builders are the clock editor's own, moved, with
   * the two things that were the clock's made options: where a colour's typed draft is kept
   * (scope), and which choice of a select means "follow the source" (inherit). */
  S.INKS = [['#f6f3ec', 'White'], ['#1a140c', 'Black'], ['#c9a25a', 'Gold'], ['#d7dbe0', 'Silver'], ['#cfe8b0', 'Lume green'], ['#9fd8ff', 'Lume blue'], ['#c8322a', 'Red'], ['#ff7a1a', 'Orange'], ['#2b59c3', 'Blue'], ['#ff2bd6', 'Magenta'], ['#22e6ff', 'Cyan'], ['#b6ff2e', 'Acid']];
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
    var H = function (label, anchor) { return mk({ label: label, isHead: true, isRow: false, anchor: anchor }); };
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
    var Ch = function (label, value, opts, onv) { return mk({ label: label, isChips: true, opts: opts.map(function (x) { return Object.assign({}, on(String(x.v) === String(value)), { label: x.label, pick: function () { onv(x.v); } }); }) }); };
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
    /* a colour: the ink palette, a picker, and a hex box that keeps a half-typed draft per
     * (scope, label). X: {none: offer None, noAuto: no Auto} */
    var Co = function (label, value, onv, autoText, scope, X) {
      X = X || {};
      var key = (scope || scopeOf()) + ':' + label, hex = value && /^#[0-9a-f]{6}$/i.test(value) ? value : null, dr = drafts[key];
      return mk({ label: label, isColor: true, value: hex || '#ffffff', on: function (e) { delete drafts[key]; onv(e.target.value); },
        hexText: dr != null ? dr : hex || '', hexTitle: hex ? 'rgb ' + hexRGB(hex) : 'Type a hex code or r, g, b',
        onHex: function (e) { var v = e.target.value, p = S.parseColor(v); if (p) { delete drafts[key]; onv(p); } else { drafts[key] = v; refresh(); } },
        onHexBlur: function () { if (drafts[key] != null) { delete drafts[key]; refresh(); } },
        valText: value === 'none' ? 'None' : !value || value === 'auto' ? (X.noAuto ? 'None' : autoText || 'Auto') : value,
        opts: (X.none ? [{ label: 'None', text: '∅', bg: '#1c2330', bd: value === 'none' ? '#e6edf3' : '#2b3540', pick: function () { onv('none'); } }] : [])
          .concat(X.noAuto ? [] : [{ label: autoText || 'Auto', text: 'A', bg: '#1c2330', bd: !value || value === 'auto' ? '#e6edf3' : '#2b3540', pick: function () { onv('auto'); } }])
          .concat(S.INKS.map(function (c) { return { label: c[1], text: '', bg: c[0], bd: c[0] === value ? '#e6edf3' : '#232c36', pick: function () { onv(c[0]); } }; })) });
    };
    var Tx = function (label, value, onv) { return mk({ label: label, isText: true, value: value == null ? '' : String(value), on: function (e) { onv(e.target.value); } }); };
    var Fi = function (label, onv, valText, accept) { return mk({ label: label, isFile: true, on: onv, valText: valText || '', accept: accept || 'image/png,image/svg+xml,image/jpeg,image/webp' }); };
    // a run of rows folded to its first `keep`, with a More / Less chip; open and toggle are the app's
    var fold = function (rows, keep, open, toggle) {
      if (rows.length <= keep) return rows;
      var hid = rows.length - keep;
      var chip = Object.assign({}, rows[0], { label: '', valText: '', isHead: false, isRow: true, anchor: null, isRange: false, isSelect: false, isSwatch: false, isColor: false, isText: false, isFile: false, isNote: false, hasLink: false, isChips: true,
        opts: [{ label: open ? 'Less' : 'More (' + hid + ')', bg: '#1c2330', bd: '#2b3540', fg: '#8b98a5', pick: toggle }] });
      return (open ? rows : rows.slice(0, keep)).concat([chip]);
    };
    return { on: on, mark: mark, mk: mk, linkOf: linkOf, H: H, snapV: snapV, Rg: Rg, Sl: Sl, Ch: Ch, Nt: Nt, Sw: Sw, Co: Co, Tx: Tx, Fi: Fi, fold: fold };
  };

  // one field row: its label (with the inherit link), its control, and its value
  function FieldRow(p) {
    var f = p.f, sw = function (x, i) { return h('button', { key: i, className: 'st-sw', onClick: x.pick, title: x.label, style: { background: x.bg, borderColor: x.bd } }, x.text); };
    var ctl =
      f.isRange ? h('div', { className: 'st-rng' }, h('input', { type: 'range', min: f.min, max: f.max, step: f.step, value: f.value, onChange: f.on }),
        h('input', { className: 'st-num', type: 'number', min: f.min, max: f.max, step: f.step, value: f.value, onChange: f.onNum, title: 'Type an exact value' }))
      : f.isSelect ? h('select', { onChange: f.on, value: String(f.value) }, list(f.opts).map(function (x, i) { return h('option', { key: i, value: x.v }, x.label); }))
      : f.isChips ? h('div', { className: 'ui-chips' }, list(f.opts).map(function (x, i) { return h('button', { key: i, onClick: x.pick, style: { background: x.bg, borderColor: x.bd, color: x.fg } }, x.label); }))
      : f.isSwatch ? h('div', { className: 'ui-chips' }, list(f.opts).map(sw))
      : f.isColor ? h('div', { className: 'ui-chips', style: { alignItems: 'center' } }, list(f.opts).map(sw),
          h('input', { type: 'color', value: f.value, onChange: f.on, title: 'Custom colour' }),
          h('input', { type: 'text', value: f.hexText, onChange: f.onHex, onBlur: f.onHexBlur, placeholder: '#rrggbb', title: f.hexTitle, style: { flex: '1 1 80px', fontFamily: 'var(--mono)' } }))
      : f.isText ? h('input', { type: 'text', value: f.value, onChange: f.on })
      : f.isFile ? h('input', { type: 'file', accept: f.accept, onChange: f.on })
      : f.isNote ? h('div', { className: 'st-note', title: f.note }, f.note) : null;
    return h('div', { className: cx('st-row', f.rowCls), 'data-a': f.anchor },
      h('label', { title: f.label }, f.label, f.hasLink ? h('span', { onClick: f.relink, title: f.linkTip, style: { color: f.linkFg, cursor: f.linkCursor } }, f.linkText) : null),
      h('div', { style: { minWidth: 0 } }, ctl, h('div', { className: 'st-cname' }, f.valText)),
      h('div', { className: 'st-val', title: f.valText }, f.valText));
  }
  // fields: the kit's rows; a slider keeps the value column, every other row reads its value under it
  S.Fields = function (p) {
    return h('div', { className: 'st-fields' }, list(p.fields).map(function (f, i) {
      if (f.isHead) return h('div', { key: i, className: 'st-sec', 'data-a': f.anchor }, f.label);
      if (!f.isRow) return null;
      return h(FieldRow, { key: i, f: Object.assign({}, f, { rowCls: f.rowCls != null ? f.rowCls : f.isRange ? '' : 'wide' }) });
    }));
  };

  /* ---- the layers panel (K7). The app decides what each layer is and how it may move; the
   * shell lays the rows out: a band head per band, a row indented by its depth under its
   * parent, the selection, the arrows, the eye, the row's explanation as its tooltip.
   * items: [{id, parent, band, label, movable}] in display order. o:
   *   sel (the selected row id), heads ({band: label}), dragging (the dragged row id),
   *   icon(it), note(it), tag(it), warn(it) -> text, hidden(it) -> bool,
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
      rows.push({ id: it.id, parent: it.parent || null, isHead: false, isRow: true, label: it.label, warn: warn ? ' ⚠' : '', icon: call(o.icon, it, '•'),
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
            h('span', { className: 'st-nm', style: { fontWeight: r.weight } }, r.label, h('span', { className: 'st-warn' }, r.warn), h('small', null, r.tag)),
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
