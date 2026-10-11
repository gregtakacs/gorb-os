// Canvas/DOM stubs sufficient to load watch-engine.js and the composer component under
// plain Node, with no browser. Gradients, shadows and text metrics resolve to no-ops:
// this checks decisions, not pixels. Pixel truth stays with export-test.html in Chrome.
'use strict';
const path = require('path');
const DIR = path.join(__dirname, '..');

const SIZES = {};                                    // dataUrl -> [w,h] for the fake Image
const ALPHAS = {};                                   // dataUrl -> Uint8ClampedArray RGBA fixture
function stubCtx() {
  // Real 2x3 CTM tracking: compose's opening lens and the shadow pre-scale it drives are
  // invisible unless getTransform answers with the matrix the call sequence built.
  let m = { a: 1, b: 0, c: 0, d: 1, e: 0, f: 0 }, stack = [];
  const mul = (n) => {
    m = { a: m.a * n.a + m.c * n.b, b: m.b * n.a + m.d * n.b, c: m.a * n.c + m.c * n.d, d: m.b * n.c + m.d * n.d, e: m.a * n.e + m.c * n.f + m.e, f: m.b * n.e + m.d * n.f + m.f };
  };
  const c = {
    canvas: null, globalAlpha: 1, globalCompositeOperation: 'source-over', filter: 'none',
    draws: [],                                        // every drawImage, for the assertions
    ops: [],                                          // ordered op log: what was drawn, and when
    geom: [],                                         // path/turn calls, kept OUT of ops: the
                                                      // ops cap is sized for drawImage/fillText
                                                      // assertions, and folding these in would
                                                      // evict them from a busy render
    lineWidth: 1, lineCap: 'butt', lineJoin: 'miter', strokeStyle: '#000', fillStyle: '#000',
    shadowColor: 'rgba(0,0,0,0)', shadowBlur: 0, shadowOffsetX: 0, shadowOffsetY: 0,
    font: '10px sans-serif', textAlign: 'start', textBaseline: 'alphabetic', imageSmoothingQuality: null,
    imageSmoothingEnabled: true, letterSpacing: '0px',
    createLinearGradient: () => ({ addColorStop() { } }),
    createRadialGradient: () => ({ addColorStop() { } }),
    createConicGradient: () => ({ addColorStop() { } }),
    createPattern: () => null,
    getTransform: () => ({ a: m.a, b: m.b, c: m.c, d: m.d, e: m.e, f: m.f }),
    getImage: () => null,

    createImageData: (w, h) => ({ data: new Uint8ClampedArray(Math.max(1, w * h * 4)), width: w, height: h }),
    measureText: (t) => ({ width: 6 * String(t).length }),
    getImageData: (x, y, w, h) => ({ data: (c.canvas && c.canvas._alpha) || new Uint8ClampedArray(Math.max(1, w * h * 4)), width: w, height: h }),
    putImageData: (d) => { if (c.canvas) c.canvas._alpha = d.data; },
    setTransform(a, b, c2, d, e, f) { m = arguments.length === 1 ? { a: a.a, b: a.b, c: a.c, d: a.d, e: a.e, f: a.f } : { a, b, c: c2, d, e, f }; }, resetTransform() { m = { a: 1, b: 0, c: 0, d: 1, e: 0, f: 0 }; },
    save() { stack.push({ ...m }); }, restore() { if (stack.length) m = stack.pop(); },
    // The geometry calls record their arguments so an assertion can read the guide and
    // the case mask as data. Each call stays a no-op apart from that push.
    translate(x, y) { this._g('translate', x, y); mul({ a: 1, b: 0, c: 0, d: 1, e: x, f: y }); },
    scale(x, y) { this._g('scale', x, y); mul({ a: x, b: 0, c: 0, d: y === undefined ? x : y, e: 0, f: 0 }); },
    rotate(a) { this._g('rotate', a); mul({ a: Math.cos(a), b: Math.sin(a), c: -Math.sin(a), d: Math.cos(a), e: 0, f: 0 }); }, transform() { }, setLineDash(d) { this._g('dash', d && d.length ? String(d) : 'solid'); },
    beginPath() { }, closePath() { }, moveTo(x, y) { this._g('moveTo', x, y); }, lineTo(x, y) { this._g('lineTo', x, y); },
    bezierCurveTo() { },
    quadraticCurveTo() { }, arc(x, y, r, a0, a1) { this._g('arc', x, y, r, a0, a1); }, arcTo() { }, ellipse() { }, rect() { }, roundRect() { },
    fill() { this._g('fill', this.fillStyle); }, stroke() { this._g('stroke', this.strokeStyle); }, clip() { this._op('clip'); }, fillRect(x, y, w, h) { this._g('fillRect', x, y, w, h); }, strokeRect(x, y, w, h) { this._g('strokeRect', x, y, w, h); }, clearRect() { },
    drawImage(img) {
      const s = (img && img._src && SIZES[img._src]) || [10, 10];
      this._w = s[0]; this._h = s[1];
      const d = { src: img && img._src, args: Array.prototype.slice.call(arguments, 1) };
      if (this.shadowColor && this.shadowColor !== 'rgba(0,0,0,0)') d.sh = { x: this.shadowOffsetX, y: this.shadowOffsetY, b: this.shadowBlur, c: this.shadowColor };
      this.draws.push(d);
      if (this.ops.length < 400) this.ops.push({ op: 'drawImage', src: img && img._src });
      if (img && img._src && ALPHAS[img._src] && c.canvas) c.canvas._alpha = ALPHAS[img._src];
    },
    fillText(t) { if (this.ops.length < 400) this.ops.push({ op: 'fillText', text: String(t) }); },
    strokeText(t) { if (this.ops.length < 400) this.ops.push({ op: 'fillText', text: String(t) }); },
  };
  // Helper pushes live on the context so the method shorthands above can share one cap.
  c._g = function (op) { if (this.geom.length < 600) this.geom.push({ op, a: Array.prototype.slice.call(arguments, 1) }); };
  c._op = function (op) { if (this.ops.length < 400) this.ops.push({ op }); };
  return c;
}
const CANVASES = [];
class FakeCanvas {
  constructor() { this.width = 300; this.height = 150; this._ctx = null; this._alpha = null; CANVASES.push(this); }
  getContext() { if (!this._ctx) { this._ctx = stubCtx(); this._ctx.canvas = this; } return this._ctx; }
  // the re-encode reports what the alpha pass actually did, so the test can assert on it
  toDataURL() {
    const d = this._alpha; let faint = 0, ink = 0;
    if (d) for (let i = 3; i < d.length; i += 4) { if (d[i] > 0 && d[i] < 6) faint++; if (d[i] >= 6) ink++; }
    const url = 'data:image/png;base64,' + Buffer.from(JSON.stringify({ w: this.width, h: this.height, faint, ink, n: d ? d.length / 4 : 0 })).toString('base64');
    SIZES[url] = [this.width, this.height];
    return url;
  }
}
global.Path2D = class { moveTo() { } lineTo() { } arc() { } closePath() { } rect() { } ellipse() { } bezierCurveTo() { } quadraticCurveTo() { } };
global.CanvasRenderingContext2D = function () { };
global.CanvasRenderingContext2D.prototype = stubCtx();
const loaded = [];
global.Image = class {
  constructor() { this.naturalWidth = 0; this.naturalHeight = 0; this.width = 0; this.height = 0; this._src = null; }
  set src(v) {
    this._src = v; loaded.push(this);
    const s = SIZES[v] || [0, 0];
    setTimeout(() => {
      if (v === 'BAD') { this.onerror && this.onerror({}); return; }
      this.naturalWidth = s[0]; this.naturalWidth && (this.width = s[0]); this.naturalHeight = s[1]; this.height = s[1];
      this.onload && this.onload({});
    }, 0);
  }
  get src() { return this._src; }
};
global.FileReader = class {
  readAsDataURL(file) { setTimeout(() => { this.result = file._url; this.onload && this.onload({}); }, 0); }
  readAsText(file) {
    setTimeout(() => {
      const u = String(file._url || '');
      this.result = u.indexOf('base64,') >= 0 ? Buffer.from(u.split('base64,')[1], 'base64').toString('utf8') : u;
      this.onload && this.onload({});
    }, 0);
  }
};
global.TextDecoder = require('util').TextDecoder;
global.document = {
  createElement: (t) => (t === 'canvas' ? new FakeCanvas() : { style: {}, getContext: () => stubCtx(), appendChild() { }, addEventListener() { } }),
  fonts: { load: () => Promise.resolve(), check: () => true },
  body: { appendChild() { } },
  documentElement: { style: {} },
};
const win = global.window = global;
win.document = document;
Object.defineProperty(win, 'navigator', { value: { userAgent: 'node' }, writable: true, configurable: true });   // node 21+ ships a getter-only global
win.performance = require('perf_hooks').performance;
win.devicePixelRatio = 1;
win.localStorage = (() => { const m = {}; return { getItem: (k) => (k in m ? m[k] : null), setItem: (k, v) => { m[k] = String(v); }, removeItem: (k) => { delete m[k]; } }; })();

