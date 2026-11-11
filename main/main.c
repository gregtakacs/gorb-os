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
#include "orb_rotation.h"
#include "esp_timer.h"
#include "orb_boot_overlay.h"
#include "orb_app.h"
#include "orb_app_scan.h"   /* the /sd/apps drop zone registers beside the card */
#include "orb_app_install.h"
#include "orb_app_net.h"     /* orb_app_cred_sweep: the boot's secret sweep */
#include "orb_jobs.h"
#include "orb_home.h"
#include "orb_places.h"
#include "orb_units.h"
#include "orb_tls_mem.h"
#include "orb_svc.h"
#include "orb_carousel.h"
#include "knob.h"
#include "orb_strip.h"
#include "input_router.h"
#include "imu.h"
#include "orb_sound.h"     /* D39: the sound engine, after the I2C bus */
#include "idle_dim.h"
#include "orb_net.h"
#include "orb_sd.h"
#include "orb_usb_mtp.h"   /* the file-share mode's one-shot claim; stub false when off */   /* D37: the MTP mode's, on the same boot-ladder terms  */
#include "orb_usb_msc.h"   /* D38: the disk mode's claim, same seam, opposite mount rule  */
#include "orb_log.h"       /* the RAM log ring: /api/v1/log's only source */
#include "orb_panic.h"     /* the last panic, kept across its reset for /api/v1/status */
#include "orb_layout_carry.h" /* storage across the Layout B -> C table change */
#include "orb_theme.h"
#include "art_bank.h"
#include "orb_edge_arc.h"
#include "orb_ring.h"     /* the OS ring's service + boot init, beside the arc's (ring-design §8) */

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
        orb_edge_arc_service();        /* the system arc's rows ride this pass's flush */
        orb_ring_disp_service();       /* the OS ring's rows too; the arc wins while a copy runs */
        const int64_t t_h = esp_timer_get_time();
        uint32_t sleep_ms = lv_timer_handler();
        const int64_t t_s = esp_timer_get_time();
        display_lvgl_pass_mark();      /* a store waits on this before it unmaps */
        display_dma_service();         /* a still page re-sends what a card read cost */
        if (sleep_ms > 50) sleep_ms = 50;
        if (sleep_ms < 5) sleep_ms = 5;
        vTaskDelay(pdMS_TO_TICKS(sleep_ms));
        display_bench_loop_add((uint32_t)(t_s - t_h), sleep_ms,
                               (uint32_t)(esp_timer_get_time() - t_s));
    }
}

