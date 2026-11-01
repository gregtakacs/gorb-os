# D39: Clock sound — ticks and chimes, delivered as theme content

**Why this file exists.** The owner wants the clock to tick and chime, with
synthesized defaults and owner-uploaded sounds for every event. The design was
argued in one session on 2026-10-28 and agreed in full. This file records the
decision, the hardware facts it rests on, the prototype's measured output, and
the slices, so the next session builds it rather than re-deriving it. Read it
before touching the composer's Sound tab, the asset class enum, the FORMAT 3
element table for sound, or anything that opens I2S.

**Status: RULED (owner, 2026-10-28). BUILT 2026-10-31 on `feat/clock-sound`,
bench-proved only as far as §11 says.** The engine, the format, the clock policy,
the Sound tab and its generator exist and pass their host suites; the codec comes
up on orb-B. Nobody has yet heard it play: main's v1.1.42 cannot bring Wi-Fi up on
that board (§11.2), so no door reached the engine. These are settled and not open
for re-litigation:

| Parameter | Ruled value |
|---|---|
| Where sounds live on the device | In the theme bundle, the clock's FORMAT 3 section. Never in the `.orbapp`'s installed part (D21 holds) |
| Who plays them | The OS: one clock-agnostic sound engine behind a `sound` capability |
| Who decides when | The clock app: tick loop selection, quarters, hour strikes, quiet hours |
| Where the defaults come from | A generator in the clock's editor region (D37, `docs/app-theme-editor-plan.md`), not WAV bytes |
| Where owners set it all | A new **Sound** tab in the clock's theme editor (the composer) |
| Slot model | One slot per event (§3.1), so any slot can be an owner upload |

**Amended (owner, 2026-10-30).** These are settled too:

| Parameter | Ruled value |
|---|---|
| Tick rate | The loop beats at the second hand's step rate: 1, 2, 4 or 8 per second. A sweeping hand takes the movement's balance beats (§4.2). Every tick voice renders at every rate (§6.1) |
| Hour patterns | `count`, `ships_bells` and `morse` are all offered; `repeater` is in the catalog with its trigger open (§5.1) |
| Steps and jumps | A step on a minute, hour or calendar row is an event with its own slot (`jump_minute`, `jump_hour`, `jump_date`). Motion itself is D41 (`docs/hand-motion-plan.md`), where `jump` is `step` 1 |
| Defaults | Every preset gets a sensible default mapping, and every sound in the catalog is available for any slot of its kind (§7.1, §7.2) |

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
| `strike_hour` | at :00 after `chime_hour`, as the hour pattern says (§5.1) | one stroke, repeated by the app; the ship's bell under `ships_bells` |
| `jump_minute` | on each step of a minute-source row: a jumping minute, or a half-minute `step` 2 (D41) | a short event; the station's clunk |
| `jump_hour` | on each step of an hour-source row, such as a jump hour hand or disc (D41) | |
| `jump_date` | when a calendar row steps at midnight (D41) | silent under `drag`, which has no single instant |
| `morse_dit`, `morse_dah` | the `morse` hour pattern's two symbols (§5.1) | the dah is three dits long, by Morse's own timing |
| `strike_qtr`, `strike_min` | the `repeater` pattern's quarter ting-tang and high minute stroke (§5.1) | filled only when that pattern is chosen; named to fit the wire's 11-character slot name |

