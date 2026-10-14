/**
 * orb_svc.c — the broker (see orb_svc.h for the contract; plan §4.2/§4.3).
 *
 * One static row per registry slot carries what the frozen ABI needs the OS to
 * remember about an app: its stored owner-grants, its asks, its call and
 * denial counts (the P3 pass line on /api/v1/status), its D16 beat, its
 * config byte charge, and the last line its status() printed. The row index
 * is orb_app_index_of(), so the table order — which IS boot order — is also
 * the persistence key, the same stability argument orb_app.h uses for
 * art_offset. P4 pins the slot in the catalog beside the app id.
 *
 * Free caps (CAP_FREE_MASK: time/config/ui/art) are never stored and never
 * revocable — orb_grant_ok() answers them before it ever looks at NVS, which
 * is §4.2's "always on" made structural. The owner-granted four follow ruling
 * D19 (declare-at-install is the grant): an app's effective mask is its
 * declared set (orb_app_t.caps) intersected with whatever the owner has last
 * written, so installing is consenting and the dial's ladder is a revocation
 * surface, not an approval queue. A cap the manifest never declared is not
 * grantable by anyone, which is what keeps a stale NVS word, a restored
 * partition and a mis-clicked dial all pointing one way: down, never up.
 *
 * Stubs are honest: every entry that awaits a later commit's wiring (time ->
 * the compose_now extraction, power -> the idle_dim request path, net -> P4,
 * layer_* -> P5, themes -> the D18 wrappers) sits BEHIND its gate, counts the
 * call, and answers ESP_ERR_NOT_SUPPORTED. A stub in place of the gate would
 * pass this file's own suite while shipping an app that can call below its
 * grant — the one failure mode the whole broker exists to prevent.
 */
#include "orb_svc.h"
#include "orb_app.h"
#include "orb_jobs.h"
#include "art_bank.h"
#include "orb_net.h"          /* orb_net_time_synced / tz_label / tz_offset */
#include "orb_theme.h"        /* the D18 family: enumerate and request, never bytes */
#include "idle_dim.h"         /* the power request path — a request, not a write */
#include "orb_layers.h"       /* P5.5: the layer table the four entries drive */

#include <math.h>             /* fabs, floor — the drift rule's own tools */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_system.h"       /* esp_reset_reason_t — who ended last boot */
#include "esp_timer.h"        /* the anchor half of the time pair */
#include "esp_heap_caps.h"    /* the layer planes: PSRAM, off the frame path */
#include "nvs.h"
#include "nvs_flash.h"        /* nvs_flash_init_partition("storage") — the
                              * same idempotent call idle_dim.c makes before
                              * opening its namespace */

static const char *TAG = "orb_svc";

/* The layer plane allocator, installed into orb_layers.c below at init.
 * PSRAM like every other bulk pixel claim in this firmware (the tourbillon's
 * chords, the canvas, the bank arena): a fallback plane is read once per
 * commit by the same pass that already streams PSRAM art, and internal RAM
 * is exactly the budget the framebuffer and the staging bands spend. NULL on
 * an out-of-memory is the honest answer — the table turns it into a counted
 * refusal, and the app keeps its themes. */
