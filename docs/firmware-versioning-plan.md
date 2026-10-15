# Firmware version stamps: declare, embed, read out

**Status: APPROVED — self-contained implementation plan; pick it up cold.**
Author: the planning session of 2026-10-14. Owner's decision on record: implement
the full scheme including cmake commit metadata. Repo:
`/home/greg/docker/Projects/gorb-os` (`gregtakacs/gorb-os`, branch `main`).
Build env: ESP-IDF **v5.4** at `~/esp/esp-idf` —
`. $HOME/esp/esp-idf/export.sh && idf.py build`. If the tree moved since the last
build, `idf.py fullclean` first: the CMake cache pins absolute paths. Read
`.clinerules` before running any build or flash — anything over 30 s must be
launched detached with the `setsid … & disown` idiom and polled, or the tool
kills it mid-write and leaves a half-flashed board.

## The problem, verified against the tree

Firmware identity today is a bare commit SHA. `git tag` on this repo prints
nothing, so IDF's default `git describe` yields `0fa0ba5-dirty`, and that string
is the whole version on every surface.

That is not cosmetic. It is a correctness defect in three places:

1. **No ordering exists.** `internal/firmware/firmware.go:340` says it outright:
   "A git describe like `8eb4bf0-dirty` carries no ordering, so it is never the
   basis for 'newer'." So `Compare` (`:414`) decides upgrade versus downgrade from
   the build timestamp alone. A clock-skewed bench build out-ranks a real release.
2. **`sameVersion` carries a `-dirty` string hack** (`:447`, a `TrimSuffix`). A
   rebuild of one commit with local edits reads as a different release.
3. **No pre-release exists.** Nothing can say "bench build of the next release".
   No downgrade refusal is possible. `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`
   (`sdkconfig:439`) already rolls a bad update back, and nothing can stop the
   push before it starts. `CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK` is unset
   (`sdkconfig:440`), so IDF enforces no floor either.

## The scheme (the contract)

```
v<MAJOR>.<MINOR>.<PATCH>[-<pre>]+<7hex>[-dirty]        at most 31 bytes
v1.2.0+e1a2b3c          v1.3.0-rc.1+e1a2b3c          v1.2.0+e1a2b3c-dirty
```
Each component holds one or two digits. The validators enforce that, not the prose.

- `version.txt` at the repo root holds the declared part, one line, no comment:
  `v1.0.0`. Seed `v1.0.0` unless the owner names another number.
- Commit metadata (`+<sha>`, `-dirty`) is appended by cmake at configure time.
  A tree with no git gets the bare declared string.
- Bump discipline, to go in the docs: PATCH for fixes; MINOR for new
  user-visible capability; MAJOR when a contract another running component
  depends on breaks — the fleet store shape, the TXT keys, the OTA frame, the
  partition layout, the provisioning flow. `-rc.N` for bench candidates.
- Two digits per component, not three, because the field is 32 bytes and the
  metadata is not optional in practice. Worst case measures 29 bytes:
  `v99.99.99-rc.99+e1a2b3c-dirty`. A three-digit grammar needs 32 bytes for its
  worst case and would not fit. The realistic daily stamp is 20 bytes:
  `v1.0.0+e1a2b3c-dirty`.

**No new wire fields.** `fw=` in the TXT record and `"fw"` in `/api/v1/status`
already carry `app->version` verbatim, so the string grows and nothing else moves.

## Four surfaces, one field — all already echo it

| Surface | Site | Today | After |
|---|---|---|---|
| Serial | bootloader `app_init: app version:`, parsed at `internal/esprom/identity.go:44,53` | `0fa0ba5-dirty` | `v1.0.0+0fa0ba5-dirty` |
| mDNS TXT | `main/net/orb_mdns.c:342`, sourced `:725` | `fw=0fa0ba5` | `fw=v1.0.0+0fa0ba5` |
| Device web | `h_status` format `main/net/orb_net.c:2287`, arg `:2336` | `"fw":"0fa0ba5"` | `"fw":"v1.0.0+0fa0ba5"` |
| Bundle | `appDescriptor` at `firmware.go:245`, printed by `cmd/orbpack/main.go:59` | `0fa0ba5-dirty` | `v1.0.0+0fa0ba5-dirty` |

Widths checked, so no buffer work is needed: `orb_mdns.h:51` declares `fw[32]`
"the width of esp_app_desc_t.version, which is what is copied in, so the copy can
never truncate"; the TXT `char kv[64]` (`orb_mdns.c:339`) holds
`fw=v1.0.0+0fa0ba5-dirty` at 23 bytes plus one length octet; `/api/v1/status` has
377 B headroom in its 1536 B frame (`orb_net.c:2064` comment), and the field grows
from `0fa0ba5-dirty`'s 13 bytes to 20.

## Toolchain ground truth, checked on this box

These four findings decide the design. Do not re-derive them; do not "modernise"
past them.

1. **The `VERSION_3` mechanism is absent here.** `grep -rln VERSION_3
   ~/esp/esp-idf/tools` finds nothing, and no `project_version.cmake` exists. So
   structured cmake version variables are not available on IDF 5.4 as installed.