void app_main(void)
{
    /* First, above the line that says booting started. The ring exists to answer a
     * question asked over HTTP minutes later, and a hook installed after the boot
     * ladder has run has already lost the only part of the record that explains a
     * failure — which is most of what this replaces about reading a serial cable.
     *
     * Chained, not replacing: esp_log_set_vprintf returns the previous sink and this
     * calls it, and the previous sink is plain vprintf, which follows stdout
     * whenever the console VFS gets installed below. So the console keeps every
     * byte it printed before this file existed. */
    orb_log_init();
    /* Before anything can panic again: count this boot so a panic record the RTC
     * kept can say how many boots ago it happened. */
    orb_panic_boot();
    /* mbedTLS's allocator before anything uses it: the radio's supplicant
     * allocates through it from esp_wifi_start on (net/orb_tls_mem.c). */
    orb_tls_mem_init();

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

    /* Before anything opens `storage`: a board that crossed from Layout B still keeps
     * its credentials at the old offset, and nothing else on this boot could find them. */
    orb_layout_carry_storage();

    /* A serial RESET ALL acked last boot lands its erase HERE, at the top of the
     * heap's rich moment — before the theme, Wi-Fi, or the display have anything
     * to say about the internal pool. A no-op unless the RTC intent is set. */
    orb_net_apply_pending_factory_reset();

    /* Shield the web server's stack before the theme can fragment it. One board
     * has been seen headless with largest-free-internal at 3,456 B (status line,
     * 2026-10-26), under the ladder's smallest tier; that tier of failure is
     * what this prevents. The serial capture the same night proved the other
     * headless cause — a refused task create with 13 KB largest — immune to any
     * shield; the retry timer answers that one. Costs 8 KB of internal for a
     * few seconds of boot; net_start_httpd hands it back before the ladder runs. */
    orb_net_reserve_httpd_stack();

    /* C2 (§6.4): the panel comes up before the theme resolves, so every slow
     * moment below is spent in front of the overlay, not a dark dial. The
     * install-before-display rule this replaced was about erasing while LVGL
     * paints; the overlay is not LVGL and paints no dial, so the ordering
     * reason disappears while the safety it protected is kept verbatim: the
     * flush path is not armed until display_init(), and the LVGL task that
     * could paint is created after it. */
    ESP_ERROR_CHECK(display_panel_early());
    orb_boot_overlay("starting", 0, 0, 0);

    /* Resolve the theme: orb_theme_boot() maps the one slot's bundle through
     * art_bank_map() — mechanism A at the call since C3, no arena copy and no
     * refill pass — and reports whether the slot's header is a drawable bundle
     * or a tear the staged witness will name. There is no choice to make and no
     * install to carry out here: Migration 1 made the store a worker job that
     * reboots, and a boot draws the slot or the base face.
     * Deliberately not ESP_ERROR_CHECK — the clock still has to tell the time. */
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
        char line[32];
        /* §32.3: the glass carries the sentence plus six hex digits. The full
         * 32-char id is wider than the panel at any legible size — the old line
         * clipped mid-token into 48 bytes and the centered band ran off both
         * edges, which is the "theme load failed xxxxx" fragment cut off on the
         * glass on 2026-10-23. The whole id rides the serial line here and the
         * hub's card, where it is readable and copyable. */
        snprintf(line, sizeof line, "theme load failed %.6s", orb_theme_staged_id());
        ESP_LOGE(TAG, "theme load failed: the staged store for %s never landed",
                 orb_theme_staged_id());
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
    /* The dial owns the glass from here, so the overlay gives back its band of
     * internal DMA RAM, held until now only for frames nobody sees again this
     * boot. Held for the life of the boot (at 22 KB, before it was halved) it
     * starved esp_wifi_init on orb-B: 8 of 10 static rx buffers with a 3.5 MB
     * theme resident. A store that needs the overlay later takes it back. */
    orb_boot_overlay_release();

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
    /* D36 (§0.11): the image ships no apps. A board whose ledger comes back empty
     * stays empty — the dial boots to the shell menu and waits for its owner.
     * The clock and the HSB editor are ordinary card apps the hub seeds onto the
     * card and installs through the ordinary doors, the same way every other
     * app arrives, so there is nothing left to provision here. */
    /* The broker's grant words load before the first app builds, so the first
     * gated call of the boot never pays an NVS read on the frame path — and
     * so /api/v1/status answers with real grant/call numbers from second one
     * (plan §8, the P3 pass line). Inert until an app calls through it; the
     * compiled-in pair reaches the OS directly until P6, as §14 records. */
    ESP_ERROR_CHECK(orb_svc_init());
    /* §43.3: a secret lives only as long as an installed app declares it; the card
     * table is complete here, and NVS is up. */
    orb_app_cred_sweep();
    /* The home location (flight-tracker-design §6.4) loads beside the grant
     * words and for the same reason: the apps that read it build next. One
     * NVS blob read; the zone it set is applied by orb_net_start's tz load. */
    orb_home_init();
    /* The three places beside home (radar-plan §2.1), for the same reason. */
    orb_places_init();
    /* The units (weather-plan §2.1) load beside it for the same reason. */
    orb_units_init();
    orb_rotation_init();     /* §59: the carousel's order, read before the registry walks */
    /* The OS ring's ownership table, cleared before the first app builds: an app can set
     * the ring from its own init, and a stale owner row must never show at boot (ring-design §5). */
    orb_ring_disp_init();
    /* P7.4's rung: the code tier loads after the broker's words are known (a
     * quarantined app must not cost its D27 memory) and before init_all asks each
     * module to build — the wasm module's init answers OK only once its slot
     * holds a runtime, so this rung is what makes an installed code app walkable. */
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
    orb_strip_init();          /* plan §54: the carousel's gear now, its previews after boot */
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
    /* Pinned to the LCD bus's ISR core: see DISPLAY_LCD_CORE (display.h). */
    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 8192, NULL, 5, NULL, DISPLAY_LCD_CORE);

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

