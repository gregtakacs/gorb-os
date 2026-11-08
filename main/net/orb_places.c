/* The orb's three places beside home — contract in orb_places.h. */
#include "orb_places.h"

#include <stdio.h>
#include <string.h>

/* ---- pure: lifted VERBATIM by tools/host_places_test.py down to the marker below ---- */

/* Printable text without the two characters that would end a JSON string: the name is
 * echoed by /api/v1/places with %s, as orb_home.c reasons for home's. UTF-8 whole: Győr
 * and Tatabánya are names, and the fonts draw them (radar-plan §6.22, OS v1.2.37). */
static bool places_safe_text(const char *s)
{
    const unsigned char *u = (const unsigned char *)s;
    while (*u) {
        if (*u < 0x80) {
            if (*u < 0x20 || *u == 0x7f || *u == '"' || *u == '\\') return false;
            u++;
            continue;
        }
        const int n = (*u & 0xE0) == 0xC0 ? 1 : (*u & 0xF0) == 0xE0 ? 2 : (*u & 0xF8) == 0xF0 ? 3 : -1;
        if (n < 0 || *u < 0xC2) return false;           /* a stray continuation, an overlong lead */
        for (int k = 1; k <= n; k++) if ((u[k] & 0xC0) != 0x80) return false;
        u += n + 1;
    }
    return true;
}

const char *orb_places_check(int i, int32_t lat_e6, int32_t lon_e6, const char *name)
{
    if (i < 0 || i >= ORB_PLACES_N) return "no such place (0-2)";
    if (!orb_geo_valid(lat_e6, lon_e6)) return "position out of range";
    if (name && (!places_safe_text(name) || strlen(name) >= ORB_GEO_PLACE_MAX))
        return "name unprintable or too long";
    return NULL;
}

/* ---- end of the pure half --------------------------------------------------------- */

#include "orb_tzrule.h"

#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

static const char *TAG = "places";

/* Its own namespace on `storage`, beside orb.home: a factory reset's partition erase
 * takes it with the other owner settings. One versioned blob a slot, so a cleared slot
 * is an erased key and a later field is an append. */
#define PLACES_PART "storage"
#define PLACES_NS   "orb.places"
#define PLACES_VER  2

/* Version 2 appends the zone its writer named (-1: the nearest city's); a version 1 blob
 * (no zone) still loads, its zone the nearest city's. */
typedef struct {
    uint8_t ver;
    int32_t lat_e6, lon_e6;
    char    name[ORB_GEO_PLACE_MAX];
} place_blob1_t;
typedef struct {
    uint8_t ver;
    int32_t lat_e6, lon_e6;
    char    name[ORB_GEO_PLACE_MAX];
    int16_t zone;
} place_blob_t;

/* The zone a place stands in: the named one, else its nearest city's. */
static void place_zone_fill(orb_place_rec_t *p, int named)
{
    p->zone_named = named >= 0 && named < ORB_GEO_N_ZONES;
    if (p->zone_named) { p->zone = (int16_t)named; return; }
    const int c = orb_geo_nearest(p->lat_e6, p->lon_e6, NULL);
    const int z = c < 0 ? -1 : orb_geo_city_zone(c);
    p->zone = (int16_t)(z >= 0 && z < ORB_GEO_N_ZONES ? z : -1);
}

/* PSRAM: ~150 B nobody reads on the frame path (orb_home.c's reasoning). */
EXT_RAM_BSS_ATTR static orb_place_rec_t s_places[ORB_PLACES_N];
static uint32_t          s_gen;
static SemaphoreHandle_t s_mx;
static StaticSemaphore_t s_mx_buf;

static void lock(void)   { xSemaphoreTake(s_mx, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_mx); }

static void key(int i, char out[4]) { snprintf(out, 4, "p%d", i); }

