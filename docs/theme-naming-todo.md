# Theme naming: the card name, the baked slug, and the three caps that disagree

**Status: PARTIALLY FIXED — the firmware now refuses a mismatched file early, the
underlying cap disagreement is NOT fixed, and none of it is verified on hardware.**

Filed 2026-10-10. Everything below is a known outstanding item, not a bug hunt.

---

## 1. What happened

A baked `skeleton.orbtheme` (header slug `skeleton`, title `Skeleton`) was uploaded to
the card under the name `Skeleton`. Upload succeeded. The list showed a Load button.
Tapping it answered `could not stage Skeleton: that theme cannot be loaded`.

The refusal is `orb_theme_stage_load()`'s rule: the name a theme is filed under must
equal the slug baked inside it. `h_theme_load()` flattens every reason to one generic
string, so the actual sentence (`the file calls itself "skeleton"`) reached USB serial
only. The owner had no cable attached.

Already fixed, in `main/art/orb_theme.c`:

| Consumer | Behaviour now |
|---|---|
| `orb_theme_upload_end()` | Rejects before the rename, so the page reports `must be filed as "skeleton"` |
| `orb_theme_scan()` | Marks the row not-ok, so no dead Load button appears |
| `orb_theme_boot()` | Re-checks identity before flashing, so a swapped card cannot install |
| `orb_theme_stage_load()` | Delegates to the one helper |

All four reach the rule through `filed_as_its_own_slug()`. `tools/host_sd_test.py`'s
`naming_lint()` asserts the helper exists, compares `->slug` against the card name, and
is reached by all four functions. The mutation battery carries four regressions.

---

## 2. The real fix still owed: three different maximums for one name

The name has three independent length limits, and nothing reconciles them.

| Where | Limit | Source |
|---|---|---|
| Card filename accepted by the page | 31 | `ORB_SD_NAME_MAX`, `main/sd/orb_sd.h:39` |
| Slug stored in the container header | 24 | `NAME_MAX`, `tools/orbtheme.py:46` (`W_SLUG`, 6 words) |
| Slug `bake_clock.py` actually writes | 23 | `[: _ot.NAME_MAX - 1]`, `tools/bake_clock.py:1854` |

Consequence: **any card file named 24 to 31 characters can never load.** The upload is
accepted and `orb_sd_name_ok()` passes it, while `filed_as_its_own_slug()` compares it
against a 23-character field it can never equal. The new upload refusal at least says so.

Pick one and make it the only number:

- [ ] Decide the canonical cap. 23 is the only value all three layers can honour without
      widening the header, which costs a format change. Recommend 23.
- [ ] Make `orb_sd_name_ok()` reject a theme name over that cap, or document why the card
      layer stays looser than the format.
- [ ] Move the cap into one constant both `orb_sd.h` and `tools/orbtheme.py` pin against,
      so the three numbers cannot drift apart again. `host_sd_test.py` already compares the
      Python writer against the C reader field by field; add this to it.
- [ ] Make `bake_clock.py` refuse an over-long `--theme-slug` instead of truncating it.
      Silent truncation hands back a slug nobody asked for. It warns on a filename/slug
      mismatch today; it does not warn on truncation.

## 3. Case sensitivity is undecided

`strcmp()` is case-sensitive. FATFS lookups are case-insensitive on this build, so
`skeleton.orbtheme` and `Skeleton.orbtheme` name one file, while the identity rule treats
them as two themes. A re-upload under a different case will collide or mismatch unpredictably.

- [ ] Decide: case-sensitive identity plus a normalized upload, or `strcasecmp` with a
      documented rule.
- [ ] Normalize the slug at bake time if lowercase canonical wins.
- [ ] Check whether `f_rename()` can produce two rows differing only in case.

## 4. `h_theme_load()` still reports nothing specific

`main/net/orb_net.c:2331` answers 422 with the fixed string `"that theme cannot be loaded"`
for every distinct failure: damaged container, wrong format, mismatched name, no card. The
comment above it claims the list's `why` field already carries the reason. For the name case
it does not, until this row is done.

- [ ] Have `orb_theme_stage_load()` hand back its reason (out-param, or a per-file
      `s_stage_why` matching `s_up_why`'s one-writer lifetime rule) and put it in the body the
      way `h_theme_upload()` already does.


---

## 5. `h_sd()`'s JSON buffer is too small now that a good row can carry a `why`

`static char json[2300]` at `main/net/orb_net.c:2278` budgets "12 rows at the ~150 B each
can cost". A row with a full 31-char slug, a 40-char title and a 43-char name `why` escapes
to about 238 B. Twelve of those plus the ~300 B preamble is about 3.2 KB, over the bound.

Two problems, one of them worse than a truncated body:

- [ ] Raise the bound to `THEME_LIST_MAX` at real worst case, or cap the rows rendered.
- [ ] **The overflow check runs after the loop.** `snprintf()` returns the length it *would*
      have written, so once `off` passes 2300 the next iteration computes
      `sizeof json - (size_t)off`, which wraps to a huge `size_t`, and writes through
      `json + off`, past the buffer. Break on `off >= sizeof json` inside the loop.

Before the naming fix an ok row always had an empty `why`, so realistic worst case stayed
under the bound. That margin is gone. Treat this as part of the same change, not follow-up.

## 6. Verification debt

What is green here proves less than it looks.

- [ ] **Nothing in section 1 has run on a device with a card in the socket.** The bench has
      no card and no radio (see `## Bench has no WiFi radio` in `.clinerules`).
      `host_sd_test.py` lifts and lints shipped source; it does not execute scan, upload or
      install.
- [ ] Load with the correct name, confirm the dial draws it, then exercise the failure paths
      below on the same card.
- [ ] Upload a mismatched file, confirm the page names the required name and that no `.part`
      survives the refusal.
- [ ] Copy a file onto the card by hand under a wrong name, confirm the row reads
      `CANNOT LOAD: must be filed as "..."` with no Load button.
- [ ] Stage a theme, replace the card file, reboot, confirm the boot check refuses and the
      fail ladder falls back to the previous theme.
- [ ] Build with 12 themes including long names and confirm the list answers instead of
      truncating, exercising section 5.
- [ ] Flash the build. `/tmp/serflash.sh` still points at `B=.../Projects/gorb-os/build`, the
      upstream clone, not this `gorb-os-2` tree. Fix `B` or the device re-flashes the old image.

## 7. Housekeeping

- [ ] Commit the four modified files as two changes: `fix(prep)` for the
      `tools/prep_watch_kit.py` bug fixes, and `fix(theme)` for the naming work.
- [ ] Keep the naming rule documented in one place. It now sits in `.clinerules` prose, in
      this file, and in `filed_as_its_own_slug()`'s comment. Make the code comment the source
      of truth and let the other two point at it.
- [ ] Unrelated, still open from the same session: gate the debug HTTP endpoints behind
      `CONFIG_ORB_DEBUG_HTTP`, and finish the AP passphrase UI that is still a stub.

---

## Reproduce the original failure

```
python3 tools/prep_watch_kit.py skeleton_theme.zip /tmp/skelout
python3 tools/bake_clock.py /tmp/skelout/theme --bg-frames /tmp/skelout/frames \
    --bg-fps 4 --tick-rate 4 --orbtheme /tmp/skelout/skeleton.orbtheme \
    --theme-slug skeleton --theme-title Skeleton
```

Upload that file typing `Skeleton` in the name box. Before section 1 shipped, it uploaded,
listed as loadable, and failed to stage with the generic 422. Load the same bytes with the
box blank: the filename stem matches the slug, and staging succeeds.

