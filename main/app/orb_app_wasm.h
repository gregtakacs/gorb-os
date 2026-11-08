#pragma once
/**
 * orb_app_wasm — the wasm3 code tier (docs/app-platform-plan.md D3/D20/D27, §4.1,
 * §8 P7). P7.2: the engine vendored at components/wasm3 (PROVENANCE.md) behind ONE
 * generic behaviour module that every code-bearing .orbapp resolves to through
 * behaviour_kind == ORB_BEHAVIOUR_WASM (C6, append-only, next free id after the
 * picker), exactly as the HSB picker resolves through ORB_BEHAVIOUR_COLOR_PICKER.
 *
 * The shim, not the guest, is the thing the OS registers (D15: the frozen broker
 * table is the ONLY surface an app may call). A guest module gets its entry points
 * CALLED by the registry's existing hooks and calls the broker BACK through imports
 * bound from that module's granted caps — so a wasm app and a compiled app meet the
 * same contract at the same line. There is no per-app C code: one iface, N instances,
 * the same design that lets the picker's one iface serve both the compiled and an
 * installed HSB (the C6 singleton note in orb_app.c).
 *
 * Threading, unchanged from the registry (orb_app.h): every hook here runs on the
 * LVGL task, and the guest code it calls therefore runs on the LVGL task. That is
 * the same exposure a compiled app has; the difference is the guest cannot touch
 * memory outside its linear memory (wasm3 bounds every access — the trap site bills
 * C2 exactly as D27 promises, see the note at wasm_call), and its memory is capped at
 * manifest's D27 word before any of its code compiles.
 *
 * The pure decision half, orb_wasm_plan(), has no engine dependency: it turns a
 * header + the firmware's ceilings into "load or refuse, and if load, with what
 * memory cap". It is the toolchain-free gate the P7.1 reader already began, and the
 * host sentinel tools/host_orbapp_wasm_test.py lifts it exactly the way
 * host_orbapp_install_test.py lifts install_decide — hand-authored bytes, no board,
 * no engine. The engine calls below it are thin wrappers the sentinel cannot run and
 * does not pretend to; they are watched by the compile itself and by the size quote.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "orb_app.h"              /* orb_app_t, orb_app_iface_t — the iface this returns */
#include "orb_app_manifest.h"     /* orbapp_hdr_t — the D27 word the gate reads */

#ifdef __cplusplus
extern "C" {
#endif

/* The firmware's D27 ceiling for a guest's linear memory, in bytes. A manifest may
 * declare a SMALLER bound and gets it; it may never declare a larger one. Duplicated
 * from (and kept equal to) the installer's ceiling of the same name in
 * orb_app_install.h by test, not by include — the wasm TU and the install TU are
 * separate, and a reader that refuses at the door and a loader that would have
 * refused must state the SAME number or the two halves of D27 disagree. */
#ifndef ORB_WASM_MEM_MAX
#define ORB_WASM_MEM_MAX  (128u * 1024u)
#endif

/* The outcome of the pure load decision. */
typedef struct {
    bool     load;           /* false => refuse, and never reach the engine */
    uint32_t mem_cap;        /* the D27 bound to arm, only meaningful when load */
    const char *why;         /* refusal sentence, or NULL when load */
} orb_wasm_plan_t;

/** PURE. Given a slot header whose code triple the reader already validated, decide
 *  whether this firmware can host the module and, if so, the exact linear-memory cap
 *  to hand m3_SetResourceLimit. It re-checks only the two things the READER cannot
 *  know at parse time and the loader must not trust: that a code region is actually
 *  claimed (a FORMAT 2 header with behaviour_kind == WASM but zero code is a
 *  contradiction, not a load), and that the declared D27 bound is a positive number
 *  no larger than the firmware ceiling. It never touches the partition, the engine,
 *  or NVS. */
orb_wasm_plan_t orb_wasm_plan(const orbapp_hdr_t *h);

/* Per-slot instance table (indexed by flash_slot, the row's stable identity). These
 * run on the LVGL task only. */

/** Map slot `slot`'s code region (already read off the partition into `bytes`, which
 *  the caller keeps persistent for the runtime's lifetime) and bind its imports from
 *  `caps`. Returns ESP_OK only when the module parsed, its memory got a D27 cap at or
 *  below its declared bound, and its entry points resolved enough to run. On any
 *  failure the slot holds no engine state. `hdr` is the same header the caller read. */
esp_err_t orb_wasm_load(int slot, const orbapp_hdr_t *hdr,
                        const uint8_t *bytes, uint32_t len);

/** Why the last orb_wasm_load refused, in the owner's words ("" when it said nothing). */
const char *orb_wasm_load_why(void);

/** Release slot `slot`'s engine state, if any. Idempotent. */
void orb_wasm_unload(int slot);

/** True when slot `slot` currently holds a loaded runtime. */
bool orb_wasm_loaded(int slot);

/** §61: the icon `app` handed over with orb.icon (a deflated disc, `side` px), when it did. */
bool orb_wasm_icon(const orb_app_t *app, const uint8_t **z, uint32_t *n, uint16_t *side);

/* The compiled-in behaviour module's iface, registered under ORB_BEHAVIOUR_WASM in
 * orb_app.c's behaviour table (as a file-scope constant, so it is declared as data
 * here, not a function — a static initializer cannot call a getter). The registry
 * reaches it through orb_app_behaviour_iface(), never by reading this directly. */
extern const orb_app_iface_t orb_wasm_iface;

/* Point slot `slot`'s instance at its registry row, so a guest import reaches the
 * broker with its real caller. The callers are orb_app_install's load rung
 * (apply_code at boot), never the shim. */
void orb_wasm_bind_app(int slot, orb_app_t *app);

/** Bytes of the guest's shared native stack ever touched (§43), 0 before any call. */
uint32_t orb_wasm_stack_peak(void);
/** Deadlines the time budget forgave because a large transfer shared the call. */
uint32_t orb_wasm_budget_slips(void);

/** The beat landing for a resident code app: call its orb_tick(now_ms). Safe on an
 *  unloaded slot (no-op). The registry's tick path drives this on the LVGL task. */
void orb_wasm_tick(orb_app_t *app, uint32_t now_ms);

/* Running wall-time totals inside the guest, in us: orb_tick, orb_service, and every
 * other entry. Any out-param may be NULL; read as deltas. */
void orb_wasm_bench_us(uint64_t *tick, uint64_t *service, uint64_t *other);
uint32_t orb_wasm_bench_bg_max_us(bool reset);   /* longest non-tick guest call since the last reset */
/* The three heaviest app.entry pairs since the last call ("flights.tick 69ms/16, ..."). */
void orb_wasm_bench_top(char *out, size_t cap);

/* The OS wants PSRAM (the theme carousel, §61): for the next 2 s every app off the glass gives
 * back its kept bodies and map files on its service pass. Call it again to hold it. */
void orb_app_want_room(void);

#ifdef __cplusplus
}
#endif
