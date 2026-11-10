# Bug ledger

Known bugs that aren't fixed yet. Add new ones at the bottom with the next id. When a bug is
fixed, set its status to `fixed`, add the commit or release, and leave the entry in place.

| Status | Meaning |
|---|---|
| `open` | reported, not yet looked at |
| `investigating` | someone is on it; the cause isn't known yet |
| `fixed` | fixed; the entry says where (commit or release) |
| `wontfix` | deliberately left as is; the entry says why |

---

## B1. Audio files larger than about 800 KB won't load or save

- **Status:** open
- **Reported:** 2026-11-10 by greg
- **Area:** audio in themes and editors. The cause isn't known yet: it could be a size cap in
  the editor, the bake or the hub upload, or the on-orb theme slot.
- **Symptom:** an audio file over about 800 KB fails to load, and fails to save.
- **Repro:** not written down yet. Add one: which app's editor, which file and size, and
  whether it fails in the editor, on Download/bake, on send to an orb, or on the orb itself.
- **Notes:** —

## B3. TestThePassParksForADemandAndResumesItsOwnQueue is flaky

- **Status:** open
- **Reported:** 2026-11-10, found while checking G3
- **Area:** Orb Manager's Go tests (`make check-hub`)
- **Symptom:** the test fails now and then with no code change. It failed 2 of 10 runs
  (`go test -count=10 -run '^TestThePassParksForADemandAndResumesItsOwnQueue$' .`) on `main` at
  ea04c30, inside `delegate-worker:gorb2`, and once in a full `check-hub` run on the G3 branch. It looks
  like timing-dependent queue parking rather than a real fault.
- **Repro:** the `-count=10` command above, from `tools/orb-manager`.
- **Notes:** CI can fail on it at random, so a red `check-hub` should be re-run before it's believed.
