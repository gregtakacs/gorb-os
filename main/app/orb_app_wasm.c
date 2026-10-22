/**
 * orb_app_wasm.c — the wasm3 behaviour module (docs/app-platform-plan.md D3/D20/D27,
 * §4.1, §8 P7.2). The one compiled iface every code-bearing .orbapp shares, resolved
 * through behaviour_kind == ORB_BEHAVIOUR_WASM exactly as the picker resolves through
 * ORB_BEHAVIOUR_COLOR_PICKER (D1/C6, and the append-only rule keeps the id stable).
 *
 * Two directions cross the boundary, and both are the SAME contract a compiled app
 * already meets (D15/D20):
 *   OS -> guest : the registry calls this file's iface hooks; each finds the calling
 *                 app's own M3 runtime (keyed by flash_slot, the row's stable id —
 *                 never the registry index, which a card removal renumbers) and runs
 *                 that entry point IF the module exported it. A missing export is a
 *                 skipped hook, exactly as a NULL hook is for a compiled app.
 *   guest -> OS : the broker table, bound here as wasm imports. Each import resolves
 *                 the calling app back from the runtime's userdata and calls the
 *                 matching orb_svc_t entry WITH that app, so the broker's own gate
 *                 (orb_grant_ok, checked inside every entry, never here) sees the
 *                 real app and its real caps. The shim adds no authority of its own.
 *
 * What is provable WITHOUT the engine: orb_wasm_plan() and the entry table. Both are
 * lifted verbatim by tools/host_orbapp_wasm_test.py the way that suite lifts
 * install_decide — hand-authored header words, refusal sentences compared, the load
 * decision mutated. The m3 calls below are thin wrappers the sentinel cannot run and
 * does not claim to; they are watched by the compiler, by the size quote recorded in
 * the P7.2 commit (they must pull the engine into the image or the tier is a lie),
 * and by the trap path that bills the C2 counter (P7.3 LANDED with this file: the
 * trap site calls orb_svc_note_fault, and the second strike releases the engine —
 * after that init_all's existing C2 gate keeps the row off the dial until reinstall).
 */
#include "orb_app_wasm.h"

#include <stdio.h>            /* snprintf — the C2 fault reason is built, not literal */
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"      /* the guest image lives in PSRAM, not .bss — as the
                                 * settings scratch and art bank both do */
#include "esp_log.h"
#include "orb_app.h"            /* orb_app_t, orb_app_iface_t, ORB_APP_CAP, flash_slot */
#include "orb_svc.h"            /* orb_svc_table() — the imports forward to it */
#include "wasm3.h"

static const char *TAG = "appwasm";

/* One instance per flash slot that names the wasm tier. ORB_APP_CAP is the same 8 the
 * catalog caps installed apps at, so this array can never overflow the slots it maps. */
#define WASM_N_SLOTS  ORB_APP_CAP

/* Value-stack bytes m3 gives the guest before a deep recursion traps. Independent of
 * the D27 linear-memory cap: one bounds the call stack, the other the data. */
#ifndef ORB_WASM_STACK_BYTES
#define ORB_WASM_STACK_BYTES  8192u
#endif

/* ---- the entry-point vocabulary ------------------------------------------
 *
 * The fixed export names a guest uses to be called. They are the wasm spelling of the
 * orb_app_iface_t hooks, so "orb_tick" is answerable by the OS's tick path exactly as
 * color_picker's tick() is. Kept in one table so the sentinel can pin every hook the
 * shim may call and prove none is invented at a call site. */
enum { W_ACTIVATE = 0, W_TICK, W_SUSPEND, W_RESUME, W_INPUT, W_SERVICE, W_COUNT };

typedef struct {
    const char *export_name;   /* what the guest must export to be called */
    bool        required;      /* a module that omits this cannot LOAD */
} wasm_entry_t;

/* Only "orb_tick" is required: a code app that does nothing per beat is not an app.
 * Every other hook has a compiled-tier twin that is legitimately NULL for some apps
 * (a screen-less or input-less entry), so it is optional. */
