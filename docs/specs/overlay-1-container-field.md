# overlay-1: the overlay colour's word in the FORMAT 3 container

> **Read first: `docs/specs/overlay-design.md` §2, §3, §8 (checks 1-3).** This step is the byte;
> the OS's read is overlay-2 and the editors' write is overlay-3.

## Goal

Turn the geometry index head's second word — the one `tools/orbbundle.py` has always written as a
literal zero and every reader has always ignored — into the theme's overlay colour:
`0x00RRGGBB` means no colour, `0x01RRGGBB` means the colour. Ship it on all four legs of the
three-way rule at once: the Python writer, the browser baker, the device's C reader, and the
sentinel suite that keeps them honest. No behaviour changes on any shipped theme: the new argument
defaults to the zero that is already in every file.

## Read first

- `docs/specs/overlay-design.md` §2 (why this word and not the header), §3 (the four legs, with
  the exact functions), §8 checks 1-3.
- `tools/orbbundle.py` — `pack_index` (:358), `unpack_index` (:366, note `_rsvd`), `write_bundle`
  (:404 signature, the region assembly at :491, `geom_crc` right after), `read_bundle`, and the
  module's refusal sentences the C copies byte for byte.
- `tools/orb-manager/web/runtime/baker@1/orbtheme-baker.js` — `packIndex3` (:3025), `unpackIndex3`
  (:3040), `writeBundle3` (:3099, the region assembly at :3170), `parseBundle3`/`verifyBundle3`,
  stage3's `pack` (:3800) and `finish` (:3809), `bakeClock3`'s pack call (:4338).
- `main/art/orb_bundle.h` — `orb_bundle_t` (:132), `ORB_BUNDLE_IDX_WORDS`/`ORB_BUNDLE_SECT_MAX`;
  `main/art/orb_bundle.c` — `IDX_HEAD_BYTES` (:48), `parse_core` (:410): the region CRC at :417,
  `b32(region)` at :424, the `memset(b, 0, sizeof *b)` at :433, and `orb_bundle_rebase` (:581-617)
  to see that a rebase touches only record bytes 24..27.
- `tools/host_orb_bundle_c_test.py` — the suite's whole shape: its SHIM/HARNESS, how it builds
  vectors with `ob.pack_index` (:393), how it dumps fields, and the `MUTATIONS` list it runs under
  `--mutations`. `tools/host_theme_format3_test.py` :255 for the writer's own index vectors.
- `tools/host_ring_test.py` (at RING_TIP) for the host-suite output style to match
  (`RESULT: pass`, `N of N mutants caught`).

## Contract

**`tools/orbbundle.py`.** `def pack_index(recs, overlay=0)` emitting
`struct.pack("<2I", len(recs), _u(overlay))`; `def write_bundle(path, name, sections, ...,
overlay=0)` forwarding it; a new `def index_overlay(region)` returning `struct.unpack_from("<I",
region, 4)[0]`, documented as the reader helper that never raises about the tag; and
`def file_overlay(path)`, which opens a written container, runs `parse_header3`, slices the region and
answers `index_overlay` of it — the one-line read-back the app gates, the shelf check and a bench
script all want, and the reason there is one file-level helper rather than three copies of the same
four lines. `unpack_index` still returns `(n, recs)` and still ignores the second word. The docstring
at `pack_index` says what the word now means, in one line, and names design §2.

**`orbtheme-baker.js`.** `function packIndex3(recs, overlay)`, and everywhere it is called from a
public entry point, the value comes from one optional `overlay` on the caller's options object:
`writeBundle3(o)` uses `o.overlay >>> 0`; stage3's `pack(m)` forwards `overlay: m.overlay || 0`;
`bakeClock3` forwards `overlay: o.overlay || 0` into `B.pack`. `verifyBundle3`/`parseBundle3`
report `overlay` in the object they return, read from the region — the browser leg's dump is how
check 2 compares the two writers.

**`main/art/orb_bundle.h`.** Two additions: `uint32_t overlay;` in `orb_bundle_t` next to
`n_sections`, documented as the raw word, and

```c
/* The bundle's overlay colour (§64.9), already tag-checked: 0x01RRGGBB, or 0 when the theme names
 * none — no word, a zero tag, or a tag this build does not know. The rule lives here so the C
 * reader, the installer's dump and orb_theme.c's accessor cannot each invent their own. */
uint32_t orb_bundle_overlay(const orb_bundle_t *b);
```

`orb_bundle_t` is public and `orb_bundle_parse_*` is its only writer, so no other caller sees a
stale value.

**`main/art/orb_bundle.c`.** One read in `parse_core`, after the count and cross-check that already
sit there (:422-431) and after the `memset` at :433:

```c
b->overlay = b32(region + 4);   /* §64.9: 0x01RRGGBB names a colour, anything else none */
```

That is the whole C change. `ORB_BUNDLE_IDX_WORDS`, `IDX_BYTES`, `IDX_HEAD_BYTES`, the walk, the
refusal sentences and the header asserts do not move; no new refusal is added, because an
unassigned tag is an absent colour and not damage — damage inside the region is already `geom_crc`'s
job (:416). Do not read the word in `orb_bundle3_hdr_parse`: the header is a 128-byte word array
and the field is in the region, and mixing them would put the read before the checksum that
protects it. `orb_bundle_overlay()` is three lines in the same file — `(b->overlay & 0xFF000000u)
== 0x01000000u ? b->overlay : 0u` — and is the only place the tag is interpreted.

**`tools/host_orb_bundle_c_test.py`.** Vectors, in the file's existing families:

