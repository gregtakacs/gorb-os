#!/usr/bin/env python3
"""host_orbapp_install_test.py — the sentinel for the app installer (P4).

Two halves, mirroring the two files it watches.

THE PURE DECISION. install_decide() in main/app/orb_app_install.c is the cap,
the quota, the slot and the serial over a header the reader already accepted and
a slot state the caller read under the lock. It is PURE — no partition, no fd,
no NVS — so this suite LIFTS it verbatim (dying by name if it moves, the way
host_orbapp_scan_test.py lifts row_decide), recompiles it with host gcc -Werror
against the SHIPPED orbapp_hdr_t / orb_setting_desc_t, and drives it with headers
whose geometry/art/settings fields it sets directly — the art field since P4b
(D21) filled with JUNK the decision must not read, pinning the shedding by
demanding the same verdict as if it were zero. install_decide never calls
the reader, so building its input struct field-by-field is the honest vector, not
a shortcut.

THE LADDER ORDER. The device half cannot run off the board, so its ordering —
the whole point of D9 — is pinned by a STRUCTURAL lint over the shipped
orb_app_install.c text: the payload checksums before the first erase, the pure
decision runs before the erase, the geometry CRCs back FROM the partition before
the header goes live, the header is the LAST flash write, and the caps reach the
resident row from the manifest header and not from a scan row or a stored grant
word (D19). A mutation that reorders any of those turns the lint red here, not on
the bench.

Run:      python3 tools/host_orbapp_install_test.py
Mutants:  python3 tools/host_orbapp_install_test.py --mutations
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
INSTALL_C = os.path.join(ROOT, "main", "app", "orb_app_install.c")
INSTALL_H = os.path.join(ROOT, "main", "app", "orb_app_install.h")
MANIFEST_H = os.path.join(ROOT, "main", "app", "orb_app_manifest.h")

# The quota the device prices against, and a tiny one that lets a legal table
# trip the gate so its refusal path is a working store, not an unfired blank.
QUOTA_DEFAULT = 2048
QUOTA_TINY = 48


def read(p):
    if not os.path.exists(p):
        sys.exit("missing " + p + " — the lifts below have no fallback")
    return open(p, encoding="utf-8").read()


def lift(text, pattern, where):
    m = re.search(pattern, text, re.S)
    if not m:
        sys.exit("could not lift %r from %s — did the code move? Dying here is "
                 "the point: an empty lift would keep this suite green while "
                 "testing nothing." % (pattern[:60], where))
    return m.group(0)


inst_src = read(INSTALL_C)
inst_hdr = read(INSTALL_H)

# The lifts. install_decide ends at the first column-0 brace; every closing brace
# inside it is indented, so the anchor cannot stop early. The two typedefs and the
# APPS_* macro block come from the header so the struct layout and the caps are the
# SHIPPED numbers, not a re-typed copy that could drift from the device.
f_decide = lift(inst_src, r"bool install_decide\(const orbapp_hdr_t \*h[\s\S]*?\n\}", INSTALL_C)
f_slots = lift(inst_hdr, r"typedef struct \{[^}]*\} orbapp_install_slots_t;", INSTALL_H)
f_verdict = lift(inst_hdr, r"typedef struct \{[^}]*\} orbapp_install_verdict_t;", INSTALL_H)
f_appcap = lift(inst_hdr, r"#define APPS_PARTITION_NAME[\s\S]*?#define APPS_N_SLOTS[^\n]*\n", INSTALL_H)

f_worst = lift(inst_src, r"#define APPS_STR_WORST[\s\S]*?#define APPS_NUM_WORST[^\n]*\n", INSTALL_C)

# install_decide uses snprintf only — no strlcpy, no I/O — so it lifts clean with
# just the header structs and one quota define in scope.

# ---- the harness -------------------------------------------------------------
# Case file (little endian): u32 n_cases, then per case
#   u32 geom_off, geom_size, art_size,
#   u32 str_count, n_settings,          (first str_count settings are STRING rows)
#   i32 existing_slot, u32 installed_count, u32 used_mask,
#   u32 serial[8]
# stdout per case: "idx accept slot reinstall serial cfg_worst <hex why>"
HARNESS_C = r"""
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "orb_app_manifest.h"   /* orbapp_hdr_t, orb_setting_desc_t, caps */

#define ORB_APP_CAP 8
@APPS@
@WORST@
@SLOTS@
@VERDICT@
@DECIDE@