static const wasm_entry_t s_entries[W_COUNT] = {
    [W_ACTIVATE] = { "orb_activate", false },
    [W_TICK]     = { "orb_tick",     true  },
    [W_SUSPEND]  = { "orb_suspend",  false },
    [W_RESUME]   = { "orb_resume",   false },
    [W_INPUT]    = { "orb_input",    false },
    [W_SERVICE]  = { "orb_service",  false },
};

/* ---- the pure load decision ----------------------------------------------
 *
 * No engine, no partition, no NVS. See the header's orb_wasm_plan contract; a change
 * here is tested by the sentinel, not on the dial. */
orb_wasm_plan_t orb_wasm_plan(const orbapp_hdr_t *h)
{
    orb_wasm_plan_t p = { .load = false, .mem_cap = 0, .why = NULL };

    /* A WASM-kind app that claims no code region is a contradiction the FORMAT 2
     * reader cannot see: it validates the code triple only when code_size is
     * non-zero. Here "there is a module" stops being assumed and becomes a fact, so
     * refuse the empty case rather than hand m3_ParseModule a null. */
    if (h->code_size == 0) {
        p.why = "the app names the wasm tier but carries no code — re-export it";
        return p;
    }
    /* D27, the runtime twin of the installer's Gate 1b: the manifest word is the
     * whole of the owner's consent to this much memory. It must be positive (a zero
     * bound allocates nothing, so the module cannot run) and within the firmware's
     * ceiling. The installer refused a larger word before the erase; the loader
     * re-checks because it is the last place the number is read before it becomes a
     * real allocation. */
    if (h->wasm_mem_max == 0 || h->wasm_mem_max > ORB_WASM_MEM_MAX) {
        p.why = "the app asks for more memory than this dial can give it";
        return p;
    }
    p.load = true;
    p.mem_cap = h->wasm_mem_max;
    return p;
}

/* ---- per-slot engine state ------------------------------------------------ */

typedef struct {
    IM3Environment env;
    IM3Runtime     rt;
    IM3Module      mod;          /* non-NULL only between Parse and Load; after Load
                                  * the runtime owns it (wasm3.h:378) so it is NULLed */
    uint8_t       *code;         /* PSRAM copy of the image; ours to free, see below */
    IM3Function    fn[W_COUNT];  /* resolved exports; NULL where the module omits one */
    orb_app_t     *app;          /* the row this instance answers to */
    bool           armed;        /* the iface's armed flag (no guest hook owns it) */
} wasm_slot_t;

static wasm_slot_t s_slot[WASM_N_SLOTS];

/* m3 hands a raw import the runtime, not the app. This finds the slot a runtime came
 * from so the import can forward to the broker with its real caller. Linear over 8 —
 * no hash is worth 8 entries. */
static wasm_slot_t *slot_of_runtime(IM3Runtime rt)
{
    for (int i = 0; i < WASM_N_SLOTS; i++)
        if (s_slot[i].rt == rt) return &s_slot[i];
    return NULL;
}

/* Release one slot's engine state. m3's teardown order: the runtime owns the module
 * once LoadModule took it, so FreeRuntime frees the module — only a never-loaded
 * module needs m3_FreeModule, and the failure paths below do that by hand. Idempotent
 * and zeroes the row, so a double unload is harmless. */
static void slot_release(wasm_slot_t *s)
{
    if (s->rt)  m3_FreeRuntime(s->rt);
    if (s->env) m3_FreeEnvironment(s->env);
    if (s->code) heap_caps_free(s->code);
    memset(s, 0, sizeof *s);
}

/* ---- one broker import, the guest -> OS direction ------------------------
 *
 * The full surface an app may call is orb_svc_t; binding every entry is the same
 * mechanical work and is NOT done in P7.2 — the sentinel runs a module with zero
 * imports, and the P8 commit hook needs only the OS->guest tick direction. Binding one
 * honest import proves the whole mechanism (resolve caller -> call broker entry with
 * it -> gate applies): a guest calling "orb.now" reaches orb_svc_t.now with the
 * guest's own app, so an app that never declared CAP_TIME is refused by the broker
 * exactly as a compiled app calling SVC(app, TIME) would be. More imports append to
 * s_binds as real apps need them, never pre-bound on speculation. */