// DialRenderer: the engine only needs the material helpers it calls by name. Anything
// the stub does not name explicitly resolves to a no-op, so the harness exercises the
// engine's own control flow instead of chasing missing canvas primitives.
const DR_BASE = {
  rng: () => { let s = 1; return () => (s = (s * 1103515245 + 12345) % 2147483648) / 2147483648; },
  grad: () => ({ addColorStop() { } }),
  FIN: { brass: {}, copper: {}, steel: {}, rhodium: {} }, BACK: {}, STEEL: {},
  BAL: { x: 300, y: 150 }, BAL0: { x: 300, y: 150 },
  FRONT: [], REAR: [], FRONT0: [], REAR0: [],
};
const NOOP = () => { };
const DR = new Proxy(Object.assign({
  configure: (o) => o, setTrain: NOOP, drawGear: NOOP, drawBalance: NOOP, jewel: NOOP,
  post: NOOP, screw: NOOP, bridge: NOOP, solid: NOOP, sideShadows: () => 0, sideWash: NOOP,
  enamelFill: NOOP, setTemp: NOOP, setWhite: NOOP, setLight: NOOP, applyWb: (c) => c,
}, DR_BASE), {
  get(t, k) {
    if (k in t) return t[k];
    if (typeof k === 'symbol') return undefined;
    return NOOP;
  },
});
win.DialRenderer = { TEETH_PER_LOOP: 12, bankFrames: (f) => f * 2, _: DR };

require(path.join(DIR, 'watch-engine.js'));
const E = win.WatchEngine;
module.exports = { E, SIZES, loaded, FakeCanvas, ALPHAS, CANVASES };
