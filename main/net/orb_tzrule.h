#pragma once
/* A POSIX TZ rule evaluated for any instant, without touching the process's own zone
 * (newlib's localtime applies one TZ, the owner's). The places beside home each carry a
 * zone (radar-plan §2.1, owner 2026-11-08: "apps building on the location have the same
 * information that we have in the OS"), so the OS answers a place's offset at a time
 * from its zone's rule.
 *
 * The forms are the zone table's (main/net/orb_cities.c, tools/make_cities.py): a
 * standard offset "STD<h>[:<mm>]", optionally a daylight one "DST<h>[:<mm>]" and two
 * changeovers ",M<m>.<w>.<d>[/<h>],M<m>.<w>.<d>[/<h>]" (week 5 = the month's last;
 * the time in the zone's local time before the change, 2 when absent, and, as POSIX's
 * extension and tzdata's own rules have it, anywhere in -167..167 hours: "M3.4.4/26" is
 * the Thursday's 26th hour, the Friday's 02:00). A southern rule
 * starts after it ends. POSIX offsets are west of UTC, so "STD5" is UTC-5. Pure: lifted
 * by tools/host_tzrule_test.py, which holds every rule in the table to Python's zoneinfo. */
#include <stdbool.h>
#include <stdint.h>

/* The rule's offset at `utc` (seconds since the epoch), in minutes EAST of UTC, and
 * whether daylight time is in force. False for a rule this cannot read (nothing is
 * written then). */
bool orb_tzrule_offset(const char *rule, int64_t utc, int32_t *off_min, bool *dst);
