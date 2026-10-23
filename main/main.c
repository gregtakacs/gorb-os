/**
 * gorb-os — main application entry.
 *
 * MVP: Initialize CO5300 AMOLED + LVGL 9, display a centered digital clock.
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "lvgl.h"

#include "board_config.h"
#include "display.h"
#include "orb_boot_overlay.h"
#include "orb_app.h"
#include "orb_app_scan.h"   /* the /sd/apps drop zone registers beside the card */
#include "orb_app_install.h" /* orb_app_install_resolve_boot: the card table from the apps partition */
#include "orb_jobs.h"
#include "orb_svc.h"
#include "orb_carousel.h"
#include "knob.h"
#include "input_router.h"
#include "imu.h"
#include "idle_dim.h"
#include "orb_net.h"
#include "orb_sd.h"
#include "orb_theme.h"
#include "art_bank.h"

static const char *TAG = "main";

/* LVGL handler task — calls lv_timer_handler() periodically */
static void lvgl_task(void *arg)
{
    while (1) {
        /* Brightness writes the idle-dim task decided are issued here, on the
         * same task as the frame flush, before this iteration touches the panel.
         * The last flush fully retired when the previous lv_timer_handler()
         * returned, so no transaction is in flight to split -- the placement the
         * idle task cannot guarantee at its own priority. No-op when it posted
         * nothing; its decision latency does not depend on this running. */
        idle_dim_render_drain();
        uint32_t sleep_ms = lv_timer_handler();
        if (sleep_ms > 50) sleep_ms = 50;
        if (sleep_ms < 5) sleep_ms = 5;
        vTaskDelay(pdMS_TO_TICKS(sleep_ms));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== gorb-os booting ===");

    /* Initialize NVS (WiFi driver's own store: namespace "nvs.net80211").
     *
     * The recovery erase below is safe for user data because application
     * settings and WiFi credentials do NOT live in this partition — they are in
     * the separate `storage` partition (partitions.csv) owned by main/net. At
     * worst a wipe here costs the driver's cached profile and PHY calibration,
     * both of which are re-derived; orb_net_start() re-pushes the stored SSID
     * and passphrase on every boot, so credentials survive it. */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "corrupt/unformatted NVS — erasing (WiFi creds are safe in `storage`)");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    /* C2 (§6.4): the panel comes up before the theme resolves, so every slow
     * moment below is spent in front of the overlay, not a dark dial. The
     * install-before-display rule this replaced was about erasing while LVGL
     * paints; the overlay is not LVGL and paints no dial, so the ordering
     * reason disappears while the safety it protected is kept verbatim: the
     * flush path is not armed until display_init(), and the LVGL task that
     * could paint is created after it. */
    ESP_ERROR_CHECK(display_panel_early());
    orb_boot_overlay("starting", 0, 0, 0);

    /* Resolve the theme — and carry out any install the config page staged.
     * art_bank_init() must map the slot the CHOSEN theme lives in, and only
     * this call knows which of the two that is; a staged-but-never-committed
     * load is a torn copy, and the copy belongs before any pixels can read it.
     * Deliberately not ESP_ERROR_CHECK — a failed load falls back to the theme
     * that was already installed (or to the baked-in one), and the clock still
     * has to tell the time. */
    orb_boot_overlay("mapping", 0, 0, 0);
    const esp_err_t theme_err = orb_theme_boot();
    if (theme_err != ESP_OK)
        ESP_LOGE(TAG, "theme bring-up degraded (%s) — clock runs on the fallback bank",
                 esp_err_to_name(theme_err));

    /* §6.4's torn-store case, on the glass instead of only on a serial line
     * nobody reads on a device hung on a wall: the staged witness survived a
     * reboot and names an id the slot does not hold. Hold the sentence on the
     * overlay for 10 s — nothing else can paint during it — then the base
     * face. The 10 s figure is §6.4's, and it costs a boot nobody likes less
     * than a dial that fails silently. */
    if (orb_theme_staged_id()[0]) {
        char line[48];
        snprintf(line, sizeof line, "theme load failed %s", orb_theme_staged_id());
        orb_boot_overlay(line, 0, 0, 0);
        vTaskDelay(pdMS_TO_TICKS(10000));
    }

#if ORB_ART_M2_PROBE
    /* M2 residency protocol (docs/app-platform-plan.md §6): runs after the
     * theme's art_bank_init() so the primary counts, and before display_init()
     * so the dial's own fps lines, later in the same boot log, are measured
     * with every probed bank still mapped. */
    art_bank_m2_probe();
#endif

#if ORB_ART_LAT_PROBE
    /* Reclaim-latency protocol (docs/app-platform-plan.md §6): prices the
     * map_extra/unmap_extra pair a suspend/release and a resume/reclaim run, so
     * the release-vs-carry decision has a measured µs number. Same placement as
     * M2 — after the theme's art_bank_init() so the primary counts, before
     * display_init() so the dial's own fps lines later in the log are made with
     * the MMU table back at its shipping state (every lat bank is unmapped). */
    art_bank_lat_probe();
#endif

#if ORB_ART_M3_PROBE
    /* A-vs-B overflow protocol (docs/app-platform-plan.md §6 M3): prices the
     * one bank the slot layout has no room for, re-map against PSRAM arena.
     * Same placement rule as M2 — after the theme's art_bank_init() so the
     * primary counts, before display_init() so the dial's own fps lines later
     * in this boot log are made with the bench bank still resident (the probe
     * leaves it mapped/armes the DISPLAY_BENCH plane; see the m3: done line).
     * Requires the bench bank flashed at assets+0x100000 — the probe refuses
     * to measure erased flash rather than printing a confident lie. */
    art_bank_m3_probe();
#endif

    /* Arm the LVGL flush on the panel that came up before the theme (§6.2).
     * The overlay's "ready" frame is the last thing the glass shows before the
     * first real commit paints over it. */
    orb_boot_overlay("ready", 0, 0, 0);
    ESP_ERROR_CHECK(display_init());

    /* Build the apps and their screens. Each registers its own LVGL screen
     * object; the input router decides which one is loaded. The GIF background
     * animates from inside the clock view (delta-rect bank), so there is no
     * separate orb/bench screen any more. */
    /* Build the apps, in registry table order. main.c no longer names any of
     * them: the table in main/app/orb_app.c decides what boots, in what order,
     * and an app whose init fails is dropped from the carousel with its reason
     * logged instead of taking the dial down with it.
     *
     * The order the table encodes is the order these three were called in
     * before, and it is not arbitrary: the dial must exist before the router can
     * home to it, and the menu's pages are built after the display exists.
     * Blocking work belongs to nobody's private task — the shell's one orb_jobs
     * worker, started below, runs every app's card and NVS work, which is what
     * keeps a second 4 KB stack out of the internal heap the framebuffer also
     * spends. */
    ESP_ERROR_CHECK(orb_jobs_start());
    /* Rebuild the registry's card table from the apps partition BEFORE the broker
     * loads its grant words, so orb_svc_init preloads a row for every installed
     * card app and masks the owner's stored word against the caps that app's slot
     * header declares (D19 — the header is the sole grant source, and the broker's
     * own load runs once, here, over the finished table). It reads only flash, so
     * it stands with or without a card in the socket. */
    orb_app_install_resolve_boot();
    /* D35 (§35): a board with an empty apps partition is a clean slate, and the
     * factory pair — the clock and the HSB editor, shipped in the image as real
     * containers — installs through the ordinary ladder before the broker reads
     * any ledger, so its grant and enable words exist for this same boot. */
    orb_app_install_provision_clean();
    /* The broker's grant words load before the first app builds, so the first
     * gated call of the boot never pays an NVS read on the frame path — and
     * so /api/v1/status answers with real grant/call numbers from second one
     * (plan §8, the P3 pass line). Inert until an app calls through it; the
     * compiled-in pair reaches the OS directly until P6, as §14 records. */
    ESP_ERROR_CHECK(orb_svc_init());
    /* P4a's rung: the owner's enable words apply after the broker has the
     * partition up and before anything builds a screen — an installed app
     * walks this boot only if its stored word says so, and one with no word
     * stays exactly as installed: off. */
    orb_app_install_apply_enable();
    /* P7.4's rung: the code tier loads after the enable words are known (a
     * disabled app must not cost its D27 memory) and before init_all asks each
     * module to build — the wasm module's init answers OK only once its slot
     * holds a runtime, so this rung is what makes an enabled code app walkable. */
    orb_app_install_apply_code();
    ESP_ERROR_CHECK(orb_app_init_all());

    /* The carousel's name chip, on the top layer, built once here so no flick
     * ever allocates. Deliberately NOT an ESP_ERROR_CHECK: the chip is chrome,
     * and a dial that cannot paint a destination falls back to switching
     * immediately on each turn (orb_carousel_ready()) rather than refusing to
     * boot into a sealed case. */
    const esp_err_t carousel_err = orb_carousel_init();
    if (carousel_err != ESP_OK)
        ESP_LOGE("carousel", "chip refused (%s) — the knob walks without a peek",
                 esp_err_to_name(carousel_err));

    /* Rotary knob: GPIO edge ISRs feed the detent + press streams. */
    ESP_ERROR_CHECK(knob_init());

    /* Route knob events to the active screen (color picker first) and start the
     * ~10 ms input poll, which runs on the LVGL task created below. */
    ESP_ERROR_CHECK(input_router_init());

    /* Idle dim, last of the bring-up: it needs the panel (to write brightness)
     * and the IMU (to poll it in the dark), and neither is worth a dependency
     * elsewhere. The IMU is brought up only when motion wake is enabled, so
     * ORB_IDLE_MOTION_ENABLE=n drops the driver out of the image entirely. */
#if CONFIG_ORB_IDLE_MOTION_ENABLE
    ESP_ERROR_CHECK(imu_init());
#endif
    ESP_ERROR_CHECK(idle_dim_init());

    /* Launch LVGL handler task */
    xTaskCreate(lvgl_task, "lvgl", 8192, NULL, 5, NULL);

    ESP_LOGI(TAG, "gorb-os running — knob: turn swaps apps, push opens, hold returns to the menu");

#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    /* Rollback handshake. With the safety net compiled in, the bootloader boots a
     * freshly OTA'd image in PENDING_VERIFY and REVERTS it at the next reboot
     * unless the app confirms itself workable. "Workable" here means the dial
     * actually drew, so wait for a couple of real flushes from the LVGL task
     * instead of confirming the instant we reach this line: an image that comes
     * up, wedges the display, and leaves a frozen clock is exactly the case the
     * net exists for, and a blind delay would wave it through. */
    for (int waited_ms = 0; waited_ms < 6000; waited_ms += 100) {
        uint32_t frames = 0;
        uint64_t us_total = 0;
        uint32_t us_max = 0;
        display_bench_snapshot(&frames, &us_total, &us_max);
        if (frames >= 2) break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    const esp_err_t rb = esp_ota_mark_app_valid_cancel_rollback();
    if (rb != ESP_OK) {
        /* Not fatal: the clock is running. But this image stays unconfirmed, so a
         * reboot sends it back to the previous slot — say so loudly. */
        ESP_LOGE(TAG, "rollback not cancelled (%s) — next reboot may revert to previous image",
                 esp_err_to_name(rb));
    }
#endif

#if CONFIG_ORB_NET_ENABLE
    /* Start the network AFTER the LVGL task exists, so the dial is already
     * ticking while the radio comes up and a bring-up failure can never show up
     * as a blank screen. Deliberately not ESP_ERROR_CHECK: the clock is the
     * product, and this is a device in a sealed case. */
    if (orb_net_start() != ESP_OK)
        ESP_LOGE(TAG, "networking down — clock running without config page");
#endif

#if CONFIG_ORB_SD_ENABLE
    /* Same rule as the network, and for the same reason: the TF socket has no
     * card-detect line, so finding out whether a card is in it means driving
     * the SPI bus through an init ladder that can take a second per failed
     * rung. That belongs after the dial is drawing, never before it. */
    if (orb_sd_init() != ESP_OK)
        ESP_LOGW(TAG, "no SD card — themes can still be installed from the "
                      "card once one is inserted (POST /api/v1/sd/rescan)");
    /* The /sd/apps drop zone, registered beside the card itself (P4). A table
     * write with no I/O, so it stands or fails identically with or without a
     * card in the socket; the walk asks the volume to mount when it runs. The
     * theme source is declared by clock_view.c next to the scans it serves —
     * this one is declared here because no compiled app owns the drop zone:
     * the OS does (an app that could declare where card apps live would move
     * everyone's front door). A refusal is loud and permanent: the apps page
     * answers the -1, and the log line here says why. */
    if (orb_app_scan_init() != ESP_OK)
        ESP_LOGE(TAG, "the /sd/apps drop zone refused registration — "
                      "the apps page will list nothing");
#endif

    /* C2, last act of the ladder: the boot survived to here — the dial is
     * built, the router polls, the network and card tried — so the reset
     * record moves from ARM to RUN and starts following the glass. A reset
     * seen by the NEXT boot's orb_svc_init bills the app it names only if the
     * chip blames the code for it: the bench's RTS pulses and the owner's EN
     * button leave a live record too, and pay nobody (orb_reset_blames_code). */
    orb_svc_boot_done();
}

