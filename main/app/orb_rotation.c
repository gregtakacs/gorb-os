/**
 * orb_rotation.c — the carousel's order and its rotation (see header; plan §59).
 */
#include "orb_rotation.h"

#include <stdio.h>
#include <string.h>

#ifndef ORB_ROTATION_HOST
#define ORB_ROTATION_NVS 1
#include "nvs.h"
#include "esp_log.h"
static const char *TAG = "rotation";
#define NS "orb.carousel"
#endif

static char           s_order[ORB_ROTATION_MAX][ORB_ROTATION_SLUG];
static int            s_order_n;
static orb_rotation_t s_rot = { .on = false, .every_s = ORB_ROTATION_DEF_S, .n = 0 };

int orb_rotation_csv(const char *csv, char out[][ORB_ROTATION_SLUG], int max)
{
    int n = 0;
    const char *p = csv ? csv : "";
    while (*p && n < max) {
        while (*p == ',' || *p == ' ') p++;
        const char *e = p;
        while (*e && *e != ',') e++;
        size_t len = (size_t)(e - p);
        while (len && p[len - 1] == ' ') len--;
        if (len && len < ORB_ROTATION_SLUG) {
            char slug[ORB_ROTATION_SLUG];
            memcpy(slug, p, len);
            slug[len] = '\0';
            bool dup = false;
            for (int k = 0; k < n; k++) if (!strcmp(out[k], slug)) dup = true;
            if (!dup) { memcpy(out[n], slug, len + 1); n++; }
        }
        p = e;
    }
    return n;
}

#if ORB_ROTATION_NVS
static void join(char *out, size_t cap, char list[][ORB_ROTATION_SLUG], int n)
{
    size_t at = 0;
    out[0] = '\0';
    for (int i = 0; i < n; i++) {
        const int w = snprintf(out + at, cap - at, "%s%s", i ? "," : "", list[i]);
        if (w < 0 || (size_t)w >= cap - at) break;
        at += (size_t)w;
    }
}

static void save(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) { ESP_LOGW(TAG, "nvs would not open"); return; }
    char buf[ORB_ROTATION_MAX * ORB_ROTATION_SLUG];
    join(buf, sizeof buf, s_order, s_order_n);
    nvs_set_str(h, "order", buf);
    join(buf, sizeof buf, s_rot.apps, s_rot.n);
    nvs_set_str(h, "apps", buf);
    nvs_set_u8(h, "on", s_rot.on ? 1 : 0);
    nvs_set_u32(h, "every", s_rot.every_s);
    nvs_commit(h);
    nvs_close(h);
}
#else
static void save(void) { }
#endif

void orb_rotation_init(void)
{
#if ORB_ROTATION_NVS
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;   /* first boot: the defaults */
    char buf[ORB_ROTATION_MAX * ORB_ROTATION_SLUG];
    size_t sz = sizeof buf;
    if (nvs_get_str(h, "order", buf, &sz) == ESP_OK)
        s_order_n = orb_rotation_csv(buf, s_order, ORB_ROTATION_MAX);
    sz = sizeof buf;
    if (nvs_get_str(h, "apps", buf, &sz) == ESP_OK)
        s_rot.n = orb_rotation_csv(buf, s_rot.apps, ORB_ROTATION_MAX);
    uint8_t on = 0;
    if (nvs_get_u8(h, "on", &on) == ESP_OK) s_rot.on = on != 0;
    uint32_t ev = 0;
    if (nvs_get_u32(h, "every", &ev) == ESP_OK && ev >= ORB_ROTATION_MIN_S && ev <= ORB_ROTATION_MAX_S)
        s_rot.every_s = ev;
    nvs_close(h);
    ESP_LOGI(TAG, "order %d app(s); rotation %s every %lu s over %d app(s)",
             s_order_n, s_rot.on ? "on" : "off", (unsigned long)s_rot.every_s, s_rot.n);
#endif
}

int orb_rotation_order(char out[][ORB_ROTATION_SLUG], int max)
{
    const int n = s_order_n < max ? s_order_n : max;
    for (int i = 0; i < n; i++) memcpy(out[i], s_order[i], ORB_ROTATION_SLUG);
    return n;
}

const char *orb_rotation_set_order(const char *csv)
{
    if (!csv) return "no order was given";
    s_order_n = orb_rotation_csv(csv, s_order, ORB_ROTATION_MAX);
    save();
    return NULL;
}

int orb_rotation_rank(const char *slug, int fallback)
{
    for (int i = 0; slug && i < s_order_n; i++)
        if (!strcmp(s_order[i], slug)) return i;
    return ORB_ROTATION_MAX + fallback;
}

void orb_rotation_get(orb_rotation_t *out) { *out = s_rot; }

const char *orb_rotation_set(bool on, uint32_t every_s, const char *apps_csv)
{
    if (every_s < ORB_ROTATION_MIN_S || every_s > ORB_ROTATION_MAX_S)
        return "the interval runs 10 s to a day";
    s_rot.on = on;
    s_rot.every_s = every_s;
    if (apps_csv) s_rot.n = orb_rotation_csv(apps_csv, s_rot.apps, ORB_ROTATION_MAX);
    save();
    return NULL;
}

bool orb_rotation_takes_part(const char *slug)
{
    for (int i = 0; slug && i < s_rot.n; i++)
        if (!strcmp(s_rot.apps[i], slug)) return true;
    return false;
}

static bool in_set(const orb_rotation_t *r, const char *slug)
{
    for (int i = 0; slug && i < r->n; i++)
        if (!strcmp(r->apps[i], slug)) return true;
    return false;
}

int orb_rotation_pick(const orb_rotation_t *r, const char *const *walk, int n, int cur,
                      int64_t quiet_us)
{
    if (!r || !r->on || !walk || cur < 0 || cur >= n) return -1;
    if (!in_set(r, walk[cur])) return -1;                 /* an app the owner chose to stay on */
    if (quiet_us < (int64_t)r->every_s * 1000000) return -1;
    for (int k = 1; k < n; k++) {                         /* the next one along, wrapping */
        const int i = (cur + k) % n;
        if (in_set(r, walk[i])) return i;
    }
    return -1;                                            /* it is the only one taking part */
}
