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

Three legs, standing
--------------------
The theme path's three-way mirror rule (writer, C reader, sentinel test): the
C reader is main/app/orb_app_manifest.c and its sentinel is
tools/host_orbapp_reader_test.py, which compiles the shipping reader and pins
its refusals — in the writer's own words — against this file's answers. What
THIS suite owns, with no board and no reader in sight:

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
            period_ms=4000, counts=(1, 2, 3, 4), serial=0x5E51, baked_ms=0x1234,
            art_req=0x10000)   # a D21 declaration with no payload behind it
SETTS = [dict(id="city", type="string", def_str="Denver"),
         dict(id="offset", type="int", min=-720, max=720, default=-1),
         dict(id="units", type="enum", min=0, max=2, default=2),
         dict(id="alarm", type="toggle", default=1)]
GEOM = bytes((i * 7) & 0xFF for i in range(4 * 10))       # 10 words, opaque
ART = b""                                                 # D21: none ships
# P5b (D23): the sentinel's table carries one row of every shape the format can
# express — an owner-scoped slot, a required global one, and a hand with a non-square
# box — so the file-level round trip exercises the offset arithmetic against a table
# whose size is neither zero nor a round number, and a writer that misplaced the
# geometry blob by one row would trip on the geometry CRC rather than pass.
ASSETS = [dict(name="dial", cls="sprite", x=24, y=24, w=200, h=200),
          dict(name="bg", cls="plate", ns="global", required=1,
               x=0, y=0, w=466, h=466),
          dict(name="hour", cls="hand", x=224, y=120, w=8, h=104)]

D = tempfile.mkdtemp(prefix="orbapp-")
PATH = os.path.join(D, "weather.orbapp")
n = A.write_container(PATH, SENT["id"], SENT["title"], caps=SENT["caps"],
                      settings=SETTS, geometry=GEOM,
                      art_req=SENT["art_req"],
                      period_ms=SENT["period_ms"], counts=SENT["counts"],
                      serial=SENT["serial"], baked_ms=SENT["baked_ms"],
                      behaviour_kind=0xABCD, assets=ASSETS)
h, st, geom, art = A.read_container(PATH)

FIELDS = (("fmt", A.FORMAT), ("abi", A.GEOM_ABI), ("hdr_size", A.HDR_WORDS * 4),
          ("serial", SENT["serial"]), ("baked_ms", SENT["baked_ms"]),
          ("abi_min", A.GEOM_ABI), ("abi_max", A.GEOM_ABI), ("sig", 0),
          ("app_id", A.app_id_of(SENT["id"])), ("behaviour_kind", 0xABCD),
          ("period_ms", SENT["period_ms"]), ("caps", SENT["caps"]),
          ("n_settings", len(SETTS)), ("n_layer", 1), ("n_comp", 2),
          ("n_aux", 3), ("n_bank", 4), ("art_req", SENT["art_req"]),
          ("art_size", 0), ("geom_size", len(GEOM)),
          ("slug", SENT["id"]), ("title", SENT["title"]))
for k, want in FIELDS:
    check("field %s carries its own sentinel through the write" % k,
          h[k] == want, "wrote %r, read %r" % (want, h[k]))
check("the geometry blob is byte-identical (opaque, not reinterpreted)",
      geom == GEOM, "%d B vs %d B" % (len(geom), len(GEOM)))
