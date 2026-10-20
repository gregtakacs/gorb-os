/* clk_types.h — the analog dial's geometry types.
 *
 * §26 (P6.5) retired the FORMAT 2 dialect and the compiled assets/clock_assets
 * pair with it. These structs were generated into that header by
 * tools/bake_clock.py and are the runtime geometry every clock draws into —
 * FORMAT 3's reader (main/art/orb_clock3.c) projects a bundle's element table
 * straight into a clk_theme_t, and clock_view.c reads one. The types outlive the
 * FORMAT 2 pair that first defined them, so they now live here, hand-authored.
 *
 * No generated data lives here: no clk_theme instance, no pixel-plane arrays, no
 * CLK_BLOB_BYTES. The dial's geometry comes from a resident container — the
 * flashed slot-A default bundle or an installed one — never from a compiled blob.
 * CLK_SCR_W/H are the panel's own resolution, fixed for the device.
 */
#pragma once

#include <stdint.h>

#define CLK_SCR_W 466
#define CLK_SCR_H 466

typedef struct {
  uint16_t w, h;            /* cropped ink size */
  int16_t  offx, offy;      /* crop origin in the original canvas */
  int16_t  pivotx, pivoty;  /* rotation pivot in CROPPED coords */
  int16_t  centerx, centery;/* screen point the pivot maps to */
  uint8_t  blend;           /* 0 normal 1 multiply 2 screen */
  uint8_t  show;            /* draw this layer */
  uint32_t color_off;       /* blob offset of the colour plane (w*h*2);
                             *   the alpha plane follows at +w*h*2, stride w */
  int16_t  diffx, diffy;    /* tourbillon frames only: rect that differs
                             *   from the PREVIOUS frame, CROPPED coords;
                             *   0,0 = identical to its predecessor */
  uint16_t diffw, diffh;
} clk_sprite_t;

#define CLK_TOURB_MAX 16

typedef struct {
  uint32_t bg;
  uint8_t  plate_follow;    /* 0 none 1 hour 2 minute 3 second */
  uint8_t  shadow_on;
  int8_t   shadow_dx, shadow_dy;
  uint8_t  second_sweep, second_railway, text_over_hands, tick_rate;
  uint16_t bg_frames;       /* distinct extra plate frames (0 = still) */
  int16_t  bg_fps; uint8_t bg_loop; uint16_t bg_every;
  uint32_t plate_off;       /* full-screen RGB565 colour plane offset */
  uint8_t  order[5]; uint8_t order_n;
  uint8_t  matte_mask;      /* bit i = hand slot i folded into the
                             *   background at bake time, never
                             *   composited at runtime; bit 5 = the
                             *   glass overlay was folded too */
  clk_sprite_t hand[5];
  clk_sprite_t shadow[5];
  clk_sprite_t overlay;
  uint8_t  bg_mode;         /* 0 = plate + bgAnim frames, 1 = animated delta bank */
  uint32_t bg_key_off;      /* mode 1: full RGB565 keyframe (frame 0) */
  uint16_t gif_frames;      /* mode 1: frames in the loop (incl. keyframe) */
  uint16_t tourbillon_n;    /* second-hand frame bank size (0 = plain hand) */
  uint16_t tourbillon_fps;  /* bank playback tempo */
  clk_sprite_t tourbillon[CLK_TOURB_MAX];  /* shown frame's pixels replace hand[2]'s */
} clk_theme_t;

typedef struct {
  uint16_t x, y, w, h;  /* screen rect */
  uint32_t off;         /* blob offset of the RGB565 payload */
} clk_bg_rect_t;
