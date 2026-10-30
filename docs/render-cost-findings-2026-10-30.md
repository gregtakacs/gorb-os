# Why Station renders at ~7 fps — measured 2026-10-30, orb-A

Question put to the bench: Station is a simple design (three hands, no
complications, no tourbillon) and draws at 7 fps while the Aviator GIF dials hold
12-15 fps. The answer is a phase decomposition, and it retires the x-clipping
idea with numbers rather than opinion. Reproduce with `tools/bench/flush_breakdown.py`
and `tools/bench/ab_xclip.py`; the dial's own `clock`/`phases`/`render` report
lines are the only instrument needed.

**Verdict up front (§2, §3).** X-clipping: implemented, −29.6% bytes on the wire,
**zero fps** — shipped off. The canvas-restore lever (bypass LVGL for the swept
box, as the GIF dials already do): **worth doing**, and the prize is bigger than it
first looks — LVGL's rotated hand transform is 70% of render and the plate only 30%,
and the same dial holds a measured 15.0 fps with the transform removed. Killing the
hands' AA is not that lever: measured, it buys ~0 fps.

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

## 3. The plate share, measured — and the verdict on the canvas-restore lever

The open question before this section: an L4-style restore replaces only the
**background re-composite**, so the prize is the plate's share of the 2,170
µs/kpx, not the whole 90%. Two build arms separate it, both read over the same
device-time window (8-20 s) and each proven by its own boot line.

**Arm "ink hidden" (plate share).** Every hand, shadow, aux and carriage layer
takes `LV_OBJ_FLAG_HIDDEN` at creation. The tick's swept-union invalidation is
driven off theme geometry (`clock_view.c:507-512`), so it still fires on the same
box and LVGL still has to fill it — with no rotated texel to sample and nothing to
blend. Whatever is left over is the plate share.

**Arm "AA off" (the cheaper rival).** `hand_aa_for()` returns false for every
layer. One line, at the seam the AA work already provides. If the 4-tap filter
were the cost, this would show it.

| arm | fps | render ms/s | render kpx/s | µs/kpx |
|---|---|---|---|---|
| shipped (AA on hour/minute) | 7.0 | 870 | 482 | 1,863 |
| AA off, all hands nearest | 7.5 | 848 | 503 | 1,724 |
| ink hidden, plate only | **15.0** | 640 | 1,047 | **555** |

Three conclusions follow.

**The ink, not the plate, owns the render.** Hidden-ink costs 555 of 1,863
µs/kpx, so the plate re-composite is **30%** and LVGL's hand transform is **70%**.
The "hands fill a few percent of their box, so the box is mostly plate" arithmetic
describes pixels, not cost: the ink's 4 PSRAM fetches and blend per pixel dominate
the few that the plate copies.

**Filtering is not the lever.** Turning AA off moves fps 7.0 → 7.5 (within the
noise band) and unit cost only 1,863 → 1,724 µs/kpx. Ablating the AA the dial now
ships would buy about nothing, and it is a visual loss. This closes the cheaper
option: the cost is LVGL's compositing of a rotated layer, which nearest sampling
still pays.

**15 fps is reachable on this exact dial, today.** The hidden-ink arm reports
`clock 15 fps | flush frames 15`, i.e. 15 real committed frames a second, at 15
flush frames — not a ceiling read. It clears it while drawing 2.2× the pixels,
because DMA/TE then gate instead of render. Station's 7 fps is therefore not a
panel limit and not a bandwidth limit; it is the hand transform, and the ceiling is
twice as far away as the shipped build reaches.

**Verdict: worth doing, at roughly the full 70%, not the 30% a naive reading
allows.** A canvas restore that bypasses LVGL for the swept box does not merely
replace the plate blit — it also takes over the ink, which the L4 painter already
composites at a measured 185-232 ns/px in internal RAM. The two costs it pays
instead are known: a restore at the mirror's own memcpy rate (measured in
`display.c` at ~22 µs/kpx) plus blends on ink pixels only, and the ink is a few
percent of the box. That projects to tens of ms per second against 870 today,
which is why the hidden-ink arm behaves as an achievable target rather than a
horizon.

**Both blockers are now closed.** Funding: the probe printed `psram free 7744 kB,
largest block 7680 kB` where a full RGB565 canvas needs 424 kB, and for a still
plate no canvas is needed at all — the restore source can be the resident baked
plate (`blob() + plate_off`), which is what LVGL reads today. The `bg_mode == 1`
canvas gate at `clock_view.c:2191` exists because a GIF's background *changes*;
Station's does not. Fidelity: the painter is nearest-only (`orb_gfx.c:366-380`
samples one texel), so it would have dropped the hour/minute AA — but the AA-off
arm shows that AA is worth ~0 fps, so the honest reading is the reverse: adding a
4-tap path to the painter is affordable, and the dial does not have to choose
between its tempo and its current AA. The shadow stack is already handled there
(MULTIPLY at `orb_gfx.c:242`, per-role blend counters), and row-major order is
caller-supplied, so the `S2 H2 S1 H1 S0 H0` stack is expressible.

