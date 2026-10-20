"""Run the re-bake mapping's mutants: break one rule, require the tool to fail.

Same discipline as mut_bundle.py / mut_element.py, kept as a file because the
shell mangles heredocs. The mapping's teeth are its round trip: every mutant
below breaks ONE clause of §25.2's rule or one of §25.3's refusals, and the
run must say so — the CLI path for a mapping break (verify exits nonzero), a
forged-geometry driver for a guard the av-kit source cannot trip. Delete a
clause and the harness must report it, or the clause was never tested.

Run from the repo root:
  cp tools/rebake_bundle.py /tmp/rb.keep && python3 tools/mut_rebake.py
"""
import shutil
import subprocess
import sys

KEEP = "/tmp/rb.keep"
TOOL = "tools/rebake_bundle.py"
IN = "/tmp/m5_avkit.orbtheme"
OUT = "/tmp/b3/mut.orbtheme"
src = open(KEEP).read()

# The guards §25.3 states but av-kit's own source cannot trip — the driver
# below forges each shape onto a copy of the real geometry and requires the
# refusal sentence. Each mutant drops its guard, so the call must NOT raise.
REFUSE = [
    ("drop tourbillon refusal", 'if any(x["show"] and (x["w"] or x["h"]) for x in g["tourbillon"]):', 'if False:'),
    ("drop gif-rect refusal", 'if nz:', 'if False:'),
    ("drop comp-table refusal", 'if g["comps"] or g["aux"] or g["bank"]:', 'if False:'),
    ("drop plate_off refusal", 'if sc["plate_off"] != 0:', 'if False:'),
]

# The mapping mutants: each breaks one clause of the ruled identity, and the
# tool's own verify (or a walk refusal it unmasks) must fail the run.
MAP = [
    ("hand x,y swapped",
     'r["x"], r["y"] = s["offx"], s["offy"]\n            r["pivot_x"], r["pivot_y"] = s["centerx"] - s["offx"], s["centery"] - s["offy"]\n        elif kind == "shadow":',
     'r["x"], r["y"] = s["offy"], s["offx"]\n            r["pivot_x"], r["pivot_y"] = s["centerx"] - s["offx"], s["centery"] - s["offy"]\n        elif kind == "shadow":'),
    ("shadow pivot = crop pivot, not center-off",
     's = g["shadows"][idx]\n            r["x"], r["y"] = s["offx"], s["offy"]\n            r["pivot_x"], r["pivot_y"] = s["centerx"] - s["offx"], s["centery"] - s["offy"]',
     's = g["shadows"][idx]\n            r["x"], r["y"] = s["offx"], s["offy"]\n            r["pivot_x"], r["pivot_y"] = s["pivotx"], s["pivoty"]'),
    ("hand loses its rung above its shadow",
     'e["class"], e["order"] = ts.CLASS_HAND, 2 * rank.get(idx, idx) + 1',
     'e["class"], e["order"] = ts.CLASS_HAND, 2 * rank.get(idx, idx)'),
    ("overlay not last",
     'e["order"] = 63', 'e["order"] = 0'),
    ("hands claim the sprite class",
     'e["class"], e["order"] = ts.CLASS_HAND, 2 * rank.get(idx, idx) + 1',
     'e["class"], e["order"] = ts.CLASS_SPRITE, 2 * rank.get(idx, idx) + 1'),
    ("slot stops being the array address",
     'e["slot"] = idx', 'e["slot"] = 0'),
    ("frame0 stops indexing the rect list",
     'e["frame0"] = len(recs)', 'e["frame0"] = 0'),
    ("hand plane forgets its alpha",
     'planes.append(("hand", i, s["color_off"], s["w"] * s["h"] * 3, s["w"], s["h"]))',
     'planes.append(("hand", i, s["color_off"], s["w"] * s["h"] * 2, s["w"], s["h"]))'),
    ("plate plane claims an alpha it has not",
     'planes = [("plate", 0, sc["plate_off"], scr_w * scr_h * 2, scr_w, scr_h)]',
     'planes = [("plate", 0, sc["plate_off"], scr_w * scr_h * 3, scr_w, scr_h)]'),
    ("planes not sorted by byte offset",
     'planes.sort(key=lambda p: p[2])', 'pass'),
    ("scalars not carried verbatim",
     'scal = {n: v for n, v in g["scalars"].items()}', 'scal = {}'),
    ("screen pair left in the header",
     '             "scr_w": scr_w, "scr_h": scr_h,\n', ''),
    ("name identity not preserved",
     'nm = name or h["name"]', 'nm = name or "not-the-source-name"'),
]

