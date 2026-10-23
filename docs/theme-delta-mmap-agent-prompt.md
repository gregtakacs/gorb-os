# Agent prompt: theme delta storage + mmap load (execute as written)

Implement `docs/theme-delta-mmap-plan.md` in `/home/greg/docker/Projects/gorb-os`,
in the six commits its §8 names (C1–C6). Read that doc and `.clinerules` fully
before touching code. This prompt adds the bench commands, the ground rules, and
the four claims from the design discussion that verification disproved. Every
claim below was checked against the tree on 2026-10-22.

Work the commits in order and stop rather than improvising whenever a stop
condition applies.

## Ground rules

1. **Long commands run detached, always.** Anything past ~30 s must be launched
   with `setsid /tmp/job.sh </dev/null >/dev/null 2>&1 & disown` and polled from
   a log. This includes every flash, every WiFi OTA push, and any `idf.py build`
   after an `sdkconfig` change. A command killed by the tool timeout mid-flash
   leaves a half-written slot, and you pay the wait twice.
2. **Flash order is fixed.** `tools/serflash.sh` refuses an image whose embedded
   stamp differs from `tools/orb_version.sh`. The order is always: edit, commit,
   `idf.py reconfigure && idf.py build`, then flash. A `version.txt` edit needs
   the reconfigure or `PROJECT_VER` stays cached at the old value.
3. **The bound board is one orb.** `.orb-target` at the clone root names orb-B,
   MAC `44:bd:8d:78:26:18`, IP `192.168.2.34`. Every tool in `tools/` re-proves
   identity through that MAC. Never name a bare `/dev/ttyACMx`; a second S3 is
   attached.
4. **A green build is not done.** Each commit's pass line must run on the board
   and its evidence go into your final report: the command, the log file, the
   observed line. You are pre-authorized to flash, reboot, store themes and
   erase the slot on orb-B for this work; do not ask first. Use
   `tools/serflash.sh` for the app, `tools/capf.py <secs> <log>` for the boot
   log, `tools/fbgrab.py` for pixels.
5. **The partition table does not change.** The `assets` row is already one 8 MB
   partition holding one slot. Do not touch `partitions.csv`; a table change
   costs a full reflash and breaks the NVS retention invariant that row guards.
6. **Do not hand-edit** `main/app/orb_app_factory_blobs.c` (generated, with a
   stale-file lint) or `tools/orb-manager/web/composer/orbtheme-baker.js` (a
   vendor copy; rebuild it from `tools/watch-composer/` instead).
7. **Scope stays in this one project folder.** Nothing here touches NAS-STACK,
   DEV-LLM-STACK or any other neighbour of the workspace.

## Suite commands

```sh
# FORMAT 3 writer vs the Python that defines the format (node + a python3 that
# can import tools/; no pillow, no numpy):
python3 tools/make_bundle_fixture.py --vectors /tmp/bundle3_vectors
cd tools/watch-composer/_selftest && \
  B3_VEC=/tmp/bundle3_vectors ~/.local/share/nodeenv-gorb/bin/node headless-bundle3.test.js

# device-side loader door, on the host:
python3 tools/host_clock3_test.py              # golden, derivations, refusals
python3 tools/host_clock3_test.py --mutations  # and the C mutants

# ink-chord stamp invariants (exit 0 = clean) — mandatory for C5 and C6:
python3 tools/sim_tourb_mark.py

# browser bake suites:
#   headless-orbtheme.test.js   needs the bake venv (pillow + numpy) as well as node
#   headless-regression.test.js all 15 presets still build
```

`python3 tools/host_sd_test.py` pins the ABI word counts (`AUX_WORDS`,
`BANK_WORDS`) against `theme_schema.words_of()`, so it catches an ABI bump that
moved one table and not the other. Run it with C4.

## Four claims that are wrong in the wild, and what is true

The design doc states these correctly. They are repeated here because each was
believed at some point in the design discussion, and each is the kind of error
that silently breaks a build.

1. **There is no `clk_composite()`.** Since Migration 2 Commit 4 the clock has no
   private compositor: `clock_layers_build()` claims one layer row per element,
   `clock_layers_refresh()` re-poses them, and `orb_sched_commit_layers()` is the
   only L4 commit path on the glass. That build pass **already sorts by `order`**
   (`clock_view.c:1775-1800`), so z-order is not device work at all. The gap is
   bake-side: the element record's order column is not written from the engine's
   `drawOrder`, so every row ties and table order silently decides. The engine's
   order is available at `orbtheme-baker.js:1325` and does not survive into the
   emitted row.
