# Agent handoff - gorb-os Skeleton theme, 4 open issues (2026-10-23)

You are picking up a long, corrupted session. Start fresh from this file. Read
AGENTS.md, .clinerules, and README.md in the repo root first. Nested project
rules win over any habit you carry in.

## Repo state right now
- Project root: /home/greg/docker/Projects/gorb-os. Scope your work to this folder.
- HEAD is d0c53d0. The working tree is clean.
- Three commits just landed on top of 635961a and are DONE (do not redo):
  - 13dbc77 fix(composer): bake verifier tolerates the delta's own dither
    (DELTA_TOL in orbtheme-baker.js, section 36 pins).
  - 8c47ff1 fix(composer): stop the second send a re-send arms (section 32.9
    exact/duplicate/clear verdict - this change is implicated in Issue 4 below).
  - d0c53d0 fix(hub): hub accepts the ABI4 delta dialect (v3 and v4).

## Environment (no login shell on this box)
- node is NOT on PATH. Use the VS Code bundled interpreter:
  /home/greg/.vscode-server/bin/6d62b1d5dace93a33b3a6d3eca1317bd5e15bcb1/node
  (reports v24.21.0). There is no selftest.js; run the suites directly.
- UI suites (both must stay green):
  <node> tools/watch-composer/_selftest/headless-bundle3.test.js  (190 checks)
  <node> tools/watch-composer/_selftest/headless-ui.test.js       (250 checks)
- Go tests: cd tools/orb-manager && go test ./...
- Hub: run the built binary tools/orb-manager/bin/orb-manager (rebuild after Go
  changes). make run is the lifecycle owner. To drive the upload gate headless,
  use -no-window (no browser-exit watchdog). The normal desktop run ties server
  life to the app window, so a detached relaunch without a persistent Chrome
  window self-terminates ("browser exited; shutting down").
- The theme-upload gate runs on BOTH precheck and real send (theme.go:171). Auth
  token rotates each launch; extract it from the Set-Cookie: orb_token=... header
  on GET /composer/index.html and send it back as X-Orb-Token.

## The four issues

### Issue 1 - progress bar flickers into a "double green bar" during SD memload
Symptom: during the store/memload SD-copy phase the black card is mostly solid,
but for a frame or two the progress area shows a second green bar. It settles.

Green = COL_ARC = RGB565(0,255,165), the boot overlay progress bar. The memload
"storing" frames come from orb_boot_overlay("storing", done, total, 0)
(main/art/orb_theme.c:873). Each frame paints THREE bands into ONE reused buffer
s_band (main/display/orb_boot_overlay.c:357-381), committed top-first at
LINE1_Y=184, LINE2_Y=208, BAR_Y=232 (orb_boot_overlay.c:145-157).

display_band_commit hands s_band straight to the QSPI GDMA via tx_color /
esp_lcd_panel_io_tx_color - NO copy
(.../esp_lcd_co5300/esp_lcd_co5300_spi.c:313). The DMA reads the buffer AFTER
draw_bitmap returns, asynchronously.

Prior fix d33b901 made display_band_commit block until its own transaction's
trans_done cb fires (retirement counter s_tile_done), killing the WITHIN-frame
restamp race (main/display/display.c:372-409). Two residuals remain, both from
that one shared buffer:
  (a) Carry-over after a STARVED commit. On a 500 ms retire timeout,
      display_band_commit returns ESP_ERR_TIMEOUT and deliberately does NOT
      decrement s_band_inflight (display.c:391-403) - the running DMA still owns
      s_band. The starved flag forces a redraw next tick, and that redraw calls
      band_text(line1, b) (orb_boot_overlay.c:358) which MUTATES s_band BEFORE
      the next commit's quiet-bus drain (display.c:382) protects it. A
      not-yet-retired band latches the new bytes -> stray bar/word.
  (b) Queue depth > 1. trans_queue_depth = s_tiles_n when the ring is live
      (display.c:1012), so three band commits of one frame sit in the SPI queue
      at once; a slow retire lets the next frame's first stamp overlap.

DECISIVE FIRST CHECK (ask operator / watch serial log): when the flicker happens
does the log print "band at rNNN did not retire within 500 ms" (display.c:401)?
  - YES -> residual (a). Fix: drain to a quiet bus BEFORE stamping s_band at the
    top of a frame (not only inside display_band_commit). On a starved/timeout
    commit, force the next frame to wait for the stale band to retire before any
    band_text/band_bar mutation.
  - NO -> residual (b). Fix: double-buffer the overlay band (stamp a second
    buffer and commit it) or run the overlay path at queue depth 1.

