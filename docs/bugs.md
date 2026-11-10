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