2. **`ORB_ART_MECH_B` is one global define** (`art_bank.h:108`, currently 1),
   obeyed by `art_bank_init()` for every caller (`art_bank.c:343-347`). Flipping
   it would move app banks onto mmap, which the doc's §9 rules out. Themes need
   the mechanism chosen per call; `art_bank.h:225` already documents the A form as
   available regardless of the define. The define stays 1, so the rest of the
   build is unaffected.
3. **Tempo is one theme-wide scalar.** `tourbillon_fps` (`orb_clock3.c:292`,
   `clk_types.h:69`), consumed at `orb_sched.c:501` and `clock_view.c:677`, where
   zero defaults to 15 Hz. `orb_clock3.c:811` records that R6 deliberately retired
   the per-frame delay, and `:818-822` fills every slot from the one scalar.
   Per-row tempo is therefore new ABI vocabulary, not a reinterpretation of an
   existing column.
4. **The budget arithmetic, exactly:** 859,837 B is 10.3% of the real 8 MB cap
   (`ORB_THEME_SLOT_CAP`, 8,323,072 B) and 20.5% of the stale 4 MB figure. A full
   slot costs **127** pages of the 234-page art budget, not 128.

## The hazard mmap introduces

`orb_theme_install_bundle()` contains no unmap — `grep unmap main/art/orb_theme.c`
is empty. That is safe only because today's arena model never leaves the slot
mapped. Under mmap, erasing beneath a live map leaves cached DROM pages holding
the old bytes, so a store can report success and reboot into a stale plane. The
rule is ordering, not a check: park the clock, unmap, erase, write, reboot. Never
remap in place; the reboot re-establishes the map from the bytes on the flash.

## The trap in C1

C1 rewrites two assertions that currently pin the broken behaviour:
`headless-bundle3.test.js:144` asserts `B.ART_CAP === 4128768` under a comment
claiming those are "orb_theme.c's own numbers", and `:353-354` asserts
`bg_mode === 0 && gif_frames === 0` under the label "delta vocabularies zero".
After C1 the second of those **must fail**, because the format does not yet do
what the new contract demands. That red is the spec for C2–C5.

Do not resolve the failure by restoring the old assertion or by making the test
read the current output. That is exactly how this bug survived its last three
test suites: a test that asserts the shipped value cannot notice the shipped
value is wrong. The suite is correct when it is red on exactly the delta
assertions and green everywhere else.

## Stop conditions — stop and report rather than guess

- An `sdkconfig`, Kconfig or `partitions.csv` change looks necessary. None is in
  scope; report what you needed and why instead of making it.
- The boot log or an M2/M3 probe says mapped-theme reads cost more than ~2× the
  arena copy did on the fps line. §10 makes C3 conditional on that measurement.
- A delta chain will not close on frame 0, or the encoder needs to emit a frame
  that is not an exact diff. A chain that drifts corrupts every later frame.
- The re-baked tourbillon lands far off the §3 budget. Report the real number per
  region; do not adjust the budget to match the output.
- `sim_tourb_mark.py` goes non-zero. Two stamp invariants are invisible to the fps
  counter when broken — halve-radius stamps, and the wide-chord row walk — and
  both are recorded in `.clinerules`. Fix the stamp, not the test.
- Any commit needs a factory-app or app-bank mechanism change. That is a separate
  piece of work.

## Commit messages

Conventional form, imperative, under 50 characters:
`fix(theme): ...`, `feat(theme): ...`, `test(baker): ...`. The body explains
why, not what. No hedging, no marketing words.

## Report format

Per commit, in order:

1. The diff summary (files, and what changed in each).
2. The commit message used, verbatim: `git log --format=%s -1`.
3. The suite output as observed, not as expected.
4. The bench evidence: command, log file, and the exact line satisfying the pass
   line.
5. Any deviation from §8 and its reason.

End with the C6 measured numbers against §3's per-region budget, and an explicit
list of anything you could not verify. Do not report a commit as done on the
strength of a passing build alone.
