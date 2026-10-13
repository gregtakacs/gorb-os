#!/usr/bin/env python3
"""host_orbapp_test.py — the `.orbapp` container, tested on the host (P4).

Why this file exists
--------------------
tools/orbapp.py is the only sanctioned writer of the container (C11), and the
bytes it emits ARE the D19 consent: the caps word it writes is what the broker's
gate treats as granted, and the install confirm is the one place the owner ever
agrees to it. A writer bug here is not a wrong label. Declaring `net` where the
author wrote `power` ships an app holding a socket the owner never read about,
and nothing on the dial can raise that ceiling — so the ceiling has to be right,
and only a test can say so.

Two legs, not three — said out loud
-----------------------------------
The theme path's three-way mirror rule (writer, C reader, sentinel test) needs a
C reader, and P4 has not landed one: the installer reads the `apps` partition,
which the owner still has to decide (§3.2). This suite makes NO reader
round-trip claim. What it pins today, with no board and no reader:

  1. THE CAP ORDER — orbapp's tuples against orb_svc.h's orb_cap_t, lifted by
     regex, order AND count. A renumber moves every installed app's consent to a
     different permission while the ladder keeps looking right.
  2. THE SETTING TYPES — same lift against orb_set_type_t, whose order is the
     wire value of the type word (C9, frozen). A mismatch reads an INT as ENUM.
  3. THE SHARED NUMBERS — the quota, the descriptor widths and the D16 floor,
     each against the C that enforces them.
  4. THE WIRE OFFSETS, spelled independently of orbapp's own constants, so a
     reordered field fails HERE rather than becoming the future reader's bug.
  5. THE SENTINEL ROUND TRIP, a distinct value in every field.
  6. THE REFUSALS — every rule that exists to stop a bad install gets a vector
     that tries to break it, and the reason must name what it refused.

Run:  python3 tools/host_orbapp_test.py
      python3 tools/host_orbapp_test.py --mutations
"""

import os, re, struct, subprocess, sys, tempfile

