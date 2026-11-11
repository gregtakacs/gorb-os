# Overlay design: every theme carries the colour the OS draws its overlays in

Design slice for `docs/app-platform-plan.md` §64.9, second bullet. Issue #11. Chain
`docs/specs/overlay-chain.json`, base `RING_TIP` (the ring's three steps land first: this slice
adds a field to the ring record and a caller for it, so it has nothing to attach to until
`main/display/orb_ring.c` exists).

## 0. The rulings that bind this slice

- §64.9 (owner, 2026-11-11): **every theme carries an overlay colour** — the colour the OS draws
  its overlays in over that app. Each app's editor sets it with the kit's colour picker, and
  **each look's default is its own accent**. The OS reads it from the theme the app on the glass
  wears. "Match the app" draws the ring in it; the copy arc and §55's blips may use it too.
  Every themed app: clock, radar, weather, flights, pomodoro. **A theme without one (baked before
  this) falls back to the overlay's own colour.**
- pomodoro-design amendment H: "The app sets the record's match flag, **which the overlay slice
  adds**. Until that exists on the base, 'Match the app' behaves as 'From the look'." This slice
  adds the flag and the substitution.
- The three-way rule (§52): a FORMAT 3 field is written by `tools/orbbundle.py` **and**
  `baker@1/orbtheme-baker.js`, read by `main/art/orb_bundle.c`, and pinned by
  `tools/host_orb_bundle_c_test.py`. Those four move together or the field is a lie.
- The ring's rules this slice must not break (ring-design): the record stays 16 bytes and equal to
  the SDK's `orb_ring_t`; the core stays host-compilable; the copy arc wins the rim.

## 1. What the slice adds, in one paragraph

One 32-bit word in the theme container — the geometry index head's second word, which every
writer has written as zero and every reader has ignored since FORMAT 3 shipped — becomes the
theme's overlay colour. The four editors gain one field each (the kit's colour picker, defaulting
to that look's accent). The device's bundle reader carries the word into the live theme, and the
ring's glue substitutes it into the winning record's arc and track — keeping their alphas — when
that record asks to match the app. Nothing else in the OS consumes it yet; the copy arc and §55's
blips are named, not built (§9).

## 2. Where the colour lives, and why there

FORMAT 3's header has **no free word**. All 32 are spoken for (0 magic, 1 format, 2 geometry ABI,
3 header size, 4 geometry offset, 5 geometry size, 6 geometry CRC, 7/8 art size+CRC, 9 section
count, 10 bake time in seconds, 11 serial, 12 bake time in milliseconds, 13 art offset, 14..27
the name, 28 the header CRC, 29..31 the design pointer), and `main/art/orb_bundle.c` carries
`_Static_assert(W_WORDS == 32)`, so a header that grows is a deliberate act. Word 12 is
quasi-vestigial in FORMAT 3 — word 10's second-resolution stamp superseded it, because word 12
wraps every 49.7 days and cannot order two bakes — but the shipped writers still put real
millisecond stamps there, so repurposing it would make some shipped themes read as carrying a
colour they never had. That is the one thing this field may not do: §64.9 requires that a theme
baked before this falls back, and a fallback is only honest if "no colour" is the state every
existing file is already in.

The placement that satisfies that is the **geometry index head**, the first two words of the
region the header points at:

| | |
|---|---|
| `region + 0` | `n_sections` — read by every reader, cross-checked against the header's word 9 |
| `region + 4` | was `0`, is now `0x0TRRGGBB`: tag `0x00` = **no overlay colour**, tag `0x01` = **present**, RGB in the low 24 bits |

Writers today: `tools/orbbundle.py:pack_index` (`struct.pack("<2I", len(recs), 0)`), and the
browser baker's `packIndex3` (`orbtheme-baker.js:3025`). Readers today: `orb_bundle.c:parse_core`
(reads `b32(region)` and nothing else, :422), `orbbundle.py:unpack_index` (binds the second word
to `_rsvd` and drops it), and the hub's `indexSlugs` in `internal/orbnet/themecontainer.go`
(checks `head != n`, ignores the rest). No reader refuses a nonzero second word, so **the field is
additive for every shipped binary**: an old dial installs and draws a new theme exactly as it does
now, ignoring the colour.

Why this word and not the alternatives:

