/* The setup screen (app-platform-plan §66): the only thing on the glass in a setup
 * boot. A QR code that joins the orb's own hotspot, its name in words, and one hint.
 * Hold the knob to swap the code for the setup page's address (for phones that do not
 * read Wi-Fi codes). After two minutes untouched the glass goes black; any turn or
 * press brings it back. Nothing of the dial, no theme and no app is involved. */
#pragma once

#include "esp_err.h"

/** Build the screen and load it, and start the knob poll. Call once, from app_main's
 *  setup branch, after display_init() and knob_init() and before the LVGL loop runs. */
esp_err_t setup_view_start(void);
