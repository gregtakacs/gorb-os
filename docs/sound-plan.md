# D39: Clock sound — ticks and chimes, delivered as theme content

**Why this file exists.** The owner wants the clock to tick and chime, with
synthesized defaults and owner-uploaded sounds for every event. The design was
argued in one session on 2026-10-28 and agreed in full. This file records the
decision, the hardware facts it rests on, the prototype's measured output, and
the slices, so the next session builds it rather than re-deriving it. Read it
before touching the composer's Sound tab, the asset class enum, the FORMAT 3
element table for sound, or anything that opens I2S.

**Status: RULED (owner, 2026-10-28). Not built.** The prototype generator is
committed (`tools/sound_synth_proto.py`, §6). Nothing else exists yet. These
are settled and not open for re-litigation:

| Parameter | Ruled value |
|---|---|
| Where sounds live on the device | In the theme bundle, the clock's FORMAT 3 section. Never in the `.orbapp`'s installed part (D21 holds) |
| Who plays them | The OS: one clock-agnostic sound engine behind a `sound` capability |
| Who decides when | The clock app: tick loop selection, quarters, hour strikes, quiet hours |
| Where the defaults come from | A generator in the clock's editor region (D37), not WAV bytes |
| Where owners set it all | A new **Sound** tab in the clock's theme editor (the composer) |
| Slot model | One slot per event (§3.1), so any slot can be an owner upload |

## 1. The split, mirrored from pixels

The platform already splits pixels three ways. Sound takes the same split
line for line, and that is the whole argument for where each part lives.

| Part | The pixel precedent | Sound | Lives in |
|---|---|---|---|
| Content | theme art (D21) | 16-bit PCM per slot | the theme bundle, clock section |
| Policy | `hand_angles`, the complication job list (§1.2a) | which loop, which chime at :15/:30/:45/:00, how many strikes, quiet hours | the clock app |
| Engine | the compositor and frame scheduler (§1.1) | I2S, ES8311, amp enable, mixer, loop anchoring | the OS |
| Authoring | the composer, which ships generators and no art (`watch-engine.js`, `dial-renderer.js`) | Sound tab: generate, upload, listen, budget | the clock's editor region |

**The engine cannot live in the app, for three reasons.**

1. **It is hardware.** The ES8311 shares the I2C bus with touch, IMU, RTC and
   PMIC, and amp enable is a power decision. §1.1 keeps the panel, the radios
   and power policy in the OS for the same reason, and a wasm3 app cannot reach
   a driver anyway.
2. **Per-sample work has to be compiled.** §5's rule, "interpreted decisions
   are free, interpreted pixels are fatal", applies unchanged to samples:
   mixing at 22,050 Hz is per-unit work of the same kind.
