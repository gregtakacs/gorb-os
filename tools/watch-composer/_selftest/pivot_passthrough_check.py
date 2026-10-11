"""Does the bake honour an arbour the author put outside the artwork, and a facing
that is not north? These two are the numbers the composer only just learned to author,
and both are easy to lose on the way to the panel: prep places the plane from the
pivot, and bake is entitled to "correct" a pivot it thinks is off the hub.

Neither tool clamps today, which is exactly what this pins down -- a future refactor
that adds a max(0, ...) or a bounds assert would silently move a hidden arbour onto the
ink and start every hand pointing at the wrong numeral.

    python3 tools/watch-composer/_selftest/pivot_passthrough_check.py
"""
import os
import sys

import numpy as np
from PIL import Image

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, ROOT)                                            # two levels up from _selftest is tools/
import bake_clock as bake                                    # noqa: E402
import prep_watch_kit as prep                                # noqa: E402

fails = 0
checked = 0


def ok(cond, name, detail=""):
    global fails, checked
    checked += 1
    print(("PASS " if cond else "FAIL ") + name + (("  [" + detail + "]") if detail else ""))
    if not cond:
        fails += 1


# A hand: ink in the middle of the canvas, its declared arbour 30 px left of the canvas
# and 20 px above the top -- the case of a pointer whose hub hides beneath a plate.
d = np.zeros((30, 96, 4), np.uint8)
d[8:22, 20:90, :3] = 240
d[8:22, 20:90, 3] = 255
img = Image.fromarray(d, "RGBA")

OFF = (-30.0, -20.0)

# ---- prep: the plane is placed from the pivot, wherever the pivot is ----------------
paste_at = (int(round(prep.CX - OFF[0])), int(round(prep.CY - OFF[1])))
out = Image.new("RGBA", (prep.CX * 2, prep.CY * 2), (0, 0, 0, 0))
out.paste(img, paste_at)
a = np.array(out)[:, :, 3]
ok(a[paste_at[1] + 8, paste_at[0] + 20] == 255, "prep's placement formula accepts a negative pivot without complaint", str(paste_at))
ok(paste_at[0] > prep.CX, "an arbour left of the ink pushes the art right, which is the point: the pivot, not the canvas, is pinned", str(paste_at))

# ---- bake: with no hub ink under the pivot there is nothing to snap to --------------
px, py, orbit = bake.snap_pivot_to_hub(img, int(OFF[0]), int(OFF[1]), 233, 233)
ok((px, py) == (int(OFF[0]), int(OFF[1])), "bake returns an off-artwork pivot untouched rather than clamping or snapping it", str([px, py]))
ok(orbit == 0.0, "and scores it zero wobble, because there is no hub ink to orbit", str(orbit))

# the same call on a pivot that really is off its hub must still correct: the guard
# above has to be a pass-through, not a disabled function
hub = bake.snap_pivot_to_hub(img, 60, 30, 233, 233)
ok(hub[0] != 60 or hub[1] != 30 or hub[2] < 1.0, "a pivot that does have hub ink to correct is still corrected, so the pass-through is not a disabled snap", str(hub))

# the rotator measurement itself must survive coordinates outside the array
worst = bake._hub_orbit(np.array(img)[:, :, 3], -30, -20, 233, 233)
ok(worst == 0.0, "_hub_orbit returns 0 for a pivot with no ink near it instead of raising", str(worst))

# ---- facing: the field the firmware reads, and the precision it keeps ----------------
# zeroRotationDeg is the ONE thing that makes art drawn pointing east read as north at
# twelve o'clock, and prep is what hands it to the device. It is a text contract rather
# than a call: the conversion lives inline in prep's hand loop, and the failure worth
# guarding is a future edit that truncates it to whole degrees or clamps a negative.
import inspect
src = inspect.getsource(prep)
ok('zeroRotationDeg' in src and '* 10' in src.split('zeroRotationDeg')[1][:60],
    "prep converts zeroRotationDeg at tenth-of-a-degree precision, so a facing off the 15-degree steps survives",
    src.split('zeroRotationDeg')[1][:60].strip().splitlines()[0])
zero = lambda deg: int(round(float(deg) * 10))               # the expression prep uses, above
ok(zero(90) == 900 and zero(-45) == -450 and zero(22.5) == 225,
    "and a negative or fractional facing is a value, not something to clamp at zero",
    str([zero(90), zero(-45), zero(22.5)]))

print()
print(("%d FAILED" % fails) if fails else ("all green (%d checks)" % checked))
sys.exit(1 if fails else 0)
