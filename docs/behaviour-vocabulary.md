# Behaviour vocabulary — the P0 signable table

Draft 2026-10-12. This is the paper task of §9.2 and the last item holding
P0 closed: "the owner signs the vocabulary subset; no code before this line
closes" (§8's P0 row). It names what the OS registers — owns the derivation
of, and freezes — and what stays clock-app-private. The inventory is
`tools/theme_schema.py`'s `SOURCES` / `KINDS` / `FLAGS` (plus the aux-drive
kinds and `HOSTS`, which §9.2's wording forgot — see §5 and G6 below). The
firmware mirror is `main/art/orb_theme.h:88-93`.

## 0. What "registered" means here

Registered = the OS owns the value's derivation; the broker hands it to any
app; the bake owns the frame layout across the value's domain; both sides
implement one published contract, and a host mirror proves they agree —
`host_theme_test.py`'s pack-here-parse-THERE discipline, extended to values.

- An app may not recompute a registered value for itself. Every app on the
  glass then agrees which day is Saturday.
- A value the OS does not own stays the app's problem: the OS refuses the
  source at grant time, logs it, and the element hides — today's
  validated-not-drawn discipline (`clock_view.c:1086-1091`), which is also
  how P3's pass line "a denied app degrades visibly" will be met.
- The wire is already frozen by `theme_schema.py:52-53`: index order is the
  value, appending is allowed, reordering is an ABI break, and
  `orb_theme.h:122` static-asserts the count against the tuple. Registering
  or de-registering a name here changes no wire value.
- The caps are ABI too: `CLK_COMP_MAX` 16, `CLK_AUX_MAX` 8, `CLK_BANK_MAX`
  256, `CLK_STRIP_GLYPHS` 8. Growing one bumps `GEOM_ABI` (C4's refuse rule,
  §4.1); it never silently reorders an enum.
- Time semantics are OS-owned: every registered time-derived value comes
  from the ONE `struct tm` + `time_t` pair `compose_now` samples
  (`clock_view.c:1191-1196`), after the OS tz table
  (docs/sntp-timezone-plan.md). An app never supplies its own time to a
  registered derivation. The names below match the firmware's `CLK_SRC_*`
  exactly; the broker re-exports them as `ORB_SRC_*` in `orb_svc.h`, same
  indices, and the host mirror pins that equality.

## 1. Registered: the time service (capability `time` — free, per §4.2)

Thirteen sources. The derivation is the firmware's today — `comp_value()`
(`clock_view.c:1196-1218`) lifts into the broker unchanged; P3's
`orb_time_now()` and these become the `time` surface's whole ABI.

| # | Name | Domain | Contract (pinned by host mirror) |
|---|---|---|---|
| 0 | `none` | 0 | static; always frame 0. A blank record stays inert |
| 1 | `weekday` | 0..6 | **Monday == 0**: `(tm_wday + 6) % 7`. Matches `bake_clock._WEEKDAY_TILES` |
| 2 | `month` | 0..11 | `tm_mon`; **January == 0**. Matches `_MONTH_TILES` |
| 3 | `date` | 1..31 | `tm_mday`. The bake sets `value_min=1` (`bake_clock.py:2054`) |
| 4 | `hour12` | 0..11 | `tm_hour % 12`. 12:30 am reports 0 |
| 5 | `hour24` | 0..23 | `tm_hour` |
| 6 | `minute` | 0..59 | `tm_min`; sweep phase is the scheduler's, G5 |
| 7 | `second` | 0..59 | `tm_sec`; same sweep-phase note |
| 8 | `ampm` | 0..1 | `tm_hour >= 12`. The bake cannot reach it today — G1 |
| 9 | `week` | 1..53 | ISO-8601: week 1 holds the year's first Thursday; `comp_iso_week`'s boundary forensics (`clock_view.c:1168-1189`) are the contract, verbatim. Bake-unreachable today — G1 |
| 10 | `day_of_year` | 1..366 | `tm_yday + 1`. Bake-unreachable today — G1 |
| 11 | `quarter` | **1..4** | `tm_mon / 3 + 1`, so Q1 == 1. `theme_schema.py:74`'s "0..3" comment is wrong; this ruling fixes it — G2 |
| 12 | `moon_age` | 0..29 | Mean synodic month, whole seconds: epoch `MOON_NEW_MOON_S 947182440` (2000-01-06 18:14 UTC), period `MOON_SYNODIC_S 2551443`. The age steps once per whole day, never interpolated. A true-astronomical phase is NOT the contract — G7 prices the mean month |

