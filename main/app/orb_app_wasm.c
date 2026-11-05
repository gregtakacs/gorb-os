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
#include "esp_attr.h"        /* RTC_NOINIT_ATTR: the slip count survives a soft reset */
#include "orb_io_window.h"   /* the time budget forgives a deadline lost to I/O */

#include <math.h>             /* the orb.sin/cos/... imports are the OS's libm */
#include <stdio.h>            /* snprintf — the C2 fault reason is built, not literal */
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"      /* the guest image lives in PSRAM, not .bss — as the
                                 * settings scratch and art bank both do */
#include "esp_log.h"
#include "orb_app.h"            /* orb_app_t, orb_app_iface_t, ORB_APP_CAP, flash_slot */
#include "orb_svc.h"            /* orb_svc_table() — the imports forward to it */
#include "orb_ui.h"             /* the widget and canvas service behind ui_* / cv_* */
#include "orb_app_net.h"        /* the HTTPS service and credential store behind net_* */
#include "orb_stage.h"         /* the stage behind stage_* (D50) */
#include "orb_sound.h"          /* the owner's sound settings, the voices the stage leases */
#include "orb_clock3.h"         /* clk_sound_t: the theme records the stage hands over */
#include "orb_theme.h"          /* orb_theme_live_slug: whose theme the stage draws */
#include "orb_sched.h"          /* orb_sched_burst, behind orb.burst */
#include "orb_home.h"           /* orb_home_gen, behind orb.home_gen */
#include "esp_timer.h"
#include "esp_expression_with_stack.h"   /* the guest's own native stack, below */
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "orb_jobs.h"           /* settings writes leave the guest's stack for the worker */
#include "lvgl.h"               /* the beat timer; LVGL task only, like every hook */
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
 * The fixed export names a guest uses to be called (apps/sdk/orb.h lists them for the
 * app's author). They are the wasm spelling of the orb_app_iface_t hooks, kept in one
 * table so the sentinel can pin every hook the shim may call and prove none is
 * invented at a call site. */
enum {
    W_INIT = 0, W_ACTIVATE, W_TICK, W_SUSPEND, W_RESUME, W_INPUT, W_SERVICE,
    W_SHOW, W_SET_ARMED,
    W_OPT_COUNT, W_OPT_KIND, W_OPT_LABEL, W_OPT_VALUE, W_OPT_INC, W_OPT_SEL, W_OPT_ARM,
    W_COUNT
};

typedef struct {
    const char *export_name;   /* what the guest must export to be called */
    bool        required;      /* a module that omits this cannot LOAD */
} wasm_entry_t;

/* Only "orb_init" is required: a module that never builds itself is not an app. Every
 * other hook has a compiled-tier twin that is legitimately NULL for some apps (a
 * beat-less editor, a screen-less service), so it is optional. */
static const wasm_entry_t s_entries[W_COUNT] = {
    [W_INIT]      = { "orb_init",      true  },
    [W_ACTIVATE]  = { "orb_activate",  false },
    [W_TICK]      = { "orb_tick",      false },
    [W_SUSPEND]   = { "orb_suspend",   false },
    [W_RESUME]    = { "orb_resume",    false },
    [W_INPUT]     = { "orb_input",     false },
    [W_SERVICE]   = { "orb_service",   false },
    [W_SHOW]      = { "orb_show",      false },
    [W_SET_ARMED] = { "orb_set_armed", false },
    [W_OPT_COUNT] = { "orb_opt_count", false },
    [W_OPT_KIND]  = { "orb_opt_kind",  false },
    [W_OPT_LABEL] = { "orb_opt_label", false },
    [W_OPT_VALUE] = { "orb_opt_value", false },
    [W_OPT_INC]   = { "orb_opt_inc",   false },
    [W_OPT_SEL]   = { "orb_opt_sel",   false },
    [W_OPT_ARM]   = { "orb_opt_arm",   false },
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

#define REPLY_MAX 64             /* one option row's label or value, the menu's width */

typedef struct {
    IM3Environment env;
    IM3Runtime     rt;
    IM3Module      mod;          /* non-NULL only between Parse and Load; after Load
                                  * the runtime owns it (wasm3.h:378) so it is NULLed */
    uint8_t       *code;         /* PSRAM copy of the image; ours to free, see below */
    IM3Function    fn[W_COUNT];  /* resolved exports; NULL where the module omits one */
    orb_app_t     *app;          /* the row this instance answers to */
    bool           armed;        /* the armed state the guest last answered */
    bool           on_glass;     /* between activate/resume and suspend */
    uint32_t       beat_ms;      /* the beat the broker granted, 0 = none */
    lv_timer_t    *beat;         /* runs orb_tick while on the glass */
    char          *reply;        /* where orb_reply lands during an opt call */
    char           label[REPLY_MAX], value[REPLY_MAX];
    struct cfg_ent *cfg;         /* the settings mirror, one per declared setting */
    uint32_t       cfg_seen;     /* the broker's cfg_gen the mirror was read at */
    uint32_t       cfg_local;    /* moves on every mirror change: orb.cfg_gen */
    uint32_t       cfg_pending;  /* writes posted to the worker, not yet landed */
    bool           cfg_stale;    /* a posted write was refused: re-read at next call */
    bool           stage;        /* the live theme is this app's: it owns the stage */
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

static int slot_index(const wasm_slot_t *s) { return (int)(s - s_slot); }

/* Release one slot's engine state. m3's teardown order: the runtime owns the module
 * once LoadModule took it, so FreeRuntime frees the module — only a never-loaded
 * module needs m3_FreeModule, and the failure paths below do that by hand. The app's
 * page and its beat go with it. Idempotent and zeroes the row, so a double unload is
 * harmless. */
static void slot_release(wasm_slot_t *s)
{
    if (s->beat) lv_timer_delete(s->beat);
    orb_ui_release(slot_index(s));
    orb_app_net_release(slot_index(s));
    if (s->rt) m3_FreeRuntime(s->rt);
    if (s->env) m3_FreeEnvironment(s->env);
    if (s->code) heap_caps_free(s->code);
    if (s->cfg) heap_caps_free(s->cfg);
    memset(s, 0, sizeof *s);
}

/* ---- the guest's native stack ----------------------------------------------
 *
 * wasm3 is a threaded interpreter that leans on tail calls, and the Xtensa windowed
 * ABI GCC builds this firmware with cannot make one: every op a guest executes is a
 * native frame until its block returns. The picker's orb_init alone ran the LVGL
 * task's 8 KB stack into the heap on the bench (2026-11-01: a LoadProhibited inside
 * heap_caps_malloc, called from m3's lazy compile, under ~60 nested op frames). So
 * every guest call runs on one shared stack of its own, sized for the interpreter and
 * placed in PSRAM, where 64 KB costs nothing the dial can feel; internal RAM has no
 * such block to give.
 *
 * A PSRAM stack carries one rule: no flash operation may run on it, because a flash
 * write or read disables the cache that PSRAM is reached through. That is why the
 * settings imports answer from a RAM mirror and post their writes to the jobs worker
 * (below), and why a trap's C2 bill — an NVS write — is paid after the call returns to
 * the task's own stack, never inside it. Every other import is RAM-only. */
#ifndef ORB_WASM_NATIVE_STACK
#define ORB_WASM_NATIVE_STACK (64u * 1024u)
#endif
#define XSTACK_FILL 0xA5u

static uint8_t          *s_xstack;
static SemaphoreHandle_t s_xlock;
static bool              s_on_xstack;
static struct {
    IM3Function  f;
    uint32_t     argc;
    const void **argv;
    M3Result     r;
} s_xcall;

static void xcall_fn(void)
{
    s_xcall.r = m3_Call(s_xcall.f, s_xcall.argc, s_xcall.argv);
}

/* ---- the time budget (§43.4) ----------------------------------------------------
 * A guest runs on the LVGL task, so one that loops forever is a frozen dial. wasm3 has
 * no instruction budget of its own; it calls the weak m3_Yield() on every call and —
 * by the recorded addition in components/wasm3 — on every loop iteration, and this
 * file's strong m3_Yield answers a trap once the call's deadline passed. The deadline
 * is an esp_timer that only sets a flag, so the check on the hot path is one load. The
 * trap bills C2 exactly like any other: a second overrun quarantines the app.
 *
 * Wall time is only the guest's own when nothing heavy shares the board. A deadline
 * that lands while a large transfer runs, or after one started during the call
 * (orb_io_window.h), re-arms for another slice instead of trapping, up to
 * ORB_WASM_IO_SLIP_MS of extra time per call: a guest starved by a 3 MB upload is
 * not a runaway, and one that really spins still traps once the slip is spent. */
#ifndef ORB_WASM_INIT_BUDGET_MS
#define ORB_WASM_INIT_BUDGET_MS 1000u   /* orb_init: builds the page, compiles on first use */
#endif
#ifndef ORB_WASM_CALL_BUDGET_MS
#define ORB_WASM_CALL_BUDGET_MS 250u    /* every other entry; the shipped apps use < 40 ms */
#endif
#ifndef ORB_WASM_IO_SLIP_MS
#define ORB_WASM_IO_SLIP_MS     2000u   /* extra time one call may get while I/O runs */
#endif

static const char s_overrun_trap[] = "[trap] the app ran past its time budget";
static esp_timer_handle_t s_budget;
static volatile bool      s_overrun;
static uint32_t           s_budget_ms = ORB_WASM_CALL_BUDGET_MS;
static uint32_t           s_slip_ms;      /* extra time this call has had      */
static uint32_t           s_call_gen;     /* the I/O window's gen it last saw  */
/* Deadlines forgiven since power-on. RTC memory survives the software reset a theme
 * store ends in, which is the transfer most likely to cost a guest its deadline, so a
 * bench can read the count after the reboot (the magic says the word is ours). */
#define SLIPS_MAGIC 0x51195EEDu
static RTC_NOINIT_ATTR uint32_t s_slips_magic;
static RTC_NOINIT_ATTR uint32_t s_slips;

/* The deadline. It is an esp_timer callback, so it may re-arm its own timer. */
static void budget_fire(void *arg)
{
    (void)arg;
    const uint32_t gen = orb_io_heavy_gen();
    const bool io = orb_io_heavy_now() || gen != s_call_gen;
    if (io && s_slip_ms + s_budget_ms <= ORB_WASM_IO_SLIP_MS) {
        s_slip_ms += s_budget_ms;
        s_call_gen = gen;                 /* the next slice needs I/O of its own */
        if (s_slips_magic != SLIPS_MAGIC) { s_slips_magic = SLIPS_MAGIC; s_slips = 0; }
        s_slips++;
        esp_timer_start_once(s_budget, (uint64_t)s_budget_ms * 1000u);
        return;
    }
    s_overrun = true;
}

uint32_t orb_wasm_budget_slips(void)
{
    return s_slips_magic == SLIPS_MAGIC ? s_slips : 0;
}

M3Result m3_Yield(void)
{
    return s_overrun ? s_overrun_trap : m3Err_none;
}

static M3Result guest_call(IM3Function f, uint32_t argc, const void *argv[])
{
    if (s_on_xstack) return m3_Call(f, argc, argv);   /* nested: already on it */
    if (!s_budget) {
        const esp_timer_create_args_t a = { .callback = budget_fire, .name = "wasmbudget" };
        if (esp_timer_create(&a, &s_budget) != ESP_OK) s_budget = NULL;
    }
    if (!s_xstack) {
        s_xlock = xSemaphoreCreateMutex();
        s_xstack = heap_caps_malloc(ORB_WASM_NATIVE_STACK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_xstack || !s_xlock) return "no native stack for the guest";
        memset(s_xstack, XSTACK_FILL, ORB_WASM_NATIVE_STACK);
    }
    s_xcall.f = f;
    s_xcall.argc = argc;
    s_xcall.argv = argv;
    s_overrun = false;
    s_slip_ms = 0;
    s_call_gen = orb_io_heavy_gen();
    if (s_budget) esp_timer_start_once(s_budget, (uint64_t)s_budget_ms * 1000u);
    s_on_xstack = true;
    esp_execute_shared_stack_function(s_xlock, s_xstack, ORB_WASM_NATIVE_STACK, xcall_fn);
    s_on_xstack = false;
    if (s_budget) esp_timer_stop(s_budget);
    s_overrun = false;
    return s_xcall.r;
}

/* Bytes of the guest stack ever touched (it grows down from the top). */
uint32_t orb_wasm_stack_peak(void)
{
    if (!s_xstack) return 0;
    uint32_t i = 0;
    while (i < ORB_WASM_NATIVE_STACK && s_xstack[i] == XSTACK_FILL) i++;
    return ORB_WASM_NATIVE_STACK - i;
}

/* ---- the settings mirror -----------------------------------------------------
 *
 * One entry per declared setting, read through the broker's own cfg entries on the
 * task's stack (mirror_load runs before a guest call, never inside one). A guest read
 * answers from here; a guest write is validated here against the same descriptor the
 * broker checks, lands in the mirror at once, and is persisted by the broker's
 * cfg_set on the jobs worker. A change from outside (the LAN door, the dial's options
 * page) moves the broker's cfg_gen, and the next call re-reads — but not while one of
 * the app's own writes is still in flight, or the re-read would briefly hand it back
 * the value it just replaced. */
typedef struct cfg_ent {
    int32_t v;
    char    str[32];
} cfg_ent_t;

static void mirror_load(wasm_slot_t *s)
{
    orb_app_t *a = s->app;
    if (!a || a->n_settings <= 0 || !a->settings) return;
    if (!s->cfg) {
        s->cfg = heap_caps_calloc((size_t)a->n_settings, sizeof *s->cfg, MALLOC_CAP_SPIRAM);
        if (!s->cfg) return;
    }
    const orb_svc_t *svc = orb_svc_table();
    s->cfg_seen = orb_svc_cfg_gen(a);
    for (int i = 0; i < a->n_settings; i++) {
        const orb_setting_desc_t *d = &a->settings[i];
        cfg_ent_t *e = &s->cfg[i];
        if (d->type == ORB_SET_SECRET) continue;   /* §43: never in the app's reach */
        if (d->type == ORB_SET_STRING) {
            if (svc->cfg_get_str(a, d->id, e->str, sizeof e->str) != ESP_OK)
                snprintf(e->str, sizeof e->str, "%s", d->def_str);
        } else if (svc->cfg_get(a, d->id, &e->v) != ESP_OK) {
            e->v = d->def;
        }
    }
    s->cfg_stale = false;
    s->cfg_local++;
}

static void mirror_sync(wasm_slot_t *s)
{
    if (s->app) orb_svc_grants_warm(s->app);   /* the grant word is NVS too */
    if (!s->app || __atomic_load_n(&s->cfg_pending, __ATOMIC_ACQUIRE)) return;
    if (!s->cfg || s->cfg_stale || orb_svc_cfg_gen(s->app) != s->cfg_seen) mirror_load(s);
}

static int mirror_find(const wasm_slot_t *s, const char *id, bool want_str)
{
    const orb_app_t *a = s->app;
    if (!s->cfg || !a || !id) return -1;
    for (int i = 0; i < a->n_settings; i++)
        if (!strncmp(a->settings[i].id, id, sizeof a->settings[i].id) &&
            a->settings[i].type != ORB_SET_SECRET &&
            (a->settings[i].type == ORB_SET_STRING) == want_str)
            return i;
    return -1;
}

typedef struct {
    wasm_slot_t *s;
    int          idx;
    bool         is_str;
    int32_t      v;
    char         str[32];
} cfg_job_t;

static void cfg_job(void *arg)
{
    const cfg_job_t *j = arg;
    wasm_slot_t *s = j->s;
    if (s->app && j->idx < s->app->n_settings) {
        const char *id = s->app->settings[j->idx].id;
        const esp_err_t e = j->is_str ? orb_svc_table()->cfg_set_str(s->app, id, j->str)
                                      : orb_svc_table()->cfg_set(s->app, id, j->v);
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "\"%s\" setting \"%s\" was not stored: %s",
                     s->app->slug, id, esp_err_to_name(e));
            s->cfg_stale = true;
        }
    }
    __atomic_sub_fetch(&s->cfg_pending, 1, __ATOMIC_RELEASE);
}

static esp_err_t mirror_post(wasm_slot_t *s, int idx, bool is_str, int32_t v, const char *str)
{
    cfg_job_t j = { .s = s, .idx = idx, .is_str = is_str, .v = v };
    if (is_str) snprintf(j.str, sizeof j.str, "%s", str);
    __atomic_add_fetch(&s->cfg_pending, 1, __ATOMIC_ACQ_REL);
    const esp_err_t e = orb_jobs_post(cfg_job, &j, sizeof j);
    if (e != ESP_OK) {
        __atomic_sub_fetch(&s->cfg_pending, 1, __ATOMIC_RELEASE);
        s->cfg_stale = true;
    }
    return e;
}

/* ---- the broker, guest -> OS ---------------------------------------------
 *
 * Every import resolves its caller from the runtime and forwards to the broker or to
 * an OS service WITH that caller, so the broker's own gate (orb_grant_ok, inside every
 * broker entry) sees the real app and its real caps: the shim adds no authority of its
 * own. Services the broker table does not carry (widgets, canvas, logging, maths) are
 * gated here by the same orb_grant_ok, against the cap orb_svc.h files them under.
 *
 * Guest pointers are offsets into the module's own linear memory. m3ApiCheckMem traps
 * an access that leaves it, and a trap is a C2 strike against the app, never a read of
 * OS memory. Strings must be NUL-terminated inside that memory and inside GSTR_MAX. */

#define GSTR_MAX 512

#define CALLER()                                                         \
    wasm_slot_t *s = slot_of_runtime(runtime);                           \
    if (!s || !s->app) m3ApiTrap(m3Err_trapUnreachable)

/* A guest string, or NULL for a NULL pointer; traps past the module's memory. */
#define GET_STR(name)                                                    \
    m3ApiGetArg(uint32_t, name##_off);                                   \
    const char *name = NULL;                                             \
    if (name##_off) {                                                    \
        const size_t mem_n = m3_GetMemorySizeAt(_mem);                   \
        if (name##_off >= mem_n) m3ApiTrap(m3Err_trapOutOfBoundsMemoryAccess); \
        const size_t lim = mem_n - name##_off < GSTR_MAX ? mem_n - name##_off : GSTR_MAX; \
        name = (const char *)m3ApiOffsetToPtr(name##_off);               \
        if (!memchr(name, 0, lim)) m3ApiTrap(m3Err_trapOutOfBoundsMemoryAccess); \
    }

/* time */

m3ApiRawFunction(imp_now)                    /* i32 now(i64 *sec, i32 *frac_ns) */
{
    m3ApiReturnType(int32_t);
    CALLER();
    m3ApiGetArgMem(int64_t *, p_sec);
    m3ApiGetArgMem(int32_t *, p_frac);
    m3ApiCheckMem(p_sec, 8);
    m3ApiCheckMem(p_frac, 4);
    time_t sec = 0; double frac = 0.0; bool synced = false;
    orb_svc_table()->now(s->app, &sec, &frac, &synced);
    m3ApiWriteMem64(p_sec, (int64_t)sec);
    m3ApiWriteMem32(p_frac, (int32_t)(frac * 1e9));
    m3ApiReturn(synced ? 1 : 0);
}

m3ApiRawFunction(imp_uptime_ms)
{
    m3ApiReturnType(uint32_t);
    CALLER();
    m3ApiReturn((uint32_t)(esp_timer_get_time() / 1000));
}

m3ApiRawFunction(imp_uptime_us)
{
    m3ApiReturnType(int64_t);
    CALLER();
    m3ApiReturn(esp_timer_get_time());
}

m3ApiRawFunction(imp_localtime)              /* i32 localtime(i64 t, orb_tm_t *out) */
{
    m3ApiReturnType(int32_t);
    CALLER();
    m3ApiGetArg(int64_t, t);
    m3ApiGetArgMem(int32_t *, out);
    m3ApiCheckMem(out, 9 * 4);
    if (!orb_grant_ok(s->app, CAP_TIME)) m3ApiReturn(ESP_ERR_NOT_ALLOWED);
    const time_t tt = (time_t)t;
    struct tm ti;
    if (!localtime_r(&tt, &ti)) m3ApiReturn(ESP_FAIL);
    const int32_t v[9] = { ti.tm_sec, ti.tm_min, ti.tm_hour, ti.tm_mday, ti.tm_mon,
                           ti.tm_year, ti.tm_wday, ti.tm_yday, ti.tm_isdst };
    for (int i = 0; i < 9; i++) m3ApiWriteMem32(&out[i], v[i]);
    m3ApiReturn(ESP_OK);
}

m3ApiRawFunction(imp_tz_offset)              /* i32 tz_offset(i64 t) -> minutes */
{
    m3ApiReturnType(int32_t);
    CALLER();
    m3ApiGetArg(int64_t, t);
    int32_t off = 0;
    (void)orb_svc_table()->tz_offset(s->app, (time_t)t, &off);
    m3ApiReturn(off);
}

/* logging */

m3ApiRawFunction(imp_log)                    /* void log(const char *line) */
{
    CALLER();
    GET_STR(line);
    if (line) ESP_LOGI(TAG, "%s: %s", s->app->slug ? s->app->slug : "?", line);
    m3ApiSuccess();
}

/* settings */

m3ApiRawFunction(imp_cfg_get)                /* i32 cfg_get(const char *id, i32 *out) */
{
    m3ApiReturnType(int32_t);
    CALLER();
    GET_STR(id);
    m3ApiGetArgMem(int32_t *, out);
    m3ApiCheckMem(out, 4);
    if (!orb_grant_ok(s->app, CAP_CONFIG)) m3ApiReturn(ESP_ERR_NOT_ALLOWED);
    const int i = mirror_find(s, id, false);
    if (i < 0) m3ApiReturn(ESP_ERR_NOT_FOUND);
    m3ApiWriteMem32(out, s->cfg[i].v);
    m3ApiReturn(ESP_OK);
}

m3ApiRawFunction(imp_cfg_set)                /* i32 cfg_set(const char *id, i32 v) */
{
    m3ApiReturnType(int32_t);
    CALLER();
    GET_STR(id);
    m3ApiGetArg(int32_t, v);
    if (!orb_grant_ok(s->app, CAP_CONFIG)) m3ApiReturn(ESP_ERR_NOT_ALLOWED);
    const int i = mirror_find(s, id, false);
    if (i < 0) m3ApiReturn(ESP_ERR_NOT_FOUND);
    const orb_setting_desc_t *d = &s->app->settings[i];
    if (d->type == ORB_SET_TOGGLE ? (v != 0 && v != 1) : (v < d->min || v > d->max))
        m3ApiReturn(ESP_ERR_INVALID_ARG);         /* the broker's own refusal, early */
    if (s->cfg[i].v == v) m3ApiReturn(ESP_OK);
    s->cfg[i].v = v;
    s->cfg_local++;
    m3ApiReturn(mirror_post(s, i, false, v, NULL));
}

m3ApiRawFunction(imp_cfg_get_str)            /* i32 cfg_get_str(id, char *buf, u32 cap) */
{
    m3ApiReturnType(int32_t);
    CALLER();
    GET_STR(id);
    m3ApiGetArgMem(char *, buf);
    m3ApiGetArg(uint32_t, cap);
    if (!cap) m3ApiReturn(ESP_ERR_INVALID_ARG);
    m3ApiCheckMem(buf, cap);
    if (!orb_grant_ok(s->app, CAP_CONFIG)) m3ApiReturn(ESP_ERR_NOT_ALLOWED);
    const int i = mirror_find(s, id, true);
    if (i < 0) m3ApiReturn(ESP_ERR_NOT_FOUND);
    if (strlen(s->cfg[i].str) >= cap) m3ApiReturn(ESP_ERR_INVALID_SIZE);
    memcpy(buf, s->cfg[i].str, strlen(s->cfg[i].str) + 1);
    m3ApiReturn(ESP_OK);
}

m3ApiRawFunction(imp_cfg_set_str)            /* i32 cfg_set_str(id, const char *v) */
{
    m3ApiReturnType(int32_t);
    CALLER();
    GET_STR(id);
    GET_STR(v);
    if (!v) m3ApiReturn(ESP_ERR_INVALID_ARG);
    if (!orb_grant_ok(s->app, CAP_CONFIG)) m3ApiReturn(ESP_ERR_NOT_ALLOWED);
    const int i = mirror_find(s, id, true);
    if (i < 0) m3ApiReturn(ESP_ERR_NOT_FOUND);
    if (strlen(v) >= sizeof s->cfg[i].str) m3ApiReturn(ESP_ERR_INVALID_SIZE);
    if (!strcmp(s->cfg[i].str, v)) m3ApiReturn(ESP_OK);
    snprintf(s->cfg[i].str, sizeof s->cfg[i].str, "%s", v);
    s->cfg_local++;
    m3ApiReturn(mirror_post(s, i, true, 0, v));
}

m3ApiRawFunction(imp_cfg_gen)
{
    m3ApiReturnType(uint32_t);
    CALLER();
    m3ApiReturn(s->cfg_local);
}

/* the beat */

static void beat_cb(lv_timer_t *t);

/* The broker owns the decision (the manifest floor, the UI cap); the shim owns the
 * timer that delivers it, armed only while the app is on the glass. */
static void beat_apply(wasm_slot_t *s)
{
    if (!s->beat_ms || !s->on_glass) {
        if (s->beat) lv_timer_pause(s->beat);
        return;
    }
    if (!s->beat) s->beat = lv_timer_create(beat_cb, s->beat_ms, s);
    if (!s->beat) return;
    lv_timer_set_period(s->beat, s->beat_ms);
    lv_timer_resume(s->beat);
}

m3ApiRawFunction(imp_set_beat)               /* i32 set_beat(u32 period_ms) */
{
    m3ApiReturnType(int32_t);
    CALLER();
    m3ApiGetArg(uint32_t, period);
    const esp_err_t e = orb_svc_table()->set_beat(s->app, period);
    if (e == ESP_OK) { s->beat_ms = period; beat_apply(s); }
    m3ApiReturn(e);
}

m3ApiRawFunction(imp_burst)                  /* void burst(u32 ms) */
{
    CALLER();
    m3ApiGetArg(uint32_t, ms);
    if (orb_grant_ok(s->app, CAP_UI) && s->on_glass) orb_sched_burst(ms > 5000 ? 5000 : ms);
    m3ApiSuccess();
}

/* widgets and canvas: CAP_UI, which is free and cannot be revoked, but is counted */

#define UI_GATE() if (!orb_grant_ok(s->app, CAP_UI)) m3ApiSuccess()

m3ApiRawFunction(imp_ui_create)              /* i32 ui_create(i32 kind, i32 parent) */
{
    m3ApiReturnType(int32_t);
    CALLER();
    m3ApiGetArg(int32_t, kind);
    m3ApiGetArg(int32_t, parent);
    if (!orb_grant_ok(s->app, CAP_UI)) m3ApiReturn(0);
    m3ApiReturn(orb_ui_create(slot_index(s), kind, parent));
}

m3ApiRawFunction(imp_ui_set)                 /* void ui_set(i32 h, i32 prop, i32 v) */
{
    CALLER();
    m3ApiGetArg(int32_t, h);
    m3ApiGetArg(int32_t, prop);
    m3ApiGetArg(int32_t, v);
    UI_GATE();
    orb_ui_set(slot_index(s), h, prop, v);
    m3ApiSuccess();
}

m3ApiRawFunction(imp_ui_text)                /* void ui_text(i32 h, const char *s) */
{
    CALLER();
    m3ApiGetArg(int32_t, h);
    GET_STR(text);
    UI_GATE();
    orb_ui_text(slot_index(s), h, text);
    m3ApiSuccess();
}

m3ApiRawFunction(imp_ui_align)               /* void ui_align(i32 h, i32 al, i32 x, i32 y) */
{
    CALLER();
    m3ApiGetArg(int32_t, h);
    m3ApiGetArg(int32_t, al);
    m3ApiGetArg(int32_t, x);
    m3ApiGetArg(int32_t, y);
    UI_GATE();
    orb_ui_align(slot_index(s), h, al, x, y);
    m3ApiSuccess();
}

m3ApiRawFunction(imp_ui_refresh)
{
    CALLER();
    UI_GATE();
    orb_ui_refresh(slot_index(s));
    m3ApiSuccess();
}

m3ApiRawFunction(imp_cv_create)              /* i32 cv_create(i32 parent, i32 w, i32 h) */
{
    m3ApiReturnType(int32_t);
    CALLER();
    m3ApiGetArg(int32_t, parent);
    m3ApiGetArg(int32_t, w);
    m3ApiGetArg(int32_t, h);
    if (!orb_grant_ok(s->app, CAP_UI)) m3ApiReturn(0);
    m3ApiReturn(orb_ui_cv_create(slot_index(s), parent, w, h));
}

m3ApiRawFunction(imp_cv_fill)                /* void cv_fill(i32 h, u32 rgb) */
{
    CALLER();
    m3ApiGetArg(int32_t, h);
    m3ApiGetArg(uint32_t, rgb);
    UI_GATE();
    orb_ui_cv_fill(slot_index(s), h, rgb);
    m3ApiSuccess();
}

m3ApiRawFunction(imp_cv_pixel)               /* void cv_pixel(i32 h, i32 x, i32 y, u32 rgb) */
{
    CALLER();
    m3ApiGetArg(int32_t, h);
    m3ApiGetArg(int32_t, x);
    m3ApiGetArg(int32_t, y);
    m3ApiGetArg(uint32_t, rgb);
    UI_GATE();
    orb_ui_cv_pixel(slot_index(s), h, x, y, rgb);
    m3ApiSuccess();
}

m3ApiRawFunction(imp_cv_ring)                /* void cv_ring(h, cx, cy, r, rgb) */
{
    CALLER();
    m3ApiGetArg(int32_t, h);
    m3ApiGetArg(int32_t, cx);
    m3ApiGetArg(int32_t, cy);
    m3ApiGetArg(int32_t, r);
    m3ApiGetArg(uint32_t, rgb);
    UI_GATE();
    orb_ui_cv_ring(slot_index(s), h, cx, cy, r, rgb);
    m3ApiSuccess();
}

m3ApiRawFunction(imp_cv_disc)                /* void cv_disc(h, cx, cy, r, rgb) */
{
    CALLER();
    m3ApiGetArg(int32_t, h);
    m3ApiGetArg(int32_t, cx);
    m3ApiGetArg(int32_t, cy);
    m3ApiGetArg(int32_t, r);
    m3ApiGetArg(uint32_t, rgb);
    UI_GATE();
    orb_ui_cv_disc(slot_index(s), h, cx, cy, r, rgb);
    m3ApiSuccess();
}

m3ApiRawFunction(imp_cv_line)                /* void cv_line(h, x0, y0, x1, y1, rgb) */
{
    CALLER();
    m3ApiGetArg(int32_t, h);
    m3ApiGetArg(int32_t, x0);
    m3ApiGetArg(int32_t, y0);
    m3ApiGetArg(int32_t, x1);
    m3ApiGetArg(int32_t, y1);
    m3ApiGetArg(uint32_t, rgb);
    UI_GATE();
    orb_ui_cv_line(slot_index(s), h, x0, y0, x1, y1, rgb);
    m3ApiSuccess();
}

m3ApiRawFunction(imp_cv_text)                /* void cv_text(h, x, y, w, font, rgb, s) */
{
    CALLER();
    m3ApiGetArg(int32_t, h);
    m3ApiGetArg(int32_t, x);
    m3ApiGetArg(int32_t, y);
    m3ApiGetArg(int32_t, w);
    m3ApiGetArg(int32_t, font);
    m3ApiGetArg(uint32_t, rgb);
    GET_STR(text);
    UI_GATE();
    orb_ui_cv_text(slot_index(s), h, x, y, w, font, rgb, text);
    m3ApiSuccess();
}

m3ApiRawFunction(imp_cv_flush)               /* void cv_flush(i32 h) */
{
    CALLER();
    m3ApiGetArg(int32_t, h);
    UI_GATE();
    orb_ui_cv_flush(slot_index(s), h);
    m3ApiSuccess();
}

m3ApiRawFunction(imp_armed)                  /* void armed(i32 armed) */
{
    CALLER();
    m3ApiGetArg(int32_t, armed);
    s->armed = armed != 0;
    m3ApiSuccess();
}

/* option pages: the answer to an opt_label/opt_value call, copied while it runs */

m3ApiRawFunction(imp_reply)                  /* void reply(const char *s) */
{
    CALLER();
    GET_STR(text);
    if (s->reply && text) {
        strncpy(s->reply, text, REPLY_MAX - 1);
        s->reply[REPLY_MAX - 1] = '\0';
    }
    m3ApiSuccess();
}

/* sound, location, power: the broker's own gates (CAP_SOUND, CAP_LOCATION, CAP_POWER) */

m3ApiRawFunction(imp_sound_play)             /* i32 sound_play(u32 slot, u32 flags, u32 delay) */
{
    m3ApiReturnType(int32_t);
    CALLER();
    m3ApiGetArg(uint32_t, slot);
    m3ApiGetArg(uint32_t, flags);
    m3ApiGetArg(uint32_t, delay);
    m3ApiReturn(orb_svc_table()->sound_play(s->app, (uint16_t)slot, flags, delay));
}

m3ApiRawFunction(imp_sound_stop)             /* void sound_stop(i32 h) */
{
    CALLER();
    m3ApiGetArg(int32_t, h);
    orb_svc_table()->sound_stop(s->app, h);
    m3ApiSuccess();
}

m3ApiRawFunction(imp_sound_active)           /* i32 sound_active(i32 h) */
{
    m3ApiReturnType(int32_t);
    CALLER();
    m3ApiGetArg(int32_t, h);
    m3ApiReturn(orb_svc_table()->sound_active(s->app, h) ? 1 : 0);
}

m3ApiRawFunction(imp_home)                   /* i32 home(orb_home_t *out) — 52 B */
{
    m3ApiReturnType(int32_t);
    CALLER();
    m3ApiGetArgMem(uint8_t *, out);
    m3ApiCheckMem(out, 52);
    orb_svc_home_t h;
    const esp_err_t e = orb_svc_table()->home(s->app, &h);
    if (e == ESP_OK) {
        m3ApiWriteMem32(out + 0, (uint32_t)h.lat_e6);
        m3ApiWriteMem32(out + 4, (uint32_t)h.lon_e6);
        m3ApiWriteMem32(out + 8, h.gen);
        memcpy(out + 12, h.place, 40);
        out[12 + 39] = 0;
    }
    m3ApiReturn(e);
}

m3ApiRawFunction(imp_home_gen)
{
    m3ApiReturnType(uint32_t);
    CALLER();
    m3ApiReturn(orb_home_gen());
}

m3ApiRawFunction(imp_power_request)          /* i32 power_request(u32 timeout_s, i32 hold) */
{
    m3ApiReturnType(int32_t);
    CALLER();
    m3ApiGetArg(uint32_t, timeout_s);
    m3ApiGetArg(int32_t, hold);
    m3ApiReturn(orb_svc_table()->power_request(s->app, timeout_s, hold != 0));
}

/* network: CAP_NET, the broker's own gate word (the broker's http_get entry is the
 * synchronous shape this asynchronous one replaces for a guest) */

#define NET_GATE(fail) if (!orb_grant_ok(s->app, CAP_NET)) m3ApiReturn(fail)

/* i32 net_request(u32 method, url, headers, body, u32 body_len, const char *cred) */
m3ApiRawFunction(imp_net_request)
{
    m3ApiReturnType(int32_t);
    CALLER();
    m3ApiGetArg(uint32_t, method);
    GET_STR(url);
    GET_STR(headers);
    m3ApiGetArgMem(const uint8_t *, body);
    m3ApiGetArg(uint32_t, body_len);
    GET_STR(cred);
    if (body_len) m3ApiCheckMem(body, body_len);
    NET_GATE(ESP_ERR_NOT_ALLOWED);
    m3ApiReturn(orb_app_net_request(slot_index(s), s->app, method, url, headers,
                                    body_len ? body : NULL, body_len, cred));
}

m3ApiRawFunction(imp_net_header)     /* i32 net_header(const char *name, char *buf, u32 cap) */
{
    m3ApiReturnType(int32_t);
    CALLER();
    GET_STR(name);
    m3ApiGetArgMem(char *, buf);
    m3ApiGetArg(uint32_t, cap);
    if (!cap || !name) m3ApiReturn(-1);
    m3ApiCheckMem(buf, cap);
    NET_GATE(-1);
    m3ApiReturn(orb_app_net_header(slot_index(s), name, buf, cap));
}

/* JSON (D51): free — the reader parses bytes the app already holds or fetched. The
 * token crosses verbatim (apps/sdk/orb.h's orb_json_tok_t, asserted the same size). */
_Static_assert(sizeof(orb_json_tok_t) == 152, "orb_json_tok_t's layout is the guest ABI");

m3ApiRawFunction(imp_json_begin)     /* i32 json_begin(const void *buf, u32 len): NULL = net body */
{
    m3ApiReturnType(int32_t);
    CALLER();
    m3ApiGetArgMem(const uint8_t *, buf);
    m3ApiGetArg(uint32_t, len);
    const bool body = m3ApiIsNullPtr(buf);
    if (!body && len) m3ApiCheckMem(buf, len);
    m3ApiReturn(orb_app_json_begin(slot_index(s), body ? NULL : buf, len));
}

m3ApiRawFunction(imp_json_next)      /* i32 json_next(orb_json_tok_t *out) */
{
    m3ApiReturnType(int32_t);
    CALLER();
    m3ApiGetArgMem(uint8_t *, out);
    m3ApiCheckMem(out, sizeof(orb_json_tok_t));
    orb_json_tok_t t;
    const int32_t r = orb_app_json_next(slot_index(s), &t);
    memcpy(out, &t, sizeof t);
    m3ApiReturn(r);
}

m3ApiRawFunction(imp_json_skip)
{
    m3ApiReturnType(int32_t);
    CALLER();
    m3ApiReturn(orb_app_json_skip(slot_index(s)));
}

m3ApiRawFunction(imp_net_info)       /* i32 net_info(orb_net_info_t *out) — 24 B */
{
    m3ApiReturnType(int32_t);
    CALLER();
    m3ApiGetArgMem(uint32_t *, out);
    m3ApiCheckMem(out, sizeof(orb_app_net_info_t));
    NET_GATE(ESP_ERR_NOT_ALLOWED);
    orb_app_net_info_t i;
    orb_app_net_info(slot_index(s), &i);
    memcpy(out, &i, sizeof i);                 /* six 32-bit words, both sides LE */
    m3ApiReturn(ESP_OK);
}

m3ApiRawFunction(imp_net_read)       /* i32 net_read(u32 off, void *buf, u32 cap) */
{
    m3ApiReturnType(int32_t);
    CALLER();
    m3ApiGetArg(uint32_t, off);
    m3ApiGetArgMem(uint8_t *, buf);
    m3ApiGetArg(uint32_t, cap);
    m3ApiCheckMem(buf, cap);
    NET_GATE(0);
    m3ApiReturn((int32_t)orb_app_net_read(slot_index(s), off, buf, cap));
}

m3ApiRawFunction(imp_net_free)
{
    CALLER();
    orb_app_net_free(slot_index(s));
    m3ApiSuccess();
}

m3ApiRawFunction(imp_net_cred)       /* i32 net_cred(const char *id): 1 when that secret is set */
{
    m3ApiReturnType(int32_t);
    CALLER();
    GET_STR(id);
    m3ApiReturn(id && orb_app_cred_has(s->app, id) ? 1 : 0);
}

/* the stage (D50): it draws the live theme for the app that theme belongs to (a
 * theme is one app's, D48), and only that app may pose it; any other app that opens
 * it is told it holds no theme and draws its own face (D31). The stage itself was built, and is run
 * and parked, by the hooks below on the task's own stack; these only read the
 * theme's records, take the frame's poses and play the app's own sounds. */

/* theme-pool-plan P4: an app with a theme in the pool holds the stage only while it is
 * bound to it — while the app builds, and while it is on the glass. Unbound, every import
 * answers as the base face does, so an app ticking in the background can neither read
 * nor pose another app's rows. */
#define STAGE_ON(s) ((s)->stage && stage_bound((s)->app))
#define STAGE_GATE(fail) if (!STAGE_ON(s)) m3ApiReturn(fail)

m3ApiRawFunction(imp_stage_open)              /* i32 stage_open(orb_stage_info_t *out) — 20 B */
{
    m3ApiReturnType(int32_t);
    CALLER();
    m3ApiGetArgMem(uint8_t *, out);
    m3ApiCheckMem(out, 20);
    bool base = true;
    if (!STAGE_ON(s)) {              /* no theme of this app's is bound: it draws its own */
        memset(out, 0, 20);
        out[6] = CLK_PAT_NONE; out[7] = 1; out[8] = 0xFF;
        m3ApiReturn(ESP_OK);
    }
    if (stage_open(s->app, &base) != ESP_OK) m3ApiReturn(ESP_FAIL);
    uint16_t nr, nc, tn;
    uint8_t car, tr, sw, bg, pat = CLK_PAT_NONE;
    stage_info(&nr, &nc, &car, &tr, &sw, &bg, &tn);
    uint32_t ns = 0;
    (void)stage_sounds(&ns, &pat);
    memset(out, 0, 20);
    memcpy(out + 0, &nr, 2);
    memcpy(out + 2, &nc, 2);
    const uint16_t ns16 = (uint16_t)ns;
    memcpy(out + 4, &ns16, 2);
    out[6] = pat; out[7] = base; out[8] = car; out[9] = tr; out[10] = sw; out[11] = bg;
    memcpy(out + 12, &tn, 2);
    m3ApiReturn(ESP_OK);
}

/* A theme record, copied verbatim: the guest's apps/sdk/clk_types.h is a byte copy
 * of the OS's (apps/sdk/orbsdk.py refuses a drift), and both sides are ILP32 LE. */
#define STAGE_COPY(fn, type, getter)                                                \
    m3ApiRawFunction(fn) {                                                         \
        m3ApiReturnType(int32_t);                                                  \
        CALLER();                                                                  \
        m3ApiGetArg(int32_t, i);                                                   \
        m3ApiGetArgMem(uint8_t *, out);                                            \
        m3ApiCheckMem(out, sizeof(type));                                          \
        STAGE_GATE(ESP_ERR_NOT_ALLOWED);                                            \
        const type *r = getter;                                                    \
        if (!r) m3ApiReturn(ESP_ERR_NOT_FOUND);                                    \
        memcpy(out, r, sizeof(type));                                              \
        m3ApiReturn(ESP_OK);                                                       \
    }
STAGE_COPY(imp_stage_row,  clk_row_t,  stage_row(i))
STAGE_COPY(imp_stage_comp, clk_comp_t, stage_comp(i))

static const clk_sound_t *stage_sound_at(int i)
{
    uint32_t n = 0;
    uint8_t pat;
    const clk_sound_t *t = stage_sounds(&n, &pat);
    return (t && i >= 0 && (uint32_t)i < n) ? &t[i] : NULL;
}
STAGE_COPY(imp_stage_sound, clk_sound_t, stage_sound_at(i))

m3ApiRawFunction(imp_stage_now)               /* void stage_now(i64 *t, i32 *frac, i32 *wall) */
{
    CALLER();
    m3ApiGetArgMem(int64_t *, pt);
    m3ApiGetArgMem(int32_t *, pf);
    m3ApiGetArgMem(int32_t *, pw);
    m3ApiCheckMem(pt, 8);
    m3ApiCheckMem(pf, 4);
    m3ApiCheckMem(pw, 4);
    if (!STAGE_ON(s)) m3ApiSuccess();
    time_t t; double f, w;
    stage_now(&t, &f, &w);
    m3ApiWriteMem64(pt, (int64_t)t);
    m3ApiWriteMem32(pf, (int32_t)(f * 1e9));
    m3ApiWriteMem32(pw, (int32_t)(w * 1e9));
    m3ApiSuccess();
}

m3ApiRawFunction(imp_stage_pose)              /* void stage_pose(i, ang_10, ox, oy) */
{
    CALLER();
    m3ApiGetArg(int32_t, i);
    m3ApiGetArg(int32_t, a);
    m3ApiGetArg(int32_t, ox);
    m3ApiGetArg(int32_t, oy);
    if (STAGE_ON(s)) stage_pose(i, a, ox, oy);
    m3ApiSuccess();
}

m3ApiRawFunction(imp_stage_frame)             /* i32 stage_frame(i, f) */
{
    m3ApiReturnType(int32_t);
    CALLER();
    m3ApiGetArg(int32_t, i);
    m3ApiGetArg(int32_t, f);
    m3ApiReturn(STAGE_ON(s) ? stage_frame(i, f) : 0);
}

m3ApiRawFunction(imp_stage_opacity)           /* void stage_opacity(i, opa) */
{
    CALLER();
    m3ApiGetArg(int32_t, i);
    m3ApiGetArg(int32_t, opa);
    if (STAGE_ON(s)) stage_opacity(i, opa);
    m3ApiSuccess();
}

m3ApiRawFunction(imp_stage_text)              /* i32 stage_text(i, const char *s, tone) */
{
    m3ApiReturnType(int32_t);
    CALLER();
    m3ApiGetArg(int32_t, i);
    GET_STR(str);
    m3ApiGetArg(int32_t, tone);
    m3ApiReturn(STAGE_ON(s) ? stage_text(i, str, tone) : 0);
}

m3ApiRawFunction(imp_stage_comp_set)          /* void stage_comp_set(i, value, live) */
{
    CALLER();
    m3ApiGetArg(int32_t, i);
    m3ApiGetArg(int32_t, v);
    m3ApiGetArg(int32_t, live);
    if (STAGE_ON(s)) stage_comp_set(i, v, live != 0);
    m3ApiSuccess();
}

m3ApiRawFunction(imp_stage_snd_ready)
{
    m3ApiReturnType(int32_t);
    CALLER();
    m3ApiReturn(STAGE_ON(s) && orb_sound_ready() &&
                (orb_svc_grants(s->app) & (1u << CAP_SOUND)) ? 1 : 0);
}

m3ApiRawFunction(imp_stage_snd_cfg)           /* void stage_snd_cfg(orb_stage_sound_cfg_t *) — 8 B */
{
    CALLER();
    m3ApiGetArgMem(uint8_t *, out);
    m3ApiCheckMem(out, 8);
    orb_sound_cfg_t c;
    orb_sound_cfg_get(&c);
    out[0] = c.volume; out[1] = c.tick; out[2] = c.chimes; out[3] = c.strikes;
    out[4] = c.jumps; out[5] = (uint8_t)c.quiet_from; out[6] = (uint8_t)c.quiet_to;
    out[7] = (uint8_t)c.pattern;
    m3ApiSuccess();
}

m3ApiRawFunction(imp_stage_snd_quiet)         /* i32 stage_snd_quiet(i32 hour) */
{
    m3ApiReturnType(int32_t);
    CALLER();
    m3ApiGetArg(int32_t, hour);
    m3ApiReturn(orb_sound_quiet_at(hour) ? 1 : 0);
}

m3ApiRawFunction(imp_stage_snd_play)          /* i32 stage_snd_play(i, flags, i64 at, i64 anchor) */
{
    m3ApiReturnType(int32_t);
    CALLER();
    m3ApiGetArg(int32_t, i);
    m3ApiGetArg(uint32_t, flags);
    m3ApiGetArg(int64_t, at);
    m3ApiGetArg(int64_t, anchor);
    m3ApiReturn(STAGE_ON(s) ? stage_snd_play(i, flags, at, anchor) : -1);
}

m3ApiRawFunction(imp_stage_snd_stop)
{
    CALLER();
    m3ApiGetArg(int32_t, h);
    if (STAGE_ON(s) && h > 0) orb_sound_stop(h);
    m3ApiSuccess();
}

m3ApiRawFunction(imp_stage_snd_active)
{
    m3ApiReturnType(int32_t);
    CALLER();
    m3ApiGetArg(int32_t, h);
    m3ApiReturn(STAGE_ON(s) && h > 0 && orb_sound_active(h) ? 1 : 0);
}

m3ApiRawFunction(imp_stage_snd_reanchor)
{
    CALLER();
    m3ApiGetArg(int32_t, h);
    m3ApiGetArg(int64_t, anchor);
    if (STAGE_ON(s) && h > 0) orb_sound_reanchor(h, anchor);
    m3ApiSuccess();
}

/* maths: the OS's libm, free */

#define MATH1(fn, call)                                                  \
    m3ApiRawFunction(fn) {                                               \
        m3ApiReturnType(double);                                         \
        CALLER();                                                        \
        m3ApiGetArg(double, x);                                          \
        m3ApiReturn(call);                                               \
    }
MATH1(imp_sin, sin(x))
MATH1(imp_cos, cos(x))
MATH1(imp_asin, asin(x))
MATH1(imp_acos, acos(x))

m3ApiRawFunction(imp_atan2)
{
    m3ApiReturnType(double);
    CALLER();
    m3ApiGetArg(double, y);
    m3ApiGetArg(double, x);
    m3ApiReturn(atan2(y, x));
}

m3ApiRawFunction(imp_fmod)
{
    m3ApiReturnType(double);
    CALLER();
    m3ApiGetArg(double, x);
    m3ApiGetArg(double, y);
    m3ApiReturn(fmod(x, y));
}

/* Name, signature, host fn. The signature is wasm3's raw-call notation: the result
 * first, then the params in parens — "i(ii)" is two i32 args and an i32 back; 'I' is
 * i64, 'F' f64, 'v' nothing. m3_bind.c's SignatureToFuncType has no case for '_', so
 * the module-dump spelling ("ii_i") parses its params as returns and throws "malformed
 * signature; return count overflow" — caught on the bench enabling the first
 * import-bearing module 2026-10-21. apps/sdk/orb.h is the other half of this table:
 * a row here and a declaration there move together. */
typedef struct {
    const char *module;
    const char *name;
    const char *sig;
    M3RawCall   fn;
} wasm_bind_t;

static const wasm_bind_t s_binds[] = {
    { "orb", "now",           "i(ii)",     imp_now },
    { "orb", "uptime_ms",     "i()",       imp_uptime_ms },
    { "orb", "uptime_us",     "I()",       imp_uptime_us },
    { "orb", "localtime",     "i(Ii)",     imp_localtime },
    { "orb", "tz_offset",     "i(I)",      imp_tz_offset },
    { "orb", "log",           "v(i)",      imp_log },
    { "orb", "cfg_get",       "i(ii)",     imp_cfg_get },
    { "orb", "cfg_set",       "i(ii)",     imp_cfg_set },
    { "orb", "cfg_get_str",   "i(iii)",    imp_cfg_get_str },
    { "orb", "cfg_set_str",   "i(ii)",     imp_cfg_set_str },
    { "orb", "cfg_gen",       "i()",       imp_cfg_gen },
    { "orb", "set_beat",      "i(i)",      imp_set_beat },
    { "orb", "burst",         "v(i)",      imp_burst },
    { "orb", "ui_create",     "i(ii)",     imp_ui_create },
    { "orb", "ui_set",        "v(iii)",    imp_ui_set },
    { "orb", "ui_text",       "v(ii)",     imp_ui_text },
    { "orb", "ui_align",      "v(iiii)",   imp_ui_align },
    { "orb", "ui_refresh",    "v()",       imp_ui_refresh },
    { "orb", "cv_create",     "i(iii)",    imp_cv_create },
    { "orb", "cv_fill",       "v(ii)",     imp_cv_fill },
    { "orb", "cv_pixel",      "v(iiii)",   imp_cv_pixel },
    { "orb", "cv_ring",       "v(iiiii)",  imp_cv_ring },
    { "orb", "cv_disc",       "v(iiiii)",  imp_cv_disc },
    { "orb", "cv_line",       "v(iiiiii)", imp_cv_line },
    { "orb", "cv_text",       "v(iiiiiii)", imp_cv_text },
    { "orb", "cv_flush",      "v(i)",      imp_cv_flush },
    { "orb", "reply",         "v(i)",      imp_reply },
    { "orb", "armed",         "v(i)",      imp_armed },
    { "orb", "sound_play",    "i(iii)",    imp_sound_play },
    { "orb", "sound_stop",    "v(i)",      imp_sound_stop },
    { "orb", "sound_active",  "i(i)",      imp_sound_active },
    { "orb", "home",          "i(i)",      imp_home },
    { "orb", "home_gen",      "i()",       imp_home_gen },
    { "orb", "power_request", "i(ii)",     imp_power_request },
    { "orb", "net_request",   "i(iiiiii)", imp_net_request },
    { "orb", "net_header",    "i(iii)",    imp_net_header },
    { "orb", "json_begin",    "i(ii)",     imp_json_begin },
    { "orb", "json_next",     "i(i)",      imp_json_next },
    { "orb", "json_skip",     "i()",       imp_json_skip },
    { "orb", "net_info",      "i(i)",      imp_net_info },
    { "orb", "net_read",      "i(iii)",    imp_net_read },
    { "orb", "net_free",      "v()",       imp_net_free },
    { "orb", "net_cred",      "i(i)",      imp_net_cred },
    { "orb", "stage_open",     "i(i)",      imp_stage_open },
    { "orb", "stage_row",      "i(ii)",     imp_stage_row },
    { "orb", "stage_comp",     "i(ii)",     imp_stage_comp },
    { "orb", "stage_sound",    "i(ii)",     imp_stage_sound },
    { "orb", "stage_now",      "v(iii)",    imp_stage_now },
    { "orb", "stage_pose",     "v(iiii)",   imp_stage_pose },
    { "orb", "stage_frame",    "i(ii)",     imp_stage_frame },
    { "orb", "stage_opacity",  "v(ii)",     imp_stage_opacity },
    { "orb", "stage_text",     "i(iii)",    imp_stage_text },
    { "orb", "stage_comp_set", "v(iii)",    imp_stage_comp_set },
    { "orb", "stage_snd_ready",    "i()",      imp_stage_snd_ready },
    { "orb", "stage_snd_cfg",      "v(i)",     imp_stage_snd_cfg },
    { "orb", "stage_snd_quiet",    "i(i)",     imp_stage_snd_quiet },
    { "orb", "stage_snd_play",     "i(iiII)",  imp_stage_snd_play },
    { "orb", "stage_snd_stop",     "v(i)",     imp_stage_snd_stop },
    { "orb", "stage_snd_active",   "i(i)",     imp_stage_snd_active },
    { "orb", "stage_snd_reanchor", "v(iI)",    imp_stage_snd_reanchor },
    { "orb", "sin",           "F(F)",      imp_sin },
    { "orb", "cos",           "F(F)",      imp_cos },
    { "orb", "asin",          "F(F)",      imp_asin },
    { "orb", "acos",          "F(F)",      imp_acos },
    { "orb", "atan2",         "F(FF)",     imp_atan2 },
    { "orb", "fmod",          "F(FF)",     imp_fmod },
};

/* Apply every import the module actually declared. m3_LinkRawFunction on a name the
 * module does not import returns m3Err_functionLookupFailed — expected, not a failure:
 * an app that never asked for "orb.now" must not fail to load over it. Only a real
 * link error (moduleNotLinked, an import-type mismatch, say) is worth surfacing.
 * Extra services (orb_app_net.c) append their own rows through orb_wasm_bind_more. */
/* The last load's refusal, as the owner should read it: the install ladder copies it onto
 * the row (orb_svc_note_refused), so the menu and the hub say why, not only the log. */
static char s_load_why[96];
const char *orb_wasm_load_why(void) { return s_load_why; }

static M3Result bind_imports(IM3Module mod)
{
    for (size_t i = 0; i < sizeof s_binds / sizeof s_binds[0]; i++) {
        M3Result r = m3_LinkRawFunction(mod, s_binds[i].module, s_binds[i].name,
                                        s_binds[i].sig, s_binds[i].fn);
        if (r && r != m3Err_functionLookupFailed) return r;
    }
    /* Every import the module declares must now be linked. One this OS does not bind
     * — a module built for an older or newer surface — would otherwise run until it
     * called it, and wasm3 calling an unlinked import traps or jumps through nothing
     * (orb-A 2026-11-01: an InstructionFetchError at a PSRAM address, then two C2
     * strikes, from a flights module still importing the retired net_get). Refuse it
     * at load, by name, before any of its code runs. */
    const char *missing = m3_FirstUnlinkedImport(mod);
    if (missing) {
        ESP_LOGE(TAG, "module imports orb.%s, which this OS does not provide — rebuild the app",
                 missing);
        snprintf(s_load_why, sizeof s_load_why,
                 "needs orb.%s, which this firmware lacks: update the firmware", missing);
        return "the app imports a function this OS does not provide";
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
    mirror_sync(s);                            /* flash reads happen here, not on the guest stack */
    M3Result r = guest_call(f, argc, argv);
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
 * Each hook finds the row's instance and runs the matching export. The page is the
 * app's own (orb_ui.c), built by its orb_init through the ui_* imports and handed to
 * the registry by screen(). The beat is the shim's timer, armed between activate or
 * resume and suspend. */

/* An export's i32 result, or `dflt` when it is absent or trapped. */
static int32_t wasm_call_i32(wasm_slot_t *s, int entry, const void *argv[], uint32_t argc,
                             int32_t dflt)
{
    if (!s->fn[entry] || !wasm_call(s, entry, argv, argc) || !s->rt) return dflt;
    int32_t v = dflt;
    if (m3_GetResultsV(s->fn[entry], &v)) return dflt;
    return v;
}

static esp_err_t wasm_init(orb_app_t *app)
{
    wasm_slot_t *s = slot_of_app(app);
    if (!s || !s->rt) return ESP_FAIL;          /* never loaded: nothing to build */
    if (!orb_ui_screen(slot_index(s))) return ESP_ERR_NO_MEM;
    /* The app the live theme belongs to gets the stage (D50), built here on
     * the task's own stack (it touches the bank, the scheduler and the layer table)
     * before its orb_init can ask what it holds. */
    s->stage = app->slug && orb_theme_has(app->slug);   /* theme-pool-plan P4: its own */
    if (s->stage) {
        bool base = true;
        if (stage_open(app, &base) != ESP_OK) s->stage = false;
    }
    s_budget_ms = ORB_WASM_INIT_BUDGET_MS;
    const int32_t r = wasm_call_i32(s, W_INIT, NULL, 0, ESP_FAIL);
    s_budget_ms = ORB_WASM_CALL_BUDGET_MS;
    if (r != 0) ESP_LOGW(TAG, "\"%s\" refused to start (%ld)", app->slug, (long)r);
    ESP_LOGI(TAG, "\"%s\" started; guest stack peak %u of %u B", app->slug,
             (unsigned)orb_wasm_stack_peak(), (unsigned)ORB_WASM_NATIVE_STACK);
    return r == 0 ? ESP_OK : ESP_FAIL;
}

static esp_err_t wasm_activate(orb_app_t *app)
{
    wasm_slot_t *s = slot_of_app(app);
    if (!s || !s->rt) return ESP_FAIL;
    s->on_glass = true;
    if (!wasm_call(s, W_ACTIVATE, NULL, 0)) return ESP_FAIL;
    if (s->rt) beat_apply(s);
    if (s->stage && stage_bind(app) == ESP_OK)   /* its own theme on its own page */
        stage_run(true);                          /* the stage's frames, which call orb_tick */
    return ESP_OK;
}

static struct _lv_obj_t *wasm_screen(orb_app_t *app)
{
    wasm_slot_t *s = slot_of_app(app);
    return s ? orb_ui_screen(slot_index(s)) : NULL;
}

static void wasm_suspend(orb_app_t *app)
{
    wasm_slot_t *s = slot_of_app(app);
    if (!s) return;
    s->on_glass = false;
    beat_apply(s);
    if (s->stage && stage_bound(app)) stage_run(false);
    if (s->rt) (void)wasm_call(s, W_SUSPEND, NULL, 0);
}

static void wasm_resume(orb_app_t *app)
{
    wasm_slot_t *s = slot_of_app(app);
    if (!s || !s->rt) return;
    s->on_glass = true;
    (void)wasm_call(s, W_RESUME, NULL, 0);
    if (s->rt) beat_apply(s);
    if (s->stage && stage_bind(app) == ESP_OK) stage_run(true);
}

static void wasm_show(orb_app_t *app)
{
    wasm_slot_t *s = slot_of_app(app);
    if (s && s->rt) (void)wasm_call(s, W_SHOW, NULL, 0);
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

/* The guest answers the state it took: an app with no editor stays unarmed on a
 * press (the dial's repeater is exactly that), and the router reads it back here. */
static void wasm_set_armed(orb_app_t *app, bool armed)
{
    wasm_slot_t *s = slot_of_app(app);
    if (!s || !s->rt) return;
    if (!s->fn[W_SET_ARMED]) { s->armed = armed; return; }
    uint32_t a = armed ? 1u : 0u;
    const void *argv[1] = { &a };
    s->armed = wasm_call_i32(s, W_SET_ARMED, argv, 1, 0) != 0;
}

static void wasm_service(orb_app_t *app)
{
    wasm_slot_t *s = slot_of_app(app);
    if (s && s->rt) (void)wasm_call(s, W_SERVICE, NULL, 0);
}

static void beat_cb(lv_timer_t *t)
{
    wasm_slot_t *s = lv_timer_get_user_data(t);
    if (!s || !s->rt || !s->on_glass) return;
    uint32_t a = lv_tick_get();
    const void *argv[1] = { &a };
    (void)wasm_call(s, W_TICK, argv, 1);
}

/* The beat landing for a caller that drives its own frame clock (the dial's stage
 * runs the clock's tick from the frame scheduler, not from the shim's timer). */
void orb_wasm_tick(orb_app_t *app, uint32_t now_ms)
{
    wasm_slot_t *s = slot_of_app(app);
    if (!s || !s->rt) return;
    uint32_t a = now_ms;
    const void *argv[1] = { &a };
    (void)wasm_call(s, W_TICK, argv, 1);
}

/* ---- the option page: the menu draws, the guest answers ---------------------- */

static int wasm_opt_count(orb_app_t *app)
{
    wasm_slot_t *s = slot_of_app(app);
    return s && s->rt ? (int)wasm_call_i32(s, W_OPT_COUNT, NULL, 0, 0) : 0;
}

static orb_opt_kind_t wasm_opt_kind(orb_app_t *app, int idx)
{
    wasm_slot_t *s = slot_of_app(app);
    if (!s || !s->rt) return OPT_ROW;
    uint32_t a = (uint32_t)idx;
    const void *argv[1] = { &a };
    const int32_t k = wasm_call_i32(s, W_OPT_KIND, argv, 1, OPT_ROW);
    return k == OPT_GROUP || k == OPT_ITEM ? (orb_opt_kind_t)k : OPT_ROW;
}

static const char *wasm_opt_text(orb_app_t *app, int entry, int idx, bool label)
{
    wasm_slot_t *s = slot_of_app(app);
    if (!s || !s->rt) return "";
    char *buf = label ? s->label : s->value;
    buf[0] = '\0';
    s->reply = buf;
    uint32_t a = (uint32_t)idx;
    const void *argv[1] = { &a };
    (void)wasm_call(s, entry, argv, 1);
    s->reply = NULL;
    return buf;
}

static const char *wasm_opt_label(orb_app_t *app, int idx)
{
    return wasm_opt_text(app, W_OPT_LABEL, idx, true);
}

static const char *wasm_opt_value(orb_app_t *app, int idx)
{
    return wasm_opt_text(app, W_OPT_VALUE, idx, false);
}

static void wasm_opt_inc(orb_app_t *app, int idx, int32_t steps)
{
    wasm_slot_t *s = slot_of_app(app);
    if (!s || !s->rt) return;
    uint32_t a[2] = { (uint32_t)idx, (uint32_t)steps };
    const void *argv[2] = { &a[0], &a[1] };
    (void)wasm_call(s, W_OPT_INC, argv, 2);
}

static void wasm_opt_sel(orb_app_t *app, int idx)
{
    wasm_slot_t *s = slot_of_app(app);
    if (!s || !s->rt) return;
    uint32_t a = (uint32_t)idx;
    const void *argv[1] = { &a };
    (void)wasm_call(s, W_OPT_SEL, argv, 1);
}

static void wasm_opt_arm(orb_app_t *app, bool armed)
{
    wasm_slot_t *s = slot_of_app(app);
    if (!s || !s->rt) return;
    uint32_t a = armed ? 1u : 0u;
    const void *argv[1] = { &a };
    (void)wasm_call(s, W_OPT_ARM, argv, 1);
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
    .show      = wasm_show,
    .input     = wasm_input,
    .armed     = wasm_armed,
    .set_armed = wasm_set_armed,
    .service   = wasm_service,
    .opt_count = wasm_opt_count,
    .opt_kind  = wasm_opt_kind,
    .opt_label = wasm_opt_label,
    .opt_value = wasm_opt_value,
    .opt_inc   = wasm_opt_inc,
    .opt_sel   = wasm_opt_sel,
    .opt_arm   = wasm_opt_arm,
};

/* ---- lifecycle ----------------------------------------------------------- */

esp_err_t orb_wasm_load(int slot, const orbapp_hdr_t *hdr,
                        const uint8_t *bytes, uint32_t len)
{
    s_load_why[0] = '\0';
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

    r = m3_LoadModule(s->rt, s->mod);          /* takes s->mod either way */
    if (r) {
        ESP_LOGW(TAG, "slot %d: module would not load: %s", slot, r);
        snprintf(s_load_why, sizeof s_load_why, "the engine would not load its code: %s", r);
        slot_release(s); return ESP_FAIL;
    }

    /* Link AFTER Load: m3_LinkRawFunction throws moduleNotLinked on a module whose
     * runtime pointer is still NULL (m3_bind.c:134 — caught on the bench 2026-10-21,
     * where the pre-load bind refused every module). The pointer is valid to use here;
     * it is the runtime's from this line, and slot_release frees it as part of the
     * runtime, so it is NULLed the moment the link lands. */
    M3Result br = bind_imports(s->mod);
    s->mod = NULL;                             /* now owned by the runtime */
    if (br) { ESP_LOGW(TAG, "slot %d: import bind failed: %s", slot, br); slot_release(s); return ESP_FAIL; }

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
                snprintf(s_load_why, sizeof s_load_why, "its code has no %s entry point",
                         s_entries[i].export_name);
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
 * orb_wasm_load that succeeds, at boot. */
void orb_wasm_bind_app(int slot, orb_app_t *app)
{
    if (slot < 0 || slot >= WASM_N_SLOTS) return;
    s_slot[slot].app = app;
    /* The caller is on an internal stack (the boot ladder), which is where an
     * NVS read may happen; a guest's network request answers from this copy. */
    if (app && app->slug && (orb_svc_grants(app) & (1u << CAP_NET)))   /* not the counting gate */
        orb_app_cred_preload(app);
}
