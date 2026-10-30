# Watch Composer — evaluation and improvement plan

Status: evaluation only. The four files sit in `tools/watch-composer/` byte-identical
(md5-matched against `/mnt/c/Users/greg/Downloads/Skeleton Dial Animation/`). Nothing was
altered, renamed, or reformatted, per instruction. Line references below are into the
ingested copies.

Companion execution plan: `docs/fps-budget-benchmark-plan.md` (device fps vs row/pixel
budget).

## What it is

A single-file studio that generates the layered watch kits this repo consumes. Four
files:

| File | Lines | Role |
|---|---|---|
| `Watch Composer.dc.html` | 1065 | `<x-dc>` template + inline `class Component extends DCLogic`; tabs, live preview, exports, in-browser kit gate |
| `watch-engine.js` | 2148 | `window.WatchEngine`: procedural face engine — 15 presets, 6+ movements, complications, hands, case/glass, dual lighting |
| `dial-renderer.js` | 723 | `window.DialRenderer`: base plate + geartrain with a guaranteed seamless loop (every wheel advances exactly K=4 teeth per loop) |
| `support.js` | 1911 | Generated dc-runtime ("do not edit" header). Bootstraps React 18.3.1 + ReactDOM + Babel from unpkg with SRI pins (1144–1148), then interprets the template |

## Provenance — this is our vendor

`assets/sources/watch-orbital-kit3.zip` came from this exact tool: identical manifest
key set against `exportKit()` (html 609–708), the `WCD5-` design-ID format (365–373),
`innerScale: 0.8755656108597285` == engine `BEZEL_IN/DR`, `movement: "orbital"` with the
tool's defaults `orbD:128 orbTip:194 cageD:107`. Kit2 is the same schema; kit1 predates
`innerScale`. The whole `tourbillon-axis-spec.md` pipeline consumes this tool's output.

## Contract fit — verified, not assumed

Checked against `tools/prep_watch_kit.py`, `tools/bake_clock.py`, `tools/kit_check.py`:

| Consumer requirement | Producer | Status |
|---|---|---|
| `size: 466` (prep 251 hard gate) | const in both engine files | ok |
| `fps == frames`, ≤16 (bake 575) | `get F()` returns 8 for caged/orbital trains (html 309) | ok |
| `drawOrder[] tourbillon` with `centre/pivot/size/hubJewel/zeroRotationDeg` (prep 261–262) | html 643 | ok |
| `hands[]` `pivot/centre/size/zeroRotationDeg` + `shadow{pivot,offsetPx}` (prep 318–344) | html 681–682; shadow canvas = hand canvas + uniform blur margin, keeps prep's integer-margin proof green | ok |
| Bank 466×466, axis on a texel centre (the even-band lesson, commit `310ca00`) | `cageFrame(f,{k,dx,dy})` draws the carriage directly in kit space, `T+0.5` snap at html 633–636, never resampled | ok |
| kit_check gates (red mask a≥60, R>100, R−max(G,B)>40; odd bbox; centroid ≤0.25; spread ≤0.01) | `kitCheck()` html 578–608 is a true mirror; ships `kit_check.txt` in the zip | ok |
| Seam closure | `buildMovement()` renders frame F+1 and pixel-diffs it against frame 1, surfaced under the dial (html 455–460) | ok |
| Live layers for prep's fold (outer/dial/glass/subdials) | html 626–630 | ok |

## Strengths

1. The seamless gear maths, kit-space carriage draw, and texel-snap encode the hardest
   lessons this repo learned the expensive way — they are inside the tool now.
2. `kitCheck` runs before download and duplicates our Python gate exactly.
3. Seeded RNG + design-ID gives reproducible art; `busy()` blocks export mid-render.
4. Frame build yields via MessageChannel; thumbnails are token-cancelled and lazy.
5. The hand-rolled STORE-only ZIP writer (html 553–575) is minimal and reads cleanly in
   Python's `zipfile`.
6. Feature breadth: the two remaining perf levers are already sliders — `orbD`, `orbTip`,
   `cageD` are Art lever B; sparkle/flare complications are deletable components, Art
   lever A. Plus moon phase, custom date windows with `byDate`, lume, ~40 typefaces,
   case/glass styles, dual light, Kelvin white balance.


## Flaws

1. **No offline mode.** First paint throws without unpkg (`getReact()`), and ~45 Google
   Font families (html 11–21) need the network too. The critical 14 faces have a 4 s
   load race (html 400–401); the 30 display faces can flash fallback.
2. **No persistence.** No localStorage anywhere. A refresh loses every design not
   manually copied into the WCD5 field. No named library, no bulk import/export.