2. **`PROJECT_VER` set before `project()` wins.** `~/esp/esp-idf/tools/cmake/project.cmake:712`
   takes a pre-defined `PROJECT_VER` verbatim; the fallback order documented at
   `:660-665` is version.txt, then the `project(VERSION)` argument, then
   `git describe`, then `1`. This is the mechanism to use.
3. **`version.txt` is read verbatim** (`project.cmake:142-144`), so a file holding
   `v1.2.0-rc.1` would pass the declared string through with no commit metadata.
   It is the declared source of truth, and cmake overrides it on purpose to append
   metadata.
4. **Two sdkconfig options silently kill the scheme**: `CONFIG_APP_PROJECT_VER_FROM_CONFIG`
   (`sdkconfig:465`) and `CONFIG_APP_EXCLUDE_PROJECT_VER_VAR` (`:463`). Both are
   currently off, which is what lets the descriptor carry our string. Say so in the
   docs, because a future `sdkconfig` refresh can turn the version into a Kconfig
   constant without touching a line of our code.

Verified by inspection, and worth knowing so no step re-invents it: IDF writes the
resolved version into `build/project_description.json` as `project_version`
(`tools/cmake/project_description.json.in`), where it currently reads
`0fa0ba5-dirty`. Read that file to confirm what a configure resolved, rather than
re-running git by hand: `CONFIG_BOOTLOADER_PROJECT_VER=1` at `sdkconfig:397` shows a
non-numeric `PROJECT_VER` is accepted without disturbing the bootloader.

## Step 1 — `tools/orb_version.sh` (new): the one composer

Two callers need the identical string: cmake stamps it into the image, and
`serflash.sh` audits the image against the tree. Give them one implementation.

```bash
#!/bin/bash
# Print this tree's firmware version stamp, or explain why it cannot.
#
# One implementation for two callers. Root CMakeLists.txt stamps the image at
# configure time, and tools/serflash.sh audits the bytes about to enter a board.
# A disagreement between those two is the failure this file exists to prevent, so
# neither side computes the string itself.
#
# Usage: tools/orb_version.sh [repo-root]        default: parent of this script
#        tools/orb_version.sh --self-test        run the built-in assertions
set -u

ROOT="${1:-$(cd "$(dirname "$0")/.." && pwd)}"
[ -f "$ROOT/version.txt" ] || { echo "orb_version: no version.txt at $ROOT" >&2; exit 1; }

# One line, nothing else. cmake's file(STRINGS) folds every line into the value,
# so a comment line in version.txt would become part of the version.
DECL=$(head -n1 "$ROOT/version.txt" | tr -d ' \t\r')
printf '%s' "$DECL" | grep -Eq '^v[0-9]{1,2}\.[0-9]{1,2}\.[0-9]{1,2}$' || {
  echo "orb_version: version.txt holds [$DECL]; want v<major>.<minor>.<patch>" >&2
  exit 1; }

# No git, no metadata: a source tarball still names the release it is.
if ! command -v git >/dev/null 2>&1 || ! git -C "$ROOT" rev-parse --git-dir >/dev/null 2>&1; then
  echo "$DECL"; exit 0; fi

# --verify prints nothing at all on a repo with no commit yet, unlike a bare
# rev-parse, which echoes the literal string HEAD. Without -q --verify an empty
# repo stamps itself v1.0.0+HEAD-dirty.
SHA=$(git -C "$ROOT" rev-parse -q --verify HEAD 2>/dev/null | cut -c1-7)
[ -n "$SHA" ] || { echo "$DECL"; exit 0; }      # a tree with no commit yet

DIRTY=""
git -C "$ROOT" diff --quiet --ignore-submodules HEAD -- 2>/dev/null || DIRTY="-dirty"
echo "${DECL}+${SHA}${DIRTY}"
```

`chmod +x` it. Then handle `--self-test` before that logic: build a temp dir with
`git init`, one commit, and a good `version.txt`, and assert (1) a clean tree
prints `v1.0.0+<7hex>` with no `-dirty`, (2) an edit to a tracked file adds
`-dirty`, (3) an untracked file does not, (4) `1.0.0` without the `v` exits 1 with
a message, (5) no git repo prints the bare triple, (6) an empty repo with no commit
prints the bare triple. Print `SELFTEST OK` and exit 0. This is the only check that
runs without ESP-IDF, so make it real rather than decorative.

### Corrected on 2026-10-14, during implementation

The block above is the contract's shape, not the shipped script. Four things had to
change, and every one of them was a bug in this plan rather than a preference:

- **The declared part accepts a prerelease.** The step-1 grammar was
  `^v[0-9]{1,2}\.[0-9]{1,2}\.[0-9]{1,2}$`, which refuses `v1.3.0-rc.1` outright —
  so the step would have shipped a composer unable to emit the grammar its own
  contract section specifies. The shipped check splits the line, matches the triple
  against the eight patterns a one-or-two-digit component allows (bash `case` has no
  counted repeat), and runs a `valid_pre` function over the identifiers. The width
  check runs BEFORE the grammar check, so a too-long line fails with the width named.
