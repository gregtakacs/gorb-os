window.COMP_CLASS = function (DCLogic) {

class Component extends DCLogic {
  DEF = { tick: 'sweep', balance: '1', plate: 'train', spokes: 'train', etching: 'none', pinion: 'on', center: 'jewel', finish: 'train', glow: 1, temp: 2200, innerAngle: 90, outerLight: 1, outerAngle: 315, outerTemp: 2200, trainAngle: 0, plateAngle: 0, plateScale: 1, hue: 30, hueSat: 0, outerHue: 210, outerSat: 0, wb: 6500, orbD: 128, orbTip: 194, cageD: 107, rearFinish: 'train', shAngle: -1, shDist: 1, shBlur: 1, shOp: 1 };
  state = { frame: 0, playing: true, slow: false, seam: 'Rendering…', status: '', seeds: { spokes: 1, pinion: 1, center: 1, finish: 1, etching: 1 }, dial: 'skeleton', designs: {}, sel: null, selKind: null, themeName: '', artSlot: 'sf', guide: 'auto', hands: 'auto', handFinish: 'auto', hidden: {}, timeMode: 'live', idInput: '', caseStyle: 'polished', caseMetal: 'auto', caseOpen: 221.5, glassStyle: 'domed', train: 'classic', tab: 'dial', cfg: {}, view: 'true', zoom: 2, viewCal: 1 };
  KEYS = Object.keys(this.DEF);
  RND = ['spokes', 'pinion', 'center', 'finish', 'etching'];
  TABS = [['dial', 'Dial'], ['comps', 'Complications'], ['hands', 'Hands'], ['move', 'Movement'], ['light', 'Light'], ['case', 'Case & glass'], ['export', 'Export']];
  LAYERS = [
    ['outer', '6', 'Case & outer shine', 'Bezel, vignette, halo'],
    ['glass', '5', 'Glass', 'Reflections follow the outer light'],
    ['hands', 'H', 'Hands', 'Placed and sized by the layout'],
    ['subdials', '4', 'Raised dials', 'One layer each, in list order'],
    ['dial', '3', 'Dial', 'Cut through by openings'],
    ['complications', 'C', 'Discs', 'Date, day, month & moon under their host'],
    ['geartrain', '2', 'Geartrain', 'Every moving part, inner light'],
    ['movement', '1', 'Clock insides', 'Static plate, inner light'],
  ];
  base = null; gt = []; faces = {}; budgetStats = null; _artKey = null; _ftier = 15; _ftierKey = null; _ftierOcc = null; _rpDiag = null; _occCover = -1; _bk = null; _bp = 0; _Lk = null; _L = null; _mbc = null; _occ = null; _occKey = null; _bi = null; _biKey = null; _hbFps = 0; _rmTimer = 0; _rmAt = 0;
  dialEls = {}; handEls = {}; layerEls = {}; caseEls = {}; glassEls = {}; artEls = {}; cbs = {};
  mainRef = React.createRef();
  paneRef = React.createRef();
  cfg() {
    const c = { ...this.DEF, ...this.state.cfg }, E = window.WatchEngine, T = E && E.TRAINS[this.state.train];
    if (T) { if (c.plate === T.plate) c.plate = 'train'; if (c.spokes === (T.spokes || 'arched')) c.spokes = 'train'; if (c.finish === (T.finish || 'brass')) c.finish = 'train'; }
    return c;
  }
  setCfg(k, v) {
    this.setState((st) => ({ cfg: { ...st.cfg, [k]: v }, ...(this.RND.includes(k) && v === 'random' ? { seeds: { ...st.seeds, [k]: Math.floor(Math.random() * 1e6) } } : {}) }));
  }
  _bG = 0;
  /* The budget is reached through this very getter, and the movement key it
   * builds goes through opts(). If that key ever reads the tempo again the two
   * calls fold into each other and the page dies on "maximum call stack" before
   * anything renders. Fail loudly and name the culprit instead. */
  get F() {
    if (this._bG) throw new Error('rows budget read this.F: the key it builds must not depend on the export frame count -- call opts(id, 1) / mvKey(1), which leave frames out');
    return this.budget().fps;
  }
  /* The bank: how many PNGs one authored loop is chopped into. Deliberately
   * deeper than the tempo -- see BANKS in dial-renderer.js. The device advances
   * exactly one bank frame per tick, so a deeper bank plays the SAME loop more
   * slowly for the same rows-per-tick price, and a rim that would step past half
   * a tooth pitch (and so read as running backwards) is stepped under it instead.
   * COUNT frames with this; PACE them with F. budgetStats.f stays F: the row
   * price is per tick, and the ladder compares per-tick prices. */
  get B() { return window.DialRenderer.bankFrames(this.F); }
  /* Device rows budget (docs/rows-capacity-2026-10-10.md, batch-6 numbers
   * under CLOCK_L4_MARK_STATIC_BOXES=0 -- now the shipped marking): the tick
   * that flushes the whole clock must fit 1000/fps minus ~17 ms of idle+TE,
   * and flush grows ~0.2 ms per DMA'd background row. buildMovement() measures
   * the widest per-tick union of rows the bg delta bank pushes -- consecutive
   * BASE frames culled to the texels that can reach the glass (rowPrice()).
   * Caps below are the largest holding band at each target from the decision
   * table; a visible moving second hand owns ~130 rows of its swept box on
   * every tick it moves, so it eats the same budget as that many background rows.
   * rowPrice() is an UPPER bound (it cannot see the bake's --matte fold), so a
   * fully closed dial still prices a ~48-row geartrain sliver it never DMA's.
   * A binary 15-fps cap of 0 would strand such a dial on the 12 tier -- the
   * needle's 15 cap is the measured sweeping-needle frontier instead: the tick
   * held 15.5 fps on a static bg (54.5 ms) and missed at a 96-row band (72.8 ms,
   * 13.1 fps), so ~64 rows is the largest band a needle still sweeps at 15 fps
   * (docs/rows-capacity-2026-10-10.md). It clears a closed dial's sliver while an
   * open skeleton (300 rows) and a busy needle band (96) still fall to 4 and 12.
   *
   * Three things keep this from fighting the renderer. (1) A measurement is
   * valid while the MOVEMENT signature is unchanged (budgetStats.mv), not while
   * a full key matches: the rows depend on the dial too (it hides most of the
   * train), and an art key that moved a frame before the new number was priced
   * is exactly the window where the cap snaps back to want and the train
   * re-renders -- the flicker and the walking numbers. A dial edit therefore
   * keeps quoting the last priced rows until remeasureRows lands. (2) The tier
   * ladder only ever compares rows measured at the delivered tempo, and _ftier
   * makes the choice monotone downward for a given art key, so buildMovement
   * settles after at most one or two extra passes. (3) Each pass is capped by
   * _bp in componentDidUpdate. forceWant pins the target tier without the
   * monotone clamp -- the emulator uses it to price a chosen tier directly. */
  budget(forceWant) {
    this._bG = 1;
    try { return this._budget(forceWant); } finally { this._bG = 0; }
  }
  _budget(forceWant) {
    const T = window.WatchEngine && WatchEngine.TRAINS[this.state.train];
    const car = !!(T && (T.cage || T.orbital));
    const c = this.cfg(), D = this.design();
    const needle = !!(D.centre && D.centre.second === 1) || (D.comps || []).some((x) => x.scale === 'seconds');
    const want = forceWant || (c.tick === 'sweep' ? 15 : 12);
    /* CAPS are rows-per-TICK, measured on the panel with a 1:1 bank (one frame
   * per tick), which is the largest step any tier can be asked to make. The
   * bank is now deeper than the tempo below 15 fps, so each tick turns a
   * SMALLER fraction of a tooth and changes fewer rows than the figure the cap
   * was struck against. A cap measured at the bigger step therefore still
   * bounds the smaller one; deepening a bank can only buy rows back, never
   * spend rows the cap does not already allow. */
  const CAPS = needle ? { 15: 64, 12: 96, 8: 240 } : { 15: 96, 12: 160, 8: 304 };
  /* The carriage bank is a fixed frame count on the device (bake_clock.py holds
   * CLK_TOURB_MAX=16), so a carriage kit may not sit at a tempo whose bank is
   * deeper than that -- the 12 fps tier's 24-frame bank cannot be baked. This is
   * a device limit, not an art limit: the carriage's ROWS are priced by
   * baseImgs()/rowPrice() like every other mover, so an exposed carriage still
   * costs rows (and falls to 4 fps) while one sealed behind an opaque dial costs
   * none and earns the full 15 fps a closed dial gets. */
  const MAXBANK = 16;
    const mvk = this.mvKey(1), ak = this.artKey(mvk);
    const s = this.budgetStats && this.budgetStats.mv === mvk ? this.budgetStats : null;
    /* Three cases. (1) Nothing measured for this movement: quote the wanted rate
     * so the build renders and measures something -- the _bp seatbelt bounds it.
     * (2) Measured, and it is THIS art that was priced: compute the tier.
     * (3) Measured for this movement but priced through the dial before the last
     * edit -- the rows in hand describe a dial the user no longer has. Quote the
     * rate already delivered and let remeasureRows, in this very commit, bring
     * the honest number. Pricing case 3 is the bug: the stale rows run through
     * the monotone _ftier clamp keyed on the NEW art key, so closing an openwork
     * dial hands the clamp the open dial's cap (420 rows -> 4 fps) as the new
     * key's ceiling and it never claws back up. The readout and the frames
     * on screen would then belong to different tempos. */
    if (s && s.ak !== ak) return { fps: this._hbFps || want, want, car, needle, rows: s.rows, cap: CAPS[want], measured: false, measuredAt: s.f, why: '' };
    if (!s) return { fps: want, want, car, needle, rows: 0, cap: CAPS[want], measured: false, measuredAt: 0, why: '' };
    const r = s.rows;
    let fps = 4;
    for (const t of [15, 12, 8]) { if (t > want) continue; if (car && WatchEngine.bankFrames(t) > MAXBANK) continue; if (r <= CAPS[t]) { fps = t; break; } }
    if (!forceWant) {
      const fk = ak + '|' + want;
      if (this._ftierKey !== fk) { this._ftierKey = fk; this._ftier = 15; this._ftierOcc = null; }
      /* The pin stops the tiers flicking between two prices of ONE measurement, so
       * it may only hold against the measurement that set it -- identified by the
       * occluder mask the number was taken against, which is the one input that can
       * silently land late (dial canvas still building, an upload arriving). Without
       * the release a single bad first price pins a 0-row closed dial at 8 fps for
       * the whole session, because min(15, 8) is 8 forever. Rows cannot be the
       * trigger: a lower count is often just fewer frames to differ over, and a
       * lower tempo is no new information about the art at all. */
      if (this._ftierOcc != null && s.occ !== this._ftierOcc) this._ftier = 15;
      fps = Math.min(fps, this._ftier); this._ftier = fps;
      this._ftierOcc = s.occ;                                   // the mask this measurement used
      this._hbFps = fps;                                         // the rate the frames on screen were rendered at
    }
    return { fps, want, car, needle, rows: r, cap: CAPS[want], measured: true, measuredAt: s.f, why: '' };
  }
  preset(id) { return window.WatchEngine.PRESETS[id || this.state.dial]; }
  design(id) { id = id || this.state.dial; return WatchEngine.canonDesign(JSON.parse(JSON.stringify(this.state.designs[id] || this.preset(id).design))); }
  train(id) { WatchEngine.tune(this.cfg()); return WatchEngine.trainAngle(this.design(id), this.cfg().trainAngle, this.state.train); }
  /* noframes MUST be resolved here rather than by deleting o.frames afterwards:
   * `frames: this.B` is the getter for the budget (through this.F), and budget()
   * calls mvKey(1). Building the full opts() and then dropping the field
   * evaluates that getter, so budget -> mvKey -> opts -> F -> budget recursed
   * until the stack died. The ternary below is what breaks the cycle -- with
   * noframes set, neither this.B nor this.F is ever read. Every other consumer
   * still sees the real tempo and the real bank. */
  opts(id, noframes) {
    const c = this.cfg(), s = this.state, dsg = s.designs[id || s.dial] || {};
    // the backdrop travels with the dial, but it is drawn at the plate slot, so it has to
    // reach movement() through the movement options -- and mvKey() must see it, because
    // swapping the art swaps the base frames the whole budget is priced against
    const bg = {}; ['bgArt', 'bgFit', 'bgZoom', 'bgX', 'bgY', 'bgSize', 'bgOpacity', 'bgRot', 'bgFinishBlend', 'bgFinishColor', 'bgFinishKey', 'bgFinishKeyTol'].forEach((k) => { if (dsg[k] != null && dsg[k] !== '') bg[k] = dsg[k]; });
    // the finish blended onto the plate-slot art is the DIAL's, so the surface that owns
    // the texture has to travel with it: changing Surface re-bakes these base frames
    if (bg.bgFinishBlend && bg.bgFinishBlend !== 'off') { bg.bgSurface = dsg.surface || 'enamel'; if (dsg.texture != null) bg.bgTexture = dsg.texture; if (dsg.weaveAngle != null) bg.bgWeaveAngle = dsg.weaveAngle; if (dsg.weaveScale != null) bg.bgWeaveScale = dsg.weaveScale; }
    return Object.assign({ finish: c.finish, plate: c.plate, tick: c.tick, glow: +c.glow, beats: +c.balance, spokes: c.spokes, etching: c.etching, pinion: c.pinion, center: c.center, temp: +c.temp, innerAngle: +c.innerAngle, outerLight: +c.outerLight, outerAngle: +c.outerAngle, outerTemp: +c.outerTemp, hue: +c.hue, hueSat: +c.hueSat, outerHue: +c.outerHue, outerSat: +c.outerSat, plateAngle: +c.plateAngle, plateScale: +c.plateScale, orbD: +c.orbD, orbTip: +c.orbTip, cageD: +c.cageD, rearFinish: c.rearFinish, shAngle: +c.shAngle, shDist: +c.shDist, shBlur: +c.shBlur, shOp: +c.shOp, trainAngle: this.ready ? this.train(id).deg : 0, caseStyle: s.caseStyle, caseMetal: s.caseMetal, caseOpen: +s.caseOpen, glassStyle: s.glassStyle, train: s.train, seeds: s.seeds, frames: noframes ? 0 : this.B }, bg);
  }
  mvKey(noframes) { const o = this.opts(null, noframes); ['tick', 'outerLight', 'outerAngle', 'outerTemp', 'outerHue', 'outerSat', 'shAngle', 'shDist', 'shBlur', 'shOp', 'caseStyle', 'caseMetal', 'caseOpen', 'glassStyle'].forEach((k) => delete o[k]); // under "no case" the plate-slot backdrop is built by the FACE, not baked into these frames -- the opening must be able to move without resampling it -- so the bank depends on which side of that option it was priced for
    o.noCase = this.state.caseStyle === 'none' ? 1 : 0; if (noframes) delete o.frames; return JSON.stringify(o); }
  artKey(mvk) { return (mvk || this.mvKey(1)) + '|' + this.state.dial + '|' + JSON.stringify(this.design()); }   // priced-art signature: the movement art, the tempo-independent part, plus the dial that hides it
  handDef(id) { const p = this.preset(id); return { style: this.state.hands === 'auto' ? p.hands : this.state.hands, finish: this.state.handFinish === 'auto' ? p.handFinish : this.state.handFinish }; }
  faceKey(id) { return JSON.stringify([this.opts(id), id, this.handDef(id), this.design(id)]); }
  face(id) {
    if (!this.ready) return null;
    id = id || this.state.dial;
    const k = this.faceKey(id);
    if (!this.faces[k]) {
      const keys = Object.keys(this.faces); if (keys.length > 12) delete this.faces[keys[0]];
      const hd = this.handDef(id);
      this.faces[k] = WatchEngine.face({ ...this.opts(id), design: this.design(id), hands: hd.style, handFinish: hd.finish });
    }
    return this.faces[k];
  }
  now() {
    const d = new Date();
    if (this.state.timeMode === 'pose') d.setHours(10, 9, 36, 0);
    if (this.state.timeMode === 'cycle') return WatchEngine.cycleDate(d, Math.floor(performance.now() / 450));
    return d;
  }
  scr() { if (!this._scr) { this._scr = document.createElement('canvas'); this._scr.width = this._scr.height = 466; } return this._scr; }
  cb(group, id, draw) {
    const k = group + ':' + id;
    if (!this.cbs[k]) this.cbs[k] = (el) => { this[group][id] = el; if (el) draw(id); };
    return this.cbs[k];
  }
  upd(fn) { this.setState((st) => { const id = st.dial, d = JSON.parse(JSON.stringify(st.designs[id] || this.preset(id).design)); fn(d, st); return { designs: { ...st.designs, [id]: WatchEngine.canonDesign(d) } }; }); }
  setComp(cid, patch) { this.upd((d) => { const c = d.comps.find((q) => q.id === cid); if (c) Object.assign(c, patch); }); }
  addComp(type) {
    const E = WatchEngine;
    this.upd((d, st) => {
      const n = 1 + Math.max(0, ...d.comps.map((c) => +String(c.id).replace(/\D/g, '') || 0)), id = 'c' + n;
      const sel = d.comps.find((c) => c.id === st.sel), host = sel && sel.type === 'subdial' && type !== 'subdial' ? sel.id : 'dial';
      d.comps.push({ id, type, host, ...JSON.parse(JSON.stringify(E.NEW_COMP[type])), ...(host !== 'dial' ? { dist: 0, size: type === 'moon' || type === 'opening' ? Math.round(sel.size * 0.5) : type === 'flare' ? Math.round(sel.size * 0.22) : 1 } : {}) });
      this._pendingSel = id;
    });
    setTimeout(() => this._pendingSel && this.setState({ sel: this._pendingSel, selKind: 'comp' }), 0);
  }
  delComp(cid) { this.upd((d) => { d.comps = d.comps.filter((c) => c.id !== cid); d.comps.forEach((c) => { if (c.host === cid) c.host = 'dial'; }); }); if (this.state.sel === cid) this.setState({ sel: null }); }
  moveComp(cid, dir) { this.upd((d) => { const i = d.comps.findIndex((c) => c.id === cid), j = i + dir; if (i < 0 || j < 0 || j >= d.comps.length) return; const t = d.comps[i]; d.comps[i] = d.comps[j]; d.comps[j] = t; }); }
  moveHand(hid, dir) {
    const fc = this.face(); if (!fc) return;
    const order = fc.handOrder.slice(), i = order.indexOf(hid), j = i + dir;
    if (i < 0 || j < 0 || j >= order.length) return;
    order[i] = order[j]; order[j] = hid;
    this.upd((d) => { d.order = order; });
  }
  setHand(hid, patch) { this.upd((d) => { d.hands = d.hands || {}; d.hands[hid] = { ...(d.hands[hid] || {}), ...patch }; }); }
  /* Facing stepper. Reads the design as it is NOW rather than the value the row was
   * rendered with -- a control that reports its own snapshot sticks after one click, so
   * reaching 90 degrees by 15-degree steps would need nine presses and land nowhere.
   * The angle wraps to [0,360), where -15 and 345 are the same facing but only one of
   * them is what an author expects to read back in the field. */
  stepFacing(hid, d) { const hc = ((this.design().hands || {})[hid]) || {}; const v = +hc.rot0 || 0; this.setHand(hid, { rot0: ((v + d) % 360 + 360) % 360 }); }
  // A dial carries ONE additional centre pointer beside hour, minute and seconds. What it indicates
  // is the single choice, made in one dropdown offered on both tabs; the printed ring is derived
  // from that choice through WatchEngine.EXTRA_RING, so the rim can never carry a scale that
  // disagrees with the hand reading it, and there is no longer a show/hide box per calendar
  // function to lose track of. The ring has one display switch of its own: Chapter pull-in 1.00
  // leaves the chapter at full size, which leaves nothing out there to print.
  calName(k) { return (window.WatchEngine.EXTRA_HANDS || {})[k] || k; }
  // The option label carries the split count: the dropdown is the only place the ring is
  // described, and n comes from PERIM_SPEC through perimGeom rather than from a second copy of
  // the numbers that could fall out of step with the print.
  extraOpts(E) {
    return [{ v: 'none', label: 'None · hour, minute and seconds only' }].concat(E.CAL_HANDS.map((k) => {
      const g = E.perimGeom({ perim: E.EXTRA_RING[k], perimShrink: 0.88 });
      return { v: k, label: E.EXTRA_HANDS[k] + ' · reads ' + (g ? g.spec.n : '?') + ' divisions on the rim' };
    }));
  }
  ringShown(D) { return !!D.extra && D.extra !== 'none' && (D.perimShrink == null || +D.perimShrink < 1); }
  setExtra(k) {
    const known = !!window.WatchEngine.EXTRA_HANDS[k];
    this.upd((d) => { d.extra = known ? k : 'none'; });
    if (known) this.setState({ status: this.calName(k) + ' hand on · the ring it reads prints with it' });
  }
  // Chapter pull-in 1.00 is the ring's off switch. Switching it back on hands the ring its
  // default width rather than restoring a value the user may never have set.
  setRingOn(on) {
    const D = this.design();
    if (!D.extra || D.extra === 'none') return;
    this.upd((d) => { if (on) delete d.perimShrink; else d.perimShrink = 1; });
    this.setState({ status: on
      ? this.calName(D.extra) + ' ring printed on the rim'
      : this.calName(D.extra) + ' ring hidden · the hand now stops inside the chapter print' });
  }
  /* Screen calibration lives with the browser, not with the dial: it describes the monitor the
   * studio is open on, so it must survive a reload and must never reach a design id, a kit, or
   * the artwork. localStorage can throw (file://, private mode), so every touch is guarded, and
   * "the user has calibrated" is tracked on the instance rather than re-read from storage, so a
   * blocked storage never makes an honest caption lie about itself. */
  CAL_KEY = 'wc.viewCal';
  CAL_MIN = 0.5;
  CAL_MAX = 3;
  _calSet = false;
  cal() {
    const c = +this.state.viewCal;
    return c >= this.CAL_MIN && c <= this.CAL_MAX ? c : 1;
  }
  calibrated() { return !!this._calSet; }
  /* A 0.005 step moves the true-size dial about 0.8 css px once past 2×, finer than the stage can
   * show, so the steps widen there. One definition feeds the slider step and the −/+ buttons. */
  calStep() { return this.cal() > 2 ? 0.01 : 0.005; }
  loadCal() {
    try {
      const v = +window.localStorage.getItem(this.CAL_KEY);
      if (v >= this.CAL_MIN && v <= this.CAL_MAX) { this._calSet = true; this.setState({ viewCal: v }); }
    } catch (e) { /* no storage: calibrate per session */ }
  }
  setCal(v) {
    const c = Math.min(this.CAL_MAX, Math.max(this.CAL_MIN, +v || 1));
    this._calSet = true;
    this.setState({ viewCal: +c.toFixed(4) });
    try { window.localStorage.setItem(this.CAL_KEY, String(c)); } catch (e) { /* ignore */ }
  }
  /* The measured millimetres the user reads off a real ruler against the on-screen bar solve the
   * factor outright, which beats hunting with a slider: the bar is nominal RULER_MM, so whatever
   * it really measures says how many CSS px make a millimetre here. */
  solveCal(mm) {
    if (mm == null || String(mm).trim() === '') return;
    const m = +mm;
    if (!(m >= 5 && m <= 300)) { this.setState({ status: 'Type what the ruler really measures, in mm — between 5 and 300' }); return; }
    this.setCal(this.cal() * (window.WatchEngine.RULER_MM / m));
    this.setState({ status: 'Ruler matched at ' + m + ' mm · true size now draws the dial ' + (window.WatchEngine.PANEL.inch * 25.4).toFixed(1) + ' mm across on this screen' });
  }
  /* The stage is the studio's only claim about how big the dial will be, and the artwork's own
   * 466 px is a misleading claim: the panel is 466 px across 1.75 in, so a preview drawn 1:1
   * prints under a millimetre of type that still looks fine on the monitor, which is how dials
   * got authored too small. True size is the default because it is the only honest preview, and
   * it is the only view that needs the ruler: the page cannot know the screen's dpi, so the user
   * sets the factor against a real ruler. Full keeps the old 1:1 view and Magnify scrolls a
   * bigger frame through the stage for precision work. */
  viewK() {
    const s = this.state, E = window.WatchEngine;
    if (s.view === 'full') return 1;
    if (s.view === 'zoom') return Math.min(4, Math.max(1, +s.zoom || 2));
    return ((E && E.REAL_Z) || 168 / 466) * this.cal();
  }
  setView(v) { this.setState((st) => ({ view: v, ...(v === 'zoom' && !(+st.zoom >= 1) ? { zoom: 2 } : {}) })); }
  setZoom(z) { const k = Math.min(4, Math.max(1, +z || 1)); this.setState({ view: 'zoom', zoom: k }); }
  /* Everything the stage row needs, derived from one number so the caption cannot disagree with
   * the box the canvas sits in. Past 1.00x the box stops growing and the frame scrolls through it:
   * every layer is baked at 466 px, so magnifying enlarges the artwork without inventing detail. */
  viewVals() {
    const E = window.WatchEngine, cal = this.cal(), k = this.viewK(), v = this.state.view;
    const sel = (b) => ({ bg: b ? '#2a2316' : '#1f1d1a', bd: b ? '#c9a25a' : '#3a352d', fg: b ? '#f0d79a' : '#e9e2d2' });
    // one source for each size: the engine owns what true size and the ruler mean
    const css = v === 'true' ? E.realCss(cal) : Math.max(60, Math.round(466 * k)), inch = E.screenInch(css, cal);
    // the stage box holds 466 px: the canvas only scrolls through it once it is physically
    // bigger than that, whether the user picked Magnify or just calibrated a dense screen
    const zoomed = v === 'zoom' || css > 466;
    const bez = Math.max(3, Math.min(12, Math.round((12 * css) / 466)));
    const mm = E.RULER_MM, mmPanel = (E.PANEL.inch * 25.4).toFixed(1);
    const ticks = [];
    for (let i = 0; i <= mm; i++) ticks.push({ left: (i / mm) * 100, h: i % 10 === 0 ? 14 : i % 5 === 0 ? 10 : 6, op: i % 5 === 0 ? '0.9' : '0.45', lab: i % 10 === 0 ? String(i) : '' });
    const nudge = (d) => this.setCal(this.cal() + d * this.calStep());
    return {
      stageCv: css, stageWin: zoomed ? 466 : css, stageRing: bez, stageRing2: bez + 1,
      stageFlow: zoomed ? 'auto' : 'hidden', stageRound: zoomed ? '14px' : '50%',
      hasZoom: zoomed,
      // the ruler belongs to true size only: Full and Magnify are magnifications of the artwork,
      // and a physical claim drawn there would be a claim about the zoom, not the watch
      showRuler: v === 'true', rulerPx: E.rulerPx(cal), rulerMm: mm, rulerTicks: ticks,
      calText: cal.toFixed(3) + '× · ' + Math.round(E.PANEL.cssInch * cal) + ' css px per screen in',
      calVal: cal, calMin: this.CAL_MIN, calMax: this.CAL_MAX, calStep: this.calStep(),
      setCal: (e) => this.setCal(e.target.value),
      calUp: () => nudge(1), calDown: () => nudge(-1),
      solve: (e) => this.solveCal(e.target.value),
      resetCal: () => { this.setCal(1); this.setState({ status: 'Screen calibration reset to the 96 px/in standard' }); },
      viewOpts: [['true', 'True size · 1.75 in', 'The whole dial at the size the panel is on the wrist. The only preview that tells you how print will read. Needs the ruler calibrated once.'],
                 ['full', 'Full · 1:1 · 466 px', 'The whole dial at the artwork\'s own scale, magnified for authoring detail.'],
                 ['zoom', 'Magnify', 'Scroll a bigger frame through the stage to inspect the print. The layers are baked at 466 px, so this enlarges them; it does not add detail.']]
        .map(([id, label, tip]) => ({ label, tip, pick: () => this.setView(id), ...sel(v === id) })),
      zoomOpts: [1, 1.5, 2, 3, 4].map((z) => ({ label: z + '×', pick: () => this.setZoom(z), ...sel(k === z) })),
      viewText: (v === 'full'
        ? 'Shown at the artwork\'s own scale: ' + inch.toFixed(2) + ' in across on this screen, ' + (inch / E.PANEL.inch).toFixed(1) + '× the 1.75 in panel'
        : v === 'zoom'
          ? 'Magnified ' + k.toFixed(2) + '× — the frame is baked at 466 px, so this enlarges it without adding detail'
          : this.calibrated()
            ? 'True size: ' + inch.toFixed(2) + ' in on this screen, the 1.75 in panel at ' + mmPanel + ' mm'
            // a page cannot measure its own screen, so an uncalibrated 168 px is only an
            // assumption — say so, and point at the instrument that fixes it once
            : 'True size drawn at the 96 px/in standard, so the 1.75 in panel is ' + inch.toFixed(2) + ' in on this screen — on a denser monitor that is smaller than the watch; match the ruler below to a real one once and it is exact')
        + ' · 1 dial px = ' + ((E && E.MM_PER_PX) || 0.0954).toFixed(4) + ' mm on the watch',
    };
  }

  /* Where the ring starts is the user's call, and the label must say what the turn does rather
   * than leave them guessing at a degree. The chips come off the same spec the ring prints from
   * (WatchEngine.ringStarts), so picking 'MON at 12' cannot disagree with the print, and the
   * pointer that reads the ring takes the same turn through perimGeom().offTurn. X is the field
   * builder bundle the tab already uses, passed in so this stays one definition for both tabs. */
  ringStartFields(E, D, X) {
    const g = E.perimGeom(D); if (!g) return [];
    const rot = g.rot, top = E.ringTop(g.spec, rot, g.between);
    return [
      X.Ch('Ring start', String(rot), E.ringStarts(g.spec, g.between), (v) => this.upd((d) => { d.perimRot = +v; })),
      X.Rg('Turn', rot, 0, 359, 1, X.ud('perimRot'), rot.toFixed(0) + '°'),
      X.Nt('', (rot === 0 ? 'Index 0 sits at 12 o\'clock.' : top.lab + ' now reads at 12 o\'clock, ' + rot.toFixed(0) + '° round.')
        + ' Ticks, labels and the hand that reads them all turn together, and the kit\'s drive formula carries the same turn.'),
    ];
  }
  designId() {
    const c = this.cfg(), s = this.state, E = window.WatchEngine, P = this.ready ? this.preset() : null;
    if (this.ready && this.train().by) c.trainAngle = 0;
    const seeds = this.RND.map((k) => (c[k] === 'random' ? s.seeds[k] : 0));
    const hands = P && s.hands === P.hands ? 'auto' : s.hands, hfin = P && s.handFinish === P.handFinish ? 'auto' : s.handFinish;
    const metal = this.ready && s.caseMetal !== 'auto' && s.caseMetal === E.caseMetalOf({ ...this.opts(), caseMetal: 'auto' }) ? 'auto' : s.caseMetal;
    // The case opening joins the ID vector only when it is the one thing the author
    // set by hand. Under "no case" the opening radius IS the design, so two watches
    // with different ones must not collide; under every other style the value is
    // derived from the style, and appending it unconditionally would renumber every
    // Design ID ever shared for a parameter nobody changed.
    const copen = this.ready && s.caseStyle === 'none' && E.caseOpenR ? E.caseOpenR({ caseStyle: 'none', caseOpen: s.caseOpen }) : null;
    const v = [this.KEYS.map((k) => c[k]), seeds, s.dial, hands, hfin, this.ready ? this.design() : null, s.caseStyle, metal, s.glassStyle, s.train, ...(copen != null ? [copen] : [])];
    return 'WCD5-' + btoa(unescape(encodeURIComponent(JSON.stringify(v)))).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
  }
  loadId(str) {
    try {
      const m = String(str).trim().match(/^WCD5-([A-Za-z0-9_-]+)$/); if (!m) throw 0;
      let b = m[1].replace(/-/g, '+').replace(/_/g, '/'); while (b.length % 4) b += '=';
      const [vals, seeds, dial, hands, handFinish, design, caseStyle, caseMetal, glassStyle, train, caseOpen] = JSON.parse(decodeURIComponent(escape(atob(b))));
      const cfg = {}; this.KEYS.forEach((k, i) => { if (vals[i] !== undefined) cfg[k] = vals[i]; });
      const sd = {}; this.RND.forEach((k, i) => (sd[k] = seeds[i]));
      // A bare Design ID carries references, not bytes. Say up front which art this
      // browser cannot draw, so an author never mistakes a missing sprite for a bug
      // in the bake -- the .wckit is the only thing that transports the art itself.
      const refs = design ? this.artRefs(WatchEngine.canonDesign(JSON.parse(JSON.stringify(design)))) : [];
      const miss = refs.filter((r) => !WatchEngine.assetKnown(r.id));
      this.setState((st) => ({ cfg, seeds: sd, dial, hands, handFinish, designs: design ? { ...st.designs, [dial]: design } : st.designs, caseStyle, caseMetal, glassStyle, train: train || 'classic', caseOpen: +caseOpen > 0 ? +caseOpen : st.caseOpen, sel: null, selKind: null, idInput: '',
        status: miss.length ? 'Loaded design — ' + miss.length + ' of ' + refs.length + ' art asset' + (refs.length > 1 ? 's' : '') + ' missing from this browser (' + miss.map((m) => m.usedBy).join(', ') + '). Load the .wckit bundle to get them back.' : 'Loaded design' }));
    } catch (e) { this.setState({ status: 'That design ID could not be read' }); }
  }
  /* ------------------------------ uploaded art ------------------------------------
   * The bytes live in the engine registry; a design carries only REFERENCES to them, so
   * a Design ID stays short and shareable and the .wckit bundle is what carries art
   * between browsers. localStorage can throw -- file://, private mode, quota -- so every
   * touch is guarded, and the readout says whether art really persisted rather than
   * claiming a save that never happened. */
  ART_KEY = 'wc.assets';
  artSaved = null;                                    // null = not attempted, else true/false
  /* Every asset this design points at, and what for: the manifest and the missing-art
   * warning are built from this one walk, so they cannot disagree about what is used. */
  artRefs(D) {
    const d = D || (this.ready ? this.design() : { comps: [], hands: {} }), out = [];
    if (d.bgArt) out.push({ id: d.bgArt, usedBy: 'plate backdrop', slot: 'bg' });
    if (d.sfArt) out.push({ id: d.sfArt, usedBy: 'dial surface', slot: 'sf' });
    Object.keys(d.hands || {}).forEach((k) => { const h = d.hands[k]; if (h && h.art) out.push({ id: h.art, usedBy: 'hand ' + k, slot: 'hand:' + k }); });
    (d.comps || []).forEach((c) => { if (c.icon === 'image' && c.img) out.push({ id: c.img, usedBy: c.type + ' #' + c.id, slot: 'comp:' + c.id }); });
    return out;
  }
  /* One line, stating what is held, what is missing, and whether it persisted. Naming
   * the count and the byte figure is the point: "saved" tells the author nothing about
   * whether the next reload will still have the art. */
  artNote(refs) {
    const E = window.WatchEngine, list = refs || this.artRefs();
    if (!list.length) return '';
    // count distinct ASSETS, not slots: one image can fill the plate and the dial face
    // at once, and "3 assets" when two slots share one file would misreport the store
    const ids = Array.from(new Set(list.map((r) => r.id)));
    const missing = ids.filter((id) => !E.assetKnown(id))
      .map((id) => ({ id, usedBy: list.filter((r) => r.id === id).map((r) => r.usedBy).join(' + ') }));
    const kb = Math.round(E.assetBytes() / 1024);
    const held = ids.length - missing.length;
    const base = held + '/' + ids.length + ' art ' + (ids.length === 1 ? 'asset' : 'assets') + ' held (' + kb + ' KB' + (this.artSaved === false ? ', this session only — storage is blocked' : this.artSaved ? ', saved in this browser' : '') + ')';
    if (!missing.length) return base;
    return base + ' · MISSING ' + missing.map((m) => m.usedBy + ' (' + m.id + ')').join(', ') + ' — the .wckit bundle carries them; a bare Design ID does not';
  }
  saveArt() {
    const E = window.WatchEngine;
    try {
      window.localStorage.setItem(this.ART_KEY, JSON.stringify(E.assetDump(true)));
      this.artSaved = true;
      if (E.assetBytes() > E.ASSET_TOTAL_BYTES) this.setState({ status: 'Art store is ' + Math.round(E.assetBytes() / 1024) + ' KB — above the ' + (E.ASSET_TOTAL_BYTES / 1e6).toFixed(1) + ' MB comfort line for this browser' });
    } catch (e) {
      this.artSaved = false;                                    // quota, private mode, file://
      this.setState({ status: 'Art will not survive a reload here: this browser refused to store it (' + (e && e.name ? e.name : 'storage error') + ')' });
    }
  }
  loadArt() {
    const E = window.WatchEngine;
    try {
      const raw = window.localStorage.getItem(this.ART_KEY);
      if (!raw) { this.artSaved = false; return; }
      const r = E.assetRestore(JSON.parse(raw)) || { added: 0, needBytes: 0 };
      this.artSaved = true;
      if (r.needBytes) this.setState({ status: r.needBytes + ' referenced art asset(s) had no bytes stored — re-upload or load the .wckit' });
    } catch (e) { this.artSaved = false; /* no storage: art is session-only, artNote says so */ }
  }
  /* An upload decodes off the main thread, so the render that asked for it is already
   * drawn without it. Drop the two caches that hold those pixels and let the normal
   * build path redraw: rebuilding the movement also re-prices the fps budget, whose
   * occlusion mask the new art changes. */
  artDirty() { this.faces = {}; this.base = null; this.gt = []; this.buildMovement(); this.forceUpdate(); }
  /* The one upload path for every slot: gate the file, register it, persist it, and
   * write the reference into the design. The status line carries the measured reason
   * when the engine refuses it. */
  pickArt(file, slot, apply) {
    const E = window.WatchEngine;
    if (!file) return;
    E.assetFromFile(file).then((a) => {
      if (!a || a.error) { this.setState({ status: a && a.error ? a.error : 'That file was not usable' }); return; }
      this.saveArt();
      apply(a.id);
      this.setState({ status: a.name + ' → ' + slot + ' (' + a.kind + ', ' + (a.dataUrl.length / 1024).toFixed(0) + ' KB) · ' + (this.artSaved ? 'saved' : 'session only') });
    });
  }
  resetTab(tab) {
    const drop = (keys) => this.setState((st) => { const c = { ...st.cfg }; keys.forEach((k) => delete c[k]); return { cfg: c }; });
    const P = this.preset(), say = (t) => this.setState({ status: t + ' reset' });
    if (tab === 'dial') { this.upd((d) => { const keep = { comps: d.comps, hands: d.hands, order: d.order, centre: d.centre }; Object.keys(d).forEach((k) => delete d[k]); Object.assign(d, JSON.parse(JSON.stringify(P.design)), keep); }); say('Dial'); }
    if (tab === 'comps') { this.upd((d) => { d.comps = JSON.parse(JSON.stringify(P.design.comps || [])); }); this.setState({ sel: null }); say('Complications'); }
    if (tab === 'hands') { this.upd((d) => { delete d.hands; delete d.order; delete d.secStyle; delete d.handLume; delete d.handShade; delete d.extra; ['perim', 'perimShrink', 'perimColor', 'perimFont', 'perimOrient', 'perimSize', 'perimPos'].forEach((k) => delete d[k]); if (P.design.handShade) d.handShade = P.design.handShade; if (P.design.handLume) d.handLume = P.design.handLume; if (P.design.secStyle) d.secStyle = P.design.secStyle; d.centre = JSON.parse(JSON.stringify(P.design.centre || {})); if (P.design.hands) d.hands = JSON.parse(JSON.stringify(P.design.hands)); }); this.setState({ hands: 'auto', handFinish: 'auto' }); drop(['tick', 'shAngle', 'shDist', 'shBlur', 'shOp']); say('Hands'); }
    if (tab === 'move') { drop(['rearFinish', 'plateAngle', 'plateScale', 'plate', 'spokes', 'etching', 'pinion', 'center', 'finish', 'balance', 'trainAngle', 'orbD', 'orbTip', 'cageD']); this.setState({ train: P.train || 'classic' }); say('Movement'); }
    if (tab === 'light') { drop(['glow', 'temp', 'innerAngle', 'outerLight', 'outerAngle', 'outerTemp', 'hue', 'hueSat', 'outerHue', 'outerSat', 'wb']); say('Light'); }
    if (tab === 'case') { this.setState({ caseStyle: P.caseStyle || 'polished', caseMetal: P.caseMetal || 'auto', glassStyle: 'domed' }); say('Case & glass'); }
  }
  roll(k) { this.setState((s) => ({ seeds: { ...s.seeds, [k]: Math.floor(Math.random() * 1e6) } })); }

  async componentDidMount() {
    const p = {}; this.KEYS.forEach((k) => { if (this.props && this.props[k] != null) p[k] = this.props[k]; });
    if (Object.keys(p).length) this.setState({ cfg: p });
    this.loadCal();
    window.__assetDirty = () => { if (this.ready) this.artDirty(); };
    // Escape on the stage deselects. Bound on the window rather than the canvas because
    // a canvas takes no focus: the key has to arrive whether the cursor is on the watch
    // or on the pane, and onKey ignores it while a field has it.
    this._onKey = (e) => { this.onKey(e); };
    window.addEventListener('keydown', this._onKey);
    while (!window.WatchEngine || !window.DialRenderer || !window.DialRenderer._) await new Promise((r) => setTimeout(r, 50));
    this.loadArt();                                       // needs the engine's registry, so after the wait
    const fams = ['700 31px "Cormorant Garamond"', 'italic 600 30px "Cormorant Garamond"', '600 40px "IBM Plex Sans"', '500 21px "IBM Plex Sans"', 'italic 600 30px "IBM Plex Sans"', '700 31px "Jost"', '500 21px "Jost"', 'italic 600 30px "Jost"', '700 31px "Barlow Condensed"', '500 21px "Barlow Condensed"', 'italic 600 30px "Barlow Condensed"', '700 31px "Bodoni Moda"', '500 21px "Bodoni Moda"', 'italic 600 30px "Bodoni Moda"'];
    try { await Promise.race([Promise.all(fams.map((f) => document.fonts.load(f))), new Promise((r) => setTimeout(r, 4000))]); } catch (e) {}
    this.ready = true;
    this.buildMovement(); this.startTimer(); this.forceUpdate();
  }
  componentWillUnmount() { clearInterval(this.timer); if (this._rmTimer) clearTimeout(this._rmTimer); if (this._onKey) window.removeEventListener('keydown', this._onKey); this.buildId = -1; }
  ensureFont(k) {
    this._fl = this._fl || {}; if (!k || this._fl[k]) return; this._fl[k] = 1;
    const n = WatchEngine.FONT_NAME(k); if (!n) return;
    Promise.race([Promise.all(['700 31px', '500 21px', 'italic 600 30px', '600 40px'].map((f) => document.fonts.load(f + ' "' + n + '"'))), new Promise((r) => setTimeout(r, 5000))])
      .catch(() => {}).then(() => { this.faces = {}; this._fk = null; this._tk = null; this.forceUpdate(); });
  }
  componentDidUpdate() {
    // A selection armed a scroll target in onDown; the rows it names only exist once
    // this commit has bound them, so this is the first place the jump can be honoured.
    if (this._scrollTo) { const a = this._scrollTo; this._scrollTo = null; this.scrollToAnchor(a); }
    if (!this.ready) return;
    const D0 = this.design(); [D0.font, D0.perimFont, D0.n12Font, D0.nQFont, ...D0.comps.flatMap((c) => [c.tfont, c.font, c.wfont])].forEach((k) => this.ensureFont(k));
    if (this._mk !== this.mvKey() || (!this.rendering && this.gt.length && this.gt.length !== this.B)) {
      /* The second clause is the self-heal for a superseded build: buildMovement
       * stamps _mk at its start, so a discarded build can leave the key matching
       * while the frames on screen still belong to the old bank. Without this the
       * mismatch hides from both branches and the timer's length guard strands
       * the movement. */
      this.buildGate();
    } else if (this._artKey !== this.artKey()) {
      /* Dial-only change: the base frames still describe the train exactly, and
       * only what the dial hides has moved. Re-price the rows against the cached
       * frames -- remeasureRows advances _artKey itself. Re-rendering the train
       * for a skeleton slider is what flickered. If the cache no longer matches
       * the tempo the frames must be re-rendered, and the key is only allowed to
       * move once the new number is priced: advancing it early would strand the
       * cap on a stale row count, because budget() holds the tempo for an art key
       * it has never priced rather than trust the pairing. */
      const st = this.remeasureSoon();
      if (st === 'stale') this.buildGate();                       // frames must be re-rendered
      /* Closing the dial can buy the tempo back, and the matching bank has to
       * re-render in this SAME commit -- leaving it shows one rate while the
       * frames on screen belong to another. A 'pending' trailing price re-checks
       * this on landing, in its own commit. */
      else if (st === 'priced' && this._mk !== this.mvKey()) this.buildGate();
    }
    /* The escapement cannot beat 4 times a second at a tempo that paints fewer
     * than 12 frames a second -- it just aliases. The chips hide the option, and
     * this drops a selection made while the dial was tighter. Reading this.F here
     * is safe (not inside a budget build) and the write is monotone -- balance
     * only falls to 2, so the precondition cannot return and re-fire. */
    if (this._mk === this.mvKey() && +this.cfg().balance > 2 && this.F < 12) this.setCfg('balance', '2');
    if (this._lastSlow !== this.state.slow || this._lastF !== this.F) this.startTimer();
    const fk = this.faceKey(this.state.dial) + this.state.timeMode;
    if (fk !== this._fk) { this._fk = fk; this.drawLayers(); this.drawHandOpts(); this.drawArtThumbs(); }
    const ck = JSON.stringify([this.opts(), this.state.dial, !!this.gt[0]]);
    if (ck !== this._ck && this.gt[0] && !this.drag) { this._ck = ck; this.caseThumbs(); }
    const tk = JSON.stringify([this.opts(), this.state.hands, this.state.handFinish, this.state.designs, !!this.gt[0]]);
    if (tk !== this._tk && this.gt[0] && !this.drag) { this._tk = tk; this.dialThumbs(); }
    if (this._tab !== this.state.tab) { this._tab = this.state.tab; this.drawHandOpts(); this.drawArtThumbs(); }
    this.drawMain();
  }
  startTimer() {
    clearInterval(this.timer);
    this._lastSlow = this.state.slow; this._lastF = this.F;
    const F = this.F;
    this.timer = setInterval(() => {
      if (this.state.playing && !this.rendering && this.gt.length === this.B) this.setState((s) => ({ frame: (s.frame + 1) % this.B }));
      else this.drawMain();
    }, this.state.slow ? 1000 / Math.max(1, F / 5) : 1000 / F);
  }
  /* Alpha of the layers that sit ABOVE the bg delta bank and never change during
   * a tick: the dial (SEALED over the date/moon apertures), the glass, the outer
   * ring. Those are the only layers that may excuse a row cull (bake_clock.py's
   * --bg-occlude). The dial is the fc.dialSealed canvas, not fc.dial: a dial-hosted
   * window or moon is an opaque data disc composited over its own aperture every
   * day, so the geartrain under it never reaches the panel and must not be priced
   * -- and the shipped layer, so the bake's opaque static1, seals it the same way.
   * A genuine skeleton opening keeps its hole (seal is never set on an 'opening'),
   * so an exposed train is still charged. The complication VALUE and the subdial
   * scales change when the date rolls and the hands are movers that lift off a
   * texel later, so they are not occluders. A design drawn through the inset lens
   * (fc.inset < 1) composites its statics through a scaled sub-canvas that flat()
   * does not reproduce, so the cull is skipped there rather than applied in the
   * wrong space: an all-zero alpha mask culls nothing and the row count falls back
   * to the old honest number. */
  occlMask() {
    const fc = this.face(), k = (fc && (fc.inset || 1) * (fc.dynK || 1)) || 1;
    /* The '|noface' tail matters: a mask built while the face is still missing is
     * all zeros, and caching it under the dial's own key would keep culling nothing
     * for the rest of the session after the dial finally arrives -- the row count
     * would then charge for a train the closed dial plainly hides. */
    const ck = this.faceKey(this.state.dial) + '|i' + k + (fc ? '' : '|noface');
    if (this._occKey === ck) return this._occ;
    const N = 466 * 466, occ = new Uint8Array(N);   // 1 = a single layer covers alone
    if (fc && k === 1) {
      const c = document.createElement('canvas'); c.width = c.height = 466;
      const g = c.getContext('2d');
      // Compositing the three and testing the RESULT's alpha would be wrong: 200
      // over 200 over 200 lands on 255, yet the panel still blends through every
      // one of those texels, so the row is not free. bg_occluder demands one layer
      // cover the texel BY ITSELF, so each layer is read back on its own and the
      // verdicts are OR'd. fc.dialSealed (not fc.dial) seals the date/moon apertures.
      [fc.dialSealed, fc.glass, fc.outer].forEach((L) => {
        if (!L) return;
        g.setTransform(1, 0, 0, 1, 0, 0); g.globalCompositeOperation = 'source-over';
        g.clearRect(0, 0, 466, 466); g.drawImage(L, 0, 0);
        const d = g.getImageData(0, 0, 466, 466).data;
        for (let t = 0, p = 3; t < N; t++, p += 4) if (d[p] >= 255) occ[t] = 1;
      });
    }
    let ncov = 0; for (let t = 0; t < N; t++) ncov += occ[t];
    this._occ = occ; this._occKey = ck; this._occCover = ncov;
    return this._occ;
  }
  /* The build gate, shared by both triggers in componentDidUpdate. budget()
   * lowering the export fps is the one mvKey field the build itself can move:
   * rows grow as the tempo drops, so the tier only ever walks down (_ftier) and
   * settles within a pass or two. This counter is the seatbelt that keeps a
   * pathological dial from rebuilding the movement forever.
   * The window opens per WANTED rate, not per art: tick is absent from artKey,
   * so a 4/s -> sweep -> 4/s round trip asks three or four legitimate rebuilds of
   * one unchanged art signature -- counted against a per-art budget of three, the
   * last flip never builds, gt.length no longer equals the bank B, and the frame
   * timer's guard freezes the movement on screen forever. */
  buildGate() {
    const ak = this.artKey() + '|' + this.cfg().tick;
    if (ak !== this._bk) { this._bk = ak; this._bp = 0; }
    if (this._bp < 5) { this._bp++; this.buildMovement(); }
  }
  async buildMovement() {
    const id = (this.buildId = (this.buildId || 0) + 1);
    this._mk = this.mvKey(); this._artKey = this.artKey();
    await new Promise((r) => setTimeout(r, 30));
    if (id !== this.buildId) return;
    const F = this.F, B = this.B, M = WatchEngine.movement(this.opts());
    /* Snapshot the movement signature the instant the art is built: the render
     * loop below awaits between frames, and a slider move in that window would
     * otherwise key these frames under a signature they were not rendered for.
     * Keyed to the art actually measured, stale stats simply fail the mvKey()
     * check in componentDidUpdate and the movement rebuilds -- self-correcting. */
    const mvk = this.mvKey(1);
    const start = Math.min(this.state.frame, B - 1), order = Array.from({ length: B }, (_, i) => (start + i) % B);
    const ng = new Array(B);
    this.base = M; this.rendering = true;
    this.setState({ seam: 'Rendering movement…', status: 'Rendering 1 / ' + B + '…', frame: start });
    for (let k = 0; k < B; k++) {
      if (id !== this.buildId) return;
      const f = order[k], r = M.render(f);
      ng[f] = r.gt; this.gt[f] = r.gt;
      if (k === 0) { this.gt.length = B; this.drawMain(); this.drawLayers(); }
      if (k % 2 === 1 || k === B - 1) this.setState({ status: 'Rendering ' + (k + 1) + ' / ' + B + '…' });
      await new Promise((r) => { const ch = new MessageChannel(); ch.port1.onmessage = () => r(); ch.port2.postMessage(0); });
    }
    if (id !== this.buildId) return;
    this.gt = ng; this.rendering = false;
    const end = M.render(B), a = this.scr(), b = document.createElement('canvas'); b.width = b.height = 466;
    const flat = (c, m, g, noCage) => { const x = c.getContext('2d'); x.setTransform(1, 0, 0, 1, 0, 0); x.clearRect(0, 0, 466, 466); x.drawImage(m, 0, 0); x.drawImage(g, 0, 0); if (g.cage && !noCage) x.drawImage(g.cage, 0, 0); return x.getImageData(0, 0, 466, 466).data; };
    const da = flat(a, M.plate, ng[0]), db = flat(b, M.plate, end.gt);
    let max = 0, n = 0;
    for (let i = 0; i < da.length; i += 4) { const d = Math.max(Math.abs(da[i] - db[i]), Math.abs(da[i + 1] - db[i + 1]), Math.abs(da[i + 2] - db[i + 2])); if (d > max) max = d; if (d > 2) n++; }
    const seam = n === 0 ? 'Seam check, frame ' + (B + 1) + ' vs 1: ✓ identical' : n < 50 ? 'Seam check: ✓ seamless (' + n + ' px rounding)' : 'Seam check: ✗ ' + n + ' px differ, max Δ ' + max;
    /* Device budget: rows that change between consecutive BASE frames. For a
     * carriage kit baseImgs() also composites the cage through a full minute
     * orbit (flatCar), so the carriage's swept rows enter this price and the
     * occluder cull drops whatever the dial seals -- an exposed carriage costs
     * rows, a sealed one costs none. The bg delta bank pushes exactly these
     * rows every tick, so the worst consecutive pair is the tick the flush fits. */
    let bgRows = 0;
    /* Read the dial and the occluder off the SAME commit as the price, and key
     * the number on the movement signature the frames were rendered for (mvk). */
    const occ = this.occlMask(), ak = this.artKey(), occk = this._occKey;
    try { bgRows = this.rowPrice(this.baseImgs(flat, M, ng, B, mvk), occ); }
    catch (e) { bgRows = -1; }
    if (bgRows >= 0) { this._artKey = ak; this.budgetStats = { mv: mvk, ak: ak, f: F, rows: bgRows, diag: this._rpDiag, occ: occk }; }
    else this.budgetStats = null;
    this._tk = null; this._ck = null;
    this.setState({ seam, status: '' });
    this.drawLayers();
  }
  /* The flattened base frames (plate + geartrain, no cage) the row diff runs
   * on, kept between builds. They depend on the movement art and the tempo only
   * -- the dial never touches them -- so a skeleton-slider move re-prices the
   * rows against these instead of re-rendering every frame, which is what the
   * flicker was. The caller passes the signature its frames were rendered for
   * (mvKey(1): the movement minus the export frame count, since a cap change
   * alone must not throw the cache away), never a live mvKey(). */
  isCarriage(ng) { const g = ng || this.gt; return !!(this.base && this.base.cageAt && g && g[0] && g[0].cage); }
  biKey(mvk, B, ng) { return mvk + '|' + B + (this.isCarriage(ng) ? '|car' : ''); }
  baseImgs(flat, M, ng, B, mvk) {
    /* Carriage kits price the carriage too: flatten the cage through a FULL
     * minute orbit, so the union of consecutive frames marks every device row
     * the carriage ever sweeps. The occluder cull in rowPrice() then drops the
     * rows the dial, glass and bezel cover -- an exposed carriage costs rows,
     * one opened ring at a time, and one sealed behind an opaque dial costs
     * none. Rotating the whole way round (not one tick's step) is deliberate:
     * it makes the bound independent of where in the minute the tick lands, and
     * rowPrice wants an upper bound that never underestimates. */
    const car = this.isCarriage(ng);
    const k = this.biKey(mvk, B, ng);
    if (this._bi && this._biKey === k && this._bi.length === B) return this._bi;
    const imgs = [];
    for (let f = 0; f < B; f++) { const cv = document.createElement('canvas'); cv.width = cv.height = 466; imgs.push(car ? this.flatCar(cv, M.plate, ng[f], (f / B) * Math.PI * 2) : flat(cv, M.plate, ng[f], true)); }
    this._bi = imgs; this._biKey = k;
    return imgs;
  }
  /* Plate + geartrain + the cage bitmap rotated th about the carriage staff.
   * Same composite drawCage() does on the panel, minus the live seconds angle:
   * the sweep across frames f=0..B-1 IS the orbit for pricing purposes. */
  flatCar(c, m, g, th) {
    const x = c.getContext('2d'); x.setTransform(1, 0, 0, 1, 0, 0); x.clearRect(0, 0, 466, 466);
    x.drawImage(m, 0, 0); x.drawImage(g, 0, 0);
    if (g.cage && this.base.cageAt) { const a = this.base.cageAt; x.save(); x.translate(a[0], a[1]); x.rotate(th); x.translate(-a[0], -a[1]); x.drawImage(g.cage, 0, 0); x.restore(); }
    return x.getImageData(0, 0, 466, 466).data;
  }
  /* Widest per-tick DMA'd row count over the bank, as the packer counts it.
   * A texel the layers in occlMask() cover at alpha 255 is dropped: the commit
   * hook paints that layer back over any row it rebuilds, so the pixel can never
   * reach the panel and the row must not be paid for (bake_clock.py's
   * --bg-occlude applies the identical cull at bake time, which is the gate).
   * Rows are then taken off the 16 px tile grid _pack_mask uses, and a frame
   * whose tiles cover >0.62 of the page is the packer's full-page collapse.
   * This is an UPPER bound on what the bank pushes: the composer cannot know
   * which statics the bake folds into the bank with --matte auto, and a folded
   * static paints the same pixels in every frame, so those rows never differ and
   * disappear from the bank anyway (measured on the synthetic closed dial: 432
   * uncullled, 64 here, fewer still once the dial folds). A bound that is never
   * underestimates is the only kind that cannot choose an fps the flush misses. */
  rowPrice(imgs, occ) {
    const n = imgs.length;                            // the BANK, not the tempo
    let worst = 0;
    /* Where the worst tick's rows sit, kept for the Movement readout. A solid dial
     * priced at a handful of rows is the signature of a sliver -- a tile row or two
     * surviving the cull -- and the bands name them instead of leaving it to guess. */
    this._rpDiag = { rows: 0, texels: 0, bands: [], rmin: 0, rmax: 0, cover: this._occCover };
    for (let f = 0; f < n; f++) {
      const p = imgs[f], q = imgs[(f + 1) % n], tm = new Uint8Array(30 * 30);
      let ch = 0, rmin = 999, rmax = 0;
      for (let y = 0; y < 466; y++) {
        const o = y * 466 * 4, ro = y * 466;
        for (let x = 0; x < 466; x++) {
          const i = o + x * 4;
          if (occ[ro + x]) continue;        // one static covers it alone at a=255
          if (Math.abs(p[i] - q[i]) > 2 || Math.abs(p[i + 1] - q[i + 1]) > 2 || Math.abs(p[i + 2] - q[i + 2]) > 2) { const t = (y >> 4) * 30 + (x >> 4); if (!tm[t]) { tm[t] = 1; ch++; const rr = Math.hypot(x - 233, y - 233); if (rr < rmin) rmin = rr; if (rr > rmax) rmax = rr; } }
        }
      }
      let nt = 0; for (let k = 0; k < tm.length; k++) nt += tm[k];
      let c, bands = null;
      if (nt / tm.length > 0.62) c = 466;
      else { c = 0; for (let ty = 0; ty < 30; ty++) { let a = 0; for (let tx = 0; tx < 30; tx++) a |= tm[ty * 30 + tx]; if (a) { c += Math.min(16, 466 - ty * 16); (bands || (bands = [])).push(ty); } } }
      if (c > worst) { worst = c; this._rpDiag = { rows: c, texels: ch, bands: bands || [], rmin: ch ? Math.round(rmin) : 0, rmax: ch ? Math.round(rmax) : 0, cover: this._occCover }; }
    }
    return worst;
  }
  /* Throttled front door to remeasureRows. A skeleton slider fires one React
   * commit per mousemove, and a price costs one getImageData plus a diff of
   * every exported frame -- running that inline would make the exact control
   * that flickered feel laggy. The wait is safe only because budget() quotes the
   * delivered tempo for an art key it has not priced: the number on screen stays
   * truthful while the trailing price is in flight, and it corrects itself when
   * the pointer settles. 'stale' means the cache does not describe the tempo, so
   * the frames themselves have to be re-rendered -- never deferred. */
  remeasureSoon() {
    if (this._rmTimer) return 'pending';
    const now = performance.now();
    if (now - (this._rmAt || 0) < 120) {
      this._rmTimer = setTimeout(() => {
        this._rmTimer = 0; this._rmAt = performance.now();
        if (!this.remeasureRows()) { this.buildGate(); return; }
        // The tempo may have just been earned back: the bank has to follow in this
        // same pass, or the readout quotes a rate the frame bank does not hold.
        if (this._mk !== this.mvKey()) this.buildGate();
        this.forceUpdate();                      // republish the readout with the new rows
      }, 120 - (now - (this._rmAt || 0)));
      return 'pending';
    }
    this._rmAt = now;
    return this.remeasureRows() ? 'priced' : 'stale';
  }
  /* Re-price the rows after a dial-only change: the cached frames still describe
   * the art, only the occlusion moved. Cheap enough to run from componentDidUpdate
   * -- one getImageData + a diff, no gear re-render. Advances _artKey itself, so
   * the caller never has to guess whether the number it is quoting is the current
   * one. Returns false when the cache does not describe the tempo being priced,
   * which means the frames themselves must be re-rendered. */
  remeasureRows() {
    const F = this.F, B = this.B;
    if (!this._bi || this._bi.length !== B || this._biKey !== this.biKey(this.mvKey(1), B)) return false;
    const occ = this.occlMask(), ak = this.artKey(), occk = this._occKey;
    try {
      const r = this.rowPrice(this._bi, occ);
      this.budgetStats = { mv: this.mvKey(1), ak: ak, f: F, rows: r, diag: this._rpDiag, occ: occk };
      this._artKey = ak;
      return true;
    } catch (e) { return false; }
  }
  drawMain() {
    const c = this.mainRef.current, fc = this.face(); if (!c || !fc) return;
    const f = this.state.frame, gt = this.gt[f] || this.gt.find(Boolean), d = this.now(), x = c.getContext('2d');
    WatchEngine.compose(x, this.base, gt, fc, d, this.state.hidden);
    if (+this.cfg().wb !== 6500) WatchEngine.wb(c, +this.cfg().wb);
    const o = this.state.sel && this.state.tab === 'comps' && fc.outline(this.state.sel), k = ((fc.inset || 1) * (fc.dynK || 1));
    if (o) {
      x.save(); x.setLineDash([5, 4]); x.lineWidth = 1.5; x.strokeStyle = 'rgba(240,215,154,.95)'; x.shadowColor = 'rgba(0,0,0,.8)'; x.shadowBlur = 3;
      x.beginPath(); x.arc(233 + (o.x - 233) * k, 233 + (o.y - 233) * k, (o.r + 3) * k, 0, Math.PI * 2); x.stroke(); x.restore();
    }
    if (this.guideOn()) this.drawGuide(x, fc, d, k);
    const s = d.getSeconds();
    if (s !== this._sec) { this._sec = s; this.drawLayer('hands'); if (this.base && this.base.cageAt) this.drawLayer('geartrain'); }
  }
  guideOn() {
    const s = this.state;
    if (s.guide === 'off') return false;
    if (s.guide === 'on') return true;
    if (s.tab === 'case' && s.caseStyle === 'none') return true;   // the opening's gold ring is how the author sets where the glass ends
    return !!this.drag || s.selKind === 'dial' || s.selKind === 'hand';
  }
  /* Two ways out of the overlay, because they answer two different questions. Clearing
   * the selection is the one that matters: with the dial selected, EVERY pixel inside
   * the dial belongs to it, so "click somewhere empty" cannot deselect it -- there is no
   * empty pixel to click. Escape does it, and the button under the stage says so. The
   * guide toggle is the other half: it hides the overlay while the selection is kept,
   * for the author who wants to place by eye first and check the numbers after. */
  clearSel() { this.drag = null; this._clickAt = null; this._pend = this._pendArt = this._pendHand = null; this.setState({ sel: null, selKind: null }); }
  onKey(e) {
    if (!e || e.key !== 'Escape') return false;
    const t = e.target || {}, tag = String(t.tagName || '').toLowerCase();
    if (tag === 'input' || tag === 'select' || tag === 'textarea') return false;   // Escape in a field is the field's business
    const s = this.state;
    if (!this.drag && s.sel == null && s.selKind == null && s.guide === 'auto') return false;
    this.drag = null; this._clickAt = null; this.setState({ sel: null, selKind: null, guide: 'auto' });
    return true;
  }
  cycleGuide() { this.setState({ guide: this.state.guide === 'auto' ? 'on' : this.state.guide === 'on' ? 'off' : 'auto' }); }
  /* The alignment overlay. It is on while it is answering the question being asked:
   * during a drag, while the dial art or a hand is selected (the two things positioned
   * by eye), and whenever the author pins it on. Otherwise a look at the watch stays a
   * look at the watch. Everything it draws comes out of the engine's own geometry --
   * fc.caseOpen, artBox, handAngle -- so the guide and the pixels cannot disagree.
   *
   * Two things are drawn here and they must never be confused, so they never share a
   * colour. The REFERENCE (the opening, the 15-degree spokes, the range rings, the
   * complication centres, the hub cross) is pinned to the WATCH: it is the fixed thing
   * an off-centre graphic gets walked onto, so it cannot be allowed to ride along with
   * the thing being moved. The PLACED ITEM (art frame, art centre, hand ghost) is drawn
   * in blue and does move, because it is the thing under the cursor. */
  drawGuide(x, fc, d, k) {
    const E = window.WatchEngine, D = this.design() || {}, TAU = Math.PI * 2;
    const px = (v) => 233 + (v - 233) * k;                       // dial space -> composited screen space
    const OP = fc.caseOpen == null ? 221.5 : fc.caseOpen;        // where the device's metal starts
    x.save(); x.shadowColor = 'rgba(0,0,0,.85)'; x.shadowBlur = 3;
    // The mask cuts here in panel space, so the ring is drawn there and not through the
    // face's own scale: asking the author to line art up to a ring the metal is not on
    // would be a guide that lies.
    x.lineWidth = 1; x.strokeStyle = 'rgba(240,215,154,.55)';
    x.beginPath(); x.arc(233, 233, OP, 0, TAU); x.stroke();
    x.setLineDash([3, 5]); x.lineWidth = 1; x.strokeStyle = 'rgba(240,215,154,.3)';
    for (let i = 0; i < 24; i++) {                               // 15 degree spokes, out of the hub: the dial's own grid, and the axis an off-centre print is set by
      const a = (i / 24) * TAU;
      x.beginPath(); x.moveTo(233, 233); x.lineTo(233 + Math.sin(a) * 216, 233 - Math.cos(a) * 216); x.stroke();
    }
    [0.25, 0.5, 0.75].forEach((f) => {                           // range rings, so radius is as readable as angle
      x.beginPath(); x.arc(233, 233, OP * f, 0, TAU); x.stroke();
    });
    if (this.state.selKind === 'dial' && fc.lay && fc.lay.G) {   // the centres the graphic is usually being aimed at
      Object.keys(fc.lay.G).forEach((id) => {
        const g = fc.lay.G[id]; if (!g || g.x == null) return;
        const gx = px(g.x), gy = px(g.y);
        x.beginPath(); x.moveTo(gx - 7, gy); x.lineTo(gx + 7, gy); x.moveTo(gx, gy - 7); x.lineTo(gx, gy + 7); x.stroke();
        x.beginPath(); x.arc(gx, gy, 3, 0, TAU); x.stroke();
      });
    }
    x.setLineDash([]); x.lineWidth = 1.5; x.strokeStyle = 'rgba(240,215,154,.95)';
    x.beginPath(); x.moveTo(220, 233); x.lineTo(246, 233); x.moveTo(233, 220); x.lineTo(233, 246); x.stroke();
    x.beginPath(); x.arc(233, 233, 3.5, 0, TAU); x.stroke();     // THE hub: fixed, and not the art's centre
    const slot = (this.state.selKind === 'dial' && ((this.drag && this.drag.kind === 'dial' && this.drag.slot) || this.artSlot())) || null;
    if (slot && D[slot + 'Art'] && E.artBox) {
      // The uploaded image is never eased in by the opening (see the engine's dynScale):
      // the aim box is drawn at the art's own scale, which is the case inset, not the
      // composited scale this overlay otherwise maps with.
      const kA = (fc.inset == null ? k : fc.inset) || 1;
      const pxa = (v) => 233 + (v - 233) * kA;
      const b = E.artBox(D[slot + 'Art'], { x: 233 + (+D[slot + 'X'] || 0), y: 233 + (+D[slot + 'Y'] || 0), w: D[slot + 'Size'] == null ? 442 : +D[slot + 'Size'], h: D[slot + 'Size'] == null ? 442 : +D[slot + 'Size'], fit: D[slot + 'Fit'] || 'cover', zoom: D[slot + 'Zoom'] == null ? 1 : +D[slot + 'Zoom'] });
      // The engine spins the art about its own centre (drawAsset's rot); the frame owes
      // the author the same turn about the same point, or the guide would lie about the crop.
      const rotA = (Math.round(+D[slot + 'Rot'] || 0) * Math.PI) / 180;
      const ox = pxa(233 + (+D[slot + 'X'] || 0)), oy = pxa(233 + (+D[slot + 'Y'] || 0));
      x.save();
      if (rotA) { x.translate(ox, oy); x.rotate(rotA); x.translate(-ox, -oy); }
      x.setLineDash([]); x.lineWidth = 1.25; x.strokeStyle = 'rgba(122,214,255,.9)';
      const L = pxa(b.x - b.w / 2), T = pxa(b.y - b.h / 2), W = b.w * kA, H = b.h * kA;
      x.strokeRect(L, T, W, H);
      x.lineWidth = 2.5; x.strokeStyle = 'rgba(122,214,255,1)';   // corner ticks: the box reads even over busy art
      [[0, 0, 1, 1], [W, 0, -1, 1], [0, H, 1, -1], [W, H, -1, -1]].forEach((q) => { x.beginPath(); x.moveTo(L + q[0], T + q[1]); x.lineTo(L + q[0] + 9 * q[2], T + q[1]); x.moveTo(L + q[0], T + q[1]); x.lineTo(L + q[0], T + q[1] + 9 * q[3]); x.stroke(); });
      x.setLineDash([3, 4]); x.lineWidth = 1; x.strokeStyle = 'rgba(122,214,255,.55)';
      x.beginPath(); x.moveTo(ox - 30, oy); x.lineTo(ox + 30, oy); x.moveTo(ox, oy - 30); x.lineTo(ox, oy + 30); x.stroke();
      x.beginPath(); x.arc(ox, oy, 3, 0, TAU); x.stroke();         // the art's own centre, which is NOT the hub
      x.restore();
    }
    if (this.state.selKind === 'hand') this.drawHandGhost(x, fc, d, k, px);
    x.restore();
  }
  /* The virtual hand: where the selected hand is, and where the arbour it turns about
   * sits. The dashed box is the sprite after its own turn AND the author's facing
   * correction; the cross is the pivot the device puts on the centre; the ring is how
   * far the ink sweeps, which is the box the device must dirty every tick; the ray is
   * the line the CLOCK says this hand indicates -- the data angle plus the tilt of the
   * subdial it rides on, and never the facing. That is deliberate: Facing is the author
   * swinging the picture under a standing sightline, so an art drawn across the movement
   * shows its tip on the ray exactly when the facing is right, and which slice of the
   * art lands on the ray is the slice that truly indicates. If the ray rode with the
   * facing too it would say "still perfect" at every wrong angle. */
  drawHandGhost(x, fc, d, k, px) {
    const E = window.WatchEngine, dr = this.drag;
    const hd = (fc.hands || []).find((q) => q.id === this.state.sel);
    if (!hd || !hd.sp) return;
    const ang = dr && dr.kind === 'hand' && dr.id === hd.id ? dr.ang : (E.handAngle ? E.handAngle(hd, d) : hd.turn(d) * Math.PI * 2);
    const sp = hd.sp, w = sp.c.width, h = sp.c.height;
    let reach = 0;
    [[-sp.px, -sp.py], [w - sp.px, -sp.py], [-sp.px, h - sp.py], [w - sp.px, h - sp.py]].forEach((q) => { reach = Math.max(reach, Math.hypot(q[0], q[1])); });
    x.save(); x.translate(px(hd.x), px(hd.y));
    x.setLineDash([3, 5]); x.lineWidth = 1; x.strokeStyle = 'rgba(240,215,154,.4)';
    x.beginPath(); x.arc(0, 0, reach * k, 0, Math.PI * 2); x.stroke();          // the sweep envelope
    x.rotate(ang); x.scale(k, k);
    x.setLineDash([4, 3]); x.lineWidth = 1.25 / k; x.strokeStyle = 'rgba(122,214,255,.95)';
    x.strokeRect(-sp.px, -sp.py, w, h);                                          // the sprite, turned
    x.save(); x.rotate(-(hd.artRot || 0));                                       // off the facing, back onto the time
    x.strokeStyle = 'rgba(240,215,154,.8)'; x.lineWidth = 1 / k;
    x.beginPath(); x.moveTo(0, 0); x.lineTo(0, -(reach - 6)); x.stroke();        // where it should read
    x.restore();
    x.strokeStyle = '#f0d79a'; x.lineWidth = 1.6 / k; x.setLineDash([]);         // the arbour itself
    x.beginPath(); x.moveTo(-7 / k, 0); x.lineTo(7 / k, 0); x.moveTo(0, -7 / k); x.lineTo(0, 7 / k); x.stroke();
    x.beginPath(); x.arc(0, 0, 3 / k, 0, Math.PI * 2); x.stroke();
    x.restore();
  }
  /* Screen -> dial space. Complications and hands are drawn through the composited scale
   * (inset x dynK), so clicks map back through it; the two art slots are not -- under "no
   * case" the opening moves every other pixel and leaves the author's image exactly where
   * they put it -- so art pans map back through the art's own scale (the case inset). */
  ptK(e, k) { const r = e.currentTarget.getBoundingClientRect(); const x = ((e.clientX - r.left) / r.width) * 466, y = ((e.clientY - r.top) / r.height) * 466; return [233 + (x - 233) / k, 233 + (y - 233) / k]; }
  pt(e) { const fc = this.face(); return this.ptK(e, ((fc && fc.inset) || 1) * ((fc && fc.dynK) || 1)); }
  ptArt(e) { const fc = this.face(); return this.ptK(e, (fc && fc.inset) || 1); }
  /* Which of the two art slots a dial drag pans. Asked outright rather than guessed:
   * both can hold art at once, and quietly moving the wrong one wastes more of the
   * author's time than one chip does. */
  artSlot() {
    const D = this.design() || {}, s = this.state.artSlot, has = (p) => !!D[p + 'Art'];
    if (has(s)) return s;
    return has('sf') ? 'sf' : has('bg') ? 'bg' : null;
  }
  /* Selecting is not only a highlight: the Editing Pane jumps to the pane that can
   * change what was grabbed, because a selected hand under the Dial tab is a tool that
   * cannot touch the thing under the cursor. Switching the tab is the half that was
   * missing: the Hands tab opens at its Default-style grid and the selected hand's own
   * row -- pivot fields, facing, sprite -- sits well below the fold, so the pane the
   * author arrived at still showed none of it. Each place that can be jumped to carries
   * a data-a anchor, and the target is decided here rather than in the DOM so the pair
   * can be checked against the anchors renderVals actually emits. */
  selTab = { comp: 'comps', dial: 'dial', hand: 'hands' };
  scrollTargetFor(t) {
    if (!t) return null;
    const D = this.design() || {};
    if (t.kind === 'hand' || t.kind === 'comp') return t.id ? t.kind + '-' + t.id : null;
    if (t.kind === 'dial') { const p = this.artSlot(); return p && D[p + 'Art'] ? 'art-' + p : '#'; }
    return null;
  }
  scrollTarget() { const s = this.state; return s.selKind ? this.scrollTargetFor({ kind: s.selKind, id: s.sel }) : null; }
  /* '#' means the top of the pane. The retry loop is not decoration: Declarative
   * Components rebinds the field list after this component's own commit, so the row can
   * legitimately not exist yet in the frame the selection landed. */
  scrollToAnchor(a, tries) {
    const pane = this.paneRef && this.paneRef.current;
    if (!pane || !a || typeof pane.querySelector !== 'function') return false;
    if (a === '#') { pane.scrollTop = 0; return true; }
    const el = pane.querySelector('[data-a="' + a.replace(/["\\]/g, '') + '"]');
    if (!el) { if ((tries || 0) < 8) setTimeout(() => this.scrollToAnchor(a, (tries || 0) + 1), 16); return false; }
    const pr = pane.getBoundingClientRect(), er = el.getBoundingClientRect();
    if (er.top - pr.top < 12 || er.bottom - pr.bottom > -24) { pane.scrollTop += (er.top - pr.top) - 12; return true; }
    return true;
  }
  onDown(e) {
    const fc = this.face(); if (!fc) return;
    const d = this.now(), [x, y] = this.pt(e);
    /* Where several things overlap, a second click on the same spot steps down the
     * stack -- the layer picker every design tool has. Without it a 12 o'clock date
     * window is unreachable for the whole life of the seconds hand that sweeps it,
     * because that hand is drawn over it and so always wins the pixel.
     * A complication already picked on the Complications tab wins that pixel outright
     * on a fresh click: the author selected it on purpose, reached for it on the dial,
     * and means to drag IT, not the hand someone else drew on top of it. The same-spot
     * cycle still runs from wherever the pick landed, so nothing below stays hidden. */
    const stack = fc.pickAll ? fc.pickAll(x, y, d) : (function () { const id = fc.hit(x, y); return id ? [{ kind: 'comp', id }] : []; })();
    const last = this._clickAt;
    let i = 0;
    if (stack.length > 1 && last && Math.hypot(last.x - x, last.y - y) < 4) {
      const at = stack.findIndex((q) => q.kind === last.kind && q.id === last.id);
      if (at >= 0) i = (at + 1) % stack.length;
    } else if (stack.length > 1 && this.state.selKind === 'comp' && this.state.sel != null) {
      const at = stack.findIndex((q) => q.kind === 'comp' && String(q.id) === String(this.state.sel));
      if (at > 0) i = at;
    }
    const t = stack[i] || null;
    this._clickAt = t ? { x, y, kind: t.kind, id: t.id } : null;
    this._scrollTo = this.scrollTargetFor(t);
    const grab = () => { try { e.currentTarget.setPointerCapture(e.pointerId); } catch (err) {} };
    this.drag = null;
    if (!t) return this.setState({ sel: null, selKind: null });
    const jump = { sel: t.id, selKind: t.kind, tab: this.selTab[t.kind] };
    if (t.kind === 'hand') {
      const hd = (fc.hands || []).find((q) => q.id === t.id);
      // Only an image-backed hand has an arbour for the author to place: a procedural
      // sprite is built to its own box from the style's geometry, so its pivot is the
      // engine's. Selecting one still opens its pane, where the upload lives.
      if (hd && hd.art) {
        const hc = ((this.design() || {}).hands || {})[hd.id] || {}, turn = hd.turn(d) * Math.PI * 2, sc = hd.sp.scale || 1;
        const given = hc.pivotX != null && hc.pivotY != null;
        this.drag = { kind: 'hand', id: hd.id, px: x, py: y, turn, ang: turn + (hd.rot || 0) + (hd.artRot || 0), k: sc, sx: given ? +hc.pivotX : hd.sp.px / sc, sy: given ? +hc.pivotY : hd.sp.py / sc };
      }
      grab(); return this.setState(jump);
    }
    if (t.kind === 'dial') {
      const p = this.artSlot();
      if (p) { const D = this.design() || {}; const [ax, ay] = this.ptArt(e); this.drag = { kind: 'dial', slot: p, px: ax, py: ay, sx: +D[p + 'X'] || 0, sy: +D[p + 'Y'] || 0 }; }
      grab(); return this.setState(jump);
    }
    const g = fc.lay.G[t.id];
    if (g) this.drag = { kind: 'comp', id: t.id, ox: x - g.x, oy: y - g.y };
    grab(); this.setState(jump);
  }
  onMove(e) {
    const dr = this.drag; if (!dr) return;
    const fc = this.face(); if (!fc) return;
    const [x, y] = dr.kind === 'dial' ? this.ptArt(e) : this.pt(e);
    if (dr.kind === 'dial') {
      this._pendArt = { p: dr.slot, x: Math.round(dr.sx + (x - dr.px)), y: Math.round(dr.sy + (y - dr.py)) };
    } else if (dr.kind === 'hand') {
      /* p' = p - R(-θ)Δ / k. Grab the picture and the point of it that has to sit on
       * the arbour moves the other way; the hand's own turn is undone first, or
       * dragging a hand that reads at 6 would slide its pivot along the wrong axis.
       * The draw holds that angle for the drag (see drawLayer), so the geometry under
       * the cursor cannot rotate out from under it while the seconds sweep. */
      const dx = x - dr.px, dy = y - dr.py, c = Math.cos(-dr.ang), s = Math.sin(-dr.ang);
      this._pendHand = { id: dr.id, pivotX: +((dr.sx - (dx * c - dy * s) / dr.k).toFixed(1)), pivotY: +((dr.sy - (dx * s + dy * c) / dr.k).toFixed(1)) };
    } else {
      const g = fc.lay.G[dr.id]; if (!g) return;
      const nx = x - dr.ox, ny = y - dr.oy, dx = nx - g.hx, dy = ny - g.hy;
      const dist = Math.round(Math.hypot(dx, dy) * 2) / 2, ang = Math.round(((((Math.atan2(dx, -dy) - g.hrot) * 180) / Math.PI) % 360 + 360) % 360);
      const c = this.design().comps.find((q) => q.id === dr.id);
      this._pend = c && c.type === 'opening' && c.show === 'balance' ? { angle: ang } : { angle: ang, dist };
    }
    if (!this._raf) this._raf = requestAnimationFrame(() => { this._raf = null; this.flushDrag(); });
  }
  flushDrag() {
    const dr = this.drag; if (!dr) return;
    if (this._pend) { this.setComp(dr.id, this._pend); this._pend = null; }
    if (this._pendArt) { const a = this._pendArt; this._pendArt = null; this.upd((d) => { d[a.p + 'X'] = a.x; d[a.p + 'Y'] = a.y; }); }
    if (this._pendHand) { const p = this._pendHand; this._pendHand = null; this.setHand(p.id, { pivotX: p.pivotX, pivotY: p.pivotY }); }
  }
  onUp() { if (this.drag) { this.flushDrag(); this.drag = null; this._tk = null; this._ck = null; this.forceUpdate(); } }
  /* A click on the black around the watch, not on the watch, is the author putting the
   * pen down: whatever was picked on the tabs or the stage is dropped. The canvas' own
   * pointerdown bubbles through this handler too, so only a click whose target is the
   * surround itself counts; an empty pixel INSIDE the canvas already deselects. */
  stageDown(e) {
    if (!e || e.target !== e.currentTarget) return;
    if (this.state.sel == null && this.state.selKind == null && !this.drag) return;
    this.clearSel();
  }
  drawLayers() { this.LAYERS.forEach(([id]) => this.drawLayer(id)); }
  drawLayer(id) {
    const el = this.layerEls[id], fc = this.face(); if (!el || !fc) return;
    const x = el.getContext('2d'), sc = this.scr(), s = sc.getContext('2d'), d = this.now();
    s.setTransform(1, 0, 0, 1, 0, 0); s.globalCompositeOperation = 'source-over'; s.clearRect(0, 0, 466, 466);
    if (id === 'movement' && this.base) { s.drawImage(this.base.plate, 0, 0); if (fc.bgL) s.drawImage(fc.bgL, 0, 0); if (fc.plateFlares) s.drawImage(fc.plateFlares, 0, 0); }
    if (id === 'geartrain' && this.gt[0]) { s.drawImage(this.gt[0], 0, 0); WatchEngine.drawCage(s, this.base, this.gt[0], d); }
    if (id === 'complications') { const cp = fc.comps(d); s.drawImage(cp.dial, 0, 0); Object.values(cp.subs).forEach((c) => s.drawImage(c, 0, 0)); }
    if (id === 'dial') s.drawImage(fc.dial, 0, 0);
    if (id === 'subdials') fc.subLayers.forEach((l) => s.drawImage(l.canvas, 0, 0));
    // While a pivot drag is under way the dragged hand holds its angle: the drag maps
    // the cursor into the sprite's own frame, and a seconds hand sweeping underneath
    // would rotate that frame out from under the pointer mid-stroke. Set and cleared
    // around the one call, so a face in the cache is never left holding it.
    if (id === 'hands') {
      const dr = this.drag, on = dr && dr.kind === 'hand' ? (fc.hands || []).filter((h) => h.id === dr.id) : [];
      on.forEach((h) => { h.freeze = dr.turn; });
      fc.drawHands(s, d);
      on.forEach((h) => { delete h.freeze; });
    }
    if (id === 'glass') { s.fillStyle = '#2a2622'; s.beginPath(); s.arc(233, 233, 221, 0, Math.PI * 2); s.fill(); s.drawImage(fc.glass, 0, 0); }
    if (id === 'outer') {
      s.drawImage(fc.outer, 0, 0);
      // Under "no case" this layer is legitimately empty, and a bare blank square in the
      // isolate reads as a broken render. Say what the empty means: here is the rim the
      // device owns, and there is no pixel of ours on it.
      if (this.state.caseStyle === 'none') {
        s.setLineDash([5, 4]); s.lineWidth = 1.5; s.strokeStyle = 'rgba(240,215,154,.85)';
        s.beginPath(); s.arc(233, 233, fc.caseOpen == null ? 221.5 : fc.caseOpen, 0, Math.PI * 2); s.stroke();
        s.setLineDash([]);
        s.font = "600 13px 'IBM Plex Mono',ui-monospace,monospace"; s.fillStyle = 'rgba(240,215,154,.9)';
        s.fillText('no case drawn', 178, 30);
      }
    }
    x.clearRect(0, 0, el.width, el.height); x.drawImage(sc, 0, 0, el.width, el.height);
  }
  drawDialThumb(id) {
    const el = this.dialEls[id]; if (!el || !this.gt[0] || !this.ready) return;
    const fc = this.face(id), sc = this.scr(), d = new Date(); d.setHours(10, 9, 36, 0);
    WatchEngine.compose(sc.getContext('2d'), this.base, this.gt[0], fc, d, {});
    el.getContext('2d').drawImage(sc, 0, 0, el.width, el.height);
  }
  dialThumbs() {
    const tok = this._tk, ids = Object.keys(WatchEngine.PRESETS);
    const run = (i) => { if (i >= ids.length || tok !== this._tk) return; setTimeout(() => { if (tok !== this._tk) return; this.drawDialThumb(ids[i]); run(i + 1); }, 40); };
    run(0);
  }
  caseThumbs() {
    const tok = this._ck, E = WatchEngine, jobs = [...Object.keys(E.CASE_STYLES).map((k) => ['case', k]), ...Object.keys(E.GLASS_STYLES).map((k) => ['glass', k])];
    const run = (i) => { if (i >= jobs.length || tok !== this._ck) return; setTimeout(() => { if (tok !== this._ck) return; this.drawCaseThumb(...jobs[i]); run(i + 1); }, 30); };
    run(0);
  }
  drawCaseThumb(kind, id) {
    const el = (kind === 'case' ? this.caseEls : this.glassEls)[id], fc = this.face(); if (!el || !fc || !this.gt[0]) return;
    const E = WatchEngine, sc = this.scr(), x = sc.getContext('2d'), d = new Date(), o = { ...this.opts() }; d.setHours(10, 9, 36, 0);
    E.compose(x, this.base, this.gt[0], fc, d, { glass: 1, outer: 1 });
    const out = el.getContext('2d'); out.clearRect(0, 0, 112, 112);
    if (kind === 'case') {
      // "No case" composites the dial and marks where the device's own metal takes
      // over. Rendered honestly it would be an empty chip, which reads as a broken
      // thumbnail rather than as the one option that draws nothing.
      if (id === 'none') {
        const r = E.caseOpenR ? E.caseOpenR(o) : 221.5;
        out.drawImage(sc, 0, 0, 466, 466, 0, 0, 112, 112);
        out.setLineDash([4, 3]); out.lineWidth = 1.5; out.strokeStyle = 'rgba(240,215,154,.85)';
        out.beginPath(); out.arc(56, 56, (r / 233) * 56, 0, Math.PI * 2); out.stroke(); out.setLineDash([]);
        return;
      }
      x.drawImage(E.caseLayer({ ...o, caseStyle: id }), 0, 0); out.drawImage(sc, 168, -2, 130, 130, 0, 0, 112, 112);
    }
    else { x.drawImage(E.glassLayer({ ...o, glassStyle: id }), 0, 0); x.drawImage(fc.outer, 0, 0); out.drawImage(sc, 0, 0, 466, 466, 0, 0, 112, 112); }
  }
  drawHandOpt(id) {
    const el = this.handEls[id], fc = this.face(); if (!el || !fc) return;
    const style = id === 'auto' ? this.preset().hands : id;
    WatchEngine.handPreview(el.getContext('2d'), style, fc.handFinish, el.width, fc.dialEn ? fc.dialEn.a : fc.en.a);
  }
  drawHandOpts() { if (!this.ready) return; ['auto', ...Object.keys(WatchEngine.HAND_STYLES)].forEach((id) => this.drawHandOpt(id)); }
  /* The art-backed hand's own thumbnail: the exact canvas the kit will ship, on a checker
   * so "transparent" is visible rather than assumed, with a cross marking the pivot --
   * the point the device rotates about, sampled with antialiasing off. Click or drag to
   * author it; the click position is divided back to SOURCE pixels, which is what the
   * design stores, so resizing the art later keeps the pivot on the same ink. */
  drawArtThumb(id) {
    const el = this.artEls[id]; if (!el) return;
    const W = el.width, x = el.getContext('2d');
    x.setTransform(1, 0, 0, 1, 0, 0); x.clearRect(0, 0, W, W);
    for (let j = 0; j < W; j += 8) for (let i = 0; i < W; i += 8) { x.fillStyle = ((i / 8 + j / 8) % 2) ? '#1b1916' : '#231f1a'; x.fillRect(i, j, 8, 8); }
    const fc = this.face(), hd = fc && fc.hands.find((q) => q.id === id), sp = hd && hd.sp;
    if (!sp) { x.fillStyle = '#8d8472'; x.font = '500 9px "IBM Plex Sans", sans-serif'; x.fillText('no hand', 4, 12); return; }
    const k = Math.min(W / sp.c.width, W / sp.c.height) * 0.94, ox = (W - sp.c.width * k) / 2, oy = (W - sp.c.height * k) / 2;
    x.imageSmoothingEnabled = false;                                      // the device rotates nearest-neighbour; preview it that way
    x.drawImage(sp.c, ox, oy, sp.c.width * k, sp.c.height * k);
    /* The plane is drawn upright and unrotated on purpose: this is the authoring space,
     * where the pivot is placed on the hub by eye and a click maps straight to a pixel of
     * the image. Where the padding ring is, the arbour sits outside the ink -- the faint
     * frame is the ink itself, so the gap between the two is visible instead of being a
     * number that has to be read off in the panel. */
    if (hd.art && sp.ink) {
      x.strokeStyle = 'rgba(122,214,255,.5)'; x.lineWidth = 1;
      x.strokeRect(ox + sp.ink[0] * k, oy + sp.ink[1] * k, (sp.ink[2] - sp.ink[0]) * k, (sp.ink[3] - sp.ink[1]) * k);
    }
    x.strokeStyle = '#c9a25a'; x.lineWidth = 1; x.beginPath();
    x.moveTo(ox + sp.px * k - 6, oy + sp.py * k); x.lineTo(ox + sp.px * k + 6, oy + sp.py * k);
    x.moveTo(ox + sp.px * k, oy + sp.py * k - 6); x.lineTo(ox + sp.px * k, oy + sp.py * k + 6); x.stroke();
    /* Which way the device will make this hand read, measured from the arbour: the art
     * has to lie along it. If the ink runs off in another direction, Facing is wrong by
     * the angle between the two. Drawn in the thumb because that is where the facing is
     * typed; the stage's virtual hand is the same ray at full size. */
    if (hd.art) {
      let reach = 0;
      [[0, 0], [sp.c.width, 0], [0, sp.c.height], [sp.c.width, sp.c.height]].forEach((q) => { reach = Math.max(reach, Math.hypot(q[0] - sp.px, q[1] - sp.py)); });
      const a = ((hd.rot0 || 0) - 90) * Math.PI / 180;                    // 0 deg facing means the art points up
      x.strokeStyle = 'rgba(240,215,154,.85)'; x.setLineDash([3, 3]); x.lineWidth = 1.25; x.beginPath();
      x.moveTo(ox + sp.px * k, oy + sp.py * k);
      x.lineTo(ox + sp.px * k + Math.cos(a) * reach * k, oy + sp.py * k + Math.sin(a) * reach * k);
      x.stroke(); x.setLineDash([]);
    }
    el._m = { k, ox, oy, s: (hd.art && hd.art.scale) || 1 };
  }
  drawArtThumbs() { if (!this.ready) return; Object.keys(this.artEls).forEach((id) => this.drawArtThumb(id)); }
  artThumbSet(e, id) {
    const el = this.artEls[id], m = el && el._m; if (!m) return;
    const r = el.getBoundingClientRect();
    const sx = (e.clientX - r.left) * (el.width / r.width), sy = (e.clientY - r.top) * (el.height / r.height);
    // No clamp to the image: the point that must sit on the arbour can legitimately be
    // outside the ink, and the engine grows the plane to hold it.
    this.setHand(id, { pivotX: +((sx - m.ox) / m.k / m.s).toFixed(1), pivotY: +((sy - m.oy) / m.k / m.s).toFixed(1) });
  }
  download(blob, name) {
    const a = document.createElement('a'); a.href = URL.createObjectURL(blob); a.download = name;
    document.body.appendChild(a); a.click(); a.remove(); setTimeout(() => URL.revokeObjectURL(a.href), 4000);
  }
  png(c, raw) {
    const K = +this.cfg().wb;
    if (!raw && K !== 6500) { const o = document.createElement('canvas'); o.width = c.width; o.height = c.height; o.getContext('2d').drawImage(c, 0, 0); c = WatchEngine.wb(o, K); }
    return new Promise((r) => c.toBlob(async (b) => r(new Uint8Array(await b.arrayBuffer())), 'image/png')); }
  zip(files) {
    const T = new Uint32Array(256);
    for (let n = 0; n < 256; n++) { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; T[n] = c >>> 0; }
    const crc = (u) => { let c = 0xffffffff; for (let i = 0; i < u.length; i++) c = T[(c ^ u[i]) & 255] ^ (c >>> 8); return (c ^ 0xffffffff) >>> 0; };
    const parts = [], cen = []; let off = 0;
    for (const f of files) {
      if (!(f.data instanceof Uint8Array)) f.data = typeof f.data === 'string' ? new TextEncoder().encode(f.data) : new Uint8Array(f.data);
      const nm = new TextEncoder().encode(f.name), cr = crc(f.data), sz = f.data.length;
      const h = new DataView(new ArrayBuffer(30));
      h.setUint32(0, 0x04034b50, true); h.setUint16(4, 20, true); h.setUint32(14, cr, true);
      h.setUint32(18, sz, true); h.setUint32(22, sz, true); h.setUint16(26, nm.length, true);
      parts.push(new Uint8Array(h.buffer), nm, f.data);
      const c = new DataView(new ArrayBuffer(46));
      c.setUint32(0, 0x02014b50, true); c.setUint16(4, 20, true); c.setUint16(6, 20, true); c.setUint32(16, cr, true);
      c.setUint32(20, sz, true); c.setUint32(24, sz, true); c.setUint16(28, nm.length, true); c.setUint32(42, off, true);
      cen.push(new Uint8Array(c.buffer), nm);
      off += 30 + nm.length + sz;
    }
    const cs = cen.reduce((s, u) => s + u.length, 0), e = new DataView(new ArrayBuffer(22));
    e.setUint32(0, 0x06054b50, true); e.setUint16(8, files.length, true); e.setUint16(10, files.length, true);
    e.setUint32(12, cs, true); e.setUint32(16, off, true);
    return new Blob([...parts, ...cen, new Uint8Array(e.buffer)], { type: 'application/zip' });
  }
  busy() { if (!this.gt.length || this.rendering) { this.setState({ status: 'Still rendering — try again in a moment' }); return true; } return false; }
  frameCanvas(f, hidden, date) { const c = document.createElement('canvas'); c.width = c.height = 466; WatchEngine.compose(c.getContext('2d'), this.base, this.gt[f], this.face(), date || this.now(), hidden); return c; }
  // The kit zip's preview.png is the theme's ID photo, so it never inherits the transport:
  // hands at 10:09:36 and the moon at PREVIEW_MOON whatever mode the preview is playing in.
  previewCanvas() { const f = Math.min(this.state.frame, Math.max(0, this.gt.length - 1)); return this.frameCanvas(f, {}, WatchEngine.previewDate()); }
  // mirror of kit_check.py: red staff jewel (alpha>=60, R>100, R-max(G,B)>40), 8-connected, blob nearest the centre
  kitCheck(bank, T) {
    const W = 466, out = [], lines = [], fails = []; let ink = [W, W, -1, -1];
    bank.forEach((c, fi) => {
      const d = c.getContext('2d').getImageData(0, 0, W, W).data, red = new Uint8Array(W * W);
      for (let i = 0; i < W * W; i++) { const a = d[i * 4 + 3]; if (a > 0) { const x = i % W, y = (i / W) | 0; if (x < ink[0]) ink[0] = x; if (y < ink[1]) ink[1] = y; if (x > ink[2]) ink[2] = x; if (y > ink[3]) ink[3] = y; }
        if (a >= 60) { const r = d[i * 4], g = d[i * 4 + 1], b = d[i * 4 + 2]; if (r > 100 && r - Math.max(g, b) > 40) red[i] = 1; } }
      const seen = new Uint8Array(W * W); let best = null;
      for (let i = 0; i < W * W; i++) {
        if (!red[i] || seen[i]) continue;
        const st = [i]; seen[i] = 1; let x0 = W, y0 = W, x1 = -1, y1 = -1, sw = 0, sx = 0, sy = 0, n = 0;
        while (st.length) { const j = st.pop(), x = j % W, y = (j / W) | 0, a = d[j * 4 + 3]; n++; sw += a; sx += x * a; sy += y * a; if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y;
          for (let dy = -1; dy <= 1; dy++) for (let dx = -1; dx <= 1; dx++) { const qx = x + dx, qy = y + dy; if (qx < 0 || qy < 0 || qx >= W || qy >= W) continue; const q = qy * W + qx; if (red[q] && !seen[q]) { seen[q] = 1; st.push(q); } } }
        const cx = sx / sw, cy = sy / sw, dd = (cx - T[0]) ** 2 + (cy - T[1]) ** 2;
        if (!best || dd < best.dd) best = { bbox: [x0, y0, x1, y1], c: [cx, cy], n, dd };
      }
      if (!best) { fails.push('frame_' + (fi + 1) + ' has no red ink'); return; }
      out.push(best);
      const bw = best.bbox[2] - best.bbox[0] + 1, bh = best.bbox[3] - best.bbox[1] + 1;
      lines.push('  frame_' + String(fi + 1).padStart(2, '0') + '  hub bbox x' + best.bbox[0] + '..' + best.bbox[2] + ' y' + best.bbox[1] + '..' + best.bbox[3] + ' (' + bw + 'x' + bh + ')  centroid (' + best.c[0].toFixed(4) + ', ' + best.c[1].toFixed(4) + ')  ' + best.n + ' px');
      if (fi === 0) {
        if (bw % 2 === 0 || bh % 2 === 0) fails.push('staff jewel bbox is ' + bw + 'x' + bh + ': must be odd x odd');
        if (Math.abs(best.c[0] - T[0]) > 0.25 || Math.abs(best.c[1] - T[1]) > 0.25) fails.push('centroid off the declared centre by (' + (best.c[0] - T[0]).toFixed(3) + ', ' + (best.c[1] - T[1]).toFixed(3) + ')');
      }
    });
    const sp = out.length ? [0, 1].map((k) => Math.max(...out.map((o) => o.c[k])) - Math.min(...out.map((o) => o.c[k]))) : [0, 0];
    if (new Set(out.map((o) => o.bbox.join())).size > 1 || sp[0] > 0.01 || sp[1] > 0.01) fails.push('bank not registered: spread (' + sp[0].toFixed(4) + ', ' + sp[1].toFixed(4) + ')');
    const b0 = out[0] || { bbox: [0, 0, 0, 0], c: [0, 0] }, pass = !fails.length;
    const report = ['kit_check (in-browser mirror of kit_check.py)', 'canvas         : 466x466', 'declared centre: [' + T.join(', ') + ']   frames=' + bank.length, '', ...lines, '', '  frame-to-frame registration spread: (' + sp[0].toFixed(4) + ', ' + sp[1].toFixed(4) + ')', '', pass ? 'RESULT: PASS  (centroid on the declared centre, odd bbox, bank registered, geometry self-declared)' : 'RESULT: FAIL', ...fails.map((f) => '  - ' + f)].join('\n');
    return { pass, report, bbox: b0.bbox, centroid: [+b0.c[0].toFixed(4), +b0.c[1].toFixed(4)], size: ink[2] < 0 ? [0, 0] : [ink[2] - ink[0] + 1, ink[3] - ink[1] + 1] };
  }
  /* A transparent subdial lays no plate, so the layout hands whatever was hosted on it to
   * the dial -- the same re-route its holes and flares already take. The kit's draw order
   * has to say where the sprite actually lands: prep_watch_kit decides which live layer
   * covers each complication tile from this order, and a tile recorded under a layer that
   * never ships is a tile the device paints over. */
  compHost(fc, c) {
    const id = c.host;
    if (!id || id === 'dial') return 'dial';
    const l = fc && fc.subLayers && fc.subLayers.find((q) => q.id === id);
    return l && l.transparent ? 'dial' : id;
  }
  /* manifest.assets[] plus the sources/ payloads, from the one walk the UI also uses to
   * warn about missing art. `placed` records how the design used it, so the zip can be
   * re-baked or audited without the composer; sha256 is the browser's own digest when it
   * has one, and the recorded reason when it does not (file:// has no SubtleCrypto). */
  async kitAssets(files, D) {
    const E = window.WatchEngine, by = {};
    this.artRefs(D).forEach((r) => { (by[r.id] = by[r.id] || []).push(r.usedBy); });
    const out = [];
    for (const id of Object.keys(by)) {
      const a = E.assetGet(id);
      if (!a) { out.push({ id, usedBy: by[id], missing: true, note: 'no bytes in the authoring browser; load the .wckit bundle that carries them' }); continue; }
      // an asset can fill both backdrop slots at once; price it in each and report the
      // worst verdict, so sharing one image between plate and dial cannot hide an upscale
      const cands = [];
      ['bg', 'sf'].forEach((p) => { if (D && id === D[p + 'Art']) { const s = D[p + 'Size'] == null ? 442 : +D[p + 'Size']; cands.push({ box: [s, s], fit: E.assetFits(id, s, s, D[p + 'Fit'] || 'cover') }); } });
      if (!cands.length) cands.push({ box: [a.w, a.h], fit: E.assetFits(id, a.w, a.h, 'contain') });
      const pick = cands.filter((q) => !q.fit.ok)[0] || cands[0];
      const box = pick.box, fit = pick.fit;
      const hex = await E.sha256Of(E.assetBytesOf(a.dataUrl));
      const rec = { id, name: a.name, mime: a.mime, kind: a.kind, usedBy: by[id], sha256: hex || null,
        sha256Note: hex ? 'Web Crypto SHA-256 over the stored bytes' : 'this browser gave no SubtleCrypto (file:// is not a secure context); sha32 is an FNV-1a identity, not a security claim',
        sha32: a.sha, naturalSize: [a.w, a.h], drawnSize: [Math.round(box[0]), Math.round(box[1])], placed: true, bytes: Math.round(a.dataUrl.length * 3 / 4),
        resampled: fit.vector ? 'vector: rasterized at draw size' : (fit.upscale ? 'UPSAMPLED — see warning' : 'downsampled or 1:1'),
        ...(fit.ok ? {} : { warning: fit.why }) };
      // the zip carries the bytes as a file; the bundle carries them inline
      if (files) { rec.file = 'sources/' + id.replace(/[^\w.-]+/g, '-') + '.' + (a.kind === 'svg' ? 'svg' : 'png'); files.push({ name: rec.file, data: new TextEncoder().encode(a.dataUrl) }); }
      else rec.dataUrl = a.dataUrl;
      out.push(rec);
    }
    return out;
  }
  async exportKit(asTheme) {
    if (this.busy()) return;
    const fc = this.face(), F = this.F, B = this.B, pad = (n) => String(n).padStart(2, '0'), files = [], slug = this.state.dial, s0 = this.state;
    this.setState({ status: 'Packing layer kit…' });
    const baseOnly = { complications: 1, dial: 1, subdials: 1, hands: 1, glass: 1, outer: 1 };
    for (let f = 0; f < B; f++) {
      const c = document.createElement('canvas'); c.width = c.height = 466;
      const g = this.gt[f], noCage = g && g.cage ? Object.assign(document.createElement('canvas'), { width: 466, height: 466 }) : g;
      if (g && g.cage) noCage.getContext('2d').drawImage(g, 0, 0);
      WatchEngine.compose(c.getContext('2d'), this.base, noCage, this.face(), this.now(), baseOnly);
      files.push({ name: 'base/frame_' + pad(f + 1) + '.png', data: await this.png(c) });
    }
    // Under "no case" the drawn content ships eased in by the opening (dynScale) and the
    // author's art ships exactly as authored: the dial layer is already final (its art was
    // merged untransformed at bake time), so only the plate-frame pass and the procedural
    // layers take the dynamic scale -- and the dial.png bake must not rescale it again.
    const K = (fc.inset || 1) * (fc.dynK || 1), M = (v) => 233 + (v - 233) * K;
    const bake = (c, s) => { const q = s == null ? K : s; if (q === 1) return c; const o = document.createElement('canvas'); o.width = o.height = 466; const x = o.getContext('2d'); x.translate(233, 233); x.scale(q, q); x.translate(-233, -233); x.drawImage(c, 0, 0); return o; };
    // custom-format windows are assembled on device from a text kit, so the static dial/subdial layers are built without their holes.
    // The day-of-month field is NOT filtered out even though it now assembles the same way: its aperture is sealed (layout's hole.seal),
    // so the shipped dial carries no hole there already, and dropping the comp would also stop printMain avoiding numerals under it.
    const D0 = this.design(), hasCustom = D0.comps.some((c) => c.type === 'window' && c.shows === 'custom'), hd0 = this.handDef(this.state.dial);
    const lf = hasCustom ? WatchEngine.face({ ...this.opts(), design: { ...D0, comps: D0.comps.filter((c) => !(c.type === 'window' && c.shows === 'custom')) }, hands: hd0.style, handFinish: hd0.finish }) : fc;
    // layers/dial.png is the baked static layer the device paints over the bank: ship it
    // SEALED, with the date/moon apertures left solid. The complication value is a separate
    // per-day sprite composited on top at frame build (updates once a day, jitter accepted),
    // so the geartrain under an aperture is never DMA'd -- and prep_watch_kit's fold and
    // bake_clock.py's --bg-occlude (opaque static1) already cull those rows. A holed dial
    // here would make the bank push a train the sealed disc plainly hides.
    files.push({ name: 'layers/dial.png', data: await this.png(bake(lf.dialSealed, fc.inset || 1)) });
    files.push({ name: 'layers/glass.png', data: await this.png(bake(fc.glass)) });
    files.push({ name: 'layers/outer.png', data: await this.png(fc.outer) });
    const subFiles = [];
    // A transparent subdial has no plate to ship: an empty canvas written out would be
    // a fully transparent PNG the device blends for nothing every tick, and prep would
    // count it as a live static2 layer. Drop the file AND its drawOrder entry, which is
    // the same "no subdial in this kit" shape prep already handles (show2 = false).
    const realSubs = lf.subLayers.filter((l) => !l.transparent);
    const ghostSubs = {}; lf.subLayers.forEach((l) => { if (l.transparent) ghostSubs[l.id] = 1; });
    for (const [i, l] of realSubs.entries()) { const name = 'layers/subdial_' + pad(i + 1) + '_' + l.id + '.png'; subFiles.push({ id: l.id, file: name }); files.push({ name, data: await this.png(bake(l.canvas)) }); }
    let cage = null;
    if (this.base.cageAt && this.base.cageFrame && this.gt[0] && this.gt[0].cage) {
      /* The baker holds the carriage bank at CLK_TOURB_MAX = 16 frames, so a
       * deeper one ships a zip prep_watch_kit.py cannot bake. budget()'s MAXBANK
       * guard already keeps a carriage kit off any tier whose bank exceeds 16 --
       * the only such tier is 12 fps (bank 24) -- so this catches a bank that
       * outran the guard, and a half-written kit is worse than none. */
      if (B > 16) { this.setState({ status: 'Carriage bank is ' + B + ' frames but the baker holds at 16 (CLK_TOURB_MAX). This tempo should have been skipped by the budget.' }); return; }
      // put the staff axis on a texel centre: art centre at T + 0.5 in canvas space == pixel index T
      const want = this.base.cageAt.map((v) => M(v)), T = want.map((v) => Math.round(v)), WB = +this.cfg().wb, bank = [];
      for (let f = 0; f < B; f++) {
        let c = this.base.cageFrame(f, { k: K, dx: T[0] + 0.5 - want[0], dy: T[1] + 0.5 - want[1] });
        if (WB !== 6500) c = WatchEngine.wb(c, WB);
        bank.push(c); files.push({ name: 'tourbillon/frame_' + pad(f + 1) + '.png', data: await this.png(c, true) });
      }
      const chk = this.kitCheck(bank, T);
      files.push({ name: 'kit_check.txt', data: new TextEncoder().encode(chk.report) });
      if (!chk.pass) this.setState({ status: 'Tourbillon geometry check FAILED — see kit_check.txt in the zip' });
      cage = { files: 'tourbillon/frame_01…' + pad(B) + '.png', centre: T, pivot: T.slice(), size: chk.size, hubJewel: { bbox: chk.bbox, centroid: chk.centroid }, zeroRotationDeg: +(-(this.base.cageOff || 0) * 180 / Math.PI).toFixed(2), note: (this.base.orbital ? 'orbital carriage = seconds hand; ' : '') + 'full-size layers; rotate about centre by seconds/60 × 360° + zeroRotationDeg and draw over the base frame' };
    }
    const comps = [];
    const textKits = [];
    for (const kt of fc.textKits ? fc.textKits(K) : []) {
      const dir = 'text/' + kt.id + '_' + kt.kind + '/', piece = async (nm, p) => { if (!p) return null; files.push({ name: dir + nm, data: await this.png(p.canvas) }); return { file: dir + nm, w: p.w, h: p.h }; };
      const capL = await piece('cap_l.png', kt.capL), capR = await piece('cap_r.png', kt.capR), slice = await piece('slice.png', kt.slice), shade = await piece('shade.png', kt.shade), glyphs = {};
      for (const g of kt.glyphs) { const nm = 'glyph_' + g.ch.codePointAt(0).toString(16).padStart(4, '0') + '.png'; files.push({ name: dir + nm, data: await this.png(g.canvas) }); glyphs[g.ch] = { file: dir + nm, w: g.w, h: g.h, advance: g.adv, offsetX: g.ox }; }
      const shadeNote = shade ? ' Finally draw shade.png centred on the strip at x = (W − shade.w)/2, y = 0 (before the rotation): it is the window ring, its drop shadow and the aperture inner shade, which depend on where a pixel sits in the WINDOW and not on the string being drawn, so it must go on LAST — ink left on top of it reads brighter than the same watch in the dial preview.' : '';
      textKits.push({ id: kt.id, kind: kt.kind, host: kt.host, centre: kt.centre, rotationDeg: kt.rotDeg, height: kt.height, opaque: kt.opaque, format: kt.format, case: kt.fcase,
        capL, capR, slice, shade, padX: kt.padX, currentWidth: kt.fitWidth, widestRun: kt.widest, glyphs,
        ...(kt.kind === 'date' ? { byDay: kt.byDate } : { byDate: kt.byDate }),
        assemble: (kt.capL && kt.kind !== 'date'
          ? 'text = byDate[YYYY-MM-DD] (or format it yourself). textW = Σ glyphs[ch].advance. W = capL.w + padX + textW + padX + capR.w; for a strip the dial was cut to fit, use currentWidth, which is never narrower than any string this format can render. Build an unrotated W×height strip: capL at x=0, repeat slice from capL.w to W−capR.w, capR at W−capR.w, then glyph tiles from x = (W − textW)/2, each advancing by its advance (tiles already include the background). Rotate the strip by rotationDeg and centre it on centre, over its host layer.'
          : kt.kind === 'date'
            ? 'Day of month, assembled from ten digit tiles: byDay[<day, 2 digits>] gives the string to draw (a bare 1…9 for the first nine days — no leading zero). Use W = currentWidth, the width the dial was cut for; it is the same box every day of the month. textW = Σ glyphs[ch].advance (at most widestRun). Build an unrotated W×height strip: capL at x=0, repeat slice from capL.w to W−capR.w, capR at W−capR.w, then the glyph tiles from x = (W − textW)/2, each advancing by its advance — the run is drawn last, over the plate, and may cross the cap zone because the tiles carry the disc behind their ink. Rotate the strip by rotationDeg and centre it on centre, over its host layer.'
            : 'text = byDate[YYYY-MM-DD]. textW = Σ advance = widestRun at most (currentWidth). Pen starts at centre.x − textW/2; draw each glyph tile at pen − offsetX, vertically centred on centre.y (tiles are transparent and may overlap), pen += advance. Rotate the run by rotationDeg about centre.') + shadeNote });
    }
    for (const s0 of fc.compSprites()) {
      let s = s0;
      if (K !== 1) {
        const full = document.createElement('canvas'); full.width = full.height = 466; full.getContext('2d').drawImage(s0.canvas, s0.x, s0.y);
        const b = bake(full), x = Math.floor(M(s0.x)), y = Math.floor(M(s0.y)), w = Math.ceil(s0.w * K) + 2, hh = Math.ceil(s0.h * K) + 2, cc = document.createElement('canvas');
        cc.width = w; cc.height = hh; cc.getContext('2d').drawImage(b, -x, -y); s = { ...s0, x, y, w, h: hh, canvas: cc };
      }
      const name = 'complications/' + s.id + '_' + s.type + '/' + s.value + '.png';
      files.push({ name, data: await this.png(s.canvas) });
      let e = comps.find((c) => c.id === s.id);
      if (!e) comps.push((e = { id: s.id, type: s.type, host: s.host, x: s.x, y: s.y, w: s.w, h: s.h, ...(s.type === 'custom' || s.type === 'flare-date' ? { format: s.format, case: s.fcase, texts: s.texts, byDate: s.index } : {}), files: [] }));
      e.files.push(name);
    }
    const hands = [];
    // The additional calendar pointer is studio-only today: prep_watch_kit.py and bake_clock.py
    // accept hour/minute/second alone, so the firmware never sees them. Ship the angle each
    // one needs anyway, in the same value x 360 form the hands layer note already uses, so
    // wiring them up on the device is a loader change and not a reverse-engineering job.
    const CAL_DRIVE = {
      hours24: 'value = (H24 + min/60 + sec/3600) / 24 — one turn per 24 hours of local time, read against the 24-hour ring',
      gmt: 'value = (((H24 + gmtOffset) mod 24) + min/60 + sec/3600) / 24 — a second hour hand on a whole-hour zone offset, read against the same 24-hour ring',
      day: 'value = weekday / 7 — 0 = Sunday lands at 12 o\'clock, read against the day ring',
      date: 'value = (dayOfMonth - 1) / 31 — 1 lands at 12 o\'clock, read against the date ring',
    };
    // One spare centre hand at most, and the ring it reads is the same decision: the drive formula
    // the kit ships has to agree with where those labels were printed, so a ring that sets its
    // labels between the division lines moves its own pointer by half a division too.
    const pg = WatchEngine.perimGeom ? WatchEngine.perimGeom(D0) : null;
    const ringHand = D0.extra && WatchEngine.EXTRA_HANDS[D0.extra] ? D0.extra : null;
    const offTurn = pg ? pg.offTurn : 0;
    const offWhy = pg ? [pg.between ? 'the labels print between the division lines (half a division)' : '', pg.rot ? 'the ring is turned ' + pg.rot.toFixed(2) + '° clockwise from 12 o\'clock' : ''].filter(Boolean).join(' and ') : '';
    for (const [z, h] of fc.hands.entries()) {
      const name = 'hands/' + pad(z + 1) + '_' + h.id + '.png';
      let sp = h.sp.c;
      if (K !== 1) { sp = document.createElement('canvas'); sp.width = Math.ceil(h.sp.c.width * K); sp.height = Math.ceil(h.sp.c.height * K); sp.getContext('2d').drawImage(h.sp.c, 0, 0, sp.width, sp.height); }
      files.push({ name, data: await this.png(sp) });
      const sh = fc.handShadow(h), bl = sh.blur * K, pd = Math.ceil(bl * 1.5 + 2), mc = document.createElement('canvas'), sil = document.createElement('canvas');
      sil.width = sp.width; sil.height = sp.height; const sx = sil.getContext('2d'); sx.drawImage(sp, 0, 0); sx.globalCompositeOperation = 'source-in'; sx.fillStyle = '#000'; sx.fillRect(0, 0, sp.width, sp.height);
      mc.width = sp.width + 2 * pd; mc.height = sp.height + 2 * pd; const mx = mc.getContext('2d'); mx.globalAlpha = sh.alpha; if (bl > 0.1) mx.filter = 'blur(' + (bl / 2).toFixed(2) + 'px)'; mx.drawImage(sil, pd, pd);
      const shName = 'hands/' + pad(z + 1) + '_' + h.id + '_shadow.png';
      files.push({ name: shName, data: await this.png(mc, true) });
      const shadow = { file: shName, size: [mc.width, mc.height], pivot: [+(h.sp.px * K + pd).toFixed(2), +(h.sp.py * K + pd).toFixed(2)], offsetPx: [+(sh.dx * K).toFixed(2), +(sh.dy * K).toFixed(2)], directionDeg: fc.shadow.deg, distancePx: +(Math.hypot(sh.dx, sh.dy) * K).toFixed(2), blurPx: +bl.toFixed(2), opacity: +sh.alpha.toFixed(3), baked: 'blur + opacity baked into the mask' };
      hands.push({ shadow, z: z + 1, file: name, mount: h.id, kind: h.kind, centre: [+M(h.x).toFixed(2), +M(h.y).toFixed(2)], zeroRotationDeg: WatchEngine.handZeroDeg ? WatchEngine.handZeroDeg(h) : +((h.rot * 180) / Math.PI).toFixed(2), pivot: [+(h.sp.px * K).toFixed(2), +(h.sp.py * K).toFixed(2)], size: [sp.width, sp.height], cap: h.cap ? +(h.cap * K).toFixed(1) : 0, tick: h.kind === 'second' ? this.cfg().tick : undefined, ...(CAL_DRIVE[h.kind] && String(h.id).startsWith('centre-') ? { drive: CAL_DRIVE[h.kind] + (offTurn && h.kind === ringHand ? ' — add ' + offTurn.toFixed(4) + ' for ' + offWhy : ''), deviceReady: false } : {}) });
    }
    const perimeter = !pg ? undefined : {
      scale: D0.perim, splits: pg.spec.n, outerPx: +M(pg.out + 1.6).toFixed(2), innerPx: +M(pg.inr - 1.6).toFixed(2),
      tickPx: [+M(pg.tick1).toFixed(2), +M(pg.tick0).toFixed(2)], labelPx: +M(pg.lab).toFixed(2), fontSizePx: +M(pg.fs).toFixed(2),
      clockwise: true, index0Deg: pg ? pg.rot : 0, chapterScale: +pg.k.toFixed(3),
      rotationDeg: +pg.rot.toFixed(2),
      labelPlacement: pg.between ? 'between the division lines' : 'on the division lines',
      labelOffsetDeg: pg.between ? +(360 / pg.spec.n / 2).toFixed(3) : 0,
      orientation: D0.perimOrient === 'radial' ? 'radial (each label turned to face out)' : 'upright (every label bottom-aligned)',
      typeface: WatchEngine.FONT_NAME(D0.perimFont) || WatchEngine.FONT_NAME(D0.font) || 'dial default',
      fontSizeMm: +(WatchEngine.pxMm(pg.fs)).toFixed(3),
      colour: D0.perimColor || (D0.numColor && D0.numColor !== 'auto' ? D0.numColor : 'dial ink'),
      labels: Array.from({ length: pg.spec.n }, (_, i) => pg.spec.lab(i)),
      emphasised: Array.from({ length: pg.spec.n }, (_, i) => i).filter((i) => pg.spec.maj(i)),
      note: 'printed into layers/dial.png — nothing to draw at runtime; the scale runs clockwise and index 0 sits at index0Deg degrees clockwise from 12 o\'clock, which is what the drive formula on each calendar hand assumes',
    };
    const manifest = {
      size: 466, fps: F, frames: B, secondHand: this.cfg().tick === 'sweep' ? 'sweep' : this.cfg().tick + ' ticks per second', innerScale: K, preset: this.preset().name, movement: s0.train, designId: this.designId(), layout: this.design(),
      drawOrder: [
        { layer: 'base', files: 'base/frame_01…' + pad(B) + '.png', note: 'static plate + geartrain, lit by the inner light, opaque, one ' + B + '-frame loop played at ' + F + ' fps (' + (B / F).toFixed(B % F ? 2 : 0) + ' s)' },
        ...(cage ? [{ layer: 'tourbillon', ...cage }] : []),
        { layer: 'complications', where: 'under dial', items: comps.filter((c) => this.compHost(fc, c) === 'dial' && c.type !== 'flare-date').map((c) => c.id) },
        { layer: 'dial', file: 'layers/dial.png' },
        ...(textKits.some((t) => this.compHost(fc, t) === 'dial') ? [{ layer: 'text', where: 'over dial', items: textKits.filter((t) => this.compHost(fc, t) === 'dial').map((t) => t.id), note: 'assemble from textKits' }] : []),
        ...subFiles.flatMap((sf) => [{ layer: 'complications', where: 'under subdial ' + sf.id, items: comps.filter((c) => c.host === sf.id && c.type !== 'flare-date').map((c) => c.id) }, { layer: 'subdial', id: sf.id, file: sf.file },
          ...(textKits.some((t) => t.host === sf.id) ? [{ layer: 'text', where: 'over subdial ' + sf.id, items: textKits.filter((t) => t.host === sf.id).map((t) => t.id), note: 'assemble from textKits' }] : [])]),
        { layer: 'hands', note: 'draw in z order (1 = lowest); for each hand first draw its shadow mask rotated by the same angle about shadow.pivot, with the pivot placed at centre + shadow.offsetPx (offset stays fixed in screen space), then the hand sprite rotated about its pivot by value×360° + zeroRotationDeg with the pivot on centre; mount is the unique key — kind repeats when a subdial and a centre pointer show the same data, so never name a file by kind alone' },
        { layer: 'glass', file: 'layers/glass.png' },
        { layer: 'outer', file: 'layers/outer.png' },
      ],
      complications: comps.map((c) => ({ ...c, values: c.type === 'moon' ? '00…29 = moon age / 29.53 × 30' : c.type === 'custom' || c.type === 'flare-date' ? 'one PNG per distinct rendered string; look up today in byDate (YYYY-MM-DD → file stem), covers this year and next' : c.type })),
      hands,
      ...(perimeter ? { perimeter } : {}),
      textKits,
      handShadow: { directionDeg: fc.shadow.deg, followsLight: fc.shadow.auto, distance: fc.shadow.dist, softness: fc.shadow.blur, opacity: fc.shadow.op, note: 'per-hand offsets grow with stack height; see hands[].shadow' },
      assets: await this.kitAssets(files, D0),
      backgroundArt: D0.bgArt ? { asset: D0.bgArt, fit: D0.bgFit || 'cover', offsetPx: [+D0.bgX || 0, +D0.bgY || 0], sizePx: D0.bgSize == null ? 442 : +D0.bgSize, rotDeg: Math.round(+D0.bgRot || 0), opacity: D0.bgOpacity == null ? 1 : +D0.bgOpacity, finishBlend: D0.bgFinishBlend || undefined, finishBlendColor: D0.bgFinishBlend ? (D0.bgFinishColor || (D0.bgFinishBlend === 'screen' ? '#fffaf0' : '#3a2a14')) : undefined, finishBlendKey: D0.bgFinishBlend && /^#[0-9a-f]{6}$/i.test(D0.bgFinishKey || '') ? D0.bgFinishKey : undefined, finishBlendKeyTolerance: D0.bgFinishBlend && /^#[0-9a-f]{6}$/i.test(D0.bgFinishKey || '') ? (D0.bgFinishKeyTol == null ? WatchEngine.KEY_TOL_DEF : +D0.bgFinishKeyTol) : undefined, bakedInto: 'base/frame_*.png (the plate slot, under the geartrain)', note: 'stored into the bank with the plate, so it costs nothing per tick; it is not a layer to draw' } : undefined,
      // on the dial face, so it ships inside dial.png rather than as its own layer:
      // print, numerals and markers were drawn over it, apertures cut through it
      surfaceArt: D0.sfArt ? { asset: D0.sfArt, fit: D0.sfFit || 'cover', offsetPx: [+D0.sfX || 0, +D0.sfY || 0], sizePx: D0.sfSize == null ? 442 : +D0.sfSize, rotDeg: Math.round(+D0.sfRot || 0), opacity: D0.sfOpacity == null ? 1 : +D0.sfOpacity, finishBlend: D0.sfFinishBlend || undefined, finishBlendColor: D0.sfFinishBlend ? (D0.sfFinishColor || (D0.sfFinishBlend === 'screen' ? '#fffaf0' : '#3a2a14')) : undefined, finishBlendKey: D0.sfFinishBlend && /^#[0-9a-f]{6}$/i.test(D0.sfFinishKey || '') ? D0.sfFinishKey : undefined, finishBlendKeyTolerance: D0.sfFinishBlend && /^#[0-9a-f]{6}$/i.test(D0.sfFinishKey || '') ? (D0.sfFinishKeyTol == null ? WatchEngine.KEY_TOL_DEF : +D0.sfFinishKeyTol) : undefined, bakedInto: 'layers/dial.png (the dial face, under the chapter print)', note: 'the dial is already one blended layer per tick, so art inside it costs nothing; there is no separate art plane to composite' } : undefined,
      transparentSubdials: Object.keys(ghostSubs),
      whiteBalanceK: +this.cfg().wb, whiteBalanceNote: +this.cfg().wb === 6500 ? 'neutral' : 'baked into every colour PNG (not into shadow masks)',
    };
    files.push({ name: 'manifest.json', data: new TextEncoder().encode(JSON.stringify(manifest, null, 2)) });
    if (asTheme) return this.finishTheme(files, manifest, this.themeName());
    files.push({ name: 'preview.png', data: await this.png(this.previewCanvas()) });
    this.download(this.zip(files), 'watch-' + slug + '-kit.zip');
    const artN = manifest.assets ? manifest.assets.length : 0;
    this.setState({ status: 'Downloaded watch-' + slug + '-kit.zip (' + files.length + ' files' + (textKits.length ? ', ' + textKits.length + ' date-sprite kit' + (textKits.length > 1 ? 's' : '') : '') + (artN ? ', ' + artN + ' art source' + (artN > 1 ? 's' : '') + ' in sources/' : '') + (Object.keys(ghostSubs).length ? ', ' + Object.keys(ghostSubs).length + ' transparent sub-dial shipped as no layer' : '') + ', preview at 10:09)' });
  }
  /* FORMAT 2 made the name the whole identity the owner sees: the card prints it in the
   * theme picker and files the upload under sha256(name)[:12]. Until someone types over
   * the field, the dial's own preset title stands in, so a first bake is never blank and
   * never a path the card would have to rescue -- there is nothing left to rescue. */
  themeName() {
    const typed = this.state.themeName;
    if (typeof typed === 'string' && typed.trim()) return typed.trim();
    // render() calls this on every export-pane paint, and preset() dereferences the
    // engine's table: a missing preset must fall back to the dial key, not blank the pane.
    let P = null;
    try { P = this.preset(); } catch (e) { P = null; }
    return (P && P.name) || this.state.dial || 'watch';
  }
  /* The device theme, baked here rather than on the bench. It starts from the exact PNG
   * bytes the layer kit would have shipped (white balance included, shadows raw), so the
   * only difference from what tools/bake_clock.py produces is what that script folds:
   * the browser path runs no static fold and no occlusion cull, and says so in its note.
   * A container is refused, not written, when the dial exceeds a device table or the
   * name will not fit the header's one name field -- both are the baker's own checks,
   * and the field above measures that limit in the same bytes the baker counts. */
  async finishTheme(files, manifest, themeName) {
    const G = window.GorbBake;
    if (!G) { this.setState({ status: 'The theme baker (orbtheme-baker.js) did not load — the layer kit is unaffected' }); return; }
    // FORMAT 2 made the name the whole identity: it travels inside the container, the
    // device files the theme under sha256(name)[:12], and the upload filename carries
    // nothing. So this is free text now — spaces, capitals, an em dash — and the one
    // rule left is the field's own size, which the baker measures in UTF-8 bytes.
    const nm = String(themeName || 'watch').replace(/\u0000/g, '').trim();
    if (!G.nameOk(nm)) {
      this.setState({ status: 'Not baked: "' + nm + '" is not a theme name — the header holds ' +
        (G.NAME_MAX - 1) + ' bytes of it and this one measures ' + G.nameBytes(nm) +
        (G.nameBytes(nm) === 0 ? ' (a theme needs a name the owner can read)' : ' (shorten it)') });
      return;
    }
    this.setState({ status: 'Baking "' + nm + '"…' });
    try {
      const t0 = performance.now();
      const planes = {};
      // sources/ holds the art as data-URL text, not PNG bytes, and the kit's non-images are
      // text: bakeKit only ever looks a plane up by the name the manifest uses.
      for (const f of files) if (/[.]png$/i.test(f.name) && f.name.indexOf('sources/') !== 0) planes[f.name] = await G.decodePngAsync(f.data);
      const r = G.bakeKit({ manifest, planes, name: nm, serial: G.crc32(new TextEncoder().encode(JSON.stringify(manifest))), bakedMs: Math.round(performance.now() - t0) });
      // Name the download for the id the card will hold it under, so the file in the
      // owner's Downloads folder and the file on the card are the same thing to look at.
      // sha256 comes from the browser's own crypto (subtle is async, and the baker stays
      // DOM-free and sync); a context without it — an old file:// open — keeps the old
      // name and says so, because a wrong-looking filename beats a wrong filename.
      let id = null;
      try {
        if (window.crypto && window.crypto.subtle) {
          id = G.themeIdFromDigest(new Uint8Array(
            await window.crypto.subtle.digest('SHA-256', new TextEncoder().encode(nm))));
        }
      } catch (e) { id = null; }
      const stem = id || nm.replace(/[^\w.-]+/g, '-').slice(0, G.NAME_MAX - 1);
      this.download(new Blob([r.bytes], { type: 'application/octet-stream' }), stem + '.orbtheme');
      const c = r.report.counts;
      this.setState({ status: 'Downloaded ' + stem + '.orbtheme — "' + nm + '"' + (id ? '' : ' (id unavailable here: upload it and the page will name it)') +
        ' (' + (r.bytes.length / 1048576).toFixed(2) + ' MB · bank ' + c.rects + ' rects · ' + (c.tourbillon || 0) + ' carriage frames · ' + c.comps + ' complications · ' + c.aux + ' pointer' + (c.aux === 1 ? '' : 's') + ') · ' + r.report.notes.join(' · ') });
    } catch (e) {
      // The caps and the fold proof throw here: the device would silently drop or misdraw
      // anything past them, so the bake stops instead of shipping a theme that lies.
      this.setState({ status: 'Not baked: ' + (e && e.message ? e.message : String(e)) });
    }
  }
  /* The bundle is the half a Design ID cannot carry: the bytes. Format is named and
   * versioned because a design that silently loads against a different shape is worse
   * than one that refuses -- the loader checks both before it touches state. */
  async exportBundle() {
    if (!this.ready) return;
    const D = this.design();
    const assets = await this.kitAssets(null, D);
    const missing = assets.filter((a) => a.missing);
    if (missing.length) { this.setState({ status: 'Not bundled: ' + missing.length + ' referenced asset(s) have no bytes in this browser — re-upload them or import the .wckit that has them' }); return; }
    const body = { format: 'wckit', version: 1, size: 466, made: new Date().toISOString(), designId: this.designId(), dial: this.state.dial, design: D, assets, cfg: this.cfg(), caseStyle: this.state.caseStyle, caseMetal: this.state.caseMetal, glassStyle: this.state.glassStyle, train: this.state.train, hands: this.state.hands, handFinish: this.state.handFinish };
    this.download(new Blob([JSON.stringify(body, null, 2)], { type: 'application/json' }), 'watch-' + this.state.dial + '-' + (assets.length ? assets.length + 'art-' : 'noart-') + '.wckit');
    this.setState({ status: 'Downloaded watch-' + this.state.dial + '.wckit (' + assets.length + ' art asset' + (assets.length === 1 ? '' : 's') + ', ' + Math.round(JSON.stringify(body).length / 1024) + ' KB)' });
  }
  importBundle(file) {
    const E = window.WatchEngine;
    if (!file) return;
    const rd = new FileReader();
    rd.onerror = () => this.setState({ status: 'That file could not be read' });
    rd.onload = () => {
      let o = null;
      try { o = JSON.parse(String(rd.result)); } catch (e) { this.setState({ status: 'Not a .wckit: the file is not JSON' }); return; }
      if (!o || o.format !== 'wckit') { this.setState({ status: 'Not a .wckit: no "format": "wckit" header' }); return; }
      if (o.version > 1) { this.setState({ status: 'That .wckit is format version ' + o.version + '; this build reads 1' }); return; }
      if (!o.design || !Array.isArray(o.design.comps)) { this.setState({ status: 'That .wckit has no design in it' }); return; }
      const r = E.assetRestore(o.assets || []);
      this.saveArt();
      const refs = this.artRefs(o.design), miss = refs.filter((x) => !E.assetKnown(x.id));
      const id = o.dial || this.state.dial;
      this.setState((st) => ({
        designs: { ...st.designs, [id]: E.canonDesign(JSON.parse(JSON.stringify(o.design))) },
        dial: id, sel: null,
        ...(o.cfg ? { cfg: o.cfg } : {}), ...(o.train ? { train: o.train } : {}),
        ...(o.caseStyle ? { caseStyle: o.caseStyle } : {}), ...(o.caseMetal ? { caseMetal: o.caseMetal } : {}), ...(o.glassStyle ? { glassStyle: o.glassStyle } : {}),
        ...(o.hands ? { hands: o.hands } : {}), ...(o.handFinish ? { handFinish: o.handFinish } : {}),
        status: 'Loaded ' + (o.design.name || id) + ' with ' + r.added + ' art asset' + (r.added === 1 ? '' : 's') + (miss.length ? ' — ' + miss.length + ' still missing (' + miss.map((m) => m.usedBy).join(', ') + ')' : ''),
      }));
      this.faces = {}; this.buildMovement();
    };
    rd.readAsText(file);
  }
  renderVals() {
    const s = this.state, F = this.F, B = this.B, pad = (n) => String(n).padStart(2, '0'), c0 = this.cfg();
    const stop = (fn) => () => this.setState((st) => ({ playing: false, frame: fn(st.frame) }));
    const E = window.WatchEngine, ready = !!(this.ready && E);
    const PR = ready ? this.preset() : null, D = ready ? this.design() : { comps: [], centre: {} };
    const on = (b) => ({ bg: b ? '#2a2316' : '#1f1d1a', bd: b ? '#c9a25a' : '#3a352d', fg: b ? '#f0d79a' : '#e9e2d2' });
    const did = this.designId(), fc = ready ? this.face() : null, tr = ready ? this.train() : { deg: 0, by: null };
    const cap = (t) => t[0].toUpperCase() + t.slice(1);
    const subs = D.comps.filter((c) => c.type === 'subdial');
    const num = (cid) => D.comps.findIndex((c) => c.id === cid) + 1;
    const compTitle = (c) => !ready ? '' : c.type === 'subdial' ? 'Subdial · ' + E.SCALES[c.scale] : c.type === 'window' ? E.SHOWS[c.shows] + ' window' : c.type === 'moon' ? 'Moon phase' : c.type === 'flare' ? 'Flare · ' + (E.FLARE_ICONS[c.icon] || '') : c.show === 'balance' ? 'Escapement opening' : 'Opening';
    const hostName = (h) => (!h || h === 'dial' ? 'dial' : h === 'plate' ? 'movement plate' : '#' + num(h) + ' subdial');
    const sel = D.comps.find((c) => c.id === s.sel);
    const feats = ready ? E.features(s.train) : [];
    const opt = (obj) => Object.entries(obj).map(([v, label]) => ({ v, label }));
    const mark = (list, v) => list.map((o) => ({ ...o, sel: String(o.v) === String(v) }));
    const mk = (o) => ({ label: '', isHead: false, isRow: true, isRange: false, isSelect: false, isChips: false, isNote: false, isColor: false, isText: false, isFile: false, min: 0, max: 1, step: 1, value: '', on: null, opts: [], valText: '', note: '', anchor: null, ...o });
    /* anchor: the pane's scroll destination for a selection (see scrollTargetFor). An
     * optional second argument, because only a handful of sections are ever aimed at --
     * a label alone would leave the field objects carrying names nothing reads. */
    const H = (label, anchor) => mk({ label, isHead: true, isRow: false, anchor });
    const Rg = (label, value, min, max, step, onv, valText) => mk({ label, isRange: true, min, max, step, value, on: (e) => onv(+e.target.value), valText });
    const Sl = (label, value, opts, onv) => mk({ label, isSelect: true, value, opts: mark(opts, value), on: (e) => onv(e.target.value) });
    const Ch = (label, value, opts, onv) => mk({ label, isChips: true, opts: opts.map((o) => ({ ...on(String(o.v) === String(value)), label: o.label, pick: () => onv(o.v) })) });
    const Nt = (label, note) => mk({ label, isNote: true, note });
    const sw = (k) => 'linear-gradient(135deg,' + E.DIAL_COLORS[k][0] + ',' + E.DIAL_COLORS[k][1] + ')';
    const Sw = (label, value, keys, onv, extra) => mk({ label, isSwatch: true, valText: keys.includes(value) ? E.COLOR_NAMES[value] : '', opts: [...(extra || []), ...keys.map((k) => ({ label: E.COLOR_NAMES[k], text: '', bg: sw(k), bd: k === value ? '#f0d79a' : '#2e2a24', pick: () => onv(k) }))] });
    const INKS = [['#f6f3ec', 'White'], ['#1a140c', 'Black'], ['#c9a25a', 'Gold'], ['#d7dbe0', 'Silver'], ['#cfe8b0', 'Lume green'], ['#9fd8ff', 'Lume blue'], ['#c8322a', 'Red'], ['#ff7a1a', 'Orange'], ['#2b59c3', 'Blue'], ['#ff2bd6', 'Magenta'], ['#22e6ff', 'Cyan'], ['#b6ff2e', 'Acid']];
    const parseCol = (t) => {
      t = String(t || '').trim().toLowerCase(); let m;
      if ((m = t.match(/^#?([0-9a-f]{6})$/))) return '#' + m[1];
      if ((m = t.match(/^#?([0-9a-f])([0-9a-f])([0-9a-f])$/))) return '#' + m[1] + m[1] + m[2] + m[2] + m[3] + m[3];
      if ((m = t.match(/^(?:rgb\s*\()?\s*(\d{1,3})[\s,]+(\d{1,3})[\s,]+(\d{1,3})\s*\)?$/))) { const v = [m[1], m[2], m[3]].map(Number); if (v.every((x) => x <= 255)) return '#' + v.map((x) => x.toString(16).padStart(2, '0')).join(''); }
      return null;
    };
    this._drafts = this._drafts || {};
    const isHexColor = (v) => /^#[0-9a-f]{6}$/i.test(String(v || ''));
    const hexRGB = (v) => { const n = parseInt(v.slice(1), 16); return (n >> 16) + ', ' + ((n >> 8) & 255) + ', ' + (n & 255); };
    const Co = (label, value, onv, autoText, scope, X) => {
      X = X || {};
      const key = (scope || (s.sel || 'dial')) + ':' + label, hex = value && /^#[0-9a-f]{6}$/i.test(value) ? value : null, dr = this._drafts[key];
      return mk({ label, isColor: true, value: hex || '#ffffff', on: (e) => { delete this._drafts[key]; onv(e.target.value); },
        hexText: dr != null ? dr : hex || '', hexTitle: hex ? 'rgb ' + hexRGB(hex) : 'Type a hex code or r, g, b',
        onHex: (e) => { const v = e.target.value, p = parseCol(v); if (p) { delete this._drafts[key]; onv(p); } else { this._drafts[key] = v; this.forceUpdate(); } },
        onHexBlur: () => { if (this._drafts[key] != null) { delete this._drafts[key]; this.forceUpdate(); } },
        valText: value === 'none' ? 'None' : !value || value === 'auto' ? (X.noAuto ? 'None' : autoText || 'Auto') : value, valText: value === 'none' ? 'None' : !value || value === 'auto' ? (X.noAuto ? 'None' : autoText || 'Auto') : value,
      opts: [...(X.none ? [{ label: 'None', text: '∅', bg: '#1f1d1a', bd: value === 'none' ? '#f0d79a' : '#3a352d', pick: () => onv('none') }] : []),
        ...(X.noAuto ? [] : [{ label: autoText || 'Auto', text: 'A', bg: '#1f1d1a', bd: !value || value === 'auto' ? '#f0d79a' : '#3a352d', pick: () => onv('auto') }]), ...INKS.map(([c, n]) => ({ label: n, text: '', bg: c, bd: c === value ? '#f0d79a' : '#2e2a24', pick: () => onv(c) }))] }); };
    const Tx = (label, value, onv) => mk({ label, isText: true, value: value == null ? '' : String(value), on: (e) => onv(e.target.value) });
    const Fi = (label, onv, valText) => mk({ label, isFile: true, on: onv, valText: valText || '' });
    const T = ready ? E.TRAINS[s.train] : null;

    const selFields = [];
    if (sel && ready) {
      const set = (k, n) => (v) => this.setComp(sel.id, { [k]: n ? +v : v });
      const R = (label, k, min, max, step, fmt, dflt) => { const v = sel[k] == null ? (dflt ?? 0) : +sel[k]; selFields.push(Rg(label, v, min, max, step, set(k, true), fmt ? fmt(v) : String(v))); };
      const Sx = (label, k, opts, n) => selFields.push(Sl(label, String(sel[k] ?? ''), opts, set(k, n)));
      const Cx = (label, k, opts, dflt) => selFields.push(Ch(label, sel[k] ?? dflt, opts, set(k)));
      const follow = sel.type === 'opening' && sel.show === 'balance';
      if (sel.type !== 'subdial' && !follow) Sx('Host', 'host', [{ v: 'dial', label: 'Main dial' }, ...(sel.type === 'flare' ? [{ v: 'plate', label: 'Movement plate · seen through openings' }] : []), ...subs.map((q) => ({ v: q.id, label: '#' + num(q.id) + ' ' + compTitle(q) }))]);
      if (follow && tr.by && tr.by !== sel.id) selFields.push(Nt('Angle', 'Riding on the balance — the train is aimed by #' + num(tr.by)));
      else R(follow ? 'Angle · train' : 'Angle', 'angle', 0, 359, 1, (v) => v + '°');
      if (!follow) R('Distance', 'dist', 0, 220, 0.5, (v) => v + 'px');
      if (sel.type === 'opening') Sx('Shape', 'shape', opt(E.SHAPES));
      if (sel.type === 'flare') {
        selFields.push(Sl('Kind', String(sel.icon ?? ''), opt(E.FLARE_ICONS), (v) => this.setComp(sel.id, v === 'datetext' && sel.icon !== 'datetext' ? { icon: v, size: Math.min(+sel.size || 16, 8) } : { icon: v })));
        if (sel.icon === 'datetext') {
          selFields.push(Tx('Format', sel.fmt == null ? 'ddd DD MMM' : sel.fmt, set('fmt')));
          Cx('Case', 'fcase', [{ v: 'upper', label: 'UPPER' }, { v: 'asis', label: 'As written' }, { v: 'lower', label: 'lower' }], 'upper');
          selFields.push(Nt('Preview', E.fmtDate(sel.fmt == null ? 'ddd DD MMM' : sel.fmt, this.now(), sel.fcase) || '—'), Nt('Tokens', 'YYYY YY · MMMM MMM MM M · DD D Do · DDDD DDD (day of year) · dddd ddd dd · d E (weekday no.) · WW W (ISO week) · Q · strftime: %Y %y %B %b %m %d %-d %e %j %A %a %u %V %U · [text] stays literal'));
        }
        if (sel.icon === 'text' || sel.icon === 'arctext' || sel.icon === 'datetext') {
          if (sel.icon !== 'datetext') selFields.push(Tx('Text', sel.text, set('text')));
          selFields.push(Sl('Typeface', sel.font || 'dial', [{ v: 'dial', label: 'Same as dial' }, ...opt(E.TYPEFACES)], (v) => this.setComp(sel.id, { font: v === 'dial' ? undefined : v })));
          Cx('Weight', 'weight', [{ v: 'bold', label: 'Bold' }, { v: 'regular', label: 'Regular' }, { v: 'italic', label: 'Italic' }], 'bold');
          R('Letter spacing', 'spacing', 0, 1, 0.02, (v) => v.toFixed(2), 0);
        }
        if (sel.icon === 'image') {
          selFields.push(Fi('Image', (e) => {
            const file = e.target.files && e.target.files[0]; if (!file) return;
            this.pickArt(file, 'flare #' + num(sel.id) + ' art', (id) => this.setComp(sel.id, { img: id }));
          }, sel.img && E.assetReady(sel.img) ? 'loaded' : sel.img ? 'missing' : ''));
          const fa = sel.img && E.assetGet(sel.img);
          // the old wording promised the upload was gone at reload; it persists now, so
          // the line states what actually governs: the asset store, and the bundle
          selFields.push(Nt('', fa
            ? fa.name + ' · ' + fa.kind + ' · ' + fa.w + '×' + fa.h + ' source · ' + Math.round(fa.dataUrl.length / 1024) + ' KB · held in this browser and shipped in sources/ with the kit'
            : sel.img ? 'That reference has no bytes in this browser — load the .wckit bundle that carries it.'
              : 'PNG or SVG. Colour Auto keeps the image’s own colours. The reference travels in the Design ID; the bytes travel in the .wckit bundle and in the kit’s sources/. The device reads a PNG rotated without antialiasing, so author it at final size.'));
          if (fa) selFields.push(mk({ label: ' ', isRow: true, isChips: true, opts: [{ v: 'x', label: 'Remove image', bg: '#1f1d1a', bd: '#3a352d', fg: '#e9e2d2', pick: () => this.setComp(sel.id, { img: undefined }) }] }));
        }
      }
      if (sel.type !== 'opening' && !(sel.type === 'flare' && sel.icon === 'arctext') || (sel.shape && sel.shape !== 'circle')) R('Rotation', 'rot', -180, 180, 1, (v) => v + '°');
      const sr = E.SIZE_RANGE[sel.type];
      R(sel.type === 'window' ? 'Text size' : sel.type === 'flare' ? (sel.bg && sel.bg !== 'none' ? 'Overall size' : 'Size') : 'Radius', 'size', sr[0], sr[1], sr[2], (v) => (sel.type === 'window' ? v.toFixed(2) + '×' : v + 'px'));
      if (sel.type === 'window') { R('Width', 'ww', 0.5, 3, 0.05, (v) => v.toFixed(2) + '×', 1); R('Height', 'wh', 0.5, 3, 0.05, (v) => v.toFixed(2) + '×', 1);
        if (sel.wshape === 'circle') selFields.push(Nt('', 'At equal Width and Height the round aperture takes the longer side as its diameter. Pull the two apart and the round opening becomes a true ellipse: half of each is a radius, so the aperture can stand taller than wide.')); }
      Sx('Style', 'style', opt(E.COMP_STYLES[sel.type]));
      // The raised ring only exists when the style draws it: the choice appears with the
      // style that uses it, and Gilt is the movement's own metal -- nothing was decided
      // until a different one is picked, so the Design ID of an untouched dial is unchanged.
      if ((sel.type === 'window' && (!sel.style || sel.style === 'frame')) || (sel.type === 'opening' && (sel.style === 'frame' || sel.style === 'lip')))
        Cx('Frame colour', 'frameColor', opt(E.FRAME_COLORS), 'gilt');
      if (sel.type === 'moon' && (sel.style === 'graphic' || sel.style === 'neon')) {
        const neon = sel.style === 'neon';
        selFields.push(Co(neon ? 'Glow colour' : 'Moon colour', sel.moonColor, set('moonColor'), neon ? 'Magenta' : 'Dial ink'));
        selFields.push(Co(neon ? 'Fade colour' : 'Ring colour', sel.moonColor2, set('moonColor2'), neon ? 'Cyan' : 'Same'));
        selFields.push(Co('Background', sel.moonBg, set('moonBg'), neon ? 'Night' : 'Dial'));
      }
      if (sel.type === 'flare') {
        selFields.push(Co(sel.icon === 'arctext' ? 'Colour' : 'Foreground', sel.color, set('color'), sel.style === 'applied' ? 'Metal' : 'Dial ink', null, { none: true }));
        if (sel.icon !== 'arctext') {
          selFields.push(Co('Background', sel.bg || 'none', (v) => this.setComp(sel.id, { bg: v }), 'None', null, { none: true, noAuto: true }));
          if (!sel.bg || sel.bg === 'none') selFields.push(Nt('', 'Pick a background colour to add a plate behind the icon; plate shape, size, offset and icon size appear here.'));
          else {
            if (sel.color !== 'none') R('Icon size', 'fgScale', 0.2, 2, 0.02, (v) => Math.round(v * 100) + '%', 1);
            Cx('Plate', 'bgShape', [{ v: 'rect', label: 'Square' }, { v: 'rounded', label: 'Rounded' }, { v: 'pill', label: 'Pill' }, { v: 'circle', label: 'Circle' }], 'rect');
            R('Plate width', 'bgW', 0.6, 8, 0.05, (v) => v.toFixed(2) + '×', 1.6);
            R('Plate height', 'bgH', 0.4, 4, 0.05, (v) => v.toFixed(2) + '×', 1);
            R('Plate offset ←→', 'bgX', -6, 6, 0.05, (v) => v.toFixed(2), 0);
            R('Plate offset ↑↓', 'bgY', -4, 4, 0.05, (v) => v.toFixed(2), 0);
          }
        }
        R('Height', 'height', -2, 3, 0.05, (v) => (v < 0 ? 'sunk ' + (-v).toFixed(2) : v === 0 ? 'flush' : 'raised ' + v.toFixed(2)), 0);
        R('Opacity', 'opacity', 0.1, 1, 0.05, (v) => Math.round(v * 100) + '%', 1);
      }
      if (sel.type === 'subdial') {
        Sx('Scale', 'scale', opt(E.SCALES));
        R('Open centre', 'open', 0, 0.8, 0.05, (v) => (v ? Math.round(v * 100) + '%' : 'closed'));
        R('Height', 'height', -1.5, 2, 0.05, (v) => (v < 0 ? 'sunk ' + (-v).toFixed(2) : v === 0 ? 'flush' : 'raised ' + v.toFixed(2)), 1);
        if (sel.style === 'carbon') R('Weave angle', 'weaveAngle', 0, 90, 1, (v) => v + '°', +D.weaveAngle || 0);
        if (sel.style === 'carbon' || sel.style === 'forged') R(sel.style === 'carbon' ? 'Weave size' : 'Flake size', 'weaveScale', 0.4, 3, 0.05, (v) => v.toFixed(2) + '×', D.weaveScale == null ? 1 : +D.weaveScale);
        if (sel.style && sel.style !== 'enamel') R('Texture', 'tex', 0, 2.5, 0.05, (v) => v.toFixed(2) + '×', D.texture == null ? 1 : +D.texture);
        const cv = sel.color || 'match', setC = set('color');
        selFields.push(Sw('Colour', cv, E.COLOR_GROUPS.classic, setC, [{ label: 'Match dial', text: '=', bg: '#1f1d1a', bd: cv === 'match' ? '#f0d79a' : '#3a352d', pick: () => setC('match') }]));
        selFields.push(Sw('Vivid', cv, E.COLOR_GROUPS.vivid, setC));
        selFields.push(Co('Custom', E.isHex(cv) ? cv : null, (v) => setC(v === 'auto' ? 'match' : v), 'Preset'));
        if (sel.scale !== 'none') {
          selFields.push(H('Subdial display'));
          selFields.push(Sl('Display', sel.tstyle || 'classic', opt(E.SUB_DISPLAYS), set('tstyle')));
          selFields.push(Co('Text colour', sel.tcolor, set('tcolor'), 'Auto'));
          Sx('Typeface', 'tfont', [{ v: 'dial', label: 'Same as dial' }, ...opt(E.TYPEFACES)]);
          if (!sel.tfont) selFields[selFields.length - 1] = Sl('Typeface', 'dial', [{ v: 'dial', label: 'Same as dial' }, ...opt(E.TYPEFACES)], set('tfont'));
          Cx('Weight', 'tweight', [{ v: 'bold', label: 'Bold' }, { v: 'regular', label: 'Regular' }, { v: 'italic', label: 'Italic' }], 'bold');
          Cx('Orientation', 'torient', [{ v: 'upright', label: 'Upright' }, { v: 'radial', label: 'Radial' }], 'upright');
          Cx('Direction', 'tdir', [{ v: 'cw', label: 'Clockwise' }, { v: 'ccw', label: 'Anticlockwise' }], 'cw');
          R('Text size', 'tsize', 0.6, 1.6, 0.05, (v) => Math.round(v * 100) + '%', 1);
        }
      }
      if (sel.type === 'window') {
        Sx('Shows', 'shows', opt(E.SHOWS));
        if (sel.shows === 'custom') {
          selFields.push(Tx('Format', sel.fmt == null ? 'ddd DD MMM' : sel.fmt, set('fmt')));
          Cx('Case', 'fcase', [{ v: 'upper', label: 'UPPER' }, { v: 'asis', label: 'As written' }, { v: 'lower', label: 'lower' }], 'upper');
          selFields.push(Nt('Preview', E.fmtDate(sel.fmt == null ? 'ddd DD MMM' : sel.fmt, this.now(), sel.fcase) || '—'));
          selFields.push(Nt('Tokens', 'YYYY YY · MMMM MMM MM M · DD D Do · DDDD DDD (day of year) · dddd ddd dd · d E (weekday no.) · WW W (ISO week) · Q · strftime: %Y %y %B %b %m %d %-d %e %j %A %a %u %V %U · [text] stays literal'));
        }
        /* The aperture is the author's, not a consequence of the data: a cyclops over a
         * domed crystal and a single numeral punched through an enamel dial are both
         * round windows. Round takes the LONGER side as its diameter, so flipping a
         * square window to round keeps the glyph it already framed instead of cropping
         * it to the short axis. */
        Sx('Aperture', 'wshape', [{ v: 'rect', label: 'Square aperture' }, { v: 'circle', label: 'Round aperture' }]);
        Sx('Disc', 'disc', [{ v: 'auto', label: 'Match dial' }, { v: 'light', label: 'Light' }, { v: 'dark', label: 'Dark' }]);
        selFields.push(Co('Background', sel.discColor, set('discColor'), 'Disc'), Co('Text colour', sel.textColor, set('textColor'), 'Auto'));
        selFields.push(Sl('Typeface', sel.wfont || 'dial', [{ v: 'dial', label: 'Same as dial' }, ...opt(E.TYPEFACES)], set('wfont')));
      }
      if (sel.type === 'opening') {
        Sx('Through it', 'show', [{ v: 'free', label: 'Whatever is there' }, { v: 'balance', label: 'Follow the balance (escapement)' }, ...feats.filter((f) => f.id !== 'balance').map((f) => ({ v: f.id, label: 'Aim train at ' + f.name.toLowerCase() }))]);
        const g = fc && fc.lay.G[sel.id];
        if (sel.show !== 'free' && sel.show !== 'balance' && g && Math.hypot(g.x - 233, g.y - 233) < 4) Sx('Place it', 'hour', Array.from({ length: 12 }, (_, k) => ({ v: String(k + 1), label: 'at ' + (k + 1) + ' o’clock' })), true);
        if (sel.show !== 'free' && sel.show !== 'balance' && tr.by && tr.by !== sel.id) selFields.push(Nt('Note', 'Only the first aimed opening (#' + num(tr.by) + ') turns the train'));
      }
    }

    const fields = [];
    const cf = (k) => (v) => this.setCfg(k, v);
    if (ready && s.tab === 'comps') fields.push(...selFields);
    if (ready && s.tab === 'dial') {
      const ud = (k) => (v) => this.upd((d) => { d[k] = v; });
      const pr = D.print || 'roman', col = D.color || 'cream', surf = D.surface || 'enamel';
      fields.push(H('Face'),
        Sl('Chapter ring', pr, opt(E.PRINTS), ud('print')),
        Sl('Numerals', D.numerals || 'all', opt(E.NUMERALS), ud('numerals')));
      if (E.HAS_NUMS.includes(pr) && D.numerals !== 'none') fields.push(Ch('Orientation', D.numOrient || (pr === 'roman' ? 'radial' : 'upright'), [{ v: 'upright', label: 'Bottom aligned' }, { v: 'radial', label: 'Circular' }], ud('numOrient')));
      fields.push(Sl('Typeface', D.font || E.RING_FONT(pr), opt(E.TYPEFACES), ud('font')));
      if (pr !== 'none' && D.numerals !== 'none') {
        if (pr !== 'baton') fields.push(Co(pr === 'diver' ? 'Plot colour' : 'Numeral colour', D.numColor, ud('numColor'), pr === 'diver' ? 'Lume' : 'Auto · ' + (fc && fc.dialEn && fc.dialEn.dark ? 'light' : 'dark')));
        const ns = D.numScale == null ? 1 : +D.numScale;
        fields.push(Rg(pr === 'diver' ? 'Plot size' : pr === 'baton' || pr === 'minimal' ? 'Baton size' : 'Numeral size', ns, 0.5, 2, 0.05, ud('numScale'), ns.toFixed(2) + '×'));
        if (pr !== 'baton') fields.push(Sl('Finish', D.numStyle || 'print', opt(E.COMP_STYLES.flare), ud('numStyle')));
        const sz = (k) => (D[k] == null ? 1 : +D[k]);
        fields.push(H('The 12 · and 3, 6, 9'));
        const hasText = E.HAS_NUMS.includes(pr), same = [{ v: 'same', label: 'Same as dial' }, ...opt(E.TYPEFACES)];
        if (hasText) fields.push(Sl('12 typeface', D.n12Font || 'same', same, ud('n12Font')), Sl('3·6·9 typeface', D.nQFont || 'same', same, ud('nQFont')));
        if (pr !== 'baton') fields.push(Co('12 colour', D.n12Color, ud('n12Color'), 'Same'));
        fields.push(Rg('12 size', sz('n12Scale'), 0.5, 2, 0.05, ud('n12Scale'), sz('n12Scale').toFixed(2) + '×'));
        if (pr !== 'baton') fields.push(Co('3·6·9 colour', D.nQColor, ud('nQColor'), 'Same'));
        fields.push(Rg('3·6·9 size', sz('nQScale'), 0.5, 2, 0.05, ud('nQScale'), sz('nQScale').toFixed(2) + '×'),
          Nt('', 'Multiplied on top of the overall size.'));
      }
      const ow = D.openwork || 'none', ov = (k, d) => (D[k] == null ? d : +D[k]);
      fields.push(H('Openwork'), Sl('Skeleton', ow, opt(E.OPENWORK), ud('openwork')));
      if (ow !== 'none') fields.push(
        Rg('Bar width', ov('owBar', 20), 8, 40, 1, ud('owBar'), ov('owBar', 20) + 'px'),
        Rg('Chapter ring', ov('owRing', 168), 120, 205, 1, ud('owRing'), 'from ' + ov('owRing', 168) + 'px'),
        ...(ow !== 'ring' ? [Rg('Hub', ov('owHub', 36), 18, 80, 1, ud('owHub'), ov('owHub', 36) + 'px')] : []),
        Rg('Rim around openings', ov('owRim', 10), 3, 24, 1, ud('owRim'), ov('owRim', 10) + 'px'),
        Rg('Depth', ov('owDepth', 1), 0, 2.5, 0.05, ud('owDepth'), ov('owDepth', 1).toFixed(2) + '×'),
        ...(ow !== 'ring' ? [Ch('Inlay', +D.owInlay ? 'on' : 'off', [{ v: 'off', label: 'Plain bars' }, { v: 'on', label: 'Sparkle inlay' }], (v) => this.upd((d) => { d.owInlay = v === 'on' ? 1 : 0; }))] : []),
        Nt('', 'Openings and subdials get a rim of dial left around them automatically.'));
      // ONE additional hand, chosen by what it indicates. The ring it reads is derived from that
      // choice, so this group holds the whole decision -- the pointer, whether its scale prints,
      // and how that scale is set -- and no half of it lives on another tab.
      const ex = D.extra && E.EXTRA_HANDS[D.extra] ? D.extra : 'none', pkv = D.perimShrink == null ? 0.88 : +D.perimShrink, shown = this.ringShown(D);
      fields.push(H('Additional hand · outer ring'), Sl('Additional hand', ex, this.extraOpts(E), (v) => this.setExtra(v)));
      if (ex !== 'none') fields.push(
        Ch('Outer ring', shown ? 'on' : 'off', [{ v: 'on', label: 'Printed on the rim' }, { v: 'off', label: 'Hidden' }], (v) => this.setRingOn(v === 'on')));
      if (shown) {
        const pss = D.perimSize == null ? 1 : +D.perimSize, pgD = E.perimGeom(D);
        const mm = pgD ? pgD.fs * ((E.MM_PER_PX || 0.0954)) : 0;
        fields.push(
          Rg('Chapter pull-in', pkv, 0.70, 1.00, 0.01, ud('perimShrink'), pkv.toFixed(2) + '×'),
          Nt('', 'The ring takes the rim, so the chapter print, its numerals and the hour, minute and seconds hands all scale inward by this factor to clear it. Pull in further for a wider ring; 1.00× hides it.'),
          Co('Ring colour', D.perimColor, ud('perimColor'), 'Auto · same as numerals'),
          Sl('Ring typeface', D.perimFont || 'same', [{ v: 'same', label: 'Same as dial' }, ...opt(E.TYPEFACES)], ud('perimFont')),
          Ch('Orientation', D.perimOrient || 'upright', [{ v: 'upright', label: 'Bottom aligned' }, { v: 'radial', label: 'Circular' }], ud('perimOrient')),
          Rg('Label size', pss, 0.6, 2.6, 0.05, ud('perimSize'), mm.toFixed(2) + ' mm'),
          Ch('Position', D.perimPos || 'ontick', [{ v: 'ontick', label: 'On the lines' }, { v: 'between', label: 'Between the lines' }], ud('perimPos')),
          ...this.ringStartFields(E, D, { Ch, Rg, Nt, ud }),
          Nt('', 'The band the labels print in grows with Label size, so the seven day names can be spread across the rim without touching the chapter print. Between the lines moves the labels, and the hand that reads them, into the middle of each division. The figure beside Label size is the height that print really gets on the panel — a dial px is ' + ((E.MM_PER_PX || 0.0954)).toFixed(3) + ' mm there — and below about 1.4 mm the labels stop reading at arm\'s length, however comfortable they look at 466 px on a monitor.'));
      } else if (ex !== 'none') fields.push(
        Nt('', 'Nothing prints on the rim, so the hand stops short of the chapter print. Pick Printed on the rim to put its scale back.'));
      fields.push(H('Colour'), Sw('Classic', col, E.COLOR_GROUPS.classic, ud('color')), Sw('Vivid', col, E.COLOR_GROUPS.vivid, ud('color')),
        Co('Custom', E.isHex(col) ? col : null, (v) => ud('color')(v === 'auto' ? 'cream' : v), 'Preset', 'dial'),
        H('Surface'), Sl('Finish', surf, opt(E.SURFACES), ud('surface')));
      if (surf === 'carbon') fields.push(Rg('Weave angle', +D.weaveAngle || 0, 0, 90, 1, ud('weaveAngle'), (+D.weaveAngle || 0) + '°'));
      if (surf === 'carbon' || surf === 'forged') { const wsv = D.weaveScale == null ? 1 : +D.weaveScale; fields.push(Rg(surf === 'carbon' ? 'Weave size' : 'Flake size', wsv, 0.4, 3, 0.05, ud('weaveScale'), wsv.toFixed(2) + '×')); }
      if (surf !== 'enamel') fields.push(Rg('Texture', D.texture == null ? 1 : +D.texture, 0, 2.5, 0.05, ud('texture'), (D.texture == null ? 1 : +D.texture).toFixed(2) + '×'));
      /* Two backdrop slots, one control set each.
       *   bg -- painted at the plate slot, under the geartrain, so it reads through an
       *        openwork dial and every aperture.
       *   sf -- painted on the dial face itself, over the surface material and UNDER the
       *        chapter print, so numerals, applied markers and the perimeter ring still
       *        land on top of it and openwork/apertures still cut it out.
       * Neither costs anything per tick. Each is baked into a canvas the device already
       * blends anyway -- the plate into the static base frames at 76 ns/px, the dial as
       * the one static1 layer compose() draws every frame -- so the art changes pixels
       * in an existing layer instead of adding a live plane (217,156 blends at the
       * measured 447 ns would be 97 ms of a 125 ms tick). */
      const artBlock = (p, title, blurb, price) => {
        const id = D[p + 'Art'] || null, a = id && E.assetGet(id);
        const box = D[p + 'Size'] == null ? 442 : +D[p + 'Size'];
        const zm = D[p + 'Zoom'] == null ? 1 : +D[p + 'Zoom'];
        const rot = Math.round(+D[p + 'Rot'] || 0);
        const bm = D[p + 'FinishBlend'] || 'off';
        const fits = id ? E.assetFits(id, box, box, D[p + 'Fit'] || 'cover', zm) : null;
        const drop = () => this.upd((d) => { [p + 'Art', p + 'Fit', p + 'Zoom', p + 'X', p + 'Y', p + 'Size', p + 'Opacity', p + 'Rot', p + 'FinishBlend', p + 'FinishColor', p + 'FinishKey', p + 'FinishKeyTol'].forEach((k) => delete d[k]); });
        const out = [H(title, 'art-' + p),
          Fi('Image', (e) => {
            const file = e.target.files && e.target.files[0]; if (!file) return;
            this.pickArt(file, title.split(' · ')[0].toLowerCase(), (aid) => this.upd((d) => { d[p + 'Art'] = aid; }));
          }, id ? (E.assetReady(id) ? 'loaded' : 'missing') : 'none yet')];
        if (!id) { out.push(Nt('', blurb)); return out; }
        out.push(Sl('Fit', D[p + 'Fit'] || 'cover', [{ v: 'cover', label: 'Cover · fill the dial, crop the overflow' }, { v: 'contain', label: 'Contain · whole image inside the dial' }, { v: 'stretch', label: 'Stretch · ignore the aspect' }], ud(p + 'Fit')),
          Rg('Offset X', +D[p + 'X'] || 0, -220, 220, 1, ud(p + 'X'), (+D[p + 'X'] || 0) + 'px'),
          Rg('Offset Y', +D[p + 'Y'] || 0, -220, 220, 1, ud(p + 'Y'), (+D[p + 'Y'] || 0) + 'px'),
          Rg('Size', box, 60, 940, 1, ud(p + 'Size'), box + 'px'),
          // Zoom is separate from Size because Size is the box the FIT works in: at
          // cover, enlarging Size enlarges the crop window and the visible content
          // together, while Zoom keeps the framing and resamples the art about the
          // centre. The dial clip is applied after both, so a zoomed image still
          // cannot cross the rehaut.
          Rg('Zoom', zm, 0.2, 4, 0.01, ud(p + 'Zoom'), zm.toFixed(2) + '\u00d7'),
          // Rotation turns the picture about its own centre, the same point the pan
          // offsets carry: aim an artwork that was scanned crooked without touching
          // Size or Offset, and the guide's dashed frame turns with it.
          Rg('Rotate', rot, 0, 359, 1, ud(p + 'Rot'), rot + '\u00b0'),
          Rg('Opacity', D[p + 'Opacity'] == null ? 1 : +D[p + 'Opacity'], 0, 1, 0.01, ud(p + 'Opacity'), Math.round((D[p + 'Opacity'] == null ? 1 : +D[p + 'Opacity']) * 100) + '%'));
        if (rot) out.push(Nt('', 'A rotated cover image can bring its edge inside the dial: raise Size or Zoom until the corners are covered again.'));
        /* The surface finish the dial would wear with no art (Surface, under Colour) is
         * drawn UNDER the picture, so an opaque image buries it. This lays the texture
         * back over the art in one composite pass, tinted to the author's target colour:
         * multiply presses it in, screen brushes it on, overlay runs both. enamel has no
         * texture at all, so the control appears for the six finishes that have one --
         * the four engine-turned ones and both carbons, whose weave/flake relief and
         * sheen are replayed as the mask instead of their own greyscale. */
        if (['guilloche', 'azure', 'metal', 'matte', 'carbon', 'forged'].indexOf(surf) >= 0) {
          out.push(Ch('Finish blend', bm, [{ v: 'off', label: 'Off \u00b7 art lies flat' }, { v: 'multiply', label: 'Multiply \u00b7 texture pressed in' }, { v: 'screen', label: 'Screen \u00b7 texture brushed on' }, { v: 'overlay', label: 'Overlay \u00b7 both' }], ud(p + 'FinishBlend')));
          // the number the engine itself falls back to, read off the engine, so the
          // slider label and the render cannot drift apart
          const tv = D[p + 'FinishKeyTol'] == null ? E.KEY_TOL_DEF : +D[p + 'FinishKeyTol'];
          if (bm !== 'off') out.push(Co('Blend colour', D[p + 'FinishColor'], (v) => this.upd((d) => { d[p + 'FinishColor'] = v; }), bm === 'screen' ? 'Warm white' : 'Umber', p),
            Nt('', 'The dial\u2019s ' + (E.SURFACES[surf] || surf) + ' texture lands on the whole image in this colour under the ' + bm + ' blend. The texture\u2019s own alpha is the mask, so only the raised and shadowed strokes tint \u2014 the flat picture stays untouched.'),
            /* A colour key narrows that pass to the part of the picture already wearing the
             * colour you point at: key a two-tone art to its red and only the red takes the
             * weave, while the other tone keeps the flat print. It is a gate ON TOP of the
             * blend -- leaving it on None keeps the whole-image blend exactly as it was.
             * Tolerance is the per-channel window the key matches inside; 0 asks for the
             * colour itself and nothing else, 128 lets most of the picture in. */
            Co('Key colour', D[p + 'FinishKey'], (v) => this.upd((d) => { d[p + 'FinishKey'] = /^#[0-9a-f]{6}$/i.test(v) ? v : 'none'; }), null, p, { none: true, noAuto: true }),
            Nt('', isHexColor(D[p + 'FinishKey'])
              ? 'Only the pixels already within ' + tv + ' of ' + D[p + 'FinishKey'] + ' receive the ' + (E.SURFACES[surf] || surf) + ' texture; the rest of the image keeps the flat print. Nothing in range and the blend paints nothing \u2014 widen the tolerance or pick the colour off the art.'
              : 'Optional. Pick a colour off the art to confine the texture to that region; leave it on None to blend the whole image.'));
          if (isHexColor(D[p + 'FinishKey'])) out.push(Rg('Key tolerance', tv, 0, 128, 1, ud(p + 'FinishKeyTol'), tv + ' / 128'));
        }
        const line = !E.assetReady(id) ? 'No bytes for ' + id + ' in this browser — the reference is in the Design ID, the .wckit bundle carries the art.'
          : fits && fits.ok ? price : fits.why;
        out.push(Nt('', a ? a.name + ' · ' + a.kind + ' · ' + a.w + '×' + a.h + ' source into a ' + Math.round(box * zm) + '×' + Math.round(box * zm) + ' box' + (zm !== 1 ? ' at ' + zm.toFixed(2) + ' zoom' : '') + ' · ' + Math.round(a.dataUrl.length / 1024) + ' KB · ' + line : line),
          mk({ label: ' ', isRow: true, isChips: true, opts: [{ v: 'drag', label: (this.state.artSlot === p ? 'Dragging the dial pans THIS slot' : 'Drag the dial to pan this slot'), bg: this.state.artSlot === p ? '#2a2722' : '#1f1d1a', bd: this.state.artSlot === p ? '#c9a25a' : '#3a352d', fg: '#e9e2d2', pick: () => this.setState({ artSlot: p, guide: 'on' }) }, { v: 'x', label: 'Remove this image', bg: '#1f1d1a', bd: '#3a352d', fg: '#e9e2d2', pick: drop }] }));
        return out;
      };
      fields.push.apply(fields, artBlock('bg', 'Background art · behind the geartrain',
        'No backdrop: the procedural plate is drawn. A PNG or SVG goes in at the plate slot, under the geartrain, and reads through an openwork dial and every aperture.',
        'Baked into the base frames with the plate: a one-time store, no cost per tick.'));
      fields.push.apply(fields, artBlock('sf', 'Dial surface art · on the dial, under the print',
        'No dial art: the procedural surface is drawn. A PNG or SVG prints on the dial face, over the surface material and under the chapter ring, numerals and applied markers, which you can still set yourself on this tab. Openwork and the apertures cut through it.',
        'Baked into the dial sprite: that layer is already blended every tick, so the art is free.'));
      const W = { aging: D.aging == null ? 1 : +D.aging, stains: D.stains == null ? 1 : +D.stains, dust: D.dust == null ? 1 : +D.dust };
      const wp = { New: [0, 0, 0], Light: [0.3, 0.15, 0.4], Vintage: [1, 1, 1], Relic: [1.5, 1.5, 1.5] };
      const cur = Object.keys(wp).find((k) => wp[k][0] === W.aging && wp[k][1] === W.stains && wp[k][2] === W.dust) || '';
      fields.push(H('Wear · dial and subdials'),
        Ch('Quick set', cur, Object.keys(wp).map((k) => ({ v: k, label: k })), (k) => this.upd((d) => { [d.aging, d.stains, d.dust] = wp[k]; })),
        Rg('Ageing', W.aging, 0, 1.5, 0.05, ud('aging'), Math.round(W.aging * 100) + '%'),
        Rg('Stains', W.stains, 0, 1.5, 0.05, ud('stains'), Math.round(W.stains * 100) + '%'),
        Rg('Dust & scratches', W.dust, 0, 1.5, 0.05, ud('dust'), Math.round(W.dust * 100) + '%'));
    }
    if (ready && s.tab === 'hands') {
      const HB = this.budget();
      /* Sweep is always offered: the seconds hand motion is a look, and the
       * tempo is decided separately by the row price below. Picking sweep asks
       * for the 15 fps tier; if the art cannot pay for it, budget() silently
       * clamps to the highest tier it can (never a hard refusal -- the user
       * chooses the look, the dial decides the rate). */
      const tickOpts = [{ v: '1', label: '1 tick / s' }, { v: '2', label: '2 / s' }, { v: '4', label: '4 / s' },
        { v: 'sweep', label: 'Sweep' }];
      fields.push(H('Second hand'), mk({ ...Ch('Motion', c0.tick, tickOpts, cf('tick')), valText: HB.fps + ' fps' }),
        Nt('', 'Available frame rates depend on how much of the dial opening is visible.'),
        H('Drop shadow · hands and raised/sunken subdials'),
        Ch('Direction', +c0.shAngle >= 0 ? 'fixed' : 'auto', [{ v: 'auto', label: 'Follow outer light' }, { v: 'fixed', label: 'Set angle' }], (v) => this.setCfg('shAngle', v === 'auto' ? -1 : fc ? fc.shadow.deg : 135)),
        ...(+c0.shAngle >= 0 ? [Rg('Angle', +c0.shAngle, 0, 359, 1, cf('shAngle'), c0.shAngle + '°')] : [Nt('Angle', (fc ? fc.shadow.deg : 0) + '° — away from the outer light (Light tab)')]),
        Rg('Distance', +c0.shDist, 0, 3, 0.05, cf('shDist'), (+c0.shDist).toFixed(2) + '×'),
        Rg('Softness', +c0.shBlur, 0, 3, 0.05, cf('shBlur'), (+c0.shBlur).toFixed(2) + '×'),
        Rg('Opacity', +c0.shOp, 0, 2, 0.05, cf('shOp'), Math.round(+c0.shOp * 100) + '%'),
        H('Defaults'), Ch('Shading', D.handShade || '3d', [{ v: '3d', label: '3D · bevelled' }, { v: 'flat', label: 'Flat' }], (v) => this.upd((d) => { d.handShade = v; })), Co('Lume', D.handLume, (v) => this.upd((d) => { d.handLume = v; }), 'None'), Sl('Seconds style', D.secStyle || 'classic', opt(E.SEC_STYLES), (v) => this.upd((d) => { d.secStyle = v; })), Sl('Finish', s.handFinish, [{ v: 'auto', label: 'Preset · ' + E.HAND_FINISHES[PR.handFinish] }, ...opt(E.HAND_FINISHES)], (v) => this.setState({ handFinish: v })));
      // The one spare hand is chosen here as much as on the Dial tab -- same dropdown, same rules,
      // so the pair is never split across two tabs. Its row in the list above carries its style;
      // this group decides what it indicates, whether its scale prints, and how wide the rim gets.
      const exH = D.extra && E.EXTRA_HANDS[D.extra] ? D.extra : 'none', pkvH = D.perimShrink == null ? 0.88 : +D.perimShrink, shownH = this.ringShown(D);
      const go = Math.round(+D.gmtOffset || 0);
      fields.push(H('Additional hand · outer ring'),
        Sl('Additional hand', exH, this.extraOpts(E), (v) => this.setExtra(v)),
        ...(exH === 'none'
          ? [Nt('', 'Hour, minute and seconds only. Pick what the one spare hand should indicate, and the scale it reads prints on the rim with it.')]
          : [Ch('Outer ring', shownH ? 'on' : 'off', [{ v: 'on', label: 'Printed on the rim' }, { v: 'off', label: 'Hidden' }], (v) => this.setRingOn(v === 'on')),
            ...(shownH ? [Rg('Chapter pull-in', pkvH, 0.70, 1.00, 0.01, (v) => this.upd((d) => { d.perimShrink = v; }), pkvH.toFixed(2) + '×')] : []),
            Nt('', shownH
              ? 'Its tip lands on its own ticks, and the Length slider in its row above trims it off them — 100% sits on the ticks, less stops short. Colour, typeface, orientation, position and where the ring starts are set with the rest of the dial, under Additional hand on the Dial tab.'
              : 'Nothing prints on the rim, so the tip stops inside the chapter print and its Length trims it from there. Pick Printed on the rim to take the rim back.'),
            ...(exH === 'gmt' ? [Rg('Zone offset', go, -12, 14, 1, (v) => this.upd((d) => { d.gmtOffset = v; }), go === 0 ? 'same as local' : (go > 0 ? '+' : '') + go + ' h')] : [])]));
    }
    if (ready && s.tab === 'move') {
      const fins = [{ v: 'train', label: 'Movement default · ' + (T.finish || 'brass') }, { v: 'brass', label: 'Brass' }, { v: 'copper', label: 'Copper' }, { v: 'gunmetal', label: 'Gunmetal' }, { v: 'rhodium', label: 'Rhodium' }, { v: 'random', label: 'Random per wheel' }];
      const spk = [{ v: 'train', label: 'Movement default · ' + (T.spokes || 'arched') }, { v: 'arched', label: 'Arched' }, { v: 'straight', label: 'Straight' }, { v: 'tapered', label: 'Tapered' }, { v: 'closed', label: 'Closed web' }, { v: 'holes', label: 'Lightening holes' }, { v: 'thin', label: 'Fine spokes' }, { v: 'random', label: 'Random per wheel' }];
      fields.push(H('Backdrop'), Sl('Plate', c0.plate, [{ v: 'train', label: 'Movement default · ' + E.PLATES[T.plate] }, ...opt(E.PLATES).filter((o) => o.v !== 'train')], cf('plate')),
        ...(() => { const pl = c0.plate === 'train' ? T.plate : c0.plate; return pl === 'stripes' ? [Rg('Stripe angle', +c0.plateAngle, 0, 179, 1, cf('plateAngle'), c0.plateAngle + '°'), Rg('Stripe width', +c0.plateScale, 0.4, 3, 0.05, cf('plateScale'), (+c0.plateScale).toFixed(2) + '×')]
          : pl === 'carbon' ? [Rg('Weave angle', +c0.plateAngle % 90, 0, 90, 1, cf('plateAngle'), (+c0.plateAngle % 90) + '°'), Rg('Weave size', +c0.plateScale, 0.4, 3, 0.05, cf('plateScale'), (+c0.plateScale).toFixed(2) + '×')]
          : pl === 'forged' ? [Rg('Flake size', +c0.plateScale, 0.4, 3, 0.05, cf('plateScale'), (+c0.plateScale).toFixed(2) + '×')] : []; })(),
        H('Wheels'),
        Sl('Metal', c0.finish, fins, cf('finish')),
        ...(T.gears.rear && T.gears.rear.length ? [Sl('Rear wheels', c0.rearFinish || 'train', [{ v: 'train', label: 'Movement default · ' + (T.rear === 'match' ? 'match wheels' : 'antique brass') }, { v: 'brass', label: 'Antique brass' }, { v: 'match', label: 'Match wheels (darkened)' }, { v: 'gunmetal', label: 'Gunmetal' }, { v: 'rhodium', label: 'Rhodium' }, { v: 'copper', label: 'Copper' }], cf('rearFinish'))] : []),
        Sl('Spokes', c0.spokes, spk, cf('spokes')),
        Ch('Etching', c0.etching, [{ v: 'none', label: 'None' }, { v: 'rings', label: 'Rings' }, { v: 'perlage', label: 'Perlage' }, { v: 'random', label: 'Random' }], cf('etching')),
        Ch('Pinions', c0.pinion, [{ v: 'on', label: 'On' }, { v: 'off', label: 'Off' }, { v: 'random', label: 'Random' }], cf('pinion')),
        Ch('Wheel centres', c0.center, [{ v: 'jewel', label: 'Jewels' }, { v: 'screw', label: 'Screws' }, { v: 'random', label: 'Random' }], cf('center')));
      const rr = this.RND.filter((k) => c0[k] === 'random');
      if (rr.length) fields.push(Ch('Re-roll', '', rr.map((k) => ({ v: k, label: '⟳ ' + ({ spokes: 'Spokes', pinion: 'Pinions', center: 'Centres', finish: 'Metal', etching: 'Etching' })[k] })), (k) => this.roll(k)));
      if (T.orbital) fields.push(H('Orbital carriage'),
        Rg('Escapement', +c0.orbD, 70, T.orbital.shape === 'kite' ? 150 : 160, 1, cf('orbD'), c0.orbD + 'px'),
        Rg('Arrow tip', Math.max(+c0.orbD + 50, +c0.orbTip), 120, 212, 1, cf('orbTip'), Math.max(+c0.orbD + 50, +c0.orbTip) + 'px'),
        Nt('', 'Distances from the centre. Pull them in to ring the carriage with a dial.'));
      if (T.cage) fields.push(H('Tourbillon'), Rg('Cage distance', +c0.cageD, 50, 150, 1, cf('cageD'), c0.cageD + 'px'), Nt('', 'Distance from the centre. Escapement openings follow it.'));
      fields.push(H('Placement'));
      if (tr.by) fields.push(Nt('Train angle', tr.deg + '° — set by #' + num(tr.by) + ' ' + compTitle(D.comps.find((c) => c.id === tr.by) || {}) + '. Move that opening to turn the train.'));
      else fields.push(Rg('Train angle', +c0.trainAngle || 0, 0, 359, 1, cf('trainAngle'), (+c0.trainAngle || 0) + '°'));
      const HB = this.budget();
      const balV = String(HB.fps < 12 && +c0.balance > 2 ? 2 : c0.balance);
      const balOpts = [{ v: '1', label: '1 / s' }, { v: '2', label: '2 / s' }].concat(HB.fps >= 12 ? [{ v: '4', label: '4 / s' }] : []);
      fields.push(H('Loop'),
        Nt('Frame rate', F + ' fps — ' + (HB.measured
          ? 'available frame rates depend on how much of the dial opening is visible'
          : 'measuring the geartrain row footprint…')),
        Ch('Balance beats', balV, balOpts, cf('balance')));
    }
    if (ready && s.tab === 'light') {
      fields.push(H('Inner glow · sources under the dial ring'),
        Rg('Strength', +c0.glow, 0, 2, 0.05, cf('glow'), (+c0.glow).toFixed(2)),
        Rg('Temperature', +c0.temp, 1500, 6500, 100, cf('temp'), c0.temp + 'K'),
        Rg('Hue', +c0.hue, 0, 359, 1, cf('hue'), c0.hue + '°'),
        Rg('Colour tint', +c0.hueSat, 0, 1, 0.02, cf('hueSat'), +c0.hueSat ? Math.round(c0.hueSat * 100) + '%' : 'off'),
        Rg('Axis', +c0.innerAngle, 0, 179, 1, cf('innerAngle'), c0.innerAngle + '°'),
        H('Outer shine · room light on glass & case'),
        Rg('Strength', +c0.outerLight, 0, 2, 0.05, cf('outerLight'), (+c0.outerLight).toFixed(2)),
        Rg('Temperature', +c0.outerTemp, 1500, 6500, 100, cf('outerTemp'), c0.outerTemp + 'K'),
        Rg('Hue', +c0.outerHue, 0, 359, 1, cf('outerHue'), c0.outerHue + '°'),
        Rg('Colour tint', +c0.outerSat, 0, 1, 0.02, cf('outerSat'), +c0.outerSat ? Math.round(c0.outerSat * 100) + '%' : 'off'),
        Rg('From', +c0.outerAngle, 0, 359, 1, cf('outerAngle'), c0.outerAngle + '°'),
        H('Camera'),
        Rg('White balance', +c0.wb, 2500, 10000, 100, cf('wb'), +c0.wb === 6500 ? 'neutral' : c0.wb + 'K'),
        Nt('', 'Lower = cooler picture, higher = warmer. Baked into exported layers.'));
    }
    if (ready && s.tab === 'case') {
      fields.push(H('Case metal'), Ch('Metal', s.caseMetal, ['auto', 'brass', 'copper', 'gunmetal', 'steel', 'rose'].map((id) => ({ v: id, label: id === 'auto' ? 'Match movement' : cap(id) })), (v) => this.setState({ caseMetal: v })));
      /* The opening is only authorable under "no case": every other style owns its own rim
       * geometry, and a slider that changed nothing (or fought the bezel) would be a lie.
       * Under "none" it is a measurement the author declares about the hardware, and it
       * draws nothing -- it is where the guide's ring sits, so the art can be aimed at it. */
      if (s.caseStyle === 'none') {
        const r = E.caseOpenR ? E.caseOpenR({ caseStyle: 'none', caseOpen: s.caseOpen }) : +s.caseOpen;
        const mm = (v) => (v * (E.MM_PER_PX || 0.0954)).toFixed(2);
        fields.push(H('Case opening'),
          Rg('Opening radius', r, E.CASE_OPEN_MIN || 90, E.CASE_OPEN_MAX || 233, 0.5, (v) => this.setState({ caseOpen: +v }), mm(r) + ' mm'),
          Nt('', 'Draws no metal and clips nothing: your own image carries the case, so it runs to the edge of the panel untouched at any setting. '
            + r.toFixed(1) + ' px (' + mm(r) + ' mm) is where the metal of the device begins — the guide paints the gold ring there, and everything the composer draws '
            + (r < 221 ? '(the chapter print, the complications, the subdials, the train, the hands, the glass) is eased about the centre to fit inside it.' : 'is at full size: the opening leaves the drawn face untouched at this radius.')));
      }
    }

    const slots = fc ? fc.handOrder.slice().reverse() : [];
    const hcfg = D.hands || {}, ce = D.centre || {};
    const handLabel = (id) => (id.startsWith('centre-') ? 'Centre ' + ({ hours24: '24-hour', gmt: 'GMT', day: 'day', date: 'date' }[id.slice(7)] || id.slice(7)) : '#' + num(id) + ' ' + (E && D.comps.find((c) => c.id === id) ? E.SCALES[D.comps.find((c) => c.id === id).scale].toLowerCase() : '') + ' hand');
    return {
      metaLabel: '466 × 466 · ' + F + ' fps · ' + B + '-frame bank · ' + (PR ? PR.name : '') + (s.designs[s.dial] ? ' (edited)' : '') + (T ? ' · ' + T.name : ''),
      ...this.viewVals(),
      mainRef: this.mainRef, paneRef: this.paneRef, cursor: this.drag ? 'grabbing' : 'pointer',
      onDown: (e) => this.onDown(e), onMove: (e) => this.onMove(e), onUp: () => this.onUp(), stageDown: (e) => this.stageDown(e),
      /* The two ways out of selection noise: a button because Escape is invisible, and a
       * guide toggle because "off means off" has to be the author's, not the component's
       * guess at when the overlay is answering a question. */
      clearSel: () => this.clearSel(), clearLabel: (s.sel != null ? 'Deselect ' + (s.selKind === 'hand' ? 'hand' : s.selKind === 'dial' ? 'dial' : 'item') : 'Deselect') + ' (Esc)',
      clearOp: s.sel != null || s.selKind != null ? 1 : 0.45,
      pickGuide: () => this.cycleGuide(),
      g: (() => { const lab = 'Guide: ' + s.guide, lit = s.guide !== 'off'; return { label: lab, bg: lit ? '#2a2722' : '#1f1d1a', bd: lit ? '#c9a25a' : '#3a352d', fg: lit ? '#f0d79a' : '#8d8472' }; })(),
      frameLabel: 'Frame ' + pad(s.frame + 1) + ' / ' + B + '  (' + F + ' fps)',
      playLabel: s.playing ? 'Pause' : 'Play',
      slowLabel: s.slow ? Math.max(1, Math.round(F / 5)) + ' fps' : F + ' fps',
      // step the BANK: the frames on the glass are the bank's, while F only
      // paces them, so wrapping here by F would strand half of a deep bank
      prev: stop((f) => (f + B - 1) % B), next: stop((f) => (f + 1) % B),
      toggle: () => this.setState((st) => ({ playing: !st.playing })),
      toggleSlow: () => this.setState((st) => ({ slow: !st.slow })),
      timeOpts: [['live', 'Live'], ['pose', '10:09'], ['cycle', 'Cycle dates']].map(([id, label]) => ({ ...on(s.timeMode === id), label, pick: () => this.setState({ timeMode: id }) })),
      seamText: s.seam, status: s.status,
      tabs: this.TABS.map(([id, label]) => ({ label, bg: s.tab === id ? '#2e2719' : 'transparent', fg: s.tab === id ? '#f0d79a' : '#b5ab97', pick: () => this.setState({ tab: id }) })),
      isDial: s.tab === 'dial', isComps: s.tab === 'comps', isHands: s.tab === 'hands', isMove: s.tab === 'move', isCase: s.tab === 'case', isExport: s.tab === 'export',
      fields, hasSel: !!sel, noSel: !sel, hasSelHead: !!sel && s.tab === 'comps',
      resetHands: () => this.upd((d) => { Object.values(d.hands || {}).forEach((hc) => { delete hc.style; delete hc.finish; delete hc.len; delete hc.wid; delete hc.sec; delete hc.lume; delete hc.shade; }); }), selTitle: sel ? '#' + num(sel.id) + ' ' + compTitle(sel) : '',
      dials: (ready ? Object.keys(E.PRESETS) : []).map((id) => ({
        ...on(id === s.dial), ref: this.cb('dialEls', id, (i) => this.drawDialThumb(i)),
        name: E.PRESETS[id].name + (s.designs[id] ? ' · edited' : ''), blurb: E.PRESETS[id].blurb,
        pick: () => this.setState((st) => ({ dial: id, sel: null, cfg: E.PRESETS[id].cfg ? { ...st.cfg, ...E.PRESETS[id].cfg } : E.PRESETS[st.dial].cfg ? Object.fromEntries(Object.entries(st.cfg).filter(([k]) => !(k in E.PRESETS[st.dial].cfg))) : st.cfg, train: E.PRESETS[id].train || (E.PRESETS[st.dial].train ? 'classic' : st.train), caseMetal: E.PRESETS[id].caseMetal || (E.PRESETS[st.dial].caseMetal ? 'auto' : st.caseMetal), caseStyle: E.PRESETS[id].caseStyle || (E.PRESETS[st.dial].caseStyle ? 'polished' : st.caseStyle) })),
      })),
      resetDesign: () => this.setState((st) => { const d = { ...st.designs }; delete d[st.dial]; const P = E.PRESETS[st.dial]; return { designs: d, sel: null, hands: 'auto', handFinish: 'auto', cfg: { ...(P.cfg || {}) }, train: P.train || 'classic', caseStyle: P.caseStyle || 'polished', caseMetal: P.caseMetal || 'auto', glassStyle: 'domed', status: 'Reset ' + P.name + ' to its preset' }; }),
      tabReset: () => this.resetTab(s.tab), hasTabReset: s.tab !== 'export', tabResetLabel: 'Reset ' + (this.TABS.find((t) => t[0] === s.tab) || ['', ''])[1].toLowerCase(),
      resetComps: () => { const P = E.PRESETS[s.dial]; this.upd((d) => { d.comps = JSON.parse(JSON.stringify(P.design.comps || [])); }); this.setState({ sel: null, status: 'Complications reset to ' + P.name }); },
      trainOpts: ready ? Object.entries(E.TRAINS).map(([id, t]) => ({ ...on(id === s.train), name: t.name, blurb: t.blurb, pick: () => this.setState({ train: id }) })) : [],
      compRows: D.comps.map((c, i) => ({
        ...on(c.id === s.sel), num: String(i + 1), title: compTitle(c), anchor: 'comp-' + c.id,
        meta: 'on ' + hostName(c.host) + (c.type === 'opening' && c.show === 'balance' ? ' · follows balance' : ' · ' + Math.round(c.angle) + '° · ' + c.dist + 'px') + ' · ' + (ready ? E.COMP_STYLES[c.type][c.style] || c.style : ''),
        pick: () => this.setState({ sel: c.id, selKind: 'comp' }),
        up: (e) => { e.stopPropagation(); this.moveComp(c.id, -1); },
        down: (e) => { e.stopPropagation(); this.moveComp(c.id, 1); },
        del: (e) => { e.stopPropagation(); this.delComp(c.id); },
      })),
      addOpts: ready ? Object.entries(E.COMP_TYPES).map(([t, label]) => ({ label: '+ ' + label + (sel && sel.type === 'subdial' && t !== 'subdial' ? ' on #' + num(sel.id) : ''), go: () => this.addComp(t) })) : [],
      handOpts: ['auto', ...(ready ? Object.keys(E.HAND_STYLES) : [])].map((id) => ({
        ...on(id === s.hands), ref: this.cb('handEls', id, (i) => this.drawHandOpt(i)),
        label: id === 'auto' ? 'Preset' : E.HAND_STYLES[id], pick: () => this.setState({ hands: id }),
      })),
      handRows: slots.map((id, i) => {
        const hc = hcfg[id] || {}, isC = id.startsWith('centre-'), k = isC ? id.slice(7) : null, shown = isC ? (!!ce[k] && hc.on !== false) : hc.on !== false;
        const len = +hc.len || 1, wid = +hc.wid || 1;
        // A seconds hand takes its shape from the seconds list, not the hand-style list:
        // handParts() ignores `style` once kind is 'second' (only breguet/spade cut a hollow
        // counterweight with it), so a style select on those rows was a dead control. The
        // seconds list takes that slot; isSec/notSec keep the two mutually exclusive.
        const secHand = id === 'centre-second' || !!D.comps.find((c) => c.id === id && c.scale === 'seconds');
        return {
          label: handLabel(id), meta: 'z ' + (slots.length - i) + (shown ? '' : ' · hidden'), op: shown ? 1 : 0.5, anchor: 'hand-' + id,
          styleOpts: mark([{ v: 'auto', label: 'Default style' }, ...(ready ? opt(E.HAND_STYLES) : [])], hc.style || 'auto'),
          finOpts: mark([{ v: 'auto', label: 'Default finish' }, ...(ready ? opt(E.HAND_FINISHES) : [])], hc.finish || 'auto'),
          onStyle: (e) => this.setHand(id, { style: e.target.value }),
          onFinish: (e) => this.setHand(id, { finish: e.target.value }),
          len, wid, lenText: Math.round(len * 100) + '%', widText: wid.toFixed(2) + '×',
          onLen: (e) => this.setHand(id, { len: +e.target.value }),
          onWid: (e) => this.setHand(id, { wid: +e.target.value }),
          isSec: secHand,
          notSec: !secHand,
          secOpts: mark([{ v: 'auto', label: 'Seconds · default (' + ((E && E.SEC_STYLES[D.secStyle || 'classic']) || '') + ')' }, ...(ready ? opt(E.SEC_STYLES) : [])], hc.sec || 'auto'),
          onSec: (e) => this.setHand(id, { sec: e.target.value }),
          lumeOpts: mark([{ v: 'auto', label: 'Lume · default (' + (D.handLume && D.handLume !== 'none' ? 'on' : 'none') + ')' }, { v: 'none', label: 'No lume' }, { v: '#cfe8b0', label: 'Lume · green' }, { v: '#e8d6a0', label: 'Lume · old radium' }, { v: '#9fd8ff', label: 'Lume · blue' }, { v: '#f4f1e6', label: 'Lume · white' }, { v: '#ff9a3c', label: 'Lume · orange' }, { v: '#ff7bd5', label: 'Lume · pink' }], hc.lume || 'auto'),
          onLume: (e) => this.setHand(id, { lume: e.target.value }),
          shadeOpts: mark([{ v: 'auto', label: 'Shading · default (' + (D.handShade === 'flat' ? 'flat' : '3D') + ')' }, { v: '3d', label: 'Shading · 3D' }, { v: 'flat', label: 'Shading · flat' }], hc.shade || 'auto'),
          onShade: (e) => this.setHand(id, { shade: e.target.value }),
          up: () => this.moveHand(id, 1), down: () => this.moveHand(id, -1),
          /* Art-backed hand. The record the engine hands back answers { c, px, py }, the
           * same three fields the procedural sprite fills, so the shadow silhouette, the
           * kit's hand PNG, its _shadow mask and the manifest pivot/centre/size all take
           * the art path untouched. (px,py) is authored in SOURCE pixels and the engine
           * scales it with the sprite, so a resize never moves the pivot off the hub. */
          hasArt: !!hc.art, artLabel: (hc.art && E.assetGet(hc.art)) ? E.assetGet(hc.art).name : hc.art ? 'no bytes for this reference' : 'Procedural',
          // the Width slider shapes the DRAWN silhouette (handParts' stroke); a sprite
          // row has no stroke to widen, so the slider leaves the row instead of lying
          showWid: !hc.art, hideWid: !!hc.art,
          rot0: hc.rot0 == null ? 0 : Math.round(+hc.rot0), zoom: hc.zoom == null ? 1 : +hc.zoom,
          onRot0: (e) => this.setHand(id, { rot0: +e.target.value || 0 }),
          onZoom: (e) => this.setHand(id, { zoom: Math.min(8, Math.max(0.05, +e.target.value || 1)) }),
          // stepped off the live design, not the value this row was rendered with: a
          // stepper that reads its own snapshot sticks on the second click
          onFacingUp: () => this.stepFacing(id, 15),
          onFacingDown: () => this.stepFacing(id, -15),
          onFacingReset: () => this.setHand(id, { rot0: null }),
          artNote: (() => {
            if (!hc.art) return 'PNG or SVG, any size: at Length 100% the art is scaled so the farthest ink along its point lands where this hand would reach drawn, and the slider scales from there. Upload art that points anywhere you like and set Facing to the clockwise angle it points at — the guide ray stands at the time the clock shows, and turning the facing swings the art under it. The device rotates hand sprites with antialiasing off (clock_view.c lv_image_set_antialias), so an enlarged or resampled source shows its loss plainly at every angle.';
            const a = E.assetGet(hc.art); if (!a) return 'That reference has no bytes in this browser — load the .wckit bundle that carries it.';
            // The engine length-fits the sprite: it measures the ink's reach from the
            // arbour and scales that -- in either direction -- so the upload's own
            // resolution never decides how long the hand is. k is that fit times the
            // author's zoom; it is read off the sprite the engine actually built, so
            // this row reports the pixels that ship instead of re-deriving a box.
            const hd = fc && (fc.hands || []).find((q) => q.id === id);
            const z = hc.zoom == null ? 1 : Math.min(8, Math.max(0.05, +hc.zoom || 1));
            const box = Math.round(2 * (isC ? Math.min(216, 205 * 0.66) : 90) * len) + 8;
            const k = hd && hd.sp && hd.sp.art && isFinite(+hd.sp.scale) ? +hd.sp.scale : Math.min(1, box / (a.w || 1), box / (a.h || 1)) * z;
            // The arbour can be off the visible ink in two ways worth telling apart: it
            // falls outside the artwork, or it merely crowds the plane edge and forced a
            // pad. Both sp.px/py and sp.ink are in plane px already, so the comparison is
            // direct -- it is only the *field* that shows the authored source number.
            const ib = hd && hd.sp && hd.sp.ink;
            const outsideInk = !!(ib && (hd.sp.px < ib[0] || hd.sp.px > ib[2] || hd.sp.py < ib[1] || hd.sp.py > ib[3]));
            const piv = hc.pivotX == null ? 'hub ink (auto)' : (+hc.pivotX) + ',' + (+hc.pivotY)
              + (outsideInk ? ' (off the artwork; the plane is grown to hold it)' : (hd && hd.sp && hd.sp.padded ? ' (near the plane edge; grown by the pad)' : ''));
            return a.name + ' · ' + a.kind + ' · ' + a.w + '×' + a.h + ' · ' + Math.round(a.dataUrl.length / 1024) + ' KB · length-fitted to this hand’s reach'
              + ' · drawn at ' + Math.round(a.w * k) + '×' + Math.round(a.h * k) + ' px' + (k < 0.985 ? ' (shrunk to fit the length)' : k > 1.015 ? ' (enlarged to fit the length — the pixels are interpolated at every angle)' : ' (1:1)')
              + ' · pivot ' + piv + ' · facing ' + (+(hc.rot0 || 0)) + '\u00b0' + (+(hc.rot0 || 0) ? '' : ' (assumed to point at 12)');
          })(),
          artRef: this.cb('artEls', id, (i) => this.drawArtThumb(i)),
          onArtFile: (e) => { const file = e.target.files && e.target.files[0]; if (!file) return; this.pickArt(file, handLabel(id) + ' sprite', (aid) => this.setHand(id, { art: aid, pivotX: null, pivotY: null, rot0: null, zoom: null })); },
          onArtClear: () => this.setHand(id, { art: null, pivotX: null, pivotY: null, rot0: null, zoom: null }),
          // No lower clamp and no upper clamp: an arbour legitimately sits outside the
          // visible ink (a hand whose hub hides under a subdial, or one whose image
          // carries only the counterpoise), and the engine grows the plane to hold it.
          onPivotX: (e) => this.setHand(id, { pivotX: e.target.value === '' ? null : +e.target.value }),
          onPivotY: (e) => this.setHand(id, { pivotY: e.target.value === '' ? null : +e.target.value }),
          pivotX: hc.pivotX == null ? '' : +hc.pivotX, pivotY: hc.pivotY == null ? '' : +hc.pivotY,
          // "hub ink" is the bake-side rule made one click: bake_clock.py:snap_pivot_to_hub
          // moves the plane so its hub ink lands on the rotation centre, and clearing the
          // authored pivot lets handSpriteAsset take the ink centroid itself.
          onPivotHub: () => this.setHand(id, { pivotX: null, pivotY: null }),
          onArtDown: (e) => { this._artDrag = id; this.artThumbSet(e, id); },
          onArtMove: (e) => { if (this._artDrag === id && (e.buttons & 1)) this.artThumbSet(e, id); },
          artSize: (E.assetGet(hc.art) || {}).w || 0,
          onPivotCentre: () => { const a = E.assetGet(hc.art); if (a) this.setHand(id, { pivotX: Math.round(a.w / 2), pivotY: Math.round(a.h / 2) }); },
          eye: isC && E.CAL_HANDS.indexOf(k) >= 0 ? 'remove' : shown ? 'shown' : 'hidden',
          // the spare hand has no eye of its own: what it indicates is the choice, and taking that
          // back to None is what removes it, so the button reads as the removal it performs
          toggle: () => (isC && E.CAL_HANDS.indexOf(k) >= 0 ? this.setExtra('none') : this.setHand(id, { on: !shown })),
        };
      }),
      caseOpts: ready ? Object.entries(E.CASE_STYLES).map(([id, label]) => ({ ...on(id === s.caseStyle), label, ref: this.cb('caseEls', id, (i) => this.drawCaseThumb('case', i)), pick: () => this.setState({ caseStyle: id }) })) : [],
      glassOpts: ready ? Object.entries(E.GLASS_STYLES).map(([id, label]) => ({ ...on(id === s.glassStyle), label, ref: this.cb('glassEls', id, (i) => this.drawCaseThumb('glass', i)), pick: () => this.setState({ glassStyle: id }) })) : [],
      layers: this.LAYERS.map(([id, n, name, note]) => ({
        ref: this.cb('layerEls', id, (i) => this.drawLayer(i)), num: n, name, note,
        eye: s.hidden[id] ? 'hidden' : 'shown', eyeFg: s.hidden[id] ? '#8d8472' : '#c9a25a', op: s.hidden[id] ? 0.45 : 1,
        toggle: () => this.setState((st) => ({ hidden: { ...st.hidden, [id]: !st.hidden[id] } })),
      })),
      kitNote: F + ' base frames + layers + sprites · .zip',
      dateKitNote: (() => {
        const cs = D.comps.filter((c) => (c.type === 'window' && (c.shows === 'custom' || c.shows === 'date')) || (c.type === 'flare' && c.icon === 'datetext'));
        if (!cs.length) return '';
        return 'Date sprite kits in this kit: ' + cs.map((c) => c.id + ' (' + (c.type === 'window' ? (c.shows === 'date' ? 'day-of-month' : 'window') : 'flare') + ' · "' + (c.type === 'window' && c.shows === 'date' ? 'D' : c.type === 'flare' && c.fmt == null ? 'ddd DD MMM' : c.fmt) + '" · ' + hostName(c.host) + ')').join(', ') + '. Each ships caps + a 1px tileable slice + one tile per glyph its field can ever produce, plus one shared shade.png for the window (the ring and its shadow depend on where a pixel sits in the window, not on the string, so it is drawn last over the finished strip) — the day-of-month field gets ten digit tiles instead of 31 whole-value PNGs, so one string is assembled on device instead of a sprite per value. manifest.json declares currentWidth per kit: the width the dial was cut for, never narrower than what the assemble rule builds.';
      })(),
      zipNote: F + ' × .png, hands at shown time · .zip',
      exportKit: () => this.exportKit(),
      // the theme bakes from the kit's own PNG bytes, so it rides the same render pass:
      // exportKit(true) collects the files and hands them to finishTheme instead of zipping
      exportTheme: () => this.exportKit(true),
      // FORMAT 2: the name is typed, so the field that types it sits beside the button,
      // and the byte counter beside it is the same count the baker applies, not JS chars.
      themeNameVal: this.state.themeName == null ? '' : this.state.themeName,
      themeNameIn: (e) => this.setState({ themeName: e.target.value }),
      themeNamePh: this.themeName(),
      themeNameLen: (() => { const G = window.GorbBake; const n = G ? G.nameBytes(this.themeName()) : String(this.themeName()).length; return n + '/' + (G ? G.NAME_MAX - 1 : 55) + ' B'; })(),
      themeNote: (() => {
        const G = window.GorbBake;
        if (!G) return 'baker not loaded';
        const nw = (D.comps || []).filter((c) => c.type === 'window').length, nf = (D.comps || []).filter((c) => c.type === 'flare').length;
        if (D.comps && D.comps.length > G.caps.comp) return 'too many complications for the device table (max ' + G.caps.comp + ')';
        // FORMAT 2 left one rule on the name -- the header's field size -- and it is the
        // only refusal here the owner could not predict from the dial itself.
        const tn = this.themeName();
        if (!G.nameOk(tn)) return 'theme name is ' + G.nameBytes(tn) + ' B; the header holds ' + (G.NAME_MAX - 1);
        return F + '-frame bank' + (nw ? ' · ' + nw + ' window' + (nw > 1 ? 's' : '') : '') + (nf ? ' · ' + nf + ' flare' + (nf > 1 ? 's' : '') : '');
      })(),
      exportBundle: () => this.exportBundle(),
      loadBundle: (e) => this.importBundle(e.target.files && e.target.files[0]),
      bundleNote: (() => { const n = new Set(this.artRefs(D).map((r) => r.id)).size; return n ? n + ' art asset' + (n > 1 ? 's' : '') + ' + design · .wckit' : 'design, no art · .wckit'; })(),
      artNote: this.artNote(this.artRefs(D)),
      exportFrames: async () => {
        if (this.busy()) return;
        this.setState({ status: 'Packing frames…' });
        const files = [];
        for (let f = 0; f < B; f++) files.push({ name: 'frame_' + pad(f + 1) + '.png', data: await this.png(this.frameCanvas(f, s.hidden)) });
        this.download(this.zip(files), 'watch-' + s.dial + '-frames.zip');
        this.setState({ status: 'Downloaded watch-' + s.dial + '-frames.zip' });
      },
      designId: did, idInput: s.idInput || '',
      onIdInput: (e) => this.setState({ idInput: e.target.value }),
      loadDesign: () => this.loadId(s.idInput || ''),
      copyId: () => { const done = () => this.setState({ status: 'Design ID copied' }), fail = () => this.setState({ status: 'Select the ID and copy it manually' }); navigator.clipboard ? navigator.clipboard.writeText(did).then(done, fail) : fail(); },
      selectAll: (e) => e.target.select(),
    };
  }
}

return Component;
};
