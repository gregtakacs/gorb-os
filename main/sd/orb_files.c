/**
 * orb_files.c — the card's files, owned by the OS, declared by the app.
 *
 * The one place on the firmware that turns an app's declaration of "the files I
 * read live here, and look like this" into an actual listing, so that the dial's
 * options page and the browser's copy of it can both enumerate an app's files
 * without either knowing what the files mean.
 *
 * Two rules here are what keeps this out of the deadlock file:
 *
 *  - The lock is taken ONCE per operation, at the top of walk/delete, and the
 *    callback never sees it. The card mutex is non-recursive and one httpd task
 *    answers every route, so the historic failure was a scan that held the lock
 *    and then called something that asked for it again: the log said `waiting
 *    for the card: held by "httpd"` and every web route stopped answering while
 *    ping kept replying. Handing the callback an open fd is what removes the
 *    temptation to open anything of its own inside the walk.
 *  - path() touches no lock and no FatFs, so a caller that already holds the
 *    mutex can still build a filename. That is the difference between this and
 *    the bug above, and host_sd_test.py's lock lint reads it that way.
 *
 * An upload's "the card is busy" check deliberately does NOT live here: it is
 * orb_theme's state, and calling it from the SD layer would make sd depend on
 * art. orb_theme_scan() makes the check itself before it asks for a walk.
 *
 * The registration is a table rather than a hook call into the registry because
 * sd must not depend on app: an app hands its sources down at init and the OS
 * keeps them, and lookups by an unknown app or id answer NULL instead of
 * inventing a directory.
 */

#include "orb_files.h"
#include "orb_sd.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_err.h"
#include "esp_log.h"

static const char *TAG = "files";

/* Apps register their sources here, at init, before anything can ask for a
 * walk. Small and fixed: it is bounded by ORB_APP_CAP x a couple of sources per
 * app, so a linear scan is cheaper than a hash on a device whose PSRAM is being
 * read as a framebuffer. */
#define ORB_FILES_SRC_MAX 12

typedef struct {
    char             app[ORB_FILES_ID_MAX + 1];
    orb_file_src_t   src;
    bool             used;
} src_rec_t;

static src_rec_t s_srcs[ORB_FILES_SRC_MAX];

/* The directory rule, used by register() and by path(). A prefix test is not
 * enough: "/sd/../etc" starts with "/sd/" and resolves outside the volume, and
 * FATFS has no objection to following it. So the segments are checked one at a
 * time — no "..", no empty segment, no leading-slash escape, and nothing that
 * would let a declared directory turn into somebody else's files. */
