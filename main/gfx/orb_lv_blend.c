/* orb_lv_blend.c — LVGL's RGB565 image blends with the dial's rounding mix (D43 §5.4).
 *
 * The bodies are rgb565_image_blend()'s three coverage branches from LVGL 9.6
 * (src/draw/sw/blend/lv_draw_sw_blend_to_rgb565.c), loop for loop, with one change:
 * the per-texel mix is mix_rounded(), which is orb_gfx.c's mix_16_16 token for token.
 * tools/watch-composer/_selftest/headless-panel.test.js lifts both and fails if they
 * ever differ, so the two paint paths cannot drift apart again. */
#include "lvgl.h"
#include "draw/sw/blend/lv_draw_sw_blend_private.h"
#include "orb_lv_blend.h"

static inline uint16_t mix_rounded(uint16_t c1, uint16_t c2, uint8_t mix)
{
    if (mix == 255) return c1;
    if (mix == 0)   return c2;
    if (c1 == c2)   return c1;
    uint32_t mix5 = ((uint32_t)mix + 4) >> 3;
    uint32_t bg = (uint32_t)(c2 | ((uint32_t)c2 << 16)) & 0x7E0F81F;
    uint32_t fg = (uint32_t)(c1 | ((uint32_t)c1 << 16)) & 0x7E0F81F;
    uint32_t res = ((((fg - bg) * mix5 + 0x2008010u) >> 5) + bg) & 0x7E0F81F;
    return (uint16_t)((res >> 16) | res);
}

static inline void *next_row(const void *buf, int32_t stride)
{
    return (void *)((const uint8_t *)buf + stride);
}

int LV_ATTRIBUTE_FAST_MEM orb_lv_rgb565_with_opa(struct _lv_draw_sw_blend_image_dsc_t *dsc)
{
    uint16_t *dest = dsc->dest_buf;
    const uint16_t *src = dsc->src_buf;
    const lv_opa_t opa = dsc->opa;
    for (int32_t y = 0; y < dsc->dest_h; y++) {
        for (int32_t x = 0; x < dsc->dest_w; x++) dest[x] = mix_rounded(src[x], dest[x], opa);
        dest = next_row(dest, dsc->dest_stride);
        src = next_row(src, dsc->src_stride);
    }
    return LV_RESULT_OK;
}

int LV_ATTRIBUTE_FAST_MEM orb_lv_rgb565_with_mask(struct _lv_draw_sw_blend_image_dsc_t *dsc)
{
    uint16_t *dest = dsc->dest_buf;
    const uint16_t *src = dsc->src_buf;
    const lv_opa_t *mask = dsc->mask_buf;
    for (int32_t y = 0; y < dsc->dest_h; y++) {
        for (int32_t x = 0; x < dsc->dest_w; x++) {
            /* the image's own alpha plane: mostly 0 or 255, both decided before the mix */
            uint32_t a = mask[x];
            if (a == 0) continue;
            dest[x] = a >= LV_OPA_MAX ? src[x] : mix_rounded(src[x], dest[x], (uint8_t)a);
        }
        dest = next_row(dest, dsc->dest_stride);
        src = next_row(src, dsc->src_stride);
        mask += dsc->mask_stride;
    }
    return LV_RESULT_OK;
}

int LV_ATTRIBUTE_FAST_MEM orb_lv_rgb565_mix_mask_opa(struct _lv_draw_sw_blend_image_dsc_t *dsc)
{
    uint16_t *dest = dsc->dest_buf;
    const uint16_t *src = dsc->src_buf;
    const lv_opa_t *mask = dsc->mask_buf;
    const lv_opa_t opa = dsc->opa;
    for (int32_t y = 0; y < dsc->dest_h; y++) {
        for (int32_t x = 0; x < dsc->dest_w; x++) dest[x] = mix_rounded(src[x], dest[x], LV_OPA_MIX2(mask[x], opa));
        dest = next_row(dest, dsc->dest_stride);
        src = next_row(src, dsc->src_stride);
        mask += dsc->mask_stride;
    }
    return LV_RESULT_OK;
}