- **Inside the region, so it is protected.** The word sits under `geom_crc`, which every door
  verifies before the index is believed (`parse_core` checks the region CRC first, :416). A torn
  or forged overlay word is a checksum refusal, not a wrong colour.
- **It reaches the OS unchanged.** The installer writes the region verbatim to the slot
  (`orb_theme.c:2078`); the rebase that moves the banks rewrites only record bytes 24..27
  (`orb_bundle.c:606-613`); bind reads the region and parses it (`adopt_bundle3`, :3002). The
  design pointer is the opposite case — those bytes are never installed and "the store, the slot
  and the dial never read it" (`orb_bundle.h:126-129`) — so a colour there is invisible to the
  reader that needs it.
- **Layout-free.** The other candidate was the section's own values blob (`values_off`, read by
  the app through `orb_stage_values`). That word would sit at an offset only the app knows, so the
  OS would need every app's values layout to fetch one colour, and each app's `VALUES_VER` would
  bump. The index head is the same word for every app and every look.
- **No format bump, which would be worse.** A FORMAT 4 header refuses every file the shelf and the
  owner's card already hold; a reserved word keeps every shipped theme valid, and old firmware
  reading a new theme — the direction that must keep working — is untouched.

One word per theme, not one per section: §64.9 says "every **theme** carries an overlay colour",
and a theme is a bundle. A multi-section bundle would therefore name one colour for several apps,
which is moot — such a bundle never binds (`orb_theme.c:sole_sec`, :3023), so the OS is never
choosing between two sections' colours.

The tag byte is a tag, not an alpha. The alpha that draws is the surface's own (the ring keeps the
app record's alphas, §6). `0x02` and above are unassigned, and a reader treats any tag that is not
`0x01` as absent rather than guessing, so a later meaning can never be read as a colour.

## 3. The field, stated for each of the four legs

- **Python writer.** `pack_index(recs, overlay=0)` emits `struct.pack("<2I", len(recs), overlay)`;
  `write_bundle(..., overlay=0)` passes it through and its default keeps every current caller's
  bytes identical. Add `index_overlay(region)` returning the raw word, and `file_overlay(path)`
  for the one-line read-back of a written container (Contract details both in overlay-1). `unpack_index`
  keeps returning `(n, recs)`: it is called in six places and its contract is the records, so a
  third return value is churn for its own sake. `pack_header3` and the header CRC are untouched —
  the word is inside the region, so it rides under `geom_crc` rather than under the header's.
- **Browser writer.** `packIndex3(recs, overlay)`, `writeBundle3` takes `o.overlay || 0`, stage3's
  `pack(m)` forwards `overlay: m.overlay || 0` (:3799) and `bakeClock3` forwards `overlay:
  o.overlay || 0` into it (:4338). Both baker paths — an app's `St.finish` and the clock's —
  therefore carry one field from the app's bake module to the byte.
- **C reader.** `orb_bundle_t` gains `uint32_t overlay;` beside `n_sections`; `parse_core` sets it
  from `b32(region + 4)` immediately after the count/cross-check that already sits there (the
  `memset` at :433 zeroes it on every refusal path first), and `orb_bundle_overlay(b)` next to it —
  the tag rule in one place, so no consumer invents its own. No new refusal is added: a bad tag is
  not damage, it is an absent colour, and damage inside the region is already the CRC's job.
- **Sentinel suite.** `tools/host_orb_bundle_c_test.py` gains the vectors §8 names. It already
  feeds the C reader the writer's own bytes and dumps every field, which is exactly the shape this
  needs; `--mutations` proves a dropped read cannot pass.

The hub's Go reader does not change in this slice: it reads the index head only to count sections,
and it has no UI that would show a swatch. Showing each library theme's overlay colour next to its
name is a §10 suggestion, not a requirement — nothing in §64.9 asks the hub to display it, and a
reader that ignores the word is still correct.

## 4. The device: the live theme's overlay colour

`orb_theme.c` carries the word from the parse into the live theme and answers it for the OS:

```c
/* orb_theme.h — the live theme's overlay colour (§64.9), 0x01RRGGBB; 0 when the theme names
 * none: a theme baked before this, the compiled-in pair, or a refused read. 0 is not a colour. */
uint32_t orb_theme_overlay(void);
```

