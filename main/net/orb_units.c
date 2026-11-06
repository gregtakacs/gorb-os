/* The orb's units — contract in orb_units.h. */
#include "orb_units.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

static const char *TAG = "units";

/* Its own namespace on `storage`, beside orb.home: a factory reset's partition
 * erase takes it with every other owner setting. One blob, versioned, so a
 * later quantity (pressure, say) is an append. */
#define UNITS_PART "storage"
#define UNITS_NS   "orb.units"
#define UNITS_KEY  "units"
#define UNITS_VER  1

typedef struct {
    uint8_t ver;
    uint8_t temp, speed, dist;
} units_blob_t;

static orb_units_t       s_units;        /* zero = metric: the default */
static uint32_t          s_gen = 1;
static SemaphoreHandle_t s_mx;
static StaticSemaphore_t s_mx_buf;

/* The keys the route speaks and the labels the dial shows, by kind then value.
 * The order is the ABI (orb.h ORB_UNIT_*). */
static const char *const KEYS[ORB_UNITS_KINDS][3] = {
    { "c", "f", NULL },
    { "kmh", "mph", "kn" },
    { "km", "mi", "nm" },
};
static const char *const LABELS[ORB_UNITS_KINDS][3] = {
    { "\xC2\xB0" "C", "\xC2\xB0" "F", NULL },
    { "km/h", "mph", "kn" },
    { "km", "mi", "nm" },
};

int orb_units_count(int kind)
{
    switch (kind) {
    case ORB_UNITS_TEMP:  return ORB_UNIT_TEMP_N;
    case ORB_UNITS_SPEED: return ORB_UNIT_SPEED_N;
    case ORB_UNITS_DIST:  return ORB_UNIT_DIST_N;
    default:              return 0;
    }
}

bool orb_units_valid(const orb_units_t *u)
{
    return u && u->temp < ORB_UNIT_TEMP_N && u->speed < ORB_UNIT_SPEED_N &&
           u->dist < ORB_UNIT_DIST_N;
}

const char *orb_units_key(int kind, int v)
{
    if (v < 0 || v >= orb_units_count(kind)) return NULL;
    return KEYS[kind][v];
}

const char *orb_units_label(int kind, int v)
{
    if (v < 0 || v >= orb_units_count(kind)) return NULL;
    return LABELS[kind][v];
}

int orb_units_parse(int kind, const char *key)
{
    if (!key) return -1;
    for (int v = 0; v < orb_units_count(kind); v++)
        if (!strcmp(KEYS[kind][v], key)) return v;
    return -1;
}

void orb_units_init(void)
{
    s_mx = xSemaphoreCreateMutexStatic(&s_mx_buf);
    nvs_handle_t h;
    if (nvs_open_from_partition(UNITS_PART, UNITS_NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "units: metric (none stored)");
        return;
    }
    units_blob_t b;
    size_t sz = sizeof b;
    const esp_err_t e = nvs_get_blob(h, UNITS_KEY, &b, &sz);
    nvs_close(h);
    if (e != ESP_OK || sz != sizeof b || b.ver != UNITS_VER) {
        if (e != ESP_ERR_NVS_NOT_FOUND) ESP_LOGW(TAG, "stored units unreadable (%s, %u B)", esp_err_to_name(e), (unsigned)sz);
        return;
    }
    const orb_units_t u = { b.temp, b.speed, b.dist };
    if (!orb_units_valid(&u)) {
        ESP_LOGW(TAG, "stored units rejected");
        return;
    }
    s_units = u;
    ESP_LOGI(TAG, "units: %s, %s, %s", KEYS[0][u.temp], KEYS[1][u.speed], KEYS[2][u.dist]);
}

void orb_units_get(orb_units_t *out)
{
    if (!s_mx) { memset(out, 0, sizeof *out); return; }
    xSemaphoreTake(s_mx, portMAX_DELAY);
    *out = s_units;
    xSemaphoreGive(s_mx);
}

uint32_t orb_units_gen(void) { return s_gen; }

const char *orb_units_set(const orb_units_t *u)
{
    if (!s_mx) return "units not ready";
    if (!orb_units_valid(u)) return "unknown unit";
    const units_blob_t b = { UNITS_VER, u->temp, u->speed, u->dist };
    nvs_handle_t h;
    if (nvs_open_from_partition(UNITS_PART, UNITS_NS, NVS_READWRITE, &h) != ESP_OK)
        return "units not stored (nvs open)";
    const esp_err_t e = nvs_set_blob(h, UNITS_KEY, &b, sizeof b);
    if (e == ESP_OK && nvs_commit(h) != ESP_OK)
        ESP_LOGW(TAG, "commit failed (kept until reboot)");      /* durability, not correctness */
    nvs_close(h);
    if (e != ESP_OK) return "units not stored (nvs)";
    xSemaphoreTake(s_mx, portMAX_DELAY);
    const bool moved = memcmp(&s_units, u, sizeof s_units) != 0;
    s_units = *u;
    if (moved) s_gen++;
    xSemaphoreGive(s_mx);
    ESP_LOGI(TAG, "units: %s, %s, %s", KEYS[0][u->temp], KEYS[1][u->speed], KEYS[2][u->dist]);
    return NULL;
}
