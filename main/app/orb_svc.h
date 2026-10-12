#pragma once
/**
 * orb_svc — the broker. The entire surface an app may call (plan §4.2/§4.3).
 *
 * P3. Ruling D15 (§0.5) signed this table as the ONLY thing the OS registers:
 * raw `time_t` out, no `struct tm`, no behaviour vocabulary across the line.
 * D16 (§0.6) added `set_beat` under the manifest's `period_ms` floor; D17
 * (§0.7) added `tz_offset`; D18 (§0.7) added the theme family. The struct
 * below is that paper made code, field for field and in the paper's order —
 * if they ever disagree, the paper wins and this file is wrong.
 *
 * Enforcement is the one gate, §4.2: every entry checks the grant table keyed
 * by the app's registry slot BEFORE doing anything, so neither a compiled-in
 * app (P3's only tier) nor a wasm3 guest (P7's) can call below its grant.
 * The gate also counts: calls and denials per app are what /api/v1/status
 * shows for the P3 pass line, and the asked-while-denied bits are what the
 * dial's grant prompt (B3) reads.
 *
 * Threading. Entry functions run on the LVGL task like every other app call,
 * with one exception by design: the config entries reach NVS and may block on
 * a sector erase, so they carry the rule the menu's header prints — call them
 * from a worker job (SVC->job_post), not from a tick. Nothing else here
 * allocates, sleeps, or touches a widget.
 *
 * Grant/config persistence. Slot-indexed namespaces on the `storage`
 * partition, the retention-protected one (partitions.csv):
 *   orb.grants   key "g<slot>"   u32 bitmask of the OWNER-GRANTED caps
 *   orb.app<slot>                one namespace per app; keys are the setting
 *                                ids themselves, so an app cannot address
 *                                another app's keys even by bug
 * The slot is the registry index: stable across boots because the table order
 * IS boot order (orb_app.c), and P4 pins it in the catalog. The 15-char NVS
 * key limit rules out any slug-keyed scheme — these are the "real key shapes"
 * §9.4 item 4 waited for to pick the quota.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"

/* The worker's fn type, declared here rather than included from orb_jobs.h:
 * the frozen ABI must not drag the worker's whole surface (and its static
 * stubs' collision with it) into every TU that includes an app header — the
 * same reason orb_app.h forward-declares LVGL objects instead. Identical
 * typedef: orb_svc.c's own include of orb_jobs.h is the compile-time pin
 * that keeps the two declarations from drifting. */
typedef void (*orb_job_fn)(void *arg);

