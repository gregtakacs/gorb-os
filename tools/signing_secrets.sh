#!/usr/bin/env bash
# signing_secrets.sh — load the release workflow's signing secrets into the repo (RELEASING.md).
#
#   tools/signing_secrets.sh windows                        # the six AZURE_* secrets
#   tools/signing_secrets.sh mac cert.p12 AuthKey_XXXX.p8   # the six Apple ones
#   tools/signing_secrets.sh check                          # which are set (names only)
#
# Every value is typed at a hidden prompt or read from the file named, so nothing lands
# in shell history, the terminal or a chat. The .p12 and .p8 go in as single-line base64,
# which the workflow decodes; base64 -i is macOS's spelling and wraps lines on Linux, so
# the files are encoded here, the same on both. Run from the repo root, with gh logged in
# as someone who can set the repo's secrets.
#
# GitHub never gives a secret back. With SIGNING_SECRETS_DIR set, each value is also kept
# there, one owner-only file per secret named as the secret (the .p12 and .p8 as their
# base64): a value already on file is loaded without asking, a missing one is asked for
# and filed. `mac` then needs no file arguments once both are filed. The files are plain
# text; the directory is the owner's to back up and keep out of every repo.
set -euo pipefail

die() { echo "signing_secrets.sh: $*" >&2; exit 1; }
command -v gh >/dev/null || die "needs the gh CLI, logged in (gh auth login)"

dir=${SIGNING_SECRETS_DIR:-}
if [ -n "$dir" ]; then
	umask 077
	mkdir -p "$dir"
	chmod 700 "$dir"
fi

# A secret from its file in $dir, else typed at a hidden prompt after a line saying what it is.
ask() {
	echo
	if [ -n "$dir" ] && [ -s "$dir/$1" ]; then
		gh secret set "$1" <"$dir/$1"
		echo "$1 set from $dir/$1"
		return
	fi
	echo "$1 — $2"
	if [ -z "$dir" ]; then
		gh secret set "$1"
		return
	fi
	local v
	read -rsp "value (hidden): " v
	echo
	[ -n "$v" ] || die "$1: empty value"
	printf '%s' "$v" >"$dir/$1"
	gh secret set "$1" <"$dir/$1"
	echo "$1 set, and filed in $dir/$1"
}

# A file's bytes as one line of base64, or the base64 already filed in $dir.
file_secret() {
	if [ -z "${2:-}" ] && [ -n "$dir" ] && [ -s "$dir/$1" ]; then
		gh secret set "$1" <"$dir/$1"
		echo "$1 set from $dir/$1"
		return
	fi
	[ -f "${2:-}" ] || die "no file for $1: usage: $0 mac <cert.p12> <AuthKey_XXXX.p8>"
	if [ -n "$dir" ]; then
		base64 <"$2" | tr -d '\n' >"$dir/$1"
		gh secret set "$1" <"$dir/$1"
		echo "$1 set from $2, and filed in $dir/$1"
	else
		base64 <"$2" | tr -d '\n' | gh secret set "$1"
		echo "$1 set from $2"
	fi
}

case ${1:-} in
windows)
	ask AZURE_TENANT_ID "Directory (tenant) ID, from gorb-os-ci's Overview"
	ask AZURE_CLIENT_ID "Application (client) ID, from gorb-os-ci's Overview"
	ask AZURE_CLIENT_SECRET "the client secret's Value (not its Secret ID)"
	ask AZURE_SIGNING_ENDPOINT "the signing account's URI, e.g. https://eus.codesigning.azure.net/"
	ask AZURE_SIGNING_ACCOUNT "the signing account's name"
	ask AZURE_CERT_PROFILE "the certificate profile's name"
	;;
mac)
	file_secret MACOS_CERT_P12 "${2:-}"
	file_secret APPLE_API_KEY_P8 "${3:-}"
	ask MACOS_CERT_PASSWORD "the .p12 export password"
	ask MACOS_SIGN_IDENTITY "security find-identity's string, e.g. Developer ID Application: Your Name (TEAMID1234)"
	ask APPLE_API_KEY_ID "the API key's Key ID"
	ask APPLE_API_ISSUER "the Issuer ID above the keys table"
	;;
check) ;;
*) die "usage: $0 windows | mac <cert.p12> <AuthKey_XXXX.p8> | check" ;;
esac

echo
have=$(gh secret list --json name -q '.[].name')
for s in AZURE_TENANT_ID AZURE_CLIENT_ID AZURE_CLIENT_SECRET AZURE_SIGNING_ENDPOINT \
	AZURE_SIGNING_ACCOUNT AZURE_CERT_PROFILE MACOS_CERT_P12 MACOS_CERT_PASSWORD \
	MACOS_SIGN_IDENTITY APPLE_API_KEY_P8 APPLE_API_KEY_ID APPLE_API_ISSUER; do
	f=
	[ -n "$dir" ] && [ -s "$dir/$s" ] && f="  (filed)"
	if grep -qx "$s" <<<"$have"; then echo "  set      $s$f"; else echo "  missing  $s$f"; fi
done