static bool dir_under_sd(const char *dir)
{
    const char *base = ORB_SD_BASE "/";
    const size_t blen = strlen(base);
    if (strncmp(dir, base, blen) != 0) return false;

    /* "/sd" alone is the whole volume: a source that covers everything is not a
     * declared subset of it, and the walk would list the owner's other files. */
    if (dir[blen] == '\0') return false;

    char seg[ORB_FILES_DIR_MAX];
    const char *p = dir + blen;
    while (*p) {
        const char *slash = strchr(p, '/');
        const size_t n = slash ? (size_t)(slash - p) : strlen(p);
        if (n == 0 || n >= sizeof(seg)) return false;          /* empty or absurd */
        memcpy(seg, p, n);
        seg[n] = '\0';
        if (strcmp(seg, ".") == 0 || strcmp(seg, "..") == 0) return false;
        for (size_t i = 0; i < n; i++) {
            const char c = seg[i];
            const bool okc = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                             (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
            if (!okc) return false;                            /* no '\\', no space */
        }
        p += n;
        if (*p == '/') { p++; }
        else break;                       /* the last segment, or nothing left */
    }
    return true;
}

esp_err_t orb_files_register(const char *app_slug, const orb_file_src_t *src)
{
    if (!app_slug || !app_slug[0] || !src || !src->id[0] || !src->dir[0] || !src->ext[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    /* The directory must be a real subdirectory of /sd. Names arrive from a
     * browser query parameter, and this table is the only thing that decides
     * which directories are walkable at all — a prefix test alone would let
     * "/sd/../etc" through, which FATFS is happy to follow. */
    if (!dir_under_sd(src->dir)) {
        ESP_LOGE(TAG, "\"%s\": dir \"%s\" is not a directory under " ORB_SD_BASE,
                 app_slug, src->dir);
        return ESP_ERR_INVALID_ARG;
    }
    /* The dot is part of the declaration, not of the caller's habit: the walk
     * compares the tail of a name against this string, and an ext of "orbtheme"
     * would silently match a file called "xorbtheme". */
    if (src->ext[0] != '.') {
        ESP_LOGE(TAG, "\"%s\": ext \"%s\" must carry the dot", app_slug, src->ext);
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < ORB_FILES_SRC_MAX; i++) {
        src_rec_t *r = &s_srcs[i];
        if (r->used && strcmp(r->app, app_slug) == 0 && strcmp(r->src.id, src->id) == 0) {
            /* Re-declaring the same source is what an app's init being called
             * twice looks like, not a second app's claim on the id. */
            r->src = *src;
            return ESP_OK;
        }
        if (!r->used) {
            r->used = true;
            r->src  = *src;
            snprintf(r->app, sizeof r->app, "%s", app_slug);
            ESP_LOGI(TAG, "%s: \"%s\" -> %s/*%s (max %u)", app_slug, src->id,
                     src->dir, src->ext, (unsigned)src->max_entries);
            return ESP_OK;
        }
    }
    ESP_LOGE(TAG, "source table full (%u) — \"%s\" refused",
             (unsigned)ORB_FILES_SRC_MAX, src->id);
    return ESP_ERR_NO_MEM;
}

const orb_file_src_t *orb_files_src(const char *app_slug, const char *id)
{
    if (!app_slug || !app_slug[0] || !id || !id[0]) return NULL;
    for (size_t i = 0; i < ORB_FILES_SRC_MAX; i++) {
        const src_rec_t *r = &s_srcs[i];
        if (r->used && strcmp(r->app, app_slug) == 0 && strcmp(r->src.id, id) == 0) {
            return &r->src;
        }
    }
    return NULL;
}

/* PURE. No lock, no mount, no FatFs — see the header's threading rule, which is
 * what lets a caller that already holds the mutex still build a filename. */
bool orb_files_path(const orb_file_src_t *src, const char *name,
                    char *out, size_t outsz)
{
    if (out == NULL || outsz == 0) return false;
    out[0] = '\0';
    /* The dir is checked here, not only at registration, because path() is also
     * called by callers holding the card lock with a source they read out of the
     * table long ago. A source whose dir is empty would format "/name.ext" — a
     * path at the volume root, outside every declared directory. */
    if (src == NULL || !src->dir[0] || src->dir[0] != '/' || !src->ext[0]) return false;
    if (!dir_under_sd(src->dir)) return false;
    /* The one name validator, reached from here rather than from each caller:
     * the names that arrive on this line are only ever a sha256-derived theme id,
     * a screenshot the device wrote itself, or an entry FATFS listed from a card
     * that anything could have been copied onto. All three are untrusted. */
    if (!orb_sd_name_ok(name)) return false;

    const int n = snprintf(out, outsz, "%s/%s%s", src->dir, name, src->ext);
    /* Compare against the formatted length, not the room left after the
     * directory: a name that only just fits must be refused, not truncated,
     * because a truncated name is a different file on the card. */
    if (n < 0 || (size_t)n >= outsz) { out[0] = '\0'; return false; }
    return true;
}

int orb_files_walk(const orb_file_src_t *src, orb_file_each_fn cb, void *arg)
{
    if (src == NULL || cb == NULL) return -1;
    if (orb_sd_ensure_mounted() != ESP_OK) return -1;

    orb_sd_lock();

    const size_t elen = strlen(src->ext);
    int kept = 0;
    int seen = 0;

    DIR *d = opendir(src->dir);
    if (!d) {
        orb_sd_unlock();
        /* An absent directory is not the same answer as an empty one: the first
         * means no card or a volume formatted elsewhere, the second means the
         * owner has not put anything on it yet. */
        ESP_LOGW(TAG, "%s is not a directory (%s)", src->dir, strerror(errno));
        return -1;
    }

    struct dirent *de;
    while (kept < (int)src->max_entries && (de = readdir(d)) != NULL) {
        const size_t len = strlen(de->d_name);
        if (len <= elen || strcmp(de->d_name + (len - elen), src->ext) != 0) continue;

        orb_file_rec_t rec;
        rec.size = 0;
        rec.name[0] = '\0';
        /* The stem, not the filename: the name is what a later request comes
         * back with, so it has to be the string the validator accepts. A name
         * too long or too strange for that is skipped — junk, not a file. */
        if (len - elen > sizeof(rec.name) - 1) continue;
        memcpy(rec.name, de->d_name, len - elen);
        rec.name[len - elen] = '\0';
        if (!orb_sd_name_ok(rec.name)) continue;

        char path[ORB_SD_PATH_MAX];
        if (!orb_files_path(src, rec.name, path, sizeof path)) continue;

        const int fd = open(path, O_RDONLY);
        if (fd < 0) {
            /* Vanished between readdir and open: a card edited on a laptop while
             * the device was listing it. Skipped, not reported — there is no
             * file left to describe. */
            ESP_LOGW(TAG, "%s: cannot open \"%s\" (%s)", src->dir, rec.name,
                     strerror(errno));
            continue;
        }

        struct stat st;
        if (fstat(fd, &st) == 0 && st.st_size > 0) rec.size = (uint32_t)st.st_size;

        seen++;
        /* The callback never closes the fd: an entry it refuses is the one path
         * on which an fd would otherwise leak. */
        if (cb(arg, &rec, fd)) kept++;
        close(fd);
    }
    closedir(d);
    orb_sd_unlock();

    if (seen) ESP_LOGI(TAG, "%s: kept %d of %d *%s", src->dir, kept, seen, src->ext);
    return kept;
}

esp_err_t orb_files_delete(const orb_file_src_t *src, const char *name)
{
    if (src == NULL) return ESP_ERR_INVALID_ARG;
    if (!orb_sd_name_ok(name)) return ESP_ERR_INVALID_ARG;
    if (orb_sd_ensure_mounted() != ESP_OK) return ESP_ERR_NOT_FOUND;

    orb_sd_lock();
    char path[ORB_SD_PATH_MAX];
    const bool ok = orb_files_path(src, name, path, sizeof path);
    const int rr = ok ? unlink(path) : -1;
    const int re = errno;
    orb_sd_unlock();

    if (!ok) return ESP_ERR_INVALID_ARG;
    if (rr != 0) {
        ESP_LOGW(TAG, "unlink %s: %s", path, strerror(re));
        /* ENOENT is its own answer: "deleted" when nothing was deleted is the
         * sentence that makes an owner stop believing the rest of the UI. */
        return (re == ENOENT) ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    ESP_LOGI(TAG, "removed %s", path);
    return ESP_OK;
}

