#!/usr/bin/env python3
"""The .orbapp header's three readers, pinned to one table and one set of bytes.

Why this file exists and what it alone proves:

1. THE C MIRROR'S POSITIONS. tools/host_orbapp_reader_test.py compiles the shipping
   reader and runs cases through it, which proves the reader BEHAVES. Nothing proved the
   reader's word map AGREED: a renumber that moved every word together with its accessor
   would have run green there and shipped a reader installing the wrong field. This file
   evaluates main/app/orb_app_manifest.h's enum, derivations and all, against the writer's
   own W_* table as the fixture's sidecar recorded it. A moved word in either language
   goes red here.

2. THE HUB'S MIRROR. tools/orb-manager/internal/orbnet/appcontainer.go is the fourth
   reader, the only one that is neither C nor Python, and the Go test that checks it
   cannot run in the Python-only lane. So its table is read here as text too, and the
   three languages meet in one place, which is the only way a disagreement between two of
   them becomes one failure with a name.

3. THE FIXTURE IS REPRODUCIBLE. tests/fixtures/sentinel.orbapp and plain.orbapp are what
   every non-Python reader is pinned against, and a fixture whose committed bytes drift
   from what the tree generates turns every reader test into a coincidence check. The
   editor region is a zip archive whose CRC lands in the header's checksum, so a stray
   timestamp is a checksum change, and a checksum change is a dial refusing a file this
   same tree wrote minutes earlier.

Run: python3 tools/host_app_fixture_test.py
Re-bake: python3 tools/make_app_fixture.py
"""
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import orbapp  # noqa: E402

failures = []


def check(cond, msg):
    if not cond:
        failures.append(msg)


SIDE_PATH = os.path.join(ROOT, "tests", "fixtures", "app-pair.json")
if not os.path.exists(SIDE_PATH):
    print("  FAIL no sidecar at %s — run: python3 tools/make_app_fixture.py" % SIDE_PATH)
    sys.exit(1)
side = json.load(open(SIDE_PATH))
WORDS = {k: int(v) for k, v in side["words"].items()}

# ---------------------------------------------------------------------------
# 0. The sidecar still says what the writer says. Everything below compares two
#    readers, so it is worthless unless the reference is current. That is why the
#    sidecar is generated rather than curated, and why that is checked first.
# ---------------------------------------------------------------------------
for k, v in sorted(WORDS.items()):
    attr = "W_" + k.upper()
    check(hasattr(orbapp, attr), "sidecar names word %r, orbapp.py has no %s" % (k, attr))
    if hasattr(orbapp, attr):
        check(getattr(orbapp, attr) == v,
              "word %s: sidecar says %d, orbapp.py says %d — re-run "
              "tools/make_app_fixture.py" % (k, v, getattr(orbapp, attr)))
check(side["hdr_words"] == orbapp.HDR_WORDS,
      "header is %d words in the sidecar, %d in the writer"
      % (side["hdr_words"], orbapp.HDR_WORDS))
check(side["writer_abi"] == orbapp.GEOM_ABI,
      "sidecar was baked by ABI %d and the writer stamps %d"
      % (side["writer_abi"], orbapp.GEOM_ABI))

# ---------------------------------------------------------------------------
# 1. The C enum, evaluated. The header derives its positions from field widths exactly
#    as the writer does, so the substitution has to happen here or the lint compares a
#    string against a number and calls that agreement.
# ---------------------------------------------------------------------------
hdr = open(os.path.join(ROOT, "main", "app", "orb_app_manifest.h")).read()

defines = {}
for m in re.finditer(r"#define\s+(ORBAPP_[A-Z0-9_]+)\s+(\d[\dxXoObB\dA-Fa-f]*)(u)?\b", hdr):
    # int(x, 0) so 0x00040000, 24 and 0b1010 all read as the compiler reads them. A
    # hex literal this had to guess at would silently compare a firmware cap of zero.
    defines[m.group(1)] = int(m.group(2).rstrip("u"), 0)

enum_m = re.search(r"The header word map.*?enum \{(.*?)\n\};", hdr, re.S)
if not enum_m:
    # The comment is matched before the brace on purpose: this header holds several enums,
    # and a lint that quietly took the wrong one would report a clean tree while comparing
    # a settings descriptor's field offsets against a header word map.
    failures.append("the header word map's enum is not where this lint expects it in "
                    "orb_app_manifest.h — the mirror is unverified, which is the failure "
                    "this lint exists to catch")
