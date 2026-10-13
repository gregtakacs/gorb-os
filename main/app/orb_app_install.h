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
 * holds NO pixels and never will: ruling D21 (§0.9) voided the container's art
 * claim, so art_size is always 0 and art_slot always -1 — the bank machinery
 * that P4 wired art-light stays for the compiled tier's banked clock and is
 * dead code for every card row until a container's payload is code (P6/P7).
 * The re-sliced assets install targets this header once described are
 * withdrawn (D21/D26); nothing here erases or maps a bank.
 *
 * Identity, the cap and the quota are the installer's three gates, all applied
 * BEFORE the first erase so a refusal costs a sentence, not a slot. Grants are
 * NOT the installer's to write (D19): the installer fills orb_app_t.caps from
 * the manifest, never from a scan row and never a stored grant word, so an
 * absent or stale orb.grants can only ever under-grant. P4a's two owner doors
 * in this file (enable, uninstall) still never WRITE a grant — uninstall only
 * ERASES the removed row's owner words through orb_svc_forget_row(), and a
 * fresh install wipes a reused slot's stale ones for the same reason.
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

/* ---- P4a: the owner's doors — enable, uninstall ------------------------------
 * Both take the app's SLUG (not a card stem — an installed app has left the
 * card; its identity is the header's slug, which is FATFS-safe by the reader's
 * own rule, so it passes orb_sd_name_ok too). The /apps page is the only
 * caller today; the dial's APPS page (P5) will reach the same two entries.
 * Both refuse while a card job owns the queue, because an install landing in
 * the same slot mid-toggle is the only way these two can interleave.
 *
 * Enable: persist the e<slot> word FIRST (a crash between the two leaves the
 * intent on disk and the boot applies it), then flip the registry's RAM bit.
 * Turning an app ON revives a failed one — the owner's press IS the clear-
 * faults gesture, and it is that door's first caller. Turning one OFF while
 * it is the entry on the glass is refused: the walkable set may not lose the
 * row the knob is holding; carousel away first.
 *
 * Uninstall: erase the slot's HEADER sector first — from that byte the D9
 * rule makes it no app at all — then forget the row's owner words, then free
 * the registry entry. A power cut after the erase costs an app the owner had
 * already ordered gone and grants nothing: the next fresh install wipes the
 * leftover words anyway. The blob bytes stay behind, unreferenced, until the
 * slot's next copy_prefix erases them — the geometry of an app nobody can
 * name is nobody's quota. The card is never touched: the owner's /sd/apps
 * file stays exactly where it was, ready to reinstall from. */
esp_err_t orb_app_enable(const char *slug, bool on, char *why, size_t whysz);
esp_err_t orb_app_uninstall(const char *slug, char *why, size_t whysz);

/** Boot ladder, AFTER orb_svc_init() and BEFORE orb_app_init_all(): apply the
 *  stored enable words to the rows resolve_boot placed. Separate from resolve
 *  because the words live in NVS and resolve runs before the partition is up
 *  (its contract is partition-reads-only); an app with no stored word stays
 *  exactly as installed — off. */
void orb_app_install_apply_enable(void);

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

