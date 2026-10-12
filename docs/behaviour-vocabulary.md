# Behaviour vocabulary — what the OS registers, and what it refuses to

Draft 2026-10-12 for the owner's signature as D15. The owner's steer of
this session set its shape: **"I didn't think we were going to expose any
of it, only time."** This paper works that out to the ABI line. §8's P0 row
and §9.2 point here; when the owner signs, the ABI freezes and P3 may
start. A rejected reading — v1 of this document proposed the broker derive
calendar values for apps; that idea is dead as proposed.

## 1. The registered surface, complete

The OS hands apps exactly what §4.3's `orb_svc_t` already spells — no
source name among them:

| Entry | Grants | Notes |
|---|---|---|
| `now(app, sec, frac, synced)` | capability `time` (free, always on) | the extraction of `compose_now` (`clock_view.c:1711-1769`): SNTP/`CLOCK_EPOCH` resolution, the `orb_net_time_synced()` trust rule, the drift re-anchor — all already OS-owned (§1.1 Time service row) |
| `tz_label(app)` | `time` | string; the label only |
| `cfg_get` / `cfg_set` / `cfg_get_str` / `cfg_set_str` | `config` (always on, own namespace, declared keys, quota — the §9.4 number) | |
| `power_request(app, timeout_s, hold)` | `power` (owner-granted) | request, never a write; `idle_dim` owns the decision |
| `bank` / `layer_add` / `layer_set` / `layer_touch` / `layer_del` | `ui` (free) | the compositor primitives, §1.1's compiled C |
| `job_post` | free | the worker-task rule |
| `status` | free | |

The manifest's capability bitmask (§4.1) is the only behaviour declaration
the OS ever inspects. Geometry stays opaque to the OS — §4.1's own words —
so `src`/`kind`/`flags` values are never validated, refused or granted by
anything compiled into the image.

## 2. What the vocabulary is, now that the OS refuses it

The `SOURCES` / `KINDS` / `FLAGS` / `HOSTS` tuples and the aux drive kinds
(`clock_view.c` / `theme_schema.py` / `orb_theme.h:88-93`) freeze as the
**bake-to-app interface**: what Watch Composer and `bake_clock.py` emit,
and what the drawing app — the clock today, a second face app whenever —
interprets in its own code. Three consequences:

- The append-only rule (`theme_schema.py:52-53`) and the static-assert
  count-pins (`orb_theme.h:122`) stay load-bearing; they bind the bake and
  the app, and the host suites keep proving the two agree —
  `host_comp_math_test.py` lifts the C maths under gcc, `host_bake_comp_test.py`
  round-trips bake to `orb_theme_geom_parse()`.
- An unknown kind or source is the app's own bad row to survive: the
  validated-not-drawn discipline (`clock_view.c:1086-1091`) is vocabulary
  rules the clock app carries, and §12's ABI already lets a bad app hang
  itself without touching the OS.
- `behaviour_kind` in §4.1's manifest header is **reserved 0**, same
  discipline as `sig` (C3: reserved, signing later moves nothing). Nothing
  registers into it until something demands it.

## 3. The one contract that makes "only time" safe

If apps derive weekday/month/week themselves from a bare `time_t`, §1.1's
"one clock authority" quietly becomes twelve: two apps on one carousel may
disagree about what Saturday is, about ISO week boundaries, about the
moon's age at midnight. The fix is one entry:

**`now()` hands out the OS's converted local `struct tm`** — alongside
`sec`/`frac`/`synced` — exactly the pair `comp_value` consumes today
(`clock_view.c:1196`). The OS keeps the tz table, the DST rule and the
sync trust; an app that wants a frame index derives it from the OS's own
broken-down time. Derivation stays the app's; conversion stays the OS's.
## 4. Findings the inventory sweep surfaced — kept as bake/app notes

Reference table first: the shipped clock's derivations
(`comp_value`, `clock_view.c:1196-1218`) — the vocabulary's de-facto
contract for any app that draws these names. It binds nobody but itself;
it is recorded so a second face app can match the shipped art instead of
re-litigating it.