- **`v1.0.0-dirty+e1a2b3c` is not the dirty form.** The contract's own
  `<sha>[<build>]` order puts `-dirty` last, and that is what the composer emits.
  The pattern above is accepted by the parser as the prerelease `dirty` with no
  provenance, so it reads clean-and-named when it is neither. The split is written
  so `dirty` in the prerelease position is not special-cased anywhere.
- **The split's guard pattern does not work in bash.** The plan's
  `case "$decl" in v[0-9][0-9]?*.-*)` intends "v, two optional digits, then a dot".
  Bash reads `[0-9]?` as bracket-then-single-character-wildcard, so the pattern needs
  a digit in position two and a third character, and it refuses `v1.0.0` outright:
  every good version line came back as malformed. The shipped guard is `v[0-9]*`, and
  the two-digit rule is enforced by the enumerated core patterns, where it belongs.
- **The dirty predicate is a function, and the dispatcher is a `case`.** Cases (3)
  and (6) only exercise the stamp as a whole; a swapped `-dirty` inside the composer
  passes them both. `dirty_check` and `sha_after_commit` are separate functions with
  their own cases, and the self-test's temp repos are nested inside the real working
  tree, so the first thing `dirty_check` does is compare `rev-parse --show-toplevel`
  against the named root and refuse. `--selftest` (no hyphen) is accepted as an
  alias because `serflash.sh`'s audit flag is spelled that way, and that script is
  the composer's other caller.

Self-verification now covers 14 assertions: the six above, plus the dirty predicate
alone on clean/dirty/untracked-only, a nested checkout, the prerelease round trip,
`rc.01` refused, a trailing hyphen refused, and a 32-byte line refused with the width
named. The oversize case needs a 32-byte line, not the 31-byte one this step
originally proposed: at 31 bytes the width check passes and the grammar message
answers instead, which is a correct refusal but not the one under test.


## Step 2 — `version.txt` (new), at the repo root

One line: `v1.0.0`. No comment, no blank line, no trailing space — see the
`file(STRINGS)` note above. Put the rationale in the docs, never in the file.

## Step 3 — root `CMakeLists.txt`: compose and pin the stamp

Insert before `include($ENV{IDF_PATH}/tools/cmake/project.cmake)` at line 13. The
mechanism is finding 2 above: a pre-defined `PROJECT_VER` beats version.txt, which
beats `git describe`. Without this block the empty tag list keeps producing
`0fa0ba5-dirty`, which is the defect itself.

```cmake
# ---------------------------------------------------------------------------
# Firmware version stamp: docs/firmware-versioning-plan.md.
#
# version.txt declares v<MAJOR>.<MINOR>.<PATCH>. The metadata appended here lets a
# board name the exact bytes it runs, and the whole string lands in
# esp_app_desc_t.version: char[32], so 31 usable bytes once strlcpy NUL-counts. PROJECT_VER set here wins: IDF takes a
# pre-defined PROJECT_VER first and only then tries version.txt, a VERSION
# argument, and git describe (project.cmake:660-712).
#
# Bump version.txt, then reconfigure. CMake caches the stamp at CONFIGURE time, so
# `idf.py build` alone rebuilds sources around the old stamp. serflash.sh reads the
# stamp out of the image and refuses the mismatch, which is the point.
# ---------------------------------------------------------------------------
find_program(ORB_BASH NAMES bash sh)
if(ORB_BASH)
    execute_process(
        COMMAND "${ORB_BASH}" "${CMAKE_CURRENT_LIST_DIR}/tools/orb_version.sh"
                "${CMAKE_CURRENT_LIST_DIR}"
        OUTPUT_VARIABLE ORB_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_VARIABLE  ORB_VERSION_ERR RESULT_VARIABLE ORB_VERSION_RC)
    if(NOT ORB_VERSION_RC EQUAL 0)
        message(FATAL_ERROR "orb_version.sh refused to name this tree: ${ORB_VERSION_ERR}")
    endif()
else()
    # No shell on PATH, which is a bare Windows configure: the declared triple
    # alone is still ordered, and order is the half of the scheme that matters most.
    file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/version.txt" ORB_VERSION)
    string(STRIP "${ORB_VERSION}" ORB_VERSION)
endif()
if(NOT ORB_VERSION MATCHES "^v[0-9][0-9]?\\.[0-9][0-9]?\\.[0-9][0-9]?")
    message(FATAL_ERROR "orb firmware version \"${ORB_VERSION}\" does not parse")
endif()
string(LENGTH "${ORB_VERSION}" ORB_VERSION_LEN)
if(ORB_VERSION_LEN GREATER 31)
    message(FATAL_ERROR "orb firmware version \"${ORB_VERSION}\" is ${ORB_VERSION_LEN} bytes; the descriptor field holds 31")
endif()
set(PROJECT_VER "${ORB_VERSION}")
# Register the dependency ourselves. Bypassing IDF's version.txt reader bypasses
# its own CMAKE_CONFIGURE_DEPENDS, so a bump would otherwise sit unnoticed in the
# cache and print a stamp that disagrees with the file.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${CMAKE_CURRENT_LIST_DIR}/version.txt")
message(STATUS "orb firmware version: ${ORB_VERSION}")
```

