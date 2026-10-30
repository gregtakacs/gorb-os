# Why Station renders at ~7 fps — measured 2026-10-30, orb-A

Question put to the bench: Station is a simple design (three hands, no
complications, no tourbillon) and draws at 7 fps while the Aviator GIF dials hold
12-15 fps. The answer is a phase decomposition, and it retires the x-clipping
idea with numbers rather than opinion. Reproduce with `tools/bench/flush_breakdown.py`
and `tools/bench/ab_xclip.py`; the dial's own `clock`/`phases`/`render` report
lines are the only instrument needed.

## 1. The frame is ~90% LVGL render

A typical Station second while live (orb-A, card mounted, sweep `on(15 bps)`):

```
clock 12 fps | flush frames 7 avg 75 ms | px/s 1123992 (517% page) chunks/s 126 te/s 92 ms submit/s 277 ms
  phases ms/s: mirror 23 te 54 dma 78 | hook 0 (rest 0 paint 0 cmp 0) | blend/s 0
  render ms/s 899 at 414 kpx/s = 2170 us/kpx
```

| Phase | ms per second of wall clock |
|---|---|
| LVGL render (outside `flush_cb`) | **899 — ~90%** |
| band submit (`dma_dirty_bands`) | 78-98 |
| TE wait | 54-92 |
| chunk mirror | 20-23 |

Two facts settle the diagnosis. `hook 0` and `blend/s 0` prove the L4 painter
never runs on this dial — `orb_sched_l4_active()` requires
`orb_bg_gif_mode()` (`orb_sched.c:191`), and Station is `bg_mode 0`, `gif_frames
0`. So the entire cost is LVGL's own re-render of the damage rectangle. And
2,170 us/kpx is **~520 cycles per pixel at 240 MHz** — 80× dearer than the
mirror's own byte-swap copy rate (~6 cyc/px). A straight plate blit cannot cost
that. The money goes to the rotated, AA-sampled hand and its shadow: four PSRAM
texel fetches per output pixel, then an RGB565A8 blend, doubled by the shadow
layer.

`clock_view.c:392` already documents the mechanism for a sweeping dial: re-posing
a hand invalidates its **full rotated box** for both the old and the new pose.
The code works around it as far as LVGL's model allows —
`lv_display_enable_invalidation(false)` around the setters, then one
`lv_obj_invalidate_area()` of the swept union (`clock_view.c:507-512`) — and the
0.2-degree gate (`HAND_REDRAW_EPS_10`) stops the hour and minute re-posing every
tick. The invalidation is therefore already tight. What remains expensive is that
LVGL must *fill* that rectangle, and filling it re-composites every layer inside
it, plate included, at ~520 cyc/px.


## 2. X-clipping the commit: implemented, measured, declined for tempo

Station's bands left the wire at 61.9% panel coverage (284.6 dirty rows = 134,312
px of a 217,156 px page, modelled by `tools/bench/xclip_model.py` over real
mirror frames). The commit path billed columns 0..465 regardless. `DB_XCLIP`
(`display.c:249`) makes each tile bill the union of its dirty rows' real extents,
taken from the `lv_area_t` LVGL already hands `lvgl_flush_cb` — a superset of the
changed pixels by construction, never a guess. Rows carrying no extent information
(a clock mark, a forced row, the whole-page redraw) keep full width, and the
window snaps even outward to match the existing `DB_EVEN_BANDS` rule, so the
stale-hairline race stays structurally impossible: a wider window can only ever
send extra correct pixels, because `s_fb` is complete before the first submit.

Paired bench, arms proven from each capture's own boot line, both read over the
same 8-19 s window of device time:

| | OFF | ON | delta |
|---|---|---|---|
| px/frame | 95,714 | 67,366 | **−29.6%** |
| `dma` ms/s | 98 | 78 | −20.5% |
| `te` ms/s | 72 | 68 | −5.2% |
| fps | 7.82 | 7.45 | **flat** (both arms [6-11], n=33) |