static FILE *F;
static uint32_t rd32(void) { uint32_t v; if (fread(&v, 4, 1, F) != 1) exit(2); return v; }
static int32_t  rdi(void)  { return (int32_t)rd32(); }
static void puthex0(const char *s)
{
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) printf("%02x", *p);
}

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    F = fopen(argv[1], "rb");
    if (!F) return 2;
    const uint32_t n = rd32();
    for (uint32_t i = 0; i < n; i++) {
        orbapp_hdr_t h; memset(&h, 0, sizeof h);
        h.geom_off = rd32();
        h.geom_size = rd32();
        h.art_size = rd32();
        const uint32_t strc = rd32();
        const uint32_t nset = rd32();
        const int32_t existing = rdi();
        const uint32_t cnt = rd32();
        const uint32_t mask = rd32();
        uint32_t serial[8];
        for (int k = 0; k < 8; k++) serial[k] = rd32();
        /* P7: the code region rides the case stream's tail, so every older vector
         * (which carries none) reads zeros and the decision prices geometry alone,
         * exactly as it did pre-P7. */
        h.code_off     = rd32();
        h.code_size    = rd32();
        h.wasm_mem_max = rd32();

        static orb_setting_desc_t set[ORBAPP_SETTINGS_MAX];
        const uint32_t use = nset < ORBAPP_SETTINGS_MAX ? nset : ORBAPP_SETTINGS_MAX;
        for (uint32_t k = 0; k < use; k++)
            set[k].type = (orb_set_type_t)((k < strc) ? ORB_SET_STRING : ORB_SET_INT);

        orbapp_install_slots_t sl; memset(&sl, 0, sizeof sl);
        for (int k = 0; k < APPS_N_SLOTS; k++) {
            sl.slot_used[k] = (mask >> k) & 1u;
            sl.slot_serial[k] = serial[k];
        }
        sl.existing_slot = existing;
        sl.installed_count = (int)cnt;
        sl.settings = set;
        sl.n_settings = (int)nset;

        orbapp_install_verdict_t v;
        install_decide(&h, &sl, &v);
        printf("%u %d %d %d %u %u ", i, (int)v.accept, v.slot, (int)v.reinstall,
               v.serial, v.cfg_worst);
        puthex0(v.why);
        printf("\n");
    }
    fclose(F);
    return 0;
}
"""


def compile_harness(tmp, quota, mutate=None):
    """Build the lifted decision against the shipped headers. `quota` overrides
    ORB_CFG_QUOTA_BYTES so a legal table can be driven over the gate; `mutate`
    rewrites the lifted install_decide text (each mutation must turn a vector
    red — an uncaught mutant means the vectors do not pin the gate they claim)."""
    shim = os.path.join(tmp, "shim")
    os.makedirs(os.path.join(shim, "mbedtls"), exist_ok=True)
    open(os.path.join(shim, "esp_err.h"), "w").write(SHIM_ESP_ERR)
    open(os.path.join(shim, "mbedtls", "sha256.h"), "w").write("#pragma once\n")
    # materialize the shipped manifest header so `#include "orb_app_manifest.h"`
    # resolves from tmp; its own `#include "app/orb_svc.h"` resolves via -I ROOT/main.
    open(os.path.join(tmp, "orb_app_manifest.h"), "w").write(read(MANIFEST_H))
    decide = f_decide
    if mutate:
        decide = mutate[1](decide)
    harness = (HARNESS_C
               .replace("@APPS@", f_appcap)
               .replace("@WORST@", f_worst)
               .replace("@SLOTS@", f_slots)
               .replace("@VERDICT@", f_verdict)
               .replace("@DECIDE@", decide))
    hac = os.path.join(tmp, "harness.c")
    open(hac, "w").write(harness)
    binp = os.path.join(tmp, "harness_%d" % quota)
    cc = subprocess.run(["gcc", "-std=gnu11", "-O1", "-Wall", "-Wextra",
                         "-Wno-unused-function", "-Werror",
                         "-DORB_CFG_QUOTA_BYTES=%d" % quota,
                         # P5b: orb_app_manifest.h includes orb_assets.h for the class
                         # vocabulary an asset row's first word carries.
                         "-I", tmp, "-I", os.path.join(ROOT, "main"),
                         "-I", os.path.join(ROOT, "main", "assets"),
"-I", os.path.join(ROOT, "main", "art"),
                         "-I", os.path.join(ROOT, "assets"),
                         "-I", shim, "-o", binp, hac],
                        capture_output=True, text=True)
    if cc.returncode:
        raise SystemExit("harness build failed:" + NL + cc.stderr[-3000:])
    return binp


SHIM_ESP_ERR = """#pragma once
typedef int esp_err_t;
#define ESP_OK 0
"""
NL = "\n"

# The caps the reference mirrors. These are the SHIPPED values; the lint below
# re-derives them from the source text and fails if a constant here and the device
# ever disagree — a re-typed number that quietly drifts is the whole reason the
# decision is lifted instead of reimplemented.
GEOM_CAP = 0x00040000          # ORBAPP_GEOM_BYTES_MAX == APPS_SLOT_GEOM_CAP
WASM_MEM_MAX = 0x00020000      # ORBAPP_WASM_MEM_MAX — D27's linear-memory ceiling
ART_JUNK = 0x003D0001          # over the OLD slot cap; install_decide must not
                               # read the art word at all since D21 (P4b) — the
                               # vectors fill it with an impossible claim and
                               # demand the same verdict as if it were zero
N_SLOTS = 8                    # APPS_N_SLOTS == ORB_APP_CAP
STR_WORST = 32                 # sizeof orb_setting_desc_t.def_str
NUM_WORST = 4                  # one nvs_set_u32
SETTINGS_MAX = 16              # ORBAPP_SETTINGS_MAX

# A refusal is checked by the fragment of its sentence that names WHICH gate fired
# (the wording itself belongs to the reader mirror, not to this decision).
WHY_KEY = {"geom": "geometry and code", "quota": "setting(s) need",
           "cap": "already holds", "range": "out of range",
           "mem": "wasm memory bound"}


def ref(geom_off, geom_size, art_size, strc, nset, existing, cnt, mask, serial, quota,
        code_off=0, code_size=0, wasm_mem=0):
    """The independent model of install_decide's ladder — the intent. The device
    runs the LIFTED code; the two must agree, and it is exactly their disagreement
    that is the bug this suite hunts.

    P7 adds the code region: the copied slot prefix is geom_end or code_end,
    whichever is larger (a native app has code_size 0 and so geom_end, unchanged),
    and gate 1b refuses a linear-memory bound past the D27 ceiling. Both default to
    the no-code shape, so every pre-P7 vector's expected verdict is untouched."""
    blob = geom_off + geom_size
    if code_size and (code_off + code_size) > blob:
        blob = code_off + code_size
    if blob > GEOM_CAP:
        return dict(accept=0, slot=-1, reinstall=0, serial=0, cfg_worst=0, key="geom")
    # Gate 1b — D27's memory ceiling, priced off the board's real budget before the
    # erase. The reader refuses past ORBAPP_WASM_MEM_MAX upstream; this is the
    # defensive twin, and a vector that trips only the ceiling (with a blob that
    # still fits) is what proves install_decide reads the word at all.
    if wasm_mem > WASM_MEM_MAX:
        return dict(accept=0, slot=-1, reinstall=0, serial=0, cfg_worst=0, key="mem")
    # D21 (P4b): no art gate lives here. The installer reads no art word; the
    # vectors still carry one, filled with junk, and the model ignores it — the
    # pair of omissions is what pins "the decision prices geometry alone".
    n = min(nset, SETTINGS_MAX)
    worst = strc * STR_WORST + (n - strc) * NUM_WORST
    if worst > quota:
        return dict(accept=0, slot=-1, reinstall=0, serial=0, cfg_worst=worst, key="quota")
    if existing >= 0:
        if existing >= N_SLOTS:
            return dict(accept=0, slot=-1, reinstall=0, serial=0, cfg_worst=worst, key="range")
        return dict(accept=1, slot=existing, reinstall=1, serial=serial[existing],
                    cfg_worst=worst, key=None)
    if cnt >= N_SLOTS:
        return dict(accept=0, slot=-1, reinstall=0, serial=0, cfg_worst=worst, key="cap")
    free = -1
    for i in range(N_SLOTS):
        if not (mask >> i) & 1:
            free = i
            break
    if free < 0:
        return dict(accept=0, slot=-1, reinstall=0, serial=0, cfg_worst=worst, key="cap")
    return dict(accept=1, slot=free, reinstall=0, serial=(serial[free] + 1) & 0xFFFFFFFF,
                cfg_worst=worst, key=None)


