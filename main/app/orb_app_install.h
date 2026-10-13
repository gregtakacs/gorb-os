/*
 * main/app/orb_app_install.h — the app installer (plan P4, §2/§3.2/D9).
 *
 * The one implementation of "put this .orbapp on the dial", reached through an
 * orb_jobs posting from the /apps page's install button, so the page and any
 * later dial picker cannot disagree about whether the multi-second card-to-flash
 * copy runs on a task allowed to take the card lock. It mirrors
 * orb_theme_install() (orb_theme.c:1456): resolve, re-verify identity inside the
 * card lock, checksum the WHOLE payload OFF the card before the first flash
 * write, copy the geometry into the app's slot, read it back and CRC it THERE,
 * and only then write the slot header — the single thing that makes a slot live
 * (D9). A copy torn by a power cut or a pulled card has no header, so its slot
 * reads as an ABSENT app, never as corrupt art.
 *
 * The apps partition (partitions.csv:57, offset 0xD80000 size 0x280000) holds,
 * per app, ONE slot of a header sector (written last) plus a geometry blob. It
 * holds NO pixels: art installs into the re-sliced `assets` slots (§3.2/D10) and
 * maps at activation through orb_app_t's art_offset/art_bytes/art_slot, the same
 * second-bank machinery color_picker's art-light case already runs with zero
 * bytes. FORMAT 1's P4 install set is art-light or fits the small slot; a bank
 * large enough to alias the live theme slot (art0/art1) is refused by size with
 * a named reason — the plan defers the reconciliation to P6, so no theme bank is
 * ever an install target here.
 *
 * Identity, the cap and the quota are the installer's three gates, all applied
 * BEFORE the first erase so a refusal costs a sentence, not a slot. Grants are
 * NOT the installer's to write (D19): the installer fills orb_app_t.caps from
 * the manifest, never from a scan row and never a stored grant word, so an
 * absent or stale orb.grants can only ever under-grant.
 *
 * install_decide() — the pure decision — is lifted verbatim by
 * tools/host_orbapp_install_test.py the way host_orbapp_scan_test.py lifts
 * row_decide(); the ordering of the device-only side effects is pinned by that
 * suite's structural lint, the way host_sd_test.py's arena lint pins
 * orb_theme.c's free/publish order.
 */
#ifndef ORB_APP_INSTALL_H
#define ORB_APP_INSTALL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "orb_app.h"              /* orb_app_t, ORB_APP_CAP — the resident slot */
#include "orb_app_manifest.h"     /* orbapp_hdr_t, the caps/settings caps */

