#!/usr/bin/env node
// GorbBake.stage3 (theme-editor-kit-plan K5): the stage bake, driven by a made-up radar app
// (slug "radar": a plate, a turning arm, a pool of plane rows sharing one sprite, a sound, a
// preview, a delta-animated scope) rather than the clock, so nothing clock-shaped can hide in
// it. Each section it writes is read back through the browser reader that guards the card.
//
//   node headless-stage.test.js
'use strict';
const path = require('path');
let fails = 0, checked = 0;
const ok = (c, n, x) => { checked++; if (!c || process.env.VERBOSE) console.log((c ? 'PASS ' : 'FAIL ') + n + (x ? '  [' + x + ']' : '')); if (!c) fails++; };
const G = require(path.join(__dirname, '..', 'baker@1', 'orbtheme-baker.js'));
const AUX = G.classId('aux'), PLATE = G.classId('plate'), W = 466;

const plane = (w, h, rgb, a) => { const p = { w, h, rgba: new Uint8Array(w * h * 4) }; for (let i = 0; i < w * h; i++) p.rgba.set([rgb[0], rgb[1], rgb[2], a == null ? 255 : a], i * 4); return p; };
const gradient = () => { const p = plane(W, W, [0, 0, 0]); for (let y = 0; y < W; y++) for (let x = 0; x < W; x++) p.rgba.set([x * 255 / W, y * 255 / W, 90, 255], (y * W + x) * 4); return p; };
const scope = (k) => { const p = plane(W, W, [6, 22, 13]); for (let y = 200; y < 260; y++) for (let x = 100 + k * 30; x < 140 + k * 30; x++) p.rgba.set([57, 255, 20, 255], (y * W + x) * 4); return p; };
const plateRow = (B, n) => ({ n: B.id(), 'class': PLATE, slot: 0, x: 0, y: 0, w: W, h: W, order: 0, alpha: 255, n_frames: n || 1 });
const readBack = (b) => { const v = G.verifyBundle3(b.bytes); return { v, sec: v.sections[0], hdr: v.header }; };

// ---- a radar section: plate, arm, a pool of three planes on one sprite, a preview
{
  const B = G.stage3({});
  const p = B.row(plateRow(B), gradient(), false, true);
  const arm = B.row({ n: B.id(), 'class': AUX, slot: 0, x: 233, y: 233, w: 20, h: 180, order: 2, alpha: 255, n_frames: 1, pivot_x: 10, pivot_y: 170 }, plane(20, 180, [57, 255, 20], 160), true, false);
  const jet = B.row({ n: B.id(), 'class': AUX, slot: 1, x: 120, y: 140, w: 38, h: 38, order: 3, alpha: 255, n_frames: 1 }, plane(38, 38, [255, 178, 60], 220), true, false);
  const pool = [2, 3].map((k) => B.element({ n: B.id(), 'class': AUX, slot: k, x: 100 + 60 * k, y: 300, w: 38, h: 38, order: 2 + k, alpha: 255, n_frames: 1, frame0: jet.frame0 }));
  const pvBytes = B.preview({ plane: plane(116, 116, [10, 40, 20]), x: 0, y: 0 });
  ok(p.frame0 === 0 && arm.frame0 === 1 && jet.frame0 === 2 && pool.every((r) => r.frame0 === 2), 'each row names its plane; a pool shares one sprite', [p.frame0, arm.frame0, jet.frame0].join());
  ok(B.elements.map((e) => e.n).join() === '1,2,3,4,5,6' && B.bank.sprites.length === 4, 'ids run in the order rows enter; the preview is one more sprite', B.elements.map((e) => e.n).join());
  // OS v1.2.12: the preview is stored as its RGB565 disc (orb_disc.h), so the picker shows it
  // round; §60: deflated, as a flat plane always shrinks
  ok(pvBytes > 6 && pvBytes < G.discBytes(116), 'the preview is its RGB565 disc, deflated', pvBytes);
  const b = B.finish({ name: 'Scope', slug: 'radar', abi: G.GEOM_ABI6, scalars: {} });
  const { sec, hdr } = readBack(b);
  ok(hdr.name === 'Scope' && sec.blob.elements.length === 6 && sec.blob.sprites.length === 4, 'the section reads back through the card’s reader: its rows and sprites', JSON.stringify([hdr.name, sec.blob.elements.length, sec.blob.sprites.length]));
  ok(sec.blob.slug === 'radar' && sec.rec.abi_min === 6 && sec.rec.abi_max === 6, 'under the app’s own slug and the ABI it asked for', sec.blob.slug);
  ok(b.meter && b.meter.artSize === B.bank.size && sec.rec.art_size === B.bank.size && b.n_elem === 6 && b.est && b.est.est_fps >= 1, 'finish answers the meter and the estimate', JSON.stringify([b.meter && b.meter.artSize, B.bank.size, b.est && b.est.est_fps]));
  const again = G.stage3({});
  again.row(plateRow(again), gradient(), false, true);
  const again2 = G.stage3({});
  again2.row(plateRow(again2), gradient(), false, true);
  const x1 = again.finish({ name: 'Same', slug: 'radar', abi: 6 }), x2 = again2.finish({ name: 'Same', slug: 'radar', abi: 6 });
  ok(Buffer.compare(Buffer.from(x1.bytes), Buffer.from(x2.bytes)) === 0, 'the same rows bake the same bytes');
}