## 2. Registered: the power service

| # | Name | Domain | Contract |
|---|---|---|---|
| 13 | `battery` | 0..100 | Per cent, full = 100. **No producer on orb hardware today** — the orb is wall-powered (`clock_view.c:1090-1091`): `comp_value` returns false, the element hides. The OS registers the name so a battery-bearing future board lights every existing theme with no re-bake |
| 14 | `power_reserve` | 0..count-1 | "Across the reserve's own range" is not one contract until the owner says whose range it is — **Q2**. Today unreachable (`comp_value` false); the shipped tourbillon's reserve beats are clock-app fiction driven by `s_bt_ctr`, not by this source |

## 3. Registered: the config service (capability `config`, type `STRING`, C9)

| # | Name | Domain | Contract |
|---|---|---|---|
| 15 | `text` | one glyph index per character | The owner-set string lives in the app's own NVS namespace under the manifest's declared setting; the OS stores and returns it, the app maps characters to glyph rows. **The charset is the open question** — G3: `strip_indices` (`theme_schema.py:403-420`) reduces decimal digits only |

## 4. Not registered — app-private by ruling

| # | Name | Why the OS refuses it |
|---|---|---|
| 16 | `value` | "raw: index == the comp's own value, for bake-decided art". There is no OS-owned input for it — the value comes from the app's own state. An app drives it through its own code; the OS guarantees only the registered frame maths (§5) maps whatever arrives |

## 5. Registered: the composition vocabulary (capability `ui` — free, per §4.2)

The compositor primitives stay compiled C in the OS (§1.1); these names are
their ABI surface. Drawing status is today's, from `clock_view.c:1086-1091`:
the **validated-not-drawn** discipline hides any row whose kind the board's
compositor cannot draw, logs it once, and never corrupts the dial.

