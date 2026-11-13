#!/usr/bin/env bash
# notary_wait.sh <submission-id> <dmg> <minutes>: asks Apple about a notarization submission
# once a minute (xcrun notarytool info) until it is answered or <minutes> pass. Accepted:
# staples the ticket to <dmg> and checks it with Gatekeeper. Invalid or Rejected: prints
# Apple's log and fails. Still In Progress at the end: not a failure. Writes state=<status>
# to $GITHUB_OUTPUT, so the caller decides what an unanswered submission means.
# Needs APPLE_API_KEY_P8 (base64), APPLE_API_KEY_ID and APPLE_API_ISSUER.
set -euo pipefail
id=$1 dmg=$2 minutes=$3
key="${RUNNER_TEMP:-/tmp}/AuthKey.p8"
echo "$APPLE_API_KEY_P8" | base64 --decode > "$key"
trap 'rm -f "$key"' EXIT
auth=(--key "$key" --key-id "$APPLE_API_KEY_ID" --issuer "$APPLE_API_ISSUER")
out() { [ -n "${GITHUB_OUTPUT:-}" ] && echo "state=$1" >> "$GITHUB_OUTPUT"; echo "notarization $id: $1"; }
end=$(( $(date +%s) + minutes * 60 ))
state=
while :; do
  # A network blip is not an answer: only a status Apple gave counts.
  if info=$(xcrun notarytool info "$id" "${auth[@]}" --output-format json 2>&1); then
    state=$(printf '%s' "$info" | sed -n 's/.*"status" *: *"\([^"]*\)".*/\1/p')
  else
    echo "notarytool info failed (retrying): $info"
  fi
  case $state in
    Accepted) break ;;
    Invalid | Rejected)
      xcrun notarytool log "$id" "${auth[@]}" || true
      out "$state"; echo "::error::Apple did not notarize $dmg ($state)"; exit 1 ;;
  esac
  if [ "$(date +%s)" -ge "$end" ]; then out "${state:-unknown}"; exit 0; fi
  echo "$(date -u +%H:%M:%S) ${state:-no answer}"
  sleep 60
done
xcrun stapler staple "$dmg"
spctl --assess --type open --context context:primary-signature --verbose=2 "$dmg"
out Accepted
