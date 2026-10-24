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

### Issue 5 - a tempo-only re-send to a banked dial prompted AFTER upload
Symptom: operator changed only the Motion/tick cadence of a dial already on the
card. The bake uploaded fully, THEN a modal appeared, and confirming it read as a
no-op ("using that one - didn't copy over").

Root cause has two halves. (1) A tempo change rides the runtime cadence scalar
(second_sweep/tick_rate), so it DOES move the shipped content serial - verified
headless: perpetual sweep=650821054, tick2=3540822210, tick4=2157822246. The
precheck reads that serial, so it should have caught the duplicate BEFORE upload.
It only failed to when it could not get a clean answer: theme.go's themeDuplicate
returns "clear" (not "ask") when the hub cannot read the device's theme list, and
the composer fell through to a blind upload, leaning on the device's late 409 - the
post-upload modal. (2) The commit success line read "replaced from the copy already
on the card", which sounded like a no-op; on identical art with a different cadence
it looked like nothing landed, though the commit did rename the new scratch and
queue a reboot (orb_net.c:4996, orb_theme.c:2080-2152).

Fix (composer only, hub contract unchanged): postTheme3ToOrb now FAILS THE PRECHECK
CLOSED. A fetch that died, an HTTP error, or a "clear" verdict carrying the hub's
note are treated as UNKNOWN card state and stop before upload with
"...could not get a clean read of X... Press Send again to replace it anyway."; the
bake is held so the second press forces the overwrite. A genuinely free name
(clear, empty note) still uploads. The commit line now reads "the new build replaced
the old copy on its card - stored - rebooting onto it". Guarded by six new
headless-ui.test.js checks (Bug B). Suites: engine 177, regression 96/15, ui 260,
component 51, orbtheme 74, bundle3 190; Go precheck/commit/embed-parity green.
A firmware-side commit-token correctness audit was NOT needed: recommit re-verifies
the staged scratch from the card and renames THAT file, so it already files the new
bytes, not the stale copy.


### Issue 6 - Watch Composer sweep pace floor-snapped an off-rung device rate
Symptom: a sweep dial's device-pace readout and preview ran on the `[15,12,8,4]` rung
ladder. A crossed-plate estimate of raw ~11 displayed as 8, under-selling the glass.

Benchmark (live, this device, 2026-10-24, through the hub console route at
`127.0.0.1:3779/api/console`): `modern` (banked running-seconds subdial, cov 0.103,
shadows on) held a post-reboot steady-state median of **10 fps**; `neon` (a static moon
crossed by the sweep, cov 0.114, shadows on) held a median of **12**. Near-identical
coverage, both under moving twins, two different sustained rates - and the old model
called both 8. Perpetual (four *static* calendar subdials, cov 0.32) still clamps to the
panel floor 4, the `07d3411` anchor.

Fix (composer only, no firmware change - the bank already plays on a millisecond flush
clock, decoupled from frame count): `_devPace` now **rounds** `raw = fps * (1 - K * cov)`
to the nearest whole fps, clamped to the scheduler floor 4 and the tier ceiling, instead
of snapping it down the rungs. `modern` now reads 11 and `neon` 11 - each within one of
its measurement. The shipped bank tempo is untouched: `F`/`B` still follow `budget().fps`
(a rung); only the preview rate and the readout fold the rounded estimate through
`devFps()`.

The plan's second half - narrowing `K_TWIN` to moving/animated crossed plates and
charging a plain static single plate only `K_PLAIN` - was TESTED AND REJECTED by this
same benchmark. Perpetual's crossed plates are static calendar subdials, yet the shipped
dial clamps to 4; only `K_TWIN` reaches 4, so `K_PLAIN` for a static plate would lift the
anchor to ~9 and break it. `neon`'s static moon under a moving twin measured 12, which
`K_TWIN` rounded (11) brackets; `K_PLAIN` would over-predict (13) and claim pace the
single static dial did not show. The discriminator the data supports is the MOVING TWIN
(`shOn`), which is what the committed model already gates on. So the gate and the two
slopes stand; only the snap was wrong.

The Motion menu gained the `8 / s` tick rung, gated on `budget().fps >= 8` (the same rung
the row ladder already prices), so a 4-fps dial is never offered a rate it cannot drive;
the readout now also states the bank frame count (`12 fps · 12 frames`). Guarded by a new
`headless-pace.test.js` (13 checks, the asserted form of `probe_pace.cjs`) and four
`headless-ui.test.js` Motion-gate checks. Suites after: engine 177, regression 96/15, ui
264, component 51, orbtheme 74, bundle3 190, pace 13 - all green. README pace trip moved
to `modern` 11 / `diver` 15 / `aviator`/`sector` 15 / `perpetual` 4.

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
