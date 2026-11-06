#!/usr/bin/env bash
# release_notes.sh — a release's notes, as the release workflow drafts them (RELEASING.md).
#
#   tools/release_notes.sh v1.2.11            # print the notes (preview before tagging)
#   tools/release_notes.sh --check v1.2.11    # exit 1 unless docs/releases/v1.2.11.md is written
#
# Three parts, top to bottom:
#   1. docs/releases/<version>.md, written by hand for people deciding whether to update:
#      what is new, what changed, anything to do. One file per tag, the hub-only -2
#      rebuilds included. The workflow refuses to build a tag without it. Then the notes
#      of every tag since the last PUBLISHED release that was never published itself (a
#      draft thrown away, a tag rebuilt as -2), so nothing an owner was never told is lost.
#   2. The template every release carries: the firmware and apps built in, the downloads,
#      what each needs, the signing warnings (WIN_SIGNED / MAC_SIGNED = true when signed).
#   3. Every commit subject since the last published release, folded.
#
# "Published" is GitHub's answer (gh, with GH_TOKEN in the workflow): the newest published
# release whose tag this version descends from. Without gh it falls back to the previous
# tag and says so on stderr. Run from the repo root. A version not yet tagged is read at
# HEAD, so the preview is what tagging HEAD would draft.
set -euo pipefail

check=false
if [ "${1:-}" = "--check" ]; then check=true; shift; fi
VERSION="${1:?usage: tools/release_notes.sh [--check] <version>}"
notes="docs/releases/$VERSION.md"

if $check; then
  if [ ! -s "$notes" ]; then
    echo "::error::$notes is missing or empty: write what $VERSION brings users (docs/releases/README.md), commit it, and move the tag" >&2
    exit 1
  fi
  echo "release notes: $notes"
  exit 0
fi

[ -s "$notes" ] || { echo "no $notes yet (docs/releases/README.md says what goes in it)" >&2; exit 1; }

# The tag when it exists, else HEAD: the commit list is what tagging HEAD would cover.
ref=HEAD
git rev-parse -q --verify "refs/tags/$VERSION" >/dev/null && ref="$VERSION"

# The last release owners actually got: published, not this one, and an ancestor of it.
prev=""
if published=$(gh release list --exclude-drafts --exclude-pre-releases --limit 50 \
                 --json tagName -q '.[].tagName' 2>/dev/null); then
  for t in $published; do
    [ "$t" = "$VERSION" ] && continue
    git rev-parse -q --verify "refs/tags/$t" >/dev/null || continue
    if git merge-base --is-ancestor "$t" "$ref"; then prev=$t; break; fi
  done
else
  prev=$(git describe --tags --abbrev=0 "$ref^" 2>/dev/null || true)
  echo "release_notes: gh could not list releases; counting from the previous tag ($prev)" >&2
fi

cat "$notes"
# Tags between that release and this one that were never published on their own: their
# notes ride along, newest first, or nobody would ever read them.
if [ -n "$prev" ]; then
  for t in $(git tag --merged "$ref" --no-merged "$prev" | sort -rV); do
    [ "$t" = "$VERSION" ] && continue
    [ -s "docs/releases/$t.md" ] || continue
    echo
    echo "## Also in $t (never released on its own)"
    echo
    sed 's/^## /### /' "docs/releases/$t.md"
  done
fi
echo
echo "---"
echo
echo "Orb Manager $VERSION, with gorb-os $(cat version.txt) firmware built in."
echo
echo "Apps included:"
for f in tools/orb-manager/apps/*.orbapp; do
  s=$(basename "$f" .orbapp)
  echo "- $s $(cat "apps/$s/version.txt")"
done
echo
echo "| Download | For |"
echo "|---|---|"
echo "| orb-manager-$VERSION-windows-amd64.zip | Windows 10/11, 64-bit |"
echo "| orb-manager-$VERSION-macos.dmg | macOS 12 or later (Apple silicon and Intel) |"
echo "| orb-manager-$VERSION-linux-amd64.tar.gz | Linux x86-64 |"
echo
echo "Each needs Chrome, Edge, Brave or Chromium installed; Orb Manager opens its window with it."
echo
echo "orb-sdk-$(cat version.txt).tar.gz is the app SDK for this OS release, for writing apps outside this repo (apps/README.md)."
[ "${WIN_SIGNED:-}" = true ] || { echo; echo "The Windows build is not code-signed: SmartScreen will warn. Choose More info, then Run anyway."; }
[ "${MAC_SIGNED:-}" = true ] || { echo; echo "The macOS build is not notarized, so macOS says it could not verify Orb Manager. Choose Done, open System Settings > Privacy & Security, press Open Anyway beside Orb Manager, then open it again. (macOS 14 and earlier: right-click Orb Manager, choose Open, then Open.)"; }
if [ -n "$prev" ]; then
  echo
  echo "<details><summary>Every change since $prev, the last release</summary>"
  echo
  git log --no-merges --format='- %s' "$prev..$ref"
  echo
  echo "</details>"
fi