// ---- raw bytes enter first and evenly; dither on and off
{
  const B = G.stage3({});
  const pcm = new Uint8Array(1000); for (let i = 0; i < pcm.length; i++) pcm[i] = i & 255;
  ok(B.put(pcm, 500, 1) === 0 && B.bank.size === 1000 && B.bank.sprites[0].off === 0, 'raw bytes go in as they are, at the head of the bank');
  B.row(plateRow(B), gradient(), false, true);
  ok(B.bank.sprites[1].off === 1000, 'and the planes follow them', B.bank.sprites[1].off);
  const D = G.stage3({}), T = G.stage3({ dither: false });
  D.row(plateRow(D), gradient(), false, true); T.row(plateRow(T), gradient(), false, true);
  ok(Buffer.compare(Buffer.from(D.bank.art), Buffer.from(T.bank.art)) !== 0 && D.bank.size === T.bank.size, 'a still plane is dithered unless the bake asks for truncation');
  ok(D.ditherAt({ x: 3, y: 4 }, true).x === 3 && T.ditherAt({ x: 3, y: 4 }, true) === null && D.ditherAt({ x: 3, y: 4 }, false) === null, 'dither sits at the row’s own x/y, and only for a still plane');
}

// ---- a delta-animated scope: keyframe, rects, the closing rect, the round trip
{
  const B = G.stage3({});
  const frames = [0, 1, 2, 3].map(scope);
  const rec = B.rowDelta(plateRow(B, frames.length), frames, 250, false, false);
  ok(rec.n_frames === 4 && rec.delay === 250 && (rec.flags & 64) && B.bank.sprites.length === 5, 'four frames: a keyframe, three rects, and the closing rect back to it', JSON.stringify([rec.n_frames, rec.flags, B.bank.sprites.length]));
  ok(B.bank.sprites[1].w < W && B.bank.sprites[1].diffx === 100, 'a frame after the first is only the rect it changed', JSON.stringify(B.bank.sprites[1]));
  const b = B.finish({ name: 'Sweep', slug: 'radar', abi: 6, scalars: { bg_mode: 1, gif_frames: 4, bg_frames: 0 } });
  ok(readBack(b).sec.blob.elements.length === 1, 'and it round-trips and reads back');
  // a chain that drifts is refused, not shipped
  const C = G.stage3({});
  const r2 = C.rowDelta(plateRow(C, frames.length), frames.map((f) => ({ w: f.w, h: f.h, rgba: f.rgba.slice() })), 250, false, false);
  C.bank.sprites[2].diffx += 40;   // a rect moved off the motion it carried
  let threw = null; try { C.finish({ name: 'Drift', slug: 'radar', abi: 6 }); } catch (e) { threw = e.message; }
  ok(/does not composite back to the frames it was handed/.test(threw || '') && r2.frame0 === 0, 'a delta chain that drifts is refused', threw);
  // a loop whose last frame is its first needs no closing rect
  const L = G.stage3({});
  const r3 = L.rowDelta(plateRow(L, 3), [scope(0), scope(1), scope(0)], 250, false, false);
  ok(!(r3.flags & 64), 'a loop that ends on its keyframe ships no closing rect');
}

