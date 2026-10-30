# D41: Hand motion — every hand and pointer sweeps, steps or jumps

**Why this file exists.** Before this ruling, only the second hand had a motion
choice. The owner wants the same choice on the hour and minute hands and on the
calendar pointers (date, day, month), and wants the station dial's signature move:
a minute hand that jumps, and a second hand that waits at 12 for it. The design
was proposed on 2026-10-29 and ruled on 2026-10-30. This file records the
vocabulary, today's code it replaces, and the slices. Read it before touching
`hand_angles`, `aux_value_1000`, the composer's Hands tab, or the clock section's
motion words.

**Status: RULED (owner, 2026-10-30). Not built.** One item is open: the wire
encoding for the main hands' words (§4). These are settled:

| Parameter | Ruled value |
|---|---|
| Vocabulary | `sweep`, `step`, `jump`, plus `stop-to-go` (second hand only) and `drag` (calendar pointers only) |
| Who owns the motion | The theme. The words are theme words that the composer sets and the clock app reads. No OS change |
| Default for a word a theme does not set | Today's behaviour, unchanged. No shipped theme moves differently after this lands |
| Jump settle | An optional overshoot on a jump. Visual only |
| Sound | A jump is an event, so it gets a sound slot (D39 amendment, `docs/sound-plan.md` §3.1) |

## 1. What the code does today

| Hand | Behaviour | Where |
|---|---|---|
| Second | Sweeps (`second_sweep`), or steps at `tick_rate` per second | `main/clock/clock_view.c` `hand_angles`, the `secF` block |
| Minute | Creeps (`min + secF/60`). It jumps only when `second_railway` **and** `second_sweep` are both set | same function, `minF` |
| Hour | Always creeps (`hour%12 + minF/60`) | same function, `hrF` |
| AUX pointer: second, minute, hour sources | Sweeps from the next finer field, or jumps when the row carries `CLK_FLAG_JUMP` | `aux_value_1000`, `main/clock/clock_view.c` |
| AUX pointer: date, weekday, month and other calendar sources | Always jumps at the change. No finer field is added, whatever the flag says | same function, `default:` arm |

**Two gaps follow from that.**

