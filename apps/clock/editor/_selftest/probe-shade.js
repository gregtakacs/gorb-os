(function () {
  const E = window.WatchEngine, DCLogic = function () { };
  try {
  DCLogic.prototype.setState = function (p) { Object.assign(this.state, typeof p === 'function' ? p(this.state) : p); };
  const inst = new (window.COMP_CLASS(DCLogic))({});
  inst.ready = true; inst.rendering = false;
  inst.state.dial = 'modern'; inst.state.timeMode = 'cycle'; inst.state.frame = 0; inst.state.hidden = {}; inst.state.playing = false;
  inst.budget = () => ({ fps: 8 });
  inst.base = E.movement(inst.opts());
  inst.gt = Array.from({ length: inst.B }, (_, f) => inst.base.render(f).gt);
  const fc = inst.face();
  const kt = fc.textKits(1).filter(t => t.kind === 'date')[0];
  const out = [];
  const h = fc.lay.comps.filter(c => c.id === kt.id)[0].hole;
  const W = kt.shade.canvas.width, H = kt.shade.canvas.height;
  const d = kt.shade.canvas.getContext('2d').getImageData(0, 0, W, H).data;
  const A = (x, y) => d[((y * W) + x) * 4 + 3];
  out.push('shade canvas ' + W + 'x' + H + '   hole ' + h.w + 'x' + h.h + ' rad ' + h.rad + ' style ' + h.style + ' rot ' + h.rot);
  out.push('max alpha per COLUMN: ' + Array.from({ length: W }, (_, x) => { let m = 0; for (let y = 0; y < H; y++) m = Math.max(m, A(x, y)); return m; }).join(''));
  out.push('max alpha per ROW:    ' + Array.from({ length: H }, (_, y) => { let m = 0; for (let x = 0; x < W; x++) m = Math.max(m, A(x, y)); return m; }).join(''));
  const cell = (x0) => { let s = 0, n = 0; for (let y = 2; y < H - 2; y++) for (let x = x0; x < x0 + 12; x++) { s += A(x, y); n++; } return +(s / n).toFixed(1); };
  const slide = [];
  for (let p = 0; p + 12 <= W; p++) slide.push(cell(p));
  out.push('mean alpha of a 12px digit cell slid across the strip: ' + slide.join(','));
  out.push('  → min ' + Math.min.apply(null, slide) + ' max ' + Math.max.apply(null, slide) + ' spread ' + (Math.max.apply(null, slide) - Math.min.apply(null, slide)));
  // what the plate's own vertical gradient does, separately, for context
  const sd = kt.slice.canvas.getContext('2d').getImageData(0, 0, 1, H).data;
  out.push('slice column RGB down the strip: ' + Array.from({ length: H }, (_, y) => sd[y * 4] + ',' + sd[y * 4 + 1] + ',' + sd[y * 4 + 2] + '@' + sd[y * 4 + 3]).join(' | '));
  window.__SHADE = out.join('\n');
  } catch (e) { window.__SHADE = 'ERROR: ' + (e && e.stack || e); }
})();