m3ApiRawFunction(imp_now)                    /* (i32,i32,i32) writes sec/frac/synced */
{
    wasm_slot_t *s = slot_of_runtime(runtime);
    if (!s) m3ApiTrap(m3Err_trapUnreachable);       /* runtime not ours: refuse outright */
    time_t sec = 0; double frac = 0.0; bool synced = false;
    orb_svc_table()->now(s->app, &sec, &frac, &synced);
    m3ApiGetArg(int32_t *, p_sec);
    m3ApiGetArg(int32_t *, p_frac_num);        /* frac*1e9, kept integer across the ABI */
    m3ApiGetArg(int32_t *, p_sync);
    m3ApiCheckMem(p_sec, 4);
    m3ApiCheckMem(p_frac_num, 4);
    m3ApiCheckMem(p_sync, 4);
    *p_sec = (int32_t)sec;
    *p_frac_num = (int32_t)(frac * 1e9);
    *p_sync = synced ? 1 : 0;
    m3ApiSuccess();
}

/* Name, signature, host fn. The signature is wasm3's notation: params then a '_'
 * separator then results, so "iii_" is three i32 args and nothing returned. */
typedef struct {
    const char *module;
    const char *name;
    const char *sig;
    M3RawCall   fn;
} wasm_bind_t;

static const wasm_bind_t s_binds[] = {
    { "orb", "now", "iii_", imp_now },
};

/* Apply every import the module actually declared. m3_LinkRawFunction on a name the
 * module does not import returns m3Err_functionLookupFailed — expected, not a failure:
 * an app that never asked for "orb.now" must not fail to load over it. Only a real
 * link error (moduleNotLinked, an import-type mismatch, say) is worth surfacing. */
static M3Result bind_imports(IM3Module mod)
{
    for (size_t i = 0; i < sizeof s_binds / sizeof s_binds[0]; i++) {
        M3Result r = m3_LinkRawFunction(mod, s_binds[i].module, s_binds[i].name,
                                        s_binds[i].sig, s_binds[i].fn);
        if (r && r != m3Err_functionLookupFailed) return r;
    }
    return m3Err_none;
}

/* ---- OS -> guest: run one export if the module has it -------------------- */

static wasm_slot_t *slot_of_app(const orb_app_t *app)
{
    if (!app || app->flash_slot < 0 || app->flash_slot >= WASM_N_SLOTS)
        return NULL;
    return &s_slot[app->flash_slot];
}

/* Call one resolved export. A missing export is a no-op true (the compiled tier's
 * "NULL hook is skipped" rule). A trap bills C2 and reports false — D27: "a trapping
 * module bills a C2 fault, so two strikes mark the app failed and the dial boots to
 * the clock." The row's own app is the caller, so the broker's fault record fills
 * exactly as a compiled app's would; the reason fits the broker's 48-byte field.
 * Once the second strike lands (orb_svc_faulted true) the engine state is released
 * here, so a module that would trap on every beat cannot bill an NVS write per beat:
 * the strikes stay the two the owner consented to, and the next boot's init_all
 * refuses the row (C2's boot-into-clock rule, which needs no wasm-specific path). */
static bool wasm_call(wasm_slot_t *s, int entry, const void *argv[], uint32_t argc)
{
    IM3Function f = s->fn[entry];
    if (!f) return true;
    M3Result r = m3_Call(f, argc, argv);
    if (r) {
        M3ErrorInfo info;
        m3_GetErrorInfo(s->rt, &info);
        char why[48];
        snprintf(why, sizeof why, "wasm trapped in %s", s_entries[entry].export_name);
        ESP_LOGE(TAG, "\"%s\" %s: %s%s%s", s->app ? s->app->slug : "?", why, r,
                 info.message ? " (" : "", info.message ? info.message : "");
        m3_ResetErrorInfo(s->rt);
        orb_svc_note_fault(s->app, why);
        if (s->app && orb_svc_faulted(s->app)) {
            ESP_LOGE(TAG, "\"%s\" failed twice — engine state released until reinstall",
                     s->app->slug);
            orb_wasm_unload((int)(s - s_slot));   /* s points into s_slot; the index is its own */
        }
        return false;
    }
    return true;
}

