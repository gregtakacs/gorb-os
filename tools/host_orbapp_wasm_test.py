#!/usr/bin/env python3
"""host_orbapp_wasm_test.py — the sentinel for the wasm3 code tier's load gate (P7.2).

The engine (m3_Parse/Load/Call) cannot run off the board, so this suite does what
host_orbapp_install_test.py does for install_decide: lift the PURE decision that says
whether a module may run at all, and pin it with hand-authored header words rather
than a real .wasm. The pure decision is orb_wasm_plan() in main/app/orb_app_wasm.c:
given a slot header the reader already accepted, it answers load-or-refuse and the
exact D27 memory cap to arm. A guest that cannot fit the bound never reaches the
engine, exactly as D27 promises, so this is the last toolchain-free gate.

THE PURE DECISION. orb_wasm_plan is lifted verbatim (dying by name if it moves) and
recompiled with host gcc -Werror against the SHIPPED orbapp_hdr_t (orb_app_manifest.h).
Its code_size / wasm_mem_max fields are set directly per vector — the honest input,
since plan() reads only those words.

THE ENTRY VOCABULARY. The suite lifts the export-name table (s_entries) and pins, via
the COMPILED table, that exactly one export (orb_tick) is required and the name set is
fixed and ordered. A rename, a dropped required flag, or a made-up hook turns a row
red here, not on the dial.

THE DEVICE HALF. The m3 calls cannot run here, so their ORDER is pinned by a
structural lint over the shipped text: the ceiling is armed before LoadModule, imports
bind before Load, the PSRAM image copy precedes Parse, and the trap path bills C2 —
orb_svc_note_fault at the report, and the engine released only once the broker's
second-strike flag lands (P7.3). Reordering or dropping any of those is a lint
failure here.

Run:      python3 tools/host_orbapp_wasm_test.py
Mutants:  python3 tools/host_orbapp_wasm_test.py --mutations
Exit 0 = every vector matched (and, with --mutations, every mutant caught).
"""
import os
import re
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
WASM_C = os.path.join(ROOT, "main", "app", "orb_app_wasm.c")
INSTALL_C = os.path.join(ROOT, "main", "app", "orb_app_install.c")
MAIN_C = os.path.join(ROOT, "main", "main.c")
WASM_H = os.path.join(ROOT, "main", "app", "orb_app_wasm.h")
MANIFEST_H = os.path.join(ROOT, "main", "app", "orb_app_manifest.h")
NL = "\n"


def read(p):
    if not os.path.exists(p):
        sys.exit("missing " + p + " — the lifts below have no fallback")
    return open(p, encoding="utf-8").read()


def lift(text, pattern, where):
    m = re.search(pattern, text, re.S)
    if not m:
        sys.exit("could not lift %r from %s — did the code move? Dying here is the "
                 "point: an empty lift would keep this suite green while testing "
                 "nothing." % (pattern[:60], where))
    return m.group(0)


src = read(WASM_C)
hdr = read(WASM_H)
F_PLAN = lift(src, r"orb_wasm_plan_t orb_wasm_plan\(const orbapp_hdr_t \*h\)\s*\{[\s\S]*?\n\}", WASM_C)
F_ENTRY_ENUM = lift(src, r"enum \{ W_ACTIVATE = 0,[\s\S]*?W_COUNT \};", WASM_C)
F_ENTRY_STRUCT = lift(src, r"typedef struct \{\s*\n\s*const char \*export_name;[\s\S]*?\} wasm_entry_t;", WASM_C)
F_ENTRIES = lift(src, r"static const wasm_entry_t s_entries\[W_COUNT\] = \{[\s\S]*?\n\};", WASM_C)
F_PLANSTRUCT = lift(hdr, r"typedef struct \{\s*\n\s*bool\s+load;[\s\S]*?\} orb_wasm_plan_t;", WASM_H)
F_MEMMAX = lift(hdr, r"#ifndef ORB_WASM_MEM_MAX\s*\n#define ORB_WASM_MEM_MAX[^\n]*\n#endif\n", WASM_H)


SHIM_ESP_ERR = "#pragma once\ntypedef int esp_err_t;\n"