check("the container ships no art payload (D21)", art == ART,
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
# ---- P5b: the declared table through the whole file ------------------------
# Compared against the INPUT and not against a restatement of it: the row that comes
# back must be the row that went in, namespace and box included, or the writer and
# its own reader agree on a shape the device will not see.
check("the asset count reaches the header as its own word",
      h["n_assets"] == len(ASSETS), "%d vs %d" % (h["n_assets"], len(ASSETS)))
check("every asset row round-trips its four fields",
      [(r["name"], r["cls"], r["ns"], r["required"]) for r in h["assets"]] ==
      [(r["name"], r["cls"], r.get("ns", "owner"), r.get("required", 0))
       for r in ASSETS], h["assets"])
check("a declared box survives the wire unchanged",
      [(r["x"], r["y"], r["w"], r["h"]) for r in h["assets"]] ==
      [(r["x"], r["y"], r["w"], r["h"]) for r in ASSETS],
      [(r["x"], r["y"], r["w"], r["h"]) for r in h["assets"]])
check("the reader assembles an owner reference from the file's own slug",
      h["assets"][0]["ref"] == "weather/dial", h["assets"][0].get("ref"))
check("the global namespace comes back without the owner's slug on it",
      h["assets"][1]["ref"] == "global/bg", h["assets"][1].get("ref"))
check("the asset table lies between the settings and the geometry",
      h["asset_off"] == A.SET_OFF + len(SETTS) * A.SETTINGS_WORDS * 4
      and h["geom_off"] == h["asset_off"] + len(ASSETS) * A.ASSET_WORDS * 4,
      "%d / %d / %d" % (h["setting_off"], h["asset_off"], h["geom_off"]))
check("the table's own bytes do not push the geometry blob",
      geom == GEOM, "the geometry read back damaged")
check("the file is the sum of its declared parts, no stray bytes",
      n == h["art_off"] + h["art_size"], "%d vs %d" % (n, h["art_off"] + h["art_size"]))
check("the payload starts sector-aligned", h["art_off"] % A.SECTOR == 0,
      hex(h["art_off"]))
check("the settings table sits right after the header", h["setting_off"] == A.SET_OFF)
check("the geometry follows the asset table exactly (ABI 2)",
      h["geom_off"] == A.SET_OFF + len(SETTS) * A.SETTINGS_WORDS * 4
                       + len(ASSETS) * A.ASSET_WORDS * 4,
      hex(h["geom_off"]))
check("the container is bigger than a bare header only because of its geometry",
      n > A.HDR_BYTES and h["art_size"] == 0 and h["art_crc"] == 0)

# Each CRC must actually be load-bearing: tear a payload byte and the read
# names the part. The art region no longer exists to be torn — a file that
# CLAIMS art bytes at all is refused at the read gate itself, which is this
# vector: patch a clean header's art_size (fixing the header CRC, so the
# refusal can only come from the D21 gate) and read_header must name it.
def patch_hdr_file(path, word, value):
    b = bytearray(open(path, "rb").read())
    w = A._words(bytes(b[:A.HDR_BYTES]))
    w[word] = value
    w[A.W_HDR_CRC] = A._hdr_crc(w)
    for i, v in enumerate(w):
        b[i * 4:i * 4 + 4] = struct.pack("<I", v)
    with open(path, "wb") as f:
        f.write(bytes(b))
    return path

p = patch_hdr_file(PATH, A.W_ART_SIZE, 4096)
try:
    A.read_header(p)
    check("a file claiming art bytes is refused at the read gate (D21)", False,
          "read clean")
except ValueError as e:
    check("a file claiming art bytes is refused at the read gate (D21)",
          "no container ships art" in str(e), e)
patch_hdr_file(PATH, A.W_ART_SIZE, 0)          # hand the golden back
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
                  settings=SETTS, geometry=GEOM,
                  art_req=SENT["art_req"],
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
expect_refuse("a declared art need past the bound is refused as a mis-count",
              lambda: A._art_req(A.ART_BYTES_MAX + 4))
# ---- P5b: the writer's own asset-table door (D23, D26, D31) -----------------
# Each refusal names the field it refuses, and the vector passes `wants` so a
# neighbour rule cannot take the credit. The C reader re-applies every one of these
# to bytes it did not write, and tools/host_orbapp_reader_test.py drives that side.
ROW = dict(name="dial", cls="sprite", x=8, y=8, w=16, h=16)
check("a legal row packs to the row's word count",
      len(A.pack_assets([ROW], "weather")) == A.ASSET_WORDS * 4)
check("an empty table is the empty string and not a zero-length row",
      A.pack_assets([], "weather") == b"")
check("a row's namespace is a word and not a string a caller pattern-matches",
      A.unpack_assets(A.pack_assets(
          [dict(ROW, ns="global")], "weather"), 1)[0]["ns"] == "global")
expect_refuse("a class outside the frozen vocabulary is refused by the class rule",
              lambda: A.check_assets([dict(ROW, cls="bitmap")]), wants="class")
expect_refuse("a class named by number is refused, not coerced",
              lambda: A.check_assets([dict(ROW, cls=1)]), wants="class")
expect_refuse("a namespace other than owner or global is refused by name",
              lambda: A.check_assets([dict(ROW, ns="weather")]), wants="namespace")
expect_refuse("an uppercase slot name is refused as a reference, not downcased",
              lambda: A.check_assets([dict(ROW, name="Dial")]), wants="lowercase")
expect_refuse("a slot name over the row's field is refused with its own bound",
              lambda: A.check_assets([dict(ROW, name="a" * (A.ASSET_NAME_MAX))]),
              wants="1..%d" % (A.ASSET_NAME_MAX - 1))
expect_refuse("a slash inside a slot name is refused: it is the separator",
              lambda: A.check_assets([dict(ROW, name="a/b")]), wants="lowercase")
expect_refuse("a box off the right edge is refused and never clipped",
              lambda: A.check_assets([dict(ROW, x=A.PANEL, w=1)]), wants="panel")
expect_refuse("a negative coordinate is refused as a bake that lost a sign",
              lambda: A.check_assets([dict(ROW, y=-8)]), wants="panel")
expect_refuse("a required flag that is not a bit is refused",
              lambda: A.check_assets([dict(ROW, required=2)]), wants="one bit")
expect_refuse("one row past the table is refused before the bytes exist",
              lambda: A.check_assets([ROW] * (A.ASSET_MAX + 1)), wants="holds")
expect_refuse("a non-integer box is refused as a count and not coerced",
              lambda: A.check_assets([dict(ROW, w="16")]), wants="count")
# The reference bound is the row's own limit crossed with the slug's. Worked out it
# is unreachable: the field is ORB_ASSET_OWNER_MAX + 1 + ORB_ASSET_NAME_MAX, the row's
# name check caps the name at ORB_ASSET_NAME_MAX - 1, and the slug's own check caps it
# at ORB_ASSET_OWNER_MAX, so the widest legal pair is `23 + 1 + 11 + 1` = the field
# exactly. pack_assets' guard therefore has no input that reaches it, which is the
# good outcome and worth pinning: the pair is checked HERE, where a violation is a
# message, rather than in the resolver, where it would be a silent truncation on the
# LVGL task. What the vector proves is that the row's bound fires and not the
# reference's, so the guard cannot be quietly replaced by the overflow it guards.
expect_refuse("the row's own name bound fires before any reference overflow can",
              lambda: A.pack_assets(
                  [dict(ROW, name="a" * A.ASSET_NAME_MAX)], "z" * 23),
              wants="1..%d" % (A.ASSET_NAME_MAX - 1))
check("the widest reference the field holds is accepted at the boundary",
      len(A.pack_assets([dict(ROW, name="a" * (A.ASSET_NAME_MAX - 1))],
                        "z" * 23)) == A.ASSET_WORDS * 4)
check("and that boundary is the field's own size, to the byte",
      len(A.asset_ref(dict(ROW, name="a" * (A.ASSET_NAME_MAX - 1)), "z" * 23)) + 1
      == A.ASSET_REF_MAX, A.ASSET_REF_MAX)
expect_refuse("a table written without the owner it declares cannot be packed",
              lambda: A.write_container(os.path.join(D, "orphan.orbapp"), "orphan",
                                        "Orphan", assets=[dict(ROW, ns="bogus")]),
              wants="namespace")
check("the row's required and namespace bytes do not collide",
      (struct.unpack("<I", A.pack_assets([dict(ROW, required=1, ns="global")],
                                         "weather")[0:4])[0] & 0xFFFF00) == 0x010100,
      hex(struct.unpack("<I", A.pack_assets([dict(ROW, required=1, ns="global")],
                                            "weather")[0:4])[0]))
check("unpack reads a signed pair back as signed",
      A.unpack_assets(A.pack_assets([dict(ROW, x=0, y=0)], "w"), 1)[0]["y"] == 0)
check("the old slot size itself is still declarable (the bound is inclusive)",
      A._art_req(A.ART_BYTES_MAX) == A.ART_BYTES_MAX)
check("a D21 declaration with no payload is the normal shape, accepted",
      A._art_req(4096) == 4096)
try:
    A.write_container(os.path.join(D, "noart.orbapp"), "noart", "NoArt",
                      art=b"\0" * 16)
    check("write_container has no art argument left to offer (D21)", False,
          "it accepted one")
except TypeError as e:
    check("write_container has no art argument left to offer (D21)",
          "art" in str(e), e)
expect_refuse("a geometry blob shorter than its counts is refused",
              lambda: A._geom_size(8, (0, 0, 0, 40)))
expect_refuse("a ragged geometry blob is refused (the format is words)",
              lambda: A._geom_size(6, (0, 0, 0, 0)))
expect_refuse("an oversized geometry blob is refused",
              lambda: A._geom_size(A.GEOM_BYTES_MAX + 4, (0, 0, 0, 0)))
# The slot boundary is the SUM (orb_app_install.c:62), so the writer refuses what the
# dial would refuse pre-erase — the §19.3 witness. `wants` pins the sum rule, not the
# blob-only cap next door: both say "refused", and only one of them moved.
expect_refuse("a full-ceiling blob is refused when a header precedes it (the SUM rule)",
              lambda: A.write_container(os.path.join(D, "sum1.orbapp"), "sum1", "Sum1",
                                        geometry=b"\xa5" * A.GEOM_BYTES_MAX),
              wants="a slot holds")
# Boundary accepted to the byte: the device takes `== cap` (the last word before the
# header sector), so the writer must take it too — at THIS container's geom_off, not at
# the fixed 0x3FE00 m5_powercut.sh's comment assumes.
_h_b = A.HDR_BYTES
_set_b = A.SETTINGS_WORDS * 4                     # one settings row, 64 B
check("the exact slot boundary is accepted, blob == cap",
      A.write_container(os.path.join(D, "sum2.orbapp"), "sum2", "Sum2",
                        geometry=b"\xa5" * (A.GEOM_BYTES_MAX - _h_b))
      == A.GEOM_BYTES_MAX, "file is not the cap")
expect_refuse("the boundary MOVES with the header: one setting costs the blob %d B"
              % _set_b,
              lambda: A.write_container(
                  os.path.join(D, "sum3.orbapp"), "sum3", "Sum3",
                  settings=[dict(id="mode", type="enum", min=0, max=3, default=1)],
                  geometry=b"\xa5" * (A.GEOM_BYTES_MAX - _h_b)),
              wants="a slot holds")
check("and a blob measured against the MOVED boundary is accepted at == cap",
      A.write_container(os.path.join(D, "sum4.orbapp"), "sum4", "Sum4",
                        settings=[dict(id="mode", type="enum", min=0, max=3, default=1)],
                        geometry=b"\xa5" * (A.GEOM_BYTES_MAX - _h_b - _set_b))
      == A.GEOM_BYTES_MAX, "file is not the cap")
# ---- P7 (D20/D27): the code region and its memory bound ----------------------
_MOD = b"\x00asm\x01\x00\x00\x00" + b"\x11" * 100   # a tiny well-formed module header
# Positive control: a native app (no code, no bound) still writes at the pre-P7 size,
# so the four new header words cost a no-code container nothing but the header.
check("a native container still round-trips with a zero code triple",
      (lambda: (A.write_container(os.path.join(D, "nat.orbapp"), "nat", "Nat"),
                A.read_container(os.path.join(D, "nat.orbapp"))[0]["code_size"] == 0))())
# Positive control: a module-bearing container round-trips its bytes and its bound.
def _code_roundtrip():
    p = os.path.join(D, "wasmapp.orbapp")
    A.write_container(p, "wasmapp", "WasmApp", code=_MOD, wasm_mem_max=65536)
    h, _st, _geom, _art = A.read_container(p)
    return h["code_size"] == len(_MOD) and h["wasm_mem_max"] == 65536 and h["code"] == _MOD
check("a code region round-trips its bytes, size and D27 bound", _code_roundtrip())
# D27's ceiling is the refusal the P7 pass line names ("install refuses > 128 KB").
expect_refuse("a wasm memory bound past the D27 ceiling is refused at pack",
              lambda: A.write_container(os.path.join(D, "big.orbapp"), "big", "Big",
                                        code=_MOD, wasm_mem_max=A.WASM_MEM_MAX + 1),
              wants="past the")
# A module with no bound is no bounded module: the gate has nothing to hold.
expect_refuse("a code region with a zero memory bound is refused",
              lambda: A.write_container(os.path.join(D, "zm.orbapp"), "zm", "ZM",
                                        code=_MOD, wasm_mem_max=0),
              wants="zero memory")
# A bound with no module bills a runtime the reader never populates.
expect_refuse("a memory bound with no code behind it is refused",
              lambda: A.write_container(os.path.join(D, "mc.orbapp"), "mc", "MC",
                                        code=b"", wasm_mem_max=65536),
              wants="no code")
# The module is priced in flash: past a slot's payload it cannot be copied, so pack
# refuses (D27's "a module that cannot fit never writes a byte of flash").
expect_refuse("a code region past the slot budget is refused",
              lambda: A.write_container(
                  os.path.join(D, "ov.orbapp"), "ov", "OV",
                  code=b"\x00asm\x01\x00\x00\x00" + b"\x00" * A.CODE_BYTES_MAX,
                  wasm_mem_max=65536),
              wants="past the")
# A code region that would push the whole copied prefix past the slot is refused by
# the same sum arithmetic the geometry uses — code rides the slot's payload region.
expect_refuse("code after a full-ceiling geometry is refused (the SUM rule)",
              lambda: A.write_container(
                  os.path.join(D, "ov2.orbapp"), "ov2", "OV2",
                  geometry=b"\xa5" * (A.GEOM_BYTES_MAX - _h_b),
                  code=_MOD, wasm_mem_max=65536),
              wants="a slot holds")
# The code region is opaque bytes: an unaligned length is legal (a wasm module is a
# byte stream, not words) — the one respect the code triple is looser than geom.
check("a code region of any byte length is legal (wasm is not worded)",
      A._code_size(7) == 7 and A._code_size(100) == 100)
expect_refuse("a title over the field is refused, never truncated",
              lambda: A.pack_header(A.app_id_of("pin"), "pin", "T" * A.TITLE_MAX,
                                    0, 0, 0, (0, 0, 0, 0), b"", 0))
expect_refuse("an inverted ABI range is refused (no board could install it)",
              lambda: A.pack_header(A.app_id_of("pin"), "pin", "P", 0, 0, 0,
                                    (0, 0, 0, 0), b"", 0, abi=(3, 2)))
expect_refuse("an ABI range excluding this writer is refused (C4)",
              lambda: A.pack_header(A.app_id_of("pin"), "pin", "P", 0, 0, 0,
                                    (0, 0, 0, 0), b"", 0,
                                    abi=(A.GEOM_ABI + 1, A.GEOM_ABI + 9)))
expect_refuse("a filled sig word is refused while signing is unimplemented (C3)",
              lambda: A.pack_header(A.app_id_of("pin"), "pin", "P", 0, 0, 0,
                                    (0, 0, 0, 0), b"", 0, sig=1))


def _fmt2():
    """A container claiming a FORMAT newer than this build's, CRC recomputed so
    only the format check can stop it: a reader that ignored FORMAT would parse
    this as the current layout. The number is FORMAT+1 (P7 bumped the build's
    FORMAT to 2, so the future vector moved to 3 with it — the check must always
    stamp a number THIS build does not speak, not a fixed one)."""
    w = A.pack_header(A.app_id_of("pin"), "pin", "P", 0, 0, 0, (0, 0, 0, 0), b"", 0)
    w[A.W_FORMAT] = A.FORMAT + 1
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
_h4 = dict(h, art_req=4)
check("an art_req declaration alone passes the read gate (D21 inverts the pair)",
      A.check_header(_h4) == _h4)
expect_refuse("a payload claim of any size is refused on read (D21)",
              lambda: A.check_header(dict(h, art_size=A.ART_BYTES_MAX + 1)),
              wants="ships art")
expect_refuse("a small payload claim is refused too — no size fits (D21)",
              lambda: A.check_header(dict(h, art_size=4096)), wants="ships art")
expect_refuse("a stale art crc is refused as if it shipped pixels (D21)",
              lambda: A.check_header(dict(h, art_crc=0xDEAD)), wants="ships art")
expect_refuse("an art need past the bound is refused on read as a miscount",
              lambda: A.check_header(dict(h, art_req=A.ART_BYTES_MAX + 1)),
              wants="miscount")
expect_refuse("a beat under the floor is refused on read too",
              lambda: A.check_header(dict(h, period_ms=100)))
expect_refuse("a blob too short for its counts is refused on read too",
              lambda: A.check_header(dict(h, geom_size=4)))
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
                   h["n_settings"])
