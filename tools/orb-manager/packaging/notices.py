#!/usr/bin/env python3
"""Regenerate packaging/THIRD_PARTY_NOTICES.txt.

    python3 packaging/notices.py > packaging/THIRD_PARTY_NOTICES.txt   (from tools/orb-manager)

Reads the Go module licences from the module cache (go mod download first), the font
families from the vendored fonts.css, and the Apache-2.0 text from an ESP-IDF checkout
($IDF_PATH, default ~/esp/esp-idf). Each font's OFL.txt is fetched from the google/fonts
repository for its copyright lines; a family that is not under ofl/ there fails the run,
because it needs a licence this file does not carry. The versions and copyright lines of
the hand-listed entries below are the ones to update when a dependency moves.
"""
import os, re, subprocess, sys, urllib.request

PKG = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
M = subprocess.run(['go', 'env', 'GOMODCACHE'], capture_output=True, text=True, check=True).stdout.strip()
IDF = os.environ.get('IDF_PATH') or os.path.expanduser('~/esp/esp-idf')
FONTS_CSS = os.path.join(PKG, 'web', 'runtime', 'vendor', 'fonts.css')
out = []
w = out.append
def rd(p): return open(p, encoding='utf-8').read().strip('\n')
_ofl = {}
def ofl(fam):
    slug = fam.replace(' ', '').lower()
    if slug not in _ofl:
        u = 'https://raw.githubusercontent.com/google/fonts/main/ofl/%s/OFL.txt' % slug
        with urllib.request.urlopen(u, timeout=30) as r:
            _ofl[slug] = r.read().decode('utf-8').strip('\n')
    return _ofl[slug]
w("Orb Manager — third-party notices\n")
w("Orb Manager is MIT-licensed (see LICENSE). It redistributes the software and fonts\nbelow, and it embeds a gorb-os firmware image that it flashes onto an orb; that image\nlinks the firmware components listed here. Each is listed with its licence, and the\nfull text of every licence follows.\n")
rule = '=' * 78
def section(t): w(rule); w(t); w(rule)
section("Hub executable (Go modules)")
w("go.bug.st/serial v1.6.4 ............. BSD-3-Clause  (C) 2014-2024 Cristian Maglie")
w("github.com/creack/goselect v0.1.2 .... MIT           (C) 2014 Guillaume J. Charmes")
w("golang.org/x/sys v0.19.0 ............. BSD-3-Clause  (C) 2009 The Go Authors")
w("Go standard library .................. BSD-3-Clause  (C) 2009 The Go Authors\n")
section("Editor runtime (vendored into the hub)")
w("React 18.3.1, ReactDOM 18.3.1 ........ MIT           (C) Facebook, Inc. and its affiliates")
w("Babel standalone 7.29.0 .............. MIT           (C) 2014-present Sebastian McKenzie and other contributors\n")
section("Fonts (vendored into the hub, all SIL Open Font License 1.1)")
fams = sorted(set(re.findall(r"font-family: *'([^']*)'", rd(FONTS_CSS))))
for fam in fams:
    first = [l for l in ofl(fam).splitlines() if l.strip()]
    cr = []
    for l in first:
        if l.startswith('This Font Software'): break
        cr.append(l.strip())
    w(fam + ':')
    for l in cr: w('  ' + l)
w('')
section("Firmware embedded in the hub (flashed onto the orb)")
w("ESP-IDF v5.4 ......................... Apache-2.0    (C) Espressif Systems (Shanghai) Co. Ltd.")
w("espressif/esp_tinyusb 3.0.0 .......... Apache-2.0    (C) Espressif Systems (Shanghai) Co. Ltd.")
w("TinyUSB (inside esp_tinyusb) ......... MIT           (C) 2012-2026 hathach (tinyusb.org)")
w("espressif/esp_lcd_co5300 2.2.0 ....... Apache-2.0    (C) Espressif Systems (Shanghai) Co. Ltd.")
w("espressif/cmake_utilities 0.5.3 ...... Apache-2.0    (C) Espressif Systems (Shanghai) Co. Ltd. (build-time only)")
w("LVGL 9.6.0 ........................... MIT           (C) 2025 LVGL Kft")
w("wasm3 ................................ MIT           (C) 2019 Steven Massey, Volodymyr Shymanskyy\n")
section("Licence texts")
w("--- MIT (React, ReactDOM, Babel, goselect, TinyUSB, LVGL, wasm3; each under its own copyright line above) ---\n")
w(rd(os.path.join(M, 'github.com/creack/goselect@v0.1.2/LICENSE')).replace('Copyright (c) 2014 Guillaume J. Charmes', 'Copyright (c) <the holders named above>'))
w("\n--- BSD-3-Clause: go.bug.st/serial ---\n")
w(rd(os.path.join(M, 'go.bug.st/serial@v1.6.4/LICENSE')))
w("\n--- BSD-3-Clause: golang.org/x/sys and the Go standard library ---\n")
w(rd(os.path.join(M, 'golang.org/x/sys@v0.19.0/LICENSE')))
w("\n--- SIL Open Font License 1.1 (every font above) ---\n")
t = ofl('Jost')
w(t[t.index('This Font Software'):])
w("\n--- Apache License 2.0 (ESP-IDF, esp_tinyusb, esp_lcd_co5300, cmake_utilities) ---\n")
w(rd(os.path.join(IDF, 'LICENSE')))
print('\n'.join(out))
