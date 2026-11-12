/**
 * orb_press.c — the pure lent-press machine (plan §64.13, alerts-design §3.2).
 *
 * State is the whole thing: one claimant slot, one queue count per slot (saturating at
 * ORB_PRESS_MAX_LENT) and a swallowed counter. All of it PSRAM — the internal-RAM cliff
 * (alerts-design §0) forbids a new internal .bss byte — and the host suite compiles this
 * file verbatim with the attribute folded to nothing. The claimant is a flash slot, the
 * same key the ring table uses, so the broker's revoke, an engine unload and a trap all
 * reclaim the claim through orb_press_drop with no knowledge of what app held it.
 *
 * No lock. Every entry runs on the LVGL task: the router queues on the poll (input_router's
 * poll_timer_cb), the wasm shim answers orb_press_lent()/orb_press_claim() on a guest call
 * which is also the LVGL task, and the lifecycle drops run on the LVGL task or the worker
 * the uninstall already serialises. A torn read of one 32-bit word would at worst lend a
 * press a beat late — there is no multi-word invariant for a lock to protect that the two
 * callers could each half-update, so the module adds no internal mutex (§3.4's "reuse
 * existing locking" rule). The one monotonic word, the swallowed counter, is written and
 * read only on the LVGL task.
 */
#include "orb_press.h"

#ifdef ESP_PLATFORM
#include "esp_attr.h"        /* EXT_RAM_BSS_ATTR */
#include "esp_log.h"         /* the swallowed-press line */
#include "orb_app.h"         /* ORB_APP_CAP: the queue is one count per flash slot */
#endif

#ifndef EXT_RAM_BSS_ATTR
#define EXT_RAM_BSS_ATTR
#endif

#ifndef ORB_APP_CAP
#define ORB_APP_CAP 8        /* the install ceiling; the host suite's own cap if unset */
#endif

#define PRESS_TAG "orb_press"

/* One claimant slot word (the ring table's owner is its twin), one saturated queue count
 * per slot, and the swallowed counter. PSRAM, zero-initialised: owner == 0 is a real slot,
 * so the reset the boot ladder performs is what turns the all-zero image into "nobody owns
 * it yet" (owner = -1). Every entry guards on the bounds regardless. */
EXT_RAM_BSS_ATTR static int s_owner;                    /* claiming flash slot, or -1      */
EXT_RAM_BSS_ATTR static int s_count[ORB_APP_CAP];       /* queued presses, saturating at 4 */
EXT_RAM_BSS_ATTR static int s_swallowed;                /* presses refused at the ceiling  */
EXT_RAM_BSS_ATTR static int s_ready;                     /* s_owner seeded to -1 this boot  */

static int valid_slot(int slot)
{
    return slot >= 0 && slot < ORB_APP_CAP;
}

/* A cold PSRAM image reads owner 0, which would name slot 0 a claimant before anyone
 * asked. First touch seeds it to "none", so a boot that never runs orb_press_init (the
 * host suite drives scenarios by calling it explicitly; on the device the boot ladder does)
 * still behaves as an empty machine. */
static void press_ready(void)
{
    if (!s_ready) {
        s_owner = -1;
        for (int i = 0; i < ORB_APP_CAP; i++) s_count[i] = 0;
        s_swallowed = 0;
        s_ready = 1;
    }
}

void orb_press_init(void)
{
    s_ready = 0;
    press_ready();
}

int orb_press_owner(void)
{
    press_ready();
    return s_owner;
}

int orb_press_claim(int slot, int on)
{
    press_ready();
    if (!valid_slot(slot)) return 0;               /* a slot the table cannot name */
    if (on) {
        /* Latest caller wins. A fresh claim always starts its own queue empty, and a
         * steal throws away whoever held it before so a claimant that is not the one
         * on the glass cannot inherit presses queued for the app it just displaced.
         * A re-claim by the current owner keeps its queue: the pomodoro asserts the
         * claim every service beat and must not throw away presses it has not read. */
        if (s_owner != slot) {
            for (int i = 0; i < ORB_APP_CAP; i++) s_count[i] = 0;
            s_owner = slot;
        }
        /* A re-claim by the current owner leaves its queue standing: the claimant
         * asserts the claim every service beat and must not throw away presses it
         * has not read yet. */
        return 1;
    }
    /* Release: only the claimant may put the press back. A non-owner's claim(0) is
     * ignored, so an app cannot drop a claim it never held. */
    if (s_owner == slot) {
        s_count[slot] = 0;
        s_owner = -1;
    }
    return s_owner == slot ? 1 : 0;
}

int orb_press_lend(int slot)
{
    press_ready();
    if (s_owner < 0) return 0;                     /* nobody claimed the press: nothing to lend */
    if (slot != s_owner) return 0;                 /* a delivery aimed at a non-owner is ignored */
    if (s_count[slot] >= ORB_PRESS_MAX_LENT) {     /* the fifth in a row is swallowed, not spilled */
        s_swallowed++;
#ifdef ESP_PLATFORM
        ESP_LOGW(PRESS_TAG, "lent press to slot %d swallowed: queue full (%d)",
                 slot, ORB_PRESS_MAX_LENT);
#endif
        return 0;
    }
    s_count[slot]++;
    return 1;
}

int orb_press_take(int slot)
{
    press_ready();
    if (slot < 0 || slot >= ORB_APP_CAP) return 0;
    if (slot != s_owner) return 0;                 /* only the claimant drains its queue */
    if (s_count[slot] <= 0) return 0;
    s_count[slot]--;
    return 1;
}

int orb_press_lent(int slot)
{
    press_ready();
    if (slot < 0 || slot >= ORB_APP_CAP) return 0;
    if (slot != s_owner) return 0;
    return s_count[slot] > 0 ? s_count[slot] : 0;
}

int orb_press_swallowed(void)
{
    press_ready();
    return s_swallowed;
}

void orb_press_drop(int slot)
{
    press_ready();
    if (!valid_slot(slot)) return;
    if (s_owner == slot) {
        s_owner = -1;
        s_count[slot] = 0;
    } else {
        s_count[slot] = 0;                         /* a dead app's queue dies even if it was not
                                                    * the claimant: a stale row is never handed on */
    }
}