Verify by configuring: `. $HOME/esp/esp-idf/export.sh && idf.py reconfigure` must
print the `orb firmware version:` line and no `FATAL_ERROR`. The run exceeds 30 s
on a cold cache, so launch it detached per `.clinerules` and grep the log. Confirm
the stamp reached the descriptor with
`dd if=build/gorb-os.bin bs=1 skip=48 count=32 status=none | tr -d '\0'` — it must
equal `tools/orb_version.sh`'s output byte for byte.

## Step 4 — `tools/serflash.sh`: point the audit at the composer

The stamp audit already exists and works; only its `WANT` source changes.

- Line 82: replace `WANT=$(git -C "$ORB_ROOT" describe --always --dirty 2>/dev/null)`
  with `WANT=$("$ORB_ROOT/tools/orb_version.sh" "$ORB_ROOT")`. If that exits
  non-zero, print which guard fired and refuse the flash. Do not silently fall back
  to `git describe`: a missing `version.txt` is exactly the condition the audit
  should stop.
- Keep `HAVE` exactly as is. `dd if=… bs=1 skip=48 count=32` reads the version
  field: the app `.bin` carries a 24-byte image header plus an 8-byte segment header
  before the descriptor, and `version` sits 16 bytes into the descriptor, so 48 is
  correct. Do not "fix" it.
- Update the comment block at lines 21-29. It must say the stamp is now a declared
  version plus commit metadata, and that `CONFIG_APP_PROJECT_VER_FROM_CONFIG` would
  override the whole mechanism. Keep the observed 2026-10-11 incident, because it is
  why the audit exists.
- Expect the first `serflash.sh` run after this lands to print `AUDIT FAIL`. That is
  correct: the image on disk still carries the pre-scheme stamp. The fix line the
  script already prints (`idf.py reconfigure && idf.py build`) resolves it.

## Step 5 — comments that state a width, refreshed

No logic changes in the firmware. Both readers already copy
`esp_app_get_description()->version` verbatim.

- `main/net/orb_mdns.h:51` — the `fw[32]` example reads `"f716ffa"`. Make it
  `"v1.0.0+e1a2b3c"`. The width claim stays true: the field is the descriptor's own
  width, so the copy still can never truncate.
- `main/net/orb_net.c:2064` — the sizing comment demands a re-measure whenever a
  field's width changes. Add one sentence: `fw` now carries `v1.0.0+e1a2b3c-dirty` at
  20 B, 25 B with an `-rc.N`, inside the 377 B headroom already recorded. Then run the
  real re-measure in the bench checklist and quote an observed range, as that comment
  insists. Reconfigure and rebuild first, or you measure an old stamp.

## Step 6 — `internal/firmware/version.go` (new): the parser and the ordering

Put it in `internal/firmware` so the bundle path and the probed-orb path order
versions with one implementation. No third-party semver package: the stdlib covers
it, and the module must stay `CGO_ENABLED=0` with the two dependencies it already has.