// ---- frames the app picks (theme-pool-plan §2.6): a picked row, and a pool of rows sharing its bank
{
  const B = G.stage3({});
  B.row(plateRow(B), plane(W, W, [6, 22, 13]), false, true);
  const shapes = Array.from({ length: 7 }, (_, i) => plane(38, 38, [40 * i, 200, 90], 230));
  const one = B.rowPick({ n: B.id(), 'class': AUX, slot: 0, x: 60, y: 60, w: 38, h: 38, pivot_x: 19, pivot_y: 19, order: 2, alpha: 255 }, shapes, true, false);
  ok(one.n_frames === 7 && one.delay === 0 && one.frame0 === 1 && B.bank.sprites.length === 8, 'a picked row lays its frames contiguously, with no frame time');
  const recs = B.pool({ 'class': AUX, w: 38, h: 38, pivot_x: 19, pivot_y: 19, alpha: 255 }, shapes,
    [{ slot: 1, x: 100, y: 100, order: 3 }, { slot: 2, x: 200, y: 100, order: 4 }, { slot: 3, x: 300, y: 100, order: 5 }], true, false);
  ok(recs.length === 3 && recs.every((r) => r.frame0 === 8 && r.n_frames === 7 && r.delay === 0) && B.bank.sprites.length === 15, 'a pool: one bank, every row naming it', JSON.stringify(recs.map((r) => r.frame0)));
  ok(new Set(B.elements.map((e) => e.n)).size === B.elements.length && recs.map((r) => r.slot).join() === '1,2,3', 'each row its own id and slot');
  const b = B.finish({ name: 'Pool', slug: 'radar', abi: 6 });
  const blob = readBack(b).sec.blob;
  ok(blob.elements.filter((e) => e['class'] === AUX).length === 4 && blob.elements.filter((e) => e['class'] === AUX && e.n_frames === 7 && !e.delay).length === 4, 'the bundle reads back with four picked rows');
  ok(b.est && b.est.carrier !== 'sprite (carriage over a still)', 'picked rows are not priced as the carriage', b.est && b.est.carrier);
  let e1 = null; try { B.rowPick({ n: B.id(), 'class': AUX, slot: 9, w: 38, h: 38, pivot_x: 19, pivot_y: 19 }, Array.from({ length: 65 }, () => shapes[0]), true, false); } catch (e) { e1 = e.message; }
  ok(/at most 64 frames to pick from, not 65/.test(e1 || ''), 'more than 64 frames is refused at the bake', e1);
  let e2 = null; try { B.rowPick({ n: B.id(), 'class': AUX, slot: 9, w: 38, h: 38, pivot_x: 19, pivot_y: 19 }, [shapes[0], plane(40, 38, [0, 0, 0])], true, false); } catch (e) { e2 = e.message; }
  ok(/frames of one size/.test(e2 || ''), 'and a frame of another size', e2);
  let e3 = null; try { B.rowPick({ n: B.id(), 'class': AUX, slot: 9, w: 38, h: 38 }, [shapes[0]], true, false); } catch (e) { e3 = e.message; }
  ok(/one frame is a still row/.test(e3 || ''), 'and a bank of one', e3);
}

// ---- radar-plan §2.11: a theme's values, one blob of the app's own layout
{
  const B = G.stage3({});
  B.row(plateRow(B), gradient(), false, true);
  const vals = new Uint8Array(300); for (let i = 0; i < vals.length; i++) vals[i] = (i * 7) & 255;
  ok(B.values(vals, 2) === 300, 'values go in as they are');
  const b = B.finish({ name: 'Scope', slug: 'radar', abi: G.GEOM_ABI6, scalars: {} });
  const { sec } = readBack(b);
  const row = sec.blob.elements.filter((e) => (e['class'] | 0) === G.CLASS_VALUES);
  const spr = row.length ? sec.blob.sprites[row[0].frame0] : null;
  ok(row.length === 1 && row[0].slot === 0 && row[0].src === 2 && row[0].n_frames === 1 && !row[0].w && !row[0].h,
     'one values row at slot 0, version 2, placing nothing', JSON.stringify(row[0] || null));
  ok(spr && spr.w === 300 && spr.h === 1 && spr.size === 300, 'its plane is the bytes, 300 by 1', JSON.stringify(spr));
  ok(spr && Buffer.compare(Buffer.from(B.bank.art.subarray(spr.off, spr.off + 300)), Buffer.from(vals)) === 0, 'byte for byte');
  const refuse = (fn) => { try { fn(); return null; } catch (e) { return e.message; } };
  const C = G.stage3({});
  ok(/1 to 4096 bytes/.test(refuse(() => C.values(new Uint8Array(0), 1)) || ''), 'empty values are refused');
  ok(/1 to 4096 bytes/.test(refuse(() => C.values(new Uint8Array(4097), 1)) || ''), 'and values past 4096 bytes');
  ok(/from 1 to 255/.test(refuse(() => C.values(new Uint8Array(4), 0)) || '') &&
     /from 1 to 255/.test(refuse(() => C.values(new Uint8Array(4), 256)) || ''), 'and a layout version outside 1-255');
  C.values(new Uint8Array(4), 1);
  ok(/a theme carries one/.test(refuse(() => C.values(new Uint8Array(4), 1)) || ''), 'and a second blob');
}

// ---- refusals
{
  const B = G.stage3({});
  let e1 = null; try { B.preview({ plane: plane(100, 80, [0, 0, 0]) }); } catch (e) { e1 = e.message; }
  ok(/a preview is a square/.test(e1 || ''), 'a preview that is not square is refused', e1);
  let e2 = null; try { B.preview({ plane: plane(470, 470, [0, 0, 0]) }); } catch (e) { e2 = e.message; }
  ok(/the glass itself/.test(e2 || ''), 'and one past the glass (§60)', e2);
  const F = G.stage3({});
  F.rowFrames(plateRow(F, 20), Array.from({ length: 20 }, (_, k) => scope(k % 4)), false, false);
  let e3 = null; try { F.finish({ name: 'Too big', slug: 'radar', abi: 6 }); } catch (e) { e3 = e.message; }
  ok(/the bake's art bank is \d+ B against the \d+ B the slot leaves for banks/.test(e3 || ''), 'an art bank past the slot is refused before it is written', e3 && e3.slice(0, 80));
}

console.log(fails ? 'RESULT: FAIL — ' + fails + ' of ' + checked + ' checks' : 'all green (' + checked + ' checks)');
process.exit(fails ? 1 : 0);
