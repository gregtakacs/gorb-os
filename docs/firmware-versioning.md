# Firmware versioning

Every image this repo builds carries one stamp, in one field, composed by one script.

    v<MAJOR>.<MINOR>.<PATCH>[-<prerelease>]+<7-char SHA>[-dirty]
    v1.0.0+e1a2b3c        v1.2.0+e1a2b3c-dirty        v1.3.0-rc.1+e1a2b3c

## Why the metadata is mandatory

`version.txt` declares an intent. Anyone with write access can type any number into
it at any time, so a bare triple says nothing about the bytes on a board. The part
after the plus is a git fact: the SHA names the commit the tree was at when cmake
ran, and `-dirty` says the tree carried uncommitted edits at that moment. A board
that prints `v1.0.0+e1a2b3c` states which commit it runs. A board that prints
`v1.0.0+e1a2b3c-dirty` states that the question has no clean answer, which is the
honest result and the reason the flag exists.

The scheme replaces the hub's previous ordering rule. A git-describe stamp has no
version order, so `firmware.Compare` could only rank builds by the embedded compile
timestamp — a clock that a dev host with a skewed RTC can set backwards, which made
an upgrade decision depend on something that is not a fact about the code. The
declared triple now outranks the timestamp, and the timestamp survives only as the
fallback that lets a mixed fleet of legacy orbs keep working.

## Grammar

`tools/orb_version.sh` is the only composer. Both callers — root `CMakeLists.txt`
and `tools/serflash.sh` — read its output rather than computing anything.

    <stamp>      ::= <declared> [ "+" <sha> [ "-dirty" ] ]
    <declared>   ::= "v" <num> "." <num> "." <num> [ "-" <pre> ]
    <num>        ::= digit | digit digit            (0 through 99)
    <pre>        ::= <ident> ( "." <ident> )*
    <ident>      ::= [0-9A-Za-z-]+ , a numeric one without a leading zero
    <sha>        ::= 7 lowercase hex

Two rules are easy to get backwards and both are tested:

- A numeric prerelease identifier may not have a leading zero (`rc.01` is refused,
  `rc.1` and `rc.0` are not). Non-numeric identifiers may contain hyphens.
- `-dirty` follows the SHA, never precedes it. `v1.0.0-dirty+e1a2b3c` parses as the
  prerelease `dirty` and is a different, legal, meaningless thing.

`--self-test` runs 14 assertions, including the dirty predicate on its own, the
no-commit case, a nested checkout, the prerelease grammar and the width. It is the
only check in this repo that needs no ESP-IDF: ~2 s, no toolchain, no board.

## Width arithmetic

`esp_app_desc_t.version` is `char[32]`: 32 bytes of storage, 31 usable characters
plus a NUL. ESP-IDF copies with `strlcpy`, so an overlong stamp truncates silently
and the build reports the wrong version forever. cmake therefore enforces the whole
stamp against 31 before anything compiles.

    v99.99.99-rc.99+e1a2b3c-dirty     31 B   the two-digit grammar's worst case
    v1.0.0-rc.1+92424ff-dirty        23 B   this project's realistic worst case

The two-digit cap is arithmetic, not taste: the same worst case with one three-digit
component is 32 bytes and will not fit. The declared version is the only part a
human edits, and at two digits per component it tops out at `v99.99.99`.

## Bumping

    edit version.txt  ->  idf.py reconfigure  ->  idf.py build

The reconfigure is load-bearing. cmake runs the composer at configure time and
caches the result in `build/CMakeCache.txt`, so `idf.py build` alone recompiles your
edits and keeps the old stamp: an image carrying new code under an old version,
which is worse than no versioning at all because it looks clean. `idf.py
set-version` is not the fix — it re-runs IDF's own version finder, which ignores
`version.txt` entirely once `PROJECT_VER` is set.

`tools/serflash.sh` reads the stamp back out of `build/gorb-os.bin` at byte offset
48 and refuses the flash unless it equals the live composer output. `--check` runs
that audit without touching a board. The mismatch line prints the fix.

`orbpack` refuses to ship a stamp that is not a version, because the built-in bundle
is what an end user flashes with no choices made. `-force` overrides it for a bench
build; `firmware.Load` stays permissive so a person can still load an old `.zip` and
deliberately downgrade.

## The four surfaces that print it

| Surface | Where it is read |
|---|---|
| mDNS TXT `fw=` | `main/net/orb_mdns.c:342`, into `char fw[32]` at `orb_mdns.h:51` |
| HTTP status | `main/net/orb_net.c:2292`, printing `app->version` at `:2341` — no intermediate buffer |
| Serial boot | the bootloader's `app_init: app version:` line, from the same field |
| Bundle / hub API | `appDescriptor` in `tools/orb-manager/firmware.go`, `Release.Version` |

The hub answers the triple as a structured `semver` object on `/api/v1/firmware` and
in each `/api/v1/identity` device row, so no front end has to parse semver:
`{legacy, raw, declared, major, minor, patch, pre, commit, dirty}`. A pre-scheme stamp
reports `legacy: true`, and the UI prints the raw string for it.

## Reading a stamp without flashing anything

The descriptor lives at a fixed offset inside the app image, so a built `.bin` answers
the question on its own:

    python3 - <<'PY'
    import struct
    d = open('build/gorb-os.bin', 'rb').read()
    o = d.index(struct.pack('<I', 0xABCD5432))          # esp_app_desc_t magic
    print(d[o+16:o+48].split(b'\0')[0].decode())        # .version, 32 bytes
    PY

`cmake -LA | grep ORB_VERSION` shows what a configure resolved. If that disagrees with
`tools/orb_version.sh`, the cache is stale and the fix is `idf.py reconfigure`.
