# Agent prompt: the unified commit path — one renderer for every dial (5 commits, bench-verified)

You are implementing `docs/unified-render-path-design-2026-10-30.md` in
`/home/greg/docker/Projects/gorb-os`, in the five commits its §11 names, on the bound
orb, with no owner input except at the one gate named in "Stop and ask" below. The
owner ruled the shape on 2026-10-30; that ruling is transcribed in the design doc and
binds everything.

Precedence order: (1) this prompt, (2) `docs/unified-render-path-design-2026-10-30.md`,
(3) `docs/app-platform-plan.md` §31/§33/§40 and `docs/render-cost-findings-2026-10-30.md`,
(4) `.clinerules`. Where the design doc and an older plan section disagree, the design
doc wins — it exists to supersede §31 contract rule 2 and the §5.2-era baker comment.

**Read before touching code:** `.clinerules` (all of it — the detached-command rule and
the no-attribution-trailer rule are both load-bearing here), `README.md`, the design
doc in full, `docs/render-cost-findings-2026-10-30.md` §3 and §5, plan §33, §39-§40,
and `main/gfx/orb_sched.h:30-60` (the L4 founding numbers). Re-verify every anchor in
the design doc's §13 against HEAD before editing it; anchors drift and the doc says so.

## Ground rules

1. Anything over 30 s runs detached and is polled:
   `setsid bash -c '<cmd>' </dev/null >/tmp/job.log 2>&1 & disown`. That includes
   every flash and every multi-file build. Each `run_commands` array element is its
   own shell — a `cd` in one does not carry to the next.
2. Commit order, always: edit → `git commit` → `idf.py reconfigure && idf.py build` →
   `tools/serflash.sh` (detached). It refuses an image whose embedded stamp differs
   from `tools/orb_version.sh`, so a stale log's `Project build complete` is not
   evidence. Never name a bare `/dev/ttyACMx`; the tools bind the orb by MAC.
3. Per commit, all gates green before the flash: every `tools/host_*_test.py`
   (`ls tools/host_*_test.py` — 31 suites), `go test ./...` in `tools/orb-manager`,
   `idf.py build` with zero new warnings, `tools/gfx_parity_test.py` identical against
   its pin, and the browser bake suites for any commit that touches the baker. Suites
   you extend grow vectors AND mutants — the repo's sentinel discipline.
4. A green build is not done. Each commit carries the bench evidence named in its
   slice below, quoted in your final report: command, log line, number.
5. `CLK_BG_DIRECT_PUSH == 0` must keep rendering exactly as it did at `1366568`. It is
   the kill switch and the classic path is its fallback, not dead code.
6. The tree is clean and HEAD pushed at every flash. Do not hand-edit
   `main/app/orb_app_factory_blobs.c`; regenerate with `tools/make_factory_apps.py`.
7. Firmware-visible change ⇒ bump `version.txt` (next patch off `1366568`, currently
   `v1.1.37`), then `idf.py reconfigure && idf.py build`. Run
   `tools/orb_version.sh --self-test` after touching the stamp composer.
8. Conventional commits, imperative summary under 50 chars, body says why. No
   `Co-Authored-By`, no trailer of any kind (`git log` shows them on old commits; that
   is the one thing there not to copy). Prose follows the writing rules in `.clinerules`.

## Suite commands

```bash
for t in tools/host_*_test.py; do python3 "$t" >/dev/null 2>&1 || echo "FAIL $t"; done
(cd tools/orb-manager && go test ./...)                      # hub, no board needed
python3 tools/gfx_parity_test.py                             # pinned reference
python3 tools/host_hand_aa_test.py                           # the AA vector set
python3 tools/host_sched_rung_test.py                        # the ask/hysteresis model
python3 tools/host_clock3_test.py                            # FORMAT-3 reader; --dump3f reads a blob
node tools/watch-composer/_selftest/headless-orbtheme.test.js    # needs bake venv
node tools/watch-composer/_selftest/headless-regression.test.js  # all 15 presets still build
```

(The headless suites live under `_selftest/`. `tools/watch-composer/` holds no test file.)

## The six decisions that are already made — do not re-litigate

1. **Carrier class chooses a restore pointer and nothing else.** No dial keeps a
   private render path. `orb_sched_l4_active()` stops mentioning GIF, and **also requires
   a claimed layer table** — see design §4.2b, without which a still dial whose table
   build refused its rows loses both renderers at once.
2. **A still plate restores from read-only XIP flash** (`blob() + clk_theme.plate_off`),
   which allocates nothing. The PSRAM canvas stays exactly one 434,312 B allocation, and
   a `bg_mode 1` dial whose canvas failed stays on the classic path (guard it explicitly).
