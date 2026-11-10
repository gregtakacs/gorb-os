#!/usr/bin/env python3
"""host_orbchrome_test.py — tools/orbchrome.py, tested on the host with no browser.

    python3 tools/host_orbchrome_test.py

Nothing here launches Chrome or writes outside a temp dir: platform, env, exists, which and
the WSL release string are injected, so the find order, the Windows-vs-temp-dir routing and
the exact C:\\Temp\\... / file:///C:/Temp/... strings (today's, byte for byte, on WSL with the
Windows Chrome) are all checked as pure functions. The temp-dir side is exercised for real
under tempfile, and cleaned up.
"""
import os
import pathlib
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import orbchrome as oc          # noqa: E402

FAILS = []


def check(name, ok, detail=""):
    print(("ok    " if ok else "FAIL  ") + name + (("  -- " + detail) if detail and not ok else ""))
    if not ok:
        FAILS.append(name)


WSL_CHROME = "/mnt/c/Program Files/Google/Chrome/Application/chrome.exe"
WSL_RELEASE = "5.15.0-139.2-microsoft-standard-WSL2"
NOT_WSL_RELEASE = "6.5.0-42-generic"


def finder(exists_paths=(), which_map=None):
    """An (exists, which) pair for injected find_chrome/candidate checks."""
    es = set(exists_paths)
    wm = dict(which_map or {})
    return (lambda p: p in es), (lambda n: wm.get(n))


