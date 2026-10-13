/*
 * main/app/orb_app_scan.h — the /sd/apps drop-zone scan (P4).
 *
 * Lists the app containers the owner has copied onto the card, each one
 * already run through the reader's full validation — orbapp_parse_header,
 * orbapp_check_header (with the filename's stem in the identity triple) and
 * orbapp_check_file_layout — plus the card's own facts: the bytes FATFS says
 * the file holds must reach the art the header claims. A row that fails any
 * of it is RETURNED with its reason, never hidden: "no apps on the card" and
 * "four apps, all refused" are different problems, and the page has to be
 * able to tell the owner whose problem it is (orb_theme_scan()'s rule
 * verbatim — orb_theme.c:1748).
 *
 * The walk itself is orb_files_walk()'s, under the one card lock, with the
 * fd already open — this layer never calls opendir(), open(), or orb_sd_lock
 * again, which is the deadlock orb_theme.c:901 documented and the lock lint
 * in tools/host_sd_test.py holds the door to. Only the header block is read
 * here: the payload checksums belong to the install (verify-then-header, D9),
 * the same division the theme path runs.
 *
 * The file source is OS-owned, registered as ("orb", "apps"): the directory
 * is the platform's drop zone, not any app's data, and an app declaring
 * where card apps live would let one app relocate everyone's front door.
 * row_decide() — the pure decision over one listed file — is lifted verbatim
 * by tools/host_orbapp_scan_test.py, the way host_sd_test.py lifts
 * theme_state(); the lift dies loudly if the function moves.
 */
#ifndef ORB_APP_SCAN_H
#define ORB_APP_SCAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "orb_app_manifest.h"   /* the caps/title caps are the row's shape */
#include "sd/orb_sd.h"          /* ORB_SD_BASE: one definition of the volume */

#ifdef __cplusplus
extern "C" {
#endif

#define ORB_APP_SCAN_SLUG   "orb"     /* the OS, as the file-source owner */
#define ORB_APP_SCAN_ID     "apps"
#define ORB_APP_DIR         ORB_SD_BASE "/apps"
#define ORB_APP_EXT         ".orbapp"
#define ORB_APP_SCAN_MAX    16        /* listed candidates; ORB_APP_CAP is the INSTALL cap */

/* One row of the listing. err == ESP_OK means install-ready as far as a
 * header read can say — the install re-verifies the payload and owns the
 * slot arithmetic. The card filename stem is kept even for a refused row:
 * it is the handle the page acts on, and a row that cannot be described
 * must still be addressable (orb_theme_entry_t's reason-is-not-the-name
 * rule, orb_theme.c:1749). */
typedef struct {
    char      stem[32];               /* filename, extension stripped, as listed */
    char      slug[ORBAPP_SLUG_MAX];
    char      title[ORBAPP_TITLE_MAX];
    uint32_t  app_id;
    uint32_t  caps;                   /* declared owner caps; disclosure source */
    uint32_t  period_ms;
    uint32_t  serial;
    uint32_t  n_settings;
    uint32_t  art_size;               /* what the install must reserve */
    uint32_t  file_size;              /* FATFS's number, for the page */
    esp_err_t err;
    char      why[256];               /* refusal sentence; "" when err == ESP_OK */
} orbapp_scan_entry_t;

/** Register the ("orb","apps") file source. Call on the boot ladder after
 *  orb_files' own init and before any scan can be asked for, exactly as
 *  clock_view.c registers the theme source. Idempotent by orb_files' rule. */
esp_err_t orb_app_scan_init(void);

/** Walk ORB_APP_DIR, up to cap rows; -1 when the card is not mounted or the
 *  directory is absent — the answer that must stay distinguishable from
 *  zero rows. Kept rows include refused ones, each with why filled and err
 *  set; the reader's refusal text arrives verbatim and unedited. */
int orb_app_scan(orbapp_scan_entry_t *out, int cap);

#ifdef __cplusplus
}
#endif
#endif /* ORB_APP_SCAN_H */
