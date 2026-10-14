#!/bin/bash
# Embed the declared version plus provenance, as one string, or explain why it
# cannot be embedded.
#
# One implementation for two callers. Root CMakeLists.txt stamps the image at
# configure time, and tools/serflash.sh audits the bytes about to enter a board.
# A disagreement between those two is the failure this file exists to prevent, so
# neither side computes the string itself.
#
# The stamp is provenance, not marketing. The commit short SHA names the source
# tree the image was actually built from, which the declared version never can.
# That is also why a dirty tree says so instead of silently reusing the last clean
# stamp. A developer can type any number into version.txt at any time; the SHA and
# the dirty flag are the parts that cannot be typed.
#
# Usage: tools/orb_version.sh [repo-root]   default: parent of this script
#        tools/orb_version.sh --self-test   run the built-in assertions
#        tools/orb_version.sh --selftest    accepted: serflash.sh's audit flag
# Exit 0 with the stamp on stdout, or exit 1 with the reason on stderr.
set -u

SELF="$(cd "$(dirname "$0")" && pwd)/$(basename "$0")"
FAILS=0

# fail <case> <detail> — record one broken assertion and keep going, so one run
# reports every breakage rather than stopping at the first.
fail() { printf 'FAIL %s: %s\n' "$1" "$2" >&2; FAILS=$((FAILS + 1)); }

# want_ok <case> <dir> <expected> — the composer must name the expected stamp.
want_ok() {
  local out rc
  out="$("$SELF" "$2" 2>&1)"; rc=$?
  [ "$rc" -eq 0 ] || { fail "$1" "exit $rc, said: $out"; return; }
  [ "$out" = "$3" ] || fail "$1" "got [$out], want [$3]"
}

selftest() {
  T="$(mktemp -d)" || { echo "orb_version: no mktemp" >&2; exit 1; }
  trap 'rm -rf "$T"' EXIT
  GC=(git -c user.email=orb@example.invalid -c user.name=orb -c commit.gpgsign=false)

  # A real repo, one commit, one good version.txt: the case every build lands in.
  R="$T/repo"; mkdir -p "$R"
  printf 'v1.0.0\n' >"$R/version.txt"
  printf 'seed\n' >"$R/tracked.txt"
  "${GC[@]}" -C "$R" init -q -b main >/dev/null 2>&1
  "${GC[@]}" -C "$R" add -A >/dev/null 2>&1
  "${GC[@]}" -C "$R" commit -q -m seed >/dev/null 2>&1
  SHA="$("${GC[@]}" -C "$R" rev-parse HEAD | cut -c1-7)"

  # (1) A clean tree names the release and the commit, and nothing else.
  want_ok clean "$R" "v1.0.0+${SHA}"

  # (2) A tracked edit is exactly what -dirty means.
  printf 'edited\n' >>"$R/tracked.txt"
  want_ok dirty-tracked "$R" "v1.0.0+${SHA}-dirty"

  # (3) An untracked scratch file is not a change to what the image was built from.
  git -C "$R" checkout -q -- tracked.txt
  printf 'scratch\n' >"$R/untracked.txt"
  want_ok dirty-untracked "$R" "v1.0.0+${SHA}"

  # (3b) The dirty predicate itself, asserted directly. Cases (2) and (3) pin the
  # whole stamp; a bug that swapped -dirty for something else inside the composer
  # would still pass both. This calls the function the audit depends on.
  git -C "$R" clean -qfd
  dirty_check "$R" || fail "no-dirty-flag-on-a-clean-repo" "dirty_check said dirty on a clean repo"
  printf 'edited\n' >>"$R/tracked.txt"
  dirty_check "$R" && fail "dirty-flag-on-a-dirty-repo" "dirty_check said clean with a tracked edit"
  git -C "$R" checkout -q -- tracked.txt
  printf 'scratch\n' >"$R/untracked.txt"
  dirty_check "$R" || fail "no-dirty-flag-on-untracked-only" "dirty_check said dirty with only an untracked file"

  # (4) The v and the whole triple are mandatory: a bare number is not a release.
  printf '1.0.0\n' >"$R/version.txt"
  if out="$("$SELF" "$R" 2>&1)"; then
    fail missing-v 'a version.txt of [1.0.0] was accepted'
  else
    printf '%s' "$out" | grep -q 'version.txt' \
      || fail missing-v "refused without naming version.txt: [$out]"
  fi
  printf 'v1.0.0\n' >"$R/version.txt"

  # (5) No git at all: a source tarball still names its release, unadorned.
  N="$T/plain"; mkdir -p "$N"; printf 'v1.0.0\n' >"$N/version.txt"
  want_ok no-git "$N" 'v1.0.0'

  # (6) A repo with no commit: HEAD is not a SHA, and must not print as one.
  E="$T/empty"; mkdir -p "$E"; printf 'v1.0.0\n' >"$E/version.txt"
  "${GC[@]}" -C "$E" init -q -b main >/dev/null 2>&1
  want_ok empty-repo "$E" 'v1.0.0'

  # (7) A build stamp must name the tree it was read from, so a nested checkout
  # never inherits the enclosing repository's provenance.
  S="$T/repo/nested"; mkdir -p "$S"
  printf 'v1.0.0\n' >"$S/version.txt"
  "${GC[@]}" -C "$S" init -q -b main >/dev/null 2>&1
  want_ok nested-repo "$S" 'v1.0.0'

  # (8) The prerelease grammar and the width, both run through a whole build so
  # the checks sit on the production path and not in a helper nobody calls. The
  # expected value carries the provenance on purpose: rewriting version.txt is a
  # tracked edit, so this is also the case that proves a prerelease line still
  # gets its SHA and its -dirty.
  printf 'v1.3.0-rc.1\n' >"$R/version.txt"
  want_ok prerelease "$R" "v1.3.0-rc.1+${SHA}-dirty"
  printf 'v1.3.0-rc.01\n' >"$R/version.txt"
  "$SELF" "$R" >/dev/null 2>&1 \
    && fail prerelease-leading-zero 'a version.txt of [v1.3.0-rc.01] was accepted'
  printf 'v1.3.0-\n' >"$R/version.txt"
  "$SELF" "$R" >/dev/null 2>&1 \
    && fail prerelease-empty 'a version.txt of [v1.3.0-] was accepted'
  # 32 bytes: one over the field, so the width check answers before the grammar
  # check does. The plan's example string was 31 bytes and so fell through to the
  # grammar message, which is why this one has one more digit than it looks like it needs.
  printf 'v1234567890123456789012345678901\n' >"$R/version.txt"
  if out="$("$SELF" "$R" 2>&1)"; then
    fail oversize-version 'a 32-byte version line was accepted'
  else
    printf '%s' "$out" | grep -qi '31\|too long\|width' \
      || fail oversize-version "refused without naming the width: [$out]"
  fi

  [ "$FAILS" -eq 0 ] || { echo "SELFTEST FAILED ($FAILS case(s))" >&2; exit 1; }
  echo 'SELFTEST OK'
  exit 0
}