| Name | Domain | Derivation | Live input? |
|---|---|---|---|
| `none` | 0 | static frame 0 | yes |
| `weekday` | 0..6 | `(tm_wday + 6) % 7` — **Monday == 0**, matching `bake_clock._WEEKDAY_TILES` | yes |
| `month` | 0..11 | `tm_mon` — January == 0 | yes |
| `date` | 1..31 | `tm_mday`; the bake sets `value_min=1` (`bake_clock.py:2054`) | yes |
| `hour12` / `hour24` | 0..11 / 0..23 | `tm_hour % 12` / `tm_hour` | yes |
| `minute` / `second` | 0..59 | `tm_min` / `tm_sec`; sweep uses the OS frac, §3 | yes |
| `ampm` | 0..1 | `tm_hour >= 12` | yes; no bake word (§4 G1) |
| `week` | 1..53 | ISO-8601 Thursday rule — `comp_iso_week` (`clock_view.c:1168-1189`) is the reference | yes; no bake word |
| `day_of_year` | 1..366 | `tm_yday + 1` | yes; no bake word |
| `quarter` | **1..4** | `tm_mon / 3 + 1`; `theme_schema.py:74`'s "0..3" comment is wrong — G2 | yes; no bake word |
| `moon_age` | 0..29 | mean synodic month from epoch `MOON_NEW_MOON_S 947182440` (2000-01-06 18:14 UTC), period `MOON_SYNODIC_S 2551443`; steps once per whole day, never interpolated — G3 | yes |
| `battery` | 0..100 | no producer — the orb is wall-powered (`clock_view.c:1090-1091`); hides | no |
| `power_reserve` | 0..count-1 | hides at read; the shipped tourbillon's reserve beats are clock-app state (`s_bt_ctr`), not a service | no |
| `text` | per-char | the app maps characters to glyph rows from its own `config` string; `strip_indices` covers decimal digits only | partial — G4 |
| `value` | raw | the comp's own value; no input outside the app | app-owned |

- **G1 — four names have no `MANIFEST_SOURCE` word** (`week`,
  `day_of_year`, `quarter`, `ampm` — `theme_schema.py:311-328`): no kit
  can request them. A hand-written `.orbtheme` (any `src < 17` passes
  `orb_theme.h` bounds) still can. Keeping the firmware branches costs
  20 lines; deleting is the owner's option, not P0's gate.
- **G2 — `quarter`'s comment says `0..3`, the code returns `1..4`.**
  Harmless only because G1 kept it unreachable. The comment is the bug;
  fixed alongside this paper. A card whose theme ever carried `quarter`
  must be baked against `1..4`.
- **G3 — the moon is a mean month on a fixed epoch**, up to half a day off
  the real sky; a photo-realistic lunar tile set is inside the vocabulary,
  an astronomical one is not. Accepted where it was priced
  (`clock_view.c:1158-1164`); recorded so no future change "fixes" it and
  silently moves the sky on every card in the drawer.
- **G4 — `text` has no character contract.** `strip_indices`
  (`theme_schema.py:405-422`) reduces digits and `% count`-wraps anything
  else into a valid-but-wrong glyph. Since the OS no longer grants sources,
  the missing-glyph choice belongs to the drawing app; the recommendation
  to face apps is the shipped strips' real charset, `[0-9.-]`, refused at
  the knob. The OS keeps `cfg_set_str` a length-capped byte pipe.
- **G5 — the aux drive kinds and `HOSTS` are wire enums too** (rotating /
  retrograde / indexed pointer; `dial` / `subdial` / `custom`). §9.2's
  wording named only SOURCES/KINDS/FLAGS. They freeze with the same
  append-only rule, at the same bake/app layer.

## 5. Sign-off block — D15

The owner signs this paper by answering **Q1**. Q2 is decided by precedent
(`sig`, `COMP_FIELDS.reserve`) and listed for confirmation, not debate.

| Q | Question | Recommendation |
|---|---|---|
| **Q1** | Does `orb_svc_t.now()` hand out the OS's converted local `struct tm` (plus `sec`, `frac`, `synced`), keeping conversion OS-owned while every source derivation stays app-side? Or only the raw `time_t`, leaving each app its own tz maths? | hand out the tm — §1.1's "one clock authority" otherwise dies politely, two dials can disagree about Saturday, and the tz table is already OS code |
| Q2 | §4.1's `behaviour_kind` ships reserved 0? | yes — same discipline as `sig` and `COMP_FIELDS.reserve` |

What signing closes: §8's P0 row (the last gate), §9's item 2, and the ABI
freeze of `orb_svc.h`'s time entry. P3 then implements §4.3's table
verbatim — broker, grants, prompts, fault counter — with no vocabulary
question left open. What stays open by design: per-app NVS quota (§9.4, a
P3 number) and the wasm3 tier (P7).