void orb_places_init(void)
{
    s_mx = xSemaphoreCreateMutexStatic(&s_mx_buf);
    memset(s_places, 0, sizeof s_places);
    nvs_handle_t h;
    if (nvs_open_from_partition(PLACES_PART, PLACES_NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no places set");
        return;
    }
    int n = 0;
    for (int i = 0; i < ORB_PLACES_N; i++) {
        char k[4];
        key(i, k);
        place_blob_t b;
        size_t sz = sizeof b;
        const esp_err_t e = nvs_get_blob(h, k, &b, &sz);
        if (e == ESP_ERR_NVS_NOT_FOUND) continue;
        b.name[sizeof b.name - 1] = 0;
        const bool v1 = e == ESP_OK && sz == sizeof(place_blob1_t) && b.ver == 1;
        if (v1) b.zone = -1;
        if (e != ESP_OK || !(v1 || (sz == sizeof b && b.ver == PLACES_VER)) ||
            orb_places_check(i, b.lat_e6, b.lon_e6, b.name)) {
            ESP_LOGW(TAG, "stored place %d rejected (%s, %u B)", i, esp_err_to_name(e), (unsigned)sz);
            continue;
        }
        s_places[i].set = true;
        s_places[i].lat_e6 = b.lat_e6;
        s_places[i].lon_e6 = b.lon_e6;
        memcpy(s_places[i].name, b.name, sizeof s_places[i].name);
        place_zone_fill(&s_places[i], b.zone);
        n++;
    }
    nvs_close(h);
    s_gen = 1;
    ESP_LOGI(TAG, "%d place%s", n, n == 1 ? "" : "s");
}

bool orb_places_get(int i, orb_place_rec_t *out)
{
    memset(out, 0, sizeof *out);
    if (!s_mx || i < 0 || i >= ORB_PLACES_N) return false;
    lock();
    *out = s_places[i];
    unlock();
    return out->set;
}

uint32_t orb_places_gen(void) { return s_gen; }

static const char *store(int i, const place_blob_t *b)
{
    char k[4];
    key(i, k);
    nvs_handle_t h;
    if (nvs_open_from_partition(PLACES_PART, PLACES_NS, NVS_READWRITE, &h) != ESP_OK)
        return "place not stored (nvs open)";
    esp_err_t e = b ? nvs_set_blob(h, k, b, sizeof *b) : nvs_erase_key(h, k);
    if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK;                  /* clearing an empty slot */
    if (e == ESP_OK && nvs_commit(h) != ESP_OK)
        ESP_LOGW(TAG, "commit failed (kept until reboot)");
    nvs_close(h);
    return e == ESP_OK ? NULL : "place not stored (nvs)";
}

const char *orb_places_set(int i, int32_t lat_e6, int32_t lon_e6, const char *name, const char *zone)
{
    if (!s_mx) return "places not ready";
    const char *why = orb_places_check(i, lat_e6, lon_e6, name);
    if (why) return why;
    const int named = zone && zone[0] ? orb_geo_zone_find(zone) : -1;
    if (zone && zone[0] && named < 0) return "zone not in the zone table";
    place_blob_t b = { .ver = PLACES_VER, .lat_e6 = lat_e6, .lon_e6 = lon_e6, .zone = (int16_t)named };
    if (name && name[0]) snprintf(b.name, sizeof b.name, "%s", name);
    else orb_geo_place(lat_e6, lon_e6, b.name, sizeof b.name);
    why = store(i, &b);
    if (why) return why;
    lock();
    s_places[i].set = true;
    s_places[i].lat_e6 = lat_e6;
    s_places[i].lon_e6 = lon_e6;
    memcpy(s_places[i].name, b.name, sizeof s_places[i].name);
    place_zone_fill(&s_places[i], named);
    s_gen++;
    unlock();
    ESP_LOGI(TAG, "place %d set: %s (%s)", i + 1, b.name, orb_geo_zone_iana(s_places[i].zone));
    return NULL;
}

bool orb_places_zone(int i, int64_t utc, int32_t *off_min, bool *dst, const char **iana, bool *exact)
{
    orb_place_rec_t p;
    if (!orb_places_get(i, &p) || p.zone < 0) return false;
    int32_t off = 0;
    bool d = false;
    if (!orb_tzrule_offset(orb_geo_zone_posix(p.zone), utc, &off, &d)) return false;
    if (off_min) *off_min = off;
    if (dst) *dst = d;
    if (iana) *iana = orb_geo_zone_iana(p.zone);
    if (exact) *exact = orb_geo_zone_exact(p.zone);
    return true;
}

const char *orb_places_clear(int i)
{
    if (!s_mx) return "places not ready";
    if (i < 0 || i >= ORB_PLACES_N) return "no such place (0-2)";
    const char *why = store(i, NULL);
    if (why) return why;
    lock();
    memset(&s_places[i], 0, sizeof s_places[i]);
    s_gen++;
    unlock();
    ESP_LOGI(TAG, "place %d cleared", i + 1);
    return NULL;
}