# ---- vectors -----------------------------------------------------------------
# (name, geom_off, geom_size, art_size, strc, nset, existing, cnt, mask, serials).
# serials is a length-8 list. Refusals are keyed to their gate via ref().
def V():
    S0 = [0] * 8
    g = []
    a = g.append
    a(("accept_new_empty", 152, 1024, 0, 0, 0, -1, 0, 0b0, S0))
    a(("accept_exact_cap", 0, GEOM_CAP, 0, 0, 0, -1, 0, 0b0, S0))
    a(("refuse_geom_over", 152, GEOM_CAP, 0, 0, 0, -1, 0, 0b0, S0))
    a(("geom_before_all", 0, GEOM_CAP + 1, ART_JUNK, 0, 0, -1, 9, 0xFF, S0))
    # D21 (P4b): a header claiming a flagship bank reaches the decision with a
    # junk art word — the reader refuses that file upstream, and if a vector
    # shows the decision ANY opinion about the word, an installer regrew one.
    a(("art_junk_ignored", 0, 1024, ART_JUNK, 0, 0, -1, 0, 0b0, S0))
    a(("art_junk_still_capped", 0, 1024, ART_JUNK, 0, 0, -1, 8, 0b11111110, S0))
    a(("accept_reuse", 0, 1024, 0, 0, 0, 3, 3, 0b1111, [0, 0, 0, 7] + [0] * 4))
    a(("reuse_out_of_range", 0, 1024, 0, 0, 0, 8, 1, 0b0, S0))
    a(("accept_new_serial_stale", 0, 1024, 0, 0, 0, -1, 1, 0b1, [9] + S0[1:]))
    a(("cap_8_free_but_count", 0, 1024, 0, 0, 0, -1, 8, 0b11111110, S0))
    a(("cap_all_used", 0, 1024, 0, 0, 0, -1, 7, 0xFF, S0))
    a(("accept_7", 0, 1024, 0, 0, 0, -1, 7, 0b01111111, S0))
    a(("mixed_quota_accept", 0, 1024, 0, 2, 4, -1, 0, 0b0, S0))
    a(("legal_table_never_trips", 0, 1024, 0, 16, 16, -1, 0, 0b0, S0))
    # ---- P7: the code region and D27's ceiling ----------------------------------
    # code_off/code_size/wasm_mem ride as three extra trailing fields (see the
    # tuple unpack below). A code region inside a slot's slack is ACCEPTED — the
    # module is opaque bytes, so nothing here prices the geometry's counts against
    # it, only the slot the whole prefix must fit. The code-bearing vectors each
    # carry a legal (in-ceiling) memory bound EXCEPT the one named for the gate.
    a(("accept_code_small", 0, 1024, 0, 0, 0, -1, 0, 0b0, S0, 4096, 1024, 65536))
    # The code region past the slot the geom_end left room for: geom is a hair
    # under cap, code rides past it — the max(geom_end, code_end) rule.
    a(("refuse_code_over_slot", 0, GEOM_CAP - 512, 0, 0, 0, -1, 0, 0b0, S0,
       GEOM_CAP - 512, 4096, 65536))
    # D27's ceiling alone: a small blob that fits, but a bound past the D27 cap —
    # the exact refusal the P7 pass line names, tripping gate 1b and not gate 1.
    a(("refuse_mem_over_ceiling", 0, 1024, 0, 0, 0, -1, 0, 0b0, S0,
       4096, 1024, WASM_MEM_MAX + 1))
    return g


# Quota refusals run against the tiny build so a legal table trips the gate.
def V_TINY():
    S0 = [0] * 8
    g = []
    a = g.append
    a(("t_12int_edge", 0, 1024, 0, 0, 12, -1, 0, 0b0, S0))
    a(("t_13int_over", 0, 1024, 0, 0, 13, -1, 0, 0b0, S0))
    a(("t_1str_ok", 0, 1024, 0, 1, 1, -1, 0, 0b0, S0))
    a(("t_2str_over", 0, 1024, 0, 2, 2, -1, 0, 0b0, S0))
    return g


def pack_case(c):
    (name, go, gs, art, strc, nset, existing, cnt, mask, serials) = c[:10]
    # P7: the code triple is an optional tail. Older vectors carry none and pack as
    # three zeros, which the harness fills into the header so the decision prices the
    # geometry alone — the pre-P7 shape, byte for byte.
    code_off, code_size, wasm = (c[10], c[11], c[12]) if len(c) > 10 else (0, 0, 0)
    return struct.pack("<11I8I", go, gs, art, strc, nset,
                       existing & 0xFFFFFFFF, cnt, mask, *serials,
                       code_off, code_size, wasm)


def run_vectors(cases, quota, binp, cases_file):
    with open(cases_file, "wb") as f:
        f.write(struct.pack("<I", len(cases)))
        for c in cases:
            f.write(pack_case(c))
    out = subprocess.run([binp, cases_file], capture_output=True, text=True)
    if out.returncode:
        raise SystemExit("harness run rc=%d\n%s%s" % (out.returncode, out.stdout, out.stderr))
    lines = [ln for ln in out.stdout.split("\n") if ln]
    assert len(lines) == len(cases), \
        "harness printed %d lines, expected %d" % (len(lines), len(cases))
    bad = []
    for c, line in zip(cases, lines):
        exp = ref(c[1], c[2], c[3], c[4], c[5], c[6], c[7], c[8], c[9], quota,
                  *(c[10:13] if len(c) > 10 else (0, 0, 0)))
        p = line.split(" ")
        got = dict(accept=int(p[1]), slot=int(p[2]), reinstall=int(p[3]),
                   serial=int(p[4]), cfg_worst=int(p[5]),
                   why=bytes.fromhex(p[6]).decode("latin-1") if p[6] else "")
        ok = (got["accept"] == exp["accept"] and got["slot"] == exp["slot"] and
              got["reinstall"] == exp["reinstall"] and got["serial"] == exp["serial"] and
              got["cfg_worst"] == exp["cfg_worst"])
        if exp["key"] is None:
            ok = ok and got["why"] == ""
        else:
            ok = ok and WHY_KEY[exp["key"]] in got["why"]
        if not ok:
            bad.append((c[0], exp, got))
    return bad


def run_pure(mutate=None):
    fails = 0
    with tempfile.TemporaryDirectory() as tmp:
        b_def = compile_harness(tmp, QUOTA_DEFAULT, mutate)
        b_tiny = compile_harness(tmp, QUOTA_TINY, mutate)
        for label, cases, quota, binp in (
                ("default", V(), QUOTA_DEFAULT, b_def),
                ("tiny-quota", V_TINY(), QUOTA_TINY, b_tiny)):
            bad = run_vectors(cases, quota, binp, os.path.join(tmp, "cases_%s.bin" % label))
            if bad:
                fails += len(bad)
                for name, exp, got in bad:
                    print("FAIL [%s] %s\n  want %s\n  got  %s" % (label, name, exp, got))
            else:
                print("ok    %-11s %2d vectors match the model" % (label + ":", len(cases)))
    return fails


