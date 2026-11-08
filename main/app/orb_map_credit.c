/* The OS maps' on-glass credit — contract in orb_map_credit.h.
 *
 * The carousel chip's pattern (orb_carousel.c): a label on LVGL's top layer, so it
 * stands over whatever app draws the map and is no app's widget (an app's own widget
 * would turn its dial's fast path off), and its box goes into the compositor's keep-out
 * (GFX_KO_CREDIT) so a map redrawn under it does not paint over it. Opaque, for the same
 * reason the chip is. At the foot of the glass, clear of the chip's band (306-369) and
 * inside the circle (at y 432 the glass is 242 px wide). */
#include "orb_map_credit.h"

#include "esp_log.h"
#include "lvgl.h"
#include "orb_gfx.h"

static const char *TAG = "mapcredit";

#define CREDIT_W     200
#define CREDIT_H      24
#define CREDIT_FOOT   34       /* px from the glass's bottom edge to the box's */
#define CREDIT_MS   5000       /* OSMF: a credit may fade "automatically after five seconds" */

static lv_obj_t   *s_box;
static lv_timer_t *s_timer;
static bool        s_shown;    /* once a boot */
static bool        s_up;

static void credit_off(void)
{
    if (!s_up) return;
    s_up = false;
    if (s_box) lv_obj_add_flag(s_box, LV_OBJ_FLAG_HIDDEN);
    gfx_keepout_clear(GFX_KO_CREDIT);          /* the map takes its pixels back */
}

static void credit_due(lv_timer_t *t)
{
    (void)t;
    s_timer = NULL;                            /* a one-shot deletes itself */
    credit_off();
}

static bool credit_build(void)
{
    if (s_box) return true;
    lv_obj_t *top = lv_layer_top();
    if (!top) return false;
    s_box = lv_obj_create(top);
    if (!s_box) return false;
    lv_obj_t *l = lv_label_create(s_box);
    if (!l) return false;
    lv_obj_set_size(s_box, CREDIT_W, CREDIT_H);
    lv_obj_align(s_box, LV_ALIGN_BOTTOM_MID, 0, -CREDIT_FOOT);
    lv_obj_set_style_bg_color(s_box, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_box, CREDIT_H / 2, 0);
    lv_obj_set_style_border_width(s_box, 0, 0);
    lv_obj_set_style_pad_all(s_box, 0, 0);
    lv_obj_set_scrollable(s_box, false);
    lv_obj_set_scrollbar_mode(s_box, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(s_box, LV_OBJ_FLAG_FLOATING);
    lv_obj_remove_flag(s_box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    /* "Map data from OpenStreetMap" is a form the guidelines name; the built-in fonts
     * carry no copyright sign. */
    lv_label_set_text(l, "Map data: OpenStreetMap");
    lv_obj_center(l);
    lv_obj_add_flag(s_box, LV_OBJ_FLAG_HIDDEN);
    return true;
}

void orb_map_credit_drawn(const orb_app_t *app)
{
    if (s_shown || !app || app != orb_app_current()) return;
    if (!credit_build()) return;
    s_shown = true;
    s_up = true;
    lv_obj_remove_flag(s_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_box);
    lv_obj_update_layout(s_box);
    lv_area_t a;
    lv_obj_get_coords(s_box, &a);
    gfx_keepout_set(GFX_KO_CREDIT, a.x1, a.y1, a.x2, a.y2);
    s_timer = lv_timer_create(credit_due, CREDIT_MS, NULL);
    if (s_timer) lv_timer_set_repeat_count(s_timer, 1);
    ESP_LOGI(TAG, "map credit shown for %s", app->slug ? app->slug : "?");
}

void orb_map_credit_hide(void)
{
    if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
    credit_off();
}

bool orb_map_credit_shown(void) { return s_shown; }
