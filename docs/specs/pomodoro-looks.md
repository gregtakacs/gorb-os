# Pomodoro looks: eight timers and their chimes

The look design for `apps/pomodoro`'s editor (plan §64.3, §64.7, §64.8). This supersedes
`pomodoro-design.md` §10–§11's look notes. Written by Claude: per the owner's ruling, design and
audiovisual work is not delegated.

## Four rules every look keeps

1. **A timer, not a clock.** Each look is a timing instrument from its own world: a countdown
   readout, a printed ticket, a marine stopwatch, a drawing of a timer, a departure board, an
   hourglass, a chronograph, a flight log. No hour numerals, no hands, and no twelve-hour chapter
   ring. The orb's first app is a clock, and the pomodoro must never be mistaken for it.
2. **No scale that names minutes.** The OS ring drains over the phase's own length (25 min, 5,
   15), so any minute scale under it would be wrong at its tip. Furniture marks are neutral: four
   cardinals at most, unnumbered.
3. **Digits of one width.** The time is `mm:ss` and changes every second. The bake sets every
   digit in a cell as wide as the face's widest digit, centred, so no face (Playfair, Cormorant,
   Barlow) ever shifts the figure sideways. It is a property of the font bake (the kit's fonts
   step), not of a face that happens to have tabular figures.
4. **The rim is the ring's.** The OS ring is a band at radius 224–231 of the 233 glass.
   Furniture stays inside radius 200 (a 24 px moat), so the ring stands alone at the edge. No
   bezels, knurls or rims near the edge.

The rows are `pomodoro-design.md` §10.1's, unchanged:
- the big time (`mm:ss`), the phase name, the count (`3 of 4`) and the dots row;
- the state line (`Paused`, `Waiting`);
- the header row, which every look has, New included. It carries the look's quiet caption, and
  the press-and-turn choice replaces it while choosing.

## The eight

Grounds, faces and inks are the same-named look's, taken from radar, weather and flights by value.
The furniture, the header caption and the chime are the pomodoro's own.

| # | Look | The instrument | Furniture (all inside r 200) | Header caption | Dots | Ring colours (focus · short · long) |
|---|---|---|---|---|---|---|
| 1 | **Scope** | a phosphor countdown readout | a faint graticule (crosshair and two unlabelled range circles), scan-line texture | `T−` | square blips | phosphor green `#4BE38A` · amber `#E0B040` · cyan `#5AD1E6` |
| 2 | **Paper** | a printed timer ticket | a tear-off ticket: a perforated rule top and bottom, a stamped `No.` box for the count | `TIMER TICKET` | inked rounds | tomato `#C8401E` · sage `#5E8C4A` · ink blue `#2F5B8C` |
| 3 | **Brass** | a marine stopwatch | an engraved inner dial plate (r 150), a crown and pusher at the top edge of the plate, four cardinal pips | `CHRONOMETER` | brass studs | warm brass `#E0A43C` · verdigris `#4FA38A` · deep sea `#3A6EA5` |
| 4 | **Blueprint** | a drawing of a timer | a dashed construction circle, a dimension line under the time with arrowheads, a title block (`POMODORO · PHASE · SHEET n OF N`) | `DWG 25-MIN` | drafted squares | drafting yellow `#FFE07A` · white `#EAF4FF` · light blue `#8EC5FF` |
| 5 | **Station** | a departure board countdown | the time on black split-flap tiles with a hinge line, a red platform bar, the phase as a destination line | `DEPARTS IN` | black discs | signal red `#E3000F` · green `#1E8E3E` · navy `#1F4E8C` |
| 6 | **Celestial** | an hourglass of stars | a constellation hourglass outline behind the time, a star field | `ANNO HORAE` | stars, gold when filled | gold `#FFD27A` · lilac `#B9A8FF` · teal `#62D2C6` |
| 7 | **Silver** | a chronograph stopwatch | a brushed inner disc (r 170), two pushers at 11 and 1 on the disc's edge, four cardinal bars | `SPLIT` | machined rings | ice blue `#7FC8FF` · silver `#D6DCE4` · steel blue `#5B7DA8` |
| 8 | **Sectional** | a flight log | a magenta checkpoint box around the time on chart grid, a leg line across the glass | `ETE` | chart squares | magenta `#C2187A` · chart green `#2E7D32` · chart blue `#2A5DB0` |
| + | **New** | none | none: every row present, nothing drawn | `POMODORO` | rounds | Ruby `#D62828` · Emerald `#0FA36B` · Sky `#2E93E4` |

Ring colours are baked into the theme's values (`pm_values_t.ring_rgb`). Each look picks colours
that read on its own ground, as well as over the other apps, at the default 85 % opacity.

## The chimes (generated, kit sound pane)

The kit's sound pane generates each chime from a recipe: synthesized partials and envelopes,
as the clock's `sound-engine.js` makes its voices. Each chime stays above 350 Hz for the 2 W
speaker. Each look has one chime, used at a focus's end, with a brighter variant for a break's
end, so the ear knows which phase just ended. An author can pick any chime for any phase, or
upload their own.

| Look | Chime | Recipe (summary) |
|---|---|---|
| Scope | sonar ping | a 1.2 kHz sine with a fast attack, a 0.9 s decay and a quieter echo at 180 ms |
| Paper | kitchen bell | a struck bell: partials at 2.0, 4.8 and 7.1 kHz, 1.2 s ring |
| Brass | ship's bell | inharmonic partials over 620 Hz (1 : 2.0 : 2.4 : 3.0), 1.8 s ring, two strikes |
| Blueprint | drafting beep | two short square-ish beeps at 1.6 kHz, soft-edged |
| Station | platform chime | three tones, G5–E5–C6, each a soft sine-plus-octave bell |
| Celestial | glass bowl | a sustained 880 Hz with beating partials (880 and 883 Hz) and a shimmer at 2.6 kHz |
| Silver | stopwatch click and ping | two mechanical clicks (noise bursts, band-passed at 3 kHz), then a 2.4 kHz ping |
| Sectional | cabin chime | a two-tone high–low (1.05 kHz, then 830 Hz), a sine with a little second harmonic |
| New | simple tone | an 1 kHz sine, 0.4 s |

## What the editor offers (via the kit)

- The start sheet with the eight looks plus New, from the kit.
- The steps: **Ground**, **Furniture** (the look's instrument parts, each can be hidden),
  **Words** (three faces, sizes, inks), **Count** (dots), **Ring** (three colours, previewed on the
  look's ground *and* over a busy app, so a low-contrast pick is visible before it ships),
  **Chimes** (the kit's sound pane: the generated set, the owner's uploads, a chime per phase
  end).
- Views: focus, short and long; running, paused, waiting and choosing; `1 of 4` to `4 of 4`.