```go
package firmware

import (
	"regexp"
	"strconv"
	"strings"
)

// Version is a parsed gorb-os firmware stamp: v1.2.3-rc.1+e1a2b3c-dirty. The triple
// is the declared release, Pre marks a candidate, Commit names the tree it was built
// from, and Dirty says the build carried uncommitted edits. Ordering reads the triple
// and the Pre only: Commit and Dirty distinguish builds, and a distinguishing fact
// must not decide which one is newer.
type Version struct {
	Major, Minor, Patch int
	Pre                 string
	Commit              string
	Dirty               bool
	Raw                 string
}

// versionRe is the whole scheme, anchored, so a legacy git describe cannot half-parse:
// 8eb4bf0-dirty has no leading v and no triple, and fails here rather than becoming a
// bogus 0.0.0 that would outrank nothing.
var versionRe = regexp.MustCompile(`^v(\d{1,2})\.(\d{1,2})\.(\d{1,2})` +
	`(?:-([0-9A-Za-z][0-9A-Za-z.-]*))?(?:\+([0-9a-f]{7}))?(-dirty)?$`)

// ParseVersion reads the stamp exactly as tools/orb_version.sh writes it. ok is false
// for every pre-scheme string, and callers must then fall back to the build timestamp
// rather than guess: firmware built before this scheme exists is real firmware, and
// reporting it as versionless would refuse a working orb.
func ParseVersion(raw string) (v Version, ok bool) {
	m := versionRe.FindStringSubmatch(strings.TrimSpace(raw))
	if m == nil {
		return Version{}, false
	}
	v = Version{
		Pre:    m[4],
		Commit: m[5],
		Dirty:  m[6] != "",
		Raw:    strings.TrimSpace(raw),
	}
	// Every group matched, so each Atoi is infallible. Ignore the errors rather than
	// branch on them, and let a zero stand.
	v.Major, _ = strconv.Atoi(m[1])
	v.Minor, _ = strconv.Atoi(m[2])
	v.Patch, _ = strconv.Atoi(m[3])
	if !validPre(v.Pre) {
		return Version{}, false
	}
	return v, true
}

// validPre enforces the two semver rules a regex cannot reach: no empty identifier,
// and no leading zero on a numeric one. comparePre splits on the same dots, so an
// empty piece would compare as nothing and a padded "01" would sort against "1".
func validPre(pre string) bool {
	if pre == "" {
		return true
	}
	for _, id := range strings.Split(pre, ".") {
		if id == "" {
			return false
		}
		numeric := true
		for i := 0; i < len(id); i++ {
			if id[i] < '0' || id[i] > '9' {
				numeric = false
				break
			}
		}
		if numeric && len(id) > 1 && id[0] == '0' {
			return false
		}
	}
	return true
}

// Declared is the version with no build metadata: what a human calls the release.
func (v Version) Declared() string {
	s := "v" + strconv.Itoa(v.Major) + "." + strconv.Itoa(v.Minor) + "." + strconv.Itoa(v.Patch)
	if v.Pre != "" {
		return s + "-" + v.Pre
	}
	return s
}

// Compare returns -1, 0, or 1 by semver precedence. Commit metadata is ignored, so
// v1.2.3+aaaaaaa and v1.2.3+bbbbbbb are one release, and a -dirty build of a release
// still IS that release.
func (v Version) Compare(o Version) int {
	if v.Major != o.Major {
		return sign(v.Major - o.Major)
	}
	if v.Minor != o.Minor {
		return sign(v.Minor - o.Minor)
	}
	if v.Patch != o.Patch {
		return sign(v.Patch - o.Patch)
	}
	return comparePre(v.Pre, o.Pre)
}

func sign(d int) int {
	switch {
	case d < 0:
		return -1
	case d > 0:
		return 1
	}
	return 0
}

// comparePre orders a pre-release below the release it precedes, then identifier by
// identifier as semver defines it: numeric identifiers compare numerically,
// alphanumeric lexically, numeric below alphanumeric, and a shorter set below a longer
// one that starts with it. v1.3.0-rc.1 < v1.3.0-rc.2 < v1.3.0.
func comparePre(a, b string) int {
	switch {
	case a == b:
		return 0
	case a == "":
		return 1 // a final release outranks any candidate of it
	case b == "":
		return -1
	}
	as, bs := strings.Split(a, "."), strings.Split(b, ".")
	for i := 0; i < len(as) && i < len(bs); i++ {
		if c := compareIdent(as[i], bs[i]); c != 0 {
			return c
		}
	}
	return sign(len(as) - len(bs))
}

func compareIdent(a, b string) int {
	an, aerr := strconv.ParseInt(a, 10, 64)
	bn, berr := strconv.ParseInt(b, 10, 64)
	switch {
	case aerr == nil && berr == nil:
		return sign(int(an - bn))
	case aerr == nil: // numeric ranks below alphanumeric
		return -1
	case berr == nil:
		return 1
	}
	return strings.Compare(a, b)
}
```

## Step 7 — `internal/firmware/firmware.go`: honest verdicts

Keep the ELF-equal short-circuit at the head of `Compare` (`:414`) untouched.
Insert the version order immediately after it, before the timestamp ladder.

```go
	// A declared version outranks a build timestamp, because a timestamp measures
	// when someone's clock was, and a triple measures what the release was.
	ov, ook := ParseVersion(orb.Version)
	tv, tok := ParseVersion(target.Version)
	if ook && tok {
		switch c := ov.Compare(tv); {
		case c < 0:
			return VerdictUpgrade
		case c > 0:
			return VerdictDowngrade
		default:
			// One release, different bytes: a rebuild, a different commit, or a
			// dirty tree. Not an upgrade, and not nothing — the orb still reboots.
			return VerdictSameVersion
		}
	}
	// At least one side predates the scheme, so fall through to the timestamp
	// ladder. Declaring a named release newer than an unnamed build would push a
	// user into replacing a working orb on an assumption.
```

Rewrite the rule list in `Compare`'s doc comment: the sentence calling the build
stamp "the only honest ordering a git describe can offer" becomes false, and the
comment must now state the three-tier order — digest, declared version, timestamp
fallback. Same for `Release`'s comment at `:340`.

Then replace `sameVersion` (`:447`) with parsed equality, keeping the legacy path so
pre-scheme builds behave exactly as they do today:

```go
// sameVersion reports two stamps naming one release. A parsed pair ignores commit
// and -dirty, which is what a rebuild of one release means. A legacy pair keeps the
// old rule of dropping -dirty and comparing the rest, because a bare git describe
// carries no other structure to compare.
func sameVersion(a, b string) bool {
	av, aok := ParseVersion(a)
	bv, bok := ParseVersion(b)
	if aok && bok {
		return av.Compare(bv) == 0
	}
	t := func(s string) string { return strings.TrimSuffix(strings.TrimSpace(s), "-dirty") }
	return t(a) != "" && t(a) == t(b)
}
```