else:
    body = re.sub(r"/\*.*?\*/", "", enum_m.group(1), flags=re.S)
    cvals = {}
    for part in body.replace("\n", " ").split(","):
        if "=" not in part:
            continue
        name, expr = (s.strip() for s in part.split("=", 1))
        if not name.startswith("ORBAPP_W_"):
            continue
        py = expr
        for d, val in defines.items():
            py = py.replace(d, str(val))
        for cname, cval in list(cvals.items()):
            py = py.replace(cname, str(cval))
        py = re.sub(r"0x([0-9A-Fa-f]+)u?", r"0x\1", py).replace("u", "")
        try:
            cvals[name] = int(eval(py, {"__builtins__": {}}, {}))
        except Exception as e:
            failures.append("cannot evaluate %s = %r: %s" % (name, expr, e))
    check(len(cvals) >= len(WORDS),
          "C enum yields %d word positions, sidecar names %d — the lint read too little"
          % (len(cvals), len(WORDS)))
    for k, v in sorted(WORDS.items()):
        cn = "ORBAPP_W_" + k.upper()
        if cn not in cvals:
            failures.append("C header declares no %s; the writer names word %d" % (cn, v))
            continue
        check(cvals[cn] == v, "word %s: C header says %d, orbapp.py says %d"
              % (k, cvals[cn], v))
    # The editor triple sits INSIDE the checksum's span. That is D37's placement rule, and
    # placement is exactly what a tidy reorder breaks while every behavioural test keeps
    # passing, so it is asserted here rather than merely commented.
    if "ORBAPP_W_EDITOR_CRC" in cvals and "ORBAPP_W_HDR_CRC" in cvals:
        check(cvals["ORBAPP_W_EDITOR_CRC"] < cvals["ORBAPP_W_HDR_CRC"],
              "C header puts the editor triple at or after hdr_crc, so a card could "
              "rewrite an editor's placement without disturbing any checksum")

# ---------------------------------------------------------------------------
# 2. The hub's mirror, read as text. The Go half of the agreement is checked by a Go
#    test that needs a toolchain this lane does not assume, so the table is verified
#    here as well. The mapping is spelled pair by pair rather than derived from case
#    folding, because Go's appWNNBank and Python's n_bank do not fold into each other,
#    and a rename on either side has to fail as a missing mapping rather than as an
#    accidental comparison against some other word.
# ---------------------------------------------------------------------------
GO_PAIRS = (
    ("appWMagic", "magic"), ("appWFormat", "format"), ("appWGeomABI", "geom_abi"),
    ("appWHdrSize", "hdr_size"), ("appWSerial", "serial"), ("appWBakedMs", "baked_ms"),
    ("appWABIMin", "abi_min"), ("appWABIMax", "abi_max"), ("appWSig", "sig"),
    ("appWAppID", "app_id"), ("appWSlug", "slug"), ("appWTitle", "title"),
    ("appWBehaviour", "behaviour"), ("appWArtReq", "art_req"),
    ("appWNLayer", "n_layer"), ("appWNComp", "n_comp"), ("appWNAux", "n_aux"),
    ("appWNNBank", "n_bank"), ("appWPeriodMS", "period_ms"), ("appWCaps", "caps"),
    ("appWNSettings", "n_settings"), ("appWSetOff", "set_off"),
    ("appWGeomOff", "geom_off"), ("appWGeomSize", "geom_size"),
    ("appWGeomCRC", "geom_crc"), ("appWArtOff", "art_off"),
    ("appWArtSize", "art_size"), ("appWArtCRC", "art_crc"),
    ("appWCodeOff", "code_off"), ("appWCodeSize", "code_size"),
    ("appWCodeCRC", "code_crc"), ("appWWasmMem", "wasm_mem"),
    ("appWEditOff", "editor_off"), ("appWEditSize", "editor_size"),
    ("appWEditCRC", "editor_crc"), ("appWHdrCRC", "hdr_crc"),
    ("appWAssetOff", "asset_off"), ("appWNAssets", "n_assets"),
)

go_path = os.path.join(ROOT, "tools", "orb-manager", "internal", "orbnet",
                       "appcontainer.go")
if not os.path.exists(go_path):
    failures.append("no %s — the hub's .orbapp reader moved or vanished" % go_path)