- `orb_theme_geo_t` gains `uint32_t overlay;` beside `values_off`. The region is freed as soon as
  the parse and the map are done (`adopt_bundle3`'s `heap_caps_free(region)`, :3060 — "the parse's
  copy dies with the map"), so the word must be copied out to a scalar exactly as `values_off` and
  `blob_off` already are: `const uint32_t overlay = orb_bundle_overlay(&b);` where the parse's
  result is still live, and `s_geo.overlay = overlay;` with the other `s_geo` fills (:3106-3131).
  The compiled-in pair and every refusal path leave it 0, which is the fallback.
- `orb_theme_overlay()` returns `s_geo.from_container ? s_geo.overlay : 0`, masking the tag so the
  caller cannot see an unassigned tag either: it answers `0` or `0x01000000 | rgb`. One word, no
  allocation, safe to call from the LVGL task — it reads a static the bind wrote before the theme
  went live, the same discipline as `orb_theme_geo()`.
- A theme change under a standing ring is the interesting case, and the OS already has a 2 Hz
  pulse to notice it in: §6's service compares the value each tick.

## 5. The ring's match flag: the record's reserved byte

pomodoro-design amendment H defers the flag to this slice. The record has two reserved fields, and
byte 1 (`orb_ring_rec._r0`, SDK `orb_ring_t._res0`, documented "reserved, 0") becomes it:

```c
#define ORB_RING_MATCH 0x01u   /* arc/track colours come from the live theme's overlay colour */
typedef struct { uint16_t fraction; uint8_t effect; uint8_t match;
                 uint32_t arc, track; uint16_t period_ms; uint16_t _r1; } orb_ring_rec;
```

`main/display/orb_ring.h`, `apps/sdk/orb.h` and `apps/README.md` all restate the record, so all
three change in one step and the `_Static_assert`s keep them 16 bytes. The offset and the size do
not move, so: `imp_ring_set` (which `memcpy`s 16 bytes verbatim after `m3ApiCheckMem`) needs no
change, `binds.json`'s `i(i)` signature is unchanged, and `orbsdk.py`'s size gate has nothing new
to refuse.

The reserved byte was chosen over a bit in `effect` (the other free space) for two reasons.
`ring_clamp` validates `effect` against three known values and three separate consumers read it
(`ring_effect_alpha`, `effect_name`, and the service's `cur.effect != ORB_RING_STEADY` test), so a
flag bit there silently degrades `ORB_RING_FLASH | 0x80` to *steady* on any reader that forgets to
mask — the failure is invisible and the state the app was signalling is lost. A reserved byte is
inert: an app that sets `match` on firmware without this slice keeps its own arc and track colours,
which is precisely amendment H's stated interim behaviour. `ring_clamp` normalises the byte to 0 or
1, so an app that writes garbage into a reserved field does not get a second meaning out of it.

## 6. The substitution, and where it runs

One pure function, so the rule has a host test and the glue stays three lines:

```c
/* Fold the live theme's overlay colour into a picked record (§64.9): when the record asks to
 * match the app and `overlay` names a colour (0x01RRGGBB; anything else does nothing), arc and
 * track keep their own alphas and take this RGB. Alpha 0 still draws nothing — a record the app
 * muted is not unmuted by a theme. Idempotent, so a caller may apply it every tick. */
void ring_apply_overlay(orb_ring_rec *r, uint32_t overlay);
```

It runs in `orb_ring_disp_service`, in the LVGL task, right after `ring_pick` and before `s_rec` is
updated — never in the stamp, which must stay a per-pixel blend with no policy in it. The service
already wakes every `ORB_RING_STEP_MS` (500 ms), so it re-reads `orb_theme_overlay()` each tick and
holds the last value in a static `s_ovl`; when the theme's colour changed while a ring stood — the
owner swiped to another app, or a new theme landed under it — it re-marks the annulus (the existing
`mark_band()`), so a stale colour cannot sit on the glass for longer than one grid step. The
`/status` line is unchanged: it names the owner, the fraction and the effect, and no colour today.

The copy arc keeps winning the rim (`orb_edge_arc_shown()` gates the app ring's dispatch, ring-2
§8) and keeps its own colour: §64.9 says the overlay colour *may* serve the copy arc too, which is
a later decision, not this slice's (§9).

**Fallback, spelled out.** No overlay colour in the live theme → `ring_apply_overlay` does nothing
→ the ring keeps the colours the app set, which is what "Match the app" means when the app cannot
be matched, and what amendment H already promises until the field exists. The alternative reading
of "falls back to the overlay's own colour" — the OS picking its own hue — would put an
unrequested colour over an app that asked for none, and the OS has no overlay colour of its own
today: each surface brings its own.

## 7. The editors: one field, its look's accent as the default

The design key is `overlay`, a `#rrggbb` string, optional. The **bake always writes a concrete
colour** — the owner's value when the design carries one, the look's accent when it does not — so
the OS never needs to know what any app's look is, and a theme the owner never touched still
carries its accent. That is what makes §2's single word enough for five apps.

Each app's bake module owns the derivation, as one exported function of the same name, so the rule
has one home per app and its selftest can pin it:

| app | accent for a look | derivation | field's home |
|---|---|---|---|
| radar | `d.glass.ring`, whose preset value is `preset(d.base).glass.ring` | `overlayOf(d)`: `d.overlay` if a hex, else that | `ground` step |
| weather | the pack's accent role — the one `PACKS[p].roles` labels `Lightning` or `Accent` (nebula's is `orange`, pictogram's `red`, the other four `warn`) — so `d.icons.c[key]` | `overlayOf(d)`, keyed off the same `roles` read `weather-bake.js:216` already makes; fallback `#FFB23C` | `ground` step |
| flights | `d.words.accent`, whose preset value is `WORDS[d.scope.ground].accent` | `overlayOf(d)`; fallback `WORDS.scope.accent` | `scope` step |
| clock | the first exposed wheel comp's `accent`, else `d.handLume` if a hex, else `#ff2d55` | `WatchEngine.overlayOf(d)` | `dial` step |

Three of the four read a live design value first, so an owner's earlier change to the accent itself
— a different ring colour, a different bolt colour — moves the default overlay with it. The weather
needs the `roles` indirection rather than a hard-coded `warn` because two of its six packs have no
`warn` key at all (`PACKS.nebula` and `PACKS.pictogram`, :129-130), and the pack table already says
which of its colours is the accent.

The clock is the one look with no single accent field: `accent` is a wheel-comp property
(`watch-engine.js:1393`, canon at :5242) and the engine's own fallback when a wheel has none is
`#ff2d55` (:1498, and :3593 for a moon). `overlayOf` therefore reads the design the way the engine
does — first exposed wheel with an `accent` wins, then the lume colour, then the engine's constant
— rather than inventing a per-preset colour for 22 presets that §64.9 does not ask for. A preset
whose art is not an accent is then themed with the engine's own red, which is the colour the dial
already draws its laser indexes in. The rule is a function, not a table, so a new preset needs no
edit here.

The field itself is the kit's `Co` (`fieldKit()` in `studio@1/studio.js:641`), which is the picker
§64.9 names — palette tabs, hex draft, and the preset chip that says "Back to the preset's
#3ddc84". Radar's existing usage is the pattern to copy (`radar-editor.js:78`, through its local
`C()` helper): `K.Co(label, value, onv, '', path, { noAuto: true, preset: <accent> })`. `noAuto`
because there is no auto here — the accent *is* the preset — and `preset` so the chip can put the
look's colour back after the owner has typed over it. Each editor puts it under its own
`K.H('The OS’s overlay')` heading with one `K.Nt` line saying what it is for, in the step where
that app's look-wide colours already live: the two map apps on their `ground`, flights on `scope`,
the clock on `dial`.

Radar is deliberately not on its `overlay` step: that step is the app's own glass overlay — its
plate, its dot, its layout — and a field named "the OS's overlay" inside it would read as another
glass layer. Same reason the copy says *the OS's overlay* and never just *overlay*: the word
already means the boot overlay (`tools/host_boot_overlay_test.py`), the clock's crystal overlay
sprite (the baker's `overlayRec`), and radar's step above. §64.9 is the owner's wording and the
field keeps it; the strings the owner reads disambiguate.

The clock's `canonDesign` gains one line — `if (!isHex(d.overlay)) delete d.overlay;` — mirroring
`bezelInk` at :5114, so a half-typed hex never reaches a bake and an untouched watch still
serialises to the same design. The app bakes have no canon pass (only `radar-bake.js:scheme` and
the tone/label builders), so their `overlayOf` validates the hex itself with the same regex the kit
uses (`/^#[0-9a-f]{6}$/i`) and falls back to the accent; `Co` can hand back anything the draft box
held.

No design-schema bump is needed: `overlay` is an optional key with a derived default, so every
fixture in `apps/*/editor/_fixtures/` and every design the owner has saved opens unchanged and
bakes to its accent. The `version.txt` bumps in step 4 are there because the shipped bytes change,
which is what `tools/rebake_shelf.py` gates on.

## 8. The gate, as concrete checks

1. **Old files still read.** Every shelf theme's bytes are unchanged by step 1 (`pack_index`'s and
   `packIndex3`'s defaults are the same zero they wrote before), so
   `python3 tools/host_theme_format3_test.py` and `python3 tools/host_orb_bundle_c_test.py` pass
   with no fixture edited, and the four `tools/orb-manager/themes/*.orbtheme` files' checksums do
   not move until step 4 re-bakes them on purpose.
2. **The three legs agree on the new word.** One fixture theme baked with `overlay=0x01135c79`
   through `orbbundle.write_bundle`: the C reader's dump says that word, the browser baker's
   `verifyBundle3` on the same bytes reports it, and `index_overlay()` reads it back from the
   region. Mutants: drop the write (the writer emits zero) and drop the read (the C's dump goes to
   0). Both must bite under `--mutations`.
3. **A bad tag is absence, not damage.** `0x02rrggbb` and `0x00rrggbb` both install, parse, and
   answer 0; the theme still passes every existing check, and the suite pins that no new refusal
   sentence appeared.
4. **Substitution keeps alphas.** `ring_apply_overlay` over a record with arc `0x80FF0000` and
   track `0x3300FF00`, given `0x01123456`, answers `0x80123456` and `0x33123456`; with `match == 0`
   it changes nothing byte for byte; with `overlay == 0` nothing; with an arc alpha of 0 the arc
   still draws nothing; applying it twice is applying it once.
5. **Clamp owns the byte.** `match = 7` clamps to 1, `match = 0` stays 0, every other field is
   untouched, and the record is still 16 bytes on both sides of the SDK seam.
6. **Each editor's bake reads the colour back.** `python3 apps/<app>/editor/_selftest/bake_check.py`
   for the three map apps (that file already reads the theme back through `orbbundle` and
   `theme_schema`); the clock has no `bake_check.py`, so its assertion goes in
   `_selftest/headless-bundle3.test.js`, which `make check-editor` runs, and its shelf driver
   `bank3-drive.html` gains the option. Each checks: the region's second head word is the accent when
   the design sets nothing, and the owner's colour when it does.
7. **The shelf is fresh after step 4.** `make rebake`, then `python3 tools/rebake_shelf.py --check`,
   then a read-back of all four shipped themes: a `0x01` tag and that default's accent each.

## 9. What this slice does not build

- **The pomodoro's own overlay colour.** §64.9 lists the pomodoro among the themed apps, and the
  container field is ready for it the day the app has an editor: it is one `overlayOf` and one
  `Co` field in the `pomodoro-editor` slice (pomodoro-design amendment I), whose default is the
  look's own accent from `docs/specs/pomodoro-looks.md`. This slice does not touch
  `apps/pomodoro/`, which does not exist on the base yet.
- **The copy arc and §55's blips reading the colour.** §64.9 says they *may* use it. The accessor
  in §4 is the whole contract they need; each surface decides later whether to take it, and the
  copy arc's own colour is currently its `0x66DF`/`0x3186` pair.
- **An OS-wide default overlay colour.** The fallback is per-surface (§6), so there is no NVS key,
  no settings row and no hub control for one.
- **A hub swatch**, as §3 says.
- **`/api/v1/status`'s ring line**, which names no colour today.
- **Round-tripping the colour through a design file.** An exported `.orbdesign` carries `overlay`
  as any other design key, and re-baking it lands the same word; nothing special is needed.

## 10. Suggestions (not in this chain)

- Show each library theme's overlay colour in the hub's theme row: `themecontainer.go` would
  return the head's second word beside each slug, and `tabs.js` paint a 12 px square. Cheap, and it
  tells an owner why two themes' rings differ — but nothing in §64.9 asks for it.
- A preview of the ring in each editor's preview pane, in the overlay colour, so the field's effect
  is visible where it is set. The editor already draws the whole glass (§60); the ring is not part
  of the bake, so this is a preview-only overlay and needs its own design.
- The boot overlay could take the theme's colour when it names one; today it has its own.

## 11. Slices → steps

Four steps, `docs/specs/overlay-chain.json`, base `RING_TIP`. Three of them are `hard`: the container
field, everything after it reads bytes only it can produce; the firmware glue, because a wrong
substitution is a visible OS behaviour the pomodoro chain depends on; and the editors, because a
field that derives nothing ships a black ring. Step 4 is the one soft step: its gate is the shelf's
own, and it cannot be half-done.

| step | what | depends on |
|---|---|---|
| `overlay-1-container-field` | the word: Python writer + `index_overlay`, JS baker's three call points, the C reader's field, the sentinel vectors | RING_TIP |
| `overlay-2-firmware-ring` | `s_geo.overlay`, `orb_theme_overlay()`, `match` in the record (SDK + firmware + README), `ring_apply_overlay`, the service's re-apply | 1 |
| `overlay-3-editor-fields` | `overlayOf` × 4, the `Co` field × 4, the clock's canon line, each app's bake forwards `overlay` | 1 |
| `overlay-4-ship` | four `version.txt` bumps, `make rebake`, the shelf's read-back | 2, 3 |

Step 2 does not depend on step 3 (the OS reads whatever a theme says), and step 3 does not depend
on step 2 (an editor's field is a byte, not a behaviour): they are independent legs off step 1, and
step 4 is where they meet. The ring code paths step 2 names — `main/display/orb_ring.c`,
`tools/host_ring_test.py`, `apps/sdk/orb.h`'s ring section — exist at `RING_TIP` and are absent
from `pomodoro-base`, which is why the chain's base is the tip and not the branch.

## Owner questions

1. **Unassigned tags.** §2 reserves every tag but `0x00`/`0x01` and treats anything else as absent.
   The alternative is a strict reader that refuses a theme whose tag it does not know; the
   three-way rule's whole history is that refusing is worse than ignoring. Assumed: ignore.
2. **"Falls back to the overlay's own colour."** Read as "the surface keeps the colour it brought"
   (§6), not "the OS invents one". If the owner means a single OS-level default that the ring shows
   over an unlabelled theme, that is a setting, and settings are the pomodoro's (§64.7/§64.9), so
   it would arrive as `ring_col_away`'s third special rather than as a hidden constant. Assumed as
   written here.
3. **The clock's accent.** §64.9's "each look's default is its own accent" has no field to read for
   20 of the clock's 22 presets, so §7 derives it (wheel accent → lume → `#ff2d55`) rather than
   hand-picking 22 colours the owner has not seen. If a per-preset accent is wanted, it belongs in
   `WatchEngine.PRESETS` as data, and `overlayOf`'s first clause becomes the only line that changes.






## Orchestrator amendments (2026-11-11) — these override the sections above

1. **The reserved byte is `flags`, not `match`** (owner: the ring also takes a direction and a
   start, plan §64.8). The record becomes:

```c
#define ORB_RING_MATCH 0x01u   /* arc/track take the live theme's overlay colour (§64.9) */
#define ORB_RING_CCW   0x02u   /* the arc runs anticlockwise from its start (§64.8)      */
typedef struct { uint16_t fraction; uint8_t effect; uint8_t flags;
                 uint32_t arc, track; uint16_t period_ms; uint16_t start; } orb_ring_rec;
/* start: the angle where the arc begins, 0..359 degrees clockwise from twelve; >359 wraps */
```

   - `ring_clamp` keeps only the known bits of `flags` (masks to 0x03) and wraps `start` mod 360.
   - The stamp's span maths (ring amendment 1: spans per row, one boundary per pick) takes the
     start angle and the direction: the arc covers `[start, start + frac·360)` clockwise, or
     `[start − frac·360, start]` with CCW. The track is the rest.
   - The host suite's per-pixel reference proves it for every start in {0, 90, 180, 270, 45, 359}
     × both directions × the fraction sweep, within the same 1 px boundary allowance.
   - Mutants cover the CCW branch and the start offset.
   - This is step **overlay-2-firmware-ring**'s, in the same change as the match substitution.
     `orb.h`, `orb_ring.h` and `apps/README.md` restate the record with both fields, still
     16 bytes.
2. Owner questions 1–3: accepted as assumed.
3. The base stays `RING_TIP` until the ring chain lands; the orchestrator sets it.
