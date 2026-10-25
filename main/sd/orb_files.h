/**
 * orb_files.h — the card's files, owned by the OS, declared by the app.
 *
 * An app does not open its own files. It declares a SOURCE — an id, the
 * directory it reads, the extension its files carry, and how many it can use —
 * and the OS does the mounting, the locking, the path, the walk and the
 * deletion. The bytes that come back are the app's to interpret; getting them
 * is the OS's. This is the split the plan's B1/B2 rendering side needs (see
 * docs/app-platform-plan.md §4.1 and §7): the shell's options page and the
 * browser's copy of it both have to list an app's files without either one
 * knowing what a theme is, and the only way to do that is for the OS to be the
 * one that knows where the files live and to be told, per app, what it is
 * looking for.
 *
 * Why the descriptor and not just a path: the directory and the extension used
 * to be constants in the theme and SD layers (ORB_SD_THEME_DIR, ORB_THEME_EXT),
 * so every consumer that could name a file could also name any OTHER file on
 * the volume. A declared source bounds the walk to one directory and one
 * extension, and the browser only ever sends the name inside it — which is the
 * same rule orb_sd_name_ok() enforces, now applied by the one function that
 * builds paths rather than by each caller remembering to.
 *
 * Threading — the rule that keeps this from being the next deadlock:
 * the card mutex is non-recursive and esp_http_server answers every route from
 * ONE task, so a lock holder that reaches a second take leaves the web dead
 * while the dial keeps drawing and ping keeps replying. So:
 *   - orb_files_walk() and orb_files_delete() take the lock themselves and hold
 *     it across the whole operation. Neither may be called while holding it.
 *   - orb_files_path() is PURE: no mount, no lock, no FatFs. A caller that
 *     already holds the lock may build a path. tools/host_sd_test.py's lock lint
 *     treats the two locked entry points as lockers on that basis, so adding a
 *     take to path() is caught there and not on a wall.
 *   - The walk hands the callback an already-open fd and closes it afterwards,
 *     so an app never calls open() and never has to think about the lock at all.
 *
 * Nothing here is fatal. A board with no card gets the same empty answer it
 * would have got before the socket existed, which is orb_sd.h's rule.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define ORB_FILES_ID_MAX   15    /* the source's name, e.g. "themes" */
#define ORB_FILES_DIR_MAX  64    /* a full directory path on /sd */
/* ORB_FILES_MAX_NAME equals ORB_SD_NAME_MAX, not +1 as it was when the two
 * read 32 and 31: the drift let the walk's buffer and the validator disagree
 * about which names exist. orb_app_scan.h pins the direction the walk needs
 * (ORB_SD_NAME_MAX <= ORB_FILES_MAX_NAME) with a static assert. The value line
 * stays single-line: host_files_test.py lifts it with a one-line regex. */
#define ORB_FILES_MAX_NAME 55    /* one FATFS name, sans directory */

/** One declared source of files. `id` is what the app and the OS both call it by
 *  ("themes"); it reaches the browser in a query parameter, so it is matched
 *  exactly against this table and is never used as a path component. */
typedef struct {
    char        id[ORB_FILES_ID_MAX + 1];
    char        dir[ORB_FILES_DIR_MAX];   /* absolute: "/sd/themes" */
    char        ext[16];                  /* with the dot: ".orbtheme" */
    uint8_t     max_entries;              /* most entries one walk returns */
} orb_file_src_t;

/** One file the OS found. `name` is the stem the browser sends back to act on
 *  it (extension stripped, exactly as it was validated on the way in); `size` is
 *  what the card says the file is, which is the only number the app may plan
 *  around before it has read the thing. */
typedef struct {
    char        name[ORB_FILES_MAX_NAME + 1];
    uint32_t    size;
} orb_file_rec_t;

/** Called once per candidate file, on the caller's task, with the card lock held
 *  and `fd` open for reading. Return true to keep the entry in the list.
 *
 *  The callback runs under the lock on purpose: an entry the app has not finished
 *  reading cannot be swapped out from under it by a concurrent scan, and reading
 *  outside the lock is what let a half-written upload be listed as a theme. It may
 *  read `fd` and nothing else — no open(), no opendir(), no second take of the
 *  mutex, and no blocking work that is not this file. */
typedef bool (*orb_file_each_fn)(void *arg, const orb_file_rec_t *rec, int fd);

/** Register one source for one app. Called from the app's init on the boot
 *  ladder, before any walk can be asked for. Refused, with the reason in the
 *  log, when the directory is not under /sd or the extension does not carry its
 *  dot — a declaration that would make the OS walk somewhere it has no business
 *  walking is a build bug, not a runtime condition to tolerate. */
esp_err_t orb_files_register(const char *app_slug, const orb_file_src_t *src);

/** The sources one app declares, by id. NULL when no such source exists, which
 *  every caller must report rather than guess at: a wrong directory is not an
 *  empty listing, it is somebody else's files. */
const orb_file_src_t *orb_files_src(const char *app_slug, const char *id);

/** "/sd/<dir>/<name><ext>" from a declared source, or false.
 *  PURE — see the threading rule above. `name` is checked by orb_sd_name_ok()
 *  here rather than by each caller, and the extension is appended HERE rather
 *  than by the client: a browser that could spell the suffix would choose both
 *  the extension and, through it, what the next walk considers a file. */
bool orb_files_path(const orb_file_src_t *src, const char *name,
                    char *out, size_t outsz);

/** Walk the source's directory, calling `cb` for each file carrying its
 *  extension, up to src->max_entries. Returns the number kept, or -1 when the
 *  card is not mounted or the directory is not there — the two states the page
 *  must be able to tell apart from "zero files".
 *
 *  Entries the callback refuses are counted out, not hidden silently: the caller
 *  owns what "damaged" means and is expected to keep the row with its reason,
 *  which is orb_theme_scan()'s shown-with-reason rule verbatim. */
int orb_files_walk(const orb_file_src_t *src, orb_file_each_fn cb, void *arg);

/** Remove one file by name. Refuses a name the validator refuses, and reports
 *  ENOENT as ESP_ERR_NOT_FOUND rather than as success — deleting nothing and
 *  saying so is a different answer from deleting the wrong thing. */
esp_err_t orb_files_delete(const orb_file_src_t *src, const char *name);