/* ---- the iface every wasm app shares -------------------------------------
 *
 * No per-app widgets: a code app composes through the layer table (CAP_UI), so there
 * is no screen to reserve. screen() answers NULL; the code app's layer output lands in
 * the shared container through the compositor (that path is the compositor's, not this
 * tier's, and its per-element repaint is P8's). */
static esp_err_t wasm_init(orb_app_t *app)
{
    wasm_slot_t *s = slot_of_app(app);
    return (s && s->rt) ? ESP_OK : ESP_FAIL;   /* never loaded: nothing to build */
}

static esp_err_t wasm_activate(orb_app_t *app)
{
    wasm_slot_t *s = slot_of_app(app);
    if (!s || !s->rt) return ESP_FAIL;
    return wasm_call(s, W_ACTIVATE, NULL, 0) ? ESP_OK : ESP_FAIL;
}

static struct _lv_obj_t *wasm_screen(orb_app_t *app) { (void)app; return NULL; }

static void wasm_suspend(orb_app_t *app)
{
    wasm_slot_t *s = slot_of_app(app);
    if (s && s->rt) (void)wasm_call(s, W_SUSPEND, NULL, 0);
}

static void wasm_resume(orb_app_t *app)
{
    wasm_slot_t *s = slot_of_app(app);
    if (s && s->rt) (void)wasm_call(s, W_RESUME, NULL, 0);
}

static void wasm_input(orb_app_t *app, int32_t delta, bool pressed)
{
    wasm_slot_t *s = slot_of_app(app);
    if (s && s->rt) {
        uint32_t a[2] = { (uint32_t)delta, pressed ? 1u : 0u };
        const void *argv[2] = { &a[0], &a[1] };
        (void)wasm_call(s, W_INPUT, argv, 2);
    }
}

static bool wasm_armed(const orb_app_t *app)
{
    wasm_slot_t *s = slot_of_app(app);
    return s ? s->armed : false;
}

static void wasm_set_armed(orb_app_t *app, bool armed)
{
    wasm_slot_t *s = slot_of_app(app);
    if (s) s->armed = armed;
}

static void wasm_service(orb_app_t *app)
{
    wasm_slot_t *s = slot_of_app(app);
    if (s && s->rt) (void)wasm_call(s, W_SERVICE, NULL, 0);
}

/* The beat landing. The registry's tick path (D16/P8) calls this for the visible app
 * each beat with now_ms as the guest's only argument. */
void orb_wasm_tick(orb_app_t *app, uint32_t now_ms)
{
    wasm_slot_t *s = slot_of_app(app);
    if (!s || !s->rt) return;
    uint32_t a = now_ms;
    const void *argv[1] = { &a };
    (void)wasm_call(s, W_TICK, argv, 1);
}

/* Named (not static) and `extern`-declared in the header so orb_app.c's behaviour
 * table can reference it in a file-scope initializer. It is read-only data, and the
 * registry reaches it through orb_app_behaviour_iface() by kind, never by this name. */
const orb_app_iface_t orb_wasm_iface = {
    .init      = wasm_init,
    .activate  = wasm_activate,
    .screen    = wasm_screen,
    .suspend   = wasm_suspend,
    .resume    = wasm_resume,
    .input     = wasm_input,
    .armed     = wasm_armed,
    .set_armed = wasm_set_armed,
    .service   = wasm_service,
};

/* ---- lifecycle ----------------------------------------------------------- */

