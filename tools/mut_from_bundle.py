#!/usr/bin/env python3
"""mut_rebake.py -- mutate the FORWARD mapping (FORMAT 2 -> FORMAT 3) and require
the re-bake's own verify() to report the break. See tools/rebake_bundle.py.
"""
import hashlib
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

SRC = os.path.join(HERE, "bake_clock.py")
SUITE = os.path.join(HERE, "host_from_bundle_test.py")
text0 = open(SRC).read()

# Each mutant breaks the reverse path in one way a bundle cannot show on its face.
# The pair must stop being byte-identical, or a refusal must stop naming its defect.
MUTANTS = [
    ("shadow crop pivot left at the element pivot (the light offset lost)",
     'shadow_recs.append(rec_of(sh, (hand_recs[i]["centerx"] - sh["x"],\n'
     '                                       hand_recs[i]["centery"] - sh["y"])))',
     'shadow_recs.append(rec_of(sh))'),
    ("centre rebuilt from the crop pivot instead of the element pivot",
     'centerx=e["x"] + e["pivot_x"], centery=e["y"] + e["pivot_y"],',
     'centerx=e["x"] + e["pivot_x"] + 1, centery=e["y"] + e["pivot_y"],'),
    ("delay regenerated at the theme's bg_fps instead of the OS tick (R6)",
     'ms = max(1, int(round(1000.0 / max(1, sc["tick_rate"]))))',
     'ms = max(1, int(round(1000.0 / max(1, sc["bg_fps"]))))'),
    ("the plane-sum check dropped, so an unaccounted bank rebuilds anyway",
     'if at != len(art):',
     'if False:'),
    ("the extent cross-check dropped, so a lying catalogue rebuilds",
     'if (s["w"], s["h"]) != (e["w"], e["h"]):',
     'if False:'),
    ("the tourbillon refusal dropped",
     'if cls == ts.CLASS_HAND and slot >= 5:',
     'if False:'),
    ("the orphaned-shadow refusal dropped",
     'if hd is None:',
     'if False:'),
    ("the plate-extent refusal dropped",
     'if (plate["x"], plate["y"], plate["w"], plate["h"]) != (0, 0, scr_w, scr_h):',
     'if False:'),
    ("the contiguous-catalogue refusal dropped",
     'if s["off"] != at:',
     'if False:'),
    ("a hidden row printed as shown",
     'show=0 if (e["flags"] & ts.EF_HIDDEN) else 1,',
     'show=1,'),
    ("the missing-clock-section refusal dropped",
     'if sec is None:',
     'if False:'),
]

import shutil

if len(sys.argv) > 1:                       # worker mode: mutate in place, run, restore
    i = int(sys.argv[1])
    _note, old, new = MUTANTS[i]
    if old not in text0:
        print("MUTANT-ABSENT " + _note)
        sys.exit(3)
    keep = SRC + ".mutkeep"
    shutil.copy2(SRC, keep)
    try:
        open(SRC, "w").write(text0.replace(old, new, 1))
        p = subprocess.run([sys.executable, SUITE], capture_output=True, text=True,
                           cwd=HERE)
        code = p.returncode
    finally:
        shutil.move(keep, SRC)
    # A mutant is caught when the suite stops being green: the byte-exact pair moves,
    # or a refusal stops naming its own defect.
    sys.exit(0 if code != 0 else 1)

caught, missed = 0, []
for i, (note, _o, _n) in enumerate(MUTANTS):
    p = subprocess.run([sys.executable, __file__, str(i)], capture_output=True,
                       text=True, cwd=HERE)
    if p.returncode == 3:
        missed.append(note + " (mutation text absent -- the code moved)")
    # The worker exits 0 exactly when the suite stopped being green, which is what
    # "caught" means. Exit 1 there is the suite still accepting the mutant.
    elif p.returncode == 0:
        caught += 1
    else:
        missed.append(note + " (suite still green)")
print("mut_from_bundle: %d/%d caught" % (caught, len(MUTANTS)))
for m in missed:
    print("  MISSED " + m)
sys.exit(1 if missed else 0)