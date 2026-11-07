/* The orb's home location — contract in orb_home.h. */
#include "orb_home.h"

#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#include "orb_net.h"
#include "orb_units.h"

static const char *TAG = "home";

/* Its own namespace on `storage`, beside orb.net: a factory reset's partition
 * erase takes it with every other owner setting, and a stray write here cannot
 * touch the credentials. One blob, versioned, so a later field is an append. */
#define HOME_PART  "storage"
#define HOME_NS    "orb.home"
#define HOME_KEY   "home"
#define HOME_VER   1

typedef struct {
    uint8_t ver;
    int32_t lat_e6, lon_e6;
    char    place[ORB_GEO_PLACE_MAX];
    char    zone[ORB_HOME_ZONE_MAX];
    char    tz[ORB_HOME_TZ_MAX];
} home_blob_t;

/* PSRAM: ~150 B nobody reads on the frame path, and internal RAM is the
 * scarce thing (flight-tracker-design §4). The mutex stays internal. */
EXT_RAM_BSS_ATTR static orb_home_t s_home;
static uint32_t          s_gen;
static SemaphoreHandle_t s_mx;
static StaticSemaphore_t s_mx_buf;

static void lock(void)   { xSemaphoreTake(s_mx, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_mx); }

/* Printable ASCII without the two characters that would end a JSON string:
 * every field is echoed by /api/v1/home with %s, the way the tz guard reasons. */
static bool safe_text(const char *s)
{
    for (; *s; s++)
        if (*s < 0x20 || *s > 0x7e || *s == '"' || *s == '\\') return false;
    return true;
}