3. **The gate does not gate.** A failed `kitCheck` sets a status line but still
   downloads the zip (html 642). Only prep's `sys.exit`s stop a doomed kit downstream.
4. **Zero device-cost visibility.** (Superseded — see "The device-pace model" below.)
   The numbers this project cares about — per-tick blend
   traffic, base delta row band, carriage bbox ink — are invisible until prep prints
   them post-hoc. The tool cannot answer "is this design inside the 125 ms budget?".
5. **Not agent-operable.** Export is a DOM anchor click (html 545–548). Regenerating a
   kit from a design ID needs a human with a browser.
6. **No undo.** Per-tab reset and "Reset everything" only. Sliders lack numeric entry;
   `dist` quantizes to 0.5 px; the transport buttons are unlabelled glyphs.
7. **Export hygiene.** No export mutex (double-click = two zips); `png()` re-reads the
   wb setting per file, so a mid-export change mixes batches; `loadId` (374–383) accepts
   wrong-length payloads and silently applies undefined settings; the frames zip carries
   no manifest or design ID.
8. **Schema drift risks.** Manifest `layout` holds the design object while this repo's
   spec language uses "layout" for the orbital/static mode — a naming collision waiting
   to mislead. `hubJewel.bbox` here is `[x0,y0,x1,y1]`; `kit_check.py` reports
   `(xmin,xmax,ymin,ymax)` — a human comparing them will misread. `S=466 C=233 DR=221`
   are duplicated across three files with no cross-file version handshake;
   `WatchEngine` reaches into `DialRenderer._` internals via the window global, so
   mismatched file pairs fail with cryptic runtime errors.

## Improvement plan (priority order; nothing started)

- **P1 — Device Budget panel.** Reuse the `kitCheck` canvases to show live: base
  row-band (rows + px), carriage ink bbox, per-layer live ink
  (dial/glass/hands/shadows), against the ink-budget table in
  `docs/tourbillon-axis-spec.md`. The carriage sliders and the sparkle-comp toggle must
  move the printed numbers in real time. Makes Art levers A/B an interactive objective
  instead of flash-and-pray.
- **P2 — Headless export hook.** `window.__composerExport = async (designId) =>
  Uint8Array`. The agent can then regenerate any shipped kit deterministically: commit
  the design ID next to each zip in `assets/sources/`, export via Playwright. Kit zips
  stop being un-reproducible binaries.
- **P3 — Gate the gate.** Disable both download buttons (or require an explicit override
  checkbox) while `kitCheck` reports FAIL; add a PASS/FAIL badge on the Export tab.
- **P4 — Offline + docs.** Vendor react, react-dom, babel and the font CSS locally beside
  the HTML; add `tools/watch-composer/README.md` (provenance, how to open, the
  export → `prep_watch_kit.py` → `bake_clock.py` chain, gate expectations, the
  generated-status caveat on `support.js`).
- **P5 — Design library.** localStorage autosave with named designs + JSON
  import/export; keep WCD5 as the share format but add a version field and length
  validation to `loadId`.
- **P6 — Undo + numeric fields.** State is already one serializable object; add an undo
  stack (Ctrl+Z) and number inputs beside every slider.
- **P7 — Schema.** Dual-emit `layout`/`design` in the manifest (rename with a compat
  alias), pick one `hubJewel.bbox` convention and document it in the spec, add a
  one-line version handshake between the three JS files.
- **P8 — Export hygiene.** Mutex + progress/cancel; include `designId` and a mini
  manifest in the flattened-frames zip too.
- Optional: commit the file as `watch-composer.dc.html` (no space) for CLI ergonomics —
  deferred, the user asked for the originals untouched.

## One-line summary

A well-built generator that already embeds our hardest geometry lessons and exposes the
exact art knobs the 8 fps plan needs, held back today by no-offline, no-persistence, a
non-blocking gate, and no visibility into the device's blend budget.

## Custom assets — implemented 2026-10-11

Two dial backdrops (behind the geartrain, and on the dial face under the print), hand
sprites and transparent sub-dials now take uploaded art, with persistence and a bundle
format. This supersedes the P4/P5 lines above for those slots.

| Slot | Where | What ships |
|---|---|---|
| Plate backdrop | Dial tab → Background art · behind the geartrain | drawn at the plate slot inside `movement()`, so it is baked into the base frames |
| Dial surface art | Dial tab → Dial surface art · on the dial, under the print | drawn into the dial canvas inside `buildDial()`, over the surface material and under the chapter print, so it ships inside `layers/dial.png` |
| Hand sprite | Hands tab, one per hand row | replaces the procedural `{c, px, py}` sprite; the hub cap steps aside |
| Sub-dial plate | Style → Transparent · no plate | no layer, no drawOrder entry; its comps, holes and flares are carried by the dial |