#ifdef __cplusplus
extern "C" {
#endif

/* The apps partition's internal layout — how the owner's approved row is used,
 * not a table change. Sector-granular (D6): every region starts and is sized on
 * a 4 KB boundary so an erase never suspends the cache across a live read, and
 * the geometry read back CRCs whole sectors as the writer emitted them. */
#define APPS_PARTITION_NAME  "apps"                       /* partitions.csv label */
#define APPS_SECTOR          0x1000u                      /* NOR erase granularity */
#define APPS_SLOT_HDR_BYTES  0x1000u                      /* one header sector, written last */
#define APPS_SLOT_GEOM_CAP   ORBAPP_GEOM_BYTES_MAX        /* 256 KB — the writer's ceiling */
#define APPS_SLOT_STRIDE     (APPS_SLOT_HDR_BYTES + APPS_SLOT_GEOM_CAP)  /* 0x41000 */
#define APPS_N_SLOTS         ORB_APP_CAP                  /* B5: 8 slots, one per app */

/* A bank into the large assets slots would alias the live theme bank until P6
 * reconciles the install paths; the P4 install set fits the small slot only. */
#define APPS_ART_SMALL_MAX   0x00050000u   /* 0.375 MB — assets art2, never a theme target */

/* install_decide()'s inputs: the slot state the caller read under the lock, plus
 * the unpacked declared-settings table (the header carries n_settings but not the
 * per-row TYPES, and the quota prices a STRING at its 32-byte descriptor, an
 * int/enum/toggle at the 4 bytes NVS stores — so the gate needs the real rows).
 * Value-only so the host suite builds one on the stack. */
typedef struct {
    bool     slot_used[APPS_N_SLOTS];    /* each slot's header read valid */
    uint32_t slot_serial[APPS_N_SLOTS];  /* its header's serial, 0 when unused */
    int      existing_slot;              /* slot already holding this slug, or -1 */
    int      installed_count;            /* distinct installed apps (B5 tally) */
    const orb_setting_desc_t *settings;  /* unpacked declared settings, or NULL */
    int      n_settings;                 /* rows in it, <= ORBAPP_SETTINGS_MAX */
} orbapp_install_slots_t;


/* install_decide()'s verdict. */
typedef struct {
    bool        accept;          /* false → refuse; .why names it */
    int         slot;            /* slot to write, 0..APPS_N_SLOTS-1; -1 refused */
    bool        reinstall;       /* slug already installed → reuse its slot */
    uint32_t    serial;          /* prior+1 on a first install, kept on reuse */
    uint32_t    cfg_worst;       /* declared-settings worst-case NVS bytes */
    char        why[128];        /* refusal sentence, "" when accept */
} orbapp_install_verdict_t;

/* The pure decision over a header the reader already accepted. Never touches a
 * partition, an fd, or NVS. */
bool install_decide(const orbapp_hdr_t *h, const orbapp_install_slots_t *sl,
                    orbapp_install_verdict_t *v);

/* Install /sd/apps/<stem>.orbapp. Worker task only. On success the app is
 * resident: slot header live, caps/settings in the registry. On failure the slot
 * is headerless (reads absent) and `why` carries the row's sentence. */
esp_err_t orb_app_install(const char *stem, char *why, size_t whysz);

/** Rebuild the registry's card table from the apps-partition slots, on the boot
 *  ladder. Call once, after orb_app_scan_init() and BEFORE orb_svc_init(), so the
 *  broker preloads a grant word for every installed card app and masks it against
 *  the caps the slot's header declares (D19: the header is the only grant source,
 *  and an absent slot is an absent app, never a corrupt one). Reads only the
 *  partition — no card, no lock. A slot whose header fails to parse, or whose
 *  settings table will not unpack, is SKIPPED (reads absent) rather than installed
 *  half-valid. Returns the number of apps resolved. */
int orb_app_install_resolve_boot(void);

/* Queue an install onto the worker. ESP_ERR_INVALID_STATE when one is
 * outstanding; ESP_ERR_NO_MEM when the queue refused. */
esp_err_t orb_app_install_post(const char *stem);

/* The last install attempted and the sentence it failed with, for the /apps
 * refresh to show the same words the dial would. */
const char *orb_app_install_try_stem(void);
const char *orb_app_install_try_why(void);

/* ---- uploading an .orbapp FROM the browser TO the card (P4) ------------------
 *
 * The owner's card socket is unreachable from a laptop that is not the one the
 * file was baked on, and the install button can only act on a file already in
 * /sd/apps. This is the missing first step: the /apps page streams the container
 * bytes in, and they land in <apps dir>/upload.part. The scratch deliberately does
 * NOT carry ORB_APP_EXT, so orb_files_walk never lists it and the install can never
 * select a half-arrived file (orb_theme_upload's rule, orb_theme.h:326).
 *
 * Chunked, not buffered: a FORMAT 1 app is header+settings+geometry+art and the
 * httpd worker has no business holding that in RAM. Nothing is verified — and
 * nothing can be, because the container's own bytes decide its name — until the
 * last byte has arrived. Then, exactly as the theme upload: re-open the .part and
 * run the reader's FULL ladder (parse, identity with the derived <slug>.orbapp
 * stem, file layout) and the payload CRCs OFF the card, and only a file that
 * survives all of it is rename()d into <slug>.orbapp. A half-uploaded or damaged
 * container is deleted, never left to be listed as an app that cannot load.
 *
 * The destination name is never a client argument: it is the header's OWN slug
 * (already validated by orbapp_check_header to be FATFS-safe and equal the stem),
 * so no ?name= reaches a path. ?overwrite=1 is the only client decision, and it
 * only governs whether a verified file replaces a DIFFERENT app already filed
 * under the same slug. Duplicate policy matches the theme path byte-for-byte:
 * same slug + same geom_crc/art_crc/serial is accepted idempotently; a different
 * set of bytes under an existing slug is refused with ORB_APP_UPLOAD_E_DUP unless
 * overwrite.
 *
 * One upload at a time, holding the one card lock for its whole duration — so
 * orb_app_scan() reports busy instead of blocking the httpd task behind a
 * multi-second card write (the reason orb_theme_scan has the same gate). */
typedef struct orb_app_up orb_app_up_t;

/* The writer's real ceilings, not an invented cap: art + geometry + header block
 * and the alignment gap. A bigger body is refused on Content-Length, before a byte
 * of it reaches the card. */
#define ORB_APP_UPLOAD_MAX  (ORBAPP_ART_BYTES_MAX + ORBAPP_GEOM_BYTES_MAX + 0x20000u)

esp_err_t orb_app_upload_begin(orb_app_up_t **out);
esp_err_t orb_app_upload_data(orb_app_up_t *u, const void *buf, size_t n);
/* A distinct code so the page can offer overwrite on a name collision and re-send
 * with ?overwrite=1, and show a plain re-package message for every other refusal.
 * Chosen above the ESP common-error range so it cannot collide with an esp_err_t
 * the transport already assigns a meaning to (orb_theme.h:353's rule). */
#define ORB_APP_UPLOAD_E_DUP  ((esp_err_t)0x0101F3)
esp_err_t orb_app_upload_end(orb_app_up_t **u, bool overwrite, const char **why);
void      orb_app_upload_cancel(orb_app_up_t **u);

/* True while an upload owns the card, so orb_app_scan() and the status endpoint can
 * answer instead of blocking the httpd worker behind the write. */
bool orb_app_upload_busy(void);

#ifdef __cplusplus
}
#endif
#endif /* ORB_APP_INSTALL_H */