A jump slot is trivially in sync with its row. The app plays it on the tick
that re-poses the row, so it needs no anchoring (D41 §6). One sound per event,
however many rows step on it: a jump hour disc and a jump hour hand share
`jump_hour`.

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
- **Per-sound facts ride the element row's own columns, and the payload is bare
  PCM** (corrected 2026-10-31; the ruling had them in a payload header). The
  projection `orb_clock3_parse` is pure and runs before a byte of the bank is
  resident (`orb_theme.c`'s load order), so a header could never be refused at
  the door; columns can. No column count moves either way, which was the
  ruling's reason. AUX's precedent widened `kind`/`src` the same way:

  | Column | On a SOUND row |
  |---|---|
  | `src` | the encoding: 0 = 16-bit LE mono PCM at 22,050 Hz, the only one wired |
  | `kind` | the theme's hour pattern (§5.1), the same on every sound row |
  | `flags` | `EF_LOOP` on the tick loop and on nothing else |
  | `vmax` / `vmin` | the loop's beat per 100 s, and the sample its first beat starts on; 0 on one-shots |
  | `w` / `h` | 0: a sound places nothing, and its length (up to 176,400 samples) would not fit a u16 |

  The catalogue row is the plane: w samples, h 1, size 2w, priced w*h*2 as a
  preview is. **It must sit at an even offset**: a 16-bit load from the mapped
  bank faults unaligned. Writers therefore put sounds at the **head** of the
  catalogue and move `plate_off` and `bg_key_off` past them (a delta plate's
  canvas seeds from `blob() + bg_key_off`, so a zero left there paints PCM).
- **Encoding.** PCM16 LE mono is ruled. Lower-rate PCM (16 kHz) and IMA-ADPCM
  (4:1) are open (§8), and both exist to answer the budget below.

### 3.3 The budget

Sound shares `ORB_THEME_SLOT_CAP` with the art — 0x7F0000, 8,323,072 B since
Migration 1 made the slot whole (this section first quoted D29's retired 4 MB), and the mapped-flash page budget with it too. Measured on the prototype
at 22,050 Hz PCM16:

| Content | Bytes |
|---|---|
| One 8 s tick loop | 352,800 |
| `westminster_q1` / `q2` / `q3` / `q4` (each includes the 3.5 s ring-out) | 291,060 / 427,768 / 564,480 / 701,188 |
| `bell_hour`, one strike | 220,500 |
| A full default set (one loop + four chimes + strike) | about 2.56 MB |
| `station_minute_clunk` (a jump slot) | 11,024 |
| A Morse dit / dah at 20 wpm (60 / 180 ms) | about 2,650 / 7,950 |

The jump and Morse slots barely register. The Westminster set is the heaviest
default; most presets' defaults (§7.2) use shorter phrases and come in well
under it.

So a full default set is about 31% of a bundle before any art. The Sound tab
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
  six `theme_*` entries (D18) did. **Still open (§8)** — and not on the critical
  path: the clock is compiled (D35's one compiled row is the shell; the clock's
  policy runs in `clock_view.c`), so it calls the engine's C surface directly
  (`main/audio/orb_sound.h`). The capability and the entries wait for the first
  card app that wants to make a sound.
- **The C surface, as built**: `orb_sound_play(pcm, n, gain, flags, at_us,
  anchor_us)` with `LOOP`, `ANCHORED` and `LEASED` flags, `stop`, `stop_all`,
  `reanchor`, and `lease_renew`. A one-shot takes an esp_timer instant, so a
  strike pattern is laid down once and lands on time; a LEASED voice dies 1.5 s
  after its owner's last tick, which is how a parked dial falls silent without
  being asked (C7 for audio).
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
section words (`orbtheme-baker.js:44`, read at `orb_clock3.c:441-444`), so the
editor knows the dial's beat when it bakes. A tick loop whose beat period
disagrees with a stepping hand's rate is refused, by the editor at bake time and
by the reader at the door (D28: verification stays at the door).

**The rate, by hand motion (ruled 2026-10-30):**

| Seconds hand (D41) | Loop beat | Anchor |
|---|---|---|
| `step` at 1, 2, 4 or 8 per second (the composer's Motion choices) | that rate exactly | beat 0 on a whole wall second, so beat *k* lands with step *k* |
| `sweep` | the movement's balance beats: 1, 2 or 4 per second (the Move tab's `Balance beats`) | beat 0 on a whole wall second, so what you hear matches the balance you see |
| `stop-to-go` | none by default: a station clock is silent between jumps, and its sound is `jump_minute` | no loop |

The table follows whichever row shows seconds: a centre hand, a sub-dial
pointer, or a retrograde seconds fan. D41 §2.2 refuses a stepping second row
whose *N* differs from `tick_rate`, so all stepping second rows on a dial share
one rate, and the loop has one beat to match. An 8 s loop
holds a whole number of beats at every rate in the grid (8, 16, 32 or 64), so
one loop length serves all of them. Rate-free voices (§7.1: `geiger`), and those
with only some rates (`heartbeat`, 1 per second), are offered only where no
stepping hand has to match them.

### 4.3 Two risks, measured before the ABI is ruled

- **M-S1: dropouts while flash writes.** A flash erase suspends the cache (D6;
  `orb_app_install.c:562` keeps the suspend from spanning a partial sector). On
  the S3 that blocks PSRAM as well as mapped flash, so playback must run from an
  internal-RAM DMA ring long enough to cover NVS writes and theme flips, or go
  quiet during them. Internal RAM is tight (`app-platform-plan.md` §16.1: "the
  wall is one 4 KB allocation"). Measure the longest cache-off window under real
  writes, and choose ring size or a mute policy from the number. **Built as the
  mute policy (2026-10-31):** the mixer's stack is PSRAM, so it simply stops for
  an erase, and the DMA is set to clear what it has sent (`auto_clear_after_cb`),
  so the speaker plays silence rather than a repeated buffer; a loop re-enters
  its phase on the next block. The ring is 4 x 256 frames (46 ms, 2 KB internal)
  and the number that would justify more is still M-S1's to give.
- **M-S2: audio-to-glass offset.** Measure from tick issue to audible click
  against tick issue to the hand's pixels landing, on the shipped dial, and
  record the offset §4.2 applies.

## 5. Policy: what the clock app owns

- **Choosing slots and schedule.** Loop while the dial is visible; play
  `chime_q1`/`chime_half`/`chime_q3` at :15/:30/:45; play `chime_hour` then the
  hour pattern (§5.1) at :00; play a jump slot on the tick its row steps. Local
  time comes through `now` plus `tz_offset` (D15/D17), as the dial's own time
  does.

### 5.1 Hour patterns

The pattern is app policy, a declared setting. It is chosen in the Sound tab
for the theme's default, and the owner can change it on the device.

| Pattern | What plays | Slots |
|---|---|---|
| `count` | `strike_hour` once per 12-hour number, about 2 s apart | `strike_hour` |
| `ships_bells` | Every half hour, 1 to 8 strokes in pairs (*ding-ding*, pause), counting up through each four-hour watch: 00:30 is one bell, 04:00 is eight, 04:30 is one again. It replaces the quarters, so `chime_*` are not played under it | `strike_hour` (the bell stroke) |
| `morse` | The 12-hour number as Morse digits at 20 wpm: 3 is `...--`, and 12 is `.---- ..---`, with a letter gap between digits | `morse_dit`, `morse_dah` |
| `repeater` | Hours on a low stroke, quarters as ting-tang pairs, minutes on a high stroke, as a minute repeater does | `strike_hour`, `strike_qtr` (one ting-tang), `strike_min` (the high stroke). **Trigger open (§8)**: a repeater sounds on demand, and the clock has no gesture for that yet |
| `none` | Only `chime_hour`, if it is filled | |

**Ship's bells keeps the plain watch count.** The Royal Navy's dog-watch
variant (1, 2, 3, then 8 bells between 18:30 and 20:00) is a later option,
not the default.
- **Declared settings** (C9 types): ticking on/off, chimes on/off, hour strikes
  on/off, quiet hours start and end. Exact ids are part of S6.
- **Volume** is an owner-level setting the OS applies, like the tz. Whether it is
  per app or global is open (§8).

## 6. Defaults: a generator in the editor region, not WAVs

**The defaults ship in the clock's `.orbapp` editor region, as code.**

- **The region is the right place.** It holds host-side authoring content that
  the firmware only CRC-checks and never installs (`docs/app-theme-editor-plan.md` §4.4). It stays on the
  card past the installer's copied prefix (same file, §3). Nothing in it is art the app
  paints, so D21 is untouched. Once baked, the sounds travel as theme content.
- **WAVs do not fit.** The region cap is 2 MiB. The composer uses 652,355 B of
  it (same file, §3), leaving about 1.4 MiB. The prototype's full output as raw PCM is
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

### 6.1 The rate grid and the audition pack (2026-10-29/30)

Two more prototypes ride beside the first, and they retire with it.

- **`tools/sound_pack_proto.py`** renders the audition pack the owner heard on
  2026-10-29: 67 sounds in 17 groups, which are §7.1's catalog.
- **`tools/sound_ticks_proto.py`** renders the rate grid: 16 tick voices × 1, 2,
  4 and 8 per second, 64 loops of 352,800 B each.
  - **A voice is a function of the beat, not the rate.** `fit()` cuts any beat
    tail past 85% of the period, with a cosine fade, so a grandfather voice at
    8 per second never smears into its next beat.
  - **Beat error scales with the period**, as it does on a real escapement.

Both import `sound_synth_proto.py` and are seeded. Run from `tools/` with an
output directory. Rerun on 2026-10-30, they reproduced every one of the 131
auditioned files byte for byte, and all 64 grid seams passed the check below.

**The seam check, corrected.** The "last sample to first" step in the table
above is the wrong test once a beat starts at sample 0, which every grid loop
does by design (beat 0 sits on the wall second). That step is then the beat's
own attack, and it ran to 5,869 LSB on the regulator voice. The check that holds
is the circular second difference at the seam, compared with the 99.99th
percentile of the loop's own second differences: a seam is only a defect if it
is sharper than anything inside the loop. All 5 original loops, all 13 pack
loops and all 64 grid loops pass it.

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

1. **Generated.** A voice and variance for `tick_loop`; a phrase for each chime;
   a stroke and pattern for the hour. The loop's beat rate is **derived, never
   picked**. It follows §4.2's table from the Hands and Move tabs, and changing
   either one re-renders the loop.
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

### 7.1 The catalog: every sound, any slot of its kind

The generator offers every catalog entry for every slot of its kind. A preset
(§7.2) only chooses which one is selected first. The names are the prototype
files' names and become the JS generator's ids.

| Kind | Fits slots | Entries |
|---|---|---|
| **Tick voices**, all at 1/2/4/8 per second | `tick_loop` | `mantel`, `grandfather`, `soft`, `pocket`, `wrist`, `classic`, `tourbillon`, `station_relay`, `regulator`, `woodblock`, `neon_buzz`, `waterdrop`, `sine_blip`, `glitch`, `double_pip`, `data_patter` |
| **Tick voices with limits** (§4.2) | `tick_loop` under a sweep only | `geiger` (rate-free), `heartbeat` (1 per second, also under a 1 per second step) |
| **Chime phrases** | `chime_q1`, `chime_half`, `chime_q3`, `chime_hour` | Westminster q1–q4; `regulator_tingtang` 1–4 pairs; `station_dingdong`, `station_dingdong_3tone`; `steam_whistle`, `steam_whistle_long_long`, `diesel_horn`, `departure_handbell`; `marimba_q2`, `marimba_hour`; `glass_dingdong`, `vibraphone_chord`; `cabin_ding`, `cabin_hilo`, `cabin_3tone`; `pit_radio_blip`, `race_start`; `orbit_arp`; `windchimes_flurry`, `windchimes_flourish`; `teacup_clink`, `glass_harmonica`, `glass_bell`; `singing_bowl`, `small_bowl`, `celesta_lullaby`; `synthwave_arp`, `synth_pad_bell`, `synth_bell`; `sonar_ping`; `music_box`, `kalimba`, `toy_piano_twinkle`, `glockenspiel`, `owl_hoot`, `dawn_chirp`, `tingsha`; `fm_glass_bell`, `time_pips`, `boot_arpeggio`, `shepard_swell`, `hologram_shimmer`, `door_whoosh_chime`, `chiptune_fanfare` |
| **Strokes** | `strike_hour`, `strike_qtr`, `strike_min` | `bell_hour`, `bell_small`, `rod_hour`, `regulator_gong`, `ships_bell_single`, `cuckoo_once`, `temple_bell`, `glass_bell`, `fm_glass_bell` |
| **Events** | `jump_minute`, `jump_hour`, `jump_date` | `station_minute_clunk`, `cabin_ding`, `teacup_clink`, `sine_blip` (one beat), `pit_radio_blip` |
| **Morse symbols** | `morse_dit`, `morse_dah` | `morse_tone` (750 Hz, rendered at both lengths) |

**Three entries have no file of their own yet:** `regulator_gong` (the low G4
rod), `regulator_tingtang` at 1, 3 and 4 pairs, and `morse_tone`. The pack renders
them only inside a phrase (`regulator_hour_3`, `regulator_tingtang_q2`,
`morse_hour_*`), and the JS port exposes them as entries.

**Phrases with a count are made by the pattern, not stored per count.** The pack
rendered `cuckoo_x3`, `ships_bells_3`/`_8`, `morse_hour_3`/`_12` and
`minute_repeater_10h47` to audition the patterns of §5.1. The theme stores one
stroke or symbol, and the app repeats it.

**Trademarks.** `cabin_*` and `time_pips` are generic forms (a cabin chime, six
pips), not recordings of any airline's or broadcaster's sound, and their names
stay generic.

### 7.2 Default mapping per preset

The first column is the composer preset (`watch-engine.js` `PRESETS`). "Tick" is
the voice; its rate comes from §4.2, so it is not listed. A blank chime cell is
an empty slot, which is silence. Every default draws only on §7.1.

| Preset | Tick | Quarters (q1 / half / q3) | Hour chime | Hour pattern and stroke | Jumps |
|---|---|---|---|---|---|
| `skeleton` | `classic` | Westminster q1 / q2 / q3 | Westminster q4 | `count`, `bell_hour` | |
| `calendar` | `mantel` | Westminster q1 / q2 / q3 | Westminster q4 | `count`, `bell_hour` | `jump_date`: `teacup_clink` |
| `modern` | `wrist` | | `glass_dingdong` | `none` | |
| `sector` | `woodblock` | `marimba_q2` / `marimba_q2` / `marimba_q2` | `marimba_hour` | `none` | |
| `diver` | `wrist` | (replaced by the pattern) | | `ships_bells`, `ships_bell_single` | |
| `orbital` | `tourbillon` | | `orbit_arp` | `none` | |
| `aviator` | `wrist` | half only: `cabin_hilo` | `cabin_3tone` | `none` | |
| `opentour` | `tourbillon` | half only: `pit_radio_blip` | `race_start` | `none` | |
| `kitetour` | `tourbillon` | `windchimes_flurry` ×3 | `windchimes_flourish` | `none` | |
| `station` | none (`stop-to-go`, D41) | | `station_dingdong_3tone` | `none` | `jump_minute`: `station_minute_clunk` |
| `luna` | `soft` | `small_bowl` ×3 | `singing_bowl` | `none` | |
| `neon` | `neon_buzz` | half only: `synth_bell` | `synth_pad_bell` | `none` | |
| `phase` | `soft` | `teacup_clink` ×3 | `glass_bell` | `count`, `glass_bell` | |
| `regulator` | `regulator` | ting-tang 1 / 2 / 3 pairs | ting-tang 4 pairs | `count`, `regulator_gong` | |
| `perpetual` | `pocket` | Westminster q1 / q2 / q3 | Westminster q4 | `count`, `bell_hour` (`repeater` offered, §5.1) | `jump_date`: `teacup_clink` |

**Why these.** The traditional and complicated dials (skeleton, calendar,
perpetual) take the traditional chime. The exposed-tourbillon dials take the
tourbillon voice, whose carriage swirl matches what is on screen. The diver
takes ship's bells. The station takes the railway's own sounds, and its hand
motion carries them. Calm dials (luna, phase) get soft ticks and short glass
or bowl phrases. Only six of the fifteen strike the hour, so an orb is not loud
by default.

**Two choices are the owner's to flip:** `steam_whistle` instead of the
three-tone gong on the station's hour, and `morse` on the neon or a futuristic
dial. Both are in the catalog and one click away in the tab.

**Delivery is the existing path.** The tab's output goes into the clock section
of the FORMAT 3 bundle the composer already bakes, through the bridge, then
`/api/theme/upload` and the device's `/api/v1/sd/theme` (filed as
`/sd/themes/<slug>.orbtheme`), then install into the flash slot. No new door,
file type or card directory. No WAV ever reaches the device.

## 8. Open items

Implement up to the question mark and leave these open until ruled.

1. **`orb_svc_t` signatures** for play, stop, gain and anchored loop (§4.1).
2. ~~**Payload header layout**~~ **Ruled by the build, 2026-10-31:** there is
   none; the facts ride the row's columns (§3.2).
3. **Encodings beyond PCM16**: 16 kHz and IMA-ADPCM, per slot or per bundle
   (§3.2).
4. ~~**Sweeping hands**: what a tick loop anchors to when `second_sweep` is set~~
   **Ruled 2026-10-30:** the balance's beat, anchored to the wall second (§4.2).
7. **The repeater's trigger** (§5.1): which gesture asks the clock to sound the
   time.
5. ~~**Volume**~~ **Built 2026-10-31 as global device config**, beside the tz:
   `GET/POST /api/v1/sound` (NVS namespace `sound`) carries the level 0..100
   (the ES8311's 0.5 dB steps, 60 = -20 dB), the tick/chime/strike/jump
   switches, quiet hours (default 22–7), the glass offset (default 90 ms, M-S2's
   to correct) and an hour-pattern override. No dial menu row yet.
6. **The DMA ring size**: the mute policy is built (§4.3); M-S1's number may
   still ask for a bigger ring.

## 9. Slices, in the order that de-risks them

| Slice | Content | Gate |
|---|---|---|
| **S1** | Prototype generator committed as `tools/sound_synth_proto.py` | **DONE 2026-10-28** |
| **S2** ✔ | JS generator plus the Sound tab: generate, upload, preview, budget, the §7.1 catalog at every grid rate and the §7.2 preset mapping. Host-side only, no format change. `make_clock_region.py`'s `members()` gains the generator file, and `host_clock_region_test.py`'s member set learns it in the same commit. The prototype retires | the tab renders all defaults; the seam and RMS checks of §6 rerun on the JS output — **DONE 2026-10-31** (`headless-sound.test.js`, `headless-ui.test.js`). The prototypes are kept until the JS output has been heard on the bench |
| **S3** ✔ | Format: `ORB_ASSET_SOUND` = 10, the row columns (no header, §3.2), the baker's emitter, the C reader, sound slots in `theme_schema.py`'s clock table. Three-way mirror plus mutants, the D34/preview pattern | host suites green with new mutants caught — **DONE 2026-10-31** (D39 battery: 18 refusals both ends, 6 mutants) |
| **S4** | Bench bring-up: I2S plus ES8311 plus amp on GPIO 46 plays one PCM buffer. Run M-S1 and M-S2 | numbers recorded here before S5 is ruled — **PARTIAL**: codec up on orb-B (chip 8311 at 0x18); nothing heard, M-S1/M-S2 unmeasured (§11.2) |
| **S5** | OS engine: mixer, `sound` capability, service entries (after §8 items 1 and 6 are ruled), loop anchoring | bench: a loop holds phase against the hand for an hour — **BUILT, unproved on the bench**; the svc entries and capability stay open (§4.1) |
| **S6** | Clock policy: schedule, the §5.1 hour patterns, jump slots (with D41's G5), settings, quiet hours | bench: :00/:15/:30/:45 on a real dial, ship's bells across a watch change, a clunk on a jumping minute — **BUILT, unproved on the bench** |

## 11. What was built, 2026-10-31

### 11.1 The pieces

| Piece | Where |
|---|---|
| ES8311 bring-up on the shared I2C master bus (Espressif's sequence, playback only) | `main/audio/orb_es8311.c` |
| Engine: I2S at 22,050 Hz, PA on GPIO 46 gated with idleness, a 16-voice mixer above LVGL timed by the DMA's own `on_sent`, anchored loops nudged one sample per quiet block, leases, settings | `main/audio/orb_sound.c` |
| Doors: `GET/POST /api/v1/sound`, `POST /api/v1/sound/test` (`ding`, `tick`, `loop` — no theme needed) | `main/net/orb_net.c`; the hub allowlists and fakes them |
| Format: class 10, 13 slots, the reader's refusals, the theme's sound table | `orb_assets.*`, `orb_clock3.*`, `orb_theme.*`, `theme_schema.py` |
| Clock policy | `clock_view.c` `sound_policy()` |
| Generator, Sound tab, bake | `tools/watch-composer/sound-engine.js`, the composer, `orbtheme-baker.js` |
| Sounds into any existing theme | `tools/orbsound.py add/list/strip` |

**Internal RAM.** This board has none to spare: the first flash, with ~3 KB of
engine statics, starved `esp_wifi_init`. Every buffer, the voice table and the
theme's sound table are PSRAM; the build is DIRAM 247,475 B against main's
246,355 B, and almost all of the 1,120 B is the I2S driver itself. The engine
comes up last in `app_main`, after everything that allocates internal RAM.

### 11.2 What the bench has and has not shown

- **Shown on orb-B** (`44:bd:8d:78:26:18`): the codec answers at 0x18 with chip
  id 8311 and takes the playback sequence; the engine comes up; the dial runs the
  sound policy with no panic and at main's own frame rate (4–5 fps on "Triple
  calendar", identical to `96ef302` flashed and measured the same way).
- **Not shown: anything audible.** On orb-B, main's v1.1.42 fails
  `esp_wifi_init` with `ESP_ERR_NO_MEM` (8 of 10 static rx buffers at
  `96ef302` alone, with no sound code in the image), so there is no web door and
  no serial door to ask the engine for a ding. orb-A runs v1.1.43 with a 1.4 MB
  theme and has Wi-Fi; the difference looks like the installed theme's internal
  RAM, which is main's budget and not this feature's.
- **Next on a board with a network:** `curl -X POST -d '{"sound":"ding"}'
  /api/v1/sound/test`, then `"loop"` beside a stepping second hand for M-S2's
  offset, a theme with `tools/orbsound.py add` for the quarters, and M-S1 under
  an install.

## 10. The one sentence to remember

Sound is theme content, played by a clock-agnostic OS engine, scheduled by the
clock app, and authored in the clock editor's Sound tab from a generator that
ships in the editor region instead of WAV files.
