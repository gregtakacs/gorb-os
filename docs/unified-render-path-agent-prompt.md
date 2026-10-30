# Agent prompt: the unified commit path — one renderer for every dial (5 commits, bench-verified)

You are implementing `docs/unified-render-path-design-2026-10-30.md` in
`/home/greg/docker/Projects/gorb-os`, in the five commits its §11 names, on the bound
orb, with no owner input except at the two gates named in "Stop and ask" below. The
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
node tools/watch-composer/headless-orbtheme.test.js          # needs bake venv
node tools/watch-composer/headless-regression.test.js        # all 15 presets still build
```

## The four decisions that are already made — do not re-litigate

1. **Carrier class chooses a restore pointer and nothing else.** No dial keeps a
   private render path. `orb_sched_l4_active()` stops mentioning GIF.
2. **A still plate restores from read-only XIP flash** (`blob() + clk_theme.plate_off`),
   which allocates nothing. The PSRAM canvas stays exactly one 434,312 B allocation.
3. **The carriage is a job layer, and is priced as one.** What §5.2's
   `deltaBank = o.carriage ? 0 : 1` (`orbtheme-baker.js:2734`) actually blocks is a
   *delta* plate under a carriage. A **still** plate under a carriage is already a
   first-class carrier — `splitPolicy3`'s `out('sprite', …)` at `:3368`/`:3378`, "plate
   rides the still". So slice 1 promotes those dials to the painter with no bake change
   at all, and only caged dials with a genuinely moving plate wait for slice 4.
4. **A ticking still dial becomes a scheduler ask**, so hysteresis can see it and
   demote it honestly instead of overrunning a floor it was never scheduled for.


## The slices, in order

Each is one coherent claim, benched alone, revertable alone. Do not merge two.

### Slice 1 — `feat(gfx): unify the commit path on a restore source`

Implement design §4.1-§4.3: `orb_sched_restore_src()` (gif canvas → still-plate XIP →
NULL), `orb_sched_l4_active()` answers "is there a source", the `:784` guard and `:787`
call test/pass it, and the boot line at §4.3 prints the decision. The four mark sites
(`clock_view.c:509/769/1203/1631`) get **no edit** — they already branch on the
predicate. Do not touch `bake_clock.py`, `orbtheme-baker.js`, or `orb_clock3.c` here.

Decide and state in the commit body which AA end state you shipped (design §8 a or b);
(a) is the default. Update `tools/host_hand_aa_test.py` vectors and their mutants to
match the state you chose, and say in the test's comment why the truth table moved.

Evidence, same glass, same `--window 8:20`:
- Station: `render path: l4=yes src=xip` in the capture, and the row goes from
  `7.0 fps / 1,863 µs/kpx` to **15 fps or a stated reason it did not**.
- Sector and Skeleton: **unchanged** fps, boot line `src=psram`, and
  `gfx_parity_test.py` identical. A regression here means the selector is not a
  superset of the old gate — stop and fix before continuing.
- A still dial with a complication (moon/phases) and a date window: both refresh on
  rollover, no ghost, no flash. `fbgrab --diff` against a classic-path capture of the
  same instant proves the pixels.
- **A `sprite` caged dial** (carriage over a still plate — bake the Orbital preset and
  check the report says "plate rides the still"): this dial was paying LVGL for a full
  carriage box and now costs its swept arc. It is the largest free win in the design, so
  it is benched here and not deferred to slice 4. Watch specifically for a **ghost** — a
  vacated carriage arc that never restores, because the chord marks are the only thing
  standing between this dial and a visible rectangle.

### Slice 2 — `feat(sched): a ticking still dial is an ask, not a pass`

Design §6. `sched_fill_asks()` (`orb_sched.c:501`) gains the ticked-still `DISTINCT`
ask; `sched_seed_floor()`'s comment (`:558-564`) stops describing a still dial as
unschedulable, because that sentence is what this commit makes false. `host_sched_rung_test.py`
grows vectors: a ticked still dial at ask 4 now gets considered and demoted, and an
off-the-rung rate lands on the ladder honestly. Evidence: a ticked still dial over a
busy band reports its **asked** rate or a demotion, never an overrun.

### Slice 3 — `feat(bake): price the commit at bake time`

Design §5 and §7. `bakeClock3` returns `est_fps` from `box_kpx × RESTORE + ink_kpx × INK`
using the §5 table's coefficients; `splitPolicy3`'s `sentence` carries it; the Motion
block shows carrier class + estimate + the rung the pick will land on. Fix
`_devPace()`'s comment (`Watch Composer.dc.html:509-510`) and delete the carriage
exemption at `:526` so it gets a number. No firmware change in this commit. Evidence:
the bake report and the Motion line both print the estimate for Station, and
est-vs-measured lands inside ~15% on every dial benched so far. Refit the slopes only
if a point moves more than that, and say which point moved.

### Slice 4 — `feat(bake): carriage dials bake a delta plate`

Design §3. Delete `deltaBank = o.carriage ? 0 : 1` (`orbtheme-baker.js:2734`), delete
the matching refusal (`orb_clock3.c:513-516`), emit rect rows for the carriage case,
bump `GEOM_ABI4` 3→4 (`:1772`), and re-check D29's 4,128,768 B cap per dial (the blob
should shrink — keyframe planes are 434,312 B each). Evidence: a carriage dial holds
**12 fps**, flush time drops from §33.3's 227 ms, boot line says `geom=4`, and a
`geom=3` theme still refuses loudly rather than misrendering. This is the only slice
that can strand an installed theme; if the refusal is not loud, stop.

### Slice 5 — `feat(clock): hand AA by cost, not by carrier class`

Design §6 point 4 + §8. `hand_aa_for()` (`clock_view.c:264-274`) reads the §5 estimate's
headroom and the slice-1 AA decision instead of `bg_mode`. Close whatever tension slice
1 left open, in the direction slice 1's commit body recorded.

### After the last firmware commit

`cd tools/orb-manager && make firmware`, and commit the re-packed
`internal/firmware/bundle/orb-firmware.zip` so the hub never serves a firmware older
than the tree. Add the pointer lines to `docs/app-platform-plan.md`: §31 contract rule 2
is superseded by the design doc, §33's carriage row gains the before/after number.

## Stop and ask (owner gates — the only two)

1. **Slice 1, before flashing:** the AA decision. Nearest-everywhere hands on a dial the
   painter owns, or bilinear in the painter? It changes what the v1.1.37 ruling means on
   the glass and the owner made that ruling the same day this design was written.
2. **Slice 4, before flashing:** the ABI bump strands installed `geom=3` themes. Confirm
   the refusal path is what the owner wants for an already-installed dial.

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