# valid_pre <prerelease> — semver 2.0.0 dot-separated identifiers: alnum-or-hyphen,
# and a numeric one may not carry a leading zero. That is the whole grammar rule;
# everything else about a release line is the width limit below.
#
# The widths are the reason the grammar caps each component at two digits.
# esp_app_desc_t.version holds 31 bytes and cmake enforces the full stamp against
# it, so the worst case has to fit: v99.99.99-rc.99+e1a2b3c-dirty is exactly 31. A
# three-digit component would need 32, and cmake would refuse the configure.
valid_pre() {
  local IFS='.' id
  for id in $1; do
    case "$id" in
      *[!0-9]*) ;;                              # non-numeric: any alnum-or-hyphen
      0*)     [ "$id" = "0" ] || return 1 ;;    # numeric, leading zero rejected
      esac
    case "$id" in
      *[!0-9A-Za-z-]* | '') return 1 ;;
    esac
  done
  return 0
}

# dirty_check <repo> — the audit's own question, so it gets its own function and
# its own test case. Only tracked content counts: an untracked scratch file does
# not change what the image was built from, and `git diff HEAD` ignores untracked
# paths by definition. --ignore-submodules keeps a nested checkout's own dirty
# state from reporting itself onto this tree's stamp.
#
# The root guard is what makes this testable from inside the hub's Go suite, which
# runs with the tools tree as its working directory but belongs to the enclosing
# repository. The audit's answer must depend only on the tree named on the command
# line. Without the guard, a caller inside an enclosing repository gets that
# repository's dirty state and commit, and serflash.sh audits an image against the
# wrong tree. `git -C "$1" rev-parse --show-toplevel` resolves the named tree to
# the repository root it actually belongs to, so a nested checkout or a scratch
# directory is refused rather than inheriting somebody else's provenance.
dirty_check() {
  local root top
  root="$(cd -P "$1" 2>/dev/null && pwd)" || return 1
  top="$(git -C "$1" rev-parse --show-toplevel 2>/dev/null)"
  [ -n "$top" ] && [ "$top" = "$root" ] || return 1
  git -C "$1" diff --quiet --ignore-submodules HEAD -- 2>/dev/null || return 1
  return 0
}

