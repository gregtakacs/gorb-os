/**
 * orb_jobs.h — one worker task, owned by the shell, used by every app.
 *
 * The rule this exists to keep: nothing that can block runs on the LVGL task.
 * input_router's 10 ms poll is the only reader of the knob and the only thing
 * that repaints the dial, so a card walk that runs there freezes the screen and
 * swallows the knob for as long as the card takes — measured at 2952 ms for a
 * 3.7 MB container and 38-48 s behind a theme upload.
 *
 * The menu already carried a task of its own for exactly this. It could not
 * stay there: an app's blocking work belongs to the app to *request*, and the
 * shell's to *schedule*. So an app posts a function and its payload, and the
 * shell keeps the one task, the queue, the busy gate, the stack high-water log
 * and the sizing decision that came from hardware. Two apps asking for a task
 * each would cost two stacks out of the same internal heap that runs the
 * framebuffer, and the second is the one that fails to allocate.
 *
 * Threading.
 *   - post() is callable from any task and never blocks. It copies arglen bytes
 *     of arg into the queue, so the caller's stack object may die on return.
 *   - fn runs on the worker, NOT on the LVGL task. It may touch the card, NVS
 *     and the network. It may not touch a widget, and it may not call
 *     lv_*(): that is the other half of the rule this file exists for.
 *   - Results cross back through the app's own state and the app's own mutex,
 *     which it reaps from its service() hook on the LVGL task. Nothing here
 *     repaints anything, and an app that marks a widget dirty from its job has
 *     put the repaint on the wrong task — orb_app.h's note on service() is the
 *     same rule from the other side.
 *   - One job outstanding at a time. A double-tap on a row must not queue two
 *     multi-second card walks, and the first result repaints the list anyway.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

/** A unit of blocking work. Runs on the worker with a private copy of arg. */
typedef void (*orb_job_fn)(void *arg);

/** Queue one job. ESP_ERR_INVALID_STATE when one is already outstanding, and
 *  ESP_ERR_NO_MEM when the queue itself refused — both reported, neither
 *  silently dropped, because a menu that never hears back sits at "working..."
 *  forever and looks like a dead device. */
esp_err_t orb_jobs_post(orb_job_fn fn, void *arg, size_t arglen);

/** True while a job is queued or running, for the UI that has to say so. */
bool orb_jobs_busy(void);

/** Create the worker. Call once, after display_init: a job can reach the card,
 *  and the card's mount ladder must not run before its driver is up. */
esp_err_t orb_jobs_start(void);

/** Arm the OS's reboot in 750 ms, with the reason in the log.
 *
 *  This is the only way anything below main/ gets to restart the device. An app
 *  that called esp_restart() would own the power state and would do it from
 *  whichever task noticed, which for a theme install means from inside the
 *  repaint — and then the last frame never reaches the panel and the owner's
 *  final sight is the screen before the one that said it worked. Same 750 ms
 *  one-shot h_reboot() and the theme install job already use, in the one
 *  place that now owns it.
 *
 *  Safe to call repeatedly: the timer is one-shot and re-arming just moves the
 *  moment. Returns the reason it could not be armed, so the caller can say so
 *  on the row instead of leaving "rebooting..." on a dial that keeps running. */
esp_err_t orb_jobs_reboot_soon(const char *why);
