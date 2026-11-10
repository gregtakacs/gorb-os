#!/usr/bin/env python3
"""orbchrome.py — find a Chrome/Chromium and run one headless bake, on any host.

The shelf rebake (tools/rebake_shelf.py) and each app's bake check run the editors' own
bake pages in headless Chrome: a shelf theme is the bytes that page's Send makes, so the
bake itself never changes — only how a browser is found, where files are staged, how the
page URL is formed and how a stuck browser is stopped. Before this module all four did that
inline, hard-wired to ONE setup: Windows Chrome driven from WSL. This makes the same bakes
run with Chrome or Chromium on Linux, macOS, native Windows and WSL, and keeps today's WSL
behaviour as the default whenever the Windows Chrome is there.

Where a browser is a Windows .exe reached through /mnt/<drive>/ (WSL driving Windows Chrome),
staging and profiles stay exactly where they always were — under C:\\Temp, i.e. /mnt/c/Temp on
the host — and are handed to Chrome as C:\\Temp\\<name> / file:///C:/Temp/<name>/..., byte for
byte as today. Anywhere else they live under tempfile.gettempdir()/<name> and the URL is
pathlib.Path(p).as_uri(), correct on Windows, macOS and Linux.
"""
import os
import pathlib
import platform as _platform
import re
import shutil
import signal
import subprocess
import sys
import tempfile

LINUX_NAMES = ("google-chrome", "google-chrome-stable", "chromium", "chromium-browser")
WSL_WIN_CHROME = ("/mnt/c/Program Files/Google/Chrome/Application/chrome.exe",
                  "/mnt/c/Program Files (x86)/Google/Chrome/Application/chrome.exe")
MAC_APPS = ("/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
            "/Applications/Chromium.app/Contents/MacOS/Chromium")

# a /mnt/<drive>/... path (the WSL view of a Windows volume), matched case-insensitively
_MNT = re.compile(r"^/mnt/([a-zA-Z])(?:/(.*))?$")


def is_wsl(platform=sys.platform, release=None):
    """True on Windows Subsystem for Linux, where a Linux kernel reports 'microsoft' or 'wsl'
    in its release. release is platform.uname().release, injectable for the host suite."""
    if platform != "linux":
        return False
    if release is None:
        release = _platform.uname().release
    r = (release or "").lower()
    return "microsoft" in r or "wsl" in r


def _split_mnt(path):
    """(DRIVE, rest-with-leading-slash) for a /mnt/<drive>/... host path, else None."""
    m = _MNT.match(path or "")
    if not m:
        return None
    return m.group(1).upper(), ("/" + m.group(2) if m.group(2) else "")


def wants_windows_paths(chrome):
    """True when chrome is a Windows .exe under /mnt/<drive>/ — WSL driving Windows Chrome,
    the one case where staging must sit on the Windows volume and paths go to Chrome in
    Windows form. Native-Windows Chrome (a C:\\ path) is False: on Windows the temp dir and
    as_uri() already speak the browser's language."""
    return _split_mnt(chrome) is not None and chrome.lower().endswith(".exe")


def _candidates(env=os.environ, platform=sys.platform, release=None):
    """The paths/names find_chrome would try, in order, for this host — also what the
    'no Chrome' message lists."""
    out = []
    if platform == "win32":
        for var in ("PROGRAMFILES", "PROGRAMFILES(X86)", "LOCALAPPDATA"):
            base = env.get(var)
            if base:
                out.append(os.path.join(base, "Google", "Chrome", "Application", "chrome.exe"))
        out.append("chrome")
    elif is_wsl(platform=platform, release=release):
        out.extend(WSL_WIN_CHROME)
        out.extend(LINUX_NAMES)
    elif platform == "darwin":
        out.extend(MAC_APPS)
        out.extend(LINUX_NAMES)
    else:                                              # linux (not WSL) and anything else
        out.extend(LINUX_NAMES)
    return out


def missing(env=os.environ, platform=sys.platform, release=None):
    """The line every caller prints when find_chrome returns None."""
    return "no Chrome or Chromium found: set ORB_CHROME to its path (tried: %s)" % \
        ", ".join(_candidates(env, platform, release))