3. **The carriage is a job layer, and is priced as one.** What §5.2's
   `deltaBank = o.carriage ? 0 : 1` (`orbtheme-baker.js:2734`) actually blocks is a
   *delta* plate under a carriage. A **still** plate under a carriage is already a
   first-class carrier — `splitPolicy3`'s `out('sprite', …)` at `:3368`/`:3378`, "plate
   rides the still". So slice 1 promotes those dials to the painter with no bake change
   at all, and only caged dials with a genuinely moving plate wait for slice 4.
4. **A ticking still dial becomes a scheduler ask** — and the `anim` test at
   `orb_sched.c:540-544` has to grow the matching term in the same commit, because
   `:544` returns the raw hand beat before `sched_fill_asks()` is ever consulted. An ask
   without that term is a commit that changes nothing.
5. **The AA grant goes false on any painter-owned dial, in slice 1.** The LVGL widgets
   stay live as the fallback, so a standing grant leaks bilinear hands on the next
   bounce. Denying it is not a style choice; leaving it is a defect. The cost-based test
   that may restore it is slice 5.
6. **There is no ABI bump anywhere in this work.** `GEOM_ABI4` is already 4. Slice 4
   changes which shape the baker emits, inside a dialect the reader accepts today.
4. **A ticking still dial becomes a scheduler ask**, so hysteresis can see it and
   demote it honestly instead of overrunning a floor it was never scheduled for.


## The slices, in order

Each is one coherent claim, benched alone, revertable alone. Do not merge two.

### Slice 1 — `feat(gfx): unify the commit path on a restore source`

Implement design §4.1-§4.3: `orb_sched_restore_src()` (gif canvas → NULL for `bg_mode 1`
→ NULL for a keyframe bank → still-plate XIP), `orb_sched_l4_active()` answers "is there
a source **and a table to weld with**", the `:784` guard and `:787` call test/pass it,
and the §4.3 line prints the decision at every build pass, co-located with the existing
`layer table:` line. The six mark sites (`clock_view.c:509/534/697/768/1202/1631`) get
**no edit** — they already branch on the predicate. Do not touch `bake_clock.py`,
`orbtheme-baker.js`, or `orb_clock3.c` here.

**AA ships in this slice, not slice 5.** Make `hand_aa_for()` return false whenever
`orb_sched_restore_src() != NULL`, and move `tools/host_hand_aa_test.py`'s still-dial
vectors and mutants with it, naming the reason in the test comment. Do not defer this:
the widgets stay live, so a standing grant repaints bilinear hands on the next bounce
and the dial shows two edge qualities until each hand happens to move. Record in the
commit body which end state (design §8 a or b) the owner chose for slice 5.

Evidence, same glass, same `--window 8:20`, and state whether the build had
`DISPLAY_BENCH` on — `orb_sched.c:844` means bench and non-bench figures differ:
- Station: `render path: l4=yes src=xip rows=N` in the capture, and the row goes from
  `7.0 fps / 1,863 µs/kpx` to **15 fps or a stated reason it did not**.
- Sector and Skeleton: **unchanged** fps, boot line `src=psram`, and
  `gfx_parity_test.py` identical. A regression here means the selector is not a
  superset of the old gate — stop and fix before continuing.
- **The table gate, tested by breaking it:** flash an arm whose layer build is forced to
  claim zero rows and confirm the dial still renders through LVGL rather than freezing.
  A still dial that goes dark is the one failure this slice must not ship.
- A still dial with a complication (moon/phases) and a date window: both refresh on
  rollover, no ghost, no flash. For a `fbgrab --diff` against the classic path, pin the
  time first (design §10) or say plainly that you compared by inspection instead.
- **A `sprite` caged dial** (carriage over a still plate — bake the Orbital preset and
  check the report says "plate rides the still"): this dial was paying LVGL for a full
  carriage box and now costs its swept arc. It is the largest free win in the design, so
  it is benched here and not deferred to slice 4. Watch specifically for a **ghost** — a
  vacated carriage arc that never restores, because the chord marks are the only thing
  standing between this dial and a visible rectangle.

### Slice 2 — `feat(sched): a ticking still dial is an ask, not a pass`

Design §6. Two halves, and the second is what makes the first reachable:
`sched_fill_asks()` (`orb_sched.c:501`) gains the ticked-still `DISTINCT` ask, **and**
`sched_tick_period_ms()`'s `anim` test (`:540-544`) gains
`|| (!clk_theme.second_sweep && clk_theme.tick_rate >= 2)`. Without that term `:544`
returns `orb_sched_tick_base_ms()` before `sched_fill_asks()` is consulted at `:545`, and
the commit is a verified no-op. `host_sched_rung_test.py` must carry a vector that
**fails when the `anim` term is removed** — a vector that merely passes with it is the
false pass `.clinerules` documents for `CONFIG_FOO`. Also correct
`sched_seed_floor()`'s comment (`:558-564`), which still calls the still dial un-schedulable.
Evidence: a ticked still dial over a busy band reports its **asked** rate or a demotion,
never an overrun.