ROOT = (sys.argv[sys.argv.index("--root") + 1] if "--root" in sys.argv
        else os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
P = lambda *a: os.path.join(ROOT, *a)
SVC_H, SVC_C = P("main", "app", "orb_svc.h"), P("main", "app", "orb_svc.c")
APP_H = P("main", "app", "orb_app.h")
for p in (SVC_H, SVC_C, APP_H):
    if not os.path.exists(p):
        sys.exit("missing " + p + " — the pins below lift it and have no fallback")

# --writer re-points the IMPORT only. A mutation run perturbs the writer and
# still lifts the real C headers: the pins exist to catch the writer drifting
# from the device, and a mutant scratch dir has no main/app/ to read.
WDIR = (sys.argv[sys.argv.index("--writer") + 1] if "--writer" in sys.argv
        else P("tools"))
sys.path.insert(0, WDIR)
import orbapp as A

svc_h, svc_c, app_h = open(SVC_H).read(), open(SVC_C).read(), open(APP_H).read()

CHECKS = []


def check(what, ok, detail=""):
    CHECKS.append((what, bool(ok), detail))
    print("%-4s %s%s" % ("ok" if ok else "FAIL", what,
                         "" if ok else "  <- " + str(detail)[:120]))


def expect_refuse(what, fn, wants=None):
    """The refusal must happen, name what it refused, and — when the caller says
    which rule is under test — be refused BY that rule.

    The `wants` argument is load-bearing. Without it a vector can be refused by
    a neighbour of the rule it targets and the suite reports green while the
    rule under test enforces nothing: four of the first mutation runs proved
    exactly that (a forged caps word refused by the free-mask check instead of
    the CAP_N check, an empty slug refused by the charset check)."""
    try:
        fn()
    except ValueError as e:
        msg = str(e)
        if len(msg) <= 20:
            check(what, False, "refused, but the reason is too thin: %s" % msg)
        elif wants and not any(w in msg for w in (wants if isinstance(wants, tuple)
                                                 else (wants,))):
            check(what, False, "refused by the WRONG rule: %s" % msg[:110])
        else:
            check(what, True)
    except Exception as e:                       # wrong type = wrong bug
        check(what, False, "raised %s, not ValueError: %s" % (type(e).__name__, e))
    else:
        check(what, False, "NOT REFUSED")


# ---- 1/2. the enums, lifted from the C the device actually builds -----------

m = re.search(r"typedef enum \{(.*?)\} orb_cap_t;", svc_h, re.S)
if not m:
    sys.exit("could not lift orb_cap_t from orb_svc.h — did the gate's vocabulary "
             "move? Dying here is the point: a pin that lifted nothing would keep "
             "reporting the cap order correct forever.")
c_caps = [n.strip() for n in re.findall(r"CAP_(\w+)\s*=", m.group(1))
          if n.strip() != "N"]
check("cap order matches orb_svc.h's orb_cap_t (the wire numbering)",
      tuple(n.lower() for n in c_caps) == A.CAP_FREE + A.CAP_OWNER,
      "C says %s, the writer says %s" % (c_caps, A.CAP_FREE + A.CAP_OWNER))
check("CAP_N is the count both sides assume", len(c_caps) == A.CAP_BITS,
      "C %d vs writer %d" % (len(c_caps), A.CAP_BITS))

m = re.search(r"typedef enum \{(.*?)\} orb_set_type_t;", svc_h, re.S)
if not m:
    sys.exit("could not lift orb_set_type_t from orb_svc.h")
c_types = [n.strip().lower() for n in re.findall(r"ORB_SET_(\w+)", m.group(1))]
check("setting types match orb_set_type_t, in order (C9, frozen)",
      tuple(c_types) == A.SET_TYPES,
      "C %s vs writer %s" % (c_types, A.SET_TYPES))



# ---- 3. the numbers that bind the writer to the gate -----------------------

m = re.search(r"#define ORB_CFG_QUOTA_BYTES\s+(\d+)", svc_h)
check("the quota the writer prices settings against is the gate's",
      bool(m) and int(m.group(1)) == A.CFG_QUOTA_BYTES,
      "lifted %s vs CFG_QUOTA_BYTES %d" % (m.group(1) if m else "?",
                                           A.CFG_QUOTA_BYTES))
m = re.search(r"char\s+id\[(\d+)\];.*?char\s+def_str\[(\d+)\];", svc_h, re.S)
if not m:
    sys.exit("could not lift orb_setting_desc_t's id/def_str widths from orb_svc.h")
check("the settings id field is the width the descriptor has",
      int(m.group(1)) == A.ID_MAX, "C id[%s] vs ID_MAX %d" % (m.group(1), A.ID_MAX))
check("the STRING default field is the width the descriptor has",
      int(m.group(2)) == A.DEFSTR_MAX,
      "C def_str[%s] vs DEFSTR_MAX %d" % (m.group(2), A.DEFSTR_MAX))
check("the broker still refuses a beat faster than the manifest floor (D16)",
      "period_ms < a->period_ms" in svc_c,
      "orb_svc.c no longer compares against the manifest's period, so the floor "
      "this writer enforces has nothing on the device side to agree with")
check("the registry still holds the manifest fields P4 fills",
      "caps" in app_h and "period_ms" in app_h and "n_settings" in app_h,
      "orb_app.h lost the fields the container exists to fill")

# ---- 4. the wire, spelled independently of the writer's own constants ------
# Literal byte offsets read off §4.1's field order, NOT computed from orbapp's
# word map: if the two ever disagree, one of them moved, and the P4 reader would
# be written against the wrong one.
WIRE = ((0, "magic", A.MAGIC), (4, "format", A.FORMAT), (8, "geom_abi", A.GEOM_ABI),
        (12, "hdr_size", A.HDR_WORDS * 4), (24, "abi_min", A.GEOM_ABI),
        (28, "abi_max", A.GEOM_ABI), (32, "sig", 0), (36, "app_id", None))
hdr = A.pack_header(A.app_id_of("pin"), "pin", "Pin", 0, 0, 0, (0, 0, 0, 0), b"", 0)
raw = b"".join(struct.pack("<I", v) for v in hdr)
for off, nm, want in WIRE:
    got = struct.unpack_from("<I", raw, off)[0]
    if want is None:
        want = A.app_id_of("pin")
    check("the word @%d is %s, where the paper puts it" % (off, nm),
          got == want, "found 0x%X, expected 0x%X" % (got, want))

# The two name ranges must not touch: the theme path's FORMAT-1 scar was exactly
# two hand-placed ranges disagreeing, hidden until a long slug overwrote a title.
slug_lo, slug_hi = A.W_SLUG * 4, A.W_SLUG * 4 + A.SLUG_MAX
tit_lo, tit_hi = A.W_TITLE * 4, A.W_TITLE * 4 + A.TITLE_MAX
check("the slug and title ranges are disjoint", slug_hi <= tit_lo,
      "slug %d..%d overlaps title %d..%d" % (slug_lo, slug_hi, tit_lo, tit_hi))
check("the name fields end before the manifest words begin", tit_hi <= A.W_BEHAVIOUR * 4)
long_t = A.pack_header(A.app_id_of("pin"), "pin", "T" * (A.TITLE_MAX - 1), 0, 0, 0,
                       (0, 0, 0, 0), b"", 0)
check("a full-length title cannot eat the slug",
      A._getstr(long_t, A.W_SLUG, A.SLUG_MAX) == "pin",
      "slug came back %r" % A._getstr(long_t, A.W_SLUG, A.SLUG_MAX))
check("a full-length title survives intact",
      A._getstr(long_t, A.W_TITLE, A.TITLE_MAX) == "T" * (A.TITLE_MAX - 1))

# The consent sits inside the checksummed header: the property that makes D19
# survivable, and the one a future "move the manifest into the blob" change would
# break without any other test noticing.
check("caps is inside the header the CRC covers",
      A.W_CAPS < A.W_HDR_CRC and A.HDR_WORDS > A.W_HDR_CRC,
      "W_CAPS=%d W_HDR_CRC=%d HDR_WORDS=%d" % (A.W_CAPS, A.W_HDR_CRC, A.HDR_WORDS))
check("period_ms, the counts and the settings count are inside it too",
      max(A.W_PERIOD_MS, A.W_N_SETTINGS, A.W_N_BANK) < A.W_HDR_CRC,
      "manifest words run to %d, crc sits at %d" % (A.W_HDR_CRC, A.HDR_WORDS))


# ---- 5. the sentinel round trip -------------------------------------------
# A DIFFERENT value in every field. A pair of fields that share a word, or one
# that overwrites its neighbour, shows up here as a mismatch rather than as an
# app whose declared `net` turns out to be `power`.

SENT = dict(id="weather", title="Weather",
            caps=A.cap_bit("power") | A.cap_bit("sd_read") | A.cap_bit("themes"),
            period_ms=4000, counts=(1, 2, 3, 4), serial=0x5E51, baked_ms=0x1234)
SETTS = [dict(id="city", type="string", def_str="Denver"),
         dict(id="offset", type="int", min=-720, max=720, default=-1),
         dict(id="units", type="enum", min=0, max=2, default=2),
         dict(id="alarm", type="toggle", default=1)]
GEOM = bytes((i * 7) & 0xFF for i in range(4 * 10))       # 10 words, opaque
ART = bytes((i * 13 + 1) & 0xFF for i in range(4096 * 3))  # 3 pages, opaque

D = tempfile.mkdtemp(prefix="orbapp-")
PATH = os.path.join(D, "weather.orbapp")
n = A.write_container(PATH, SENT["id"], SENT["title"], caps=SENT["caps"],
                      settings=SETTS, geometry=GEOM, art=ART,
                      period_ms=SENT["period_ms"], counts=SENT["counts"],
                      serial=SENT["serial"], baked_ms=SENT["baked_ms"],
                      behaviour_kind=0xABCD)
h, st, geom, art = A.read_container(PATH)

FIELDS = (("fmt", A.FORMAT), ("abi", A.GEOM_ABI), ("hdr_size", A.HDR_WORDS * 4),
          ("serial", SENT["serial"]), ("baked_ms", SENT["baked_ms"]),
          ("abi_min", A.GEOM_ABI), ("abi_max", A.GEOM_ABI), ("sig", 0),
          ("app_id", A.app_id_of(SENT["id"])), ("behaviour_kind", 0xABCD),
          ("period_ms", SENT["period_ms"]), ("caps", SENT["caps"]),
          ("n_settings", len(SETTS)), ("n_layer", 1), ("n_comp", 2),
          ("n_aux", 3), ("n_bank", 4), ("art_req", len(ART)),
          ("art_size", len(ART)), ("geom_size", len(GEOM)),
          ("slug", SENT["id"]), ("title", SENT["title"]))
for k, want in FIELDS:
    check("field %s carries its own sentinel through the write" % k,
          h[k] == want, "wrote %r, read %r" % (want, h[k]))
check("the geometry blob is byte-identical (opaque, not reinterpreted)",
      geom == GEOM, "%d B vs %d B" % (len(geom), len(GEOM)))
check("the art payload is byte-identical", art == ART,
      "%d B vs %d B" % (len(art), len(ART)))
check("every settings descriptor round-trips its four bounds",
      st == [dict(id="city", type="string", min=0, max=0, default=0,
                  def_str="Denver"),
             dict(id="offset", type="int", min=-720, max=720, default=-1,
                  def_str=""),
             dict(id="units", type="enum", min=0, max=2, default=2, def_str=""),
             dict(id="alarm", type="toggle", min=0, max=1, default=1,
                  def_str="")], st)
check("a negative min survives the sign through the wire (int32, not u32)",
      st[1]["min"] == -720 and st[1]["default"] == -1, st[1])
check("the file is the sum of its declared parts, no stray bytes",
      n == h["art_off"] + h["art_size"], "%d vs %d" % (n, h["art_off"] + h["art_size"]))
check("the payload starts sector-aligned", h["art_off"] % A.SECTOR == 0,
      hex(h["art_off"]))
check("the settings table sits right after the header", h["setting_off"] == A.SET_OFF)
check("the geometry follows the settings table exactly",
      h["geom_off"] == A.SET_OFF + len(SETTS) * A.SETTINGS_WORDS * 4,
      hex(h["geom_off"]))
check("the container is bigger than a bare header only because of its payloads",
      n > A.HDR_BYTES and h["art_size"] == len(ART))

# Each CRC must actually be load-bearing: tear one payload byte and the read
# names the part, without ever consulting the other.
def tear(off):
    b = bytearray(open(PATH, "rb").read())
    b[off] ^= 0xFF
    open(PATH, "rb").close()
    with open(PATH, "wb") as f:
        f.write(bytes(b))
    return PATH

p = tear(h["art_off"]); A.read_header(p)          # header still clean
try:
    A.read_container(p, verify=True)
except ValueError as e:
    check("a torn art byte is named as ART, not as a generic failure",
          "art checksum" in str(e), e)
else:
    check("a torn art byte is named as ART, not as a generic failure", False,
          "read clean")
p2 = os.path.join(D, "weather.orbapp")             # same file, next tear
b = bytearray(open(p2, "rb").read())
b[h["geom_off"]] ^= 0xFF
with open(p2, "wb") as f:
    f.write(bytes(b))
try:
    A.read_container(p2, verify=True)
except ValueError as e:
    check("a torn geometry byte is named as GEOMETRY, so re-copy beats re-bake",
          "geometry checksum" in str(e), e)
else:
    check("a torn geometry byte is named as GEOMETRY, so re-copy beats re-bake",
          False, "read clean")

# And the D19 property, on a PRISTINE file: the earlier vectors tore this one in
# place, and a checksum vector aimed at damaged bytes proves nothing. Two claims,
# both necessary: a one-bit edit of the consent must fail the header CRC, and a
# forger who fixes the CRC anyway must still be stopped by the semantic read.
C = tempfile.mkdtemp(prefix="orbapp-crc-")
cp = os.path.join(C, "weather.orbapp")
A.write_container(cp, SENT["id"], SENT["title"], caps=SENT["caps"],
                  settings=SETTS, geometry=GEOM, art=ART,
                  period_ms=SENT["period_ms"], counts=SENT["counts"],
                  serial=SENT["serial"], baked_ms=SENT["baked_ms"])
b = bytearray(open(cp, "rb").read())
b[A.W_CAPS * 4] ^= 0x10                      # ask for `time`: a free cap
try:
    A.parse_header(bytes(b))
except ValueError as e:
    check("one bit of the CONSENT fails the header CRC", "checksum" in str(e), e)
else:
    check("one bit of the CONSENT fails the header CRC", False,
          "the caps word is not covered — an app could gain a cap on the card")
b = bytearray(open(cp, "rb").read())         # pristine again, then forge both
w = A._words(bytes(b[:A.HDR_BYTES]))
w[A.W_CAPS] = 1                              # declare a free cap...
w[A.W_HDR_CRC] = A._hdr_crc(w)               # ...and re-stamp the checksum
forged = b"".join(struct.pack("<I", v) for v in w) + bytes(b[A.HDR_BYTES:])
try:
    A.check_header(A.parse_header(forged))
except ValueError as e:
    check("a re-CRC'd header still cannot declare a free cap",
          "free caps" in str(e) or "owner caps" in str(e), e)
else:
    check("a re-CRC'd header still cannot declare a free cap", False,
          "only the checksum guards the consent")
open(os.path.join(C, "forged.orbapp"), "wb").write(forged)
try:                                     # a card scan must not list this row
    A.read_header(os.path.join(C, "forged.orbapp"))
except ValueError as e:
    check("the scan path refuses the forged container too, not only the check",
          "caps" in str(e), e)
else:
    check("the scan path refuses the forged container too, not only the check",
          False, "read_header accepted it")

# ---- 6. the refusals -------------------------------------------------------
# One vector per rule that exists to stop a bad install. Each must refuse AND
# say what it refused.

expect_refuse("a declared free cap (time) is refused", lambda: A.check_caps(1 << 0))
expect_refuse("a declared free cap (art) is refused", lambda: A.check_caps(1 << 3))
expect_refuse("every free cap at once is refused", lambda: A.check_caps(0x0F))
expect_refuse("a bit at or above CAP_N is refused, not ignored",
              lambda: A.check_caps(1 << A.CAP_BITS), wants="CAP_N")
expect_refuse("negative caps is not a mask", lambda: A.check_caps(-1))
expect_refuse("caps past a u32 is not a word", lambda: A.check_caps(1 << 32))
for owner in A.CAP_OWNER:                        # the four legal ones, each alone
    check("declaring '%s' alone is accepted" % owner,
          A.check_caps(A.cap_bit(owner)) == A.cap_bit(owner))
check("the four owner caps together are accepted",
      A.check_caps(A.CAP_OWNER_MASK) == A.CAP_OWNER_MASK)
check("declaring nothing is legal and says so",
      A.check_caps(0) == 0 and A.caps_names(0) == [])
check("caps_names lists them in orb_cap_t order, not spelling order",
      A.caps_names(A.cap_bit("themes") | A.cap_bit("net")) == ["net", "themes"],
      A.caps_names(A.cap_bit("themes") | A.cap_bit("net")))

expect_refuse("an app id with a slash is refused", lambda: A.check_slug("a/b"))
expect_refuse("an app id that is a dot-dot is refused", lambda: A.check_slug(".."))
expect_refuse("an app id with a space is refused", lambda: A.check_slug("my app"),
              wants="lowercase")
expect_refuse("an upper-case app id is refused", lambda: A.check_slug("Weather"),
              wants="lowercase")
expect_refuse("an app id with a UTF-8 byte is refused",
              lambda: A.check_slug("caf\u00e9"), wants="lowercase")
expect_refuse("an app id at 24 chars is refused", lambda: A.check_slug("x" * 24))
expect_refuse("an empty app id is refused", lambda: A.check_slug(""))
check("an app id at 23 chars is the last one accepted",
      A.check_slug("x" * 23) == "x" * 23)
check("app_id_of is stable for a slug", A.app_id_of("clock") == A.app_id_of("clock"))
check("two near-identical slugs do not collide on app_id",
      A.app_id_of("clock") != A.app_id_of("cl0ck"))

expect_refuse("a beat below the commit floor is refused", lambda: A._period(2999))
check("the beat floor itself is accepted", A._period(A.BEAT_MIN_MS) == A.BEAT_MIN_MS)
check("0 means never ticks and is accepted", A._period(0) == 0)
expect_refuse("a beat past a day is refused (say 0 instead)",
              lambda: A._period(A.BEAT_MAX_MS + 1))
expect_refuse("art over the slot is refused with its byte count",
              lambda: A._art_size(A.ART_BYTES_MAX + 4))
expect_refuse("a reservation under the payload it must hold is refused",
              lambda: A._art_req(10, 4096))
expect_refuse("a bank-less app claiming pages is refused",
              lambda: A._art_req(4096, 0))
expect_refuse("a geometry blob shorter than its counts is refused",
              lambda: A._geom_size(8, (0, 0, 0, 40)))
expect_refuse("a ragged geometry blob is refused (the format is words)",
              lambda: A._geom_size(6, (0, 0, 0, 0)))
expect_refuse("an oversized geometry blob is refused",
              lambda: A._geom_size(A.GEOM_BYTES_MAX + 4, (0, 0, 0, 0)))
expect_refuse("a title over the field is refused, never truncated",
              lambda: A.pack_header(A.app_id_of("pin"), "pin", "T" * A.TITLE_MAX,
                                    0, 0, 0, (0, 0, 0, 0), b"", 0))
expect_refuse("an inverted ABI range is refused (no board could install it)",
              lambda: A.pack_header(A.app_id_of("pin"), "pin", "P", 0, 0, 0,
                                    (0, 0, 0, 0), b"", 0, abi=(3, 2)))
expect_refuse("an ABI range excluding this writer is refused (C4)",
              lambda: A.pack_header(A.app_id_of("pin"), "pin", "P", 0, 0, 0,
                                    (0, 0, 0, 0), b"", 0, abi=(2, 9)))
expect_refuse("a filled sig word is refused while signing is unimplemented (C3)",
              lambda: A.pack_header(A.app_id_of("pin"), "pin", "P", 0, 0, 0,
                                    (0, 0, 0, 0), b"", 0, sig=1))


def _fmt2():
    """A container claiming FORMAT 2, CRC recomputed so only the format check
    can stop it: a reader that ignored FORMAT would parse this as a v1 app."""
    w = A.pack_header(A.app_id_of("pin"), "pin", "P", 0, 0, 0, (0, 0, 0, 0), b"", 0)
    w[A.W_FORMAT] = 2
    w[A.W_HDR_CRC] = A._hdr_crc(w)
    return b"".join(struct.pack("<I", v) for v in w)


# (what, settings, the phrase ONLY that rule's message contains). The third
# element is what makes a mutant of a single rule actually fail: without it a
# vector refused by a neighbouring check reports green.
SET_BAD = [
    ("a duplicate setting id",
     [dict(id="a", type="int", min=0, max=1, default=0)] * 2, "declared twice"),
    ("a default outside its own min..max",
     [dict(id="a", type="int", min=0, max=1, default=9)], "outside its own"),
    ("min above max", [dict(id="a", type="int", min=5, max=1, default=2)],
     "above max"),
    ("a toggle defaulting to 2", [dict(id="a", type="toggle", default=2)],
     "is a toggle"),
    ("a 16-char setting id (the NVS key limit)",
     [dict(id="k" * 16, type="int", min=0, max=1, default=0)], "15-char"),
    ("a def_str on an int",
     [dict(id="a", type="int", min=0, max=1, default=0, def_str="hi")],
     "only STRING has"),
    ("an unknown type", [dict(id="a", type="float", min=0, max=1, default=0)],
     "is not one of"),
    ("one setting past the table",
     [dict(id="k%d" % i, type="int", min=0, max=1, default=0)
      for i in range(A.SETTINGS_MAX + 1)], "table holds"),
    ("a STRING default over the field",
     [dict(id="a", type="string", def_str="x" * A.DEFSTR_MAX)], "UTF-8 bytes"),
    ("a setting id with a space",
     [dict(id="a b", type="int", min=0, max=1, default=0)], "lowercase"),
    ("a value that overflows int32",
     [dict(id="a", type="int", min=0, max=0x80000000, default=0)], "int32"),
]
for what, ss, wants in SET_BAD:
    expect_refuse("settings: " + what, lambda ss=ss: A.check_settings(ss),
                  wants=wants)
# The toggle's bounds are frozen to 0..1 whatever the caller claims, because that
# is what the gate accepts; a manifest saying otherwise describes a setting no
# one can use.
check("a toggle's min/max are forced to 0..1 regardless of what was asked",
      A.check_settings([dict(id="a", type="toggle", min=-5, max=99,
                             default=1)]) == [("a", 2, 0, 1, 1, "")],
      A.check_settings([dict(id="a", type="toggle", min=-5, max=99, default=1)]))
# 16 STRINGs at 31 B is 512 B: inside the 2048 B quota, so the cap that binds is
# SETTINGS_MAX and not the byte charge — and the charge is per value, not per row.
BIG = [dict(id="k%d" % i, type="string", def_str="x" * 31) for i in range(16)]
check("16 string settings still fit the quota (512 B of 2048 B)",
      len(A.check_settings(BIG)) == 16)
check("the charge is the value's bytes, so a declared default costs what it costs",
      A.check_settings([dict(id="a", type="string", def_str="y" * 31)])[0][5]
      == "y" * 31)
# The charge is REPORTED, never refused, and this pins why: 16 settings x 32 B
# is 512 B against a 2048 B quota, so no legal manifest can trip a writer-side
# quota refusal. A refusal that can never fire reads as coverage and is a
# comment; the bytes that consume the quota are the values the app writes at
# runtime, and orb_svc.c's cfg_set_str already bills and refuses those (pinned
# by host_svc_test.py). cfg_charge exists to print the number, not to gate it.
check("a STRING default charges strlen+1, the shape the broker bills",
      A.cfg_charge(A.check_settings([dict(id="a", type="string",
                                          def_str="y" * 31)])) == 32)
check("an int setting charges the four bytes the gate stores",
      A.cfg_charge(A.check_settings([dict(id="a", type="int", min=0, max=9,
                                          default=1)])) == 4)
check("the largest reachable declared charge fits the quota, so refusing on it would be dead code",
      A.SETTINGS_MAX * A.DEFSTR_MAX <= A.CFG_QUOTA_BYTES,
      "%d B against a %d B quota" % (A.SETTINGS_MAX * A.DEFSTR_MAX,
                                     A.CFG_QUOTA_BYTES))

# ---- the identity triple ---------------------------------------------------
expect_refuse("a package filed under the wrong slug",
              lambda: A.filed_as_its_own_slug("other.orbapp",
                                              A.app_id_of("timer"), "timer"),
              wants="is not the header's slug")
expect_refuse("a theme handed to the app loader by NAME is refused",
              lambda: A.filed_as_its_own_slug("diver.orbtheme",
                                              A.app_id_of("timer"), "timer"))
expect_refuse("a staged .part file is refused, not listed",
              lambda: A.filed_as_its_own_slug("timer.orbapp.part",
                                              A.app_id_of("timer"), "timer"))
expect_refuse("a header whose app_id disagrees with its slug is refused",
              lambda: A.check_header(dict(h, app_id=0xDEADBEEF)))
expect_refuse("an empty slug is refused at read, not only at write",
              lambda: A.check_header(dict(h, slug="")), wants="no slug")
expect_refuse("an empty title is refused (the confirm would have no subject)",
              lambda: A.check_header(dict(h, title="")))
expect_refuse("caps smuggled into a parsed header are refused",
              lambda: A.check_header(dict(h, caps=1 << 0)))
expect_refuse("an art_req under the payload is refused on read too",
              lambda: A.check_header(dict(h, art_req=4)))
expect_refuse("a beat under the floor is refused on read too",
              lambda: A.check_header(dict(h, period_ms=100)))
expect_refuse("a blob too short for its counts is refused on read too",
              lambda: A.check_header(dict(h, geom_size=4)))
expect_refuse("an art size over the slot is refused on read too",
              lambda: A.check_header(dict(h, art_size=A.ART_BYTES_MAX + 1)))
expect_refuse("a settings count over the table is refused on read too",
              lambda: A.check_header(dict(h, n_settings=A.SETTINGS_MAX + 1)))
check("a header that came from the writer still passes the read checks",
      A.check_header(dict(h)) == h)
expect_refuse("write_container will not file an app under a stranger's name",
              lambda: A.write_container(os.path.join(D, "imposter.orbapp"),
                                        "timer", "Timer"))
check("a .part write is honoured as staging and keeps the slug",
      A.write_container(os.path.join(D, "staged.orbapp.part"), "staged", "Staged")
      > A.HDR_BYTES and os.path.exists(os.path.join(D, "staged.orbapp.part")))
expect_refuse("a theme's magic is refused by name, not just by number",
              lambda: A.parse_header(struct.pack("<I", 0x474F5254)
                                     + b"\0" * (A.HDR_BYTES - 4)))
expect_refuse("a short file is refused before any field is trusted",
              lambda: A.parse_header(b"\0" * 16))
expect_refuse("a newer FORMAT is refused rather than half-read",
              lambda: A.parse_header(_fmt2()))

# ---- the disclosure, which IS the consent (D19) ----------------------------
txt = A.disclosure(h["slug"], h["title"], h["caps"], h["period_ms"],
                   h["art_size"], h["n_settings"])
for want in ("weather", "Weather", "power", "sd_read", "themes", "4000",
             str(len(ART)), str(len(SETTS)), "PERMISSIONS"):
    check("the install confirm names %r" % want, want in txt, txt[:100])
check("the confirm lists exactly the declared owner caps, in order",
      "power, sd_read, themes" in txt, txt)
check("the confirm never offers a free cap as revocable",
      "time" not in txt.split("may use:")[1].split("\n")[0], txt)
check("nothing declared says so in the ladder's words",
      "nothing declared" in A.disclosure("x", "X", 0, 0, 0, 0))
check("the period is the app's fastest, not a runtime rate",
      "fastest beat" in txt, txt)


# ---- the mutation harness ---------------------------------------------------
# Each entry disables ONE rule in tools/orbapp.py. A mutant that still passes the
# suite is a rule the suite never actually tried to break — which is how a consent
# check quietly becomes a comment. Targets are matched exactly once or the pin is
# reported stale rather than silently skipped.
MUTATIONS = [
    ("free caps accepted as declarations",
     "free = caps & ~CAP_OWNER_MASK", "free = 0"),
    ("a bit at or above CAP_N ignored",
     "if caps >> CAP_BITS:", "if 0:"),
    ("the cap order loses an entry (a silent renumber)",
     'CAP_OWNER = ("power", "net", "sd_read", "themes")',
     'CAP_OWNER = ("power", "net", "themes")'),
    ("the setting types lose one (wire renumber)",
     'SET_TYPES = ("int", "enum", "toggle", "string")',
     'SET_TYPES = ("int", "enum", "string")'),
    ("the beat floor is gone", "if period_ms < BEAT_MIN_MS:", "if 0:"),
    ("the art slot cap is gone", "if art_size > ART_BYTES_MAX:", "if 0:"),
    ("art_req may understate the payload", "if art_req < art_size:", "if 0:"),
    ("a bank-less app may reserve pages",
     "if art_size == 0:\n        if art_req:", "if art_size == 0:\n        if 0:"),
    ("the counts no longer price the blob", "if geom_size < need:", "if 0:"),
    ("the slug charset check is gone",
     "if not isinstance(slug, str) or not SLUG_RE.match(slug):", "if 0:"),
    ("dupe setting ids allowed", "if sid in seen:", "if 0:"),
    ("the setting default-range check is gone", "elif not (mn <= df <= mx):", "elif 0:"),
    ("the toggle's two words are not enforced", "if df not in (0, 1):", "if 0:"),
    ("the setting id width check is gone",
     "if not isinstance(sid, str) or not SET_RE.match(sid):", "if 0:"),
    ("the CRC word moves below the manifest it must cover",
     "W_HDR_CRC    = W_ART_CRC    + 1", "W_HDR_CRC    = W_SIG           + 1"),
    ("the settings table cap is gone", "if len(settings) > SETTINGS_MAX:", "if 0:"),
    ("the header CRC check is gone", "if hdr[W_HDR_CRC] != _hdr_crc(hdr):", "if 0:"),
    ("the FORMAT check is gone", "if hdr[W_FORMAT] != FORMAT:", "if 0:"),
    ("the reserved sig word is accepted filled", "if sig != 0:", "if 0:"),
    ("the filename may disagree with the slug",
     "    if stem != slug:\n        raise ValueError(\"filename stem",
     "    if 0:\n        raise ValueError(\"filename stem"),
    ("app_id no longer derives from the slug", "if h[\"app_id\"] != want:", "if 0:"),
    ("the name ranges may overlap",
     "W_TITLE      = W_SLUG      + ((SLUG_MAX   + 3) // 4)",
     "W_TITLE      = W_SLUG      + 1"),
    ("an empty slug parses", "if not h[\"slug\"]:", "if 0:"),
    ("an empty title parses", "if not h[\"title\"]:", "if 0:"),
]


def run_mutations():
    src = open(P("tools", "orbapp.py")).read()
    rc = 0
    for name, old, new in MUTATIONS:
        if src.count(old) != 1:
            print("FAIL  mutation %-48r appears %d times — the pin is stale"
                  % (name, src.count(old)))
            rc = 1
            continue
        tmp = tempfile.mkdtemp(prefix="orbapp-mut-")
        os.makedirs(os.path.join(tmp, "tools"))
        open(os.path.join(tmp, "tools", "orbapp.py"), "w").write(
            src.replace(old, new, 1))
        # --writer re-points the orbapp import at the mutant; the C lifts stay
        # on the real tree, so only the writer under test has moved.
        r = subprocess.run([sys.executable, P("tools", "host_orbapp_test.py"),
                            "--writer", os.path.join(tmp, "tools")],
                           capture_output=True, text=True)
        caught = "FAIL" in r.stdout or r.returncode != 0
        if caught:
            first = next((l for l in r.stdout.splitlines()
                          if l.startswith("FAIL")), "suite died: " + r.stderr[:60])
            print("ok    mutation %-48r caught: %s" % (name, first[:66]))
        else:
            print("FAIL  mutation %-48r SURVIVED — that rule has no cover" % (name,))
            rc = 1
    return rc


def main():
    if "--mutations" in sys.argv:
        return run_mutations()
    bad = [c for c in CHECKS if not c[1]]
    print("\n%d checks, %d failed" % (len(CHECKS), len(bad)))
    for what, _, detail in bad:
        print("  FAIL %s  %s" % (what, str(detail)[:140]))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
