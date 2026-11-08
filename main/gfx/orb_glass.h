/* orb_glass.h — the glass heap (radar-plan §6.21).
 *
 * The big buffers that go with whatever is on the glass: the stage's live layers and its
 * glass overlay square, the clock's pre-paint planes, the map service's render scratch and
 * rasters, an app's kept bodies, the theme carousel's pictures. They are 100 KB to 650 KB
 * each and come and go with the screen, and in the general PSRAM heap they cut it up until
 * a 651 KB layer no longer fits with far more than that free (Gold, 2026-11-08: the radar's
 * rain layer refused at 733 KB free). Here they share one region of their own, taken once
 * and never given back, so they fragment only each other, and only while something is on
 * the glass: when it leaves, its buffers go and the region is whole again.
 *
 * The region is carved at the first ask, not at boot: by then the apps are loaded, so it
 * takes what the board can spare (up to ORB_GLASS_MAX, leaving ORB_GLASS_RESERVE). An ask
 * the region cannot meet first lets the apps off the glass give back their lingering
 * caches (orb_glass_set_reclaim), then falls back to the general heap, counted. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ORB_GLASS_MAX     (3u * 1024u * 1024u)
#define ORB_GLASS_RESERVE (1024u * 1024u)       /* left to the general heap at the carve */

void *orb_glass_alloc(size_t n);       /* the glass region, else the general PSRAM heap */
void *orb_glass_alloc_only(size_t n);  /* the glass region alone, or NULL */
void  orb_glass_free(void *p);         /* either heap's; NULL is fine */
bool  orb_glass_owns(const void *p);

/* What frees lingering caches when the region runs short: called on the task that set it
 * (the LVGL task), with no glass lock held. Elsewhere a miss only falls back. */
void  orb_glass_set_reclaim(void (*fn)(void));

typedef struct { uint32_t size, free, largest, fallbacks; } orb_glass_info_t;
void  orb_glass_info(orb_glass_info_t *o);
