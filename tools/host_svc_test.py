#!/usr/bin/env python3
"""host_svc_test.py — the broker, tested on the host.

Why this file exists
--------------------
main/app/orb_svc.c is the file the whole platform rests on: it is the ONE
gate, and "no code path lets an app call below its grant" (§8, P3 pass line)
is not a property anybody can see on the dial. A denial that is not counted
is invisible to the owner; a stub wired in place of a gate passes every
app-side smoke test while shipping a broker that does not broker. So the
shipped orb_svc.c is compiled VERBATIM here against the real headers, with
only the leaves stubbed — not re-typed in Python. If the struct's field names
ever drift from the dispatch table, this suite fails to build rather than
passing against a fiction, which is the whole reason the table uses designated
initializers.

What is under test
------------------
  the gate        free caps pass unconditionally and are still counted; owner
                  caps refuse with ESP_ERR_NOT_ALLOWED, count the denial, and
                  latch the ask once (the prompt reads pending, not logs)
  grants          set_grant persists "g<slot>" in orb.grants BEFORE caching, so
                  a refused NVS write cannot leave the dial showing a grant the
                  reboot undoes; free caps are not the owner's to give or take;
                  orb_svc_init preloads and masks free bits out of stored words
  set_beat (D16)  may only slow from the manifest floor or stop; faster is
                  refused; an app that declared no beat gets none
  config (C9)     undeclared keys and wrong-typed keys never reach NVS at all
                  (the opens counter is the proof), ranges and the two toggle
                  words are enforced, absent reads return the manifest default
  quota (§9.4)    string bytes charge, refund on overwrite, and the second
                  -D build with a tiny quota prices the refusal path
  deferred stubs  each sits BEHIND its gate: denied ungranted (counted), and
                  NOT_SUPPORTED once granted (the body ran) — gate and stub are
                  separately observable
  status          what an app printed is what the OS pages will read

Run:  python3 tools/host_svc_test.py
"""

import os, re, subprocess, sys, tempfile