void orb_home_init(void)
{
    s_mx = xSemaphoreCreateMutexStatic(&s_mx_buf);
    memset(&s_home, 0, sizeof s_home);
    nvs_handle_t h;
    if (nvs_open_from_partition(HOME_PART, HOME_NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no home set");
        return;
    }
    home_blob_t b;
    size_t sz = sizeof b;
    const esp_err_t e = nvs_get_blob(h, HOME_KEY, &b, &sz);
    nvs_close(h);
    if (e != ESP_OK || sz != sizeof b || b.ver != HOME_VER) {
        if (e != ESP_ERR_NVS_NOT_FOUND) ESP_LOGW(TAG, "stored home unreadable (%s, %u B)", esp_err_to_name(e), (unsigned)sz);
        return;
    }
    b.place[sizeof b.place - 1] = b.zone[sizeof b.zone - 1] = b.tz[sizeof b.tz - 1] = 0;
    if (!orb_geo_valid(b.lat_e6, b.lon_e6) || !safe_text(b.place) || !safe_text(b.zone) || !safe_text(b.tz)) {
        ESP_LOGW(TAG, "stored home rejected");
        return;
    }
    s_home.set = true;
    s_home.lat_e6 = b.lat_e6;
    s_home.lon_e6 = b.lon_e6;
    memcpy(s_home.place, b.place, sizeof s_home.place);
    memcpy(s_home.zone, b.zone, sizeof s_home.zone);
    memcpy(s_home.tz, b.tz, sizeof s_home.tz);
    s_gen = 1;
    ESP_LOGI(TAG, "home: %s (zone %s)", s_home.place, s_home.zone[0] ? s_home.zone : "-");
}

bool orb_home_get(orb_home_t *out)
{
    if (!s_mx) { memset(out, 0, sizeof *out); return false; }
    lock();
    *out = s_home;
    unlock();
    return out->set;
}

uint32_t orb_home_gen(void) { return s_gen; }

void orb_home_propose(int32_t lat_e6, int32_t lon_e6, orb_home_prop_t *p)
{
    memset(p, 0, sizeof *p);
    p->lat_e6 = lat_e6;
    p->lon_e6 = lon_e6;
    orb_geo_place(lat_e6, lon_e6, p->place, sizeof p->place);
    const int c = orb_geo_nearest(lat_e6, lon_e6, &p->city_m);
    const int z = orb_geo_city_zone(c);
    orb_geo_city_cc(c, p->cc);
    p->zone = orb_geo_zone_iana(z);
    p->exact = orb_geo_zone_exact(z);
    /* The zone table's own key when it has the zone: the hub's list and the
     * dial's zone page then mark the row, and the echo reads "US/Arizona"
     * rather than a rule nobody typed. */
    const int row = orb_net_tz_find_iana(p->zone);
    p->tz = row >= 0 ? orb_net_tz_key(row) : orb_geo_zone_posix(z);
}

static const char *store(const home_blob_t *b)
{
    nvs_handle_t h;
    if (nvs_open_from_partition(HOME_PART, HOME_NS, NVS_READWRITE, &h) != ESP_OK) return "home not stored (nvs open)";
    esp_err_t e = b ? nvs_set_blob(h, HOME_KEY, b, sizeof *b) : nvs_erase_key(h, HOME_KEY);
    if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK;                  /* clearing an unset home */
    if (e == ESP_OK && nvs_commit(h) != ESP_OK)
        ESP_LOGW(TAG, "commit failed (kept until reboot)");      /* durability, not correctness */
    nvs_close(h);
    return e == ESP_OK ? NULL : "home not stored (nvs)";
}

const char *orb_home_set(int32_t lat_e6, int32_t lon_e6, const char *place,
                         const char *tz, const char *zone)
{
    if (!s_mx) return "home not ready";
    if (!orb_geo_valid(lat_e6, lon_e6)) return "position out of range";
    if (place && (!safe_text(place) || strlen(place) >= ORB_GEO_PLACE_MAX)) return "place unprintable or too long";
    if (zone && (!safe_text(zone) || strlen(zone) >= ORB_HOME_ZONE_MAX)) return "zone name unprintable or too long";

    home_blob_t b = { .ver = HOME_VER, .lat_e6 = lat_e6, .lon_e6 = lon_e6 };
    orb_home_prop_t p;
    orb_home_propose(lat_e6, lon_e6, &p);
    snprintf(b.place, sizeof b.place, "%s", place && place[0] ? place : p.place);

    if (!tz) {
        snprintf(b.tz, sizeof b.tz, "%s", p.tz);
        snprintf(b.zone, sizeof b.zone, "%s", p.zone);
    } else if (!strcmp(tz, "keep")) {
        b.tz[0] = b.zone[0] = 0;
    } else {
        if (strlen(tz) >= sizeof b.tz) return "tz too long";
        snprintf(b.tz, sizeof b.tz, "%s", tz);
        if (zone) {
            snprintf(b.zone, sizeof b.zone, "%s", zone);
        } else {
            /* A table key names its own zone; a bare rule names nothing. */
            for (int i = 0; i < orb_net_tz_count(); i++)
                if (!strcmp(orb_net_tz_key(i), tz)) {
                    snprintf(b.zone, sizeof b.zone, "%s", orb_net_tz_iana(i));
                    break;
                }
        }
    }

    /* The zone first: it is the half that can be refused, and a refusal must
     * leave the old home standing rather than a new home beside an old zone. */
    if (b.tz[0]) {
        const char *why = orb_net_tz_set_str(b.tz);
        if (why) return why;
    }
    const char *why = store(&b);
    if (why) return why;

    lock();
    const bool first = !s_home.set;
    s_home.set = true;
    s_home.lat_e6 = lat_e6;
    s_home.lon_e6 = lon_e6;
    memcpy(s_home.place, b.place, sizeof s_home.place);
    memcpy(s_home.zone, b.zone, sizeof s_home.zone);
    memcpy(s_home.tz, b.tz, sizeof s_home.tz);
    s_gen++;
    unlock();
    ESP_LOGI(TAG, "home set: %s (zone %s)", b.place, b.tz[0] ? b.tz : "kept");
    /* The orb's first home picks its units, when nobody has picked them yet (owner,
     * 2026-11-07: units follow home at setup): the nearest city's country's. A later
     * move keeps whatever the units are. */
    if (first && !orb_units_stored() && p.cc[0]) {
        orb_units_t u;
        orb_units_for_country(p.cc, &u);
        const char *uwhy = orb_units_set(&u);
        if (uwhy) ESP_LOGW(TAG, "units for %s not set: %s", p.cc, uwhy);
        else ESP_LOGI(TAG, "units follow home: %s", p.cc);
    }
    return NULL;
}

const char *orb_home_clear(void)
{
    if (!s_mx) return "home not ready";
    const char *why = store(NULL);
    if (why) return why;
    lock();
    memset(&s_home, 0, sizeof s_home);
    s_gen++;
    unlock();
    ESP_LOGI(TAG, "home cleared");
    return NULL;
}

bool orb_home_zone_follows(void)
{
    if (!s_mx) return false;
    lock();
    const bool f = s_home.set && s_home.tz[0] && !strcmp(s_home.tz, orb_net_tz_current());
    unlock();
    return f;
}

void orb_home_zone_label(char *out, size_t outsz)
{
    if (s_mx) {
        lock();
        const bool f = s_home.set && s_home.tz[0] && s_home.zone[0] &&
                       !strcmp(s_home.tz, orb_net_tz_current());
        if (f) snprintf(out, outsz, "%s", s_home.zone);
        unlock();
        if (f) return;
    }
    const int row = orb_net_tz_current_index();
    snprintf(out, outsz, "%s", row >= 0 ? orb_net_tz_label(row) : orb_net_tz_current());
}
