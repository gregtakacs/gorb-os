# rim-3: the ring's own refresh — lanes into the commit, and a 25 Hz clock

> **Read first: `docs/specs/rim-design.md` "Orchestrator amendments".** Verify ends with `make check-sandbox` (the firmware builds in the sandbox now); the pomodoro's paused breath is 2500 ms; its ring send threshold is 2 permille.

Part 3 of `docs/specs/rim-design.md` (read §5, §6, §7 and §9 first). This is the glue: the lane
batch rides the flush's own commit under `s_ring_mux` in its own tiles, and the effect clock that
drives it moves from a 2 Hz grid to a governed 25 Hz one. rim-2's `ring_lanes()` is the geometry
this step calls; nothing here re-decides it.

## Goal

A breathing ring costs 19,912 px in 60 windows instead of 215,758 px of whole rows, at up to 25 Hz,
with zero new internal RAM; a drain step costs only the lanes in the rows it swept; the copy arc
still wins the rim; the untiled and short-tile cases degrade to today's exact behaviour; and every
pure piece of the decision (`ring_alpha_moved`, `ring_next_step_ms`, the clamps, the new floors) is
host-tested before any of it is flashed.

## Read first

`docs/specs/rim-design.md` §5/§6/§7/§9/§11 · `main/display/display.c` (`s_fb`, `s_dirty_x0`/
`s_dirty_x1` and `mark_dirty_row`, `display_mark_rows`, `DB_EVEN_BANDS`, `dma_dirty_bands`/
`dma_dirty_bands_locked` and its zero-band shortcut, `s_tile[]`/`s_tile_done`/`s_tiles_n`,
`s_ring_mux`, `s_ring_parked` and the loan, `present_wait`/`wait_for_te`/`s_te_live`, `s_bench_pix`/
`s_bench_chunks`, `s_last_pending` and `on_color_trans_done`, `band_commit_here`, `fx_frame`, the two
rim-dispatch sites) · `main/display/orb_ring.c`'s IDF half (`orb_ring_disp_init`,
`orb_ring_disp_service`, `orb_ring_disp_stamp`, `ring_rows_for`, `mark_span`, `s_alpha`, the
ownership table, the `EXT_RAM_BSS_ATTR` idiom already used there) · `main/display/orb_edge_arc.c`
(the copy-arc-wins precedent, and the off-glass invalidate that carries a frame on a still page) ·
`main/display/display.h` · `tools/host_ring_test.py` · `docs/specs/ring-design.md` §6/§8.

## Contract

1. **`main/display/orb_ring.h`** — the constants of rim-design §6.1: `ORB_RING_STEP_MS` 500→**40**,
   `ORB_RING_BREATHE_MIN_MS` 4000→**1500**, `ORB_RING_FLASH_MIN_MS` 1000→**400**,
   `ORB_RING_STEP_PCT` 5→**2**, and the new `ORB_RING_STEP_ALPHA 8` and `ORB_RING_STEP_MAX_MS 400`;
   `ORB_RING_DEFAULT_PERIOD_MS` stays 4000. Declare the two pure helpers:
   `int ring_alpha_moved(int was_alpha, int now_alpha)` and `int ring_next_step_ms(int batch_us)`.
2. **`main/display/orb_ring.c`, pure half** — both helpers exactly as §6.1/§6.3 specify: the gate
   fires on |Δα| ≥ `ORB_RING_STEP_ALPHA` or when either side is 0 (going out and coming back are
   never gated); the governor returns
   `clamp(max(ORB_RING_STEP_MS, (batch_us*3)/1000), ORB_RING_STEP_MS, ORB_RING_STEP_MAX_MS)`, so a
   batch ≤ ~13 ms keeps 40 ms and a 130 ms batch lands on 400 ms; a negative `batch_us` counts as 0.
   `ring_clamp` honours the lowered floors and keeps its idempotence.