else:
    go_src = open(go_path).read()
    gconsts = {}
    for m in re.finditer(r"^\t(appW[A-Za-z]+|appWHdrWords|appSlugBytes|appTitleBytes|"
                         r"appHdrBytes|appGeomCap|appEditorCap)\s*=\s*(.+?)$",
                        go_src, re.M):
        # Strip the trailing comment now, not in the evaluator: half these lines carry
        # the reason for their value, and an evaluator that trips over prose reports
        # "cannot follow the layout" when the layout is perfectly readable.
        gconsts[m.group(1)] = m.group(2).split("//")[0].split("/*")[0].strip()
    check(len(gconsts) >= 40,
          "only %d layout constants parsed out of appcontainer.go — the reader's shape "
          "is unverified, which is the failure this section exists to catch" % len(gconsts))

    # Resolve by fixed point rather than recursion: the expressions reference each other
    # (appWTitle = appWSlug + (appSlugBytes + 3) / 4), so a single pass can only learn a
    # word once its dependents already have values, and a loop that converges says how
    # many passes it took and therefore whether something is left unresolved.
    gvals, pending = {}, dict((k, v) for k, v in gconsts.items())
    while True:
        progressed = False
        for name in sorted(pending):
            expr = pending[name]
            for other in sorted(gvals, key=len, reverse=True):
                expr = re.sub(r"\b%s\b" % re.escape(other), str(gvals[other]), expr)
            expr = re.sub(r"\b(\d+)[uU]\b", r"\1", expr).replace(" ", "")
            try:
                gvals[name] = int(eval(expr, {"__builtins__": {}}, {}))
                progressed = True
            except Exception:
                continue
        if not progressed:
            break
        for name in list(gvals):
            pending.pop(name, None)
        if not pending:
            break
    for name, expr in sorted(pending.items()):
        failures.append("cannot evaluate Go %s = %r — the reader's layout is now written "
                        "in a form this lint cannot follow" % (name, expr))

    def go_eval(name):
        return gvals.get(name)

    for gname, key in GO_PAIRS:
        gv = go_eval(gname)
        if gv is None:
            failures.append("hub reader declares no %s; the writer names word %d"
                            % (gname, WORDS.get(key, -1)))
            continue
        check(gv == WORDS[key], "word %s: Go %s says %d, orbapp.py says %d"
              % (key, gname, gv, WORDS[key]))
    hdr_words = go_eval("appWHdrWords")
    check(hdr_words == side["hdr_words"],
          "hub reader thinks the header is %s words, the writer says %d"
          % (hdr_words, side["hdr_words"]))
    check(go_eval("appHdrBytes") == side["hdr_bytes"],
          "hub reader thinks the header is %s B, the writer says %d B"
          % (go_eval("appHdrBytes"), side["hdr_bytes"]))
    for gname, dname in (("appSlugBytes", "ORBAPP_SLUG_MAX"),
                        ("appTitleBytes", "ORBAPP_TITLE_MAX")):
        check(go_eval(gname) == defines.get(dname),
              "%s is %s in the hub and %s in the firmware — the header's width is "
              "derived from it on both sides"
              % (dname, go_eval(gname), defines.get(dname)))
    for gname, dname in (("appGeomCap", "ORBAPP_GEOM_BYTES_MAX"),
                        ("appEditorCap", "ORBAPP_EDITOR_MAX")):
        check(go_eval(gname) == defines.get(dname),
              "%s is %s in the hub and %s in the firmware — the hub would forward a "
              "container the door refuses, or refuse one it installs"
              % (dname, go_eval(gname), defines.get(dname)))
    ed = go_eval("appWEditCRC")
    hc = go_eval("appWHdrCRC")
    check(ed is not None and hc is not None and ed < hc,
          "hub reader puts the editor triple at or after hdr_crc, outside the checksum "
          "D37 put it inside")

# ---------------------------------------------------------------------------
# 3. The fixture reproduces. The two containers above are the evidence every reader
#    test points at, so a committed file that no longer matches what the tree generates
#    turns each of those tests into a check of a coincidence. --verify re-bakes into a
#    scratch directory and demands byte equality; it never rewrites, so this cannot make
#    a stale fixture look current by overwriting the thing it is supposed to catch.
# ---------------------------------------------------------------------------
gen = os.path.join(HERE, "make_app_fixture.py")
r = subprocess.run([sys.executable, gen, "--verify"], cwd=ROOT,
                   capture_output=True, text=True)
check(r.returncode == 0,
      "fixture is not reproducible from this tree: %s%s"
      % (r.stdout.strip(), r.stderr.strip()[-400:]))

# ---------------------------------------------------------------------------
# 4. The identity rule both ends apply. app_id is the cross-check that catches a slug
#    edited after a container was named, and three readers derive it independently, so
#    the pair is pinned here off the fixture's own bytes and off the container the hub
#    ships today.
# ---------------------------------------------------------------------------
for slug, blob in sorted(side["apps"].items()):
    check(blob["app_id"] == orbapp.app_id_of(slug),
          "%s.orbapp stamps app_id 0x%08X and slug %r hashes to 0x%08X"
          % (slug, blob["app_id"], slug, orbapp.app_id_of(slug)))
    check(blob["slug"] == slug,
          "sidecar keys %r off a header that declares slug %r — the filename belongs "
          "to the slug" % (slug, blob["slug"]))
seed = os.path.join(ROOT, "tools", "orb-manager", "seed", "clock.orbapp")
if os.path.exists(seed):
    h, _s, _g, _a = orbapp.read_container(seed)
    check(h["app_id"] == orbapp.app_id_of(h["slug"]),
          "the shipped seed container's app_id 0x%08X does not match its slug %r's "
          "hash 0x%08X" % (h["app_id"], h["slug"], orbapp.app_id_of(h["slug"])))
    # The literal the hub's Go test pins. Two languages, one number, stated twice on
    # purpose: the Go side cannot import this file, so the pair fails together or not at
    # all, which is the point of pinning a shipped artifact rather than a generated one.
    check(h["app_id"] == 0xD8198EFA,
          "seed clock.orbapp stamps 0x%08X, and appcontainer_test.go pins 0xD8198EFA"
          % h["app_id"])
else:
    failures.append("no seed container at %s — the app_id pin has no subject" % seed)

print("== APP HEADER MIRROR SUITE: %d failure(s)" % len(failures))
for _f in failures:
    print("  FAIL " + _f)
sys.exit(1 if failures else 0)