# ---- the harness -------------------------------------------------------------
# Two modes.
#   plan    : reads a vector file (u32 n, then per case code_size, wasm_mem_max),
#             builds an orbapp_hdr_t, runs orb_wasm_plan, prints per line
#             "<i> <load 0/1> <mem_cap> <hex why>".
#   entries : prints the lifted s_entries table, one row per line as
#             "<index> <required 0/1> <name>", pinning the vocabulary by the
#             compiled table rather than a Python copy.
HARNESS_C = r"""
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "orb_app_manifest.h"   /* orbapp_hdr_t */

@MEMMAX@
@PLANSTRUCT@
@PLAN@

@ENTRYENUM@
@ENTRYSTRUCT@
@ENTRIES@

static FILE *F;
static uint32_t rd32(void) { uint32_t v; if (fread(&v, 4, 1, F) != 1) exit(2); return v; }
static void puthex0(const char *s)
{
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) printf("%02x", *p);
}

int main(int argc, char **argv)
{
    if (argc < 3) return 2;
    if (strcmp(argv[1], "entries") == 0) {
        for (int i = 0; i < W_COUNT; i++)
            printf("%d %d %s\n", i, s_entries[i].required ? 1 : 0,
                   s_entries[i].export_name);
        return 0;
    }
    F = fopen(argv[2], "rb");
    if (!F) return 2;
    const uint32_t n = rd32();
    for (uint32_t i = 0; i < n; i++) {
        orbapp_hdr_t h; memset(&h, 0, sizeof h);
        h.code_size    = rd32();
        h.wasm_mem_max = rd32();
        orb_wasm_plan_t p = orb_wasm_plan(&h);
        printf("%u %d %u ", i, (int)p.load, (unsigned)(p.load ? p.mem_cap : 0));
        puthex0(p.why ? p.why : "");
        printf("\n");
    }
    fclose(F);
    return 0;
}
"""


def compile_harness(tmp, mutate=None):
    """Build the lifted plan + entry table against the shipped headers. `mutate`
    is (kind, fn); kind "plan" rewrites the lifted orb_wasm_plan text, kind
    "entries" rewrites the lifted s_entries text. Each mutation must turn a vector
    or a table row red — an uncaught mutant means the vectors do not pin what they
    claim."""
    shim = os.path.join(tmp, "shim")
    os.makedirs(shim, exist_ok=True)
    open(os.path.join(shim, "esp_err.h"), "w").write(SHIM_ESP_ERR)
    open(os.path.join(tmp, "orb_app_manifest.h"), "w").write(read(MANIFEST_H))
    plan, entries = F_PLAN, F_ENTRIES
    if mutate:
        kind, fn = mutate
        if kind == "plan":
            plan = fn(plan)
        else:
            entries = fn(entries)
    harness = (HARNESS_C
               .replace("@MEMMAX@", F_MEMMAX)
               .replace("@PLANSTRUCT@", F_PLANSTRUCT)
               .replace("@PLAN@", plan)
               .replace("@ENTRYENUM@", F_ENTRY_ENUM)
               .replace("@ENTRYSTRUCT@", F_ENTRY_STRUCT)
               .replace("@ENTRIES@", entries))
    hac = os.path.join(tmp, "harness.c")
    open(hac, "w").write(harness)
    binp = os.path.join(tmp, "harness")
    cc = subprocess.run(["gcc", "-std=gnu11", "-O1", "-Wall", "-Wextra",
                         "-Wno-unused-function", "-Werror",
                         "-I", tmp, "-I", os.path.join(ROOT, "main"),
                         "-I", os.path.join(ROOT, "main", "assets"),
                         "-I", os.path.join(ROOT, "main", "art"),
                         "-I", os.path.join(ROOT, "assets"),
                         "-I", shim, "-o", binp, hac],
                        capture_output=True, text=True)
    if cc.returncode:
        raise SystemExit("harness build failed:" + NL + cc.stderr[-3000:])
    return binp


