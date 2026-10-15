"""Run the element-schema mutants: break one rule, require the suite to fail.

Kept as a file because the shell here mangles heredocs. The suite is the product;
this is the proof the suite has teeth. Delete a rule's condition and the suite must
say so, or the rule was never tested.
"""
import shutil
import subprocess
import sys

KEEP = "/tmp/ts.keep"
src = open(KEEP).read()

MUT = [
    ("drop class range", "if not 0 <= cls < len(CLASS_NAMES):",
     "if not -1 <= cls < len(CLASS_NAMES) + 1:"),
    ("drop flag check", "if flags & ~EF_ALL:", "if False:"),
    ('drop order bound', 'if not 0 <= int(rec["order"]) < ELEM_ORDER_N:',
     'if not 0 <= int(rec["order"]) < 256:'),
    ('drop zero-frame rule', 'if int(rec["n_frames"]) < 1:', "if False:"),
    ('drop frame bound', 'if int(rec["frame0"]) + int(rec["n_frames"]) > ELEM_FRAMES_MAX:',
     "if False:"),
    ('drop degrees check', 'if int(rec["degrees"]) >= 36000:', "if False:"),
    ("drop R6 adv rule", "if flags & EF_OS_ADV and cls in CLASS_APP_ADV:", "if False:"),
    ("drop no-anim rule", 'if cls in CLASS_NO_ANIM and int(rec["n_frames"]) != 1:',
     "if False:"),
    ("drop pivot rule", 'if cls == CLASS_HAND and int(rec["w"]) and int(rec["h"]):',
     "if False:"),
    ("drop text radius rule", 'if cls in (CLASS_TEXT, CLASS_STATUS) and int(rec["radius"]):',
     "if False:"),
    ("drop scale floor", 'if int(rec["scale16"]) and int(rec["scale16"]) < 16:',
     "if False:"),
    ("drop dup slot rule", "if key in seen_slot:", "if False:"),
    ("drop dup identity rule", "if n in seen_n:", "if False:"),
    ("drop row cap", "if len(recs) > ELEM_MAX:", "if False:"),
]

miss = survived = 0
for name, old, new in MUT:
    if old not in src:
        print("  MISS     %s (anchor not found — the rule moved)" % name)
        miss += 1
        continue
    open("tools/theme_schema.py", "w").write(src.replace(old, new, 1))
    r = subprocess.run([sys.executable, "tools/host_theme_elem_test.py"],
                       capture_output=True, text=True)
    tail = (r.stdout.strip().splitlines() or [""])[-1]
    if r.returncode == 0:
        print("  SURVIVED %-22s %s" % (name, tail))
        survived += 1
    else:
        print("  caught   %-22s %s" % (name, tail))
shutil.copy(KEEP, "tools/theme_schema.py")
print("== element-schema mutants: %d/%d caught, %d anchors missed"
      % (len(MUT) - survived - miss, len(MUT), miss))
sys.exit(1 if (survived or miss) else 0)