static void *svc_layers_alloc(uint32_t bytes)
{
    return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

/* Registry slots, incl. the shell's chrome entries — which is why this is not
 * ORB_APP_CAP: B5 caps installed APPS at 8, and the compiled-in pair plus the
 * menu entry already live beside that budget, not inside it. */
#define ORB_SVC_ROWS 16

typedef struct {
    uint32_t grants;        /* owner's last written mask (orb.grants)        */
    uint32_t denied_mask;   /* caps ever refused this boot — the divergence
                             * evidence the cap page marks, RAM only: like the
                             * counts, it says what THIS build did */
    uint32_t calls, denials;/* lifetime totals, any entry, either outcome  */
    uint32_t beat_ms;       /* D16 rate in force; 0 = stopped              */
    uint32_t cfg_used;      /* bytes charged against ORB_CFG_QUOTA_BYTES   */
    uint32_t faults;        /* C2: unexpected resets with this app shown   */
    bool     faulted;       /* C2: two consecutive faults — never boots    */
    bool     loaded;        /* grant word read from NVS                    */
    bool     stored;        /* a grant word was actually THERE, any value */
    bool     cfg_loaded;    /* quota charge read from NVS                  */
    bool     faults_loaded; /* fault words read from NVS                   */
    orb_layer_tab_t *layers; /* P5.5: this app's live compositor rows, in PSRAM
                             * and allocated on the first layer_add. BY VALUE
                             * was the first design, and the board refused it:
                             * 16 x 920 B of .bss starved httpd_start to
                             * ESP_ERR_HTTPD_TASK at 3,343 B of DMA free. The
                             * table now costs what the planes already cost —
                             * PSRAM an app pays for by using layers. Forget
                             * frees it after the wipe frees the planes. */
    uint32_t layer_refuses;  /* refusals with nowhere to ride the table's own
                             * counter: no table yet (handle without rows, or
                             * PSRAM out at add). Summed into the ledger pair
                             * beside the table's count. */
    char     last_status[72];
    char     fault_reason[48];
} svc_row_t;

static svc_row_t s_row[ORB_SVC_ROWS];

#define GRANTS_NS "orb.grants"

/* The C2 reset record — defined up here so orb_svc_init below can consume
 * and re-arm it; the boot-protocol commentary and the counter live with the
 * fault section further down. */
#define FAULTS_NS       "orb.fault"     /* storage partition, one namespace */
#define FAULT_MAGIC_ARM 0x41524D21u      /* 'ARM!' */
#define FAULT_MAGIC_RUN 0x52554E21u      /* 'RUN!' */
#define FAULT_SLOT_NONE 0xFFFFu

typedef struct { uint32_t magic; uint32_t slot; uint32_t booted; } orb_reset_rec_t;
static orb_reset_rec_t s_rec __attribute__((section(".noinit"), aligned(4)));

static void note_fault_slot(int slot, const char *why);   /* below, with C2 */

/* The declared set, cleaned: what the manifest asked for at install, minus any
 * free bit a careless table entry tried to claim. This is the ceiling (D19) —
 * every read of a mask goes through here, so an over-wide NVS word can never
 * be the reason a call passes. */
static uint32_t declared_mask(const orb_app_t *a)
{
    return a ? (a->caps & ~CAP_FREE_MASK) : 0;
}

/* One NVS round trip per row, per boot. Read-only and small; the boot ladder
 * and the first gated call are the only callers, neither on the frame path.
 *
 * Two answers, and the difference is the ruling: a word that is THERE is the
 * owner's last decision and is authoritative even when it is 0 (everything
 * revoked), while a word that is ABSENT means nobody has ever decided, which
 * under declare-at-install means the declared set stands (D19). Reading an
 * absent word as 0 was B3's rule and it silently un-installs every app's
 * consent on a partition wipe — the app would sit there denied, its calls
 * counting as divergences it never caused. */
static void row_load_grants(svc_row_t *r, const orb_app_t *a, int slot)
{
    r->loaded = true;
    r->grants = declared_mask(a);
    r->stored = false;
    nvs_handle_t h;
    if (nvs_open_from_partition("storage", GRANTS_NS, NVS_READONLY, &h) != ESP_OK)
        return;                                  /* nothing stored: as declared */
    char key[12];
    snprintf(key, sizeof key, "g%d", slot);
    uint32_t v = 0;
    if (nvs_get_u32(h, key, &v) == ESP_OK) {
        r->grants = v & declared_mask(a);         /* free caps are not storable,
                                                   * and undeclared ones are not
                                                   * grantable — the AND is the
                                                   * ceiling, not a filter */
        r->stored = true;
    }
    nvs_close(h);
}

static svc_row_t *row_of(const orb_app_t *a)
{
    const int slot = orb_app_index_of(a);
    if (slot < 0 || slot >= ORB_SVC_ROWS) return NULL;   /* not in the table:
        * an app the registry does not know has no grants to check, and a
        * silent pass here is how a broker becomes a formality. */
    return &s_row[slot];
}

bool orb_grant_ok(const orb_app_t *a, orb_cap_t cap)
{
    svc_row_t *r = row_of(a);
    if (!r) {
        ESP_LOGE(TAG, "grant check for an app that is not in the registry");
        return false;
    }
    r->calls++;
    const uint32_t bit = 1u << cap;
    if (bit & CAP_FREE_MASK) return true;        /* §4.2: always on. Counted,
                                                  * because the pass line is
                                                  * the number, not the verdict */
    const int slot = orb_app_index_of(a);
    if (!r->loaded) row_load_grants(r, a, slot);
    if (r->grants & bit) return true;

    /* Denied. Nothing is asked for (D19): the app already has everything it
     * said it needed at install. Two roads reach here and only one of them is
     * normal. The cap was declared and the owner has since closed it, which is
     * the dial working as designed and an app that must degrade politely; or
     * the cap was NEVER declared, which is the app reaching past its own
     * consent — a bug in the app, and the reason this line exists. One line
     * per (app, cap) per boot, because an app that retries a gated entry every
     * tick would otherwise bury every other line on the console, and the
     * ladder's * mark reads the same mask. */
    const char *why = (declared_mask(a) & bit)
                    ? "the owner has revoked it"
                    : "it was never declared — the app is calling past its consent";
    if (!(r->denied_mask & bit))
        ESP_LOGW(TAG, "\"%s\" called %s — denied: %s",
                 a->slug ? a->slug : "?", orb_svc_cap_name(cap), why);
    r->denied_mask |= bit;
    r->denials++;
    return false;
}

/* ---- config: declared keys only, own namespace, byte quota (§4.2/C9) ------
 *
 * The three bounds are the grant: a key the manifest did not declare is as
 * much below-grant as an ungranted capability, because an app that could
 * write arbitrary NVS keys owns the device's settings whether or not it holds
 * a cap. So every entry resolves the id through app->settings BEFORE any NVS
 * handle opens, and the namespace is the slot's own — orb.app<slot> — which
 * makes another app's keys unreachable even to a bug in this file.
 *
 * These four entries may block on NVS and so belong on the worker, per the
 * header's threading note. The byte charge is kept in RAM and written beside
 * the values: one extra u32 ("_used") pays for the quota surviving the reboot
 * that the erase it is rationing might cause. */

static const orb_setting_desc_t *find_setting(const orb_app_t *a, const char *id,
                                              orb_set_type_t want)
{
    for (int i = 0; i < a->n_settings; i++) {
        const orb_setting_desc_t *s = &a->settings[i];
        if (strcmp(s->id, id) == 0)
            return (s->type == want) ? s : NULL;  /* right key, wrong door: the
                                                   * int/string entries are not
                                                   * interchangeable — C9's types
                                                   * are the contract */
    }
    return NULL;
}

static void row_load_cfg(const orb_app_t *a, svc_row_t *r, int slot)
{
    r->cfg_loaded = true;
    char ns[20];
    snprintf(ns, sizeof ns, "orb.app%d", slot);
    nvs_handle_t h;
    if (nvs_open_from_partition("storage", ns, NVS_READONLY, &h) != ESP_OK) {
        r->cfg_used = 0;
        return;
    }
    uint32_t v = 0;
    r->cfg_used = (nvs_get_u32(h, "_used", &v) == ESP_OK) ? v : 0;
    nvs_close(h);
}

/* Returns ESP_OK with the handle open RW, or the reason it is not. Caller
 * closes. Shared by all four entries so the namespace shape appears once. */
static esp_err_t cfg_open(const orb_app_t *a, int slot, nvs_handle_t *h)
{
    char ns[20];
    snprintf(ns, sizeof ns, "orb.app%d", slot);
    return nvs_open_from_partition("storage", ns, NVS_READWRITE, h);
}

static esp_err_t svc_cfg_get(orb_app_t *a, const char *id, int32_t *out)
{
    SVC(a, CONFIG);
    const orb_setting_desc_t *s = find_setting(a, id, ORB_SET_INT);
    if (!s) s = find_setting(a, id, ORB_SET_ENUM);
    if (!s) s = find_setting(a, id, ORB_SET_TOGGLE);
    if (!s) return ESP_ERR_NOT_FOUND;      /* undeclared, or declared STRING:
                                            * the type IS part of the grant */
    const int slot = orb_app_index_of(a);
    if (!s_row[slot].cfg_loaded) row_load_cfg(a, &s_row[slot], slot);
    nvs_handle_t h;
    const esp_err_t e = cfg_open(a, slot, &h);
    if (e != ESP_OK) return e;
    uint32_t v = (uint32_t)s->def;         /* absent = the declared default,
                                            * which is why the default lives in
                                            * the manifest and not in the app:
                                            * first run has no key to read */
    (void)nvs_get_u32(h, id, &v);
    nvs_close(h);
    *out = (int32_t)v;
    return ESP_OK;
}

static esp_err_t svc_cfg_set(orb_app_t *a, const char *id, int32_t v)
{
    SVC(a, CONFIG);
    const orb_setting_desc_t *s = find_setting(a, id, ORB_SET_INT);
    if (!s) s = find_setting(a, id, ORB_SET_ENUM);
    if (!s) s = find_setting(a, id, ORB_SET_TOGGLE);
    if (!s) return ESP_ERR_NOT_FOUND;
    if (s->type == ORB_SET_TOGGLE && (v == 0 || v == 1)) {}      /* the two words */
    else if (s->type == ORB_SET_TOGGLE) return ESP_ERR_INVALID_ARG;
    else if (v < s->min || v > s->max)
        /* Out of range is the app's bug, said with the numbers: the manifest's
         * min/max are the contract the options page renders, and an app that
         * writes past them is about to read back something it did not write. */
        return ESP_ERR_INVALID_ARG;
    const int slot = orb_app_index_of(a);
    if (!s_row[slot].cfg_loaded) row_load_cfg(a, &s_row[slot], slot);
    nvs_handle_t h;
    const esp_err_t e = cfg_open(a, slot, &h);
    if (e != ESP_OK) return e;
    esp_err_t w = nvs_set_u32(h, id, (uint32_t)v);
    if (w == ESP_OK) w = nvs_commit(h);
    nvs_close(h);
    return w;
}

static esp_err_t svc_cfg_get_str(orb_app_t *a, const char *id, char *buf, size_t cap)
{
    SVC(a, CONFIG);
    const orb_setting_desc_t *s = find_setting(a, id, ORB_SET_STRING);
    if (!s) return ESP_ERR_NOT_FOUND;
    const int slot = orb_app_index_of(a);
    if (!s_row[slot].cfg_loaded) row_load_cfg(a, &s_row[slot], slot);
    nvs_handle_t h;
    const esp_err_t e = cfg_open(a, slot, &h);
    if (e != ESP_OK) return e;
    size_t n = cap;
    const esp_err_t g = nvs_get_str(h, id, buf, &n);
    nvs_close(h);
    if (g == ESP_ERR_NVS_NOT_FOUND)            /* first run: the declared default */
        return (snprintf(buf, cap, "%s", s->def_str) < 0) ? ESP_FAIL : ESP_OK;
    return g;                                   /* ESP_ERR_INVALID_SIZE when the
                                                 * caller's cap is under the stored
                                                 * length — the app's bug, named */
}

static esp_err_t svc_cfg_set_str(orb_app_t *a, const char *id, const char *v)
{
    SVC(a, CONFIG);
    const orb_setting_desc_t *s = find_setting(a, id, ORB_SET_STRING);
    if (!s) return ESP_ERR_NOT_FOUND;
    const size_t n = strlen(v);
    if (n >= sizeof s->def_str)                /* 31 + NUL, the manifest's own
                                                * width — no silent truncation:
                                                * a stored value must equal the
                                                * value the app wrote */
        return ESP_ERR_INVALID_SIZE;
    const int slot = orb_app_index_of(a);
    if (!s_row[slot].cfg_loaded) row_load_cfg(a, &s_row[slot], slot);
    nvs_handle_t h;
    esp_err_t e = cfg_open(a, slot, &h);
    if (e != ESP_OK) return e;

    /* The quota is exact against NVS's own accounting: ask the old value's
     * length (NULL buf returns the size it would have written, NUL included),
     * refund it, charge the new. Overwrite is therefore free when the value
     * shrinks and costs only the difference when it grows. */
    size_t old = 0;
    if (nvs_get_str(h, id, NULL, &old) != ESP_OK) old = 0;
    const uint32_t base = (old && s_row[slot].cfg_used >= old)
                        ? s_row[slot].cfg_used - (uint32_t)old : s_row[slot].cfg_used;
    if (base + n + 1 > ORB_CFG_QUOTA_BYTES) {
        nvs_close(h);
        /* The number goes in the log because the app cannot see it in the code
         * and the owner cannot see it on the dial until the quota page ships;
         * an unaccounted refusal is how "my setting vanished" becomes a bug
         * report against the wrong file. */
        ESP_LOGW(TAG, "\"%s\" setting \"%s\": %zu B over the %d B app quota",
                 a->slug ? a->slug : "?", id,
                 (size_t)(base + n + 1 - ORB_CFG_QUOTA_BYTES), ORB_CFG_QUOTA_BYTES);
        return ESP_ERR_INVALID_SIZE;
    }
    e = nvs_set_str(h, id, v);
    if (e == ESP_OK) {
        s_row[slot].cfg_used = base + (uint32_t)n + 1;
        e = nvs_set_u32(h, "_used", s_row[slot].cfg_used);
        if (e == ESP_OK) e = nvs_commit(h);
        if (e != ESP_OK)                    /* flash refused the pair: roll the
                                             * RAM charge back so RAM and NVS
                                             * cannot disagree about the quota */
            s_row[slot].cfg_used = (old) ? base + (uint32_t)old : base;
    }
    nvs_close(h);
    return e;
}

/* ---- entries that await a later commit's wiring -----------------------------
 * Each sits behind its gate and says NOT_SUPPORTED. The gate and the counts
 * are the shipped part NOW; the bodies land with their commits:
 *   power_request               -> the idle_dim request path (this phase)
 *   net                         -> P4's allowlisted client
 *   layer_*                     -> P5's table-driven compositor face
 * A stub in place of the gate would pass this file's own suite while shipping
 * an app that can call below its grant — the one failure the broker exists to
 * prevent. The suite therefore checks the gate AND the refusal per stub.
 */

/* ---- the wall clock: compose_now's anchoring, moved verbatim (plan §4.3,
 * the P3 time commit). The clock reads it through the same door every app
 * will; what stayed in clock_view.c is dial policy — the zone conversion,
 * the hands, the complications. The SECOND and its fraction are the OS's, as
 * ruling D15 signed, and this block is the ONLY anchor pair in the firmware.
 *
 * Free-running time base: starts at a fixed epoch, ticks in wall-clock real
 * time until SNTP lands. time() is taken from that point on — see the
 * dual-source note on orb_time_now for which source wins when. */
#define CLOCK_EPOCH 1700000000  /* Nov 2023 baseline */

/* Wall-clock base the reading ticks from, and the esp_timer instant it was
 * sampled at. ONE pair, not two sources: tm_sec and the sub-second fraction
 * must come from the same clock, or the second hand steps backward once per
 * second by the phase difference between time()'s seconds and the esp_timer
 * sawtooth. Starts on the free-running baseline and is overwritten the first
 * time the system clock looks credible (see orb_time_now). */
static time_t s_epoch_base     = CLOCK_EPOCH;
static int64_t s_t0_us         = 0;
static bool   s_anchor_is_real = false;   /* has s_epoch_base ever come from time()? */
static bool   s_seeded         = false;   /* the first call stamps t0 — the role
                                           * clock_view_init's seed line played,
                                           * keeping the pair sampled together */
#define CLOCK_RESYNC_SLACK 2.0            /* s of drift that re-triggers a re-anchor */

void orb_time_now(time_t *sec, double *frac, bool *synced)
{
    /* Two time sources, one anchor pair (see s_epoch_base). Sampled HERE, on
     * the calling task, so time() and esp_timer_get_time() are taken
     * back-to-back and the pair is consistent; the SNTP callback deliberately
     * only sets a flag.
     *
     * The system clock is trusted when it is either freshly synced or already
     * past the dial's own baseline:
     *   - orb_net_time_synced(): SNTP stepped it during this boot.
     *   - time() >= CLOCK_EPOCH: IDF keeps the boot time in RTC slow memory,
     *     so an epoch set by the previous boot SURVIVES a warm reboot
     *     (measured: after POST /api/v1/reboot the epoch is already right
     *     ~1.5 s before the next SNTP reply lands). Trusting it is what stops
     *     the dial from sliding back to Nov-2023 for a few seconds every time
     *     the orb reboots.
     * Anything lower is the 1970 default after a power cut, so the dial keeps
     * free-running from CLOCK_EPOCH instead — a stale dial beats a 1970 dial,
     * and the hourly CONFIG_LWIP_SNTP_UPDATE_DELAY re-sync then only
     * re-anchors when the drift is big enough to be worth the visible jump. */
    const int64_t now_us = esp_timer_get_time();
    const time_t  real   = time(NULL);
    const bool    trust  = orb_net_time_synced();
    if (!s_seeded) { s_t0_us = now_us; s_seeded = true; }
    if (trust || real >= (time_t)CLOCK_EPOCH) {
        const double drift = (double)(real - s_epoch_base)
                           - (double)(now_us - s_t0_us) / 1e6;
        if (!s_anchor_is_real || fabs(drift) >= CLOCK_RESYNC_SLACK) {
            s_epoch_base     = real;
            s_t0_us          = now_us;
            s_anchor_is_real = true;
        }
    }

    const double  el   = (double)(now_us - s_t0_us) / 1e6;
    *sec  = s_epoch_base + (time_t)floor(el);
    *frac = el - floor(el);
    *synced = trust;
}

static void svc_now(orb_app_t *a, time_t *sec, double *frac, bool *synced)
{
    (void)orb_grant_ok(a, CAP_TIME);             /* counted; time is §4.2's
                                                  * "free, always on" — no app
                                                  * is useful without it */
    orb_time_now(sec, frac, synced);
}

static const char *svc_tz_label(orb_app_t *a)
{
    (void)orb_grant_ok(a, CAP_TIME);
    return orb_net_tz_current();                 /* the stored zone key ("UTC"
                                                  * when none) — a name out,
                                                  * never a struct tm (D15) */
}

static esp_err_t svc_tz_offset(orb_app_t *a, time_t when, int32_t *offset_min)
{
    SVC(a, TIME);
    return orb_net_tz_offset(when, offset_min);  /* D17: the owner's zone,
                                                  * measured at `when`, via
                                                  * orb_net's one computation */
}

static esp_err_t svc_power_request(orb_app_t *a, uint32_t timeout_s, bool hold)
{
    SVC(a, POWER);
    /* A request, not a write (§4.2:375): idle_dim stays the policy owner,
     * applies the Kconfig ceiling and the owner's DISPLAY truth, and the
     * registry revokes the ask when the app leaves the glass below.
     * (0, false) is the app withdrawing an earlier ask. */
    if (!hold && timeout_s == 0) {
        idle_dim_app_release(a->slug);
        return ESP_OK;
    }
    return idle_dim_app_request(a->slug, timeout_s, hold);
}

static esp_err_t svc_http_get(orb_app_t *a, const char *url, orb_buf_t *out)
{
    SVC(a, NET);
    (void)url; (void)out;
    return ESP_ERR_NOT_SUPPORTED;                /* P4 */
}

static const uint8_t *svc_bank(orb_app_t *a)
{
    (void)orb_grant_ok(a, CAP_ART);
    return art_bank_extra_ptr(a->art_slot);      /* the app's OWN bank — §4.2's
                                                  * "free for its own bank".
                                                  * slot -1 maps to no mapping,
                                                  * which reads as "none". */
}

/* P5.5: the four frozen entries are live. The gate and the ledger stay here —
 * the table in gfx/orb_layers.c answers for the row, and answers nothing to an
 * app that never held the cap. A void entry cannot return a refusal, so a
 * denial rides the counted path and the row simply never moves: what was on
 * the glass stays on the glass.
 * create=false reads the table without paying for it; set/touch/del on a row
 * that never added anything is a refusal, and a refusal must not mint a table
 * an OOM later could not have. The allocation is PSRAM calloc — the table is
 * 920 B of book-keeping, zero-initialised is the state it wants, and internal
 * RAM is the budget the boot log just proved is spoken for. */
static orb_layer_tab_t *row_layers(int slot, bool create)
{
    orb_layer_tab_t *t = s_row[slot].layers;
    if (t || !create) return t;
    t = heap_caps_calloc(1, sizeof *t, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!t) s_row[slot].layer_refuses++;   /* the add the table never heard of */
    else    s_row[slot].layers = t;
    return t;
}

static int svc_layer_add(orb_app_t *a, const orb_layer_desc_t *d)
{
    SVC(a, UI);
    const int slot = orb_app_index_of(a);
    if (slot < 0 || !d) return -1;                 /* not a registered entry,
                                                   * or nothing to add        */
    orb_layer_tab_t *t = row_layers(slot, true);
    if (!t) return -1;                             /* PSRAM out: counted, and
                                                   * the app keeps its themes */
    return orb_layers_add(t, d);
}

static void svc_layer_set(orb_app_t *a, int h, const orb_layer_desc_t *d)
{
    if (!orb_grant_ok(a, CAP_UI)) return;          /* the denial is counted;
                                                   * that is what the owner's
                                                   * status page sees         */
    const int slot = orb_app_index_of(a);
    if (slot < 0) return;
    orb_layer_tab_t *t = row_layers(slot, false);
    if (!t) { s_row[slot].layer_refuses++; return; } /* no rows to set       */
    (void)orb_layers_set(t, h, d);
}

static void svc_layer_touch(orb_app_t *a, int h, const struct _lv_area_t *area)
{
    if (!orb_grant_ok(a, CAP_UI)) return;
    (void)area;                                    /* the OS's swept box is the
                                                   * mark, not the app's claim
                                                   * (orb_layers.h)           */
    const int slot = orb_app_index_of(a);
    if (slot < 0) return;
    orb_layer_tab_t *t = row_layers(slot, false);
    if (!t) { s_row[slot].layer_refuses++; return; }
    (void)orb_layers_touch(t, h);
}

static void svc_layer_del(orb_app_t *a, int h)
{
    if (!orb_grant_ok(a, CAP_UI)) return;
    const int slot = orb_app_index_of(a);
    if (slot < 0) return;
    orb_layer_tab_t *t = row_layers(slot, false);
    if (!t) { s_row[slot].layer_refuses++; return; }
    (void)orb_layers_del(t, h);
}

static esp_err_t svc_job_post(orb_app_t *a, orb_job_fn fn, void *arg, size_t arglen)
{
    SVC(a, UI);                                  /* free, but counted: the call
                                                  * number is what shows an app
                                                  * hammering the worker */
    return orb_jobs_post(fn, arg, arglen);       /* the worker's own busy gate
                                                  * answers for double-taps
                                                  * (orb_jobs.h) */
}

/* D16 (§0.6), the policy half — the RULE is here now because it is what makes
 * the manifest the cadence budget. The delivery half (resizing the shared
 * beat, measuring each tick against the declared budget, parking overrunners)
 * lands with the scheduler commit; the row carries the rate either way, and
 * orb_svc_beat_ms() is the seam the scheduler reads. */
static esp_err_t svc_set_beat(orb_app_t *a, uint32_t period_ms)
{
    SVC(a, UI);
    const int slot = orb_app_index_of(a);
    if (period_ms == 0) {                        /* stop until the next resume */
        s_row[slot].beat_ms = 0;
        return ESP_OK;
    }
    if (a->period_ms == 0)
        /* The app declared no beat at install: it gets none at runtime. An
         * app that wants frames asks for them where the owner can price them. */
        return ESP_ERR_NOT_SUPPORTED;
    if (period_ms < a->period_ms) {
        /* Refused with the number in the reason — the plan's wording, honoured
         * literally: without the log line, "the beat did not change" is the
         * only feedback an app ever gets. */
        ESP_LOGW(TAG, "\"%s\" asked for a %lu ms beat; its manifest floor is %lu ms",
                 a->slug ? a->slug : "?", (unsigned long)period_ms,
                 (unsigned long)a->period_ms);
        return ESP_ERR_INVALID_ARG;
    }
    s_row[slot].beat_ms = period_ms;
    return ESP_OK;
}

/* The D18 family: enumerate and request — never a path, never bytes (the
 * assets partition belongs to the OS package manager alone; §14.4). The
 * card walk is the entries' one hazard: theme_count reaches the card, so it
 * carries the config entries' rule — call it from a worker job (SVC->job_post),
 * never from a tick — while the five reads after it answer from the copy that
 * same call left behind, which is what makes a settings page able to ask a
 * row's state inside its own tick. An app that never called count gets the
 * empty answer from every one of them, not a stale one. */
#define SVC_THEME_MAX 8
static orb_theme_entry_t s_te[SVC_THEME_MAX];
static int s_te_n = -1;                       /* -1: nothing scanned yet */

static int svc_theme_count(orb_app_t *a)
{
    if (!orb_grant_ok(a, CAP_THEMES)) return -1;  /* the int cannot carry a
                                                   * code; the counted denial
                                                   * does */
    const int n = orb_theme_scan(s_te, SVC_THEME_MAX);
    s_te_n = n;
    return n;
}

static esp_err_t svc_theme_id(orb_app_t *a, int idx, char *buf, size_t cap)
{
    SVC(a, THEMES);
    if (s_te_n < 0 || idx < 0 || idx >= s_te_n || cap < ORB_THEME_ID_HEX + 1)
        return ESP_ERR_NOT_FOUND;              /* id is 12 hex + NUL, frozen by
                                                * the card's own filename shape */
    snprintf(buf, cap, "%s", s_te[idx].id);
    return ESP_OK;
}

static bool svc_theme_active(orb_app_t *a, int idx)
{
    if (!orb_grant_ok(a, CAP_THEMES)) return false;
    return s_te_n >= 0 && idx >= 0 && idx < s_te_n && s_te[idx].active;
}

static bool svc_theme_changed(orb_app_t *a, int idx)
{
    if (!orb_grant_ok(a, CAP_THEMES)) return false;
    return s_te_n >= 0 && idx >= 0 && idx < s_te_n && s_te[idx].changed;
}

static const char *svc_theme_note(orb_app_t *a, int idx)
{
    if (!orb_grant_ok(a, CAP_THEMES)) return NULL;
    if (s_te_n < 0 || idx < 0 || idx >= s_te_n) return NULL;
    return s_te[idx].why[0] ? s_te[idx].why : NULL;   /* the unloadable reason,
                                                      * entry-owned words */
}

static esp_err_t svc_theme_install(orb_app_t *a, const char *id)
{
    SVC(a, THEMES);
    if (!orb_theme_id_ok(id))                  /* the door every theme id goes
                                                * through — never a path (D18) */
        return ESP_ERR_INVALID_ARG;
    bool known = false;                        /* and it must be an id THIS
                                                * card scan published: an app
                                                * that skipped count and hands
                                                * in a well-formed id it made
                                                * up is still reading bytes it
                                                * cannot see, which is exactly
                                                * what the entry refuses */
    for (int i = 0; s_te_n > 0 && i < s_te_n; i++)
        if (strcmp(s_te[i].id, id) == 0 && s_te[i].err == ESP_OK) { known = true; break; }
    if (!known) return ESP_ERR_NOT_FOUND;
    /* D11 verbatim through the OS package manager: orb_theme_post_install owns
     * the copy-verify-mark job on the shared worker and the reboot ask at its
     * end — the same function the dial's picker and the browser's Load button
     * already queue, which is the promise that all three doors agree. */
    return orb_theme_post_install(id);
}

static void svc_status(orb_app_t *a, const char *fmt, ...)
{
    (void)orb_grant_ok(a, CAP_UI);
    const int slot = orb_app_index_of(a);
    if (slot < 0) return;                        /* not in the table: nothing
                                                  * to print to */
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_row[slot].last_status, sizeof s_row[slot].last_status, fmt, ap);
    va_end(ap);
    /* The line reaches /api/v1/status inside a JSON string, and an app owns
     * the bytes it printed. This is the same echo the tz field learned from
     * (json_str drops a backslash and keeps the escape's target, so a stored
     * quote lets a client forge keys inside the device's own replies): strip
     * to printable ASCII with quote and backslash folded to '.'. A status
     * line is a sentence, not a payload; the app loses two characters and the
     * device keeps its reply honest. */
    for (char *p = s_row[slot].last_status; *p; p++) {
        const unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c > 0x7E || c == '"' || c == '\\') *p = '.';
    }
}