# ---- the vectors -------------------------------------------------------------
# A case is (name, code_size, wasm_mem_max, expect_load, expect_cap, expect_why).
# expect_why is the exact refusal sentence (hex on the wire) or "" for a load.
# ORB_WASM_MEM_MAX is the shipped ceiling, restated here as the number vectors push
# past — the suite and the device must refuse the SAME word.
MEM_MAX = 128 * 1024
NO_CODE = "the app names the wasm tier but carries no code \u2014 re-export it".encode().hex()
TOO_MUCH = "the app asks for more memory than this dial can give it".encode().hex()


def vecs_plan():
    return [
        # Honest module: code present, positive bound at or under the ceiling.
        ("load 64 KB under the ceiling",   4096, 65536,     True,  65536,      ""),
        ("load the exact ceiling",         4096, MEM_MAX,   True,  MEM_MAX,    ""),
        ("load the smallest bound",           1,     1,     True,      1,      ""),
        ("load one under the ceiling",        1, MEM_MAX-1, True, MEM_MAX-1,   ""),
        # Zero code: the code gate fires first, before the memory gate is read.
        ("refuse empty code",                 0,  1024,    False,     0,  NO_CODE),
        ("refuse empty code, zero bound",     0,     0,    False,     0,  NO_CODE),
        # Zero bound allocates nothing; D27 consent must be positive.
        ("refuse zero bound",              1024,     0,    False,     0, TOO_MUCH),
        # Over the ceiling: exactly one past, and far past.
        ("refuse one over the ceiling",    1024, MEM_MAX+1, False, 0, TOO_MUCH),
        ("refuse 4 GiB over",              1024, 0xFFFFFFFF, False, 0, TOO_MUCH),
        # Code refusal outranks memory: with neither, the CODE sentence prints. A
        # reordering that lifted the memory check first would print TOO_MUCH here.
        ("code refusal outranks memory",      0, MEM_MAX+1, False, 0, NO_CODE),
    ]


def pack_plan(cases):
    out = bytearray(struct.pack("<I", len(cases)))
    for (_n, code, mem, _l, _c, _w) in cases:
        out += struct.pack("<II", code, mem)
    return bytes(out)


def run_plan(binp, cases, cases_file):
    open(cases_file, "wb").write(pack_plan(cases))
    r = subprocess.run([binp, "plan", cases_file], capture_output=True, text=True)
    if r.returncode:
        raise SystemExit("plan harness rc=%d\n%s%s" % (r.returncode, r.stdout, r.stderr))
    lines = r.stdout.strip().split("\n")
    assert len(lines) == len(cases), "harness printed %d lines, expected %d" % (
        len(lines), len(cases))
    fails = []
    for (name, _c, _m, want_load, want_cap, want_why), line in zip(cases, lines):
        _i, got_load, got_cap, got_why = line.split(" ", 3)
        ok = (int(got_load) == int(bool(want_load))
              and int(got_cap) == (want_cap if want_load else 0)
              and got_why == want_why)
        if not ok:
            fails.append("  %s: got load=%s cap=%s why=%s ; want load=%d cap=%d why=%s" % (
                name, got_load, got_cap, got_why,
                int(want_load), want_cap if want_load else 0, want_why or "(empty)"))
    return fails


# The vocabulary the device half and the guest both depend on: (index, required, name).
# Only orb_tick is required; the rest are optional.
EXPECT_ENTRIES = [
    (0, 0, "orb_activate"),
    (1, 1, "orb_tick"),
    (2, 0, "orb_suspend"),
    (3, 0, "orb_resume"),
    (4, 0, "orb_input"),
    (5, 0, "orb_service"),
]


def run_entries(binp):
    r = subprocess.run([binp, "entries", "-"], capture_output=True, text=True)
    if r.returncode:
        raise SystemExit("entries harness rc=%d\n%s%s" % (r.returncode, r.stdout, r.stderr))
    rows = []
    for line in r.stdout.strip().split("\n"):
        i, req, name = line.split(" ", 2)
        rows.append((int(i), int(req), name))
    if rows != EXPECT_ENTRIES:
        return ["  s_entries drifted from the pinned vocabulary:\n    got  %r\n    want %r"
                % (rows, EXPECT_ENTRIES)]
    return []