### Issue 2 - second hand still sweeps after switching Skeleton sweep -> "2/s"
Symptom: operator set the composer second-hand Motion to "2/s" (tick) but the
Skeleton theme still sweeps on the device.

This is BY DESIGN in the baker, not yet a bug - confirm before changing:
tools/watch-composer/orbtheme-baker.js:1524-1543. A delta-rect bank animates
EVERY tick, so a ticking hand strobes; the bake forces second_sweep=1 and
tick_rate=fps UNLESS the caller passes noSweep. orbtheme-baker.js:1538-1540
already emits: "...a delta-rect bank animates every tick, so the baked dial
sweeps (pass noSweep to keep the step)". The composer Motion dropdown
(Watch Composer.dc.html:3133, fed by c.tick) does NOT thread through to the
baker's noSweep for an animated/delta theme. The device honors second_sweep only
for non-delta hands (clock_view.c:288, 291-300).

Decide with the operator: either (1) plumb Motion=tick into bakeClock3 as noSweep
for delta themes and accept the strobe, or (2) keep sweep mandatory for delta
banks and make the UI say so. Do NOT silently change bake semantics - section 31's
motion split is deliberate.

### Issue 3 - full redraw every second stutters the frame rate (once per second)
Symptom: smooth at ~25 fps except a stutter once per second, as if a whole-frame
redraw fires each second. Operator wants frame 0 baked as a DELTA too so every
tick costs the same.

This is the main rendering-behavior bug to investigate. Look at:
- clock_view.c:2309-2325: on a per-second re-anchor the code sets s_tourb_shown=-1,
  calls lv_obj_invalidate(s_scr) (a FULL-screen invalidate), compose_now(), and
  clock_layers_refresh(). The full invalidate at :2323 is the prime suspect for
  the once-a-second plateau.
- The GIF/delta bank seeds ONE full keyframe then applies per-frame changed-region
  rects (clock_view.c:55-63). The keyframe is frame 0. Read orb_sched.c,
  orb_bg_show_plate_frame, orb_sched_reanchor_gif (clock_view.c:2303), and
  orb_bg_gif_mode(). Confirm whether the per-second boundary re-inates the WHOLE
  screen instead of only frame 0's delta rects.
- Operator's fix idea: bake frame 0's delta as well, so the every-second anchor
  applies a rect diff instead of a whole-screen invalidate. Verify whether the
  bake (orbtheme-baker.js) emits a delta for frame 0 or only a keyframe, then
  align the device per-second path to it.

Constraints (section 6.2 budget, section 32.5): one reused 24-row band buffer,
nothing heavy on the paint path; do not weld a dial with missing hands (see the
:2326-2330 note). Measure before/after with the fps bench -
docs/fps-budget-findings-2026-10-10.md and the fps-*.csv files name the harness.

### Issue 4 - re-uploading the SAME custom-named theme gave no overwrite prompt
Symptom: operator named the theme "Skeleton2", uploaded once, then uploaded the
same bake again and got NO "already on the orb, overwrite?" modal.

Root cause is almost certainly 8c47ff1 (section 32.9). theme.go:608-635
themeDuplicate now returns "exact" when the card holds the SAME name AND SAME
content serial. The composer postTheme3ToOrb (Watch Composer.dc.html:2407-2420)
intercepts exact and shows a STATUS LINE instead of arming the Replace modal:
"Skeleton2 is already on the card, byte for byte - nothing sent." So the modal is
intentionally suppressed when bytes are identical. That matches the code but NOT
the operator's expectation. The section 32.9 double-send fix and the section 32.6
replace-modal UX are in tension for the same-bytes case. Confirm the desired
behavior, then either (1) keep the suppression but make the status line actionable
(point at the Manage-card store button, as :2418 already claims), or (2) add an
explicit Replace affordance for the exact case. Note the theme id is
sha256(name)[:12], so a re-bake under a NEW name (Skeleton -> Skeleton2) yields a
new serial; only a same-bytes re-send is "exact".

## Ground rules for this pass
- One project (gorb-os). Do not fan a change into sibling workspace folders.
- Long bench commands (WiFi OTA, serial flash, first idf.py build) MUST run
  detached and polled - see .clinerules. A build over ~30 s gets killed
  mid-flight otherwise.
- Every fix lands with its test: Go test for hub/fake, a headless-*.test.js pin
  for composer/overlay. Commit style is Conventional: fix(scope): imperative
  summary.
- Rebuild the hub binary and run BOTH node suites + go test ./... green before
  declaring any fix done. Confirm the running process is the windowed desktop
  instance (one server), not a headless probe, before handing back.
