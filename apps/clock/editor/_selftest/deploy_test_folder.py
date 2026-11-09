#!/usr/bin/env python3
"""Deploy the Watch Composer + browser selftest to the Windows test folder.

Chrome on the Windows side of this WSL box is the only place the composer's real
pixel path, its file dialogs and its localStorage asset store can be exercised:
the headless suites cover the decisions, not the rendering. This copies the files
the app loads plus the whole _selftest folder into one flat Windows folder laid
out so every relative <script src> still resolves, then prints the two file://
URLs to open. Since D43 the page's support.js and orbtheme-baker.js are the hub's
OS runtime (tools/orb-manager/web/runtime/); the flat folder gets copies of them
beside the app's own files, which is the layout the selftest pages load.

    python3 apps/clock/editor/_selftest/deploy_test_folder.py [target_dir]

default target: /mnt/c/Users/greg/Downloads/test
"""
import hashlib
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))          # _selftest/, since E7 Stage 2
ED = os.path.dirname(HERE)                                  # the editor's own files
DEFAULT_TARGET = "/mnt/c/Users/greg/Downloads/test"

RUNTIME = os.path.join(ED, "..", "..", "..", "tools", "orb-manager", "web", "runtime")
# the page loads these siblings; _selftest/ is copied whole and reaches the engine at
# ../watch-engine.js, so the folder depth is part of the contract. (source, name):
# the app's own files from here, the runtime's from the hub.
APP_FILES = [(os.path.join(ED, f), f) for f in
             ("clock-logic.js", "dial-renderer.js", "watch-engine.js", "sound-engine.js", "stack.js")] + [
             (os.path.join(RUNTIME, "support@1", "support.js"), "support.js"),
             (os.path.join(RUNTIME, "baker@1", "orbtheme-baker.js"), "orbtheme-baker.js"),
             (os.path.join(RUNTIME, "ui@1", "ui.css"), "ui.css"),
             (os.path.join(RUNTIME, "theme@1", "send.js"), "send.js"),
             (os.path.join(RUNTIME, "theme@1", "session.js"), "session.js"),
             (os.path.join(RUNTIME, "theme@1", "preview.js"), "preview.js"),
             (os.path.join(RUNTIME, "theme@1", "measure.js"), "measure.js"),
             (os.path.join(RUNTIME, "studio@1", "studio.js"), "studio.js"),
             (os.path.join(RUNTIME, "studio@1", "studio.css"), "studio.css")]
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

    plan = [(src, os.path.join(target, name)) for src, name in APP_FILES]
    st_src = HERE
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
    for rel in ["Watch Composer.dc.html"]:
        print("  " + win + rel.replace(" ", "%20"))
    print("\nThe headless suites under _selftest/ run with node; bank3-drive.html and")
    print("bake3-test.html bake through the real editor under --headless Chrome.")


if __name__ == "__main__":
    main()
