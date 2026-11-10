#!/bin/sh
# The G4 chain's Go gate: vet, then the whole orb-manager suite.
# TestThePassParksForADemandAndResumesItsOwnQueue is a known flaky test (docs/bugs.md B3 on
# main): when it is the ONLY failure it is re-run alone before the gate is judged.
set -u
export GOFLAGS=-buildvcs=false PATH=/usr/local/go/bin:$PATH
cd "$(dirname "$0")/../../tools/orb-manager" || exit 1
go vet ./... || exit 1
log=$(mktemp)
if go test ./... >"$log" 2>&1; then rm -f "$log"; echo "go test: all green"; exit 0; fi
if grep -- '--- FAIL' "$log" | grep -v 'TestThePassParksForADemandAndResumesItsOwnQueue' | grep -q . \
   || ! grep -q -- '--- FAIL: TestThePassParksForADemandAndResumesItsOwnQueue' "$log"; then
  grep -E -- '--- FAIL|^FAIL|panic:|cannot|undefined' "$log" | head -40; tail -40 "$log"; rm -f "$log"; exit 1
fi
rm -f "$log"
echo "only the known flaky test failed; re-running it alone"
go test -run '^TestThePassParksForADemandAndResumesItsOwnQueue$' -count=2 .