**What to build.** One gate change to let the direct-push path serve `bg_mode 0`,
the clock tick's marks replacing the `lv_obj_invalidate_area()` of the swept union,
and a restore source for a still plate. Then read `render ms/s` on the same
window: it should fall from 870 toward the hidden-ink arm's 640 while fps climbs
toward 15, and the ink must stay on the glass. Risk that the spike exists to
retire: XIP throughput. The restore reads the plate from the flash-mapped assets
partition every frame instead of letting LVGL do it, and that rate is not yet
measured for a swept box — the mirror's memcpy rate is the floor, not the answer.


**Recipe for the two arms.** They are not in the tree, because `clock_view.c`
carries uncommitted work and a measurement probe should not sit in it between
sessions. Both are small and each needs its own boot line, per §5. Ink-hidden:
after the `aux_create()` call in `clock_face_layout`, add `LV_OBJ_FLAG_HIDDEN` to
`s_hand[0..4]`, `s_shadow[0..4]`, `s_car` and `s_aux_obj[0..n]`, and log the arm.
AA-off: make `hand_aa_for()` `return false` at its first line, and log the arm —
the existing `hand AA:` line will keep reporting the shipped rule, because it is
derived from theme fields. Then, for each arm: build, `tools/serflash.sh`, and
`curl 'http://<ip>/api/v1/screen?to=clock'` before capturing, or the dial is not
foreground and prints nothing. `tools/bench/render_arms.py` turns captures into the
table above and refuses a capture whose arm line is missing.

## 4. Corrections to the record

**"15 fps" is a real delivered rate.** `ORB_SCHED_FPS_CEILING` is 15
(`orb_sched.h:189`) and the ladder tops out there. An earlier revision of this file
claimed the maximum was 12 and cited `orb_sched.c:49-52`, which is the `CLK_BG_MAX`
comment — wrong line, wrong number. The hidden-ink arm of §3 settles it on the
glass: `clock 15 fps | flush frames 15`, fifteen committed frames behind fifteen
flushes. The `flush frames` field is what disambiguates a ceiling read from a
delivered rate; read it.

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

Several are mine, and one corrects a claim this file used to make. An earlier
revision blamed missing bench lines on boot-page timing. That was wrong: the
report is neither silent nor timing-sensitive.

- **After a reflash the board boots to the menu, and the menu being foreground
  stops the clock's tick — so no `clock … fps` line is ever printed.** The report
  lives in the scheduler's tick, so it runs only while the dial is on the glass.
  Put it there before capturing:
  `curl 'http://<ip>/api/v1/screen?to=clock'` (bench-only route, gated by
  `CONFIG_ORB_DEBUG_HTTP`). This single fact cost most of this session.
- **`tools/capf.py` takes `<secs> <out>`, in that order.** Passing the path first
  makes `float()` throw and the capture dies without writing a file, which is
  indistinguishable from a silent board.
- **`timeout N capf.py N` kills the capture before it flushes.** Leave the timeout
  ~10 s longer than the capture or an empty file lands where the data should be.
- **The report starts only after SNTP lands, t≈10 s cold.** Capture 20 s minimum:
  12 s of log contains every phase except the one wanted.
- **There is no `idf6.sh` in this workspace.** Build with
  `. /home/greg/esp/esp-idf/export.sh && idf.py build`. A `Project build complete`
  line read from a stale log is not evidence — `tools/serflash.sh`'s stamp audit
  caught exactly that here, refusing an image stamped `feb320e` after HEAD moved to
  `be0f7c1`, which is how the phantom build became visible.
- **A probe that changes an observable must print the change.** The shipped
  `hand AA:` line is derived from the theme's motion words, so under the AA-off arm
  it cheerfully reported the shipped rule. `commit: cols …` and the `ABLATE …`
  lines are what let each arm be proven from its own capture.
- **`tools/capf.py` resets the board when it opens the port.** Every capture
  therefore starts at device t≈0 and carries its own boot header. Use that: it is
  what makes same-window arm comparison possible at all.
- **A capture can be truncated by the hub and still exit 0.** `orb-manager`
  shares the port and periodically wins it. Gate a capture on its **sample
  count**, never on exit status. Do not kill the hub to win a bench run.
- **A flash refused with `[Errno 16]` leaves the PREVIOUS image on the board.**
  Capturing after that attributes data to the wrong arm.
- **`pkill -f <pattern>` matches this tool's own shell**, whose command line
  contains the pattern text, and kills the call that ran it — demonstrated on
  myself. Take the PID from `pgrep` and kill that.
- **Background jobs need the parenthesized form**
  `( setsid nohup … </dev/null >/dev/null 2>&1 & )`. A bare `setsid … & disown`
  died at the call boundary and left no trace of ever having run.
- **Each `run_commands` element is its own shell**: a `cd` in one does not carry to
  the next. Prefix every element, or use absolute paths.
- **Compile-time A/B arms share one version stamp** (same commit, same
  `version.txt`, `-dirty` either way), so the flasher's stamp audit cannot tell
  them apart. The boot line is the only proof of what is on the glass.

a build billed by the byte, not a faster dial.