esp_err_t orb_wasm_load(int slot, const orbapp_hdr_t *hdr,
                        const uint8_t *bytes, uint32_t len)
{
    if (slot < 0 || slot >= WASM_N_SLOTS || !hdr || !bytes || !len)
        return ESP_ERR_INVALID_ARG;
    if (s_slot[slot].rt) return ESP_ERR_INVALID_STATE;   /* already resident */

    orb_wasm_plan_t plan = orb_wasm_plan(hdr);
    if (!plan.load) {
        ESP_LOGW(TAG, "slot %d refused: %s", slot, plan.why);
        return ESP_ERR_INVALID_SIZE;
    }

    wasm_slot_t *s = &s_slot[slot];

    /* m3 parses lazily and keeps a pointer into the image, so the bytes must outlive
     * the module — copy into PSRAM we own and free at slot_release, the same reason the
     * art bank and settings scratch are heap and not stack. */
    s->code = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s->code) return ESP_ERR_NO_MEM;
    memcpy(s->code, bytes, len);

    s->env = m3_NewEnvironment();
    if (!s->env) { slot_release(s); return ESP_ERR_NO_MEM; }

    M3Result r = m3_ParseModule(s->env, &s->mod, s->code, len);
    if (r) {
        ESP_LOGW(TAG, "slot %d: module would not parse: %s", slot, r);
        m3_FreeEnvironment(s->env); s->env = NULL; s->mod = NULL;
        slot_release(s); return ESP_FAIL;
    }

    s->rt = m3_NewRuntime(s->env, ORB_WASM_STACK_BYTES, s);   /* userdata = this slot */
    if (!s->rt) { m3_FreeModule(s->mod); s->mod = NULL; slot_release(s); return ESP_ERR_NO_MEM; }

    /* D27 at the engine: arm the ceiling BEFORE LoadModule instantiates memory, so a
     * module that would grow past what the manifest declared (and past what this dial
     * allows) fails to load rather than allocating past the owner's consent. */
    if (m3_SetResourceLimit(s->rt, c_m3Limit_MemoryBytes, plan.mem_cap)) {
        slot_release(s); return ESP_ERR_INVALID_SIZE;
    }

    M3Result br = bind_imports(s->mod);        /* bind before Load: Load resolves imports */
    if (br) { ESP_LOGW(TAG, "slot %d: import bind failed: %s", slot, br); slot_release(s); return ESP_FAIL; }

    r = m3_LoadModule(s->rt, s->mod);          /* takes s->mod either way */
    if (r) { ESP_LOGW(TAG, "slot %d: module would not load: %s", slot, r); slot_release(s); return ESP_FAIL; }
    s->mod = NULL;                             /* now owned by the runtime */

    /* Resolve the entry points. A missing required export (the tick) or a lookup
     * failure on a required one releases the whole instance: a half-resolved module
     * misbehaves on the first beat, which is worse than one that never ran. */
    for (int i = 0; i < W_COUNT; i++) {
        IM3Function f = NULL;
        M3Result fr = m3_FindFunction(&f, s->rt, s_entries[i].export_name);
        if (fr || !f) {
            if (s_entries[i].required) {
                ESP_LOGW(TAG, "slot %d: required export \"%s\" missing",
                         slot, s_entries[i].export_name);
                slot_release(s); return ESP_FAIL;
            }
            continue;
        }
        s->fn[i] = f;
    }
    ESP_LOGI(TAG, "slot %d: wasm module resident (%u B code, cap %u B mem)",
             slot, (unsigned)len, (unsigned)plan.mem_cap);
    return ESP_OK;
}

void orb_wasm_unload(int slot)
{
    if (slot >= 0 && slot < WASM_N_SLOTS) slot_release(&s_slot[slot]);
}

bool orb_wasm_loaded(int slot)
{
    return slot >= 0 && slot < WASM_N_SLOTS && s_slot[slot].rt != NULL;
}

/* The registry sets the row this instance answers to, so an import can hand the broker
 * its caller. orb_app_install's load rung (P7.4) calls this right after every
 * orb_wasm_load that succeeds — at boot and behind the enable door. */
void orb_wasm_bind_app(int slot, orb_app_t *app)
{
    if (slot >= 0 && slot < WASM_N_SLOTS) s_slot[slot].app = app;
}