/* First act of the storage ladder and deliberately ahead of it: consume the "next
 * boot shares its files over USB" one-shot before anything else can act on it. The
 * stub answers false when the option is out of the build, so this line is here
 * whether or not the feature is, and the boot below can only be a file-share boot
 * on the strength of a flag it has already erased.
 *
 * The claim says NOTHING about the mount: MTP answers the host with paths inside
 * the filesystem, so an armed boot mounts /sd exactly as an ordinary boot does. */
#if CONFIG_ORB_USB_MSC
    /* D38: the disk mode's claim is load-bearing for the mount in a way the MTP
     * one is not — it sets the flag that makes mount_now() refuse, so the boot
     * that hands its card away never mounts the volume it is about to export. */
    const bool files_boot = orb_usb_msc_boot_claim();
#elif CONFIG_ORB_USB_MTP
    const bool files_boot = orb_usb_mtp_boot_claim();
#else
    const bool files_boot = false;
    (void)files_boot;   /* no mode compiled, so nothing consumes the claim below */
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
     * theme source is declared by orb_stage.c next to the scans it serves —
     * this one is declared here because no compiled app owns the drop zone:
     * the OS does (an app that could declare where card apps live would move
     * everyone's front door). A refusal is loud and permanent: the apps page
     * answers the -1, and the log line here says why. */
    if (orb_app_scan_init() != ESP_OK)
        ESP_LOGE(TAG, "the /sd/apps drop zone refused registration — "
                      "the apps page will list nothing");
#endif

#if CONFIG_ORB_USB_MSC
    /* Same rule as the MTP mode: posted, after jobs and the display are up,
     * because a raw card init and a TinyUSB task creation are the two slowest
     * things this does and a disk boot has no console — the row's state line is
     * the only account of what happened. */
    if (files_boot && orb_usb_msc_request_start() != ESP_OK)
        ESP_LOGE(TAG, "the worker refused the disk-mode start — the card stays the "
                      "dial's until the next boot");
#elif CONFIG_ORB_USB_MTP
    /* The claimed boot becomes the device here and not earlier: orb_jobs_start()
     * has run, so the request has somewhere to be posted to, and the whole display
     * ladder is up — which matters because a device boot has no console, and the
     * only account of what happened is the USB Files row on the dial. Posted rather
     * than called: a card mount, an MTP object-store scan and a TinyUSB task
     * creation are the slowest things this mode does, and app_main is not where they
     * belong. */
    if (files_boot && orb_usb_mtp_request_start() != ESP_OK)
        ESP_LOGE(TAG, "the worker refused the file-share start — the card stays the "
                      "dial's until the next boot");
#endif

    /* The sound engine (D39), after everything that allocates internal RAM. Its
     * I2S DMA ring and driver state are a few KB of the pool this board runs out of,
     * and taken earlier they starved esp_wifi_init (ESP_ERR_NO_MEM, measured on the
     * first flash). The codec shares the bus imu_init() created. Never fatal — a
     * board without the codec runs silent. */
    ESP_ERROR_CHECK(orb_sound_init());

    /* C2, last act of the ladder: the boot survived to here — the dial is
     * built, the router polls, the network and card tried — so the reset
     * record moves from ARM to RUN and starts following the glass. A reset
     * seen by the NEXT boot's orb_svc_init bills the app it names only if the
     * chip blames the code for it: the bench's RTS pulses and the owner's EN
     * button leave a live record too, and pay nobody (orb_reset_blames_code). */
    orb_svc_boot_done();
}