### Slice 3 — `feat(bake): price the commit at bake time`

Design §5 and §7. `bakeClock3` returns `est_fps` from `box_kpx × RESTORE + ink_kpx × INK`
using the §5 table's coefficients; `splitPolicy3`'s `sentence` carries it; the Motion
block shows carrier class + estimate + the rung the pick will land on. Fix
`_devPace()`'s comment (`Watch Composer.dc.html:509-510`) and delete the carriage
exemption at `:526` so it gets a number. No firmware change in this commit. Evidence:
the bake report and the Motion line both print the estimate for Station, and
est-vs-measured lands inside ~15% on every dial benched so far. Refit the slopes only
if a point moves more than that, and say which point moved.

### Slice 4 — `feat(bake): a carriage dial's moving plate bakes as a delta bank`

Design §3 and §11. **Baker-only.** Drop the `o.carriage ? 0 : 1` fork at
`orbtheme-baker.js:2734` in **both** byte-identical copies — `tools/watch-composer/` and
`tools/orb-manager/web/composer/` (`diff -q` says identical; keep them so) — then bench.

Three things this slice must **not** do, each because a first draft said otherwise:
do **not** delete the `orb_clock3.c:513` refusal (it fires on `bg_frames && bg_mode == 1`,
the delta branch writes `bg_frames = 0` at `:2987`, so it never sees this shape, and
removing it only re-admits a malformed pair); do **not** bump `GEOM_ABI4` (it is already
4, and `:3276` already stamps the delta bank with it); do **not** hand the reader any
change. Re-check D29's 4,128,768 B cap per dial anyway — the blob should shrink, since a
keyframe plane costs 434,312 B.

**The deliverable is the bench, not the diff.** No dial in the fleet has ever run a delta
tick beneath a carriage (§2), so `tourbillon_advance`'s frame-swap marks and the delta
plate's `gif_apply_frame` marks meet for the first time here. Evidence: the dial holds
**12 fps**, flush drops from §33.3's 227 ms, boot line reads `src=psram`, and a
frame-by-frame capture shows no ghost along the vacated orbit arc.

### Slice 5 — `feat(clock): hand AA by cost, not by carrier class`

Design §6 and §8, and only what §8's option (b) or a restored grant requires:
`hand_aa_for()` (`clock_view.c:269`) reads the §5 estimate's headroom instead of
`bg_mode`. Slice 1 already left every painter-owned dial nearest, so this slice can only
add quality back — it can never be the place a mixed-edge dial gets created.

### After the last firmware commit

`cd tools/orb-manager && make firmware`, and commit the re-packed
`internal/firmware/bundle/orb-firmware.zip` so the hub never serves a firmware older
than the tree. Add the pointer lines to `docs/app-platform-plan.md`: §31 contract rule 2
is superseded by the design doc, §33's carriage row gains the before/after number.

## Stop and ask (one owner gate)

1. **Slice 1, before flashing:** the AA end state. The painter stays nearest and the
   grant is decided by cost (option a), or the painter gains bilinear for granted
   classes (option b)? The slice-1 **denial** of the LVGL grant on painter-owned dials is
   not optional and needs no approval; what the owner is choosing is what happens next.
   It is the same day's ruling as v1.1.37 and must not be reversed by inference.

There is no second gate. The ABI bump the first draft feared does not exist (decision 6),
so no slice in this plan strands an installed theme. If slice 4 turns out to need a
reader change after all, stop and report rather than inventing a refusal.

## Bench traps, all paid for already

- The clock report prints **only while the dial is foreground**. A reflash boots to the
  menu, the menu stops the tick, and you get no lines. `curl` the screen route first.
- `tools/capf.py <secs> <out>` — seconds first. Wrong order dies like a silent board.
- `timeout` must exceed the capture by ~10 s or an empty file lands where data should be.
- Capture ≥20 s: nothing prints before SNTP lands at t≈10 s.
- `render_arms.py` exits non-zero on an arm whose boot line is absent. Do not average
  two arms into one row to make a number appear.
- `tools/orb-manager` shares the serial port and periodically wins it: gate a capture on
  its sample count, not on exit status.

## Report format

Per slice: the commit SHA, the command run, the quoted log line, the number, and one
sentence on whether the slice's hypothesis held. Then a table of all five bench rows
against their `1366568` baselines. Then what you would do next, in one paragraph, and
what this measurement invalidated in the design doc — if nothing did, say so plainly.
