# Agent prompt: gorb-os P8.2 + P8.3 — the OS schedules the repaint (autonomous, one shot)

You are implementing the rest of phase P8 in `/home/greg/docker/Projects/gorb-os`
in five commits, benching on the bound orb-B, with no owner input except at the
three eye-points named in "Owner prompts". The owner ruled P8's remaining shape
on 2026-10-22; that ruling is transcribed below and binds everything.

Precedence order: (1) this prompt, (2) the owner ruling section below, which
supersedes `docs/app-platform-plan.md` §23.6 R6/R7, §23.7 and §33 wherever they
disagree, (3) the plan and `docs/theme-clock-migrations.md`, (4) `.clinerules`.

Read before touching code: `.clinerules`, `README.md`, plan §23 (D32 as heard),
§33 (P8.1, the pick and the delivered-fps table), §38-§39 (the hook and its
gate), and `docs/theme-clock-migrations.md`. The tree state below was verified
against HEAD `5cbc93b` on 2026-10-22; re-verify each anchor before editing it.

## Ground rules (non-negotiable)

1. Any command that can exceed 30 s runs detached and is polled:
   `setsid bash -c '<cmd>' </dev/null >/tmp/job.log 2>&1 & disown`. This
   includes every flash. Poll `/tmp/serflash-orb-B.log` for `SERFLASH_DONE`.
2. Flash order, always: edit → `git commit` → `idf.py reconfigure &&
   idf.py build` → `tools/serflash.sh` (detached). It refuses an image whose
   embedded stamp differs from `tools/orb_version.sh`. Never name a bare
   `/dev/ttyACMx`; tools bind orb-B through `tools/orb_bind` on MAC
   `44:bd:8d:78:26:18` (IP `192.168.2.34` — re-prove via ip-neigh).
3. Boot logs: `python3 tools/capf.py 15 /tmp/boot.log` (the port open resets
   the board — that is the point). Pixels: `python3 tools/fbgrab.py --out ...
   --min-ink 1.5`; parity: `fbgrab --diff a.bmp b.bmp`.
4. A green build is not done. Each commit carries the bench evidence named in
   its slice below, quoted in your final report: command, log line, number.
5. Do not hand-edit `main/app/orb_app_factory_blobs.c`; regenerate with
   `tools/make_factory_apps.py` (a stale-file lint enforces it).