#ifdef __cplusplus
extern "C" {
#endif

typedef struct orb_app orb_app_t;            /* orb_app.h defines the struct;
                                              * shared typedef, identical type */
struct _lv_area_t;

/* ---- capabilities (§4.2) ---------------------------------------------------
 * FREE caps are always on — no app is useful without the clock, and a setting
 * of its own under a quota is what makes an app an app. The owner-granted
 * four default OFF, and the dial's prompt (B3) is the only door they open
 * through. The enum's bit numbers are the NVS grant word's layout: appended
 * entries take the next number, never a re-order. */
typedef enum {
    CAP_TIME = 0,     /* now / tz_label / tz_offset (D15 + D17)        */
    CAP_CONFIG = 1,   /* own declared settings, within the quota       */
    CAP_UI = 2,       /* bank / layer_* / status                       */
    CAP_ART = 3,      /* its own bank (free for itself, §4.2)          */
    CAP_POWER = 4,    /* owner: dim-timeout request and the hold       */
    CAP_NET = 5,      /* owner: the OS http client, P4                 */
    CAP_SD_READ = 6,  /* owner: /sd/apps/<id>/ only, P4                */
    CAP_THEMES = 7,   /* owner: enumerate + request the D11 install    */
    CAP_N             /* 8                                              */
} orb_cap_t;

/* TIME|CONFIG|UI|ART: never stored, never revoked — §4.2's "free, always on"
 * rows enforced by not being representable, the strongest grant there is. */
#define CAP_FREE_MASK ((1u << CAP_POWER) - 1u)

/* The settings descriptor words, frozen from §4.1's settings table. The
 * manifest reader (P4) hands these to the registry; the config gate reads
 * them and refuses any key the app did not declare — "its own namespace, its
 * declared types and the per-app byte quota" (§4.2). */
typedef enum {
    ORB_SET_INT = 0, ORB_SET_ENUM, ORB_SET_TOGGLE, ORB_SET_STRING   /* C9, frozen */
} orb_set_type_t;

typedef struct {
    char     id[16];          /* NVS key inside the app's own namespace      */
    orb_set_type_t type;
    int32_t  min, max, def;
    char     def_str[32];     /* STRING type only                            */
} orb_setting_desc_t;

/* §9.4 item 4, decided with the real key shapes in hand: 2 KB per app against
 * the 24 KB storage partition, shared with orb.settings/orb.net/orb.idle and
 * orb.grants — worst case 8 x 2 KB plus the ~1 KB the OS namespaces use fits
 * the 6-sector partition with NVS's sector-granularity waste absorbed. A
 * STRING value pays its strlen+1, charged at set and refunded on overwrite.
 * #ifndef so the host suite can price the quota path with a working store;
 * the shipped number is this one, pinned by tools/host_svc_test.py. */
#ifndef ORB_CFG_QUOTA_BYTES
#define ORB_CFG_QUOTA_BYTES 2048
#endif

/* ---- the frozen table (§4.3, D15/D16/D17/D18 verbatim) --------------------
 * Deferred-owner records whose CONTENTS P4/P5 freeze: the broker returns them
 * only through these pointers, and the entries that consume them answer
 * ESP_ERR_NOT_SUPPORTED until their wiring commit (net -> P4's allowlisted
 * client, layer_* -> P5's table-driven compositor). The slots exist NOW
 * because the struct is the frozen ABI; a stub refuses loudly rather than
 * pretending, and the host suite pins that each stub sits BEHIND its gate,
 * not in place of it. */
typedef struct { uint8_t *data; size_t cap, len; } orb_buf_t;
typedef struct { const void *opaque; }              orb_layer_desc_t;

typedef struct {
  /* time — cap TIME (free, always on) */
  void      (*now)(orb_app_t *, time_t *sec, double *frac, bool *synced);
  const char *(*tz_label)(orb_app_t *);
  /* the OWNER's zone, DST-aware, at any instant (D17, §0.7) — minutes
   * east of UTC. The app converts; the zone stays owner config. */
  esp_err_t (*tz_offset)(orb_app_t *, time_t when, int32_t *offset_min);
  /* power: a request only, idle_dim owns the decision */
  esp_err_t (*power_request)(orb_app_t *, uint32_t timeout_s, bool hold);
  /* config: bounded to this app's declared settings, int32 and string (C9) */
  esp_err_t (*cfg_get)(orb_app_t *, const char *id, int32_t *out);
  esp_err_t (*cfg_set)(orb_app_t *, const char *id, int32_t v);
  esp_err_t (*cfg_get_str)(orb_app_t *, const char *id, char *buf, size_t cap);
  esp_err_t (*cfg_set_str)(orb_app_t *, const char *id, const char *v);
  /* net: OS-owned, allowlisted, rate-limited, off the LVGL task */
  esp_err_t (*http_get)(orb_app_t *, const char *url, orb_buf_t *out);
  /* art and compositor */
  const uint8_t *(*bank)(orb_app_t *);
  int  (*layer_add)(orb_app_t *, const orb_layer_desc_t *);
  void (*layer_set)(orb_app_t *, int h, const orb_layer_desc_t *);
  void (*layer_touch)(orb_app_t *, int h, const struct _lv_area_t *);
  void (*layer_del)(orb_app_t *, int h);
  /* worker: never block the LVGL task, same rule as the menu's orb_menu */
  esp_err_t (*job_post)(orb_app_t *, orb_job_fn, void *arg, size_t arglen);
  /* beat: the only callback stays the iface's tick(); this SETS its rate
   * (D16, §0.6). period_ms >= the manifest's (the app's fastest), or 0 =
   * stop ticking until the next resume. Faster-than-manifest is refused
   * with the number in the reason — the install is the cadence budget. */
  esp_err_t (*set_beat)(orb_app_t *, uint32_t period_ms);
  /* themes: enumerate and request (D18, §0.7). id-keyed through
   * orb_theme_id_ok(), never a path; install = D11 through the OS package
   * manager, ending in the reboot ask. cap THEMES. */
  int       (*theme_count)(orb_app_t *);
  esp_err_t (*theme_id)(orb_app_t *, int idx, char *buf, size_t cap);
  bool      (*theme_active)(orb_app_t *, int idx);
  bool      (*theme_changed)(orb_app_t *, int idx);
  const char *(*theme_note)(orb_app_t *, int idx); /* damage/trial reason, or NULL */
  esp_err_t (*theme_install)(orb_app_t *, const char *id);
  /* ui: the shell's list primitives for the app's settings page */
  void (*status)(orb_app_t *, const char *fmt, ...);
} orb_svc_t;

/** The gate, §4.2 made real. Counts the call either way — the P3 pass line is
 *  a number on /api/v1/status, and a denial that is not counted is a denial
 *  nobody can see — and records the ask when it refuses, which is what the
 *  grant prompt (B3) offers the owner on the dial. */
bool orb_grant_ok(const orb_app_t *a, orb_cap_t cap);

#define SVC(a, cap)  do { if (!orb_grant_ok((a), CAP_##cap)) \
                              return ESP_ERR_NOT_ALLOWED; } while (0)