The `VerdictSameVersion` comment at `:399` says "same describe". Change it to "same
declared release, different build".

## Step 8 — surface the parsed version, additively

Every existing string field keeps carrying the full raw stamp, so any consumer that
ignores the new fields is unaffected. The UI needs the triple to print `v1.0.0`
without teaching JavaScript to parse semver.

`firmwareSummaryPayload` (`tools/orb-manager/firmware.go:172`) gains one key:

```go
		"semver": semverPayload(b.Version),
```

and `handleFirmwareUpload` plus `handleFirmwareReset` need no change, since both
already return that payload. In `identity.go`, add the same key to the cached-port
entry beside `elfSha256` (`:210`) and to the port's `version` line (`:207`).

The one helper, in `tools/orb-manager/firmware.go`:

```go
// semverPayload is the version as structure, so the UI never parses a stamp. An
// unversioned build is reported as legacy rather than as v0.0.0, because a fake zero
// would render as the oldest release rather than as the unknown it is.
func semverPayload(raw string) map[string]any {
	v, ok := firmware.ParseVersion(raw)
	if !ok {
		return map[string]any{"legacy": true, "raw": raw, "declared": "",
			"major": 0, "minor": 0, "patch": 0, "pre": "", "commit": "", "dirty": false}
	}
	return map[string]any{"legacy": false, "raw": v.Raw, "declared": v.Declared(),
		"major": v.Major, "minor": v.Minor, "patch": v.Patch, "pre": v.Pre,
		"commit": v.Commit, "dirty": v.Dirty}
}
```

Two blast-radius facts, both verified in the tree:

- `TestBuildPanelFieldsMatchBetweenClientAndHandler` (`api_test.go:224`) pins the
  client's read list to exactly `present,images,version,project,source,sha256`. Add
  no `b.semver` read to `web/app.js`, or that test fails by design. Leave the UI on
  the raw string, which now reads `gorb-os v1.0.0+0fa0ba5`: already the improvement
  the user asked for. Adding a rendered triple is a separate change to that test and
  the template together.
- `describeFirmware`'s absent-bundle branch (`tools/orb-manager/firmware.go:116`)
  enumerates every key by hand, so it must gain `"semver": semverPayload("")` to keep
  the shape uniform.

## Step 9 — `cmd/orbpack/main.go`: refuse to ship an unversioned release

The bundle is what an end user flashes with no choices made, so a build with no
declared version must not become the hub's built-in firmware. Gate it at release
time, where the toolchain and the git tree both exist. `firmware.Load` must stay
permissive, or a user could no longer pick an old `.zip` to downgrade deliberately:
the gate belongs here and nowhere else.

After the round-trip `firmware.Load` at line 54, before the success print:

```go
	// A bundle with no declared version cannot be ordered against the one running,
	// which would leave the hub guessing upgrades from build timestamps again.
	if _, ok := firmware.ParseVersion(b.Version); !ok {
		if !force {
			fmt.Fprintf(os.Stderr, "orbpack: refusing to ship %q as a release: not a v<major>.<minor>.<patch> version.\n"+
				"         Write version.txt, reconfigure, rebuild. -force overrides this for a bench build.\n", b.Version)
			os.Exit(1)
		}
		fmt.Fprintf(os.Stderr, "orbpack: shipping unversioned build %q by request\n", b.Version)
	}
```

Add the flag beside the two at lines 19-20: `force := flag.Bool("force", false,
"ship a build with no declared version")`. Then leave the success print at `:59`
alone — it already prints `b.Version`, which is now a real version. That print line
is the whole point of the exercise.

## Step 10 — tests

`internal/firmware/version_test.go` (new), table-driven:

- `TestParseVersionAcceptsRealStamps`: `v1.0.0+e1a2b3c`, `v1.0.0+e1a2b3c-dirty`,
  `v1.3.0-rc.1+aabbccd`, `v0.0.0+0000000`, and a padded `  v1.2.3+aaaaaaa  ` to pin
  the trim.
- `TestParseVersionRefusesEverythingElse`: the legacy forms `8eb4bf0`,
  `8eb4bf0-dirty`, `094fa22`, plus `1`, `""`, `v1.2`, `v1.2.0.4`, `v1.2.0+abc`,
  `v1.2.0-`, `v1.2.0-rc..1`, `V1.2.0+aaaaaaa`, and a 33-byte oversize string. The
  legacy rows are the regression guard: a SHA that half-parsed would silently order
  below every real release.
- `TestVersionOrdering`: `v1.9.0 < v1.10.0` (the numeric test that a lexical compare
  fails), `v1.2.3+aaaaaaa` equals `v1.2.3+bbbbbbb`, `v1.2.3-rc.9` equals
  `v1.2.3-rc.9-dirty`, `v1.3.0-rc.1 < v1.3.0-rc.2`, `v1.3.0-rc.1 < v1.3.0`,
  `v1.3.0-alpha < v1.3.0-alpha.1 < v1.3.0-alpha.beta < v1.3.0-beta < v1.3.0-rc.1`,
  and `v2.0.0 > v1.99.99`.
