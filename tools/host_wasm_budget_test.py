#!/usr/bin/env python3
"""host_wasm_budget_test.py — the wasm time budget's I/O slip, tested on the host.

    python3 tools/host_wasm_budget_test.py              # vectors
    python3 tools/host_wasm_budget_test.py --mutations  # do the vectors bite?

main/app/orb_app_wasm.c's deadline is wall time, and a large transfer (card upload,
theme store, OTA) starves the LVGL task its guests run on. budget_fire() therefore
forgives a deadline when a transfer is open or started during the call, re-arming for
another slice, up to ORB_WASM_IO_SLIP_MS a call. The shipped budget block is lifted
verbatim into gcc beside a recorded esp_timer and a scripted orb_io_window, and each
vector drives the deadline the way the timer would.
"""
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, os.pardir))
SRC = os.path.join(ROOT, "main", "app", "orb_app_wasm.c")

HARNESS = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef void *esp_timer_handle_t;
typedef const char *M3Result;
#define m3Err_none ((M3Result)0)
static int g_rearms;
static int esp_timer_start_once(esp_timer_handle_t t, uint64_t us) { (void)t; (void)us; g_rearms++; return 0; }
static bool g_heavy; static uint32_t g_gen;
static bool orb_io_heavy_now(void) { return g_heavy; }
static uint32_t orb_io_heavy_gen(void) { return g_gen; }
#define ORB_WASM_CALL_BUDGET_MS 250u
#define ORB_WASM_IO_SLIP_MS 2000u
#define RTC_NOINIT_ATTR
__BLOCK__
/* What guest_call does at the start of a call. */
static void call_start(void) { s_overrun = false; s_slip_ms = 0; s_call_gen = orb_io_heavy_gen(); g_rearms = 0; }
static int fires_until_overrun(int max)
{
    for (int i = 1; i <= max; i++) { budget_fire(0); if (s_overrun) return i; }
    return -1;
}
static int fails;
static void ck(const char *n, int ok) { printf("%s %s\n", ok ? "ok  " : "FAIL", n); if (!ok) fails++; }
int main(void)
{
    s_budget_ms = ORB_WASM_CALL_BUDGET_MS;
    g_heavy = false; g_gen = 7; call_start();
    ck("with no transfer the first deadline is an overrun", fires_until_overrun(20) == 1 && g_rearms == 0);

    g_heavy = true; call_start();
    const int n = fires_until_overrun(40);
    ck("an open transfer slips the deadline, slice by slice, up to the cap",
       n == (int)(ORB_WASM_IO_SLIP_MS / ORB_WASM_CALL_BUDGET_MS) + 1 && g_rearms == n - 1);
    ck("and every forgiven deadline is counted", orb_wasm_budget_slips() >= (uint32_t)(n - 1));

    g_heavy = false; g_gen = 7; call_start();
    g_gen = 8;                                    /* a transfer started and finished */
    ck("a transfer that came and went during the call buys one slice, not the cap",
       fires_until_overrun(20) == 2);

    g_heavy = false; g_gen = 9; call_start();
    budget_fire(0);
    ck("the slip does not leak into the next call", s_overrun);

    s_budget_ms = 1000u; g_heavy = true; call_start();
    ck("the init budget slips under the same ceiling",
       fires_until_overrun(40) == (int)(ORB_WASM_IO_SLIP_MS / 1000u) + 1);
    return fails ? 1 : 0;
}
'''

MUTATIONS = [
    ("the slip has no ceiling", "if (io && s_slip_ms + s_budget_ms <= ORB_WASM_IO_SLIP_MS) {", "if (io) {"),
    ("the window is ignored", "const bool io = orb_io_heavy_now() || gen != s_call_gen;", "const bool io = false;"),
    ("a finished transfer slips forever", "        s_call_gen = gen;                 /* the next slice needs I/O of its own */\n", ""),
    ("only an open transfer counts", "const bool io = orb_io_heavy_now() || gen != s_call_gen;", "const bool io = orb_io_heavy_now();"),
    ("a slip does not re-arm the deadline", "        esp_timer_start_once(s_budget, (uint64_t)s_budget_ms * 1000u);\n        return;", "        return;"),
]


def block(src):
    m = re.search(r"static const char s_overrun_trap\[\].*?uint32_t orb_wasm_budget_slips\(void\)\n\{.*?\n\}",
                  src, re.S)
    if not m:
        raise SystemExit("could not lift the budget block from orb_app_wasm.c — did it move?")
    return m.group(0)


def run(src_text, tmp):
    c = os.path.join(tmp, "b.c")
    open(c, "w").write(HARNESS.replace("__BLOCK__", block(src_text)))
    exe = os.path.join(tmp, "b")
    r = subprocess.run(["gcc", "-O1", "-Wall", "-Wno-unused-function", "-Wno-unused-variable", "-o", exe, c],
                       capture_output=True, text=True)
    if r.returncode:
        return None, r.stderr
    p = subprocess.run([exe], capture_output=True, text=True, timeout=10)
    return p.returncode, p.stdout


def main():
    src = open(SRC).read()
    with tempfile.TemporaryDirectory() as tmp:
        rc, out = run(src, tmp)
        print(out.strip())
        if rc != 0:
            print("FAIL the shipped budget does not pass its vectors")
            return 1
        if "--mutations" in sys.argv:
            caught = 0
            for name, old, new in MUTATIONS:
                if src.count(old) != 1:
                    print("FAIL mutation anchor lost: " + name)
                    return 1
                mrc, _ = run(src.replace(old, new, 1), tmp)
                if mrc != 0:
                    caught += 1
                else:
                    print("MISSED " + name)
            print("MUTATIONS %d/%d caught" % (caught, len(MUTATIONS)))
            return 0 if caught == len(MUTATIONS) else 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