# ---- the device-half lint ----------------------------------------------------
# The m3 calls cannot run off the board, so their ORDER — the load facts D27 depends
# on — is pinned by a structural read of the shipped orb_app_wasm.c text. Reordering
# any of these is a failure here, not a crash on the dial.
def lint(text):
    fails = []

    def pos(pat, label):
        m = re.search(pat, text, re.S)
        if not m:
            fails.append("  device lint: %s is gone from orb_app_wasm.c" % label)
            return -1
        return m.start()

    def order(a, b, whya, whyb, why):
        pa, pb = pos(a, whya), pos(b, whyb)
        if pa >= 0 and pb >= 0 and not (pa < pb):
            fails.append("  device lint: %s must come before %s (%s)" % (whya, whyb, why))

    # D27: arm the memory ceiling before LoadModule instantiates memory.
    order(r"m3_SetResourceLimit\([^;]*c_m3Limit_MemoryBytes[^;]*\)",
          r"m3_LoadModule\s*\(",
          "SetResourceLimit(MemoryBytes)", "LoadModule",
          "arm the ceiling before the module can allocate past the owner's consent")
    # Imports bind before the CALL to Load, because Load resolves them.
    order(r"=\s*bind_imports\s*\(", r"m3_LoadModule\s*\(",
          "bind_imports", "LoadModule", "imports bind before the module loads")
    # The image is copied to owned PSRAM (m3 keeps a pointer) before the parse.
    order(r"memcpy\s*\(\s*s->code", r"m3_ParseModule\s*\(",
          "PSRAM image copy", "ParseModule",
          "the code bytes the runtime keeps outlive the caller's buffer")
    # A trap bills C2: the trap path calls orb_svc_note_fault with the built reason.
    if not re.search(r"orb_svc_note_fault\s*\(\s*s->app\s*,\s*why\s*\)", text):
        fails.append("  device lint: the trap path does not bill orb_svc_note_fault — "
                     "D27's trapping-module rule is unbuilt")
    order(r"trapped in", r"orb_svc_note_fault\s*\(",
          "trap report", "orb_svc_note_fault",
          "the fault is billed where the trap is reported, in one place")
    # The strike cap is honoured live: release happens only once the broker says the
    # row is faulted, so a single trap keeps the app running and two end it.
    order(r"orb_svc_faulted\s*\(", r"orb_wasm_unload\s*\(",
          "orb_svc_faulted check", "orb_wasm_unload",
          "the engine is released only at the second strike, not the first")
    return fails


# ---- the P7.4 seam lint ------------------------------------------------------
# orb_wasm_load has no caller a board could forget: the load rung in
# orb_app_install.c is the engine's ONLY door besides the enable door, and both
# must verify before handing bytes over and bind only after the engine takes them.
# main.c's rung ORDER is pinned too: applying code before the enable words would
# load disabled apps at full D27 cost, and after init_all would build a dial where
# every code app failed its init.
def lint_seam(inst, mn):
    fails = []

    def pos(pat, label, text):
        m = re.search(pat, text, re.S)
        if not m:
            fails.append("  seam lint: %s is gone" % label)
            return -1
        return m.start()

    def order(a, b, whya, whyb, why, text):
        pa, pb = pos(a, whya, text), pos(b, whyb, text)
        if pa >= 0 and pb >= 0 and not (pa < pb):
            fails.append("  seam lint: %s must come before %s (%s)" % (whya, whyb, why))

    # The read-back CRC guards the engine: the compare sits in the reject guard
    # that gates the load, not merely somewhere before it.
    if not re.search(r"if \(e == ESP_OK && esp_rom_crc32_le\(0, buf, "
                     r"h\.code_size\) != h\.code_crc\)[\s\S]{0,200}?orb_wasm_load\(slot", inst):
        fails.append("  seam lint: the code read-back CRC does not guard the engine — "
                     "orb_wasm_load must run only after the compare rejects a rotted slot")
    # The row is bound only once the engine took the module.
    order(r"orb_wasm_load\(slot", r"orb_wasm_bind_app\(slot",
          "orb_wasm_load", "orb_wasm_bind_app",
          "the row pointer only ever names a live instance", inst)
    # The boot rung skips C2-quarantined rows: a failed app must not hold its memory.
    order(r"orb_svc_faulted\(row\)", r"wasm_row_load\(p,",
          "orb_svc_faulted skip", "wasm_row_load",
          "quarantined code apps stay cold", inst)
    # The enable door releases a stale slot before loading the new occupant.
    order(r"orb_wasm_unload\(row->flash_slot\)", r"wasm_row_load\(cp,",
          "stale-slot orb_wasm_unload", "enable-door wasm_row_load",
          "a slot reused after an uninstall never keeps its previous module", inst)
    # main.c's ladder: enable words, then code, then init.
    order(r"orb_app_install_apply_enable\s*\(\s*\)\s*;",
          r"orb_app_install_apply_code\s*\(\s*\)\s*;",
          "apply_enable", "apply_code",
          "disabled apps never pay their D27 memory at boot", mn)
    order(r"orb_app_install_apply_code\s*\(\s*\)\s*;",
          r"orb_app_init_all\s*\(\s*\)",
          "apply_code", "orb_app_init_all",
          "the wasm init answers OK only for a module this rung already loaded", mn)
    return fails