def main():
    # --- is_wsl: only a linux kernel naming microsoft/wsl in its release ---
    check("is_wsl is true on a WSL release", oc.is_wsl("linux", WSL_RELEASE))
    check("is_wsl is true on an older -rc1-microsoft release", oc.is_wsl("linux", "4.4.0-19041-Microsoft"))
    check("is_wsl is false on plain linux", not oc.is_wsl("linux", NOT_WSL_RELEASE))
    check("is_wsl is false on win32", not oc.is_wsl("win32", WSL_RELEASE))
    check("is_wsl is false on darwin", not oc.is_wsl("darwin", WSL_RELEASE))

    # --- wants_windows_paths: a .exe under /mnt/<drive>/ only ---
    check("wants_windows_paths: /mnt/c Windows chrome", oc.wants_windows_paths(WSL_CHROME))
    check("wants_windows_paths: another drive, chromium exe",
          oc.wants_windows_paths("/mnt/d/Program Files/Chromium/chrome.exe"))
    check("wants_windows_paths: native-Windows chrome (C:\\ path) is False",
          not oc.wants_windows_paths(r"C:\Program Files\Google\Chrome\Application\chrome.exe"))
    check("wants_windows_paths: linux chromium is False", not oc.wants_windows_paths("/usr/bin/chromium"))
    check("wants_windows_paths: a /mnt/c dir with no .exe is False",
          not oc.wants_windows_paths("/mnt/c/Program Files/Google/Chrome/Application/chrome"))
    check("wants_windows_paths: None is False", not oc.wants_windows_paths(None))

    # --- find order, WSL: the two Windows Chrome paths, then the Linux names ---
    order = oc._candidates({}, "linux", WSL_RELEASE)
    check("WSL find order leads with the two Windows Chrome paths",
          order[:2] == list(oc.WSL_WIN_CHROME), str(order[:2]))
    check("WSL find order falls back to the Linux names",
          order[2:] == list(oc.LINUX_NAMES), str(order[2:]))
    ex, wh = finder(exists_paths=[oc.WSL_WIN_CHROME[1]], which_map={"chromium": "/usr/bin/chromium"})
    check("WSL: the (x86) Windows Chrome is found when the plain one is absent",
          oc.find_chrome({}, "linux", ex, wh, WSL_RELEASE) == oc.WSL_WIN_CHROME[1])
    ex, wh = finder(exists_paths=[WSL_CHROME], which_map={"google-chrome": "/usr/bin/google-chrome"})
    check("WSL: the Windows Chrome wins over a Linux google-chrome on PATH",
          oc.find_chrome({}, "linux", ex, wh, WSL_RELEASE) == WSL_CHROME)
    ex, wh = finder(which_map={"chromium": "/usr/bin/chromium", "google-chrome": "/usr/bin/google-chrome"})
    check("WSL: with no Windows Chrome it takes the first Linux name on PATH",
          oc.find_chrome({}, "linux", ex, wh, WSL_RELEASE) == "/usr/bin/google-chrome")

    # --- find order, plain linux: the Linux names via which, in order ---
    order = oc._candidates({}, "linux", NOT_WSL_RELEASE)
    check("linux find order is the four names, nothing Windows", order == list(oc.LINUX_NAMES), str(order))
    ex, wh = finder(which_map={"chromium-browser": "/usr/bin/chromium-browser"})
    check("linux: the last name is taken when only it is on PATH",
          oc.find_chrome({}, "linux", ex, wh, NOT_WSL_RELEASE) == "/usr/bin/chromium-browser")
    ex, wh = finder()
    check("linux: nothing on PATH gives None", oc.find_chrome({}, "linux", ex, wh, NOT_WSL_RELEASE) is None)

    # --- find order, macOS: the two .app paths, then the Linux names ---
    order = oc._candidates({}, "darwin", NOT_WSL_RELEASE)
    check("darwin find order leads with the two app paths", order[:2] == list(oc.MAC_APPS), str(order[:2]))
    check("darwin find order falls back to the Linux names", order[2:] == list(oc.LINUX_NAMES))
    ex, wh = finder(exists_paths=[oc.MAC_APPS[1]])
    check("darwin: the Chromium app is found when Chrome is not installed",
          oc.find_chrome({}, "darwin", ex, wh, NOT_WSL_RELEASE) == oc.MAC_APPS[1])

    # --- find order, native Windows: the three env roots, then which('chrome') ---
    wenv = {"PROGRAMFILES": r"C:\Program Files", "PROGRAMFILES(X86)": r"C:\Program Files (x86)",
            "LOCALAPPDATA": r"C:\Users\me\AppData\Local"}
    win_paths = [os.path.join(wenv[v], "Google", "Chrome", "Application", "chrome.exe")
                 for v in ("PROGRAMFILES", "PROGRAMFILES(X86)", "LOCALAPPDATA")]
    order = oc._candidates(wenv, "win32")
    check("win32 find order names Program Files, (x86), LocalAppData then chrome",
          order == win_paths + ["chrome"], str(order))
    ex, wh = finder(exists_paths=[win_paths[2]])
    check("win32: a LocalAppData chrome is found",
          oc.find_chrome(wenv, "win32", ex, wh) == win_paths[2])
    ex, wh = finder(which_map={"chrome": r"C:\chrome.exe"})
    check("win32: with no install which('chrome') is taken",
          oc.find_chrome({}, "win32", ex, wh) == r"C:\chrome.exe")

    # --- ORB_CHROME wins, and a set-but-missing one is None ---
    ex, wh = finder(exists_paths=["/opt/browser/bin/chrome"], which_map={"google-chrome": "/usr/bin/google-chrome"})
    check("ORB_CHROME wins over a system Chrome",
          oc.find_chrome({"ORB_CHROME": "/opt/browser/bin/chrome"}, "linux", ex, wh, NOT_WSL_RELEASE) == "/opt/browser/bin/chrome")
    ex, wh = finder(which_map={"google-chrome": "/usr/bin/google-chrome"})
    check("ORB_CHROME set to a missing path gives None (no silent fallback)",
          oc.find_chrome({"ORB_CHROME": "/nope/chrome"}, "linux", ex, wh, NOT_WSL_RELEASE) is None)

    # --- the exact browser-facing strings, WSL-with-Windows-Chrome (today's, byte for byte) ---
    check("to_browser_path: /mnt/c profile -> C:\\Temp\\... (backslashes)",
          oc.to_browser_path("/mnt/c/Temp/rebake-shelf-profile", WSL_CHROME) == r"C:\Temp\rebake-shelf-profile",
          oc.to_browser_path("/mnt/c/Temp/rebake-shelf-profile", WSL_CHROME))
    check("file_url: staged bake.html -> file:///C:/Temp/... (as today)",
          oc.file_url("/mnt/c/Temp/flights-bake-check/bake.html", WSL_CHROME) == "file:///C:/Temp/flights-bake-check/bake.html",
          oc.file_url("/mnt/c/Temp/flights-bake-check/bake.html", WSL_CHROME))
    check("file_url: clock drive page keeps the _selftest depth",
          oc.file_url("/mnt/c/Temp/rebake-shelf-clock/_selftest/bank3-drive.html", WSL_CHROME)
          == "file:///C:/Temp/rebake-shelf-clock/_selftest/bank3-drive.html")
    check("WSL staging root is /mnt/c/Temp", oc._base_dir(WSL_CHROME) == "/mnt/c/Temp", oc._base_dir(WSL_CHROME))

    # --- the temp-dir / as_uri side, exercised for real under tempfile ---
    chromium = "/usr/bin/chromium"
    check("non-WSL staging root is the temp dir", oc._base_dir(chromium) == tempfile.gettempdir())
    stage = oc.stage_dir("orbchrome-selftest-stage", chromium)
    check("stage_dir returns a fresh dir under the temp dir",
          stage == os.path.join(tempfile.gettempdir(), "orbchrome-selftest-stage") and os.path.isdir(stage))
    prof = oc.profile_dir("orbchrome-selftest-profile", chromium)
    check("profile_dir returns a fresh dir under the temp dir",
          prof == os.path.join(tempfile.gettempdir(), "orbchrome-selftest-profile") and os.path.isdir(prof))
    baked = os.path.join(stage, "bake.html")
    check("file_url is pathlib as_uri off the temp dir", oc.file_url(baked, chromium) == pathlib.Path(baked).as_uri(),
          oc.file_url(baked, chromium))
    check("to_browser_path is the host path unchanged off WSL", oc.to_browser_path(prof, chromium) == prof)
    open(baked, "w").write("x")                                  # the stage really is writable
    oc.stage_dir("orbchrome-selftest-stage", chromium)            # re-stage drops prior content
    check("a re-stage drops prior content", not os.path.exists(baked))
    for d in (stage, prof):
        shutil.rmtree(d, ignore_errors=True)

    # --- the no-browser message every caller prints ---
    msg = oc.missing({}, "linux", NOT_WSL_RELEASE)
    check("missing() names ORB_CHROME and the tried list",
          msg.startswith("no Chrome or Chromium found: set ORB_CHROME to its path (tried: ") and "chromium-browser" in msg, msg)
    check("missing() on WSL lists the Windows Chrome first", "Program Files" in oc.missing({}, "linux", WSL_RELEASE))

    print("all orbchrome checks pass" if not FAILS else "%d FAILED" % len(FAILS))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
