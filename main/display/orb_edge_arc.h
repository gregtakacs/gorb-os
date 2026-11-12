#pragma once
/**
 * The system's edge arc: a thin progress ring around the rim of the glass, over whatever
 * is on it (owner, 2026-11-09: "a system process overlay on top of the display for the
 * copy status, maybe an arch around the edge while copying not a full blown block").
 *
 * It is no LVGL widget and never enters the mirror: display.c stamps it into each DMA tile
 * as the tile leaves for the panel, so a themed dial keeps its direct path, the mirror the
 * compositor compares against stays the dial's own, and the arc stands over the dial, the
 * menu, the carousel and the fill's face alike. The theme rings' look (theme_strip §61): a
 * bright arc clockwise from twelve over a dim track.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Show the arc at `permille` of the way round (0..1000), or hide it (< 0). Any task. */
void orb_edge_arc_set(int permille);

/** LVGL task, once a loop before lv_timer_handler: a changed arc marks the rows it changed
 *  and asks LVGL for a frame, so a still page carries them too. At most 10 Hz for a moving
 *  arc; a show or a hide at once. */
void orb_edge_arc_service(void);

/** display.c, LVGL task: stamp the arc into `rows` rows of a tile in wire order, from glass
 *  row `y0`, columns [x0, x0 + w) of the glass, the tile's stride `w`. */
void orb_edge_arc_stamp(int32_t y0, int rows, uint16_t *tile, int32_t x0, int32_t w);

/** Is a card copy in progress (the arc is shown, s_shown >= 0)? ring-design §8: the copy
 *  arc wins the rim while this is true; the display.c rim dispatch reads it every frame. */
bool orb_edge_arc_shown(void);

#ifdef __cplusplus
}
#endif
