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

/* The stored record. `time` was appended in OS v1.2.16 and `pressure` in v1.2.17: a
 * record stored before either is shorter and reads that quantity as its first choice
 * (24-hour, hPa), so an update keeps the owner's units. */
typedef struct {
    uint8_t ver;
    uint8_t temp, speed, dist;
    uint8_t time;
    uint8_t pressure;
} units_blob_t;
#define UNITS_BLOB_V1 4u               /* the record before `time` */
#define UNITS_BLOB_V2 5u               /* the record before `pressure` */

static orb_units_t       s_units;        /* zero = metric: the default */
static uint32_t          s_gen = 1;
static bool              s_stored;       /* a record was read at boot or set since */
static SemaphoreHandle_t s_mx;
static StaticSemaphore_t s_mx_buf;

/* The keys the route speaks and the labels the dial shows, by kind then value.
 * The order is the ABI (orb.h ORB_UNIT_*). */
static const char *const KEYS[ORB_UNITS_KINDS][3] = {
    { "c", "f", NULL },
    { "kmh", "mph", "kn" },
    { "km", "mi", "nm" },
    { "24h", "12h", NULL },
    { "hpa", "inhg", "mmhg" },
};
static const char *const LABELS[ORB_UNITS_KINDS][3] = {
    { "\xC2\xB0" "C", "\xC2\xB0" "F", NULL },
    { "km/h", "mph", "kn" },
    { "km", "mi", "nm" },
    { "24-hour", "12-hour", NULL },
    { "hPa", "inHg", "mmHg" },
};

int orb_units_count(int kind)
{
    switch (kind) {
    case ORB_UNITS_TEMP:  return ORB_UNIT_TEMP_N;
    case ORB_UNITS_SPEED: return ORB_UNIT_SPEED_N;
    case ORB_UNITS_DIST:  return ORB_UNIT_DIST_N;
    case ORB_UNITS_TIME:  return ORB_UNIT_TIME_N;
    case ORB_UNITS_PRESSURE: return ORB_UNIT_PRESSURE_N;
    default:              return 0;
    }
}

bool orb_units_valid(const orb_units_t *u)
{
    return u && u->temp < ORB_UNIT_TEMP_N && u->speed < ORB_UNIT_SPEED_N &&
           u->dist < ORB_UNIT_DIST_N && u->time < ORB_UNIT_TIME_N &&
           u->pressure < ORB_UNIT_PRESSURE_N;
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
    memset(&b, 0, sizeof b);
    size_t sz = sizeof b;
    const esp_err_t e = nvs_get_blob(h, UNITS_KEY, &b, &sz);
    nvs_close(h);
    if (e != ESP_OK || (sz != sizeof b && sz != UNITS_BLOB_V2 && sz != UNITS_BLOB_V1) || b.ver != UNITS_VER) {
        if (e != ESP_ERR_NVS_NOT_FOUND) ESP_LOGW(TAG, "stored units unreadable (%s, %u B)", esp_err_to_name(e), (unsigned)sz);
        return;
    }
    const orb_units_t u = { b.temp, b.speed, b.dist, sz >= UNITS_BLOB_V2 ? b.time : ORB_UNIT_24H,
                            sz == sizeof b ? b.pressure : ORB_UNIT_HPA };
    if (!orb_units_valid(&u)) {
        ESP_LOGW(TAG, "stored units rejected");
        return;
    }
    s_units = u;
    s_stored = true;
    ESP_LOGI(TAG, "units: %s, %s, %s, %s, %s", KEYS[0][u.temp], KEYS[1][u.speed], KEYS[2][u.dist],
             KEYS[3][u.time], KEYS[4][u.pressure]);
}

bool orb_units_stored(void) { return s_stored; }

const char *orb_units_forget(void)
{
    if (!s_mx) return "units not ready";
    nvs_handle_t h;
    if (nvs_open_from_partition(UNITS_PART, UNITS_NS, NVS_READWRITE, &h) != ESP_OK)
        return "units not forgotten (nvs open)";
    esp_err_t e = nvs_erase_key(h, UNITS_KEY);
    if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK;
    if (e == ESP_OK) nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK) return "units not forgotten (nvs)";
    xSemaphoreTake(s_mx, portMAX_DELAY);
    memset(&s_units, 0, sizeof s_units);
    s_stored = false;
    s_gen++;
    xSemaphoreGive(s_mx);
    ESP_LOGW(TAG, "units forgotten (bench): metric until set");
    return NULL;
}

/* ISO 3166 codes, two letters each, run together. */
static bool cc_in(const char *cc, const char *list)
{
    for (const char *p = list; p[0] && p[1]; p += 2)
        if (p[0] == cc[0] && p[1] == cc[1]) return true;
    return false;
}

void orb_units_for_country(const char *cc, orb_units_t *out)
{
    memset(out, 0, sizeof *out);                                /* metric, 24-hour */
    if (!cc || !cc[0] || !cc[1]) return;
    /* the United States and its territories, and the Fahrenheit holdouts */
    if (cc_in(cc, "USPRGUVIASMPLRPWFMMHBSBZKY")) out->temp = ORB_UNIT_F;
    /* road speeds and distances in miles */
    if (cc_in(cc, "USPRGUVIASMPGBLRMM")) { out->speed = ORB_UNIT_MPH; out->dist = ORB_UNIT_MI; }
    /* where the 12-hour clock is the everyday one */
    if (cc_in(cc, "USPRGUVIASMPCAAUNZINPHPKBDEGSA")) out->time = ORB_UNIT_12H;
    /* barometers read in inches of mercury in the US, millimetres in Russia and Belarus */
    if (cc_in(cc, "USPRGUVIASMP")) out->pressure = ORB_UNIT_INHG;
    else if (cc_in(cc, "RUBY")) out->pressure = ORB_UNIT_MMHG;
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
    const units_blob_t b = { UNITS_VER, u->temp, u->speed, u->dist, u->time, u->pressure };
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
    s_stored = true;
    xSemaphoreGive(s_mx);
    ESP_LOGI(TAG, "units: %s, %s, %s, %s, %s", KEYS[0][u->temp], KEYS[1][u->speed],
             KEYS[2][u->dist], KEYS[3][u->time], KEYS[4][u->pressure]);
    return NULL;
}
