/* orb_lv_blend.h — LVGL's custom-ASM blend hooks, pointed at the dial's rounding blend.
 *
 * CONFIG_LV_DRAW_SW_ASM_CUSTOM_INCLUDE names this file, and the top-level CMakeLists
 * puts main/lvhook on every component's include path, so LVGL's software renderer
 * includes it (lv_draw_sw_blend.h and three .c files). It is read inside LVGL's own
 * headers, before the blend descriptor is defined, so it declares no LVGL types and
 * returns plain ints (LV_RESULT_OK / LV_RESULT_INVALID).
 *
 * The three hooks are the RGB565-onto-RGB565 image blends that carry a coverage: the
 * plate, the hand and pointer sprites (RGB565 plus A8 mask) and the crystal overlay
 * when LVGL draws them itself — the first paint and the wedges orb_gfx does not. They
 * run LVGL's own loops with mix_16_16's rounding (D43 §5.4), so a texel blended by
 * LVGL equals the texel orb_gfx.c's blend_px produces. The opaque copy and every
 * other format keep LVGL's own code (the undefined hooks default to INVALID). */
#ifndef ORB_LV_BLEND_H
#define ORB_LV_BLEND_H

struct _lv_draw_sw_blend_image_dsc_t;

int orb_lv_rgb565_with_opa(struct _lv_draw_sw_blend_image_dsc_t *dsc);
int orb_lv_rgb565_with_mask(struct _lv_draw_sw_blend_image_dsc_t *dsc);
int orb_lv_rgb565_mix_mask_opa(struct _lv_draw_sw_blend_image_dsc_t *dsc);

#define LV_DRAW_SW_RGB565_BLEND_NORMAL_TO_RGB565_WITH_OPA(dsc)      orb_lv_rgb565_with_opa(dsc)
#define LV_DRAW_SW_RGB565_BLEND_NORMAL_TO_RGB565_WITH_MASK(dsc)     orb_lv_rgb565_with_mask(dsc)
#define LV_DRAW_SW_RGB565_BLEND_NORMAL_TO_RGB565_MIX_MASK_OPA(dsc)  orb_lv_rgb565_mix_mask_opa(dsc)

#endif