# ---- structural lint over the device ladder ----------------------------------
# The ladder's ORDER is the whole point of D9 and lives only on the board, so the
# only way to pin it off-device is to read the shipped text and assert the calls
# appear in the enforced sequence. These are the same invariants a power-cut bench
# test checks physically; catching them here means a reordering never reaches a
# board to be discovered as a slot that went live on unread bytes.
def lint(src):
    bad = []

    # The order checks look only inside orb_app_install()'s body — the ladder where
    # the calls happen. Searching the whole file would match each helper's own
    # DEFINITION (verify_slot_geom(, write_slot_header() far earlier than its call,
    # inverting every ordering this suite exists to enforce.
    # D35: the ladder moved from orb_app_install()'s frame into install_ladder(),
    # which BOTH sources call. The order invariants are the same words; only the
    # function that holds them moved — and the fd arguments became one src.
    lo = src.find("static esp_err_t install_ladder(")
    hi = src.find("esp_err_t orb_app_install(const char *stem")
    if lo < 0 or hi < 0:
        return ["could not locate install_ladder()'s body — the shared ladder is gone"]
    ladder = src[lo:hi]

    def order(a, b, why):
        ia, ib = ladder.find(a), ladder.find(b)
        if ia < 0 or ib < 0 or ia > ib:
            bad.append("order: %r must precede %r — %s" % (a, b, why))

    # 1. the whole payload checksums off the card, and the pure decision runs,
    #    BEFORE the first erase (the erase lives inside copy_prefix).
    order("verify_payload(src, h)", "copy_prefix(p, src, h",
          "the payload must be CRC-verified off the card before anything erases")
    order("install_decide(h, &sl, &v)", "copy_prefix(p, src, h",
          "cap/quota/slot/serial decided before the erase, so a refusal costs no slot")
    order("read_slots(p, h->slug, &sl)", "install_decide(h, &sl, &v)",
          "the slot state is read under the lock before the pure decision prices it")
    # 2. geometry read back FROM the partition, header written LAST.
    order("copy_prefix(p, src, h", "verify_slot_geom(",
          "geometry must be copied before it is read back and CRCed from flash")
    order("verify_slot_geom(", "write_slot_header(",
          "the geometry must read back intact before the header makes the slot live (D9)")
    # 3. D19: caps reach the resident row only from the manifest header.
    for tag in ("row.caps       = h->caps;", "row.caps       = h.caps;"):
        if tag not in src:
            bad.append("D19: %r missing — caps must come from the manifest header, "
                       "never a scan row or a stored grant word" % tag)
    # 3b. D1/C6: behaviour_kind reaches the resident row only from the manifest
    #     header, at BOTH upsert sites (install and boot-resolve). It is the word
    #     that selects the compiled-in module whose iface makes a card app
    #     walkable; sourced from anywhere else, a below-ABI package could name an
    #     iface the firmware never intended to run. The two sites align their
    #     assignment differently (h-> vs h.), so match the field name itself.
    for tag in ("row.behaviour_kind = h->behaviour_kind;", "row.behaviour_kind = h.behaviour_kind;"):
        if tag not in src:
            bad.append("D1/C6: %r missing — behaviour_kind must come from the manifest "
                       "header at both upsert sites, or the resolver drops the dispatch word" % tag)
    if src.count("row.behaviour_kind = h->behaviour_kind;") != 1 or \
       src.count("row.behaviour_kind = h.behaviour_kind;") != 1:
        bad.append("D1/C6: behaviour_kind must be assigned exactly once per upsert site")
    # 3c. THE STACK BUDGET (hardware-found 2026-10-13, orb-A). orb_jobs runs this
    #     ladder on a hard-measured 4096 B stack (orb_jobs.c:65: "needs more stack
    #     is a crash rather than a log line"), and settings[] + sw[] are ~1 KB each.
    #     With them as frame locals the install overflowed the worker on its
    #     first-ever bench — the same bug boot_resolve() documents for the 3584 B
    #     main task. Both tables must come from ONE PSRAM block that the single
    #     out: label frees, and must never reappear as stack arrays: no host
    #     vector can see a stack overflow, so only this pin stops the regression.
    if "MALLOC_CAP_SPIRAM" not in ladder:
        bad.append("stack: orb_app_install's settings scratch must come from "
                   "heap_caps_malloc(MALLOC_CAP_SPIRAM) — the orb_jobs worker has a "
                   "hard-measured 4096 B stack and overflowed on these two arrays")
    if "heap_caps_free(scratch)" not in ladder:
        bad.append("stack: the install's PSRAM scratch must be freed at the single "
                   "out: label, or every install leaks ~2 KB of PSRAM")
    for stack_arr in ("orb_setting_desc_t settings[ORBAPP_SETTINGS_MAX]",
                      "uint32_t sw[ORBAPP_SETTINGS_MAX * ORBAPP_SETTINGS_WORDS]"):
        if stack_arr in ladder:
            bad.append("stack: %r is back on the worker's frame — ~1 KB each on a "
                       "4096 B stack is the crash orb_jobs.c:65 warns about" % stack_arr)

    # 4. the installer never writes or reads a grant word, and never touches NVS
    #    directly (settings persist only through the broker). An absent grant stays
    #    absent — as-declared.
    for forbidden in ("orb_svc_set_grant(", "nvs_open(", "nvs_set_u32(", "nvs_write("):
        if forbidden in src:
            bad.append("D19: %r found — the installer must never write a grant word "
                       "or reach NVS itself" % forbidden)

    # 5. the browser→card upload (P4). The ordering here is the card-side twin of
    #    the D9 ladder above: a half-arrived container must never reach a loadable
    #    name, or the very next walk would list it and the install could select it.
    #    Scoped to orb_app_upload_end()'s body ONLY — verify_payload()'s own
    #    DEFINITION sits far earlier in the file and would invert an order check,
    #    the exact trap the install ladder above scopes around.
    ulo = src.find("esp_err_t orb_app_upload_end(")
    uhi = src.find("void orb_app_upload_cancel(", ulo if ulo >= 0 else 0)
    if ulo < 0 or uhi < 0:
        bad.append("upload: could not locate orb_app_upload_end()'s body — the ladder is gone")
    else:
        ubody = src[ulo:uhi]

        def uorder(a, b, why):
            ia, ib = ubody.find(a), ubody.find(b)
            if ia < 0 or ib < 0 or ia > ib:
                bad.append("upload order: %r must precede %r — %s" % (a, b, why))

        # the reader's header ladder runs off the card, before the publish rename.
        uorder("app_hdr_validate(fd, &h)", "rename(up->tmp, final)",
               "the container must pass the reader before it is filed under its loadable name")
        # The payload CRCs are NOT verified here — that is the install's D9 job, run
        # on the worker off the httpd task. The card-facing tier reads only the header
        # (scan.c:17). If the upload reaches for verify_payload it reintroduces the
        # internal-DMA-buffer squeeze that starves httpd (ESP_ERR_HTTPD_TASK).
        if "verify_payload" in ubody:
            bad.append("upload: verify_payload must not run in the upload tier — the install "
                       "re-checks the payload off the card (D9); checking it from the httpd "
                       "task needs the internal DMA buffer that starves httpd")
        # the destination is the header's OWN slug, never a client-supplied name.
        if "orb_files_path(src, h.slug, final" not in ubody:
            bad.append("upload: the destination must come from orb_files_path(src, h.slug, ...), "
                       "never a client string — a nameable upload chooses what the next walk lists")
        # a rejected or colliding upload deletes the scratch, never leaving it to be listed.
        if "unlink(up->tmp)" not in ubody:
            bad.append("upload: a rejected upload must unlink(up->tmp), not survive as a loadable file")
        # D37: the duplicate policy's identity tuple. Six words, and the list is a set
        # rather than a sample because the bug this check exists over was an omission of
        # exactly one of them: the region checksum. A container whose only difference is
        # its editor compared equal on the other five, so the upload was accepted quietly,
        # the card kept the region the author had just replaced, and no message said a
        # thing — ?overwrite=1 was the only way out of a refusal that never happened. Any
        # word dropped from this line recreates that, so every one is named.
        DUP = "if (!or_ && "
        dlo = ubody.find(DUP)
        if dlo < 0:
            bad.append("upload: the duplicate policy's comparison is gone — nothing now decides "
                       "whether a second container is the same app")
        else:
            dtail = ubody[dlo:ubody.find("{", dlo)]
            for word in ("oh.geom_crc == h.geom_crc", "oh.code_crc == h.code_crc",
                         "oh.code_size == h.code_size", "oh.editor_crc == h.editor_crc",
                         "oh.serial == h.serial", "strcmp(oh.slug, h.slug) == 0"):
                if word not in dtail:
                    bad.append("upload: the identity tuple dropped %s — an app differing only in "
                               "that word is filed as unchanged and its new bytes are discarded"
                               % word)
    # the scratch must NOT carry the walk's extension, or a half upload is listed and
    # the install's resolve-by-stem can select a file that is two thirds of an app.
    if '#define APPS_SCRATCH "upload.part"' not in src:
        bad.append("upload: APPS_SCRATCH must be \"upload.part\" — a scratch ending in .orbapp "
                   "would be listed by orb_files_walk and selectable by the install")
    # one upload owns the one card lock at a time (the scan reports busy, not blocked).
    if "if (s_up) return ESP_ERR_INVALID_STATE" not in src:
        bad.append("upload: orb_app_upload_begin must refuse a second concurrent upload")
    # 6/7. P4a uninstall and enable. Uninstall order is the power-cut safety:
    #    erase the header (D9: no header, no app), forget the owner words, free
    #    the row — reversed, a live slot resurrects at reboot or a row outlives
    #    its flash. Header sector only: B5 counts live headers, not bytes. Enable
    #    persists the e<slot> word BEFORE the RAM flip (a crash keeps the decision
    #    on disk), and a failed app goes through clear_faults — first caller.
    ulo2 = src.find("esp_err_t orb_app_uninstall(")
    uhi2 = src.find("orb_app_upload_begin", ulo2 if ulo2 >= 0 else 0)
    if ulo2 < 0 or uhi2 < 0:
        bad.append("P4a: could not locate orb_app_uninstall body")
    else:
        ub = src[ulo2:uhi2]
        ierase, ifork, irem = (ub.find("esp_partition_erase_range("),
                               ub.find("orb_svc_forget_row("),
                               ub.find("orb_app_card_remove("))
        if ierase < 0 or ifork < 0 or irem < 0 or not (ierase < ifork < irem):
            bad.append("P4a: uninstall must erase the header, then forget the words,")
        if "APPS_SLOT_HDR_BYTES)" not in ub:
            bad.append("P4a: uninstall must erase exactly the header sector")
        if "APPS_SLOT_STRIDE" in ub:
            bad.append("P4a: uninstall must not erase the whole slot stride")
        # D38: an app on the glass is uninstallable — the door homes the dial
        # (task-safe request) and waits for the switch, instead of refusing the
        # press and teaching the operator to carousel away.
        if "input_router_request(orb_app_home())" not in ub or "orb_app_current()" not in ub:
            bad.append("D38: uninstall must home a shown app and wait for the switch")
        if "carousel away from it first" in ub:
            bad.append("D38: uninstall must no longer refuse a row the knob is holding")
        if "UNHOME_WAIT_MS" not in ub:
            bad.append("D38: the home wait must be bounded by UNHOME_WAIT_MS")
    elo = src.find("esp_err_t orb_app_enable(")
    ehi = src.find("esp_err_t orb_app_uninstall(", elo if elo >= 0 else 0)
    if elo < 0 or ehi < 0:
        bad.append("P4a: could not locate orb_app_enable body")
    else:
        eb = src[elo:ehi]
        iw, ir = eb.find("orb_svc_set_enabled("), eb.find("orb_app_set_enabled(")
        if iw < 0 or ir < 0 or iw > ir:
            bad.append("P4a: enable must persist the word BEFORE the RAM flip")
        if "orb_svc_clear_faults(" not in eb:
            bad.append("P4a: enabling a failed app must clear its record first")
        if "that app is on the dial" not in eb:
            bad.append("P4a: disabling the app on the glass must be refused")
    # 7b. THE DOOR GATE'S WORKER EXEMPTION (bench, orb-B 2026-10-21). The dial
    #       menu runs the enable/uninstall doors AS its orb_jobs job; busy() is
    #       that job's own flag, so a gate that reads it unconditionally refuses
    #       every press on the glass with "a card job is running" — the door
    #       deadlocks on itself. door_open must read busy() only when the caller
    #       is not the worker, and orb_jobs.c must answer on_worker by task
    #       identity (never by a flag a posted-but-unstarted job could fake).
    dlo = src.find("static const orb_app_t *door_open(")
    dhi = src.find("esp_err_t orb_app_enable(", dlo if dlo >= 0 else 0)
    if dlo < 0 or dhi < 0:
        bad.append("P7: could not locate door_open body")
    else:
        db = src[dlo:dhi]
        if "!orb_jobs_on_worker()" not in db or "orb_jobs_busy()" not in db:
            bad.append("P7: door_open must exempt the worker from its busy gate")
        if "orb_app_upload_busy()" not in db:
            bad.append("P7: the upload window must still refuse the door")
    jsrc = read(os.path.join(ROOT, "main", "app", "orb_jobs.c"))
    if "xTaskGetCurrentTaskHandle() == s_worker" not in jsrc:
        bad.append("P7: orb_jobs_on_worker must compare task identity")
    # 8. key stability: the fresh-install wipe guards a REUSED slot (an
    #    upgrade keeps words — D19), and main.c runs the apply rung between
    #    the broker coming up and the first screen building.
    if "if (v.slot != sl.existing_slot)" not in src:
        bad.append("P4a: a fresh install must wipe a reused slot stale words")
    blo = src.find("void orb_app_install_apply_enable(void)")
    if blo < 0 or src.find("orb_svc_enabled(", blo) < 0:
        bad.append("P4a: the boot apply rung must read the stored enable words")
    ms = read(os.path.join(ROOT, "main", "main.c"))
    i1, i2, i3 = (ms.find("orb_svc_init())"),
                  ms.find("orb_app_install_apply_enable();"),
                  ms.find("orb_app_init_all())"))
    if not (0 <= i1 < i2 < i3):
        bad.append("P4a: main.c ladder order (apply after svc_init, before init_all)")
    # 9. net doors: both POSTs registered, both reaching the device, and the
    #    installed tier streamed — refusal text stays the device sentence.
    nsrc = read(os.path.join(ROOT, "main", "net", "orb_net.c"))
    for lit in ("/api/v1/apps/enable", "/api/v1/apps/uninstall",
                "h_app_enable", "h_app_uninstall",
                "orb_app_enable(id", "orb_app_uninstall(id",
                "apps_send_installed(req, row, APPS_ROW_MAX);"):
        if lit not in nsrc:
            bad.append("P4a: orb_net.c lost %r" % lit)
    for uri in ("/api/v1/apps/enable", "/api/v1/apps/uninstall"):
        iu = nsrc.find('.uri = "%s"' % uri)
        if iu < 0 or "HTTP_POST" not in nsrc[iu:iu + 120]:
            bad.append("P4a: %s must be registered as a POST" % uri)
    # 10. P4b (D21): the installer reads NO art word. The reader refuses an
    #     art-carrying container; a second opinion in the installer would be a
    #     second policy, and P4b exists to leave exactly one. verify_payload
    #     checksums geometry only; the registry row lands art-free; the upload
    #     bound carries no art term.
    for art_read in ("h->art_size", "h.art_size", "h->art_crc", "h.art_crc",
                     "oh.art_crc", "h->art_off", "h.art_off",
                     "APPS_ART_SMALL_MAX"):
        if art_read in src:
            bad.append("P4b: install.c reads the art word %r — the reader owns "
                       "that refusal now" % art_read)
    # 10b. the bench's own catch: both header validators RETURN their reason,
    #      so the reason's buffer must outlive the frame. The pre-P4b stack
    #      version reached the browser as garbage bytes; P4b is the first phase
    #      to send a real sentence through the door, and it arrived as `#\x04\x06`.
    if src.count("static char why[160];") != 2:
        bad.append("P4b: both read_and_check_header and app_hdr_validate must "
                   "carry the refusal in static storage — the door prints it "
                   "after the validators' frames are gone")
    ih = read(INSTALL_H)
    # D37 grew this bound by ONE term — the editor region — and the P4b rule it is
    # checked under survives untouched: no art term may appear. The installed bill
    # is still a slot's geometry budget plus the header block and its gap; the
    # editor adds to the largest legal FILE and not to the largest slot, which is
    # the distinction §4.2 is built on, so it is checked for by name.
    upmax = re.search(r"#define\s+ORB_APP_UPLOAD_MAX\s+\(([^)]*)\)", ih, re.S)
    if not upmax:
        bad.append("P4b: ORB_APP_UPLOAD_MAX is no longer a parseable expression — "
                   "read the value off main/app/orb_app_install.h and compare it to "
                   "orbapp.UPLOAD_MAX BY HAND; do not let this check go quiet")
    else:
        terms = upmax.group(1)
        for want in ("ORBAPP_GEOM_BYTES_MAX", "0x20000u", "ORBAPP_EDITOR_MAX", "0x1000u"):
            if want not in terms:
                bad.append("D37: ORB_APP_UPLOAD_MAX lost its %s term: %s" % (want, terms))
        if "ART" in terms.upper().replace("PART", ""):
            bad.append("P4b: the upload bound grew an art term again — the largest "
                       "installable container is header + settings + geometry, and "
                       "D37's one addition is the editor region")
    # ---- P7: the device half's code accounting, pinned as text ----------------
    # install_decide is host-lifted and mutation-pinned, but copy_prefix and
    # verify_payload run only on the worker task. Their code-region handling is the
    # other half of D27's "never writes a byte of flash" promise — an accepted
    # module must be COPIED (copy_prefix) and CHECKED OFF-CARD (verify_payload) just
    # as the geometry is. A check nobody has watched is a check that may be dead, so
    # each is pinned by a rule here and a mutant that must be caught.
    if "if (h->code_size && (h->code_off + h->code_size) > blob)\n        blob = h->code_off + h->code_size;" not in src:
        bad.append("P7: copy_prefix must widen the copied prefix to the code region's end — "
                   "install_decide reserves the slot for code; a copy that stops at geom_end "
                   "leaves the header pointing at bytes that were never written")
    if 'if (h->code_size) {\n        crc = 0;\n        pos = h->code_off;' not in src:
        bad.append("P7: verify_payload must checksum the code region off-card like the "
                   "geometry — a corrupt module must be refused before the erase, not "
                   "reach the slot the loader would trust")

    # 9. D36 (§0.11): the image ships no apps. The factory tier is gone from
    #    this file and from the build, and the hub seeds the starter pair
    #    through the ordinary doors instead. These pins keep the cut honest:
    #    one ladder, no in-image app source, and a bare flash manifest.
    if src.count("install_ladder(p, &src") < 2:
        bad.append("D36: every installer — card front and image front — must run "
                   "through install_ladder(); a private copy of the "
                   "erase/copy/verify/header-last sequence is the drift the ruling forbids")
    code_only = re.sub(r"/\*.*?\*/", "", src, flags=re.S)  # the cut note may name what died
    for gone in ("factory", "provision"):
        if gone in code_only:
            bad.append("D36: %r survives in the installer code — the factory tier was cut "
                       "with the in-image apps; a revived rung here is a second shipping "
                       "path the hub must not have to out-guess" % gone)
    return bad