Three decisions carry the design:

**Placement is the fps budget — but only relative to a layer that is already drawn.** An
opaque full-size plane blended as an EXTRA layer costs 217,156 blends at the measured
447 ns (`docs/fps-budget-findings-2026-10-10.md`) — 97 ms of a 125 ms tick. Both backdrop
slots avoid that by changing pixels in a canvas the device already draws either way: the
plate is stored into the bank once at 76 ns/px, and the dial is the one static1 layer
`compose()` blends every frame with or without art in it. Neither slot adds a blend, so
art on the dial face is not more expensive than art behind the geartrain. An earlier note
here claimed only the plate slot was affordable; that priced a second plane and was wrong.
What stays mandatory is that art never becomes its own layer. `assetFits()` also refuses a
bitmap its chosen fit would resample UP: the device samples hand rotations nearest on every
animated dial and on a sweeping seconds hand, and LVGL's rotated AA is reserved for the slow
hands of still dials (`clock_view.c` `hand_aa_for()`), so an enlarged source still shows its
loss wherever the dial pays the hot tick.

**A design references art; a bundle carries it.** `canonDesign` stores the asset id and
the placed geometry and deletes every default, so a design with no art serializes to
exactly the JSON it produced before this feature and no Design ID in the field changes.
`.wckit` (named, versioned JSON: design + settings + base64 assets) is the transfer
format. The layer kit also writes `sources/` and lists each asset in
`manifest.json → assets[]` with its slot, source size, drawn size, byte count and a
SHA-256 when the browser's Web Crypto offers one — it does not under `file://`, and the
manifest records that instead of inventing a digest.

**No on-the-fly vectorization.** Tracing in the browser would break prep's
integral-margin and reachability proofs and make the Design ID non-reproducible. SVG is
accepted and rasterized against its stamped `viewBox`; a viewBox-only SVG otherwise
decodes at Chrome's 300x150 fallback and arrives blurry or blank.

`prep_watch_kit.py` now warns instead of exiting when `layers/outer.png` has no opaque
pixel: that kit has no bezel to fold, so it bakes live, which is what the old message
promised and the old code refused to do.

## Verification limits

No browser on this box: this is a full code read plus a live schema match against
kit3's manifest — not a click-through of the running UI. The contract table above is
evidence-backed; the flaw list is code-evident except font-fallback behavior, which is
inferred from the load-race code.

## The device-pace model

`_devPace(b)` (`Watch Composer.dc.html`) predicts the rate the glass actually holds,
which the row ladder cannot price. `rowPrice()` measures the plate's own motion with
the hands hidden, and the needle cap books a sweep over plain plate. Neither prices a
sweep ACROSS opaque plates: `gfx_commit` restores each dirty row from the canvas and
repaints every layer overlapping it in table order (`orb_gfx.c`,
`orb_sched_commit_layers` in `orb_sched.c`), so a seconds wedge crossing a plate
re-blits it. The 2026-10-10 flush findings
(`docs/fps-budget-findings-2026-10-10.md`) back this mechanism: a full-width row band
costs milliseconds on every flush.

The crossed-plate tax is priced by the **arc the wedge sweeps**, not the number of
plates. Each crossed plate contributes an angular interval `[angle - w, angle + w]` at
its own bearing; overlapping intervals merge, so two plates sharing a bearing cost the
sweep once. The merged arc over 360 is `cov`, the fraction of beats whose wedge touches
a plate. `raw = b.fps * (1 - K * cov)`, with K = 2.31 while cast-shadow twins ride and
1.16 without. `raw` snaps down the `[15, 12, 8, 4]` ladder, floored at the scheduler's
4 and capped at `b.fps`. The tax is a full per-tick re-blit, so it runs to floor 4 for a
banked dial exactly as for an open one — `b.rows` proves nothing about the seconds wedge.

Two panel anchors fix the slope (measured in `07d3411`): aviator's bare ring (cov 0.00)
holds tier 15, and perpetual's four gilt-twin subdials (cov 0.32) clamp to 4. A
date-only `diver` crosses about one percent of the beat and lands at 12; `modern`
crosses about ten percent and lands at 8. An earlier one-step bank grace (`floorStep`)
read the banked perpetual as 12 and modern as 12, contradicting both anchors; it is
removed. `7649661` priced the tax on banked carriers, and the grace quietly re-exempted
them.

`tools/watch-composer/_selftest/probe_pace.cjs` is the regression trip. It mounts the
real `Component` and prints `_devPace` per preset, asserting those targets without a
browser. The `Cast shadows` chip derives its state from the same twin gate the bake
reads (`shMove` and `shOp` both positive), so the control and the export cannot disagree
for a legacy design that carries `shOp: 0` with no `shMove`.
