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
#define PLACES_VER  1

typedef struct {
    uint8_t ver;
    int32_t lat_e6, lon_e6;
    char    name[ORB_GEO_PLACE_MAX];
} place_blob_t;

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
        if (e != ESP_OK || sz != sizeof b || b.ver != PLACES_VER ||
            orb_places_check(i, b.lat_e6, b.lon_e6, b.name)) {
            ESP_LOGW(TAG, "stored place %d rejected (%s, %u B)", i, esp_err_to_name(e), (unsigned)sz);
            continue;
        }
        s_places[i].set = true;
        s_places[i].lat_e6 = b.lat_e6;
        s_places[i].lon_e6 = b.lon_e6;
        memcpy(s_places[i].name, b.name, sizeof s_places[i].name);
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

const char *orb_places_set(int i, int32_t lat_e6, int32_t lon_e6, const char *name)
{
    if (!s_mx) return "places not ready";
    const char *why = orb_places_check(i, lat_e6, lon_e6, name);
    if (why) return why;
    place_blob_t b = { .ver = PLACES_VER, .lat_e6 = lat_e6, .lon_e6 = lon_e6 };
    if (name && name[0]) snprintf(b.name, sizeof b.name, "%s", name);
    else orb_geo_place(lat_e6, lon_e6, b.name, sizeof b.name);
    why = store(i, &b);
    if (why) return why;
    lock();
    s_places[i].set = true;
    s_places[i].lat_e6 = lat_e6;
    s_places[i].lon_e6 = lon_e6;
    memcpy(s_places[i].name, b.name, sizeof s_places[i].name);
    s_gen++;
    unlock();
    ESP_LOGI(TAG, "place %d set: %s", i + 1, b.name);
    return NULL;
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