/* ---- the OS side of the table ----------------------------------------------
 * Not for apps — the shell (grant prompt, status endpoint, scheduler) calls
 * these, and none of them appears in orb_svc_t, which is the whole point. */

/** Load the grant words from NVS. Call on the boot ladder after the storage
 *  partition is up and before the first app runs. */
esp_err_t orb_svc_init(void);

/** The table every app is handed at activation (P3's compiled tier takes it
 *  from here; P4's loader passes it to iface->activate, plan §1.4). */
const orb_svc_t *orb_svc_table(void);

/** The wall reading the now() entry reports, minus the gate: the OS's own
 *  callers (clock_view's compose_now, the boot ladder) take the same pair the
 *  apps do — two anchors is the bug the extraction was for. D15: raw second
 *  plus scheduler fraction plus trust flag; conversion stays app-side. */
void orb_time_now(time_t *sec, double *frac, bool *synced);

/* For the grant prompt (B3) and /api/v1/status (the P3 pass line). */
const char    *orb_svc_cap_name(orb_cap_t cap);   /* log/prompt label, "power" */
uint32_t     orb_svc_grants(const orb_app_t *a);   /* stored owner-granted mask */
uint32_t     orb_svc_pending(const orb_app_t *a);  /* asked while denied        */
void         orb_svc_prompt_shown(const orb_app_t *a, uint32_t caps); /* clear asks */
esp_err_t    orb_svc_set_grant(orb_app_t *a, orb_cap_t cap, bool on);  /* the owner's answer */
uint32_t     orb_svc_calls(const orb_app_t *a);
uint32_t     orb_svc_denials(const orb_app_t *a);
const char  *orb_svc_last_status(const orb_app_t *a); /* what SVC->status printed */
uint32_t     orb_svc_beat_ms(const orb_app_t *a);  /* the D16 rate in force; the scheduler reads this */
uint32_t     orb_svc_cfg_used(const orb_app_t *a); /* bytes charged against the quota */

/** The registry's revoke, called where it suspends an entry: an app that
 *  left the glass loses its power ask with it (§4.2 — auto-revoke is the
 *  shell's act, never the app's permission). */
void         orb_svc_app_suspended(const orb_app_t *a);

/* ---- C2: fault counting (plan §1.4) ------------------------------------------
 * The reset record's protocol lives in orb_svc.c; these are its doors.
 * Two consecutive faults mark an app failed with its reason until
 * orb_svc_clear_faults(); the HOME entry counts but never fails, because
 * "boot into the clock" needs the clock. */
int          orb_svc_reset_verdict(uint32_t magic, uint32_t booted); /* pure, host-pinned */
void         orb_svc_note_fault(const orb_app_t *a, const char *why);
uint32_t     orb_svc_fault_count(const orb_app_t *a);
bool         orb_svc_faulted(const orb_app_t *a);
const char  *orb_svc_fault_reason(const orb_app_t *a);
esp_err_t    orb_svc_clear_faults(orb_app_t *a);       /* P4's reinstall path */
/* Boot ladder: orb_svc_init arms, boot completion RUNs, activation stamps
 * the slot, and every OS-owned reboot disarms. */
void         orb_svc_boot_done(void);
void         orb_svc_current_note(const orb_app_t *a);
void         orb_svc_note_clean_reboot(void);

#ifdef __cplusplus
}
#endif