#!/usr/bin/env python3
"""check_app_versions.py — one stamp names one set of bytes, across releases (ruling 39).

Every shelf ``.orbapp`` and the hub's firmware bundle ship under a version string. That
version is a promise: the bytes behind it do not move without it moving too. An app that
re-bakes to different bytes under the same ``version.txt`` is a silent re-release — an
operator who already installed v1.7.0 is handed a different v1.7.0 and the install ladder
(its serial rides the same version) refuses the update, so the changed bytes never reach
the orb. Ruling 39 makes that a gate failure here, at the tree, before it ships.

The reference is the newest reachable *release* tag: the last thing that left this tree as
an artifact. For each shipped blob, if its working-tree bytes differ from that tag's copy
while its version is not strictly higher than at the tag, that is a changed artifact under
an unchanged version. Same bytes pass whatever the version says; a moved version over still
bytes is a harmless bump with no bake.

    python3 tools/check_app_versions.py

The firmware bundle is versioned by the tree's own ``version.txt`` (RELEASING.md: the
release refuses a bundle whose version is not ``version.txt``), so it rides the root
version against the tag; each app rides its own ``apps/<slug>/version.txt``.

Only ``vX.Y.Z`` tags count. A suffixed hub-rebuild tag (``v1.2.9-2``, RELEASING.md step 2)
is a re-cut of the same OS version, not a release: it carries no new version fact, so it is
not the reference a bump is measured against.
"""
import importlib.util
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, os.pardir))
sys.path.insert(0, HERE)
_mk = importlib.util.spec_from_file_location(
    "make_factory_apps", os.path.join(HERE, "make_factory_apps.py"))
make_factory_apps = importlib.util.module_from_spec(_mk)
_mk.loader.exec_module(make_factory_apps)

RELEASE_RE = re.compile(r"^v(\d+)\.(\d+)\.(\d+)$")
SHELF_DIR = os.path.join("tools", "orb-manager", "apps")
FIRMWARE = os.path.join("tools", "orb-manager", "internal", "firmware", "bundle",
                        "orb-firmware.zip")


def git_show(tag, rel):
    """Raw bytes of ``rel`` at ``tag``, or None when the object/path is absent."""
    r = subprocess.run(["git", "show", "%s:%s" % (tag, rel)], cwd=ROOT,
                       capture_output=True)
    return None if r.returncode != 0 else r.stdout


def newest_release_tag():
    """Greatest reachable vX.Y.Z tag, or None. Suffixed hub-rebuild tags are not releases."""
    r = subprocess.run(["git", "tag", "--merged", "HEAD"], cwd=ROOT,
                       capture_output=True, text=True)
    if r.returncode != 0:
        return None
    best, best_key = None, None
    for name in r.stdout.split():
        m = RELEASE_RE.match(name)
        if not m:
            continue
        key = tuple(int(g) for g in m.groups())
        if best_key is None or key > best_key:
            best, best_key = name, key
    return best


def parse_version(v):
    """vX.Y.Z -> (X, Y, Z), or None if it does not parse."""
    m = RELEASE_RE.match((v or "").strip())
    return tuple(int(g) for g in m.groups()) if m else None


def references():
    """(label, artifact_rel, version_rel) for every blob ruling 39 guards: each shelf app
    against its own version.txt, and the firmware bundle against the root version.txt."""
    refs = []
    for slug, m in sorted(make_factory_apps.app_dirs().items()):
        if not m.get("shelf"):
            continue
        refs.append((slug, os.path.join(SHELF_DIR, slug + ".orbapp"),
                     os.path.join("apps", slug, "version.txt")))
    refs.append(("firmware", FIRMWARE, "version.txt"))
    return refs


def main():
    tag = newest_release_tag()
    if tag is None:
        sys.stderr.write(
            "check_app_versions: no vX.Y.Z release tag is reachable from HEAD; ruling 39 "
            "needs a released reference (git tag --merged HEAD). Fetch tags — in CI the "
            "checkout must run with fetch-depth: 0.\n")
        return 1
    fails = []
    refs = references()
    for label, artifact, version_rel in refs:
        work = os.path.join(ROOT, version_rel)
        cur = open(work).read().strip() if os.path.exists(work) else None
        tag_v = (git_show(tag, version_rel) or b"").decode("utf-8", "replace").strip() or None
        tag_bytes = git_show(tag, artifact)
        work_path = os.path.join(ROOT, artifact)
        work_bytes = open(work_path, "rb").read() if os.path.exists(work_path) else None
        changed = work_bytes is None or tag_bytes is None or work_bytes != tag_bytes
        if changed and not (tag_bytes is None and tag_v is None):
            cur_k, tag_k = parse_version(cur), parse_version(tag_v)
            if not (cur_k is not None and tag_k is not None and cur_k > tag_k):
                fails.append("%s changed since %s but its version is %s, the same as or lower than "
                             "%s at %s: raise it"
                             % (label, tag, cur or "absent", tag_v or "absent", tag))
    for f in fails:
        sys.stderr.write("refused: %s\n" % f)
    if fails:
        return 1
    print("app versions: %d checked against %s" % (len(refs), tag))
    return 0


if __name__ == "__main__":
    sys.exit(main())
