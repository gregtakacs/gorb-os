/* orb_panic.c — the last panic, kept across the reset it causes. See orb_panic.h.
 *
 * The hook is a link-time wrap of esp_panic_handler (main/CMakeLists.txt), which
 * every panic path funnels through: faults, abort(), and both watchdogs. It can
 * live in flash: both IDF entry points (panicHandler, xt_unhandled_exception in
 * esp_system/port/panic_handler.c) re-enable the flash cache before they call it,
 * so a panic inside a flash write still reaches this code. It stays out of IRAM on
 * purpose — the boot's internal-RAM margin at esp_wifi_start() is ~2 KB, and the
 * first cut's ~200 B of IRAM hook tipped it into a crash loop (2026-10-31).
 */
#include "orb_panic.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_attr.h"   /* RTC_NOINIT_ATTR */
#include "esp_cpu_utils.h"
#include "esp_debug_helpers.h"
#include "esp_private/panic_internal.h"
#include "xtensa_context.h"

#define PANIC_MAGIC 0x4f524250u   /* "ORBP" */
#define BOOT_MAGIC  0x4f524242u   /* "ORBB" */
#define PANIC_BT    12            /* frames kept; ~11 B each in the status reply */

typedef struct {
    uint32_t magic;
    uint32_t boot;                /* s_boot.count at the panic */
    uint32_t kind;                /* panic_exception_t */
    uint32_t core;
    uint32_t pc;
    uint32_t cause;               /* EXCCAUSE */
    uint32_t vaddr;               /* EXCVADDR */
    char     reason[32];          /* IDF's own words: "abort() was called", "StoreProhibited" */
    uint32_t n_bt;
    uint32_t bt[PANIC_BT];
    uint32_t sum;
} panic_rec_t;

typedef struct {
    uint32_t magic;
    uint32_t count;
} boot_ctr_t;

/* RTC slow memory, not initialised at boot: a software reset (the one every
 * panic ends in) keeps it, a power cycle fills it with noise the magic and the
 * checksum turn away. */
static RTC_NOINIT_ATTR panic_rec_t s_rec;
static RTC_NOINIT_ATTR boot_ctr_t  s_boot;
static bool s_rec_ok;             /* the record validated at this boot */

static uint32_t rec_sum(const panic_rec_t *r)
{
    const uint32_t *w = (const uint32_t *)r;
    uint32_t s = 0x9e3779b9u;
    for (size_t i = 0; i < offsetof(panic_rec_t, sum) / sizeof(uint32_t); i++)
        s = (s ^ w[i]) * 0x01000193u;
    return s;
}

void __real_esp_panic_handler(panic_info_t *info);

void __wrap_esp_panic_handler(panic_info_t *info)
{
    panic_rec_t *r = &s_rec;
    r->magic = PANIC_MAGIC;
    r->boot  = (s_boot.magic == BOOT_MAGIC) ? s_boot.count : 0;
    r->kind  = (uint32_t)info->exception;
    r->core  = (uint32_t)info->core;
    r->pc    = (uint32_t)info->addr;
    r->cause = 0;
    r->vaddr = 0;
    r->n_bt  = 0;
    /* Safe to read now: the cache is back on before this runs (see the header), and
     * IDF's reasons are short static strings. Bounded copy, always terminated. */
    size_t i = 0;
    if (info->reason)
        for (; i < sizeof r->reason - 1 && info->reason[i]; i++) r->reason[i] = info->reason[i];
    r->reason[i] = '\0';
    const XtExcFrame *xt = (const XtExcFrame *)info->frame;
    if (xt) {
        r->cause = (uint32_t)xt->exccause;
        r->vaddr = (uint32_t)xt->excvaddr;
        /* The same walk panic_print_backtrace() prints, stopped at a frame the
         * stack checks reject rather than followed into garbage. */
        esp_backtrace_frame_t f = { .pc = xt->pc, .sp = xt->a1, .next_pc = xt->a0,
                                    .exc_frame = (void *)xt };
        r->bt[r->n_bt++] = esp_cpu_process_stack_pc(f.pc);
        while (r->n_bt < PANIC_BT && f.next_pc != 0 && esp_backtrace_get_next_frame(&f))
            r->bt[r->n_bt++] = esp_cpu_process_stack_pc(f.pc);
    }
    r->sum = rec_sum(r);
    __real_esp_panic_handler(info);
}

void orb_panic_boot(void)
{
    if (s_boot.magic != BOOT_MAGIC) {
        s_boot.magic = BOOT_MAGIC;
        s_boot.count = 0;
    }
    s_boot.count++;
    s_rec_ok = s_rec.magic == PANIC_MAGIC && s_rec.n_bt <= PANIC_BT &&
               s_rec.sum == rec_sum(&s_rec) && s_rec.boot < s_boot.count;
}

static const char *kind_name(uint32_t k)
{
    switch (k) {
    case PANIC_EXCEPTION_DEBUG: return "debug";
    case PANIC_EXCEPTION_IWDT:  return "int-wdt";
    case PANIC_EXCEPTION_TWDT:  return "task-wdt";
    case PANIC_EXCEPTION_ABORT: return "abort";
    case PANIC_EXCEPTION_FAULT: return "fault";
    }
    return "?";
}

int orb_panic_json(char *out, size_t cap)
{
    if (!cap) return -1;
    out[0] = '\0';
    if (!s_rec_ok) return 0;
    /* The reason as JSON-safe text: IDF's strings carry no quotes, but a record the
     * checksum passed is still bytes from a crash, so anything outside plain
     * printable ASCII (and the two JSON specials) is dropped rather than trusted. */
    char reason[sizeof s_rec.reason];
    size_t k = 0;
    for (size_t i = 0; i < sizeof s_rec.reason && s_rec.reason[i]; i++) {
        const char c = s_rec.reason[i];
        if (c >= 0x20 && c < 0x7f && c != '"' && c != '\\') reason[k++] = c;
    }
    reason[k] = '\0';
    /* boots_ago 1 is "the reset this boot came out of"; larger numbers are older
     * panics the record has outlived, so a reader never mistakes one for news. */
    int n = snprintf(out, cap,
                     "\"panic\":{\"boots_ago\":%u,\"kind\":\"%s\",\"reason\":\"%s\",\"core\":%u,"
                     "\"pc\":\"0x%08x\",\"cause\":%u,\"vaddr\":\"0x%08x\",\"bt\":\"",
                     (unsigned)(s_boot.count - s_rec.boot), kind_name(s_rec.kind), reason,
                     (unsigned)s_rec.core, (unsigned)s_rec.pc, (unsigned)s_rec.cause,
                     (unsigned)s_rec.vaddr);
    for (uint32_t i = 0; i < s_rec.n_bt && n > 0 && (size_t)n < cap; i++)
        n += snprintf(out + n, cap - (size_t)n, i ? " 0x%08x" : "0x%08x", (unsigned)s_rec.bt[i]);
    if (n > 0 && (size_t)n < cap) n += snprintf(out + n, cap - (size_t)n, "\"},");
    if (n < 0 || (size_t)n >= cap) {
        out[0] = '\0';
        return -1;
    }
    return n;
}