The byte cut is real and large; the tempo does not move, because DMA is ~10% of
the frame and render is ~90%. It cannot be rescued by funding the tile ring
either: the boot card mount steals the ring and the return funds **one** buffer
(`tile ring re-funded short: 1 of 2 buffers, flushes serialize`, t≈7.2 s), which
is why the narrower per-row copies sit on the critical path with no DMA to
overlap. A depth-2 ring moves DMA off the path entirely — and with it the last
reason this could ever pay in fps. **Shipped with `DB_XCLIP 0`, which is
codegen-identical to the pre-existing commit.** Its future is a cardless dial or

## 3. What the next lever is, priced

To move Station's tempo, stop asking LVGL to re-composite the swept rectangle.
The mechanism exists and is proven on this board: the L4 direct-push path
(`orb_sched.c:244-289`, used by every GIF dial) restores the background for the
marked rows from `s_canvas`, paints the layers over it, and marks only per-row
extents — `gfx_mark_rows(y0, y1, x0, x1, force)` already carries sub-row x extents
that the display layer currently discards. `orb_sched.c:221` bills the plate
re-blit it avoids at "~250-300 cycles each". Enabling it for a still plate means
seeding the canvas from the baked plate rather than a GIF bank, and relaxing the
`orb_bg_gif_mode()` gate. That is the only change that attacks the 90%: it
replaces "re-blit everything inside a rectangle" with "restore the extent, blend
the ink". It also makes the byte picture honest for free, because the L4 path
already knows the extents the x-clip work had to infer.

## 4. Corrections to the record

**"15 fps" was never a delivered rate.** `ORB_SCHED_FPS_CEILING` is a ceiling
constant, and the rung ladder's maximum is 12 (`orb_sched.c:49-52`). Bench lines
reading `clock 15 fps` read the ceiling, not delivered frames.

**A single-second read is not a baseline.** The report is per-second and its
variance is large: one capture read `4 fps / avg 135 ms` at t=1.8 s and
`12 fps / avg 75 ms` at t=2.9 s, and a continuous run decayed from 12 fps at
t=11 s to 6 fps at t=74 s while its per-frame billing grew from ~22k to ~98k px.
Compare arms over the same window of device time, or compare nothing.

**The sweep is on.** The live Station dial reports `sweep on(15 bps, base 66 ms)`,
so the seconds hand re-poses ~15×/s. The shipped AA rule keeps that layer on
nearest sampling precisely so the hot tick never also pays the 4-tap filter
(`clock_view.c:268-277`). A Station with the sweep off is a different machine and
was not measured here.

## 5. Measurement traps, so the next session does not pay for them

- **`tools/capf.py` resets the board when it opens the port.** Every capture
  therefore starts at device t≈0 and carries its own boot header. Use that: it is
  what makes same-window arm comparison possible at all.
- **A capture can be truncated by the hub and still exit 0.** `orb-manager`
  shares the port and periodically wins it; the symptom is a file with boot lines
  and no clock samples. Gate a capture on its **sample count**, never on exit
  status. Do not kill the hub to win a bench run.
- **A flash refused with `[Errno 16]` leaves the PREVIOUS image on the board.**
  Capturing after that attributes data to the wrong arm — which is how one run
  produced a second OFF capture labelled ON. Give each arm a boot line naming its
  own configuration and read it back before trusting the file.
- **`pkill -f <pattern>` matches this tool's own shell**, whose command line
  contains the pattern text, and kills the call that ran it. Take the PID from
  `pgrep` and kill that.
- **`grep -r` here intermittently returns nothing for patterns containing `/`,
  `|`, or `§`.** Re-run with `grep -F`, or read the file. Never conclude an
  absent symbol from one empty grep.
- **Compile-time A/B arms share a version stamp** (same commit, same
  `version.txt`, `-dirty` either way), so `serflash.sh`'s stamp audit cannot tell
  them apart. The boot line is the only proof of what is on the glass.
- **Each `run_commands` array element is its own shell**: a `cd` in one does not
  carry to the next. Prefix every element, or use absolute paths.

a build billed by the byte, not a faster dial.