# sha_after_commit <repo> — the short SHA, or empty. --verify, not a bare rev-parse:
# on a repository with no commit yet a bare rev-parse echoes the literal string
# HEAD, and without -q it exits 0 with that string on stdout, so an empty repo
# would stamp itself v1.0.0+HEAD-dirty. A missing HEAD is a fact to report, not a
# fatal error — it is the legitimate state of a tree that has not been committed
# even once, and a source tarball has the same shape.
sha_after_commit() { git -C "$1" rev-parse -q --verify HEAD 2>/dev/null | cut -c1-7; }

# compose <repo> — the stamp for one tree, on stdout.
compose() {
  local root="$1" decl sha dirty
  [ -f "$root/version.txt" ] \
    || { echo "orb_version: no version.txt at $root" >&2; return 1; }

  # One line, nothing else. cmake's file(STRINGS) folds every line into the value,
  # so a comment line in version.txt would become part of the version.
  decl=$(head -n1 "$root/version.txt" | tr -d ' \t\r')
  [ "${#decl}" -le 31 ] \
    || { echo "orb_version: version.txt is ${#decl} B; esp_app_desc_t.version holds 31" >&2; return 1; }
  # Split on the first hyphen. The plan's pattern was `v[0-9][0-9]?*`, and bash
  # reads that as digit, digit, any-one-char, anything: it demands a digit in
  # position two and refuses v1.0.0 outright. A `?` after a bracket is a one-char
  # wildcard, not an optional quantifier. Anything that starts v-then-digit is a
  # candidate; the exact two-digit combos are what the next check enforces.
  case "$decl" in
    v[0-9]*)
      core="${decl%%-*}"
      case "$decl" in
        *-*)
          pre="${decl#*-}"
          # A trailing hyphen is an empty identifier, which semver has no room
          # for. The grammar message is the right answer for it: the line is
          # malformed, not too wide.
          [ -n "$pre" ] || {
            echo "orb_version: version.txt holds [$decl]; want v<MAJOR>.<MINOR>.<PATCH>[-<prerelease>], two digits or fewer per component, no leading zero in a numeric prerelease identifier" >&2
            return 1; }
          ;;
        *)   pre='' ;;
      esac
      ;;
    *)     core='';   pre=''   ;;
  esac
  case "$core" in
    v[0-9][0-9].[0-9][0-9].[0-9][0-9]) core_ok=1 ;;
    v[0-9].[0-9].[0-9] | v[0-9][0-9].[0-9].[0-9] | \
    v[0-9].[0-9].[0-9][0-9] | v[0-9].[0-9].[0-9][0-9] | \
    v[0-9][0-9].[0-9][0-9].[0-9] | v[0-9][0-9].[0-9].[0-9][0-9] | \
    v[0-9].[0-9][0-9].[0-9][0-9])       core_ok=1 ;;
    *)                                 core_ok=0 ;;
  esac
  { [ "$core_ok" -eq 1 ] && [ -n "$pre" ] && ! valid_pre "$pre"; } && {
    echo "orb_version: version.txt holds [$decl]; want v<MAJOR>.<MINOR>.<PATCH>[-<prerelease>], two digits or fewer per component, no leading zero in a numeric prerelease identifier" >&2
    return 1; }
  [ "$core_ok" -eq 1 ] || {
    echo "orb_version: version.txt holds [$decl]; want v<MAJOR>.<MINOR>.<PATCH>[-<prerelease>], two digits or fewer per component, no leading zero in a numeric prerelease identifier" >&2
    return 1; }

  # No git, no metadata: a source tarball still names the release it is.
  command -v git >/dev/null 2>&1 || { echo "$decl"; return 0; }
  git -C "$root" rev-parse --git-dir >/dev/null 2>&1 || { echo "$decl"; return 0; }
  sha=$(sha_after_commit "$root")
  [ -n "$sha" ] || { echo "$decl"; return 0; }   # a tree with no commit yet
  dirty=''
  dirty_check "$root" || dirty='-dirty'
  echo "${decl}+${sha}${dirty}"
}

case "${1:-}" in
  --self-test|--selftest)	selftest ;;
  *)	compose "${1:-$(cd "$(dirname "$0")/.." && pwd)}" ;;
esac

