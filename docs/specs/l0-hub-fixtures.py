#!/usr/bin/env python3
"""L0's fixture gate for the gorb-os chain (docs/specs/l0-design.md §3).

    python3 docs/specs/l0-hub-fixtures.py pre        # step 1: both schema-1 fixtures carry inline samples
    python3 docs/specs/l0-hub-fixtures.py clock      # step 3: clock schema-2 carries the sine as the orb's own file
    python3 docs/specs/l0-hub-fixtures.py pomodoro   # step 4: the same for the pomodoro

Run from the gorb-os root. Exit 0 and one "ok" line per check, or the first failure.
"""
import base64
import json
import math
import struct
import sys

N = 2205
SINE = [int(math.floor(8000 * math.sin(2 * math.pi * 660 * i / 22050) + 0.5)) for i in range(N)]
PREFIX = "data:audio/wav;base64,"


def fail(msg):
    print("FAIL " + msg)
    sys.exit(1)


def load(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def samples_of(v):
    """A legacy inline array: a list, or a {"0": v, ...} object."""
    if isinstance(v, list):
        return [int(x) for x in v]
    if isinstance(v, dict):
        keys = sorted((k for k in v if k.isdigit()), key=int)
        return [int(v[k]) for k in keys]
    return None


def expected_file():
    """l0-design §3.1's file for SINE, its body from the reference encoder (tools/theme_schema.py)."""
    sys.path.insert(0, "tools")
    import theme_schema as ts
    enc, body = ts.sound_encode(SINE)
    if enc == ts.SOUND_ENC_ADPCM:
        blocks = len(body) // 132
        head = struct.pack("<4sI4s4sIHHIIHHHH4sII4sI", b"RIFF", 52 + 132 * blocks, b"WAVE", b"fmt ", 20, 0x11, 1, 22050, 11370,
                           132, 4, 2, 256, b"fact", 4, N, b"data", 132 * blocks)
    else:
        head = struct.pack("<4sI4s4sIHHIIHH4sI", b"RIFF", 36 + 2 * N, b"WAVE", b"fmt ", 16, 1, 1, 22050, 44100, 2, 16, b"data", 2 * N)
    return head + bytes(body)


def check_file(url, where):
    if not isinstance(url, str) or not url.startswith(PREFIX):
        fail("%s is not a WAV data URL" % where)
    b = base64.b64decode(url[len(PREFIX):], validate=True)
    if base64.b64encode(b).decode() != url[len(PREFIX):]:
        fail("%s: the base64 is not canonical" % where)
    if b != expected_file():
        fail("%s is not l0-design §3.1's file for the sine (the orb's encoding, sound_encode's bytes)" % where)


def no_pcm(obj, where):
    if isinstance(obj, dict):
        if "pcm" in obj:
            fail("%s still carries pcm" % where)
        for k, v in obj.items():
            no_pcm(v, where + "." + k)
    elif isinstance(obj, list):
        for i, v in enumerate(obj):
            no_pcm(v, "%s[%d]" % (where, i))


def pomodoro_upload(doc, key):
    ups = (doc.get("design") or {}).get("uploads") or []
    hits = [u for u in ups if isinstance(u, dict) and u.get("name") == "l0-chime.wav" and key in u]
    if len(hits) != 1:
        fail("the pomodoro fixture has %d l0-chime.wav uploads carrying %s" % (len(hits), key))
    return hits[0]


def main(mode):
    if mode == "pre":
        c = load("apps/clock/editor/_fixtures/schema-1.orbdesign")
        if c.get("schema") != 1 or c.get("app") != "clock":
            fail("clock schema-1 fixture is not a schema-1 clock design")
        e = ((c.get("sounds") or {}).get("up") or {}).get("chime_hour") or {}
        if e.get("name") != "l0-chime.wav" or samples_of(e.get("pcm")) != SINE:
            fail("clock schema-1 fixture's sounds.up.chime_hour.pcm is not the 2,205-sample sine")
        print("ok clock schema-1 carries the sine inline")
        p = load("apps/pomodoro/editor/_fixtures/schema-1.orbdesign")
        if p.get("schema") != 1 or p.get("app") != "pomodoro":
            fail("pomodoro schema-1 fixture is not a schema-1 pomodoro design")
        if samples_of(pomodoro_upload(p, "pcm")["pcm"]) != SINE:
            fail("pomodoro schema-1 fixture's upload pcm is not the 2,205-sample sine")
        print("ok pomodoro schema-1 carries the sine inline")
    elif mode == "clock":
        c = load("apps/clock/editor/_fixtures/schema-2.orbdesign")
        if c.get("schema") != 2 or c.get("app") != "clock":
            fail("clock schema-2 fixture is not a schema-2 clock design")
        no_pcm(c.get("sounds"), "sounds")
        e = ((c.get("sounds") or {}).get("up") or {}).get("chime_hour") or {}
        if e.get("name") != "l0-chime.wav":
            fail("clock schema-2 fixture's chime_hour is not l0-chime.wav")
        check_file(e.get("wav"), "clock schema-2 chime_hour.wav")
        print("ok clock schema-2 carries the sine as the orb's own file")
    elif mode == "pomodoro":
        p = load("apps/pomodoro/editor/_fixtures/schema-2.orbdesign")
        if p.get("schema") != 2 or p.get("app") != "pomodoro":
            fail("pomodoro schema-2 fixture is not a schema-2 pomodoro design")
        no_pcm((p.get("design") or {}).get("uploads"), "design.uploads")
        u = pomodoro_upload(p, "wav")
        check_file(u["wav"], "pomodoro schema-2 upload wav")
        if u.get("samples") != N:
            fail("pomodoro schema-2 fixture's upload says %r samples" % u.get("samples"))
        print("ok pomodoro schema-2 carries the sine as the orb's own file")
    else:
        fail("usage: l0-hub-fixtures.py pre|clock|pomodoro")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "")