# ---- mutations ---------------------------------------------------------------
# (name, kind, rewrite). kind picks which lifted text is rewritten; each must turn at
# least one vector (plan) or table row (entries) red. An uncaught mutant means the
# vectors do not pin what they claim.
def _sub(old, new):
    def fn(t):
        if old not in t:
            raise SystemExit("mutation anchor absent — code moved: %r" % old[:60])
        return t.replace(old, new, 1)
    return fn


def _rex(pat, repl):
    def fn(t):
        out, n = re.subn(pat, repl, t, count=1)
        if n != 1:
            raise SystemExit("mutation regex matched nothing — code moved: %r" % pat)
        return out
    return fn


MUTATIONS = [
    # plan: drop the zero-code guard — the "carries no code" vectors would load.
    ("drop zero-code guard", "plan", _sub("if (h->code_size == 0) {", "if (0) {")),
    # plan: accept a zero bound — the module could allocate nothing and never runs.
    ("accept zero bound", "plan", _sub("h->wasm_mem_max == 0 ||", "0 ||")),
    # plan: read the ceiling as unlimited — the over-ceiling vectors would load.
    ("ignore the ceiling", "plan", _sub("h->wasm_mem_max > ORB_WASM_MEM_MAX", "0")),
    # plan: pass a bogus cap — a legal load now reports the wrong bound.
    ("wrong cap", "plan", _sub("p.mem_cap = h->wasm_mem_max;", "p.mem_cap = 1;")),
    # entries: tick stops being required — a module with no tick would load.
    ("tick not required", "entries", _rex(r'("orb_tick",\s*)true', r'\1false')),
    # entries: an export is renamed — the guest contract drifts silently.
    ("rename tick", "entries", _rex(r'"orb_tick"', '"orb_frame"')),
    # entries: a second required export appears — an optional hook turns mandatory.
    ("activate required", "entries", _rex(r'("orb_activate",\s*)false', r'\1true')),
    # lint: the trap path stops billing C2 (D27's trapping-module rule goes unbuilt).
    ("drop note_fault", "lint", _sub("orb_svc_note_fault(s->app, why);", ";")),
    # lint: the engine is released on the FIRST trap, not at the second strike.
    ("unload without the faulted gate", "lint",
     _sub("if (s->app && orb_svc_faulted(s->app)) {", "if (s->app) {")),
    # lint: the fault is billed before the trap is reported (the seam moves, the
    # report no longer proves a bill happened at it).
    ("bill before report", "lint",
     _sub("snprintf(why, sizeof why, \"wasm trapped in %s\", s_entries[entry].export_name);",
          "orb_svc_note_fault(s->app, why); snprintf(why, sizeof why, \"wasm %s\", "
          "s_entries[entry].export_name);")),
    # seam: the read-back becomes a size nod instead of a CRC — a rotted slot would
    # reach the engine, the exact thing verify_slot_geom refuses for geometry.
    ("drop code read-back CRC", "seam",
     _sub("if (e == ESP_OK && esp_rom_crc32_le(0, buf, h.code_size) != h.code_crc) {",
          "if (e == ESP_OK && h.code_size != 0) {")),
    # seam: the row is bound before the engine has taken the module.
    ("bind before load", "seam",
     _rex(r"(e = orb_wasm_load\(slot, &h, buf, h\.code_size\);\s*\n\s*if \(e == ESP_OK\) )"
          r"(orb_wasm_bind_app\(slot, row\);)", r"\2\n        \1")),
    # seam: the boot rung loads a C2-quarantined code app — its memory held hostage.
    ("load despite fault words", "seam",
     _sub("if (orb_svc_faulted(row)) continue;", "/* no quarantine check */")),
    # seam: the enable door keeps a previous occupant's engine state on the slot.
    ("keep stale slot", "seam",
     _sub("orb_wasm_unload(row->flash_slot);", "/* stale state kept */")),
    # seam_main: the boot ladder skips the rung — enabled code apps never load.
    ("no code rung at boot", "seam_main",
     _sub("orb_app_install_apply_code();", "/* rung skipped */")),
    # seam_main: the rung runs BEFORE the enable words — every installed code app
    # pays its D27 memory, even the ones the owner turned off.
    ("rung before enable words", "seam_main",
     _rex(r"(orb_app_install_apply_enable\(\);)",
          r"orb_app_install_apply_code();\n    \1")),
]


