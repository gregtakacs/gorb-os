/* pomodoro-editor.js — the pomodoro's part of its theme editor
 * (docs/specs/pomodoro-editor-design.md §7, docs/specs/pomodoro-looks.md "What the editor offers").
 *
 * Orb Manager owns the page (host@1): the start sheet, the steps' shell, the field kit, the
 * library, import, the Render step, the stage and its views, the fit, history and the send. What
 * is the pomodoro's is here: the six steps and their fields (Ground, Furniture, Words, Count,
 * Ring, Chimes), the parts this look's furniture has, the views the glass can be shown in, and
 * the preview — the bake's own `picture()` and nothing else, so the glass an author edits is the
 * glass that bakes (design §5's rule against a second renderer). The kit's sound pane (studio@1's
 * S.Sound, plan §64.8) offers the nine chimes and the owner's upload; the recipes are pomodoro-
 * bake's. Every field here writes a design field the bake already reads, and the editor calls the
 * bake with nothing in between.
 *
 * An owner's uploaded wave is not undo history. The host snapshots the design by value, so the
 * design carries only `{name, ref, samples}` per upload and the Int16Array itself rides in `ups`
 * below, keyed by the ref — the clock host's answer, copied (clock-host.js:93-133). It goes back
 * into the design for the bake, for the document a save writes and for playback, so a design
 * saved with an uploaded chime, reopened and baked answers the same bytes. */
