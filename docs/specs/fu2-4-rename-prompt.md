# fu2-4: saving a library design whose name breaks the basic-character rule offers a rename

## Goal

Ruling 35's name rule (basic characters, case-insensitively unique) is newer than some names already
in a hub's library. Such an entry still opens, lists and deletes under its old name. But saving it
today hits a dead end: the studio's client-side check refuses before it posts, and the hub's
create-name door refuses, so the owner sees a flat "Not saved: a design's name can use only letters
…" on a design they already own. In this step, saving such an entry instead offers a **rename
prompt** pre-filled with a cleaned, unique, never-empty suggestion; saving under that new name goes
through the **normal checked door** (the hub rule is not loosened). The exact prompt copy is
provisional — the lead will verify the behaviour live and may adjust wording. Do not touch
orb-os-net.

## Read first

- `docs/specs/fu2-design.md` §"Item 4".
- `tools/orb-manager/designs.go`: `designName` (~55 — applies no character rule), `handleDesignSave`.
- `tools/orb-manager/libentries.go`: `checkNewName` (~214), `checkThemeName`/`themeNameOK`/
  `themeNameRefusalChars` (the basic-character rule and its sentence), `handleLibKeep`, `entries`,
  `libraryEntry`, the forget door.
- `tools/orb-manager/web/runtime/theme@1/session.js`: `api.checkName` (~378, the `NAME_BASIC`/
  `NAME_CHARS` client rule), `saveEntry` (~398, the `Not saved: …` line), how `lib` (the app's
  library) and the open design's current name are available to the studio.
- The site's remix suffix (case-insensitive ` 2`, ` 3`, …) for the uniqueness convention to mirror —
  `/home/greg/docker/worktrees/orb-os-net-fu2-lead/src/lib/…` (read-only; find the remix/rename
  helper).
- `tools/orb-manager/libentries_test.go` and the studio `_selftest` suite for a seeded library.

## Contract

### Studio (`theme@1/session.js`) — the rename prompt

When `saveEntry` is about to refuse because the design already exists in `lib` under a name that
fails `api.checkName`'s character rule, it does **not** show the flat "Not saved" error. Instead it
shows a rename prompt offering a cleaned suggestion. The suggestion is computed in the studio from
the old name and the app's current library:
- fold accented Latin letters toward their basic form where one exists (`ö`→`o`, `é`→`e`, …);
- drop characters with no basic mapping (`✨`, emoji, other symbols);
- treat path/word separators (`/ \ : * ? " < > |`) as a space boundary;
- collapse runs of spaces to one; trim leading/trailing spaces and periods;
- if the result is empty, fall back to the app's default name `"design"`;
- make it unique **case-insensitively** against the app's library names by suffixing ` 2`, ` 3`, …
  (the remix convention), and re-check the result against `api.checkName` so it is always valid and
  never empty.
Saving under the suggested (or edited) name posts a normal save through the create-name door. The
old refusal path stays for a name that fails the rule and is **not** a pre-existing library entry
(new designs keep the plain refusal). Prompt copy is provisional; reuse `NAME_CHARS` plus the
suggestion, matching the design doc's Owner question 1 (option A).

### Hub (`libentries.go`) — no rule loosened

`checkNewName`/`checkThemeName` are unchanged: a save of a pre-existing badly-named entry under a
new, valid, unique name is just a normal Save As and already passes the door. Do not relax the
character rule. The listing/open/forget doors already apply no character rule — confirm, don't add.

## Tests

- `libentries_test.go`: seed a library entry with a non-basic name (e.g. `café ✨`), then assert it
  is listed, opened and forgotten under that old name; a save under the old name does not hard-fail;
  and a save under a cleaned valid name succeeds through `checkNewName`.
- Studio `_selftest`: drive `saveEntry` for a seeded design named `café ✨` when a `Cafe` entry
  already exists, and assert the prompt's suggestion is a cleaned, case-insensitively unique,
  non-empty, `checkName`-valid name (e.g. `Cafe 2`). Assert the plain new-design refusal is
  unchanged.

## Gate

`export GOFLAGS=-buildvcs=false PATH=/usr/local/go/bin:$PATH` then: `go vet ./...`;
`go test -run 'Rename|ThemeName|Design|LibEntry|Keep|Forget' -v`; `node --check
tools/orb-manager/web/runtime/theme@1/session.js`; `make check-editor`; `make check-web`;
`sh docs/specs/s3-gotest.sh`. `checkNewName`/`checkThemeName` show no diff (rule unchanged).


## What an owner sees today (lead's live probe, base c648c00)

A probe drove a real hub in headless Chromium with clock designs `Nöel ✨` and `a/b` in the
library. Both list and open fine ("Opened “Nöel ✨” from the library"). Pressing Save to library
sends nothing to the hub: the studio's own check shows a toast that is cut off at the toast's
width ("Not saved: a design's name ca…") and fades in about 3 s. `Plain` saves normally.

## Live check (required, in addition to the Gate)

Copy `tools/orb-manager/web/runtime/_selftest/rename-live.js` from
`/home/greg/docker/worktrees/gorb-os-fu2-lead-fu2-rename-probe` (mounted read-only; it is the
probe above) into this tree, and extend it so it asserts the new behaviour and exits non-zero
otherwise: for each bad name, pressing Save shows the rename prompt (a dialog that stays until
answered, not a fading toast) with the suggestion pre-filled (`Noel` and `a b` for an empty
library besides `Plain`); accepting it makes exactly one `POST /api/design/save` for the new name
answered ok; afterwards both the old and the new name list, the old one still opens, and the
old one can be forgotten. Cancelling the prompt saves nothing. It prints
`rename-live: N/N passed` and keeps writing the screenshots to `.rename-shots/`.
Add `.rename-shots/` to `.gitignore`. Root Makefile target `check-rename-live` runs it (not part
of `check`).