/* The frozen table. The order IS orb_svc_t's field order — the one place a
 * missing entry becomes a compile-visible shape rather than a NULL an app
 * discovers at runtime — and nothing below is reachable without going through
 * a gate in its own entry. */
static const orb_svc_t S_TABLE = {
    .now           = svc_now,
    .tz_label      = svc_tz_label,
    .tz_offset     = svc_tz_offset,
    .power_request = svc_power_request,
    .cfg_get       = svc_cfg_get,
    .cfg_set       = svc_cfg_set,
    .cfg_get_str   = svc_cfg_get_str,
    .cfg_set_str   = svc_cfg_set_str,
    .http_get      = svc_http_get,
    .bank          = svc_bank,
    .layer_add     = svc_layer_add,
    .layer_set     = svc_layer_set,
    .layer_touch   = svc_layer_touch,
    .layer_del     = svc_layer_del,
    .job_post      = svc_job_post,
    .set_beat      = svc_set_beat,
    .theme_count   = svc_theme_count,
    .theme_id      = svc_theme_id,
    .theme_active  = svc_theme_active,
    .theme_changed = svc_theme_changed,
    .theme_note    = svc_theme_note,
    .theme_install = svc_theme_install,
    .status        = svc_status,
};

const orb_svc_t *orb_svc_table(void) { return &S_TABLE; }