ROOT = (sys.argv[sys.argv.index("--root") + 1] if "--root" in sys.argv
        else os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
P = lambda *a: os.path.join(ROOT, "main", *a)
SVC_C = P("app", "orb_svc.c")
for p in (SVC_C, P("app", "orb_svc.h"), P("app", "orb_app.h"),
          P("app", "orb_jobs.h"), P("art", "art_bank.h")):
    if not os.path.exists(p):
        sys.exit("missing " + p + " — this suite compiles the shipped file")

STUBS = r'''
/* ---- the broker, verbatim, against stub leaves and an in-memory NVS -------- */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "esp_err.h"
#include "esp_log.h"
#include "nvs.h"
#include "orb_app.h"
#include "orb_svc.h"
#include "orb_theme.h"

/* The NVS store. Keys are namespace+key pairs, values u32 or string, exactly
 * the shapes orb_svc.c uses. The counters are half the suite: "an undeclared
 * key never reaches NVS" is only observable if somebody counts the opens. */
typedef struct { char ns[16], key[16]; int is_str; uint32_t u32; char s[64]; } ent;
static ent  S[256];
static int  SN;
static char OPEN_NS[16][16];
static int  OPEN_USED[16];
int g_opens, g_reads, g_writes, g_commits;
int g_fail_open, g_fail_commit;

static ent *find_ent(const char *ns, const char *k)
{
    for (int i = 0; i < SN; i++)
        if (!strcmp(S[i].ns, ns) && !strcmp(S[i].key, k)) return &S[i];
    return NULL;
}
static ent *new_ent(const char *ns, const char *k)
{
    ent *e = find_ent(ns, k);
    if (e) return e;
    if (SN >= 256) return NULL;
    e = &S[SN++];
    snprintf(e->ns, sizeof e->ns, "%s", ns);
    snprintf(e->key, sizeof e->key, "%s", k);
    return e;
}

esp_err_t nvs_open_from_partition(const char *part, const char *ns,
                                  nvs_open_mode_t m, nvs_handle_t *h)
{
    (void)part; (void)m;
    g_opens++;
    if (g_fail_open) return ESP_FAIL;
    for (int i = 0; i < 16; i++)                /* a closed handle frees its
                                                * slot — real handles do too,
                                                * and a store that leaks them
                                                * would fail the suite's 20th
                                                * open for the wrong reason */
        if (!OPEN_USED[i]) {
            snprintf(OPEN_NS[i], sizeof OPEN_NS[0], "%s", ns);
            OPEN_USED[i] = 1;
            *h = (nvs_handle_t)i;
            return ESP_OK;
        }
    return ESP_ERR_NO_MEM;
}
esp_err_t nvs_get_u32(nvs_handle_t h, const char *k, uint32_t *v)
{
    g_reads++;
    ent *e = find_ent(OPEN_NS[h], k);
    if (!e || e->is_str) return ESP_ERR_NVS_NOT_FOUND;
    *v = e->u32;
    return ESP_OK;
}
esp_err_t nvs_set_u32(nvs_handle_t h, const char *k, uint32_t v)
{
    g_writes++;
    ent *e = new_ent(OPEN_NS[h], k);
    if (!e) return ESP_ERR_NO_MEM;
    e->is_str = 0; e->u32 = v; e->s[0] = 0;
    return ESP_OK;
}
esp_err_t nvs_get_str(nvs_handle_t h, const char *k, char *buf, size_t *sz)
{
    g_reads++;
    ent *e = find_ent(OPEN_NS[h], k);
    if (!e || !e->is_str) return ESP_ERR_NVS_NOT_FOUND;
    const size_t n = strlen(e->s) + 1;
    if (!buf) { *sz = n; return ESP_OK; }        /* IDF's size-probe shape */
    if (*sz < n) { *sz = n; return ESP_ERR_INVALID_SIZE; }
    memcpy(buf, e->s, n);
    *sz = n;
    return ESP_OK;
}
esp_err_t nvs_set_str(nvs_handle_t h, const char *k, const char *v)
{
    g_writes++;
    ent *e = new_ent(OPEN_NS[h], k);
    if (!e) return ESP_ERR_NO_MEM;
    e->is_str = 1; e->u32 = 0;
    snprintf(e->s, sizeof e->s, "%s", v);
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; g_commits++; return g_fail_commit ? ESP_FAIL : ESP_OK; }
void      nvs_close(nvs_handle_t h) { if (h < 16) OPEN_USED[h] = 0; }
esp_err_t nvs_erase_key(nvs_handle_t h, const char *k)
{
    ent *e = find_ent(OPEN_NS[h], k);
    if (!e) return ESP_ERR_NVS_NOT_FOUND;
    *e = S[--SN];                    /* swap-remove: the store is order-free */
    return ESP_OK;
}
esp_err_t nvs_flash_init_partition(const char *p) { (void)p; return ESP_OK; }
const char *esp_err_to_name(esp_err_t e)
{
    static char b[24];
    snprintf(b, sizeof b, "0x%x", (unsigned)e);
    return b;
}

/* The fake registry: three entries with the manifest policy the broker reads
 * (D16 floor, declared settings). orb_svc.c reaches the table only through
 * orb_app_index_of(), which is the whole interface this stub must honour. */
static orb_setting_desc_t A_SET[] = {
    { "bright", ORB_SET_INT,    0, 100, 50, ""     },
    { "mode",   ORB_SET_ENUM,   0,   3,  1, ""     },
    { "on",     ORB_SET_TOGGLE, 0,   1,  1, ""     },
    { "name",   ORB_SET_STRING, 0,   0,  0, "anon" },
    { "nick",   ORB_SET_STRING, 0,   0,  0, ""     },
    { "bio",    ORB_SET_STRING, 0,   0,  0, ""     },
};
static orb_app_t TBL[3];
static orb_app_t *GHOST;              /* an app the table does not know */
static void mk_tbl(void)
{
    memset(TBL, 0, sizeof TBL);
    TBL[0].slug = "alpha"; TBL[0].period_ms = 67; TBL[0].art_slot = -1;
    TBL[0].settings = A_SET; TBL[0].n_settings = 6;
    TBL[1].slug = "beta";  TBL[1].period_ms = 0;  TBL[1].art_slot = -1;
    TBL[2].slug = "gamma"; TBL[2].period_ms = 67; TBL[2].art_slot = 2;
    static orb_app_t ghost; memset(&ghost, 0, sizeof ghost);
    ghost.slug = "ghost"; ghost.period_ms = 67; ghost.art_slot = -1;
    GHOST = &ghost;
}
int orb_app_index_of(const orb_app_t *a)
{
    for (int i = 0; i < 3; i++) if (&TBL[i] == a) return i;
    return -1;
}
static orb_app_t *g_cur;                /* the entry on the glass, for the
                                         * boot-RUN stamp */
const orb_app_t *orb_app_current(void) { return g_cur; }

/* Leaves the broker delegates to. */
static uint8_t BANKMEM[8];
int g_bank_calls;
const uint8_t *art_bank_extra_ptr(int slot)
{
    g_bank_calls++;
    return slot < 0 ? 0 : BANKMEM;
}
int g_post_calls;
esp_err_t g_post_rc;
esp_err_t orb_jobs_post(orb_job_fn fn, void *arg, size_t n)
{
    (void)fn; (void)arg; (void)n;
    g_post_calls++;
    return g_post_rc;
}

/* ---- the time leaves: everything orb_time_now reads, scripted ------------- */
int64_t g_timer_us;           /* the esp_timer half of the anchor pair  */
int64_t esp_timer_get_time(void) { return g_timer_us; }
time_t  g_real;               /* the time() half — scripted, never the host RTC */
time_t time(time_t *tloc) { if (tloc) *tloc = g_real; return g_real; }
bool    g_sync;
bool    orb_net_time_synced(void) { return g_sync; }
static const char TZ_RET[] = "America/New_York";
const char *orb_net_tz_current(void) { return TZ_RET; }
int g_tzoff_calls; int32_t g_tzoff_val;
esp_err_t orb_net_tz_offset(time_t when, int32_t *off)
{
    (void)when;
    if (!off) return ESP_ERR_INVALID_ARG;
    g_tzoff_calls++;
    *off = g_tzoff_val;
    return ESP_OK;
}

/* ---- the power leaves: what the broker's request path must call ----------- */
int g_pwr_calls;                       /* app_request invocations */
int g_rel_calls;                       /* app_release invocations */
char g_pwr_slug[24]; uint32_t g_pwr_to; int g_pwr_hold;
char g_rel_slug[24];
esp_err_t g_pwr_rc;
esp_err_t idle_dim_app_request(const char *slug, uint32_t timeout_s, bool hold)
{
    g_pwr_calls++;
    snprintf(g_pwr_slug, sizeof g_pwr_slug, "%s", slug ? slug : "");
    g_pwr_to = timeout_s; g_pwr_hold = hold ? 1 : 0;
    return g_pwr_rc;
}
void idle_dim_app_release(const char *slug)
{
    g_rel_calls++;
    snprintf(g_rel_slug, sizeof g_rel_slug, "%s", slug ? slug : "");
}

/* ---- the theme leaves: the shipped package manager, scripted ---------------- */
int g_tscan_calls;
static orb_theme_entry_t g_te[2];
int orb_theme_scan(orb_theme_entry_t *out, int cap)
{
    g_tscan_calls++;
    const int n = cap < 2 ? cap : 2;
    for (int i = 0; i < n; i++) out[i] = g_te[i];
    return n;
}
bool orb_theme_id_ok(const char *id)
{
    if (!id || strlen(id) != ORB_THEME_ID_HEX) return false;
    for (const char *p = id; *p; p++)            /* hex only, like the shipped
                                                  * gate: a 12-char path is still
                                                  * a path — which is the point
                                                  * of this vector */
        if (!(((*p >= '0') && (*p <= '9')) || ((*p >= 'a') && (*p <= 'f'))))
            return false;
    return true;
}
int g_pinst_calls; char g_pinst_id[16]; esp_err_t g_pinst_rc;
esp_err_t orb_theme_post_install(const char *id)
{
    g_pinst_calls++;
    snprintf(g_pinst_id, sizeof g_pinst_id, "%s", id);
    return g_pinst_rc;
}

static void jobfn(void *arg) { (void)arg; }

static int g_fail;
static void ck(const char *name, int ok, const char *expr)
{
    printf("%-6s %s%s%s\n", ok ? "ok" : "FAIL", name,
           ok ? "" : "   << ", ok ? "" : expr);
    if (!ok) g_fail = 1;
}
#define CK(name, cond) ck(name, (cond), #cond)
static uint32_t u32_at(const char *ns, const char *k)
{
    ent *e = find_ent(ns, k);
    return (e && !e->is_str) ? e->u32 : 0xDEADu;
}

int main(int argc, char **argv)
{
    const int tiny = argc > 1 && !strcmp(argv[1], "tiny");
    if (!tiny && ORB_CFG_QUOTA_BYTES != 2048)
        ck("shipped quota is the chosen 2048", 0, "ORB_CFG_QUOTA_BYTES");
    mk_tbl();
    orb_app_t *A = &TBL[0], *B = &TBL[1], *G = &TBL[2];
    const orb_svc_t *S = orb_svc_table();

    /* The frozen table: every paper §4.3 entry present — the designated-
     * initializer shape makes a rename a build error; this makes an omission
     * a test failure. */
    int holes = 0;
    #define F(f) do { if (!S->f) holes++; } while (0)
    F(now); F(tz_label); F(tz_offset); F(power_request);
    F(cfg_get); F(cfg_set); F(cfg_get_str); F(cfg_set_str);
    F(http_get); F(bank); F(layer_add); F(layer_set); F(layer_touch); F(layer_del);
    F(job_post); F(set_beat);
    F(theme_count); F(theme_id); F(theme_active); F(theme_changed); F(theme_note);
    F(theme_install); F(status);
    CK("all 23 frozen entries wired", holes == 0);

    /* ---- the wall clock: one scripted timeline, the compose_now rules ---- */
    time_t sec; double frac; bool synced;
    uint32_t c0 = orb_svc_calls(A);
    g_timer_us = 0; g_real = 0; g_sync = false;
    S->now(A, &sec, &frac, &synced);
    CK("first call seeds the pair and free-runs from CLOCK_EPOCH",
       sec == 1700000000 && frac == 0.0 && !synced);
    CK("free calls are counted", orb_svc_calls(A) == c0 + 1);
    g_timer_us = 3500000;                    /* +3.5 s, still untrusted */
    S->now(A, &sec, &frac, &synced);
    CK("free-running second and sweep fraction come from one pair",
       sec == 1700000003 && frac >= 0.4999 && frac <= 0.5001 && !synced);
    CK("bank: no slot means no pointer", S->bank(A) == 0 && g_bank_calls == 1);
    CK("bank: the app's own slot maps", S->bank(G) != 0);
    g_post_rc = ESP_OK;
    CK("job_post forwards", S->job_post(A, jobfn, 0, 0) == ESP_OK && g_post_calls == 1);
    g_post_rc = ESP_ERR_INVALID_STATE;
    CK("the worker's busy gate reaches the app intact",
       S->job_post(A, jobfn, 0, 0) == ESP_ERR_INVALID_STATE);
    /* warm boot: the RTC-kept epoch stands on its own — D15's trust rule */
    g_real = (time_t)1700000100;             /* sync still false */
    S->now(A, &sec, &frac, &synced);
    CK("epoch past the baseline is trusted without the SNTP flag",
       sec == (time_t)1700000100 && fabs(frac) < 1e-9 && !synced);
    /* small drift holds: re-anchoring every tick is the visible jump the
     * slack exists to prevent — base stays, so sec lands BELOW time() */
    g_real = (time_t)1700000102; g_timer_us = 4500000;   /* drift 1.0 s < 2 s */
    S->now(A, &sec, &frac, &synced);
    CK("sub-slack drift holds the anchor (no jump to real)",
       sec == (time_t)1700000101);
    /* SNTP lands with a big step: base moves, and the SAME call reports off
     * the new pair — el recomputes after the re-anchor, verbatim shipped */
    g_real = (time_t)1700005000; g_sync = true; g_timer_us = 5000000;
    S->now(A, &sec, &frac, &synced);
    CK("big trusted drift re-anchors and the read reflects it",
       sec == (time_t)1700005000 && fabs(frac) < 1e-9 && synced);
    /* power cut at the far side: time() drops to 1970, the reading must not —
     * a stale dial beats a 1970 dial, verbatim from the shipped comment */
    g_real = 0; g_sync = false; g_timer_us = 6000000;
    S->now(A, &sec, &frac, &synced);
    CK("untrusted again, free-running continues off the real anchor",
       sec == (time_t)1700005001 && !synced);
    /* the zone surface (D17): label out, offset measured, both counted */
    CK("tz_label answers with the owner's zone key",
       S->tz_label(A) == TZ_RET);
    c0 = orb_svc_calls(A);
    g_tzoff_val = -330;                      /* +5:30 west, the shape the */
    int32_t off = 12345;                     /* shipped echo table carries */
    CK("tz_offset delegates the owner's zone at the asked instant",
       S->tz_offset(A, (time_t)1700005002, &off) == ESP_OK &&
       off == -330 && g_tzoff_calls == 1);
    CK("time entries are counted, not gated", orb_svc_calls(A) > c0);

    /* ---- owner caps: denied, counted, and latched once (B3 input) --------- */
    uint32_t d0 = orb_svc_denials(A);
    CK("power refused ungranted", S->power_request(A, 5, true) == ESP_ERR_NOT_ALLOWED);
    CK("and the leaf was never touched", g_pwr_calls == 0 && g_rel_calls == 0);
    CK("denial counted", orb_svc_denials(A) == d0 + 1);
    CK("ask latched for the prompt", orb_svc_pending(A) == (1u << CAP_POWER));
    S->power_request(A, 5, true);
    CK("repeat counted, latch unchanged",
       orb_svc_denials(A) == d0 + 2 && orb_svc_pending(A) == (1u << CAP_POWER));
    CK("themes refused ungranted (int form)", S->theme_count(A) == -1);

    /* ---- grants: write-through persistence, free caps untouchable --------- */
    CK("granting a free cap is refused",
       orb_svc_set_grant(A, CAP_TIME, true) == ESP_ERR_INVALID_ARG);
    CK("owner grant sticks", orb_svc_set_grant(A, CAP_POWER, true) == ESP_OK);
    CK("granted mask reads back", orb_svc_grants(A) == (1u << CAP_POWER));
    CK("stored as orb.grants/g<slot>", u32_at("orb.grants", "g0") == (1u << CAP_POWER));
    d0 = orb_svc_denials(A);
    CK("granted power reaches the idle_dim door",
       S->power_request(A, 30, false) == ESP_OK &&
       g_pwr_calls == 1 && !strcmp(g_pwr_slug, "alpha") &&
       g_pwr_to == 30 && g_pwr_hold == 0);
    CK("hold form asks for the hold", S->power_request(A, 0, true) == ESP_OK &&
       g_pwr_hold == 1);
    CK("the (0,false) pair is the withdraw, not a request",
       S->power_request(A, 0, false) == ESP_OK &&
       g_rel_calls == 1 && !strcmp(g_rel_slug, "alpha") && g_pwr_calls == 2);
    orb_svc_app_suspended(A);
    CK("leaving the glass revokes the ask through the same holder door",
       g_rel_calls == 2 && !strcmp(g_rel_slug, "alpha"));
    CK("body-not-gate: no denial added", orb_svc_denials(A) == d0);
    g_fail_commit = 1;
    CK("refused NVS commit fails the set_grant",
       orb_svc_set_grant(A, CAP_POWER, false) == ESP_FAIL);
    CK("cache did not run ahead of flash", orb_svc_grants(A) == (1u << CAP_POWER));
    g_fail_commit = 0;
    CK("revoke sticks once NVS accepts",
       orb_svc_set_grant(A, CAP_POWER, false) == ESP_OK);
    CK("denies again", S->power_request(A, 5, true) == ESP_ERR_NOT_ALLOWED);
    orb_svc_prompt_shown(A, 1u << CAP_POWER);
    /* Only the shown cap clears: the earlier ungranted theme_count latched
     * THEMES too, and a prompt that ate every pending bit it did not show
     * would silently drop the asks still waiting on the owner. */
    CK("prompt clearing only clears what was shown",
       orb_svc_pending(A) == (1u << CAP_THEMES));
    orb_svc_prompt_shown(A, 1u << CAP_THEMES);
    CK("and the rest clears when it is shown too", orb_svc_pending(A) == 0);

    /* preload masks free bits a hand-rolled store could carry */
    ent *g1 = new_ent("orb.grants", "g1"); g1->is_str = 0;
    g1->u32 = (1u << CAP_THEMES) | (1u << CAP_TIME);
    orb_svc_init();
    CK("init preloads, free bits masked out", orb_svc_grants(B) == (1u << CAP_THEMES));
    CK("an id that never came off a scan is refused on shape alone",
       S->theme_install(B, "kit3") == ESP_ERR_INVALID_ARG && g_pinst_calls == 0);

    /* ---- the D18 family on its real bodies -------------------------------- */
    snprintf(g_te[0].id, sizeof g_te[0].id, "%s", "0123456789ab");
    snprintf(g_te[0].name, sizeof g_te[0].name, "%s", "kit3");
    g_te[0].err = ESP_OK; g_te[0].active = true; g_te[0].changed = false;
    g_te[0].why[0] = '\0';
    snprintf(g_te[1].id, sizeof g_te[1].id, "%s", "fedcba987654");
    snprintf(g_te[1].name, sizeof g_te[1].name, "%s", "broken");
    g_te[1].err = ESP_FAIL; g_te[1].active = false;
    snprintf(g_te[1].why, sizeof g_te[1].why, "%s", "bad payload crc");
    uint32_t den0 = orb_svc_denials(A);
    CK("ungranted count never reaches the card",
       S->theme_count(A) == -1 && g_tscan_calls == 0 &&
       orb_svc_denials(A) == den0 + 1);
    CK("granted count walks the card", S->theme_count(B) == 2 && g_tscan_calls == 1);
    char tid[16];
    CK("id hands back the scan's, not a path",
       S->theme_id(B, 0, tid, sizeof tid) == ESP_OK && !strcmp(tid, "0123456789ab"));
    CK("row out of range is a refusal, not a clamp",
       S->theme_id(B, 2, tid, sizeof tid) == ESP_ERR_NOT_FOUND);
    CK("active flag answers from the copy",
       S->theme_active(B, 0) && !S->theme_active(B, 1));
    CK("changed flag answers from the copy",
       !S->theme_changed(B, 0) && !S->theme_changed(B, 1));
    CK("note carries the damage reason, clean rows get none",
       S->theme_note(B, 1) && !strcmp(S->theme_note(B, 1), "bad payload crc") &&
       S->theme_note(B, 0) == NULL);
    CK("a path is refused before anything else, count or no count",
       S->theme_install(B, "../../sd/etc") == ESP_ERR_INVALID_ARG && g_pinst_calls == 0);
    CK("a well-formed id the scan never published is refused",
       S->theme_install(B, "aaaaaaaaaaaa") == ESP_ERR_NOT_FOUND && g_pinst_calls == 0);
    CK("a damaged entry cannot be installed either",
       S->theme_install(B, "fedcba987654") == ESP_ERR_NOT_FOUND && g_pinst_calls == 0);
    CK("the good scanned id reaches the D11 package manager",
       S->theme_install(B, "0123456789ab") == ESP_OK &&
       g_pinst_calls == 1 && !strcmp(g_pinst_id, "0123456789ab"));
    CK("install behind a closed gate: ungranted alpha cannot even ask",
       S->theme_install(A, "0123456789ab") == ESP_ERR_NOT_ALLOWED && g_pinst_calls == 1);
    /* ---- set_beat (D16): the manifest floor is the contract --------------- */
    CK("faster than the manifest refused", S->set_beat(A, 33) == ESP_ERR_INVALID_ARG);
    CK("the stored beat did not move", orb_svc_beat_ms(A) == 0);
    CK("slower accepted", S->set_beat(A, 100) == ESP_OK && orb_svc_beat_ms(A) == 100);
    CK("exactly the floor accepted", S->set_beat(A, 67) == ESP_OK && orb_svc_beat_ms(A) == 67);
    CK("stop accepted", S->set_beat(A, 0) == ESP_OK && orb_svc_beat_ms(A) == 0);
    CK("an app with no declared beat gets none",
       S->set_beat(B, 50) == ESP_ERR_NOT_SUPPORTED);

    /* ---- config (C9): the declaration IS the grant ------------------------ */
    int32_t out; char buf[40];
    int opens0 = g_opens;
    CK("undeclared key refused", S->cfg_get(A, "secret", &out) == ESP_ERR_NOT_FOUND);
    CK("wrong-typed key refused", S->cfg_get(A, "name", &out) == ESP_ERR_NOT_FOUND);
    CK("neither ever reached NVS", g_opens == opens0);
    CK("absent int reads the declared default",
       S->cfg_get(A, "bright", &out) == ESP_OK && out == 50);
    CK("out of range refused", S->cfg_set(A, "bright", 101) == ESP_ERR_INVALID_ARG);
    CK("in range stored", S->cfg_set(A, "bright", 60) == ESP_OK);
    CK("read back", S->cfg_get(A, "bright", &out) == ESP_OK && out == 60);
    CK("toggle accepts only its two words",
       S->cfg_set(A, "on", 2) == ESP_ERR_INVALID_ARG && S->cfg_set(A, "on", 0) == ESP_OK);
    CK("string default",
       S->cfg_get_str(A, "name", buf, sizeof buf) == ESP_OK && !strcmp(buf, "anon"));
    CK("string stored", S->cfg_set_str(A, "name", "greg") == ESP_OK);
    CK("string read back",
       S->cfg_get_str(A, "name", buf, sizeof buf) == ESP_OK && !strcmp(buf, "greg"));
    CK("bytes charged with the NUL", orb_svc_cfg_used(A) == 5);
    static const char Z32[33] = "01234567890123456789012345678901";
    CK("32 chars refused against the manifest's 31",
       S->cfg_set_str(A, "nick", Z32) == ESP_ERR_INVALID_SIZE);
    if (tiny) {
        /* quota 24: name(9) + nick(9) = 18; a third 9-byte value crosses.
         * Then the refund: shrinking nick must make the room come back. */
        CK("fill to the ceiling",
           S->cfg_set_str(A, "name", "abcdefgh") == ESP_OK &&
           S->cfg_set_str(A, "nick", "abcdefgh") == ESP_OK);
        CK("past the ceiling refused",
           S->cfg_set_str(A, "bio", "abcdefgh") == ESP_ERR_INVALID_SIZE);
        CK("shrink refunds", S->cfg_set_str(A, "nick", "ab") == ESP_OK &&
           orb_svc_cfg_used(A) == 12);
        CK("the room it made is really there",
           S->cfg_set_str(A, "bio", "abcdefgh") == ESP_OK &&
           orb_svc_cfg_used(A) == 21);
    }

    /* ---- C2: the fault rule — two strikes, persisted, clearable ---------- */
    CK("RUN with any boot flag bills", orb_svc_reset_verdict(0x52554E21u, 1u) == 1 &&
       orb_svc_reset_verdict(0x52554E21u, 0u) == 1);
    CK("ARM unbooted bills; ARM booted does not",
       orb_svc_reset_verdict(0x41524D21u, 0u) == 1 &&
       orb_svc_reset_verdict(0x41524D21u, 1u) == 0);
    CK("cold-boot garbage and the disarmed word bill nobody",
       orb_svc_reset_verdict(0xDEADBEEFu, 7u) == 0 && orb_svc_reset_verdict(0u, 0u) == 0);
    orb_svc_note_fault(B, "unexpected reset");
    CK("first fault counts, does not fail",
       orb_svc_fault_count(B) == 1 && !orb_svc_faulted(B));
    orb_svc_note_fault(B, "unexpected reset");
    CK("second consecutive fault marks failed with a reason",
       orb_svc_faulted(B) && orb_svc_fault_count(B) == 2 &&
       orb_svc_fault_reason(B)[0] != '\0');
    {
        ent *x1 = find_ent("orb.fault", "x1");
        ent *c1 = find_ent("orb.fault", "c1");
        CK("both words persisted, not just the RAM claim",
           x1 && !x1->is_str && x1->u32 == 1 && c1 && !c1->is_str && c1->u32 == 2);
    }
    CK("clear_faults lifts the ban (P4 reinstall door)",
       orb_svc_clear_faults(B) == ESP_OK && !orb_svc_faulted(B) &&
       orb_svc_fault_count(B) == 0);
    {
        ent *x1 = find_ent("orb.fault", "x1");
        CK("and the NVS words follow the RAM back down",
           x1 && !x1->is_str && x1->u32 == 0);
    }

    /* ---- status / ghost ---------------------------------------------------- */
    S->status(A, "beat %d", 7);
    CK("status line stored for the OS page", !strcmp(orb_svc_last_status(A), "beat 7"));
    S->status(A, "he\"said\"no\\path");
    CK("the status echo carries no quote or backslash into the device's JSON",
       !strstr(orb_svc_last_status(A), "\"") &&
       !strchr(orb_svc_last_status(A), '\\') &&
       orb_svc_last_status(A)[0] == 'h');
    opens0 = g_opens;
    CK("an app outside the table gets nothing, even a free cap",
       S->power_request(GHOST, 1, true) == ESP_ERR_NOT_ALLOWED);
    CK("and its cfg never reaches NVS",
       S->cfg_set(GHOST, "bright", 5) == ESP_ERR_NOT_ALLOWED && g_opens == opens0);
    printf("%s\n", g_fail ? "SOME FAILED" : "ALL PASS");
    return g_fail ? 1 : 0;
}
'''

HDRS = {
    "esp_err.h": "#pragma once\ntypedef int esp_err_t;\n"
                 "#define ESP_OK 0\n#define ESP_FAIL -1\n"
                 "#define ESP_ERR_NO_MEM 0x101\n"
                 "#define ESP_ERR_INVALID_ARG 0x102\n"
                 "#define ESP_ERR_INVALID_SIZE 0x103\n"
                 "#define ESP_ERR_INVALID_STATE 0x104\n"
                 "#define ESP_ERR_NOT_FOUND 0x106\n"
                 "#define ESP_ERR_NOT_SUPPORTED 0x107\n"
                 "#define ESP_ERR_NOT_ALLOWED 0x108\n"
                 "const char *esp_err_to_name(esp_err_t);\n",
    "esp_log.h": "#pragma once\n#define ESP_LOGI(t,...)\n#define ESP_LOGE(t,...)\n"
                 "#define ESP_LOGW(t,...)\n#define ESP_LOGD(t,...)\n",
    "esp_timer.h": "#pragma once\n#include <stdint.h>\n"
                   "int64_t esp_timer_get_time(void);\n",
    "esp_partition.h": "#pragma once\n#include <stddef.h>\n#include <stdint.h>\n"
                       "typedef struct esp_partition esp_partition_t;\n",
    "nvs_flash.h": "#pragma once\n#include \"esp_err.h\"\n"
                   "esp_err_t nvs_flash_init_partition(const char *);\n",
    "nvs.h": "#pragma once\n#include <stddef.h>\n#include <stdint.h>\n"
             "#include \"esp_err.h\"\n"
             "typedef uintptr_t nvs_handle_t;\n"
             "typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;\n"
             "#define ESP_ERR_NVS_NOT_FOUND 0x1102\n"
             "esp_err_t nvs_open_from_partition(const char *, const char *, "
             "nvs_open_mode_t, nvs_handle_t *);\n"
             "esp_err_t nvs_get_u32(nvs_handle_t, const char *, uint32_t *);\n"
             "esp_err_t nvs_set_u32(nvs_handle_t, const char *, uint32_t);\n"
             "esp_err_t nvs_get_str(nvs_handle_t, const char *, char *, size_t *);\n"
             "esp_err_t nvs_set_str(nvs_handle_t, const char *, const char *);\n"
             "esp_err_t nvs_commit(nvs_handle_t);\n"
             "void nvs_close(nvs_handle_t);\n"
             "esp_err_t nvs_erase_key(nvs_handle_t, const char *);\n"
             "esp_err_t nvs_flash_init_partition(const char *);\n",
}


def build_run(td, tag, extra_cflags, arg):
    """Compile the SHIPPED orb_svc.c with the harness; run it."""
    cpath = os.path.join(td, "svc_harness_%s.c" % tag)
    with open(cpath, "w") as f:
        f.write(STUBS)
    exe = os.path.join(td, "svc_harness_%s" % tag)
    cmd = (["cc", "-std=gnu17", "-g", "-O0", "-Wall", "-Wno-unused-parameter",
            "-I", td]                          # the IDF-shaped stubs shadow mine
           + ["-I", P("app"), "-I", P("art"), "-I", P("net"),
              "-I", P("power"), "-I", os.path.join(ROOT, "assets")]  # real project headers
           + extra_cflags + [SVC_C, cpath, "-lm", "-o", exe])
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        return None, "COMPILE FAILED\n" + r.stdout + r.stderr
    run = subprocess.run([exe] + ([arg] if arg else []),
                         capture_output=True, text=True, timeout=30)
    return run.returncode, run.stdout + run.stderr


def main():
    with tempfile.TemporaryDirectory() as td:
        for n, body in HDRS.items():
            with open(os.path.join(td, n), "w") as f:
                f.write(body)
        rc, out = build_run(td, "std", [], None)
        print("== default build (shipped quota) ==")
        print(out)
        if rc is None or rc != 0:
            sys.exit(1)
        rc, out = build_run(td, "tiny", ["-DORB_CFG_QUOTA_BYTES=24"], "tiny")
        print("== tiny-quota build ==")
        print(out)
        sys.exit(0 if rc == 0 else 1)


if __name__ == "__main__":
    main()