- `TestWorstCaseFitsTheDescriptor`: assert `len("v99.99.99-rc.99+e1a2b3c-dirty") <= 31`
  and that the string parses, then assert `v123.0.0+aaaaaaa` and `v1.0.100+aaaaaaa` are
  refused. This test is what proved the two-digit rule: a three-digit grammar needs 32
  bytes for its worst case, one more than the field holds.

- `TestDeclaredDropsMetadata`: `v1.2.3-rc.1+e1a2b3c-dirty` declares `v1.2.3-rc.1`.

Extend `TestCompareVersions` (`api_test.go:857`) rather than replacing it. Keep every
existing legacy row green — that is the mixed-fleet proof — and add:

| orb | target | want |
|---|---|---|
| `v1.0.0+aaaaaaa`, older stamp | `v1.1.0+bbbbbbb`, newer stamp | `VerdictUpgrade` |
| `v1.1.0+aaaaaaa` | `v1.0.0+bbbbbbb` with the LATER stamp | `VerdictDowngrade` |
| `v1.0.0+aaaaaaa` | `v1.0.0+bbbbbbb` | `VerdictSameVersion` |
| `v1.0.0-dirty+aaaaaaa` | `v1.0.0+aaaaaaa` | `VerdictSameVersion` |
| `v2.0.0-rc.1+aaaaaaa` | `v2.0.0+aaaaaaa` | `VerdictUpgrade` |
| `8eb4bf0` (legacy) | `v9.9.9+aaaaaaa` | `VerdictUpgrade`, by timestamp, and the row must say why |

Row two is the regression this whole change exists to kill: a higher version must
outrank a later clock.

### Corrected on 2026-10-14, during implementation

Four points in the list above do not say what they look like they say:

- The dirty row is written `v1.0.0+aaaaaaa-dirty`, not `v1.0.0-dirty+aaaaaaa`. The
  second string parses, but as prerelease `dirty` with no provenance — the exact
  ambiguity the grammar forbids, and a test that asserted on it would pin the parser
  to accepting a stamp the composer can never emit.
- The legacy-orb row's `VerdictUpgrade` comes from the timestamp, as the table says,
  and it is worth the comment it carries: it is the proof that the fallback stays
  reachable, not an oversight in the version-first rule.
- `TestBuiltInBundleCarriesASemver` belongs to this step and FAILS until
  `make firmware` repacks the bundle. It is the only test in the suite that can tell
  a stamping change from a stamping change that was never shipped, so it stays.
- The oversize row is a 33-byte line whose components are three digits. A length cap
  is not part of the parser's contract — cmake enforces the 31-byte field — so the
  refusal has to come from the grammar.

`TestOrbpackRefusesAnUnversionedBundle` calls the packer through a `pack()` function
rather than `os.Exit`, because `os.Exit` inside `main()` cannot be observed by a test
in the same package. `-force` returns 0 where a bare call returns 1; the refusal text
is pinned by a source-grep test, the idiom this suite already uses elsewhere.

Add `TestOrbpackRefusesAnUnversionedBundle` driving `cmd/orbpack`'s flag handling
through a synthetic build dir, modelled on `TestWriteBundleFromBuildDir`
(`internal/firmware/firmware_test.go:274`) and its `espImage` helper. Assert exit-1
behaviour for an unparsable stamp and success for a good one with `-force`.

Blast radius, all verified present in the tree today. Expect to touch each:

- `internal/orbfake/device.go:156` — the default `FW: "orbfake"` is unparsable, so it
  now reads as a legacy board. That is honest for a fake device. Change it to a real
  version only if a test needs the semver path, and keep at least one legacy value in
  the suite so the fallback stays covered.
- `discover_test.go:537,554,613`, `mdns_contract_test.go:117,244`,
  `internal/orbfake/{mdns_pin_test.go:87,mdns_walk_test.go:240}` — all pass bare
  SHAs through `SetFirmware`. They must keep working untouched: the contract is that
  the hub reports whatever the board announces.
- `api_test.go:661` and `identity_test.go:55` — synthetic `OrbInfo` fixtures. Safe as
  legacy rows; add a semver fixture rather than editing these.
- `device_contract_test.go:563` — pins the status key list, which does not change.
  `:659` reads `orbfleet.MaxName` from a status-width regex, unrelated to version.
- `internal/firmware/firmware_test.go:319` (`TestBuiltInBundleIsUsable`) — asserts
  only a non-empty version, so the current committed `0fa0ba5-dirty` bundle still
  passes. Add `TestBuiltInBundleCarriesASemver` only AFTER the bundle is repacked, or
  it fails against the committed artifact.
- `internal/esprom/identity_test.go:27` — its fixture line is
  `"I (703) main: === gorb-os booting ==="`. Do not add the version to that boot
  banner in `main/main.c:49`: `gorbBannerRe` (`identity.go:45`) matches the exact
  phrase, so the version belongs in the `app_init` block it already lives in.

## Step 11 — docs

- `README.md`, the `## Build & Flash` section at line 26: add the stamp rule, the
  `version.txt` bump step, and the `idf.py reconfigure` requirement after a bump.