3. **Arbitration is cross-app.** Two apps share the glass during a swap, a
   suspended app must fall silent (C7's sound equivalent of "no tick"), and
   volume and quiet hours are owner config in the way the tz is (D17). Only the
   OS sees all of that.

None of this puts clock knowledge in the OS. The engine's surface is as
clock-agnostic as `layer_add`: play slot X once or looped, at a gain, stop it,
and anchor a loop to wall time (§4.2).

## 2. The hardware, verified 2026-10-28

Source: Waveshare's brookesia BSP header
(`firmware/brookesia/components/waveshare__esp32_s3_touch_amoled_1_75/include/bsp/esp32_s3_touch_amoled_1_75.h`
in `waveshareteam/ESP32-S3-Touch-AMOLED-1.75`), cross-checked against the
schematic PDF in the same repo.

| Item | Value |
|---|---|
| Codec | ES8311, on the shared I2C bus (SDA 15 / SCL 14), `ES8311_CODEC_DEFAULT_ADDR` |
| Mic ADC | ES7210 (not used by this plan) |
| Amplifier | NS4150B class-D, mono |
| Speaker | bundled 8 Ω 2 W, MX1.25 2P header |
| I2S | MCLK **42**, BCLK 9, LRCK 45, DOUT (S3 → codec) 8, DIN 10 |
| Amp enable | GPIO **46**, drive high to play |
| BSP default rate | 22,050 Hz, 16-bit (`BSP_I2S_DUPLEX_MONO_CFG(22050)`) |

**A trap in the vendor tree.** The Arduino `examples/arduino/libraries/Mylibrary/pin_config.h`
carries a stale second block, `I2S_MCK_IO 16`. GPIO 16 is `PIN_KNOB_SW` here
(`main/board_config.h`). The BSP's 42 is correct. Nothing in `board_config.h`
claims 8, 9, 10, 42, 45 or 46 today.

**The speaker sets the floor.** A 2 W micro speaker gives little below about
300 Hz, and energy there only buys distortion. Every default is authored with
its weight above 350 Hz and passed through a 350 Hz second-order high-pass
(§6). The Sound tab offers the same filter as a "simulate the orb speaker"
toggle, so a laptop does not make a theme sound fuller than the dial will.

## 3. Content: what the theme carries

### 3.1 Slots, one per event

Owner uploads are whole recordings, so slots follow events and not bells. An
empty slot is silence for that event: D31's "the app supplies its own default"
lands here as "the default is quiet", which needs no bytes.

| Slot | Plays | Notes |
|---|---|---|
| `tick_loop` | continuously while the dial is visible and ticking is on | seamless loop, anchored to wall time (§4.2) |
| `chime_q1` | at :15 | |
| `chime_half` | at :30 | |
| `chime_q3` | at :45 | |
| `chime_hour` | at :00, before the strikes | |
| `strike_hour` | once per hour number at :00, after `chime_hour` | one strike, repeated by the app |

These names join the clock's name table in `tools/theme_schema.py`, beside its
art slots. Identity stays (class, slot) per that file's rule; the names belong
to the app.

**Superseded in the same session:** an earlier proposal had the theme carry five
single bells and the app sequence Westminster from them (about 0.5–0.8 MB
instead of about 2 MB). Arbitrary uploads of whole chimes made per-event slots
the only model that serves both. The generator renders Westminster into the
per-event slots, so the app never sequences notes and the mixer needs about two
voices rather than four to six.

### 3.2 The class and the payload

- **A new asset class, `ORB_ASSET_SOUND`, appended at 10.** That is the PREVIEW
  and AUX precedent: end of enum, `ORB_ASSET_CLASS_N` becomes 11, and
  `ORB_ASSET_CLASS_ABI` does not move. An OS that predates it refuses class 10
  at its own bound, and D26 makes that a refusal at the door.
- **Per-sound facts go in a small header at the front of the sound's payload,
  not in new element-table columns.** The facts are encoding, sample rate, loop
  flag, beat period, and the first beat's sample offset. Keeping them out of the
  table means `ELEMENT_WORDS` (25), `ELEMENT_WORDS_MIN` and
  `ORB_BUNDLE_ELEM_WORDS_MIN` do not move, and only the baker, the C reader and
  their suites move together. The header layout is open (§8).
- **Encoding.** PCM16 LE mono is ruled. Lower-rate PCM (16 kHz) and IMA-ADPCM
  (4:1) are open (§8), and both exist to answer the budget below.

### 3.3 The budget

Sound shares D29's `ORB_THEME_SLOT_CAP` (0x3F0000, 4,128,768 B per bundle) with
the art, and the mapped-flash page budget with it too. Measured on the prototype
at 22,050 Hz PCM16:

| Content | Bytes |
|---|---|
| One 8 s tick loop | 352,800 |
| `westminster_q1` / `q2` / `q3` / `q4` (each includes the 3.5 s ring-out) | 291,060 / 427,768 / 564,480 / 701,188 |
| `bell_hour`, one strike | 220,500 |
| A full default set (one loop + four chimes + strike) | about 2.56 MB |

So a full default set is about 62% of a bundle before any art. The Sound tab
prints bytes per slot against the remaining budget, and the baker refuses an
over-cap bundle exactly as it does for art (D29: editor at build time, installer
at verify time).

## 4. The engine: what the OS owns

### 4.1 Capability and service surface

- **`sound` capability**: declared at install and revocable on the dial, the D19
  pattern.
- **New `orb_svc_t` entries**, appended. The shape is play a slot (one-shot or
  looped, with gain), stop a handle, set gain, and start a loop anchored to a wall
  time. The exact signatures need an owner ruling, as `tz_offset` (D17) and the
  six `theme_*` entries (D18) did. **Open (§8).**
- **Suspend silences.** An app's sounds stop at `suspend` and do not restart on
  `resume` unless the app asks again. That is C7 read for audio.
- **The mixer is compiled C**, with at least two voices so a ringing chime can
  overlap the tick loop.

### 4.2 Loops stay locked to the second hand

A free-running loop starts at an arbitrary phase and drifts: the I2S clock comes
from its own fractional divider, not from wall time. Within an hour a 1 Hz tick
would audibly fall out of step with a stepping hand. The fix stays clock-agnostic:

- **Anchored start.** The app asks the engine to start a loop with its first
  beat (the offset in the payload header) aligned to a wall-clock instant.
- **Re-anchoring in silence.** The engine compares the loop position with wall
  time and drops or repeats a sample or two inside the silence between beats,
  where no one can hear it.
- **Display latency.** A commit reaches glass 55–138 ms after it is issued
  (`docs/fps-budget-findings-2026-10-10.md`). The same anchor can carry an
  offset so the tick lands with the hand, not ahead of it. The offset is
  measured, not guessed (M-S2).

**The beat must match the theme.** `tick_rate` and `second_sweep` are theme
section words (`orbtheme-baker.js:45`, read at `orb_clock3.c:441-444`), so the
editor knows the dial's beat when it bakes. A tick loop whose beat period
disagrees with a stepping hand's rate is refused, by the editor at bake time and
by the reader at the door (D28: verification stays at the door). How a loop
should relate to a sweeping hand (`second_sweep` set) is open (§8).

### 4.3 Two risks, measured before the ABI is ruled

- **M-S1: dropouts while flash writes.** A flash erase suspends the cache (D6;
  `orb_app_install.c:562` keeps the suspend from spanning a partial sector). On
  the S3 that blocks PSRAM as well as mapped flash, so playback must run from an
  internal-RAM DMA ring long enough to cover NVS writes and theme flips, or go
  quiet during them. Internal RAM is tight (`app-platform-plan.md` §16.1: "the
  wall is one 4 KB allocation"). Measure the longest cache-off window under real
  writes, and choose ring size or a mute policy from the number.
- **M-S2: audio-to-glass offset.** Measure from tick issue to audible click
  against tick issue to the hand's pixels landing, on the shipped dial, and
  record the offset §4.2 applies.

## 5. Policy: what the clock app owns

- **Choosing slots and schedule.** Loop while the dial is visible; play
  `chime_q1`/`chime_half`/`chime_q3` at :15/:30/:45; play `chime_hour` then
  `strike_hour` × the 12-hour number at :00. Local time comes through `now` plus
  `tz_offset` (D15/D17), as the dial's own time does.
- **Declared settings** (C9 types): ticking on/off, chimes on/off, hour strikes
  on/off, quiet hours start and end. Exact ids are part of S6.
- **Volume** is an owner-level setting the OS applies, like the tz. Whether it is
  per app or global is open (§8).

## 6. Defaults: a generator in the editor region, not WAVs

**The defaults ship in the clock's `.orbapp` editor region, as code.**

- **The region is the right place.** It holds host-side authoring content that
  the firmware only CRC-checks and never installs (D37 §4.4). It stays on the
  card past the installer's copied prefix (D37 §3). Nothing in it is art the app
  paints, so D21 is untouched. Once baked, the sounds travel as theme content.
- **WAVs do not fit.** The region cap is 2 MiB. The composer uses 652,355 B of
  it (D37 §3), leaving about 1.4 MiB. The prototype's full output as raw PCM is
  4,277,696 B: 1,764,000 B for the five tick loops and 2,513,696 B for the four
  single strikes and the four Westminster phrases. (The design session said
  "about 4.4 MB" before this was summed; the sum is the figure.)
- **A generator does fit.** A JS port of the prototype is an estimated 10–20 KB,
  renders every default in the browser in well under a second, and is seeded so
  the same theme bakes to the same bytes. It is also the code behind the tab's
  controls (§7), so the defaults come with it.

**The prototype.** `tools/sound_synth_proto.py` (numpy only) is the 2026-10-28
generator the owner auditioned. Run `python3 tools/sound_synth_proto.py <outdir>`
to write each sound as `.pcm` (16-bit LE mono, 22,050 Hz, headerless) and `.wav`.

- **Ticks.** A short noise impulse rings three tuned two-pole resonators, with
  an optional case thump at 340–380 Hz. Each beat is three sub-clicks (unlock,
  impulse, drop) a few ms apart.
- **Bells.** Additive synthesis on bell partial ratios (hum 0.5, prime 1, tierce
  1.19, quint 1.5, nominal 2, upper partials). Decay shortens with frequency, and
  slightly detuned pairs give the slow beating of a real bell. Westminster runs
  B4–G#5. The tune dates from 1793 and is public domain.
- **Loops.** Every beat is synthesized afresh with jittered resonator pitch
  (±2%), Q and timing. Tick and tock sit a few ms off-centre (beat error).
  Loudness drifts through exactly two cycles per loop, plus a small per-beat
  random term. Beats are written circularly, so a tail past the end lands at the
  start. The high-pass is an FFT filter, which is circular too.
- **Rendered loops:** `loop_mantel_1hz`, `loop_grandfather_1hz`, `loop_soft_1hz`,
  `loop_pocketwatch_4hz` (14,400 bph), `loop_watch_8hz` (28,800 bph).

Measured on that output, 2026-10-28, from a fresh run of the committed file (the
seed is fixed, so a rerun reproduces these bytes):

| Check | Result |
|---|---|
| Seam step, last sample to first | 0–3 LSB on all five loops (normal sample-to-sample steps run to the thousands) |
| Per-beat RMS spread, standard deviation | 0.5–0.9 dB |
| Energy below 300 Hz | 0.0% on all five loops; 0–0.6% on bells and chimes; 4.6% / 10.0% on the single-shot `tick_grandfather` / `tock_grandfather` (their thump), which no loop inherits |

**One finding from the prototype.** Noise into high-Q resonators swings each
beat's energy by about ±6 dB on its own: before the fix, beat RMS spread reached
12.6 dB on the 8 Hz loop, which sounds like stuttering. Each sub-click is now
normalized to unit RMS before gain is applied, so level variance comes only from
the loop's deliberate term. The JS port must keep that normalization.

**The JS port becomes the authority** and the Python prototype retires once the
port lands (slice S2). They will not match bit for bit, because numpy's random
stream is not JS's, but they will match by ear. Keeping both would be a mirror
with no test to hold it.

**If a recorded default is ever wanted** (a real grandfather clock), it cannot be
generated. It ships in the region as Opus or OGG, about 50 KB for 8 s, decoded by
the browser. That is the escape hatch, not the default path.

## 7. The Sound tab

A seventh tab, `sound`, beside the composer's `dial`, `comps`, `hands`, `move`,
`light`, `case` and `export` (`Watch Composer.dc.html`). It follows the existing
patterns: `resetTab('sound')` restores the preset's sound design, and
`roll('sound')` rerolls the generator's seed through the existing `seeds` state.

**Per slot (§3.1), one of three sources:**

1. **Generated.** Style, beat rate and variance for `tick_loop`; bell set and
   phrase for the chimes. The loop's beat rate follows the theme's `tick_rate`.
2. **Uploaded.** Any file the browser decodes (WAV, MP3, OGG, FLAC, at any rate
   or channel count). The tab resamples to 22,050 Hz mono through
   `OfflineAudioContext`, trims silence, normalizes to −1 dBFS, and optionally
   applies the 350 Hz high-pass.
3. **None.** The slot stays silent.

**Uploaded ticks, two routes:**

- **A single tick.** The tab builds the varied 8 s loop from it with the
  prototype's method: per-beat pitch and gain by resampling (±2%), timing jitter,
  a pitch-shifted copy as the tock, and circular placement.
- **A recorded loop.** The tab finds the beats, trims to a whole number of them,
  crossfades the seam circularly, and checks the beat rate against the theme's
  `tick_rate`. A mismatch is refused with a sentence, not baked.

**Always shown:** a player that loops through WebAudio, which is sample-exact so
the seam is audible exactly as on the dial; the "orb speaker" toggle; and bytes
per slot against the remaining D29 budget.

**Re-editing.** Nothing keeps raw uploads. The tab reads sounds back out of an
installed theme as WAV, so the device's copy is the canonical one. That also
covers sound for the composer's no-persistence flaw
(`docs/watch-composer-plan.md`, flaw 2).

**Delivery is the existing path.** The tab's output goes into the clock section
of the FORMAT 3 bundle the composer already bakes, through the bridge, then
`/api/theme/upload` and the device's `/api/v1/sd/theme` (filed as
`/sd/themes/<slug>.orbtheme`), then install into the flash slot. No new door,
file type or card directory. No WAV ever reaches the device.

## 8. Open items

Implement up to the question mark and leave these open until ruled.

1. **`orb_svc_t` signatures** for play, stop, gain and anchored loop (§4.1).
2. **Payload header layout** (§3.2): word order, version word, and what a reader
   refuses.
3. **Encodings beyond PCM16**: 16 kHz and IMA-ADPCM, per slot or per bundle
   (§3.2).
4. **Sweeping hands**: what a tick loop anchors to when `second_sweep` is set
   (§4.2).
5. **Volume**: per app or global, and where the owner sets it (§5).
6. **The DMA ring size or mute policy**, decided from M-S1's number (§4.3).

## 9. Slices, in the order that de-risks them

| Slice | Content | Gate |
|---|---|---|
| **S1** | Prototype generator committed as `tools/sound_synth_proto.py` | **DONE 2026-10-28** |
| **S2** | JS generator plus the Sound tab: generate, upload, preview, budget. Host-side only, no format change. `make_clock_region.py`'s `members()` gains the generator file, and `host_clock_region_test.py`'s member set learns it in the same commit. The prototype retires | the tab renders all defaults; the seam and RMS checks of §6 rerun on the JS output |
| **S3** | Format: `ORB_ASSET_SOUND` = 10, the payload header, the baker's emitter, the C reader, sound slots in `theme_schema.py`'s clock table. Three-way mirror plus mutants, the D34/preview pattern | host suites green with new mutants caught |
| **S4** | Bench bring-up: I2S plus ES8311 plus amp on GPIO 46 plays one PCM buffer. Run M-S1 and M-S2 | numbers recorded here before S5 is ruled |
| **S5** | OS engine: mixer, `sound` capability, service entries (after §8 items 1 and 6 are ruled), loop anchoring | bench: a loop holds phase against the hand for an hour |
| **S6** | Clock policy: schedule, strikes, settings, quiet hours | bench: :00/:15/:30/:45 on a real dial |

## 10. The one sentence to remember

Sound is theme content, played by a clock-agnostic OS engine, scheduled by the
clock app, and authored in the clock editor's Sound tab from a generator that
ships in the editor region instead of WAV files.