for want in ("weather", "Weather", "power", "sd_read", "themes", "4000",
             str(len(SETTS)), "PERMISSIONS"):
    check("the install confirm names %r" % want, want in txt, txt[:100])
check("the D21 disclosure offers the owner no art line to consent to (D21)",
      "art" not in txt.lower(), txt)
check("the confirm lists exactly the declared owner caps, in order",
      "power, sd_read, themes" in txt, txt)
check("the confirm never offers a free cap as revocable",
      "time" not in txt.split("may use:")[1].split("\n")[0], txt)
check("nothing declared says so in the ladder's words",
      "nothing declared" in A.disclosure("x", "X", 0, 0, 0))
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
    ("the D21 art-carry gate is gone from check_header",
     'if h["art_size"] != 0 or h["art_crc"] != 0:', "if False:"),
    ("art_req may declare past any bound", "if art_req > ART_BYTES_MAX:", "if 0:"),
    ("pack_header ships art bytes again",
     "hdr[W_ART_SIZE]   = 0", "hdr[W_ART_SIZE]   = 8"),
    ("the counts no longer price the blob", "if geom_size < need:", "if 0:"),
    ("the slot-sum gate is gone (writer emits what the dial refuses)",
     "if hdr[W_GEOM_OFF] + hdr[W_GEOM_SIZE] > GEOM_BYTES_MAX:", "if 0:"),
    ("the slug charset check is gone",
     "if not isinstance(slug, str) or not SLUG_RE.match(slug):", "if 0:"),
    ("dupe setting ids allowed", "if sid in seen:", "if 0:"),
    ("the setting default-range check is gone", "elif not (mn <= df <= mx):", "elif 0:"),
    ("the toggle's two words are not enforced", "if df not in (0, 1):", "if 0:"),
    ("the setting id width check is gone",
     "if not isinstance(sid, str) or not SET_RE.match(sid):", "if 0:"),
    ("the CRC word moves below the manifest it must cover",
     "W_HDR_CRC    = W_WASM_MEM    + 1", "W_HDR_CRC    = W_SIG           + 1"),
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