1. A golden bundle written with `overlay=0x01135c79`: the C dump names the same word, and
   `ob.index_overlay()` of the same region agrees. The suite already prints a field dump per
   vector, so this rides the existing comparison rather than a new one.
2. The same bundle with the default: the dump says `overlay=0`.
3. `overlay=0x00ff8800` — a full RGB under an absent tag — and `overlay=0x02aabbcc` — an
   unassigned tag: both parse with no refusal at any door, the dump carries the raw word, and
   `orb_bundle_overlay()` answers 0 for both. These are the "absence, not damage" vectors, and the
   pair is what pins §64.9's fallback: a theme from a build that filled the low bits and left the
   tag must not be read as carrying that colour.
4. A forgery vector: flip one byte of the overlay word in a written file without restamping
   `geom_crc`, and require the C's sentence to be the region-checksum refusal the suite already
   knows. This is what pins the field as protected rather than trusted.
5. Two mutants in the `MUTATIONS` list: the writer's `overlay` forced to 0, and the reader's
   `b32(region + 4)` forced to 0. Each must make the run differ, so a silently dropped read or
   write cannot ship.

## Behaviour and edge cases

- Every theme already on the shelf, on a card or in a fixture still parses with the identical
  verdict and the identical bytes: the new argument's default is the zero already written, and the
  new read's value in those files is 0. Check 1 asserts the shelf's checksums do not move.
- `0x00RRGGBB` where `RRGGBB` is a real colour (a theme from a build that filled the low bits and
  left the tag) reads as **no colour**. That is the whole point of the tag byte, and vector 3 and
  the write mutant are what keep it from drifting.
- A region torn below `IDX_HEAD_BYTES` is refused before the word is read, so `overlay` is the
  `memset`'s 0 on every refusal path — never uninitialised, never a partial read.
- `orb_bundle_rebase` runs over a coloured region on the installer's path: the word is untouched (it
  rewrites record bytes 24..27 only) and `geom_crc` comes back recomputed, so the slot's copy still
  proves it. Add the dump line to an existing rebase vector, not a new family.
- Both writers land identical bytes for identical input: Python's `_u(overlay)` against the baker's
  `>>> 0`, one vector each side, compared by check 2.
- A two-section bundle written with a colour reads one word, once: the reader does not go
  per-section, and `unpack_index` and the hub's `indexSlugs` still count two sections.
- `0x01000000` (tag present, black) is a real colour and must read back as `0x01000000`, not as
  absence — the tag, not the value, is the presence bit.

## Acceptance criteria

All run on a board-free sandbox (gcc, python3, node). `make check-hub` is in the chain's command
because the hub ships the same runtime, but there is no Go toolchain on this box — run it where there
is one, and say so where there is not:

1. `python3 tools/host_orb_bundle_c_test.py | tail -1` → `RESULT: pass`.
2. `python3 tools/host_orb_bundle_c_test.py --mutations > /tmp/ov1m.log 2>&1` → the last line reads
   `N of N mutants caught` with N equal to the `MUTATIONS` count, and no `MISSED` line.
3. `python3 tools/host_theme_format3_test.py` → pass with **no edit to its vectors** (proof the
   writer's default is byte-identical to what it wrote before).
4. `make check-host` → every suite green, including `host_clock3_test.py` and
   `host_orbelems_test.py`, which both call `write_bundle`.
5. `grep -q 'def file_overlay' tools/orbbundle.py && grep -q 'def index_overlay' tools/orbbundle.py && grep -q 'orb_bundle_overlay' main/art/orb_bundle.h && grep -q 'b32(region + 4)' main/art/orb_bundle.c && grep -q 'packIndex3(recs, overlay' tools/orb-manager/web/runtime/baker@1/orbtheme-baker.js`
   → all four legs really changed, so the verify cannot pass by doing nothing.
6. `cd tools/orb-manager/web/runtime/_selftest && PY=$(command -v python3) node headless-test.js` →
   the baker suite green (it runs the Python baker as its reference).
7. `sha256sum tools/orb-manager/themes/*.orbtheme` recorded before and after the step → identical.
8. `python3 -c 'import sys,struct;sys.path.insert(0,"tools");import orbbundle as ob;assert ob.index_overlay(struct.pack("<2I",1,0x01135c79))==0x01135c79 and ob.index_overlay(struct.pack("<2I",1,0))==0;print("ok")'`
   → `ok`. The chain's verify runs this assertion plus the unassigned-tag case.

## Out of scope

`main/art/orb_theme.c` and `orb_theme.h` (overlay-2), the ring (`orb_ring.*`, `apps/sdk/orb.h`,
`apps/README.md` — overlay-2), every editor and bake module (overlay-3), `tabs.js` and the Go hub
(nothing displays the word yet), `apps/*/version.txt` and the shelf (overlay-4). No new FORMAT, no
header word, no change to `orb_bundle3_hdr_parse`, no new refusal sentence: the C's refusal table
must come out of this step byte-identical, which is check 3's real job.

## Notes

- The field's power is that it is *already zero*. If the suite's golden bundles change bytes, the
  default was not that zero; if a shipped theme's checksum moves, this step stopped being additive
  and check 7 caught it.
- `parse_core` is shared by the whole-buffer door and the installer's stream door
  (`orb_bundle_parse_region`), so one read covers boot, install and bind — which is why the step
  adds no second accessor for the region.
- Keep the C to one statement and three lines of helper, the Python to one argument and one
  function. The size of this step is the vectors, not the code.