- `.clinerules`: one paragraph. The trap is configure-time caching — bumping
  `version.txt` without a reconfigure produces an image stamped with the old version,
  and `serflash.sh`'s audit will refuse it. Record the fix in one line.
- `docs/firmware-versioning.md` (new): the grammar, the width arithmetic (31-byte
  descriptor field, 23-byte realistic worst case), the bump table, and the four
  read-out surfaces with their file:line anchors.
- `tools/orb-manager/README.md`, the firmware chapter around line 165: state the new
  verdict order, and that an unversioned bundle is refused by `orbpack` but still
  loadable by hand for a deliberate downgrade.

## Ordering, and the one trap that bites

Steps 1-5 form one commit and steps 6-9 another; the firmware and the hub must not
land in a single commit, so each builds and tests on its own. Then, in this order:

1. `cd tools/orb-manager && make test` — green before any rebuild, because the
   changes are additive and legacy stamps still parse.
2. Reconfigure and rebuild the firmware, detached, per `.clinerules`.
3. `make firmware` to repack the committed bundle.
4. `make test` again.

Step 3 is mandatory, not cosmetic, and this is the trap worth knowing cold:
`TestBuiltInBundleMatchesBuildDirWhenPresent` (`firmware_test.go:340`) compares the
committed bundle's image *bytes* against `ORB_BUILD_DIR`, which `make test` sets
automatically whenever `build/flasher_args.json` exists. The version lives inside
`gorb-os.bin`'s descriptor, so a new stamp changes those bytes and the suite fails
until the bundle is repacked. The guard is correct; do not weaken it.

Commit the source first, then rebuild and repack in a second commit. A bundle packed
from a still-modified tree carries `-dirty` in its stamp, which is honest but stale
the moment the tree is clean: after committing, `serflash.sh`'s audit compares a
clean-tree `WANT` against an image whose descriptor still says `-dirty`, and refuses.
Two commits, in that order, keeps both readings true.

## Out of scope, deliberately

- Re-measuring the `/api/v1/status` frame on hardware, and repacking the bundle, and
  the bench verification below. All need a bound board and the hub's 90-second
  identify cadence, and the last one takes about six minutes of waiting.
- A new TXT key. `fw=` already carries the string, the TXT list sits inside the
  512-byte announcement cap `orb_mdns.c:329` measures at runtime, and a second key
  would make "the orb runs two different things" possible.
- The boot banner at `main/main.c:49`, and the hub's own binary version at
  `tools/orb-manager/Makefile:28`. That one stamps `orb-server` and `orbfleet`, not
  firmware, and stays `git describe` for now. Point it at `version.txt` later if the
  hub ever gets a release cadence of its own.
- `CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK` stays off. A version floor in the bootloader
  bricks a bench re-flash loop, which is exactly what the bench scripts are for.
  `CONFIG_APP_ROLLBACK_ENABLE=y` still catches a bad image at runtime.
- Fleet persistence of the version across hub restarts. `tools/orb-manager/fleet.go:229`
  already carries `fw` in `fleetView`, and the row re-fills from the first announcement
  after a hub restart. Do not touch it: `fleet.go` and `internal/orbfleet/fleet.go` are
  both currently modified in this working tree.

## Bench verification, when a board is bound

Run `tools/serflash.sh` first. A passing stamp audit proves the whole chain: cmake
composed the version, the descriptor carries it, and the image on disk matches the
tree. `AUDIT FAIL` on the very first run is the expected outcome of a pre-scheme
image, and the printed fix line clears it.

Then confirm the string is the same on all four surfaces: the serial `app version:`
line during boot, `fw=` from `dns-sd -B _orb._tcp` and the browse reply,
`curl http://<orb>/api/v1/status | jq .fw`, and the hub's Onboard card. Then re-measure
the status frame as `orb_net.c:2064`'s comment demands, and update that comment with
the observed range.

After all of this lands, cut one annotated tag: `git tag -a v1.0.0 -m "first
versioned build"`. It makes `git describe` agree with `version.txt`, so a
contributor who reads the tag instead of the file gets the same answer.

## Kickoff prompt for the implementation session

> Implement `docs/firmware-versioning-plan.md` in `/home/greg/docker/Projects/gorb-os`,
> steps 1 through 11, in order, firmware-side changes first and hub-side second. The
> plan is self-contained and every `file:line` reference in it was verified on
> 2026-10-14 against commit `0fa0ba5`; re-check any anchor before editing it. Follow
> `.clinerules` for every build or flash, including the detached-launch rule for
> anything over 30 seconds. Do not run `serflash.sh` against a board unless explicitly
> asked, and do not commit: report the diff for review. Do not modify
> `internal/fleet/fleet.go`, the boot banner in `main/main.c`, or the
> `TestBuildPanelFieldsMatchBetweenClientAndHandler` pinned field list. Verify with
> `cd tools/orb-manager && make test` before any firmware rebuild, then report which
> steps landed, which tests you added, and anything you found that contradicts the
> plan.