# ---- mutations ---------------------------------------------------------------
# A mutation is (name, kind, fn). kind "pure" rewrites the lifted install_decide
# (run_pure must report a failed vector); kind "device" rewrites the shipped
# ladder text the lint reads (lint must report a failure). An uncaught mutant is
# a claim in this suite that nothing actually enforces.
def _m(old, new):
    def f(t):
        if old not in t:
            raise SystemExit("mutation anchor absent — code moved: %r" % old[:60])
        return t.replace(old, new, 1)
    return f


MUTATIONS = [
    ("serial bumped on reinstall (a re-copy moves the grant key)", "pure",
     _m("v->serial = sl->slot_serial[sl->existing_slot];   /* kept, not bumped */",
        "v->serial = sl->slot_serial[sl->existing_slot] + 1u;")),
    ("cap off-by-one: >= becomes > (a 9th app slips in)", "pure",
     _m("if (sl->installed_count >= APPS_N_SLOTS)",
        "if (sl->installed_count > APPS_N_SLOTS)")),
    ("quota gate switched off", "pure",
     _m("if (worst > ORB_CFG_QUOTA_BYTES)",
        "if (false && worst > ORB_CFG_QUOTA_BYTES)")),
    ("geometry cap gate switched off (blob can overrun the slot)", "pure",
     _m("if (blob > (uint64_t)APPS_SLOT_GEOM_CAP)",
        "if (false && blob > (uint64_t)APPS_SLOT_GEOM_CAP)")),
    # P7: the code region must enter the slot-fit max(). Dropping it lets a
    # module-bearing app bill only its geometry and overrun the slot with code.
    ("code region dropped from the slot-fit bill (P7 regression)", "pure",
     _m("    if (h->code_size) {\n"
        "        const uint64_t code_end = (uint64_t)h->code_off + h->code_size;\n"
        "        if (code_end > blob)\n"
        "            blob = code_end;\n"
        "    }\n", "")),
    # P7 D27: the ceiling gate switched off lets an un-allocatable memory bound
    # through to the erase, which is the exact refusal the P7 pass line names.
    ("D27 memory ceiling gate switched off", "pure",
     _m("if (h->wasm_mem_max > (uint32_t)ORBAPP_WASM_MEM_MAX)",
        "if (false && h->wasm_mem_max > (uint32_t)ORBAPP_WASM_MEM_MAX)")),
    # P7 device half: the copied prefix must reach the code region's end. A copy that
    # stops at geom_end leaves the header's code triple pointing at bytes never
    # written — the loader (P7.2) would read garbage from the slot.
    ("copy_prefix stops at geom_end (code never reaches the slot)", "device",
     _m("    uint32_t blob = h->geom_off + h->geom_size;\n"
        "    if (h->code_size && (h->code_off + h->code_size) > blob)\n"
        "        blob = h->code_off + h->code_size;",
        "    uint32_t blob = h->geom_off + h->geom_size;")),
    # P7 device half: verify_payload must checksum the code off-card. Dropping the
    # block lets a corrupt module survive to the erase the loader would trust.
    ("verify_payload skips the code region's checksum", "device",
     _m("    if (h->code_size) {\n        crc = 0;\n        pos = h->code_off;",
        "    if (0 && h->code_size) {\n        crc = 0;\n        pos = h->code_off;")),
    ("installer re-reads the art word (a P4b regression re-grows the gate)", "pure",
     _m("    /* Gate 1 — the manifest+geometry+code prefix the slot must hold.",
        "    if (h->art_size) { snprintf(v->why, sizeof v->why, \"its art bank "
        "is too big\"); return false; }\n\n"
        "    /* Gate 1 — the manifest+geometry+code prefix the slot must hold.")),
    ("off-card payload check dropped (erase before verify)", "device",
     _m("const char *vv = verify_payload(fd, h);",
        "const char *vv = NULL; (void)verify_payload;")),
    ("slot header written before the geometry reads back", "device",
     _m("    if (e == ESP_OK) e = verify_slot_geom(p, h, v.slot, &fw);\n"
        "    if (e == ESP_OK) e = write_slot_header(p, v.slot, raw, &fw);",
        "    if (e == ESP_OK) e = write_slot_header(p, v.slot, raw, &fw);\n"
        "    if (e == ESP_OK) e = verify_slot_geom(p, h, v.slot, &fw);")),

    ("caps sourced from a scan row, not the header", "device",
     _m("row.caps       = h->caps;", "row.caps       = 0;")),
    ("behaviour_kind dropped at the install site (resolver never dispatches)", "device",
     _m("    row.behaviour_kind = h->behaviour_kind;   /* D1/C6: selects the compiled module */\n", "")),
    ("behaviour_kind zeroed at boot-resolve (installed app loses its module)", "device",
     _m("row.behaviour_kind = h.behaviour_kind;  /* D1/C6: selects the compiled module */",
        "row.behaviour_kind = 0;")),
    ("installer writes a grant word (breaks as-declared persistence)", "device",
     _m("row.caps       = h.caps;                 /* D19: from the slot header only */",
        "row.caps       = h.caps; orb_svc_set_grant(h.slug, h.caps);")),
    ("upload publishes before reading the header (rename before validate)", "device",
     _m("    const char *r = app_hdr_validate(fd, &h);",
        "    const char *r = NULL; (void)app_hdr_validate;")),
    ("upload reaches for the payload CRC from the httpd task (starves httpd)", "device",
     _m("    const char *r = app_hdr_validate(fd, &h);",
        "    const char *r = app_hdr_validate(fd, &h); if (!r) verify_payload(fd, &h);")),
    ("install's settings tables back on the worker's stack (the 4096 B overflow)", "device",
     _m("""    uint8_t *scratch = heap_caps_malloc(ORBAPP_SETTINGS_MAX * ORBAPP_SETTINGS_WORDS * 4u +
                                        ORBAPP_SETTINGS_MAX * sizeof(orb_setting_desc_t),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!scratch) { install_close(fd);
        return install_failed(ESP_ERR_NO_MEM, "out of memory for the settings table",
                              why, whysz); }
    uint32_t *sw = (uint32_t *)scratch;
    orb_setting_desc_t *settings = (orb_setting_desc_t *)(scratch +
                                        ORBAPP_SETTINGS_MAX * ORBAPP_SETTINGS_WORDS * 4u);""",
        """    orb_setting_desc_t settings[ORBAPP_SETTINGS_MAX];
    uint32_t sw[ORBAPP_SETTINGS_MAX * ORBAPP_SETTINGS_WORDS];
    uint8_t *scratch = NULL;
    if (false) { install_close(fd);
        return install_failed(ESP_ERR_NO_MEM, "out of memory for the settings table",
                              why, whysz); }""")),
    ("upload scratch carries the walk's extension (a half-file is selectable)", "device",
     _m('#define APPS_SCRATCH "upload.part"',
        '#define APPS_SCRATCH "upload.orbapp"')),
    ("upload names the destination from the client, not the header's slug", "device",
     _m("orb_files_path(src, h.slug, final, sizeof final)",
        "orb_files_path(src, \"guessed\", final, sizeof final)")),
    # P4a's six — authored beside the list last session; P4b folded them IN
    # after the fold caught them sitting orphaned between the _m def and the
    # list opener, where Python parses them and the runner never runs them.
    ("P4a: uninstall frees row and words but never erases the header", "device",
     _m("const esp_err_t e = esp_partition_erase_range(p, slot_base(fslot),",
        "const esp_err_t e = ESP_OK; (void)slot_base;")),
    ("P4a: uninstall keeps the owner words (revokes outlive their app)", "device",
     _m("if (orb_svc_forget_row(slot) != ESP_OK)", "if ((slot) != ESP_OK)")),
    ("P4a: enable flips RAM before the word persists", "device",
     _m("const esp_err_t e = orb_svc_set_enabled(slot, on);",
        "orb_app_set_enabled(row, on); const esp_err_t e = orb_svc_set_enabled(slot, on);")),
    ("P4a: enable skips clear_faults (revive over live crash counts)", "device",
     _m("if (orb_svc_clear_faults((orb_app_t *)row) != ESP_OK)", "if (false)")),
    ("P4a: fresh install inherits a dirty slot stale words", "device",
     _m("if (v.slot != sl.existing_slot) {", "if (false) {")),
    ("P4a: uninstall erases the whole slot stride", "device",
     _m("const esp_err_t e = esp_partition_erase_range(p, slot_base(fslot),\n"
        "                                                      APPS_SLOT_HDR_BYTES);",
        "const esp_err_t e = esp_partition_erase_range(p, slot_base(fslot),\n"
        "                                                      APPS_SLOT_STRIDE);")),
    # P4b: the reader owns the art refusal; the installer trusting an art word
    # again — at the registry row or in the payload loop — is what lint 10 eats.
    ("P4b: registry row trusts the header art word again", "device",
     _m("row.art_bytes  = 0;               /* D21: the container carries no art */",
        "row.art_bytes  = h->art_size;")),
    ("P4b: verify_payload re-grows the art CRC loop", "device",
     _m("    if (crc != h->geom_crc)\n        return \"the geometry does not match",
        "    pos = h->art_off; left = h->art_size;\n"
        "    if (crc != h->geom_crc)\n        return \"the geometry does not match")),
    # the dangling-reason bug P4b's bench found: the mutant puts the frame-local
    # buffer back in app_hdr_validate — lint 10b must eat it.
    ("P4b: upload refusal goes back on the stack (dangling detail)", "device",
     _m("     * httpd task, one upload at a time (orb_app_upload_begin's gate). */\n"
        "    static char why[160];",
        "     * httpd task, one upload at a time (orb_app_upload_begin's gate). */\n"
        "    char why[128];")),
]