Comp kinds (`KINDS` — how a comp's frames are laid down):

| Name | Drawn today? | Note |
|---|---|---|
| `window` | yes | one tile per value |
| `strip` | yes | per-glyph composition via `strip_indices` |
| `ring` | not yet | no shipped bake requests one; registering it commits the OS to draw it before any app ships one |
| `flare` | not yet | same |

Aux drive kinds (AUX's `kind` field: `0` rotating hand, `1` retrograde, `2`
indexed pointer — a wire enum exactly like `KINDS`, and one §9.2's wording
missed): **all registered**. The date hand, sub-dial seconds and retrograde
sweep arrive as data in today's shipped kits — that is the proof they are
platform vocabulary, not clock trivia.

Flags (the ten `FLAG_*` bits; frozen as a set, append-only like the enums):

| Name | Live today | Note |
|---|---|---|
| `UNDER_DIAL` | parsed only | opaque bake-composited tiles not drawn (`clock_view.c:1088-1090`); the bit also steers `window_bytes`, so it is wire-load-bearing |
| `ZERO_PAD` | live | |
| `REVERSE` | not exercised | ring/strip direction; no shipped bake uses it |
| `JUMP` | live | aux hand snaps to whole units — the kit ASKING for the mechanical cadence |
| `24H` | live | `MANIFEST_HAND hour24` sets it |
| `WRAP` | live | past `value_max` keeps sweeping |
| `BLINK` | parsed only | every-other-second draw; belongs to the scheduler's beat (§4.3), never a per-app timer. No shipped bake uses it |
| `DISABLED` | live | parse, validate, do not draw — the owner's off switch |
| `AUX_SHADOW` | live | the blur/offset twin of the hand before it, same tag+src |

Hosts (`HOSTS` = `dial` / `subdial` / `custom`): registered. They select the
aux record's anchor (`host` field: 0 = main dial centre, else a comp tag)
and, with `COMP_FIELDS.first`, the window z-bucket of a subdial-hosted
window.

The frame maths itself is registered with the kinds: `frame_index` and
`strip_indices` (`theme_schema.py:390-420`, mirrored verbatim at
`clock_view.c:1106-1158`) — clamp below zero, `% count` wrap at `count`,
half-up rounding at `scale` x16. An app that received a registered value has
no say in how it maps to a frame; the compositor maps it identically for
everyone.

## 6. What this table buys P3 (the first registered set)

P3's pass line is §8's: `/api/v1/status` shows per-app grants, call counts
and fault count; a denied app degrades visibly. The minimum broker that
satisfies "a second app can tell the time": §1's thirteen rows, the
refuse-with-reason path for §4 rows, and `text` stored and returned by
`config`. That set — rows 0-12 plus 13's name plus `text` — is what the
owner signs; it is a superset of what anything shipped today consumes (kit3
uses only hour, minute, flare, subdial, opening).

## 7. Findings — what the inventory check surfaced

- **G1 — four sources are firmware-only.** `week`, `day_of_year`, `quarter`
  and `ampm` have no `MANIFEST_SOURCE` word (`theme_schema.py:309-326`): a
  kit cannot request them. `comp_iso_week`'s twenty lines of boundary
  forensics are unreachable from any bake, and a 12-hour dial cannot ask for
  an AM/PM indicator at all. Registering rows 8-11 makes adding the missing
  bake words a P3 deliverable; deleting the working firmware code is the
  alternative (Q1).
- **G2 — `quarter`'s documented domain contradicts its own firmware.**
  `theme_schema.py:74` says `0..3`; `clock_view.c:1209` returns
  `tm_mon / 3 + 1`, i.e. `1..4`. Nothing bakeable has hidden it (G1 keeps
  `quarter` unreachable). Since the bake-side writer never existed, the
  comment is the only thing wrong: the domain is ruled `1..4`.
- **G3 — `text` has no character contract.** `strip_indices` handles decimal
  digits; the source's description says "one glyph index per character" for
  an owner-set string. What a theme does with a character its glyph bank
  never baked is undecided — today's `% count` reduction wraps anything into
  a valid-but-wrong glyph. Proposal: the registered charset is `[0-9.-]`
  (what shipped strips can draw), and `config` refuses a string outside it
  at set time — the owner learns at the knob, not on the glass. Widening
  later is the append-only rule (Q3).
- **G4 — reading battery has no capability.** §4.2's `power` grants
  *requests* (dim timeout, wake hold), not *reads* (battery per cent).
  Moot today (no producer). Before any battery-bearing board exists, the
  row must say whether `battery` rides under `power` or joins the always-on
  surface. My recommendation: rides under `power`, clamped like its requests.
- **G5 — sweep is a scheduler promise, not a value.** `second`/`minute`
  hands sweep between units using the tick's wall fraction
  (`aux_value_1000`'s `frac`, `clock_view.c:1570-1585`). Registering those
  sources registers the scheduler's phase too: an app asking for `second`
  gets `tm_sec` plus the OS's sub-second fraction, or a third app's second
  hand ticks while ours sweeps — two beats on one dial, the exact drift the
  frame scheduler exists to prevent. `JUMP` stays the opt-out.
- **G6 — §9.2's wording covers only `SOURCES`/`KINDS`/`FLAGS`.** The aux
  drive kinds (`0/1/2`) and `HOSTS` are wire enums under the same
  append-only rule, and a hosted window's `first` field doubles as its
  z-bucket. §5 above covers them; signing this paper signs them too.
- **G7 — the mean-month moon disagrees with the real sky by up to half a
  day.** The bake lays 30 tiles; the firmware steps them once per whole day
  from a fixed epoch (`clock_view.c:1158-1164` already prices this). A
  photo-realistic lunar theme is inside the vocabulary; an astronomical one
  is not, and no flag makes it so. Recorded so nobody "fixes" the firmware
  derivation later — that would move the sky on every card in the drawer.

## 8. Sign-off block

The owner signs this table as **D15** by answering Q1-Q3; each answer lands
in the decision record (§0), and P0's freeze line closes. Until then this
document is a proposal and `theme_schema.py` stays the wire authority.

| Q | Where | Question | My recommendation |
|---|---|---|---|
| Q1 | G1 | Register `week` / `day_of_year` / `quarter` / `ampm` and add their bake words at P3 — or delete the unreachable firmware branches? | register all four; the ISO-week code is the exact thing no third-party app should ever re-derive |
| Q2 | §2 row 14 | Is `power_reserve` an OS projection (`battery %` scaled across the theme's `count`), clock-app fiction that stays private, or a held-dark name with no contract? | held dark; it has no input and the tourbillon that fakes one belongs to the clock |
| Q3 | G3 | Accept the `[0-9.-]` registered charset for `text`, refused at config-set time? | yes; widening is append-only, and a wrong glyph mid-date is the failure class this whole file exists to prevent |