def run_mutations(tmp):
    caught, missed = 0, []
    for name, kind, fn in MUTATIONS:
        if kind in ("lint", "seam", "seam_main"):
            # A shipped-text rewrite: its lint must go red on the mutant. An
            # absent anchor is a MISS, not a catch: the code moved and the
            # mutant silently stopped mutating anything.
            try:
                if kind == "lint":
                    red = bool(lint(fn(src)))
                else:
                    red = bool(lint_seam(
                        fn(read(INSTALL_C)) if kind == "seam" else read(INSTALL_C),
                        fn(read(MAIN_C)) if kind == "seam_main" else read(MAIN_C)))
            except SystemExit:
                red = False
                missed.append(name + " (anchor moved)")
            if red:
                caught += 1
            elif not missed or not missed[-1].startswith(name + " ("):
                missed.append(name)
            continue
        try:
            binp = compile_harness(tmp, mutate=(kind, fn))
        except SystemExit:
            caught += 1   # a mutation that stops the harness compiling is still caught
            continue
        fails = (run_plan(binp, vecs_plan(), os.path.join(tmp, "v.bin"))
                 if kind == "plan" else run_entries(binp))
        if fails:
            caught += 1
        else:
            missed.append(name)
    return caught, missed


def main():
    mutate = "--mutations" in sys.argv
    fails = []
    with tempfile.TemporaryDirectory() as tmp:
        binp = compile_harness(tmp)
        fails += run_plan(binp, vecs_plan(), os.path.join(tmp, "v.bin"))
        fails += run_entries(binp)
        fails += lint(read(WASM_C))
        fails += lint_seam(read(INSTALL_C), read(MAIN_C))
        app_c = read(os.path.join(ROOT, "main", "app", "orb_app.c"))
        if not re.search(r"\{\s*ORB_BEHAVIOUR_WASM\s*,\s*&orb_wasm_iface\s*\}", app_c):
            fails.append("  device lint: orb_app.c does not register the wasm module")
        if mutate:
            caught, missed = run_mutations(tmp)
            print("%d/%d mutants caught" % (caught, len(MUTATIONS)))
            for m in missed:
                fails.append("  UNCAUGHT MUTANT: %s" % m)
    if fails:
        for f in fails:
            print("FAIL " + f)
        sys.exit(1)
    tail = ""
    if mutate:
        tail = ", all %d mutants caught" % len(MUTATIONS)
    print("all host checks pass (%d plan vectors, %d table rows, %d lint rules%s)" % (
        len(vecs_plan()), len(EXPECT_ENTRIES), 7 + 6, tail))


if __name__ == "__main__":
    main()