def run_mutations():
    caught = 0
    for name, kind, fn in MUTATIONS:
        if kind == "pure":
            fails = run_pure(mutate=(name, fn))
            hit = fails > 0
        else:
            broken = fn(inst_src)
            bad = lint(broken)
            hit = len(bad) > 0
        if hit:
            caught += 1
            print("ok    caught: %s" % name)
        else:
            print("FAIL  UNCAUGHT MUTANT: %s" % name)
    print("%d/%d mutants caught" % (caught, len(MUTATIONS)))
    return len(MUTATIONS) - caught


def seed_check():
    """D36 (§0.11) at S5 posture: the starter artifacts are what the ruling says.

    Re-runs both generators in --check mode (the freshness rule bake_clock.py
    serves), then reads the two starter containers back through the Python reader
    — the mirror of the device reader both reader suites pin to — and asserts
    each header’s declared identity. A starter file that no longer matches this
    list is a ruling change, and a ruling change edits this list and the D-row,
    in that order, not a silent re-bake. The theme leg proves the shipped seed
    (themes/aviator.orbtheme) still round-trips byte-identical to the slot image
    it is lifted from, name field included.
    S4 adds one leg: the clock's header must carry a CRC-covered editor region."""
    import subprocess
    fails = 0
    def check(name, ok, detail=""):
        nonlocal fails
        print(("ok    " if ok else "FAIL  ") + name + (("  " + detail) if detail else ""))
        if not ok:
            fails += 1
    r = subprocess.run([sys.executable, os.path.join(HERE, "make_factory_apps.py"),
                        "--check"], capture_output=True, text=True)
    check("starter containers fresh", r.returncode == 0, (r.stdout + r.stderr).strip())
    if r.returncode != 0:
        return 1
    r = subprocess.run([sys.executable, os.path.join(HERE, "make_seed_theme.py"),
                        "--check"], capture_output=True, text=True)
    check("starter theme fresh and round-tripping", r.returncode == 0, (r.stdout + r.stderr).strip())
    if r.returncode != 0:
        return fails + 1
    import importlib.util
    _spec = importlib.util.spec_from_file_location("orbapp", os.path.join(HERE, "orbapp.py"))
    orbapp = importlib.util.module_from_spec(_spec)
    _spec.loader.exec_module(orbapp)
    seed_dir = os.path.join(HERE, os.pardir, "tools", "orb-manager", "apps")
    # S6 (§4.1): the starter pair's version and serial come from version.txt, the one
    # source tools/make_factory_apps.py reads. Recomputed rather than typed, so this pin
    # goes red when the header's string and the tree's release disagree — and not on a
    # release that moved both together, which is the case a literal would confuse.
    want_version = open(os.path.join(HERE, os.pardir, "version.txt")).read().strip()
    want_serial = orbapp.serial_of_version(want_version)
    want = [("clock", "Clock", 3, "sd_read|themes"),
            ("color_picker", "HSB", 1, "")]
    for slug, title, kind, capnames in want:
        path = os.path.join(seed_dir, slug + ".orbapp")
        try:
            blob = open(path, "rb").read()
            h = orbapp.parse_header(blob)
        except (OSError, ValueError) as e:
            check("starter container %s reads" % slug, False, str(e))
            fails += 1
            continue
        names = list(orbapp.CAP_FREE) + list(orbapp.CAP_OWNER)
        got_caps = set(names[b] for b in range(len(names)) if h["caps"] >> b & 1)
        want_caps = set(capnames.split("|")) if capnames else set()
        # S6 (§4.1): the serial is version.txt's number, recomputed here rather than
        # matched against a literal. The starter pair's stamp rides at 1 no more, and a
        # pin that traded 1 for 10127 would go red on the next release for the right
        # reason and the wrong cause; this one goes red only when the two sources of the
        # same fact — the string in the header and the number in version.txt — disagree.
        ok = (h["slug"] == slug and h["title"] == title
              and h["behaviour_kind"] == kind and got_caps == want_caps
              and h["serial"] == want_serial and h["code_size"] == 0
              and h["art_size"] == 0
              and h["version"] == want_version)
        check("starter %s.orbapp is the %s row the ruling names" % (slug, title), ok,
              "slug=%r kind=%d caps=%s serial=%d version=%r" % (
                  h["slug"], h["behaviour_kind"], sorted(got_caps), h["serial"],
                  h["version"]))
        fails += 0 if ok else 1
        # S4: the clock's editor travels inside its own container. A region of
        # zero here is the bake silently reverting to the hub-hosted alias.
        if slug == "clock":
            has_reg = h.get("editor_size", 0) > 0 and h.get("editor_crc", 0) != 0
            check("starter clock carries its editor region", has_reg,
                  "editor_size=%d crc=%08x" % (h.get("editor_size", 0), h.get("editor_crc", 0)))
            fails += 0 if has_reg else 1
        if slug == "color_picker":
            rows = orbapp.unpack_settings(blob[h["setting_off"]:
                                          h["setting_off"] + h["n_settings"] *
                                          orbapp.SETTINGS_WORDS * 4],
                                          h["n_settings"])
            sets = {s["id"]: (s["min"], s["max"], s["default"]) for s in rows}
            want_sets = {"hue": (0, 359, 210), "sat": (0, 100, 80), "bri": (0, 100, 90)}
            check("the seeded picker’s settings match its statics", sets == want_sets, str(sets))
            fails += 0 if sets == want_sets else 1
    return fails


def main():
    argv = sys.argv[1:]
    fails = 0
    bad = lint(inst_src)
    if bad:
        fails += len(bad)
        for b in bad:
            print("FAIL  ladder lint: " + b)
    else:
        print("ok    ladder lint: off-card verify, decide-before-erase, "
              "header-last, caps-from-manifest, no grant/NVS write, "
              "upload verify-before-publish + header-derived name")
    fails += seed_check()
    fails += run_pure()
    if "--mutations" in argv:
        fails += run_mutations()
    if fails:
        print("\n%d FAILURE(S)" % fails)
        sys.exit(1)
    print("\nall host checks pass")


if __name__ == "__main__":
    main()





