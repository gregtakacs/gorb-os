#!/usr/bin/env python3
"""host_orbapp_install_test.py — the sentinel for the app installer (P4).

Two halves, mirroring the two files it watches.

THE PURE DECISION. install_decide() in main/app/orb_app_install.c is the cap,
the quota, the slot and the serial over a header the reader already accepted and
a slot state the caller read under the lock. It is PURE — no partition, no fd,
no NVS — so this suite LIFTS it verbatim (dying by name if it moves, the way
host_orbapp_scan_test.py lifts row_decide), recompiles it with host gcc -Werror
against the SHIPPED orbapp_hdr_t / orb_setting_desc_t, and drives it with headers
whose geometry/art/settings fields it sets directly. install_decide never calls
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
f_appcap = lift(inst_hdr, r"#define APPS_PARTITION_NAME[\s\S]*?#define APPS_ART_SMALL_MAX[^\n]*\n", INSTALL_H)

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
                         "-I", tmp, "-I", os.path.join(ROOT, "main"),
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
ART_SMALL = 0x00050000         # APPS_ART_SMALL_MAX
N_SLOTS = 8                    # APPS_N_SLOTS == ORB_APP_CAP
STR_WORST = 32                 # sizeof orb_setting_desc_t.def_str
NUM_WORST = 4                  # one nvs_set_u32
SETTINGS_MAX = 16              # ORBAPP_SETTINGS_MAX

# A refusal is checked by the fragment of its sentence that names WHICH gate fired
# (the wording itself belongs to the reader mirror, not to this decision).
WHY_KEY = {"geom": "manifest and geometry", "art": "art bank",
           "quota": "setting(s) need", "cap": "already holds",
           "range": "out of range"}


def ref(geom_off, geom_size, art_size, strc, nset, existing, cnt, mask, serial, quota):
    """The independent model of install_decide's ladder — the intent. The device
    runs the LIFTED code; the two must agree, and it is exactly their disagreement
    that is the bug this suite hunts."""
    blob = geom_off + geom_size
    if blob > GEOM_CAP:
        return dict(accept=0, slot=-1, reinstall=0, serial=0, cfg_worst=0, key="geom")
    if art_size > ART_SMALL:
        return dict(accept=0, slot=-1, reinstall=0, serial=0, cfg_worst=0, key="art")
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
    a(("geom_before_all", 0, GEOM_CAP + 1, ART_SMALL + 1, 0, 0, -1, 9, 0xFF, S0))
    a(("art_before_quota", 0, 1024, ART_SMALL + 1, 8, 8, -1, 0, 0b0, S0))
    a(("accept_art_small_edge", 0, 1024, ART_SMALL, 0, 0, -1, 0, 0b0, S0))
    a(("accept_reuse", 0, 1024, 0, 0, 0, 3, 3, 0b1111, [0, 0, 0, 7] + [0] * 4))
    a(("reuse_out_of_range", 0, 1024, 0, 0, 0, 8, 1, 0b0, S0))
    a(("accept_new_serial_stale", 0, 1024, 0, 0, 0, -1, 1, 0b1, [9] + S0[1:]))
    a(("cap_8_free_but_count", 0, 1024, 0, 0, 0, -1, 8, 0b11111110, S0))
    a(("cap_all_used", 0, 1024, 0, 0, 0, -1, 7, 0xFF, S0))
    a(("accept_7", 0, 1024, 0, 0, 0, -1, 7, 0b01111111, S0))
    a(("mixed_quota_accept", 0, 1024, 0, 2, 4, -1, 0, 0b0, S0))
    a(("legal_table_never_trips", 0, 1024, 0, 16, 16, -1, 0, 0b0, S0))
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
    return struct.pack("<8I8I", go, gs, art, strc, nset,
                       existing & 0xFFFFFFFF, cnt, mask, *serials)


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
        exp = ref(c[1], c[2], c[3], c[4], c[5], c[6], c[7], c[8], c[9], quota)
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
    lo = src.find("esp_err_t orb_app_install(const char *stem")
    hi = src.find("install_req_t", lo if lo >= 0 else 0)
    if lo < 0 or hi < 0:
        return ["could not locate orb_app_install()'s body — the ladder is gone"]
    ladder = src[lo:hi]

    def order(a, b, why):
        ia, ib = ladder.find(a), ladder.find(b)
        if ia < 0 or ib < 0 or ia > ib:
            bad.append("order: %r must precede %r — %s" % (a, b, why))

    # 1. the whole payload checksums off the card, and the pure decision runs,
    #    BEFORE the first erase (the erase lives inside copy_prefix).
    order("verify_payload(fd, h)", "copy_prefix(p, fd, h",
          "the payload must be CRC-verified off the card before anything erases")
    order("install_decide(h, &sl, &v)", "copy_prefix(p, fd, h",
          "cap/quota/slot/serial decided before the erase, so a refusal costs no slot")
    order("read_slots(p, h->slug, &sl)", "install_decide(h, &sl, &v)",
          "the slot state is read under the lock before the pure decision prices it")
    # 2. geometry read back FROM the partition, header written LAST.
    order("copy_prefix(p, fd, h", "verify_slot_geom(",
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
    # the scratch must NOT carry the walk's extension, or a half upload is listed and
    # the install's resolve-by-stem can select a file that is two thirds of an app.
    if '#define APPS_SCRATCH "upload.part"' not in src:
        bad.append("upload: APPS_SCRATCH must be \"upload.part\" — a scratch ending in .orbapp "
                   "would be listed by orb_files_walk and selectable by the install")
    # one upload owns the one card lock at a time (the scan reports busy, not blocked).
    if "if (s_up) return ESP_ERR_INVALID_STATE" not in src:
        bad.append("upload: orb_app_upload_begin must refuse a second concurrent upload")
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
    ("art-size gate switched off (flagship bank accepted)", "pure",
     _m("if (h->art_size > APPS_ART_SMALL_MAX)",
        "if (false && h->art_size > APPS_ART_SMALL_MAX)")),
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
    ("upload scratch carries the walk's extension (a half-file is selectable)", "device",
     _m('#define APPS_SCRATCH "upload.part"',
        '#define APPS_SCRATCH "upload.orbapp"')),
    ("upload names the destination from the client, not the header's slug", "device",
     _m("orb_files_path(src, h.slug, final, sizeof final)",
        "orb_files_path(src, \"guessed\", final, sizeof final)")),
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
    fails += run_pure()
    if "--mutations" in argv:
        fails += run_mutations()
    if fails:
        print("\n%d FAILURE(S)" % fails)
        sys.exit(1)
    print("\nall host checks pass")


if __name__ == "__main__":
    main()





