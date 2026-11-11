# rim-2: `ring_lanes()` — the annulus as 60 even-aligned rectangles

> **Read first: `docs/specs/rim-design.md` "Orchestrator amendments".** Verify ends with `make check-sandbox`.

Part 2 of `docs/specs/rim-design.md` (read §4 and §9 first). Pure geometry only: this step adds the
lane cover and its row filter to the IDF-free core, plus the host suite family that pins what they
cost. It touches no display glue, no clock and no constants outside the four it names.

## Goal

`ring_lanes()` answers the band's cover — the rectangles a ring-only refresh must send — with the
shipped numbers of rim-design §4.3 (60 lanes, 19,912 px sent, 9,908 extra, widest lane 944 px), and
`ring_lanes_in()` filters that cover to a row range for a fraction sweep. The suite proves coverage,
alignment, order, the no-double-cover rule and the budget, and its mutants bite on each claim.

## Read first

`docs/specs/rim-design.md` §4 (the set, the greedy, the numbers, the A/B table), §7 (where the cover
lives) and §9 (the gate) · `main/display/orb_ring.c` (`R_IN`, `R_OUT`, the centre, `ring_row_parts`,
`p_hit`/`e_first`/`e_last`, and the file's existing no-float-at-the-radius-boundary discipline) ·
`main/display/orb_ring.h` (the record, `ORB_RING_PARTS_MAX`, the constants block) ·
`tools/host_ring_test.py` (the reference model, the coverage checks, the mutant table, the
`RESULT: pass` / `mutants caught N/N` protocol) · `main/display/display.c`'s `DB_EVEN_BANDS` even-row
rule (read it, do not edit it) · `docs/specs/ring-design.md` §5 for how a pure core function is
documented in this repo.

## Contract

1. **`main/display/orb_ring.h`** — exactly the four constants and the two declarations of rim-design
   §4.2: `orb_ring_lane_t` (`int16_t x0, y0, x1, y1`, closed), `ORB_RING_LANES_MAX 64`,
   `ORB_RING_LANE_ROWS_MAX 64`, `ORB_RING_LANE_EXTRA_MAX 384`, `ORB_RING_LANE_MAX_PX 1024`,
   `int ring_lanes(orb_ring_lane_t *lanes, int max)` and
   `int ring_lanes_in(const orb_ring_lane_t *lanes, int n, int y0, int y1, orb_ring_lane_t *out,
   int max)`, each with a comment saying what it returns and that it is pure and deterministic. The
   record and every existing declaration stay as rim-1 left them.
2. **`main/display/orb_ring.c`** — the greedy of §4.2, top to bottom over rows 2..463, bands of even
   height with an even first row, at most `ORB_RING_LANE_ROWS_MAX` rows, accepted while the band's
   aligned area minus the annulus pixels it carries is ≤ `ORB_RING_LANE_EXTRA_MAX`, and always
   accepting a 2-row band when nothing larger fits (so the walk cannot stall and the cover is always
   total). Each side row contributes its left and right span; a cap or mixed band contributes one
   span lane; each lane's `x0` is `&= ~1` with its width rounded up to even, then clipped to the
   glass. Rows are emitted in increasing order. The annulus test reuses the shipped integer rule
   (four-times-squared against `R_IN`/`R_OUT`) — no second source of geometry truth, and
   `ring_row_parts`/`p_hit` are not edited. The file stays IDF-free and float-free in the pixel
   arithmetic; the only floats allowed are the ones `ring_row_parts` already uses.
3. **No callers yet.** This step adds no `EXT_RAM_BSS_ATTR` static, no call from `orb_ring.c`'s IDF
   glue and nothing to `display.c` — rim-3 wires the cover in. `ring_lanes()` must be safe to call
   twice with the same output array and must not allocate.
4. **`tools/host_ring_test.py`** — a lane section, in the file's existing style, that computes the
   annulus independently (the per-pixel inside test the suite already carries as its reference) and
   then checks: (a) the count is > 0 and ≤ `ORB_RING_LANES_MAX`; (b) every lane has even `x0`, even
   width, even `y0` and an even row count, and lies inside the glass and inside rows 2..463; (c)
   every lane is ≤ `ORB_RING_LANE_MAX_PX` pixels; (d) the lanes cover every annulus pixel, and every
   pixel they claim outside the annulus is counted as slack rather than as ring; (e) no annulus
   pixel is claimed by two lanes; (f) the lanes are in increasing row order; (g) the totals are the
   shipped numbers — 60 lanes, 19,912 sent, 9,908 extra; (h) `ring_lanes(NULL, 0)` and a `max` too
   small both return 0 without writing; (i) `ring_lanes_in` over the full range returns the full
   cover in order, over rows 100..140 returns 6 lanes and 1,856 px, over an out-of-band range
   returns 0, and with `y1 < y0` returns 0.
5. **Mutants** (extending the table, not replacing it): the 2-row fallback deleted (the cover stops
   being total), the x-alignment dropped (an odd window reaches the panel), the row-count evening
   dropped, the extra bound ignored by the greedy (the totals move), the band height allowed to go
   odd, the emitted order reversed, `ring_lanes_in`'s overlap test off by one row, and the
   widest-lane bound broken by merging the shoulder bands. Every mutant is caught and the reported
   count is ≥ the base commit's.

## Behaviour and edge cases

- Determinism is the property the slice leans on: two calls in one process, and a call after any
  other core function, return the same array.
- The cover is independent of the record: fraction, start, direction, colours and alpha change no
  lane, because the track paints the rest of the annulus and an alpha-0 lane still restores glass.
- The greedy's first and last bands are the caps, `x 174..291` at rows 2..9 and 456..463; the largest
  lanes are the two shoulder bands where the annulus turns from one span into two, and those are what
  the widest-lane bound exists for.
- Re-tuning `ORB_RING_LANE_EXTRA_MAX` (§4.4's A/B) must need no code change: the bounds the suite
  checks are derived (sent ≤ annulus + lanes × bound, each lane ≤ `ORB_RING_LANE_MAX_PX`) with the
  shipped exact numbers asserted separately, so a re-tune is one constant plus one expectation line.
  If this step ships a different value it says so and quotes the new totals rather than quietly
  bending the assertions.
- A `max` exactly equal to the needed count returns the full cover; one less returns 0 — all or
  nothing, so a caller can never send half a ring.
- Empty and invalid arguments (`lanes == NULL`, `max <= 0`, `n <= 0`) answer 0 and write nothing.

## Acceptance criteria

1. `python3 tools/host_ring_test.py 2>&1 | tail -3 | grep -q 'RESULT: pass'` — exit 0.
2. `python3 tools/host_ring_test.py --mutations > /tmp/rim2m.log 2>&1; grep -qE 'mutants caught
   ([0-9]+)/\1' /tmp/rim2m.log && ! grep -q MISSED /tmp/rim2m.log` — exit 0, count ≥ rim-1's.
3. `gcc -std=c11 -Wall -Wextra -Werror -I main/display -c main/display/orb_ring.c -o /dev/null` —
   exit 0.
4. `ORB_CLANG=/usr/lib/llvm-21/bin/clang make check-apps` — green (the SDK is untouched, and this
   proves it).
5. Symbol gates so a stub or a stray comment cannot pass (values checked, padding tolerant):
   `grep -q 'ring_lanes(orb_ring_lane_t' main/display/orb_ring.c main/display/orb_ring.h`,
   `grep -q 'ring_lanes_in' main/display/orb_ring.c`,
   `grep -qE 'define ORB_RING_LANES_MAX[[:space:]]+64' main/display/orb_ring.h`,
   `grep -qE 'define ORB_RING_LANE_ROWS_MAX[[:space:]]+64' main/display/orb_ring.h`,
   `grep -qE 'define ORB_RING_LANE_EXTRA_MAX[[:space:]]+384' main/display/orb_ring.h`,
   `grep -qE 'define ORB_RING_LANE_MAX_PX[[:space:]]+1024' main/display/orb_ring.h`, and the suite
   asserts the shipped totals: `grep -q 19912 tools/host_ring_test.py && grep -q 9908
   tools/host_ring_test.py`. If the A/B ships a different `LANE_EXTRA_MAX`, this gate's `384` and the
   two totals move together with the implementation, as §4.4's note says.

6. Nothing outside the slice moved: `git diff --name-only` lists only `main/display/orb_ring.c`,
   `main/display/orb_ring.h`, `tools/host_ring_test.py` and `docs/specs/*`; and
   `test -z "$(git diff --name-only -- main/display/display.c main/display/display.h apps)"` — exit 0.
7. `make check-host` is not run (rim-design §9.7); its ring half is criteria 1-2.

## Out of scope

The 25 Hz grid, the period floors, `ring_alpha_moved`, `ring_next_step_ms`, every `display.c` /
`display.h` change, the `EXT_RAM_BSS_ATTR` cover static, the pending-set index, `/status` counters,
a call from `orb_ring_disp_init()`, the pomodoro, and the SDK. rim-3 owns all of those.

## Notes

§4.3's and §4.4's numbers came from a standalone model of this exact greedy run over the shipped
`orb_ring.c` annulus test (`$DELEGATE_EVIDENCE_DIR/model/lanes8.py`, `lanesA.py`); from this commit
the suite is what keeps the firmware honest, and a disagreement between model and suite is a rim-2
bug, not a number to re-tune. If the coverage check (d) forces a change to the greedy, rim-design
§4.3's table is corrected in the same commit — the design's numbers are a claim about this function.