6. Per commit, all gates green before the flash: every `tools/host_*.py`
   suite, `go test ./...` in `tools/orb-manager` (firmware suite with
   `ORB_BUILD_DIR` set), `idf.py build` with zero new warnings,
   `tools/gfx_parity_test.py` identical against its `3116942` pin. Suites you
   extend grow vectors AND mutants (the repo's sentinel discipline).
7. After the last firmware commit: `cd tools/orb-manager && make firmware` and
   commit the re-packed `internal/firmware/bundle/orb-firmware.zip`
   (`chore(hub): re-pack the embedded bundle from the fw-stamped build`), so
   the hub never serves a firmware older than the tree.
8. Conventional commits, imperative summary under 50 chars, body says why.
   Prose follows the writing rules in `.clinerules`.
9. `HEAD` must be pushed and the tree clean at every flash. The sibling clone

## The owner ruling (2026-10-22) — binding; it amends D32's R6/R7

The owner's words, then the engineering reading. Where this differs from plan
§23.6/§23.7, this rules. Record it in the plan as §40 with the P8 row and D16
row amended by pointer (house pattern: the §-record carries the ruling; §0 rows
gain the pointer clause).

> "What every app/theme needs to declare is more than max fps. We benchmarked:
> realistically we can't achieve more than 15 fps on this hardware at all, and
> 4 fps is the floor for a complete repaint of the entire screen. So an app
> declares whether it wants fluid or distinct fps, and what the fastest is.
> Fluid: any fixed steady fps the display bears up to the limit — a 20 fps
> fluid request runs at 15, 14, 13, 7, whatever the display bears with the
> amount of movement. Distinct: it has to be 1, 2, 4, 8 or 12 and nothing else.
> Ultimately it's the theme that defines the refresh rate, not the app, since
> the clock app can have different themes with different requirements. A
> separate effort will compel the watch app to take per-element rates (the
> background at 12 fps while the second hand moves at 4): a theme-manifest
> question, so it can send proper rotational/offset values at the tick fps."

Reading, as ruled:

- **Hardware constants, now owner law** (sources: plan §33.2/§33.3):
  `ORB_SCHED_FPS_CEILING = 15` (delivered 12-15 on thin GIF-bank stacks; no
  animated stack beat 15 in any §33 run) and `ORB_SCHED_PANEL_FLOOR = 4` (the
  full-panel repaint measured 5 sustained at N=2/N=4 — the declared floor sits
  under the measurement, D9's discipline). The panel still commits static
  content at 33 ms; 15 is the ANIMATED ceiling, 4 the animated full-panel
  floor. Static screens are not scheduled by the pick at all.
- **Two rate modes.** `DISTINCT`: the runtime rate is exactly one of
  {1, 2, 4, 8, 12} — an authored frame animation must meet its frames on even
  tick boundaries. `FLUID`: any integer fps in [4, min(declared max, measured
  floor, 15)] — continuous motion (a sweep, a slide) wants steadiness, not a
  rung. Each app declares mode + max: a distinct max must be a ladder rung; a
  fluid max above 15 clamps to 15 at install with the disclosure pricing the
  clamped number.
- **The theme owns rates for themed apps; the app's declaration is consent.**
  The app declares mode+max = what its code can render at all; the theme's
  per-app section names the rate its design asks; the OS pick answers the
  theme inside the app's bound. For the clock this REVIVES the fields R6
  retired: `bg_fps`, `tourbillon_fps`, the `gif_delays` tempo and the sweep
  pair survive as theme-declared per-element asks. §23.7's retirement sentence
  is void for theme-carried fields (the app-side set it proposed is replaced
  by mode+max). av-kit re-bakes are NOT required: its existing scalars are
  already the asks the pick reads.
- **One cadence, per-element decimation.** The OS owns ONE flush beat; nothing
  changes there. The pick stays LCM-based across DISTINCT asks (4 and 12 pick
  12; the 4-fps element advances every third tick — the shipped precedent is
  `orb_sched_bg_rate()` holding plate frames `*every` ticks, `orb_sched.c:378`).
  A FLUID element contributes a cap, not a divisor. The deeper angle-pacing
  (second-hand VALUES stepping at 4 while the tick runs 12) is the named P8.4
  follow-on: build the pick's input schema to carry per-element asks so it can
  land later without a reshape, but do not implement the pacing in this work.
- **Hysteresis, signed by the owner as drafted:** demote after 2 consecutive
  report-seconds with delivered < pick; promote after 10 consecutive stable
  seconds carrying ≥ 25% model slack; demotes rate-limited to one per 30 s.
  Land as named constants. Validate on glass across at least three themes; if
  the numbers move, record the tuned ones in plan §40 and the commit body.
- **clockB is the second app** (owner pick): a second, smaller clock. It is
  NOT a second row over behaviour kind 3 — `orb_app.c:243` routes every kind-3
  row to one `s_clock_iface`, and `clock_view.c`'s state is file-static; two
  kind-3 rows would have clockB driving the original's hands. It is a new
  compiled behaviour kind `ORB_BEHAVIOUR_CLOCK_B = 4` (C6 append-only; never
  reuse a claimed id). Bound: a minimal analog face — hour and minute hands
  plus one status row — drawn ONLY through the layer table with D22 generated
  primitives, so it paints on a themeless board. DISTINCT mode, `period_ms
  1000`, one tick per frame. Its purpose is P8's pass lines: the first
  installed nonzero-beat app (the blocker §9 item 5 names for D16 delivery),
  the policed second stack, and "the dial's own hands unaffected while a
  second app is scheduled" measured literally against the original dial.


## Tree state (verified against `5cbc93b`, 2026-10-22)

- The pick: `orb_sched_pick_rung()` at `main/gfx/orb_sched.c:400`, declared at
  `orb_sched.h:170`. Pure, LCM-based, zero entry = free. ZERO production
  callers; `--gc-sections` drops it from the linked image (§33.4 records the
  delta-0 proof — your commit inverts that proof: the symbol must SURVIVE the
  link).
- The tick's period today is ad hoc at the bottom of `tick_timer_cb`
  (`orb_sched.c:649-668`): GIF mode rides `orb_bg_gif_tick_ms()` on the
  `s_tick_due` deadline ladder; a looping plate rides `orb_sched_bg_rate()`;
  otherwise `orb_sched_tick_base_ms()`. The measured delivered-fps/flush window
  already exists: `report_fps_if_due()` (`orb_sched.c:427`) recomputes every
  report-second — that is the floor source; the pick's `floor_fps` must read
  the MEASURED figure, never the ask (the header comment at `orb_sched.h:166`
  demands it).
- The clock's rate fields are runtime reads of the live bake
  (`orb_theme_geo.h`): `bg_fps`, `bg_every`, `tourbillon_fps`, `gif_delays`,
  `tick_rate`. Under the ruling these become the theme side of the asks.
- The beat seam: `svc_set_beat` (`orb_svc.c:585`) is policy-only; it records
  `s_row[slot].beat_ms`, may-only-slow, refuses a fast ask with a log line.
  `orb_svc_beat_ms()` is the read seam; the scheduler has no reader yet. That
  reader is commit 4.
- `ORBAPP_BEAT_MIN_MS 3000` (`orb_app_manifest.h:85`, enforced at
  `orb_app_manifest.c:394`) is D16's stale install floor, priced in the 185 ms
  whole-dial era. The ruling supersedes it: re-pin the install floor to the
  measured 4 fps panel floor (250 ms) for distinct, and 66 ms (15 fps) as the
  fluid install ceiling; amend §0's D16 row by pointer. Every existing factory
  manifest must still pass the re-pinned gate; check each, and say so in the
  commit body.
- `disclosure()` at `tools/orbapp.py:305` prints the single-`period_ms`
  sentence ("set_beat may only slow from here"); it grows the mode+max reading.
- Damage today: `svc_layer_touch` (`orb_svc.c:547`) casts the app's area to
  `(void)` with the comment "the OS's swept box is the mark";
  `orb_layers_touch()` (`orb_layers.c:262`) marks the row's FULL swept box
  through `gfx_swept_box`. The primitive below is already rect-honest:
  `gfx_mark_rows(y0, y1, x0, x1, …)`.
- Roles already exist: `orb_layer_desc_t.role` (`orb_layers.h:67`) feeds the
  `blend/s by layer` attribution (`gfx_role_snapshot`, CLK_PAINT_ROLES=8).
  Delivered-TICKS per role does not exist yet — that is the missing half of
  "attribute fps to one element" (§33.5).
- The clock's one wake `layer_touch` per hidden→visible transition is issued
  by `clock_layers_refresh()` in `main/clock/clock_view.c` (Commit 4, §39).
  When commit 5 makes the rect binding, that call must carry the waking box it
  already computed — and `gfx_parity_test.py`'s 3116942 pin is the fence.
- Schema anchors for per-element asks: `tools/theme_schema.py:451`
  `ELEMENT_FIELDS` (23 words, append-only past `scale16`), `:506`
  `EF_OS_ADV`, `:709-711` the bank-may-not-be-OS-advanced refusal. FORMAT 3
  element rows already carry `n_frames`; an fps column appends without a
  format bump (§23.5's append-only rule) — but prefer the clock dialect's
  existing scalars first; only grow the schema where a second consumer reads
  the column in firmware.
- The flush cost model the pick's floor reads (§5/§23.2): `flush_ms = -33.2 +
  215.1 µs/row + 648 ns/real-blend + 95 ns/synthetic-blend`, rows-driven.


## Commit 1 — `feat(sched): run the beat off the measured rung pick`

Rewire `orb_sched.c`/`.h` only. `orb_sched_pick_rung` gains mode awareness:
input becomes an array of `{uint16_t fps; uint8_t mode;}` (mode: free /
distinct / fluid) plus the measured floor; output is the tick fps. Distinct
asks are divisors through the existing LCM walk restricted to the ladder
{1,2,4,8,12}; a fluid ask caps the candidate set; free asks ride the floor.
Fallbacks keep P8.1's rulings: no clean distinct rung → the slowest distinct
ask; all free → min(measured floor, 15). Clamp the animated result into
[4..15]. Replace the old signature rather than living beside it — one rule,
one name (the header's own line: a rung that cannot fire gets deleted).

The caller: build the dial's asks from the live theme (GIF mode → the bank's
baked tempo as a distinct ask; plate bank → `bg_fps` distinct; carriage →
`tourbillon_fps` distinct; hands/sweep → fluid capped by `tick_rate` where
declared; static content → free). `floor_fps` reads the running measurement
window in `report_fps_if_due()`, seeded on the first second from the §33
table's figure for the current bank class. Drive `s_tick_due` and
`lv_timer_set_period` from the picked rung, deadline-anchored exactly as the
GIF ladder is today. GIF one-frame-per-tick stays intact: the bank's tempo is
a distinct divisor, so the pick cannot alias it. The plate's every-K hold
becomes an output of the pick (K = tick period / frame period), retiring
`orb_sched_bg_rate`'s separate decision while keeping its decimation
mechanism.

Hysteresis: the three signed constants, a small state machine at the
report-second boundary, and one `RUNG N->M` log line per move quoting the
measured figure.

Host: re-pin `tools/host_sched_rung_test.py` for the new signature — keep the
19 vectors' intent, add mode vectors (fluid 20 → 15; distinct {4,12} → 12;
distinct {4,8,12} against floor 5 → 4; all free against floor 17 → 15; a
binding fluid cap beats a loose distinct floor) — and grow the mutants.
Proof the caller exists: `nm` on the linked image shows the pick surviving
`--gc-sections` (the inverted §33.4 delta-0 check). Bench: av-kit boots,
ledger `layers: [9, 0]`, the serial `clock N fps` line sits at the picked
rung, and nothing regresses against the 2026-10-22 numbers (12 fps / 77 ms /
blend/s by layer unchanged at the same theme).

## Commit 2 — `feat(app): declare the rate mode and the fastest beat`

Manifest words: mode + max, appended INSIDE the CRC span beside `period_ms`,
gated at install in D27's shape (the same shape as caps and `period_ms`),
carried by `abi_min`/`abi_max` — no FORMAT bump (§23.5: append-only). Absent
words default to: distinct, the single ladder rung derived from `period_ms` —
so every shipped container stays byte-legal and reads as the one rung it
always was.

Move together (the tree's rule for manifest changes): writer `tools/orbapp.py`
(`--mode`/`--max-fps`; disclosure sentences — distinct: "may run at N fps max
(distinct: 1, 2, 4, 8, 12 only)"; fluid: "may run fluid up to N fps (the OS
picks the steady rate, ceiling 15)"); reader `orb_app_manifest.c` (validate:
distinct max ∈ ladder; fluid max clamped at 15 with the priced number in the
disclosure; refuse below the re-pinned floor); `install_decide` prices off the
max; `orb_app_t` carries the pair; the menu doors page prints the disclosure's
same words from the same bytes. Re-pin `ORBAPP_BEAT_MIN_MS` per the ruling:
250 ms for distinct (4 fps), 66 ms for fluid (15 fps); amend §0's D16 row with
the §40 pointer, and list in the commit body that every factory row still
passes the re-pinned gate. `svc_set_beat`: a distinct app may slow only to
another ladder rung or stop; a fluid app may ask any period in [66 ms, its
max]; both refusals log the number and the mode, the existing discipline.

Host: `host_orbapp_reader_test.py`, `host_orbapp_install_test.py`,
`host_app_registry_test.py` grow mode vectors and mutants. Regenerate the
factory blobs through `tools/make_factory_apps.py` only where a row's words
change — the defaults should bind and none should. Bench: orb-B boots every
factory row unchanged (the all-defaults read IS the back-compat proof, quote
the boot log); `/api/v1/apps` disclosure sentences quoted verbatim.


## Commit 3 — `feat(app): put a second clock on the dial`

clockB per the ruling: `ORB_BEHAVIOUR_CLOCK_B = 4` in `orb_app.h`'s enum
(append-only), a new `main/clock_b/clock_b_view.c` (its own file-static state;
do NOT reach into `clock_view.c`), a factory row generated through
`tools/make_factory_apps.py` like the P9 clock row (kind 4, DISTINCT,
`period_ms 1000`, `CAP_UI` free cap, no art claim). Face: hour and minute
hands plus a STATUS row, six layer-table rows or fewer, every pixel through
`layer_add`/`layer_set`/`layer_touch` and D22's generated primitives, so it
draws with no theme installed. Tick: read `orb_svc_t.now`, pose two hands —
that is the whole policy. It registers with the scheduler the way the dial
does (`orb_sched_start` owns one cadence; clockB rides the SAME tick callback
through the scheduler's app routing — the second policy must attach to the
existing scheduler, not mint a second lv_timer; if the scheduler's one-app-one-
policy shape resists this, generalize it in this commit and re-pin
`host_orblayers`/`host_carousel` accordingly, and say so in the body).

Bench: enable clockB on orb-B through the doors page (a press on the glass or
`/api/v1/apps` — the store door, not adb); show it; `fbgrab` shows a legible
face on the themeless baseline AND under av-kit; the ledger shows its rows;
the menu's refusal sentence at `menu_view.c:1011` ("code apps draw layers the
dial cannot paint yet") must NOT fire for it (it is compiled, not code-tier —
leave that sentence for the wasm import follow-on, do not delete it here).

## Commit 4 — `feat(sched): deliver the shared beat`

D16's delivery half, the commit §9 item 5 blocked for want of a second beat:
the tick base becomes the fastest active `orb_svc_beat_ms()` among RUNNING
apps (parked apps bill nothing — the dial's beat while it is off-glass comes
from its activation state, exactly the discipline §38's gate keeps); each
app's callback is measured against its declared budget per the plan's D16
wording; an app that overruns its budget twice in a row is parked for a cool-
down and the log says which app, what budget, what it cost; no burst
catch-up, deadline-anchored, D9's never-starve-the-rest. The overrunner test
is clockB: temporarily declare `period_ms 66` distinct on its factory row
(your choice of forcing mechanism — a build-time knob or the card re-upload —
document it, revert it before the commit, and pin the PARK path with a host
mutant).

Bench (P8's pass lines, all four, quoted): two-app board — dial and clockB
each hold their rung while the other is parked (`clock N fps` across a screen
change with no ramp artifact); the no-flicker 60 s sample over a rung boundary
(OWNER EYE-POINT E1, see below); the dial's own hands unaffected while clockB
is scheduled (`fbgrab --diff` of the dial at the same wall second, clockB on
vs clockB parked, ≤ the hand-advance baseline between same-second grabs); and
the park path proven once (log line quoted).

## Commit 5 — `feat(layers): bill the damage by element`

P8.3, the D31 route §33.5 handed forward:

- `orb_layers_touch(t, h, const lv_area_t *area)` intersects the app's rect
  with the row's swept box and marks the intersection; NULL area keeps today's
  full-box behaviour (an app may still claim nothing and pay everything).
  `svc_layer_touch` stops casting its area to `(void)`; the `orb_svc.c:550`
  comment and `orb_layers.h`'s header paragraph are rewritten to the new
  contract, and `orb_svc.h`'s `layer_touch` doc follows.
- The clock's wake touch in `clock_layers_refresh()` passes the waking box it
  already computes; `gfx_parity_test.py` must stay identical on the 3116942
  pin — if the wake rect changes pixels, the change is a bug in the rect, not
  a re-seam.
- Per-role DELIVERED counters: the commit pass already walks role-tagged jobs;
  count marked-ticks per role and extend the `blend/s by layer` line with
  `ticks/s by layer` so the report answers "the carriage held 12, the date
  window held 1" — the attribution §33.2 says the composite line cannot give.
- The pick's floor consumes the LIVE marked-row count (the §5 model: rows ×
  215.1 µs + blends), closing §33.3's open promise: a stack repainting inside
  thin rects marks fewer rows, prices a higher floor, and the hysteresis state
  machine promotes it toward the ceiling. Re-measure the damage-rect floor on
  clockB (small rects, no full-panel invalidation) and record the number in
  plan §40 — this is the figure D16's amendment said gets measured, not
  re-argued.
- Host: `host_orblayers_test.py` gains the rect vectors (a half-dial touch
  marks about half the rows; an off-row rect marks nothing and counts no
  refusal; NULL marks whole) plus mutants; `host_sched_rung_test.py` gains the
  live-floor vectors.

Bench: on clockB alone, the serial line shows its role's ticks and the re-
measured floor above the panel floor (that ordering IS the finding); av-kit
unchanged (its full-panel plate still prices the panel floor — the pick must
NOT promote the dial out of the 12-15 band it holds today; quote before/after
`clock N fps` and `phases` lines).


## Docs and bookkeeping, as you land

- Plan §40: the ruling transcribed, the four §0 pointers (D32 row, D16 row,
  the P8 phase row's status, R6's voided retirement sentence), the hysteresis
  constants as landed, the re-measured damage-rect floor. §8's P8 row reads
  P8.2/P8.3 landed with commit hashes when you finish.
- `docs/theme-clock-migrations.md`: append a line under the Commit 4 bench
  record noting the wake-touch now carries its rect (commit 5) and pointing at
  plan §40.
- Hub bundle re-pack + `chore(hub)` commit per ground rule 7. Push at the end.

## Owner prompts (the only human interaction; ask once each, continue once answered)

- E1 (commit 4): the first forced demote and the first promote on av-kit.
  Ask the owner to watch the glass for flicker across the boundary; the 60 s
  sample is their call, not just the meters.
- E2 (commit 4): the two-app dial check — their eye decides "the hands look
  untouched" while clockB runs.
- E3 (commit 3): clockB on glass — is the minimal face acceptable as P8's test
  subject. Acceptance is aesthetic; the pass line is mechanical either way.
If an owner prompt sits unanswered mid-session, proceed on the measurements,
mark the point UNJUDGED in the final report, and never call it passed.

## Out of scope — do NOT do these here

- Binding `orb.layer_*` wasm imports or flipping the doors sentence at
  `menu_view.c:1011` — the follow-on commit after P8, listed in the migration
  doc's wasm section.
- Deleting `blob()` or the classic LVGL fallback — P9's in-image bundles own
  that (§39's deviation record).
- Angle pacing (hand VALUES stepping slower than the tick) — the owner named
  it a separate effort (call it P8.4); commit 1 only leaves the asks schema
  ready for it.
- Partition table changes, FORMAT bumps, re-baking av-kit, touching
  `gorb-os-2`, or any second project folder in the workspace.

## Final report format

Per commit: hash, one-paragraph why, the quoted bench evidence (command +
observed line/number), gate lines (host suites with vector counts, go
packages, build warnings delta, parity pin status, `nm` proof on commit 1).
Then: the §40 diff summary, hub bundle commit hash, push confirmation, and the
board's end state (fw stamp via `/api/v1/status`, live theme, ledger rows,
which owner prompts were judged and how).

   `gorb-os-2` lags at `a7f9507` — do not bench there.