def find_chrome(env=os.environ, platform=sys.platform, exists=os.path.exists,
                which=shutil.which, release=None):
    """The browser to run a bake in, or None. ORB_CHROME wins when set — and a set-but-missing
    one is None too, so the caller says so rather than quietly falling back. Otherwise, by
    platform (see _candidates for the order)."""
    forced = env.get("ORB_CHROME")
    if forced:
        return forced if exists(forced) else None
    for cand in _candidates(env, platform, release):
        if cand in LINUX_NAMES or cand == "chrome":
            got = which(cand)
            if got:
                return got
        elif exists(cand):
            return cand
    return None


def _base_dir(chrome):
    return "/mnt/c/Temp" if wants_windows_paths(chrome) else tempfile.gettempdir()


def _fresh(name, chrome):
    d = os.path.join(_base_dir(chrome), name)
    if os.path.isdir(d):
        # tolerant: under WSL a chrome.exe that outlived its bake can still hold files of its
        # profile, and a locked file must not turn the next bake into a crash
        shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d, exist_ok=True)
    return d


def stage_dir(name, chrome):
    """A fresh, empty staging dir the browser can read, as a HOST path the caller copies
    files into. Under /mnt/c/Temp/<name> for WSL-with-Windows-Chrome (today's dirs), else
    under the temp dir."""
    return _fresh(name, chrome)


def profile_dir(name, chrome):
    """A fresh --user-data-dir (a HOST path; run_headless hands it to Chrome in the form that
    browser wants)."""
    return _fresh(name, chrome)


def to_browser_path(path, chrome):
    """What the browser needs on its command line for a host path — the profile dir. A WSL
    /mnt/<drive>/x path becomes X:\\x (backslashes); anywhere else the host path as is."""
    d = _split_mnt(path)
    if d and wants_windows_paths(chrome):
        return d[0] + ":" + d[1].replace("/", "\\")
    return path


def file_url(path, chrome):
    """A file:// URL of a staged file, as this browser must see it. For WSL driving Windows
    Chrome, file:///<DRIVE>:/Temp/<name>/... (forward slashes, as today); otherwise
    pathlib's as_uri(), correct on Windows, macOS and Linux."""
    d = _split_mnt(path)
    if d and wants_windows_paths(chrome):
        return "file:///" + d[0] + ":" + d[1]
    return pathlib.Path(path).as_uri()


def _stop(proc, chrome, profile):
    """Stop the browser we started and everything it spawned, on timeout."""
    if wants_windows_paths(chrome):
        # WSL: the interop shim is ours, but the Windows chrome.exe tree is not — match it by
        # the profile dir it was launched with (killing the WSL side leaves it holding the
        # profile, and the next bake exits 21).
        match = os.path.basename(profile)
        subprocess.run(["powershell.exe", "-NoProfile", "-Command",
                        "Get-CimInstance Win32_Process -Filter \"Name='chrome.exe'\" | "
                        "Where-Object { $_.CommandLine -match '%s' } | "
                        "ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }" % match],
                       capture_output=True)
    elif os.name != "nt":
        try:
            os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
        except (ProcessLookupError, PermissionError, OSError):
            pass
    else:
        subprocess.run(["taskkill", "/T", "/F", "/PID", str(proc.pid)], capture_output=True)
    try:
        if proc.poll() is None:
            proc.kill()
    except OSError:
        pass
    try:
        proc.wait(timeout=10)
    except Exception:
        pass


def run_headless(chrome, url, profile, timeout, extra=()):
    """Run one headless bake and return its CompletedProcess; raise TimeoutExpired(timeout)
    after stopping the browser it started. Flags are the ones the bakes need (headless=new,
    no sandbox, file access, --dump-dom) plus each caller's own extras."""
    cmd = [chrome, "--headless=new", "--disable-gpu", "--no-sandbox",
           "--user-data-dir=" + to_browser_path(profile, chrome),
           "--allow-file-access-from-files", "--dump-dom"]
    cmd += list(extra)
    cmd.append(url)
    kw = {}
    if os.name != "nt" and not wants_windows_paths(chrome):
        kw["start_new_session"] = True                  # so killpg can take the whole tree
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, **kw)
    try:
        out, err = proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        _stop(proc, chrome, profile)
        raise
    return subprocess.CompletedProcess(cmd, proc.returncode, out, err)