3. **`main/display/orb_ring.c`, IDF half** — at `orb_ring_disp_init()`, build the cover once into a
   `static EXT_RAM_BSS_ATTR orb_ring_lane_t s_lanes[ORB_RING_LANES_MAX]` plus `s_lane_n`; if
   `ring_lanes()` answers 0 (or `max` was short) log once, set `s_lane_n = 0`, which pins the ring to
   the old path. Keep the last alpha *sent* for the gate, the pending set as a first/last lane index
   pair (the whole cover, or `ring_lanes_in()` for a sweep), and a `s_lanes_due` flag. Then, per
   §11's table, `orb_ring_disp_service()` does nothing while `orb_edge_arc_shown()`; when
   `display_ring_lanes_ready()` and `s_lane_n` it records the pending set and invalidates the same
   off-glass pixel the arc uses instead of marking rows; otherwise it calls `display_mark_rows(2,
   464)` exactly as today. It applies `ring_next_step_ms()` to the grid after each batch from the µs
   the display layer reports, and it never advances an effect phase inside the tile callback
   (ring-design's one-clock-read rule stands).
4. **`main/display/display.h` / `display.c`** — `bool display_ring_lanes_ready(void)`
   (`s_fb && s_tiles_n == 2 && !s_ring_parked` and the mirror-up condition the tile path already
   uses); `int display_ring_lanes(const orb_ring_lane_t *lanes, int n)`, LVGL-task only, taking
   `s_ring_mux`, calling `send_ring_lanes_locked()` with an empty band list and returning the windows
   submitted, with the batch's wall time available from `display_ring_last_batch_us()`; and
   `static int send_ring_lanes_locked(...)` per §5.1/§5.2: mirror rows `memcpy`'d into the current
   tile with stride = lane width, `orb_ring_disp_stamp()` into it,
   `esp_lcd_panel_draw_bitmap(x0, y0, x0 + w, y1 + 1, tile)`, tile ping-pong under the existing
   `s_tile_done` write-guard, `s_band_inflight` balanced, `s_last_pending` set, bench counters
   bumped, lanes whose rows the caller's bands already cover skipped, submission in row order, all of
   it inside the blanking window the flush just waited for. `dma_dirty_bands_locked()` calls it after
   its band loop when the ring is due, and its zero-band shortcut must not fire while lanes are on
   the wire — it returns the lane count instead of 0.
5. **The two rim-dispatch sites and `fx_frame`** — the carousel keeps stamping the ring into the
   slabs it composes and then asks for lanes for whatever its frame did not touch (usually nothing, so
   0 windows). The direct-draw path keeps marking its own rows, the ring's lanes ride after it, and the
   §5.3 skip rule does the merge. Both sites keep using this step's `s_alpha`, unchanged.
6. **One log line** — an `ESP_LOGI` at the ring's tag naming the lane count, the batch's µs and the
   new grid, on the first batch and whenever the grid changes. No new endpoints, no `/status` fields,
   no hub change (rim-design's last owner question).
7. **Tests** — extend `tools/host_ring_test.py` with `ring_alpha_moved` cases (0→255, 255→0, 100→104
   gated off, 100→108 gated on, both 0, both 255) and `ring_next_step_ms` cases (0→40, 5000→40,
   13000→40, 30000→90, 90000→270, 200000→400, −7→40), plus mutants for both (the gate's comparison
   inverted, the 3× factor dropped, the ceiling dropped). Retarget every existing expectation that
   named the old constants (STEP 500, breathe floor 4000, flash floor 1000, STEP_PCT 5) to the new
   numbers — retargeted, never deleted — and leave the pomodoro suite's `PM_*` expectations alone:
   the app still asks for 4000/2000 and 4 ‰, which the lowered OS floors now let it have verbatim.

## Behaviour and edge cases

- **No new internal RAM**: zero new internal statics, zero new internal DMA allocations (§7). The
  step's summary states it and the firmware-only list repeats it for the bench.
- **Tiles parked / one tile / no mirror**: `display_ring_lanes_ready()` is false, the service takes
  the `display_mark_rows()` path, one log line says so, behaviour equals the base build.
- **`ring_lanes()` answered 0** (`s_lane_n == 0`): the same old path, permanently, with the one log
  line at init — a geometry failure must cost the ring its shape, not the ring.
- **Copy arc up**: the ring sends nothing and its phase does not advance; on the copy's teardown the
  ring reappears at the next step with no work for the app.
- **A still page**: the off-glass invalidate is what carries a lane frame, so a ring that only
  changes colours on a paused dial must still move — the steppy-breathe regression is what this
  slice exists to fix.
- **Gate at both ends**: an effect fading to 0 sends its last frame and its first frame back; the
  suite pins both.
- **Governor starvation**: an absurd batch time (or garbage from a failed measurement) can only push
  the grid to 400 ms, i.e. the base build's cadence, and the ring does not jump back to 25 Hz in the
  step it was throttled.
- **Alignment**: `send_ring_lanes_locked()` skips (and counts) any lane that is not even in x and row
  rather than submit an illegal window, so a geometry bug costs pixels, not a corrupt line.
- **Bus contention**: the one `s_ring_mux`; neither entry point may be called off the LVGL task, and
  both say so in the header.
- **OTA / idle dim / lock screen**: the lane path sits behind the same quiet-window helpers the tile
  flush already uses, so a locked panel sees no lane traffic.

## Acceptance criteria

1. `python3 tools/host_ring_test.py 2>&1 | tail -3 | grep -q 'RESULT: pass'` — exit 0.
2. `python3 tools/host_ring_test.py --mutations > /tmp/rim3m.log 2>&1; grep -qE 'mutants caught
   ([0-9]+)/\1' /tmp/rim3m.log && ! grep -q MISSED /tmp/rim3m.log` — exit 0, count ≥ rim-2's.
3. `python3 tools/host_pomodoro_test.py --mutations > /tmp/rim3p.log 2>&1; grep -qE 'mutants caught
   ([0-9]+)/\1' /tmp/rim3p.log && ! grep -q MISSED /tmp/rim3p.log` — exit 0, count ≥ 54 (the app is
   untouched and must stay that way).
4. `gcc -std=c11 -Wall -Wextra -Werror -I main/display -c main/display/orb_ring.c -o /dev/null` —
   exit 0, which also proves the two new helpers live in the IDF-free half.
5. `ORB_CLANG=/usr/lib/llvm-21/bin/clang make check-apps` — green.
6. Symbol gates, column-padding tolerant (`grep -qE`, because this header aligns its values):
   `grep -qE 'define ORB_RING_STEP_MS[[:space:]]+40' main/display/orb_ring.h`,
   `grep -qE 'define ORB_RING_BREATHE_MIN_MS[[:space:]]+1500' main/display/orb_ring.h`,
   `grep -qE 'define ORB_RING_FLASH_MIN_MS[[:space:]]+400' main/display/orb_ring.h`,
   `grep -qE 'define ORB_RING_STEP_ALPHA[[:space:]]+8' main/display/orb_ring.h`,
   `grep -qE 'define ORB_RING_STEP_MAX_MS[[:space:]]+400' main/display/orb_ring.h`,
   `grep -q 'ring_alpha_moved' main/display/orb_ring.c`,
   `grep -q 'ring_next_step_ms' main/display/orb_ring.c`,
   `grep -q 'send_ring_lanes_locked' main/display/display.c`,
   `grep -q 'display_ring_lanes' main/display/display.h main/display/orb_ring.c`,
   `grep -q 'orb_ring_disp_stamp' main/display/display.c` (the blend is reused, not re-implemented)
   and `! grep -q 'ring_blend565' main/display/display.c` (display.c must not grow its own blend).
7. No new product file and no build-list edit: `git status --porcelain` shows no `??` outside
   `docs/specs/`, and `git diff --name-only -- main/CMakeLists.txt` is empty.
8. `make check-host` is not run (rim-design §9.7). **Firmware-only, for the orchestrator's IDF build
   and Bronze bench**: `idf.py build` clean, and §9's bench list in full — the fps windows over radar
   and over flights, the `/health` px/s of a drain, the no-tearing-by-eye verdict on a breathing ring
   over the clock, `glass.tiles` == 2 through a card fill, internal `min_free`/`dma_min_free` low
   water no worse than the base build, `/fb` still blind to the ring, and §4.4's A/B at 384 and 1024.

## Out of scope

Any new `.c` file, any `main/CMakeLists.txt` edit, `/status` or hub changes, the copy arc's own
geometry, the carousel's composition, the theme cache, the SDK's `binds.json`, `apps/pomodoro/*`
(rim-1 owns it), the lane geometry itself (rim-2 owns it, and this step must not fork a second
greedy), and the `ORB_RING_LANE_EXTRA_MAX` re-tune — a bench decision recorded against §4.4, not a
code change here.

## Notes

Why the batch rides the flush's commit instead of a second submitter is §5.1; why it reuses the tile
ring rather than a dedicated 2 KB buffer or PSRAM-direct is §5.4 and the internal-RAM cliff it cites;
the arithmetic behind 25 Hz being affordable is §6.2, stated in the repo's own measured figures rather
than in µs nobody has published. The two risks the orchestrator should look hardest at on Bronze are
the per-window fixed cost (60 transactions where there were 24 — §4.4 exists for exactly that) and a
tear at the top edge, which is why lanes go out in scan order inside the window the flush waited for.
