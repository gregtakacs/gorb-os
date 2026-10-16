"""Run the FORMAT 3 bundle mutants: break one rule, require the suite to fail.

Kept as a file because the shell here mangles heredocs — same reason as
mut_element.py. The suite is the product; this is the proof the suite has teeth.
Delete a gate's condition and the suite must say so, or the gate was never tested.

Run from the repo root:  cp tools/orbbundle.py /tmp/ob3.keep && python3 tools/mut_bundle.py
"""
import shutil
import subprocess
import sys

KEEP = "/tmp/ob3.keep"
src = open(KEEP).read()

MUT = [
    ("drop region cap", 'if _ot.HDR_WORDS * 4 + run > GEOM_CAP:', 'if False:'),
    ("drop slot cap", 'if art_off + art_size > SLOT_CAP:', 'if False:'),
    ("drop dup-slug refusal", 'if slug in seen:', 'if False:'),
    ("loosen slug charset", "or any(not ((\"a\" <= c <= \"z\")", "or any(not ((\"A\" <= c <= \"z\")"),
    ("drop geom_off walk", 'if r["geom_off"] != walk:', 'if False:'),
    ("drop blob length refusal", 'if want != len(blob):', 'if False:'),
    ("drop region crc", 'if zlib.crc32(region) & 0xFFFFFFFF != h["geom_crc"]:', 'if False:'),
    ("drop bank crc", 'if zlib.crc32(a) & 0xFFFFFFFF != r["art_crc"]:', 'if False:'),
    ("drop blob crc", 'if zlib.crc32(blob) & 0xFFFFFFFF != r["geom_crc"]:', 'if False:'),
    ("stop sorting sections", 'sections = sorted(sections, key=lambda s: s["slug"])', 'pass'),
    ("drop empty-section refusal", 'if not s.get("elements") and not art:', 'if False:'),
    ("drop format word check", 'if hdr[1] != FORMAT3:', 'if False:'),
]

survived = miss = 0
for name, old, new in MUT:
    if old not in src:
        print("  ANCHOR LOST %s" % name)
        miss += 1
        continue
    open("tools/orbbundle.py", "w").write(src.replace(old, new, 1))
    r = subprocess.run([sys.executable, "tools/host_theme_format3_test.py"],
                       capture_output=True, text=True)
    tail = (r.stdout.strip().splitlines() or [""])[-1]
    if r.returncode == 0:
        print("  SURVIVED %-22s %s" % (name, tail))
        survived += 1
    else:
        print("  caught   %-22s %s" % (name, tail))
shutil.copy(KEEP, "tools/orbbundle.py")
print("== format3-bundle mutants: %d/%d caught, %d anchors missed"
      % (len(MUT) - survived - miss, len(MUT), miss))
sys.exit(1 if (survived or miss) else 0)
