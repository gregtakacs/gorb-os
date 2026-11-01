#pragma once
/* The flight tracker's view and app glue (docs/flight-tracker-design.md §6).
 *
 * A compiled behaviour module (ORB_BEHAVIOUR_FLIGHTS), registered in
 * orb_app.c like the picker. The feed itself is the OS's orb_flights service;
 * this file owns the settings, decides the service's rate (visible,
 * background, off) and draws the nearest aircraft around home. */
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "lvgl.h"
#include "orb_app.h"

esp_err_t flights_view_init(orb_app_t *app);
lv_obj_t *flights_view_screen(void);
void      flights_view_set_visible(bool visible);   /* activate / suspend */
void      flights_view_show(void);
void      flights_view_input(int32_t delta, bool pressed);
bool      flights_view_armed(void);
void      flights_view_set_armed(bool armed);
void      flights_view_service(void);

int          flights_opt_count(orb_app_t *app);
const char  *flights_opt_label(orb_app_t *app, int idx);
const char  *flights_opt_value(orb_app_t *app, int idx);
void         flights_opt_sel(orb_app_t *app, int idx);
