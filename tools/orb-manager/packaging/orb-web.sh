#!/bin/sh
# orb-web.sh — the release's web file: the editors, the runtime they load, and the API
# the hub answers for them (orb-os-net design §6.1).
#
#   orb-web.sh <version> <out dir> <orb-manager binary>
#
# It builds <out dir>/orb-web-<version>.tar.gz, one top folder with runtime/ (laid out by
# the binary's own export-runtime, so the release's Linux build decides the runtime the
# release's editors run against), apps/ (every shelf .orbapp) and contract/ (the hub's
# editor API fixture, which the site replays), plus version.txt. A site unpacks it, runs
# `orb-manager extract-editors` over apps/ and serves the editors beside that runtime.
#
# The archive is reproducible: sorted names, one fixed mtime, root owner, one mode set,
# gzip -n. Two runs on the same tree give the same bytes, so a sha256 in a release's
# sha256sums.txt names the file and not the minute it was packed. GNU tar is assumed
# (Linux; the release workflow builds it on ubuntu).
set -eu

here=$(cd "$(dirname "$0")" && pwd)
pkg=$(dirname "$here")

die() { echo "orb-web.sh: $*" >&2; exit 1; }

[ $# -eq 3 ] || die "usage: orb-web.sh <version> <out dir> <orb-manager binary>"
ver=$1 out=$2 bin=$3
[ -f "$bin" ] && [ -x "$bin" ] || die "no executable orb-manager at $bin — run make release first"
name="orb-web-$ver"
file="$out/$name.tar.gz"
if [ -f "$file" ]; then die "$file already exists: remove it to rebuild it"; fi

stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT INT TERM
root="$stage/$name"
mkdir -p "$root/runtime" "$root/apps" "$root/contract"

set -- "$pkg"/apps/*.orbapp
[ -f "$1" ] || die "no .orbapp files in $pkg/apps — run make apps first"
cp "$@" "$root/apps/"
"$bin" export-runtime "$root/runtime" >/dev/null
cp -R "$pkg/contract/." "$root/contract/"
printf '%s\n' "$ver" > "$root/version.txt"

mkdir -p "$out"
LC_ALL=C TZ=UTC tar -C "$stage" --sort=name --format=gnu \
	--mtime='1980-01-01 00:00:00Z' --owner=0 --group=0 --numeric-owner \
	--mode='u+rw,go+r,go-w' -cf - "$name" | gzip -n -9 > "$file"

echo "$file"
sha256sum "$file"