(function (root) {
  'use strict';
  var B = root.PomodoroBake, S = root.OrbStudio;
  var G = 466;
  var PHASES = [['focus', 'A focus ends'], ['short', 'A short break ends'], ['long', 'A long break ends']];
  var PHASE_TITLE = { focus: 'focus', short: 'short break', long: 'long break' };
  var ROWS = 12;                                 // design §2: two plates, three fonts, five labels, the dots row
  var CAP = 64;                                  // the ceiling; the values row and the three chimes ride too
  var clone = function (o) { return JSON.parse(JSON.stringify(o)); };
  var HEX6 = /^#[0-9a-f]{6}$/i;
  var SR = function () { return (root.GorbBake && root.GorbBake.SOUND_RATE) || 22050; };

  /* ---- the uploads: the waves, held by reference and never in a snapshot ---- */
  var ups = {}, upN = 0;
  // a wave back to the Int16Array the bake reads: a JSON round trip leaves a typed array as
  // {"0": v, "1": v, …}, and read back as that object every sample is silence (clock-host.js:116-127)
  function pcmOf(p) {
    if (!p || p instanceof Int16Array) return p;
    if (Array.isArray(p)) return Int16Array.from(p, function (v) { return v | 0; });
    if (typeof p !== 'object') return null;
    var ks = Object.keys(p).filter(function (k) { return /^\d+$/.test(k); }).sort(function (a, c) { return a - c; });
    if (!ks.length) return null;
    var out = new Int16Array(ks.length);
    ks.forEach(function (k, i) { out[i] = p[k] | 0; });
    return out;
  }
  // the wave in hand: keep it, and hand the design the stub that names it. The ref a document
  // carries is kept, so an open, a save and an open again name the same wave.
  function keep(pcm, name, ref) {
    var r = ref && /^u[0-9]+$/.test(ref) ? ref : 'u' + (++upN);
    if (ref && /^u[0-9]+$/.test(ref) && +ref.slice(1) > upN) upN = +ref.slice(1);
    ups[r] = pcm;
    return { name: name || 'your own', ref: r, samples: pcm.length };
  }
  function waveOf(u) { return u && u.ref && ups[u.ref] ? ups[u.ref] : null; }
  // the design as the bake and the pane want it: the stubs back to their waves
  function artOf(d) {
    if (!d || !(d.uploads || []).length) return d;
    var n = Object.assign({}, d);
    n.uploads = d.uploads.map(function (u) { var p = waveOf(u); return p ? { name: u.name, pcm: p } : { name: u.name }; });
    return n;
  }
  // what the kit's pane shows per slot: this owner's upload, with the wave behind it
  function uploadsFor(d) {
    var out = {};
    PHASES.forEach(function (p) {
      var s = (d.chimes || {})[p[0]] || {}, u = s.upload !== undefined ? (d.uploads || [])[s.upload] : null, pcm = waveOf(u);
      if (pcm) out[p[0]] = { name: u.name, pcm: pcm };
    });
    return out;
  }
  function uploadFor(d, at) {
    var s = (d.chimes || {})[at] || {};
    if (s.upload !== undefined && waveOf((d.uploads || [])[s.upload])) return s.upload;
    var have = (d.uploads || []).map(function (u, i) { return waveOf(u) ? i : -1; }).filter(function (i) { return i >= 0; });
    return have.length ? have[have.length - 1] : -1;
  }
  // a slot's new chime: a recipe keeps the variant this phase's end strikes, an upload keeps its index
  function chimeSet(d, at, sel) {
    var n = clone(d);
    n.chimes = Object.assign({}, d.chimes);
    n.chimes[at] = sel === 'upload' ? { upload: uploadFor(d, at) } : Object.assign({ variant: at === 'focus' ? '' : 'brighter' }, sel);
    return n;
  }

  /* ---- a chime the author can hear, and one the author can bring: one AudioContext, ours ---- */
  var ac = null, node = null;
  function actx() {
    var Ctor = root.AudioContext || root.webkitAudioContext;
    if (!Ctor) throw new Error('this browser cannot play a chime');
    return ac || (ac = new Ctor());
  }
  function stopPlay() {
    if (node) { try { node.stop(); } catch (e) { /* it had already ended */ } node = null; }
  }
  function play(pcm) {
    stopPlay();
    var a = actx(), buf = a.createBuffer(1, pcm.length, SR()), ch = buf.getChannelData(0);
    for (var i = 0; i < pcm.length; i++) ch[i] = pcm[i] / 32768;
    node = a.createBufferSource(); node.buffer = buf; node.connect(a.destination); node.start();
    node.onended = function () { node = null; };
  }
  // a file to the kit's rate and 16-bit samples, refused past the owner's 4.0 s with its reason
  async function decode(file) {
    var a = actx(), dec = await a.decodeAudioData(await file.arrayBuffer());
    var off = new root.OfflineAudioContext(1, Math.ceil(Math.min(B.UPLOAD_MAX_S + 0.5, dec.duration) * SR()) + 1, SR());
    var src = off.createBufferSource(); src.buffer = dec; src.connect(off.destination); src.start();
    var re = await off.startRendering(), f = re.getChannelData(0), pcm = new Int16Array(f.length);
    for (var i = 0; i < f.length; i++) pcm[i] = Math.max(-32768, Math.min(32767, Math.round(f[i] * 32767)));
    if (dec.duration > B.UPLOAD_MAX_S + 0.02)
      throw new Error('“' + file.name + '” is ' + dec.duration.toFixed(1) + ' s: a pomodoro chime is at most '
        + B.UPLOAD_MAX_S.toFixed(1) + ' s, so it was not taken');
    return pcm;
  }
  function takeUpload(at, file, c) {
    decode(file).then(function (pcm) {
      c.set(function (d) {
        var n = clone(d);
        n.uploads = (d.uploads || []).slice();
        n.uploads.push(keep(pcm, file.name));
        n.chimes = Object.assign({}, d.chimes);
        n.chimes[at] = { upload: n.uploads.length - 1 };
        return n;
      });
      c.say('“' + file.name + '” is ' + (pcm.length / SR()).toFixed(1) + ' s of chime for the ' + PHASE_TITLE[at] + '’s end');
    }, function (e) { c.say('Not taken: ' + (e && e.message ? e.message : e)); });
  }

  /* ---- what the editor refuses, before the author sends. The host prints this sentence in its
   * own bar, so it says what to do and never how the stack fell. ---- */
  function checkDesign(x) {
    if (!x || typeof x !== 'object') return 'that design has no pomodoro theme in it';
    if (B.LOOKS.indexOf(x.look) < 0) return 'this design’s look is not one of the nine Pomodoro draws';
    if (!(x.px >= B.PX_MIN && x.px <= B.PX_MAX))
      return 'the time stands ' + x.px + ' px tall: the figures are ' + B.PX_MIN + '..' + B.PX_MAX + ' px';
    if (!HEX6.test(x.ink) || !HEX6.test(x.accent) || !HEX6.test(x.quiet)) return 'one of the look’s inks is not a #rrggbb colour';
    if (!x.ring || !HEX6.test(x.ring.focus) || !HEX6.test(x.ring.short) || !HEX6.test(x.ring.long))
      return 'a ring colour is not a #rrggbb colour';
    var why = null;
    PHASES.forEach(function (p) {
      if (why) return;
      var s = (x.chimes || {})[p[0]] || {}, at = PHASE_TITLE[p[0]];
      if (s.upload !== undefined) {
        if (!waveOf((x.uploads || [])[s.upload]))
          why = 'the ' + at + '’s end names one of your own uploads and this design carries no wave for it: choose the chime again';
      } else if (s.rec === undefined)
        why = 'the ' + at + '’s end has no chime: a pomodoro that cannot chime cannot tell you a phase ended';
      else if (!B.CHIMES[s.rec])
        why = 'the ' + at + '’s end names “' + s.rec + '”, which is not one of Pomodoro’s chimes';
    });
    return why;
  }

  /* ---- the glass: one call to the bake's picture, wherever a glass is shown ---- */
  var PART_STEPS = ['ground', 'furniture', 'words', 'count'];
  function litSteps(step) { return step === 'chimes' ? ['ring'] : PART_STEPS.indexOf(step) >= 0 ? ['furniture'] : [step]; }

  // the design the preview shows: the author's, with the parts the panel's eyes have hidden put
  // down for the frame (the eyes are a preview state; the bake keeps every part the design shows)
  function previewOf(d, hidden) {
    var n = clone(d);
    Object.keys(hidden || {}).forEach(function (k) { if (hidden[k] && n.parts && k in n.parts) n.parts[k] = false; });
    return artOf(n);
  }
  // what the picture is asked for now, in the four flags the bake takes
  function picOpts(v, o) {
    var st = v.state || 'running';
    return Object.assign({ view: v.phase || 'focus', count: +v.count || 3 },
      st === 'paused' ? { paused: true } : st === 'waiting' ? { waiting: true }
        : st === 'choosing' ? { choosing: true, choice: (v.phase === 'long' ? 'Long break ' : v.phase === 'short' ? 'Short break ' : 'Focus ') + (+v.count || 3) } : {},
      v.other === 'on' ? { other: true, ring: true } : { ring: true }, o || {});
  }
  function picKey(d, o, hidden) {
    return JSON.stringify([d, o, hidden ? Object.keys(hidden).filter(function (k) { return hidden[k]; }) : []]);
  }
  function paintTo(el, im) { var x = el.getContext('2d'); x.clearRect(0, 0, el.width, el.height); x.drawImage(im, 0, 0, el.width, el.height); }
  /* a small glass — a field's `Cv` sample — from the same picture() the stage draws and the bake
   * previews, cached by what it shows (the stub design, never its waves) so a pane of fields does
   * not re-render the look on every keystroke */
  var thumbs = {}, thumbN = 0;
  function glassCv(K, label, d, size, o, tip) {
    var key = picKey(d, o) + '@' + size, e = thumbs[key];
    if (!e) {
      if (thumbN > 24) { thumbs = {}; thumbN = 0; }
      e = thumbs[key] = { n: ++thumbN, im: null, els: [] };
      B.picture(previewOf(d, null), size, o).then(function (c) {
        e.im = c; var els = e.els; e.els = []; els.forEach(function (el) { paintTo(el, c); });
      }, function () { delete thumbs[key]; });
    }
    return K.Cv(label, function (el) {
      if (!el) return;
      if (e.im) paintTo(el, e.im); else if (e.els.indexOf(el) < 0) e.els.push(el);
    }, size, size, null, null, tip || 'The look, drawn by the bake’s own picture()');
  }
  // the dots row on its own, at the bake's own size, from the bake's own dotFrame
  function dotsCv(K, label, d, filled) {
    return K.Cv(label, function (el) {
      if (!el) return;
      var f = B.dotFrame(B.complete(d), filled), x = el.getContext('2d'), s = Math.min(2, (el.width - 12) / f.width);
      x.clearRect(0, 0, el.width, el.height);
      x.drawImage(f, (el.width - f.width * s) / 2, (el.height - f.height * s) / 2, f.width * s, f.height * s);
    }, 150, 150, null, null, 'Twelve seats, ' + filled + ' filled: the frame the app picks at row 13');
  }

  /* ---- the look's parts: what the left pane lists and the Furniture step hides. Taken from
   * pomodoro-bake's own furniture table, and named per look: no two looks share a part
   * (plan §64.4), so no two looks share a word for one either. ---- */
  var PART_TITLES = {
    graticule: ['Graticule', '⊕', 'The crosshair and the two unlabelled range circles'],
    scanline: ['Scan lines', '≡', 'The phosphor’s own texture, one line in three'],
    caption: ['Header row', 'T', 'The look’s quiet caption, the row the choice replaces while pressing'],
    ticket: ['Ticket', '▭', 'The paper the timer is printed on'],
    perforation: ['Perforation', '∴', 'The tear-off rule, top and bottom'],
    stamp: ['No. box', '⌗', 'The stamp the count lands in'],
    plate: ['Dial plate', '◎', 'The engraved plate the crown and pips stand on'],
    crown: ['Crown and pusher', '♛', 'The winding crown at the plate’s top edge'],
    pips: ['Cardinal pips', '✚', 'Four, unnumbered: rule 2 allows no scale'],
    construction: ['Construction circle', '◌', 'The dashed circle the drawing is built in'],
    dimension: ['Dimension line', '↔', 'The bar under the time, arrowheaded'],
    titleblock: ['Title block', '▤', 'POMODORO · PHASE · SHEET n OF N'],
    tiles: ['Flap tiles', '▦', 'The five black tiles the time stands on, hinged'],
    platform: ['Platform bar', '—', 'The red bar under the board'],
    destination: ['Destination rules', '≡', 'The double rule the phase reads as'],
    hourglass: ['Hourglass', '⧗', 'The constellation the sand runs in'],
    field: ['Star field', '✷', 'Eighty stars, seeded, inside the rim'],
    disc: ['Brushed disc', '◍', 'The chronograph’s own face, brushed'],
    pushers: ['Pushers', '⇅', 'Two, at eleven and one'],
    bars: ['Cardinal bars', '╪', 'Four bars on the disc’s edge'],
    grid: ['Chart grid', '#', 'The log’s own squares'],
    leg: ['Leg line', '╱', 'The dashed leg across the glass'],
    checkpoint: ['Checkpoint', '▣', 'The magenta box the time is logged in']
  };
  function partsOf(d) {
    var look = (d && B.LOOKS.indexOf(d.look) >= 0) ? d.look : 'scope';
    return (B.PARTS[look] || []).map(function (p) {
      var t = PART_TITLES[p] || [p, '•', ''];
      return { id: p, label: t[0], icon: t[1], note: function () { return t[2]; }, also: ['furniture'] };
    });
  }
  // choosing a look resets its own defaults and leaves the author's face and size alone
  function chooseLook(c, k) {
    c.set(function (o) {
      var n = B.scheme(o, k), up = (o.uploads || []).slice();
      n.name = o.name; n.base = k; n.uploads = up;
      n.face = o.face; n.bodyFace = o.bodyFace; n.quietFace = o.quietFace; n.px = o.px;
      PHASES.forEach(function (ph) {                       // an upload the author made is their art, not the look's
        var s = (o.chimes || {})[ph[0]];
        if (s && s.upload !== undefined && waveOf(up[s.upload])) n.chimes[ph[0]] = { upload: s.upload };
      });
      return n;
    });
    c.say(B.STARTER_TITLES[k] + ' brought its own furniture, dots, ring, chime and overlay; your face and size stayed');
  }

  /* ---- the fields of each step ---- */
  function fields(step, c) {
    var d = c.d, K = c.K, upd = c.upd, out = [];
    var C = function (label, path, X) {
      var v = c.getIn(d, path);
      return K.Co(label, v == null ? 'none' : v, function (nv) { upd(path, nv === 'none' ? null : nv); }, '', path,
        Object.assign({ noAuto: true, preset: c.getIn(c.base, path) }, X || {}));
    };
    var faces = B.FACES.map(function (f) { return { v: f, label: f }; });
    var glass = function (label, o) { return glassCv(K, label, d, o.size || 140, o); };

    if (step === 'ground') {
      out.push(K.H('Look'));
      out.push(K.Sw('Look', d.look, B.PRESETS.map(function (k) { return { k: k, label: B.STARTER_TITLES[k], bg: B.preset(k).groundTop }; }),
        function (k) { if (k !== d.look) chooseLook(c, k); }));
      out.push(K.Nt('', 'A look is a starting point, not a lock: choosing one sets its own furniture, dots, ring colours, chime and overlay, and leaves your face and your size where you left them.'));
      out.push(K.H('The time'));
      out.push(K.Rg('Size', d.px, B.PX_MIN, B.PX_MAX, 2, function (v) { upd('px', v); }, d.px + ' px'));
      out.push(K.Nt('', 'The figures the countdown is set in, ' + B.PX_MIN + '..' + B.PX_MAX + ' px. Whatever the face, all ten digits bake to one width, so a time that changes every second never walks sideways.'));
      out.push(K.H('Ground'));
      out.push(C('Top', 'groundTop'));
      out.push(C('Bottom', 'groundFoot'));
      out.push(K.Nt('', 'The plate under everything: one sweep of two colours. The furniture stands on it, and the OS’s ring runs outside it, at the rim.'));
      out.push(glass('The glass', { view: 'focus', size: 150 }));
    } else if (step === 'furniture') {
      out.push(K.H('This look’s instrument'));
      partsOf(d).forEach(function (p) {
        out.push(K.Ch(p.label, d.parts[p.id] === false ? 'off' : 'on',
          [{ v: 'on', label: 'Shown' }, { v: 'off', label: 'Hidden' }], function (v) { upd('parts.' + p.id, v === 'on'); }));
      });
      out.push(K.Nt('', 'Four rules every look keeps: a timer and not a clock, no scale that names minutes, digits of one width, and the rim left to the ring. Nothing here is drawn past radius 200 (the bake clips it anyway), and the marks that count are four cardinals at most. The left pane’s eye only hides a part in the preview; hiding it here takes it out of the theme.'));
      out.push(glass('The glass', { view: 'focus', size: 150 }));
    } else if (step === 'words') {
      out.push(K.H('Faces'));
      out.push(K.Sl('Time', d.face, faces, function (v) { upd('face', v); }));
      out.push(K.Sl('Words', d.bodyFace, faces, function (v) { upd('bodyFace', v); }));
      out.push(K.Sl('Small', d.quietFace, faces, function (v) { upd('quietFace', v); }));
      out.push(K.Nt('', 'Three fonts bake: the figures at your size, the words at 30 px and the small words at 24. The phase name and the header caption are set in the words face; the count, the state line and Blueprint’s title block in the small one.'));
      out.push(K.H('Inks'));
      out.push(C('Ink', 'ink'));
      out.push(C('Accent', 'accent'));
      out.push(C('Quiet', 'quiet'));
      if (d.look === 'station') out.push(C('On the flaps', 'figInk'));
      out.push(K.Nt('', 'Ink is the time, the count and the empty dots’ neighbour; the accent, a break’s phase name and the choice while pressing; quiet, the header caption, the state line and an empty dot’s outline. The phase name reads in ink through a focus and in the accent through a break, which is how the state reads from across the room.'));
      out.push(glass('The glass', { view: 'focus', size: 150 }));
    } else if (step === 'count') {
      out.push(K.H('The dots'));
      out.push(K.Sl('Shape', d.dots, B.DOTS.map(function (k) { return { v: k, label: B.DOT_TITLES[k] }; }), function (v) { upd('dots', v); }));
      out.push(K.Nt('', 'Twelve seats — the app’s ceiling — and thirteen frames bake, the one with `f` filled for every f from 0 to 12, so the row says how many focuses this cycle has run without redrawing. The shape is the look’s own: a board’s discs, a chart’s squares, brass studs, stars.'));
      out.push(dotsCv(K, 'The row', d, +c.view.count || 3));
      out.push(K.H('Their colours'));
      out.push(C('Filled', 'ink'));
      out.push(C('Accent', 'accent'));
      out.push(C('Empty', 'quiet'));
      out.push(K.Nt('', 'A filled dot takes the ink, an empty one the quiet outline; stars and chart squares take the accent, and a brass stud brings its own brass. They are the look’s own three inks, the same three the Words step sets.'));
      out.push(glass('The glass', { view: 'focus', count: +c.view.count || 3, size: 150 }));
    } else if (step === 'ring') {
      out.push(K.H('The OS’s ring'));
      PHASES.forEach(function (p) {
        out.push(C(p[1], 'ring.' + p[0]));
        out.push(glassCv(K, 'On its own ground', d, 132, { view: p[0] },
          'This colour on the look’s own glass, at the ring’s own opacity'));
        out.push(glassCv(K, 'Over another app', d, 132, { view: p[0], other: true },
          'The same colour over someone else’s words: the case it has to read in'));
      });
      out.push(K.Nt('', 'The ring drains over the phase’s own length and stands at the rim, outside every mark the furniture makes. Each colour is shown twice on purpose: a pick that reads only on its own ground is the mistake this step exists to catch before it ships. The colours bake into the theme’s values; the orb’s own setting can override them, and the app picks.'));
      out.push(K.H('The overlay colour'));
      out.push(C('Overlay', 'overlay', { preset: B.preset(d.look).ring[0] }));
      // §64.9: the OS reads no overlay colour off a theme yet (values ver 2, or a new scalar,
      // ships it), so this one is collected, stored and shown here and drawn nowhere.
      out.push(K.Nt('Overlay', 'The colour the orb’s own overlays — the ring, the battery, the notices — are drawn in over this look. It starts as the look’s first ring colour. Nothing on this base draws it, so it is a swatch and not a preview: the bake keeps it in the design where the next slice will find it.'));
      out.push(K.Rd('Rows', ROWS + ' of ' + CAP, 'Two plates, three fonts, five labels and the dots row. The values and the three chimes are rows too (16 of them in all) and the ceiling is 64; the row cost is gorb-os-row-cost.'));
    } else if (step === 'chimes') {
      out.push(K.H('The three phase ends'));
      out.push(K.Nt('Chimes', 'One strike as a phase ends, from the theme’s own sounds: a focus’s end, a short break’s end and a long break’s end each take their own row, and a break strikes the brighter variant of whatever a focus strikes. A design that changes nothing pays for two waves, not three. Every chime stays above 350 Hz for the 2 W speaker. Your own upload may run to ' + B.UPLOAD_MAX_S.toFixed(1) + ' s; a longer one is refused with its length, never cut short.'));
      out.push.apply(out, S.Sound(K, {
        recipes: B.CHIME_KEYS.map(function (k) { return { name: k, label: B.CHIMES[k].label }; }),
        slots: PHASES.map(function (p) { return { key: p[0], label: p[1] }; }),   // never Silent: amendment D promises a chime
        get: function (k) {
          var s = (d.chimes || {})[k] || {};
          if (s.upload !== undefined) return waveOf((d.uploads || [])[s.upload]) ? { upload: true } : {};
          return s.rec ? { rec: s.rec, variant: s.variant || '' } : {};
        },
        set: function (k, v) { c.set(function (o) { return chimeSet(o, k, v); }); },
        uploads: uploadsFor(d),
        upload: function (k, f) { takeUpload(k, f, c); },
        play: play,
        stop: stopPlay
      }));
      out.push(glass('The glass', { view: 'focus', size: 150 }));
    }
    return out;
  }

  /* ---- the views: what the glass is showing, on the stage's upper row ---- */
  var VIEWS = [
    { key: 'phase', label: 'Phase', tip: 'Which phase the preview shows, its time and its own ring colour',
      options: [['focus', 'Focus', '25:00, the count row, the focus ring colour'],
                ['short', 'Short break', '05:00, the break row, the short-break colour'],
                ['long', 'Long break', '15:00, the break row, the long-break colour']] },
    { key: 'state', label: 'State', tip: 'The four states the app’s rows show',
      options: [['running', 'Running', 'The countdown, no state line'],
                ['paused', 'Paused', 'The state line reads Paused in the quiet ink'],
                ['waiting', 'Waiting', 'The state line reads Waiting in the accent, and the ring flashes'],
                ['choosing', 'Choosing', 'The press-and-turn choice, on the header row’s own backing (AUX 12)']] },
    { key: 'count', label: 'Count', tip: 'Which focus of the cycle this is: the count row and the dots',
      options: [[1, '1 of 4'], [2, '2 of 4'], [3, '3 of 4'], [4, '4 of 4']], init: 3 },
    { key: 'other', label: 'Glass', tip: 'The look on its own glass, or the same look’s ring over someone else’s app',
      options: [['off', 'Its own glass', 'This theme on the glass'], ['on', 'Over another app', 'Another app’s words, this theme’s ring over them']] },
  ];
  // the design the parts are named from: the host reads A.parts once per render, after it has
  // asked for the step's fields (host.js:580 then :586), and the eight sets are different words
  var cur = B.preset('scope');
  var app = {
    slug: 'pomodoro', title: 'Pomodoro', schema: 1,
    check: checkDesign, complete: function (d) { return B.complete(d); },
    blank: { note: B.STARTER_NOTES.blank, make: function () { return B.blank(); } },
    templates: B.STARTERS.map(function (k) {
      return { key: k, name: B.STARTER_TITLES[k], note: B.STARTER_NOTES[k], make: function () { return B.preset(k); } };
    }),
    // the start sheet's tiles, the library's thumbnails and the stage all ask the bake's picture
    picture: function (d, px) { return B.picture(previewOf(d, null), px, { view: 'focus', count: 3 }); },
    // the tiles change when an owner's upload arrives, because it is held outside the design
    pictureKey: function () { return Object.keys(ups).sort().join(','); },
    steps: [['ground', 'Ground'], ['furniture', 'Furniture'], ['words', 'Words'], ['count', 'Count'], ['ring', 'Ring'], ['chimes', 'Chimes']],
    // the parts are the design's furniture, named per look, and so change with the look: the host
    // reads A.parts once per render, after it has asked for that render's fields (host.js:580 then
    // :586), which is where `cur` comes from. It lays them out as a S.Layers list through
    // S.layerRows (host.js:590), each with its eye; picking one opens the Furniture step (`also`).
    fields: function (step, c) { cur = c.d; return fields(step, c); },
    views: VIEWS,
    // the design the parts are named from, kept for the left pane: fields, adopt and draw all hand it over
    adopt: function (d) { cur = d; },
    draw: function (x, d, c) {
      cur = d;
      var o = picOpts(c.view);
      var img = c.part('glass', picKey(d, o, c.hidden), function () { return B.picture(previewOf(d, c.hidden), G, o); }, { fresh: true });
      x.clearRect(0, 0, G, G);
      if (img) x.drawImage(img, 0, 0, G, G);
    },
    // the bytes are the bake's, and nothing in between: the design's upload refs are filled back in
    bake: function (d, o) { return B.bake(artOf(d), o); },
    fit: function () { return { rows: ROWS, cap: CAP }; },
    /* a wave is art and not history: it rides the document by reference, so the host's snapshots
     * (its undo stack, its live size, its unsent-changes check) carry a name and a length only,
     * and an Undo — or a reopen — back to an uploaded chime still finds the wave to play and bake. */
    toDoc: async function (d, withArt) {
      var n = clone(d);
      n.uploads = (d.uploads || []).map(function (u) {
        var o = { name: u.name, ref: u.ref, samples: u.samples };
        if (withArt && waveOf(u)) o.pcm = Array.from(ups[u.ref]);
        return o;
      });
      return { design: n };
    },
    fromDoc: function (doc) {
      var d = doc && doc.design;
      if (!d || typeof d !== 'object' || !d.look) return null;
      var n = Object.assign({}, d);
      n.uploads = (d.uploads || []).map(function (u) {
        var pcm = pcmOf(u && (u.pcm !== undefined ? u.pcm : u.samples));
        if (u && u.ref && !pcm && waveOf(u)) pcm = ups[u.ref];                 // the ref this session still holds
        return pcm ? keep(pcm, u.name, u.ref) : { name: (u && u.name) || 'your own', ref: (u && u.ref) || '', samples: 0 };
      });
      return n;
    },
  };
  // the left pane's list, read per render off the design the fields were built from (see :377)
  Object.defineProperty(app, 'parts', { enumerable: true, get: function () { return partsOf(cur); } });

  root.PomodoroEditor = { app: app, check: checkDesign, partsOf: partsOf, waves: ups };
  if (root.OrbHost) root.OrbHost.register(app);
})(typeof window !== 'undefined' ? window : globalThis);
