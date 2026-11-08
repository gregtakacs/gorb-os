/* The OS's maps (radar-plan.md §2.4): for home and each place, six .orbmap levels on the
 * card (z6-z11, written by the hub's map tool), drawn on the orb at whatever radius,
 * rotation, line widths and dashes an app's view asks for.
 *
 * A view renders once, on the map task, into a 4-bit role raster (orb_map_raster.c); the
 * app then blends that raster through its theme's palette every frame
 * (orb_ui_cv_roles), which costs no render. Each app keeps two views (the one shown and
 * the one rendering) and the level files it is using in PSRAM, all freed 15 minutes after
 * it leaves the glass or at once when it is released. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define ORB_MAP_VIEWS      2     /* per app */
#define ORB_MAP_MARKS_MAX  32    /* landmarks kept per view, best rank first */
#define ORB_MAP_DIR        "/System/Maps"

typedef struct {               /* the app's view, as the SDK's orb_map_view_t (44 B) */
    int32_t  radius_m;
    int32_t  rot_cdeg;
    uint16_t role_on;
    uint8_t  mark_on;
    uint8_t  pad;
    uint8_t  width[16];
    uint8_t  dash[16];
} orb_map_req_t;

typedef struct {
    int16_t x, y;
    uint8_t kind, rank;
    char    name[24];
} orb_map_markrec_t;

enum {
    ORB_MAP_RENDERING = 0,
    ORB_MAP_READY = 1,
    ORB_MAP_ABSENT = -1,       /* no such place, or its map is not on the card */
    ORB_MAP_NOCARD = -2,
    ORB_MAP_BAD = -3,          /* the file failed its check */
    ORB_MAP_NOMEM = -4,
};

/* The file name a place's level has on the card, under ORB_MAP_DIR: "<place>-<lat_e4>_<lon_e4>-z<z>-f2.orbmap",
 * coordinates in ten-thousandths of a degree, rounded half away from zero. tools/orbmap.py
 * and the hub's writer name files the same way. place 0 is home, 1-3 the places.
 * The name is FORMAT 2's ("-f2"; FORMAT 1's had none): each FORMAT takes a name of its own,
 * so an OS that reads it never opens an older file, and the hub, which removes every map
 * file no place names, replaces the old files on its next Make maps. */
void orb_map_file_name(int place, int32_t lat_e6, int32_t lon_e6, int z, char *out, int cap);

/* Open a view for app_slot: answers a handle (1..ORB_MAP_VIEWS) and starts its render, or
 * an ORB_MAP_* error. Opening a third view closes the oldest. */
int32_t  orb_map_open(int app_slot, int place, const orb_map_req_t *req);
int32_t  orb_map_ready(int app_slot, int32_t h);
const uint8_t *orb_map_raster_of(int app_slot, int32_t h);   /* NULL until ready */
int32_t  orb_map_mark(int app_slot, int32_t h, int i, orb_map_markrec_t *out);
int32_t  orb_map_project(int app_slot, int32_t h, int32_t lat_e6, int32_t lon_e6, int32_t *x, int32_t *y);
void     orb_map_close(int app_slot, int32_t h);
void     orb_map_release_app(int app_slot);
void     orb_map_note_glass(int app_slot, bool on_glass, int64_t now_us);
/* Everything the app holds here, now (orb_app_want_room): its views close, its files go. */
void     orb_map_drop(int app_slot);

/* Moves when the hub has written any place's maps (POST /api/v1/places/maps), so an app
 * re-opens its views; the level files every app holds are dropped with it. */
uint32_t orb_maps_gen(void);
void     orb_maps_bump(void);

/* For /api/v1/status: renders done, the last one's ms, and the files held. */
void orb_map_stats(uint32_t *renders, uint32_t *last_ms, uint32_t *files, uint32_t *file_bytes);
/* PSRAM an app holds here: its level files and its views' rasters (/status, §6.21). */
uint32_t orb_map_app_bytes(int app_slot);