- **No jumping minute ships.** A jumping minute is tied to the second hand's
  mode, and the composer bakes `second_railway: 0` unconditionally
  (`orbtheme-baker.js`, the `scalars` object). Only the legacy Python baker can
  set it (`tools/bake_clock.py:2564`, from a style's `secondRailway`).
- **No calendar pointer can move continuously.** There is no sweeping date and
  no mechanical drag.

## 2. The vocabulary

Every word reads the same on a main hand and on a pointer. A pointer that
already carries `CLK_FLAG_JUMP` means exactly what `jump` means here.

| Word | Meaning | Allowed on |
|---|---|---|
| `sweep` | Moves continuously: the hand's own value plus the next finer field | every hand and pointer |
| `step` | Moves in beats at `tick_rate` per second (1, 2, 4 or 8, the composer's Motion choices) | second hand |
| `jump` | Moves once per whole unit, exactly at the boundary | minute, hour, every AUX source |
| `stop-to-go` | Sweeps one turn in 58.5 s, rests at 12, and is released by the minute's jump | second hand, and only with a `jump` minute |
| `drag` | Holds, then creeps across a window that ends at the change (default 22:00 to 24:00) | calendar AUX sources |

**Per hand:**

- **Second:** `sweep`, `step` (today's tick choice), or `stop-to-go`.
- **Minute:** `sweep` (today's creep) or `jump`.
- **Hour:** `sweep` or `jump`. A sweeping hour reads the minute hand's value, so
  under a jumping minute it moves in 1/60-hour steps, as a station clock's does.
  That needs no extra word.
- **AUX second, minute and hour pointers:** `sweep` or `jump` (as today).
- **AUX date, weekday, month and day-of-year pointers:** `jump` (as today),
  `sweep`, or `drag`.
  - **`sweep` adds the next finer field:** date plus the hour of the day over 24;
    weekday the same; month plus `(date − 1)` over the month's length.
  - **`drag` moves pointers together.** Every calendar pointer on the dial drags
    across one window, so day and date move at the same time.
- **Battery and power reserve:** `jump` only. That is their mapping today
  (`theme_schema.py` `MANIFEST_HAND`).

**Refused, never guessed** (D26, at the door):

- two cadence words on one row;
- `stop-to-go` without a jumping minute;
- `drag` on a non-calendar source;
- `step` on anything but the second hand.

**Stop-to-go, exactly.** Let `e = sec + frac`, from 0 to 60. The second hand
stands at `min(e / 58.5, 1) × 360°`. The minute hand jumps at the wall-minute
boundary, and that instant returns `e` to 0, so the release and the jump are one
event. The 58.5 s figure is the Swiss railway convention. It is a theme word with
that default, so a dial can choose 59 s or 58 s.

**Jump settle.** This is an optional ease with a small overshoot (about 1.5°,
about 120 ms), applied to any `jump`. It costs repaints for a few frames per
jump, which is a few frames per minute. It is one theme word for the dial, not
one per hand.

## 3. Why this pays off

1. **Sound is free on the jump.** A jump is a single wall-time event, so the
   clock app plays the matching slot (`jump_minute`, `jump_hour`, `jump_date`)
   on the same tick that re-poses the hand. That needs none of the loop
   anchoring D39 §4.2 needs. The station's minute clunk is this case.
2. **It is cheaper to draw.** Re-posing a hand repaints its whole rotated box,
   and the hard-gate note in `clock_view.c` measured about 38 ms per frame when
   every hand re-posed each tick. A minute hand that moves once a minute is
   re-posed 60 times less often than one that creeps.
3. **It fits the split.** These are theme words in the three-way mirror
   (`orbtheme-baker.js`, `orb_clock3.c`, `tools/theme_schema.py`) plus the
   composer's preview. The OS does not change, and neither does any service.

## 4. Encoding: where the words ride

**Pointers (AUX rows): ruled.** The AUX record's flags word already carries
`FLAG_JUMP` (bit 3) and has free bits from 9 up (`theme_schema.py`'s `FLAG_*`).

- **Two bits are appended:** `FLAG_SWEEP` and `FLAG_DRAG`. The drag window is
  fixed at 22:00–24:00 until an owner asks for it to move.
- **A row with none of the three** keeps today's per-source default. That is
  sweep for second, minute and hour sources, and jump for the rest. So every
  baked theme reads unchanged.
- **More than one of the three is a refusal.**

**Main hands: OPEN.** The clock section's scalar count is gated exactly
(`orb_bundle.c`: `nsc != ORB_BUNDLE_SCALARS_N`, which is 26). So any new section
word is an ABI question, as D18's were. Two candidates:

- **(a) A new clock stamp.** Under it, word 6 (`second_railway`) widens into a
  packed `hand_motion` word: a field each for hour, minute and second, the
  stop-to-go period, and settle. Values 0 and 1 keep their legacy meanings,
  because `bake_clock.py` still writes 1. A reader that predates the stamp
  refuses it at the stamp gate, which is how C4's v4 rows arrived.
- **(b) Append scalars.** This needs a gate that accepts both 26 and the new
  count, which would be the first two-width scalar gate.

**Recommendation: (a).** It reuses a precedent the reader already enforces, it
moves no scalar count, and an old reader refuses at the door instead of
silently creeping a hand the theme meant to jump.

## 5. The composer

- **The Hands tab gains a Motion row per hand.**
  - The second hand's existing control (`1 / s`, `2 / s`, `4 / s`, `8 / s`,
    `Sweep`, in `Watch Composer.dc.html`) gains `Stop-to-go`. That option is
    enabled only while the minute is set to `Jump`.
  - The minute and hour hands get `Sweep` / `Jump`.
  - `Settle` is a toggle shown when any hand jumps.
- **The Comps tab gains a Motion choice on each pointer**, offering what §2
  allows for its source.
- **The preview moves as the dial will.** `tickTurn` (`watch-engine.js`) already
  quantises the second hand. It gains jump and stop-to-go, and the pointer driver
  gains drag, so the preview never shows motion the device will not.
- **Presets.** The station preset's design changes to a jumping minute plus a
  stop-to-go second, which is its identity. Every other preset keeps today's
  motion, so their renders do not move.

## 6. Slices

| Slice | Content | Gate |
|---|---|---|
| **M1** | Pointers: `FLAG_SWEEP` / `FLAG_DRAG` in the mirror, `aux_value_1000`'s calendar arms, refusals, and the composer's per-pointer Motion choice | host suites green, with a mutant for each new refusal |
| **M2** | Main hands: rule §4's encoding, then the mirror, `hand_angles`, and the Hands tab rows. Station preset updated | host suites green, with a mutant per refusal; preview matches the reader's angles at sampled times |
| **M3** | Bench: stop-to-go plus a jumping minute on a real dial, and settle frame cost measured | the hand rests and releases on the wall minute; settle stays within the frame budget |
| **M4** | Jump sounds, landed with D39's S6 (clock policy) | the clunk lands on the jump's frame on the bench |

## 7. The one sentence to remember

Every hand and pointer names its motion (`sweep`, `step`, `jump`, and
`stop-to-go` or `drag` where they fit) as theme words that default to today's
behaviour, and a jump is an event the clock can hear.
