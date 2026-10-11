#!/usr/bin/env python3
"""Deploy the Watch Composer + browser selftest to the Windows test folder.

Chrome on the Windows side of this WSL box is the only place the composer's real
pixel path, its file dialogs and its localStorage asset store can be exercised:
the headless suites cover the decisions, not the rendering. This copies the five
files the app loads plus the whole _selftest folder into one flat Windows folder
laid out so every relative <script src> still resolves, then prints the two
file:// URLs to open.

    python3 tools/watch-composer/deploy_test_folder.py [target_dir]

default target: /mnt/c/Users/greg/Downloads/test
"""
import hashlib
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_TARGET = "/mnt/c/Users/greg/Downloads/test"

# the app loads these four siblings; _selftest/ is copied whole and reaches the
# engine at ../watch-engine.js, so the folder depth is part of the contract
APP_FILES = ["Watch Composer.dc.html", "support.js", "dial-renderer.js", "watch-engine.js"]
SELFTEST_DIR = "_selftest"


def sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for blk in iter(lambda: fh.read(1 << 16), b""):
            h.update(blk)
    return h.hexdigest()[:12]


def main():
    target = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_TARGET
    if not os.path.isdir(os.path.dirname(target)):
        sys.exit("parent of {} does not exist; is /mnt/c mounted?".format(target))

    plan = [(os.path.join(HERE, f), os.path.join(target, f)) for f in APP_FILES]
    st_src = os.path.join(HERE, SELFTEST_DIR)
    for name in sorted(os.listdir(st_src)):
        src = os.path.join(st_src, name)
        if os.path.isfile(src):
            plan.append((src, os.path.join(target, SELFTEST_DIR, name)))

    os.makedirs(os.path.join(target, SELFTEST_DIR), exist_ok=True)
    n_bytes = 0
    for src, dst in plan:
        if not os.path.isfile(src):
            sys.exit("missing required file: " + src)
        before = sha(src)
        shutil.copyfile(src, dst)
        after = sha(dst)
        if before != after:
            # drvfs caches aggressively; a mismatch means Chrome would load stale JS
            sys.exit("copy verification failed for " + dst)
        n_bytes += os.path.getsize(dst)
        print("  {:>8} B  {}".format(os.path.getsize(dst), os.path.relpath(dst, target)))

    print("\n{} files, {:.1f} MiB -> {}".format(len(plan), n_bytes / 1048576, target))
    print("\nopen in Windows Chrome:")
    # Chrome on Windows takes forward slashes in a file: URL; a raw space does not
    # survive the address bar reliably, so encode it
    win = "file:///" + target.replace("/mnt/c/", "C:/").rstrip("/") + "/"
    for rel in ["Watch Composer.dc.html", os.path.join(SELFTEST_DIR, "export-test.html")]:
        print("  " + win + rel.replace(" ", "%20"))
    print("\nselftest verdict: the page title must read PASS and the last report line")
    print("must be 'RESULT: PASS'. Seven report lines are the evidence for the authoring")
    print("features; a missing line means that block never ran, which is a fail, not a pass:")
    print("  'custom art: backdrop in the plate pixels'               <- the plate slot")
    print("  'dial surface art: N art px ... print still reads'       <- the dial face, under the print")
    print("  'round aperture: r=N fills 0.785 ...'                    <- a circular window")
    print("  'no case: the case layer paints 0 px ...'                <- the case-free style draws nothing, cuts nothing, rescales nothing")
    print("  'seconds row: the seconds list reshapes that hand ...'   <- the seconds row offers the list that actually shapes it")
    print("  'alignment reference: N of 53 grid points ...'           <- the guide is pinned to the watch, not the art")
    print("  'zoom 1->2: ... facing turned the ink N -> N deg'        <- art zoom + hand facing")
    print("Run one block alone with ?ONLY=feat if the suite stops printing with no verdict:")
    print("that is a CPU-bound block outrunning --virtual-time-budget, not necessarily a bug.")
    print("localStorage is shared across every file:// page in Chrome, so assets uploaded")
    print("from the repo copy appear here too.");


if __name__ == "__main__":
    main()
