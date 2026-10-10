# FPS budget benchmark: flush time vs refreshed rows and blended pixels

Status: **executed 2026-10-10** — findings in
`docs/fps-budget-findings-2026-10-10.md`, model written into
`docs/tourbillon-axis-spec.md` ("Per-frame budget"). Three deliberate
deviations from this plan, each recorded in the findings doc: the synthetic
block is 60x60 (3600 px, the plan's own sweep unit) rather than 16x16;
`DISPLAY_BENCH_ALPHA` was added, because the specced "opaque" ink takes
`blend_px`'s store shortcut and prices the wrong op; and V-A's `[140..330]`
band is a verified null (the packer's 16-px tiles widen it back to the shipped
union), so the tile-aligned `[144..319]` carries lever A's result.

Written 2026-10-10 from the cadence-probe session. Execute this in
a fresh chat — everything needed is in this file plus the repo. Do not ask the operator
for context; the facts are below.

## Goal

Measure, on the real CO5300 device, the per-tick flush cost as a function of (a) how many
panel rows a commit refreshes and (b) how many pixels the commit hook blends. Fit
`flush_ms ≈ t0 + b·rows + c·px`, report the coefficients, and answer the open question:
**which art-side reductions bring the mean loop from 159 ms to ≤ 125 ms (8 fps), or prove
it cannot be done with art alone.** Feeds the Art lever A/B decision deferred at the end
of `docs/tourbillon-axis-spec.md` ("The 7-vs-8 fps question" section).

## Ground rules (from `.clinerules` — non-negotiable)

- Any command over ~30 s goes detached: write `/tmp/foo.sh`, launch with
  `setsid /tmp/foo.sh </dev/null >/dev/null 2>&1 & disown`, poll its log. Builds that
  touch `assets/clock_assets.h`, every `esptool write_flash`, and every serial capture
  over ~12 s are detached. Never run them inline.
- App flash: `/tmp/serflash.sh` (writes `build/gorb-os.bin` to ota_0 + otadata, ~25 s);
  poll `/tmp/serflash.log` for `Hash of data verified` + `EXIT=0`.
- Assets flash: `/tmp/serflash_assets.sh` (writes `assets/clock_assets.bin` to 0x420000);
  poll `/tmp/serflash_assets.log` the same way.
- Build: `. $HOME/esp/esp-idf/export.sh && idf.py build` in the repo root. Steady-state
  single-file rebuild is ~10 s inline; anything after a header change, detach.
- Serial: `/dev/ttyACM0` @115200. Bake venv: `/tmp/bakevenv/bin/python` (has PIL+numpy;
  recreate per README if gone: venv + pip install pillow numpy).
- Never lose the shipped art: `assets/clock_assets.bin`/`.h` are committed. Every
  variant writes them; restore with `git checkout -- assets/clock_assets.bin assets/clock_assets.h`
  before finishing and prove it with
  `cmp <(git show HEAD:assets/clock_assets.bin) assets/clock_assets.bin`.
- Do NOT re-enable `CLK_TOURB_FILTER`. Do not touch `main/display/display.c` maths.
- New knobs are `#define` flags in headers (like `DISPLAY_CADENCE_PROBE`), NOT Kconfig
  symbols — a new Kconfig `default y` silently compiles out against the local untracked
  sdkconfig (documented trap).

## Known numbers (the setup-correctness gate for Phase 1)

Device: ESP32-S3R8, 466×466 CO5300 AMOLED QSPI, shipped kit =
`assets/sources/watch-orbital-kit3.zip` (orbital arrow, 8 frames @ 8 fps, 125 ms GIF
delays). From the 65 s probe capture at commit `6b127a1`:

| Quantity | Value |
|---|---|
| Mean tick loop | 159 ms (6.3 fps); gap modes 134.5 / 151.4 / 169.0 ms |
| Commit hook (composite) | ~110 ms/frame; paint split: carriage 36, glass 25, dial 14, min 12, hr 10 ms |
| Base GIF bank | pushes 34,304 px/tick (14,723 genuinely changed); row union is a solid 208-row band [128..335], ~97k px of full-width DMA |
| te | mean 9.4 ms, max 16.8 (capped at 25); dma mean 14 ms over 3–5 bands; gap↔dma r = −0.03 |
| fps line fields | `clock_view.c:1681`: `clock %u fps \| flush frames %u avg %llu ms \| px/s %llu (%llu%% page) chunks/s %u te/s %llu ms submit/s %llu ms` |
| CAD frame record | `CAD f=.. gap=.. dur=.. idle=.. te=.. dma=.. sub=.. mir=.. hook=.. px=.. ch=.. bands=..` (µs), drained 1×/s by the clock task |

If Phase 1 does not reproduce flush ~131–135 ms and fps 6–7, stop and fix the bench
setup before spending any flashes on sweeps.

## Phase 0 — migrate the bench harness into the repo

`/tmp/gb/` holds the working harness (still present 2026-10-10). Copy to `tools/bench/`:
`cap.py` (serial capture: `python3 tools/bench/cap.py <secs> <outfile>` — pyserial
RTS-reset capture), `cadence.py` (parses CAD frame/tick records + fps lines, prints
min/p50/p90/p99/max + gap-mode histogram), `rectaudit.py` (replays `bake_clock.py`'s
`bake_bg_bank` warm/replay simulation on a frames dir and reports per-tick pushed px,
changed px, and the row union — no flash needed). Run each once against
`/tmp/gb/cad65.log` (the 65 s capture) to prove the copies work, then commit
`tools/bench/`. If `/tmp/gb/` is gone, rewrite them from these descriptions against
`bake_clock.py` — `rectaudit.py`'s replay must reproduce the committed bank
rect-for-rect before it is trusted (that is exactly how it was validated once).

## Phase 1 — reproduce the baseline with the probe on

1. `main/display/display.h`: `#define DISPLAY_CADENCE_PROBE 1` (line ~62). Detached
   build (the flag drags display.c + clock_view.c), `/tmp/serflash.sh`, poll.
2. 60 s detached capture: `tools/bench/cap.py 60 /tmp/bench/base.log`, then
   `tools/bench/cadence.py /tmp/bench/base.log`. Gate against the Known-numbers table.
3. Re-bake the shipped kit through the documented chain and record whether it is
   byte-identical to the committed art (the reproducibility answer, not a gate):
   `/tmp/bakevenv/bin/python tools/prep_watch_kit.py assets/sources/watch-orbital-kit3.zip /tmp/bench/kit3prep`
   (no `--tick` — the shipped bank is the sweep), then
   `/tmp/bakevenv/bin/python tools/bake_clock.py /tmp/bench/kit3prep/theme --bg-frames /tmp/bench/kit3prep/frames --bg-fps 8 --tourbillon /tmp/bench/kit3prep/tourbillon --tourbillon-fps 8 --tick-rate 8 --dump-dir /tmp/bench/prev`.
   `cmp` the emitted bin/header against the committed pair; if they differ, THIS recipe
   becomes the baseline for every variant (consistency beats shipping parity — all
   variants use the same recipe), and note the fact in the findings.

## Phase 2 — pixel sweep: how much does each blended px cost?

Add a synthetic live layer to `clock_view.c`, gated `#define DISPLAY_BENCH 0` beside the
probe flag (same convention, no Kconfig). When on, the commit hook paints
`DISPLAY_BENCH_PX` opaque 16×16 blocks (solid colour from PSRAM, source-over) onto the
rows being rebuilt — all blocks inside the existing GIF row band so rows stay pinned at
the shipped ~208 — and marks those rows dirty through the same path the hand swept-boxes
use (`request_commit`, clock_view.c ~485). The compare gate skips DMA only for unchanged
rows; synthetic ink guarantees the change, and CAD `bands`/`px` prove it.

Sweep (block count ×3600 px): `PX ∈ {0, 18, 36, 72, 112}` blocks → added ink
0 / 65k / 130k / 259k / 403k px/tick. One rebuild+appflash+30 s capture per point, plus
one repeat of the middle point. Record per point: flush avg from the fps line, mean
`hook`/`px`/`bands`/`gap` from cadence.py. Expected slope ≈ 310 ns/px (glass at 25 ms
pushes ~80k px) — a flat curve means blend cost is row-bound, not px-bound: STOP and
re-check before Phase 3.

## Phase 3 — row sweep: how much does each refreshed row cost?

Host-side, no synthetic ink: the bank's changed region controls the row union, so shrink
it in art and measure the real pipeline. Copy `/tmp/bench/kit3prep/frames` (from
Phase 1's recipe) to `/tmp/bench/rows<R>/`; with PIL, paste frame 01's rows back over
every later frame outside the band `[233−R/2 .. 233+R/2]` so the delta rects cannot
escape it. Verify each variant's predicted rows/pushed-px with `tools/bench/rectaudit.py`
BEFORE flashing (it predicts the bank exactly; the flash is the expensive step). Re-bake
with `--bg-frames /tmp/bench/rows<R>` only (tourbillon untouched — carriage blends stay
constant so px barely move), appflash+assetsflash, 30 s capture.

Sweep `R ∈ {32, 64, 128, 208(=shipped control), 336}`; repeat 128. Same per-point record.
Expected: DMA is full-width per row, so rows should dominate — ~0.4–0.5 ms/row from
97k px ≈ 14 ms dma + hook restore. A flat rows curve falsifies the row-band theory and
re-opens the art-lever-A question; report it loudly if so.

## Phase 4 — art variants: validate the model on the real candidates

Host-side edits of the Phase-1 recipe output (no browser, no Watch Composer needed —
but the Composer sliders `orbD/orbTip/cageD` are the human path to the same knobs, see
`docs/watch-composer-plan.md` P1):

- **V-A (lever A, shorter band):** base frames blanked outside `[140..330]` (~190 rows)
  via the Phase-3 machinery.
- **V-B (lever B, thinner carriage):** for every `/tmp/bench/kit3prep/tourbillon/*.png`,
  alpha-zero all pixels at radius > 150 from (233, 233); re-bake `--tourbillon` only.
  Gate: `tools/kit_check.py` on the edited bank must PASS (staff jewel at the centre is
  untouched by construction; if it fails, the edit is wrong, not the art).
- **V-C:** both together.

60 s capture each. Record (flush avg, hook, px, bands, gap histogram) and the model
prediction from Phases 2–3 for the same measured (rows, px). **Prediction error is the
deliverable: ±8 ms or the model is not usable for the 8 fps decision — say so plainly.**

## Phase 5 — fit, decision, documentation

Least-squares `flush_ms = t0 + b·rows + c·px` (per tick) over every Phase 2/3/4 point
plus the baseline; b in ms/row, c in ms per 1000 px, with R² and residuals. Report:

1. The coefficients and their repeat-point spread (±5 ms agreement required, else
   increase capture to 60 s and re-run outliers).
2. The budget: the (rows, px) frontier where mean flush ≤ 125 ms — and whether the
   shipped point (208 rows, ~130k live+bank px) can reach it by Art A alone, B alone,
   or A+B, using each variant's measured values.
3. Verdict: pursue the winning art change (then re-author in the Watch Composer via the
   sliders/complications and re-export), or fall back to re-timing the kit GIF delays at
   143 ms for a stable 7 fps (plan-of-record fallback from the cadence session).

Write the result as a `## The fps budget model` section in
`docs/tourbillon-axis-spec.md` (table: point, rows, px, flush, predicted, error; plus
the frontier and the verdict) and commit with the `tools/bench/` code.

## Closing checklist (every run, no exceptions)

1. `DISPLAY_BENCH 0`, `DISPLAY_CADENCE_PROBE 0` in the headers.
2. `git checkout -- assets/clock_assets.bin assets/clock_assets.h` (or restore from the
   Phase-1 baseline if the committed pair was proven non-reproducible — keep that bin
   safe at `/tmp/bench/shipped.*` before touching anything).
3. Rebuild, `/tmp/serflash.sh`, assets flash if the bin moved; 15 s spot capture proves
   ~7 fps, flush 131–135 ms, and NO `CAD` lines (probe off).
4. `git status` shows only the intended additions; commit per conventional format.

## Deliverables and abort conditions

Deliverables: `tools/bench/{cap.py,cadence.py,rectaudit.py}` committed; a
`/tmp/bench/results.csv` (point, config, rows, px, hook, flush, fps) attached to the
final report; the spec section; a commit-stable tree.

Abort and report (do not push on) if: Phase 1 fails the setup gate; either sweep curve
is flat where the theories predict slope (each falsification is itself a finding —
report it and stop the dependent phase); the model misses Phase-4 validation by > 8 ms
(the decision needs a better model, not more flashes); or the device brownouts/resets
during a sweep (bench power issue, not a data point).


