/* A POSIX TZ rule at any instant — contract in orb_tzrule.h. Pure C, no ESP-IDF. */
#include "orb_tzrule.h"

#include <stddef.h>

/* "<h>[:<mm>]" with an optional sign, in seconds: an offset (seconds WEST of UTC, h up to
 * 24) or, with max_h 167, a changeover's time (POSIX's extension, which tzdata's own rules
 * use: Santiago's "M9.1.6/24", Jerusalem's "M3.4.4/26", newlib reads them alike). */
static const char *rule_hms(const char *p, int32_t *s_out, int max_h)
{
    int sign = 1;
    if (*p == '+' || *p == '-') { sign = *p == '-' ? -1 : 1; p++; }
    if (*p < '0' || *p > '9') return NULL;
    int h = 0, m = 0;
    while (*p >= '0' && *p <= '9') h = h * 10 + (*p++ - '0');
    if (*p == ':') {
        p++;
        if (p[0] < '0' || p[0] > '9' || p[1] < '0' || p[1] > '9') return NULL;
        m = (p[0] - '0') * 10 + (p[1] - '0');
        p += 2;
    }
    if (h > max_h || m > 59) return NULL;
    *s_out = sign * (h * 3600 + m * 60);
    return p;
}
static const char *rule_offset(const char *p, int32_t *west_s) { return rule_hms(p, west_s, 24); }

/* A zone name: letters, or "<...>" as POSIX allows. */
static const char *rule_name(const char *p)
{
    if (*p == '<') { while (*p && *p != '>') p++; return *p ? p + 1 : NULL; }
    const char *s = p;
    while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) p++;
    return p - s >= 3 ? p : NULL;
}

typedef struct { int mon, week, wday; int32_t at_s; } change_t;

/* ",M<m>.<w>.<d>[/<h>[:<mm>]]" */
static const char *rule_change(const char *p, change_t *c)
{
    if (*p++ != ',' || *p++ != 'M') return NULL;
    int v[3] = { 0, 0, 0 };
    for (int k = 0; k < 3; k++) {
        if (*p < '0' || *p > '9') return NULL;
        while (*p >= '0' && *p <= '9') v[k] = v[k] * 10 + (*p++ - '0');
        if (k < 2 && *p++ != '.') return NULL;
    }
    if (v[0] < 1 || v[0] > 12 || v[1] < 1 || v[1] > 5 || v[2] > 6) return NULL;
    c->mon = v[0]; c->week = v[1]; c->wday = v[2]; c->at_s = 2 * 3600;
    if (*p == '/') {
        int32_t t;
        p = rule_hms(p + 1, &t, 167);
        if (!p) return NULL;
        c->at_s = t;
    }
    return p;
}

/* Days since 1970-01-01 of a civil date (Howard Hinnant's days_from_civil). */
static int64_t days_of(int64_t y, int m, int d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static int month_days(int64_t y, int m)
{
    static const int D[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return m == 2 && leap ? 29 : D[m - 1];
}

/* The changeover's day in year y, as days since the epoch: the w-th d-day of the month,
 * week 5 the last one. */
static int64_t change_day(int64_t y, const change_t *c)
{
    const int64_t first = days_of(y, c->mon, 1);
    const int wd1 = (int)(((first % 7) + 7 + 4) % 7);            /* 1970-01-01 was a Thursday */
    int day = 1 + (c->wday - wd1 + 7) % 7 + (c->week - 1) * 7;
    while (day > month_days(y, c->mon)) day -= 7;
    return days_of(y, c->mon, day);
}

/* The year of a UTC instant. */
static int64_t year_of(int64_t utc)
{
    int64_t z = (utc >= 0 ? utc : utc - 86399) / 86400 + 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int64_t doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    return yoe + era * 400 + (mp >= 10 ? 1 : 0);
}

bool orb_tzrule_offset(const char *rule, int64_t utc, int32_t *off_min, bool *dst)
{
    if (!rule) return false;
    int32_t std_w, dst_w;
    const char *p = rule_name(rule);
    if (!p || !(p = rule_offset(p, &std_w))) return false;
    if (!*p) { *off_min = -std_w / 60; *dst = false; return true; }
    if (!(p = rule_name(p))) return false;
    dst_w = std_w - 3600;                                        /* POSIX: an hour ahead when unsaid */
    if (*p != ',' && !(p = rule_offset(p, &dst_w))) return false;
    change_t on, off;
    if (!(p = rule_change(p, &on)) || !(p = rule_change(p, &off)) || *p) return false;
    /* The changeovers are in local time: daylight starts at its time in standard time and
     * ends at its time in daylight time. Each year's two instants, in UTC. */
    const int64_t y = year_of(utc);
    const int64_t start = change_day(y, &on) * 86400 + on.at_s + std_w;
    const int64_t end = change_day(y, &off) * 86400 + off.at_s + dst_w;
    const bool in = start < end ? (utc >= start && utc < end)    /* northern: inside the year */
                                : (utc >= start || utc < end);   /* southern: across the new year */
    *off_min = -(in ? dst_w : std_w) / 60;
    *dst = in;
    return true;
}