DRIVER = """
import sys
sys.path.insert(0, "tools")
import orbtheme as ot
import rebake_bundle as R
h, geom, art = ot.read_container(sys.argv[1])
g = ot.unpack_geom(geom)

def expect(frag, mut):
    try:
        R.planes_of(mut, 466, 466)
    except SystemExit as e:
        sys.exit(0 if frag in str(e) else 3)
    sys.exit(1)

f = dict(g)
if sys.argv[2] == "tourb":
    f["tourbillon"] = [dict(f["tourbillon"][0], show=1, w=8, h=8)] + list(f["tourbillon"][1:])
    expect("tourbillon", f)
if sys.argv[2] == "gif":
    f["rects"] = [0, 0, 64, 64, 4096] + list(f["rects"][5:])
    expect("gif bank", f)
if sys.argv[2] == "comp":
    f["comps"] = [dict(f["comps"][0] if f["comps"] else {}, kind=1, frame=0, x=0, y=0)]
    expect("complication", f)
if sys.argv[2] == "off":
    f["scalars"] = dict(f["scalars"], plate_off=4096)
    expect("plate_off", f)
sys.exit(2)
"""
with open("/tmp/rebake_refuse.py", "w") as f:
    f.write(DRIVER)

survived = miss = 0
for name, old, new in REFUSE:
    key = {"drop tourbillon refusal": "tourb", "drop gif-rect refusal": "gif",
           "drop comp-table refusal": "comp", "drop plate_off refusal": "off"}[name]
    base = subprocess.run([sys.executable, "/tmp/rebake_refuse.py", IN, key],
                          capture_output=True, text=True)
    if base.returncode != 0:
        print("  BASELINE BROKEN %s (driver exit %d)" % (name, base.returncode))
        miss += 1
        continue
    if old not in src:
        print("  ANCHOR LOST %s" % name)
        miss += 1
        continue
    open(TOOL, "w").write(src.replace(old, new, 1))
    r = subprocess.run([sys.executable, "/tmp/rebake_refuse.py", IN, key],
                       capture_output=True, text=True)
    if r.returncode == 0:
        print("  SURVIVED %-34s guard gone, sentence still printed" % name)
        survived += 1
    elif r.returncode == 1:
        print("  caught   %-34s no refusal: planes_of accepted the shape" % name)
    else:
        print("  caught   %-34s refused for a different rule (exit %d)" % (name, r.returncode))
for name, old, new in MAP:
    if old not in src:
        print("  ANCHOR LOST %s" % name)
        miss += 1
        continue
    open(TOOL, "w").write(src.replace(old, new, 1))
    r = subprocess.run([sys.executable, TOOL, IN, OUT],
                       capture_output=True, text=True)
    tail = (r.stdout.strip().splitlines() or [r.stderr.strip().splitlines() or [""]][-1])[-1]
    if r.returncode == 0:
        print("  SURVIVED %-44s %s" % (name, tail))
        survived += 1
    else:
        print("  caught   %-44s" % name)
shutil.copy(KEEP, TOOL)
print("== rebake mutants: %d/%d caught, %d anchors missed"
      % (len(REFUSE) + len(MAP) - survived - miss, len(REFUSE) + len(MAP), miss))
sys.exit(1 if (survived or miss) else 0)