const char *orb_svc_cap_name(orb_cap_t cap)
{
    static const char *const N[CAP_N] = {
        "time", "config", "ui", "art", "power", "net", "sd_read", "themes"
    };
    return (unsigned)cap < CAP_N ? N[cap] : "?";
}

/* ---- the OS side -------------------------------------------------------------
 * The grant word is the only app state NVS holds outside the app's own
 * namespace, and its only writer is the owner's toggle on the dial's ladder
 * (D19) — never an app, never the installer. Writing is the act that turns
 * "as declared" into "decided": until the owner touches a cap there is no
 * word at all and the declared set stands, which is why a partition wipe
 * costs the owner their revocations and never costs an app its consent. */
static esp_err_t store_grants(int slot, uint32_t mask)
{
    nvs_handle_t h;
    const esp_err_t e = nvs_open_from_partition("storage", GRANTS_NS,
                                                NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    char key[12];
    snprintf(key, sizeof key, "g%d", slot);
    esp_err_t w = nvs_set_u32(h, key, mask);
    if (w == ESP_OK) w = nvs_commit(h);
    nvs_close(h);
    return w;
}

/* ---- the enable ledger and the uninstall wipe (P4a) --------------------------
 * One u32 per row, key e<slot> beside the grant words it lives with: the same
 * owner's-door provenance, the same slot keying, the opposite default — a
 * stored 1 is the ONLY way an app walks (an absent grant word means "as
 * declared"; an absent enable word means "off", because the install confirm
 * promises the app stays off until the owner turns it on). */
bool orb_svc_enabled(int slot)
{
    nvs_handle_t h;
    if (nvs_open_from_partition("storage", GRANTS_NS, NVS_READONLY, &h) != ESP_OK)
        return false;                        /* no ledger readable: the dial stands */
    char key[12];
    snprintf(key, sizeof key, "e%d", slot);
    uint32_t v = 0;
    const esp_err_t r = nvs_get_u32(h, key, &v);
    nvs_close(h);
    return r == ESP_OK && (v & 1u) != 0;
}

esp_err_t orb_svc_set_enabled(int slot, bool on)
{
    nvs_handle_t h;
    const esp_err_t e = nvs_open_from_partition("storage", GRANTS_NS,
                                                NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    char key[12];
    snprintf(key, sizeof key, "e%d", slot);
    esp_err_t w;
    if (on) {
        w = nvs_set_u32(h, key, 1);
        if (w == ESP_OK) w = nvs_commit(h);
    } else {
        w = nvs_erase_key(h, key);           /* off is the erased state, not a stored 0 */
        if (w == ESP_ERR_NVS_NOT_FOUND) w = ESP_OK;
        if (w == ESP_OK) w = nvs_commit(h);
    }
    nvs_close(h);
    return w;
}

esp_err_t orb_svc_forget_row(int slot)
{
    if (slot < 0 || slot >= ORB_SVC_ROWS) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    if (nvs_open_from_partition("storage", GRANTS_NS, NVS_READWRITE, &h) == ESP_OK) {
        char key[12];
        snprintf(key, sizeof key, "g%d", slot); nvs_erase_key(h, key);
        snprintf(key, sizeof key, "e%d", slot); nvs_erase_key(h, key);
        nvs_commit(h);
        nvs_close(h);
    }
    if (nvs_open_from_partition("storage", FAULTS_NS, NVS_READWRITE, &h) == ESP_OK) {
        char key[16];
        snprintf(key, sizeof key, "c%d", slot); nvs_erase_key(h, key);
        snprintf(key, sizeof key, "x%d", slot); nvs_erase_key(h, key);
        snprintf(key, sizeof key, "r%d", slot); nvs_erase_key(h, key);
        nvs_commit(h);
        nvs_close(h);
    }
    char ns[20];
    snprintf(ns, sizeof ns, "orb.app%d", slot);
    esp_err_t e = nvs_open_from_partition("storage", ns, NVS_READWRITE, &h);
    if (e == ESP_OK) {
        nvs_erase_all(h);                    /* the app's own settings die with it:
                                              * an id collision must not hand a
                                              * successor values it never chose */
        nvs_commit(h);
        nvs_close(h);
    } else if (e != ESP_ERR_NVS_NOT_FOUND)
        return e;                            /* a ledger that will not open: retry later */
    if (s_row[slot].layers) {                  /* P5.5: the planes are heap, and
                                              * the memset below would forget
                                              * the pointers with them. The
                                              * wipe frees planes; the free
                                              * below takes the table itself —
                                              * the memset zeroes the pointer,
                                              * and the slot's next tenant
                                              * inherits no handles, dead or
                                              * otherwise. */
        orb_layers_wipe(s_row[slot].layers);
        heap_caps_free(s_row[slot].layers);
        s_row[slot].layers = NULL;
    }
    memset(&s_row[slot], 0, sizeof s_row[slot]);   /* lazy re-read sees an empty ledger,
                                                   * which under D19 is DECLARED — the
                                                   * honest state for whoever lands on
                                                   * this slot next */
    return ESP_OK;
}

esp_err_t orb_svc_init(void)
{
    /* Preload every row so the first gated call of the boot never pays an NVS
     * read on the frame path, and so /api/v1/status has real numbers before
     * anybody asks. A storage partition that will not open is not fatal: under
     * D19 no word stored is the same answer as never having revoked anything,
     * so every app reads as its declared set — the app still works, and the
     * log line below is what tells the two apart from the dial's own seat. */
    if (nvs_flash_init_partition("storage") != ESP_OK)
        ESP_LOGW(TAG, "storage partition not up — grants read as DECLARED, and "
                      "no revocation the owner made earlier will be remembered");
    /* C2, first act: read last boot's reset record BEFORE anything else can
     * crash, bill the slot it names if the verdict says the reset was not
     * the owner's, and re-arm for this boot. The registry has not run yet,
     * so this reads only the broker's own NVS words. The record is half the
     * evidence: the chip's own account of the reset decides, because the
     * bench's RTS pulses and the owner's EN button leave a live record
     * behind exactly like a crash does (orb_reset_blames_code). */
    if (orb_svc_reset_verdict(s_rec.magic, s_rec.booted)) {
        const int slot = (s_rec.slot < ORB_SVC_ROWS) ? (int)s_rec.slot : -1;
        const int rr = (int)esp_reset_reason();
        if (!orb_reset_blames_code(rr)) {
            ESP_LOGW(TAG, "reset record live but the chip says reason %d — the "
                          "owner's hand, nothing billed", rr);
            if (slot >= 0) orb_svc_note_owner_reset(slot);
        } else if (slot >= 0) note_fault_slot(slot, "unexpected reset");
        else ESP_LOGW(TAG, "unexpected reset with no app on the glass — nothing billed");
    }
    s_rec.magic  = FAULT_MAGIC_ARM;
    s_rec.slot   = FAULT_SLOT_NONE;
    s_rec.booted = 0;
    /* P5.5, first act after the fault ledger: wire the table's heap. Before
     * this line returns no bake is possible and the table says so by refusal
     * count — an app that somehow ticks before the broker cannot corrupt
     * anything, it just draws tier 1 and counts misses. */
    orb_layers_set_alloc(svc_layers_alloc, heap_caps_free);
    for (int i = 0; i < ORB_SVC_ROWS; i++)
        row_load_grants(&s_row[i], orb_app_row((size_t)i), i);
    /* The line is the P3 pass line's boot-half: it names the table so a
     * glance at serial says the broker shipped, and taking its address is
     * what stops the linker collecting an unreferenced S_TABLE out of the
     * image before P4's loader (and the status endpoint) wire consumers to
     * it — a broker that boots invisibly and a broker that is gone read the
     * same from the dial. */
    ESP_LOGI(TAG, "broker up: table %p, %u entries, %u app rows",
             (const void *)orb_svc_table(),
             (unsigned)(sizeof S_TABLE / sizeof(void *)),
             (unsigned)ORB_SVC_ROWS);
    return ESP_OK;
}

uint32_t orb_svc_grants(const orb_app_t *a)
{
    const int slot = orb_app_index_of(a);
    if (slot < 0) return 0;
    /* Load-on-read, the gate's own rule. orb_svc_init preloads every row on
     * the boot ladder, so this never blocks on a real device — but it is what
     * makes the OS's readers honest: before the first gated call of a boot an
     * unloaded row used to read as a bare 0, which under B3 was only "nothing
     * granted yet" and under D19 would be a lie, because an app that has never
     * been touched by anything still runs on its declared set and the ledger
     * and the ladder must say so. */
    if (!s_row[slot].loaded) row_load_grants(&s_row[slot], a, slot);
    return s_row[slot].grants;                        /* EFFECTIVE mask: stored
                                                       * AND declared — what the
                                                       * gate will really answer */
}

uint32_t orb_svc_declared(const orb_app_t *a)
{
    return declared_mask(a);                       /* the install-time consent:
                                                    * what the app said it needed,
                                                    * which is also the ceiling */
}

uint32_t orb_svc_denied_mask(const orb_app_t *a)
{
    const int slot = orb_app_index_of(a);
    return (slot >= 0) ? s_row[slot].denied_mask : 0;
}

esp_err_t orb_svc_set_grant(const orb_app_t *a, orb_cap_t cap, bool on)
{
    if ((1u << cap) & CAP_FREE_MASK)
        return ESP_ERR_INVALID_ARG;              /* free caps are not the
                                                  * owner's to give away, and
                                                  * revoking one would strand
                                                  * every app on the dial */
    const int slot = orb_app_index_of(a);
    if (slot < 0) return ESP_ERR_INVALID_ARG;
    /* D19's ceiling, enforced on the owner's own door: the ladder's job is
     * revoking what was declared, and re-granting a revocation. Turning ON a
     * cap the manifest never asked for would sell the owner a permission the
     * app cannot use, and would erase the one signal — a denial — that says
     * an app reached past its declaration. */
    if (on && !(declared_mask(a) & (1u << cap)))
        return ESP_ERR_INVALID_ARG;
    if (!s_row[slot].loaded) row_load_grants(&s_row[slot], a, slot);
    const uint32_t want = on ? (s_row[slot].grants | (1u << cap))
                             : (s_row[slot].grants & ~(1u << cap));
    /* Persist first, cache second: a refusal must leave the RAM answer
     * matching the flash, or the dial shows a grant the next boot undoes. */
    const esp_err_t e = store_grants(slot, want);
    if (e == ESP_OK) {
        s_row[slot].grants = want;
        s_row[slot].stored = true;               /* there is now a decision on
                                                  * flash, whatever it says — an
                                                  * erase of the last cap reads
                                                  * as revoked, not as undecided */
        /* Revoking power while the app holds the dial awake has to actually
         * put the dial down, or the owner's kill switch is a label. Same
         * owner-wins release the DISPLAY choice makes. */
        if (!on && cap == CAP_POWER) idle_dim_app_release(a->slug);
        /* P5.5 notes what it does NOT do here: no layer wipe rides a revoke,
         * because CAP_UI is free (never stored, never revoked), so there is no
         * owner door that closes an app's ink. The rows end at suspend and at
         * uninstall — the two doors that actually exist for a free cap — and
         * an app that wants its own glass back calls layer_del like it owns
         * the handles it was given. */
    }
    return e;
}

uint32_t orb_svc_calls(const orb_app_t *a)
{
    const int slot = orb_app_index_of(a);
    return (slot >= 0) ? s_row[slot].calls : 0;
}

uint32_t orb_svc_denials(const orb_app_t *a)
{
    const int slot = orb_app_index_of(a);
    return (slot >= 0) ? s_row[slot].denials : 0;
}

const char *orb_svc_last_status(const orb_app_t *a)
{
    const int slot = orb_app_index_of(a);
    return (slot >= 0) ? s_row[slot].last_status : "";
}

uint32_t orb_svc_beat_ms(const orb_app_t *a)
{
    const int slot = orb_app_index_of(a);
    return (slot >= 0) ? s_row[slot].beat_ms : 0;
}

uint32_t orb_svc_cfg_used(const orb_app_t *a)
{
    const int slot = orb_app_index_of(a);
    return (slot >= 0) ? s_row[slot].cfg_used : 0;
}

/* P5.5's ledger pair. Read-only on either shape of the row's state: no table
 * yet is zero live, and the row's own counter sums beside the table's. No
 * allocation happens here — a status read must never be the thing that runs
 * the board out of PSRAM. */
int orb_svc_layers_live(const orb_app_t *a)
{
    const int slot = orb_app_index_of(a);
    if (slot < 0 || !s_row[slot].layers) return 0;
    return orb_layers_count(s_row[slot].layers);
}

uint32_t orb_svc_layers_refused(const orb_app_t *a)
{
    const int slot = orb_app_index_of(a);
    if (slot < 0) return 0;
    /* The row's own refusals (no table yet, PSRAM out at add) plus every answer
     * the table gave. A ledger row that showed only the table's count would
     * hide the refusals made before a first table existed — exactly the ones an
     * owner wants to see. */
    return s_row[slot].layer_refuses +
           (s_row[slot].layers ? orb_layers_refuses(s_row[slot].layers) : 0);
}

/* ---- C2: the fault counter and the boot-into-clock rule (plan §1.4) ---------
 *
 * The S3 gives no MPU wall around an app, so an abort inside a hook is a
 * reboot boundary, not a recoverable error — and IDF 5.4 offers no supported
 * panic-time hook to stamp from the crash itself. The shipped mechanism is
 * the reset RECORD instead: a .noinit word survives every warm reset (panic,
 * task watchdog, brownout-that-kept-DRAM), and the boot reads it before
 * anything can crash again. Its states are the whole protocol:
 *
 *   0                 idle / a reboot the owner asked for (the OS's own
 *                     reboot paths disarm it — an owner's reboot is not
 *                     the app's fault)
 *   ARM  booted=0     armed by orb_svc_init, before any app builds: a reset
 *                     seen here died DURING boot — the armed slot pays
 *   ARM  booted=1     transient; orb_svc_boot_done moves it to RUN
 *   RUN  slot=X       on the glass showing X past the boot ladder: any reset
 *                     seen here was not the owner's, and X pays
 *
 * Two consecutive faults mark the app failed with its reason until the owner
 * reinstalls (orb_svc_clear_faults is that door, P4's APPS page will use it).
 * The HOME entry is exempt from the fail: with the clock itself faulting,
 * "boot into the clock" has no destination, and a dial that tells the time
 * through a misbehaving clock beats a bricked orb — the counter still runs
 * and the status endpoint still shows it.
 * (The record itself and the namespace words are declared above, next to
 * orb_svc_init, which consumes them first.)
 */

int orb_svc_reset_verdict(uint32_t magic, uint32_t booted)
{
    if (magic == FAULT_MAGIC_RUN) return 1;
    if (magic == FAULT_MAGIC_ARM && booted == 0) return 1;
    return 0;   /* 0, garbage after a cold boot, or a boot that finished */
}

/* The other half of the verdict, and the half the record alone cannot supply.
 * The .noinit word says a boot ended without its boot-done mark; it cannot say
 * who ended it. Only the silicon can, and the difference is the whole rule:
 * the bench pulses RTS on every esptool run and every serial capture (§1.2's
 * own bench protocol), the owner reaches for the EN button or pulls the
 * battery, and none of that is the app's doing — yet every one of those leaves
 * a live record behind, and two of them mark an app failed (§1.4's C2). So an
 * unexplained record is billed only when the chip itself accuses the code.
 * ESP_RST_SW is in the accused set on purpose: each OS-owned reboot disarms
 * the record first (orb_svc_note_clean_reboot), so a live record behind a
 * software restart means something the OS did not route reached for it.
 * Anything the chip cannot name (UNKNOWN, and the supply-side POWERON/EXT/USB/
 * BROWNOUT/PWR_GLITCH family) reads as the owner's hand: unbilled, and said out
 * loud, because a fault that disappears quietly is a fault the owner cannot see.
 * Enumerators, not numbers: a rename in IDF lands on the two host vectors that
 * name them, not on the board. */
bool orb_reset_blames_code(int rr)
{
    switch (rr) {
    case ESP_RST_PANIC:       /* abort()/panic — the crash path itself        */
    case ESP_RST_SW:          /* esp_restart() from outside the OS's own paths */
    case ESP_RST_INT_WDT:     /* wedged in an interrupt loop                   */
    case ESP_RST_TASK_WDT:    /* the frame path never yielded — C2's wedge      */
    case ESP_RST_WDT:         /* some other watchdog bit                       */
    case ESP_RST_CPU_LOCKUP:  /* double exception                              */
        return true;
    default:                  /* UNKNOWN, POWERON, EXT, USB, JTAG, SDIO,
                               * DEEPSLEEP, BROWNOUT, PWR_GLITCH, EFUSE: the
                               * owner or the supply moved the board.          */
        return false;
    }
}

static esp_err_t fault_word(int slot, const char *prefix, uint32_t *rw, bool write)
{
    nvs_handle_t h;
    const esp_err_t e = nvs_open_from_partition("storage", FAULTS_NS,
                                                write ? NVS_READWRITE : NVS_READONLY,
                                                &h);
    if (e != ESP_OK) return e;
    char key[16];
    snprintf(key, sizeof key, "%s%d", prefix, slot);
    esp_err_t r = ESP_OK;
    if (write) {
        r = nvs_set_u32(h, key, *rw);
        if (r == ESP_OK) r = nvs_commit(h);
    } else {
        uint32_t v = *rw;
        if (nvs_get_u32(h, key, &v) != ESP_OK) v = *rw;   /* absent = caller's seed */
        *rw = v;
    }
    nvs_close(h);
    return r;
}

static void fault_load_row(svc_row_t *r, int slot)
{
    r->faults_loaded = true;
    uint32_t v = 0;
    if (fault_word(slot, "c", &v, false) == ESP_OK) r->faults = v;
    v = 0;
    if (fault_word(slot, "x", &v, false) == ESP_OK) r->faulted = v != 0;
    if (r->faulted) {
        nvs_handle_t h;
        if (nvs_open_from_partition("storage", FAULTS_NS, NVS_READONLY, &h) == ESP_OK) {
            char key[16];
            snprintf(key, sizeof key, "r%d", slot);
            size_t n = sizeof r->fault_reason;
            if (nvs_get_str(h, key, r->fault_reason, &n) != ESP_OK)
                snprintf(r->fault_reason, sizeof r->fault_reason, "unexpected reset");
            nvs_close(h);
        }
    }
}

/* A reset the chip blames on the owner, not the code, proves nothing about the
 * app it interrupted — so it breaks the "two consecutive" chain C2 counts on
 * (plan §1.4: the rule is consecutive crashes, not any two resets in the
 * device's life) and heals a mark that chain had set from a reset alone. A
 * fault the worker recorded with its own reason stands until a reinstall: an
 * owner's reboot does not un-crash an app. A door, not a static: orb_svc_init
 * calls it on the owner's side of the verdict, and the host suite drives it
 * through the same front the broker does. */
void orb_svc_note_owner_reset(int slot)
{
    if (slot < 0 || slot >= ORB_SVC_ROWS) return;
    svc_row_t *r = &s_row[slot];
    if (!r->faults_loaded) fault_load_row(r, slot);
    if (r->faults) {
        r->faults = 0;
        uint32_t zero = 0;
        (void)fault_word(slot, "c", &zero, true);
    }
    if (r->faulted && !strcmp(r->fault_reason, "unexpected reset")) {
        r->faulted = false;
        r->fault_reason[0] = '\0';
        uint32_t zero = 0;
        (void)fault_word(slot, "x", &zero, true);
    }
}

static void note_fault_slot(int slot, const char *why)
{
    svc_row_t *r = &s_row[slot];
    if (!r->faults_loaded) fault_load_row(r, slot);
    r->faults++;
    uint32_t v = r->faults;
    fault_word(slot, "c", &v, true);
    if (r->faults >= 2u) {
        r->faulted = true;
        v = 1;
        fault_word(slot, "x", &v, true);
        snprintf(r->fault_reason, sizeof r->fault_reason, "%s",
                 why ? why : "unexpected reset");
        nvs_handle_t h;
        if (nvs_open_from_partition("storage", FAULTS_NS, NVS_READWRITE, &h) == ESP_OK) {
            char key[16];
            snprintf(key, sizeof key, "r%d", slot);
            nvs_set_str(h, key, r->fault_reason);
            nvs_commit(h);
            nvs_close(h);
        }
    }
    ESP_LOGE(TAG, "slot %d faulted (%s) — %u consecutive%s", slot,
             why ? why : "unexpected reset", (unsigned)r->faults,
             r->faulted ? ", marked FAILED until reinstall" : "");
}

void orb_svc_note_fault(const orb_app_t *a, const char *why)
{
    const int slot = orb_app_index_of(a);
    if (slot >= 0) note_fault_slot(slot, why);
}

uint32_t orb_svc_fault_count(const orb_app_t *a)
{
    const int slot = orb_app_index_of(a);
    if (slot < 0) return 0;
    if (!s_row[slot].faults_loaded) fault_load_row(&s_row[slot], slot);
    return s_row[slot].faults;
}

bool orb_svc_faulted(const orb_app_t *a)
{
    const int slot = orb_app_index_of(a);
    if (slot < 0) return false;
    if (!s_row[slot].faults_loaded) fault_load_row(&s_row[slot], slot);
    return s_row[slot].faulted;
}

const char *orb_svc_fault_reason(const orb_app_t *a)
{
    const int slot = orb_app_index_of(a);
    if (slot < 0 || !s_row[slot].faulted) return "";
    return s_row[slot].fault_reason;
}

esp_err_t orb_svc_clear_faults(orb_app_t *a)
{
    const int slot = orb_app_index_of(a);
    if (slot < 0) return ESP_ERR_INVALID_ARG;
    if (!s_row[slot].faults_loaded) fault_load_row(&s_row[slot], slot);
    s_row[slot].faults = 0;
    s_row[slot].faulted = false;
    s_row[slot].fault_reason[0] = '\0';
    uint32_t zero = 0;
    esp_err_t e = fault_word(slot, "c", &zero, true);
    zero = 0;
    if (e == ESP_OK) e = fault_word(slot, "x", &zero, true);
    nvs_handle_t h;
    if (nvs_open_from_partition("storage", FAULTS_NS, NVS_READWRITE, &h) == ESP_OK) {
        char key[16];
        snprintf(key, sizeof key, "r%d", slot);
        nvs_erase_key(h, key);       /* absent = ESP_ERR_NVS_NOT_FOUND: nothing
                                      * stored, nothing to do — not an error */
        nvs_commit(h);
        nvs_close(h);
    }
    return e;
}

/* The boot protocol, run by the ladder in main.c and the registry:
 * orb_svc_init arms the record, the first frame past the ladder moves it to
 * RUN, every activation keeps the slot honest, and the OS's own reboot paths
 * disarm it so the owner's reboot never bills an app. */
void orb_svc_boot_done(void)
{
    if (s_rec.magic == FAULT_MAGIC_ARM) {
        s_rec.magic  = FAULT_MAGIC_RUN;
        s_rec.booted = 1;
        const orb_app_t *c = orb_app_current();
        s_rec.slot = c ? (uint32_t)orb_app_index_of(c) : FAULT_SLOT_NONE;
    }
}

void orb_svc_current_note(const orb_app_t *a)
{
    if (s_rec.magic == FAULT_MAGIC_RUN) s_rec.slot = (uint32_t)orb_app_index_of(a);
}

void orb_svc_note_clean_reboot(void)
{
    s_rec.magic = 0;
    s_rec.slot = FAULT_SLOT_NONE;
    s_rec.booted = 0;
}

/* The registry calls this where it suspends the outgoing entry
 * (orb_app.c): §4.2's "auto-revoked when the app is not visible", executed
 * by the one place that knows visibility rather than trusted to the app.
 * Only the power ask is revoked — the beat stops with the tick delivery,
 * and the config bytes are the app's to keep. */
void orb_svc_app_suspended(const orb_app_t *a)
{
    const int slot = orb_app_index_of(a);
    if (slot < 0) return;                      /* not a registered entry */
    idle_dim_app_release(a->slug);
    if (s_row[slot].layers)                    /* P5.5: an app behind the dial
                                              * holding rows on the glass is
                                              * §4.2's clause's other half. The
                                              * wipe frees the planes and KEEPS
                                              * the table: generations survive,
                                              * so every held handle stays dead
                                              * through suspend and resume —
                                              * and resume pays no new table. */
        orb_layers_wipe(s_row[slot].layers);
}