/*
 * The ring behind orb_log.h.
 *
 * What the hook receives is the complete line. The ESP_LOGI macro in
 * components/log/include/esp_log.h:62 expands the user's format through LOG_FORMAT
 * into "I (12345) tag: message\n" before esp_log_write() is called, and
 * esp_log_writev() in components/log/src/os/log_write.c hands that expanded string
 * straight to the installed vprintf — so level, uptime, tag and the terminating
 * newline are all in what lands here, and the ring holds whole lines.
 *
 * The one thing stripped is the color escape: LOG_COLOR_* prefixes each line when
 * CONFIG_LOG_COLORS is on, and the reader of this ring is a pipe into less rather
 * than a terminal, so the escapes are noise that would need a -r to get rid of.
 *
 * Two consequences of where the hook sits, both deliberate:
 *   - Tag-level filtering happens in esp_log_writev() BEFORE the hook runs, so a
 *     tag muted by esp_log_level_set() leaves nothing in the ring either. The ring
 *     mirrors what the console would have shown, not more.
 *   - Never log from inside the hook: the log lock may already be held by the very
 *     call being captured, and re-entering it deadlocks the task that logged.
 *
 * Sizing: ORB_LOG_BYTES is .bss in internal DRAM, so it is gone before the heap is
 * ever asked. 2048 B was chosen against the one number that matters on this board —
 * heap_internal_largest, measured 13824 B idle and 7168 B with the MTP mode armed —
 * because the MTP backend's GetObjectPropList allocates two CONFIG_TINYUSB_MTP_
 * MAX_OBJECTS-sized buffers plus a measured payload copy from that same internal
 * pool, and a ring that steals the largest block is a ring that reintroduces the
 * failure it was built to observe. 1024 was too short to hold one enumeration
 * (GetDeviceInfo, OpenSession, GetStorageIDs and one prop-list line each cost
 * ~150 B); 4096 was rejected as a heap tax on every boot for lines nobody has read
 * yet. Re-measure the largest block after changing this.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "orb_log.h"
#include "esp_log.h"

/* One formatted line, on the caller's stack rather than a shared static: logs come
 * from many tasks at once and a shared buffer would interleave them into garbage.
 * 192 B truncates the longest vendor trace line by a few characters of its
 * parameter list, which is the least informative part of it. */
#define ORB_LOG_LINE 192

static char        s_ring[ORB_LOG_BYTES];
static size_t      s_head;                        /* next write index        */
static size_t      s_len;                         /* valid bytes, <= bytes   */
static uint32_t    s_dropped;                     /* bytes overwritten       */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_prev;

static void ring_push(const char *line, int n)
{
    if (n <= 0) {
        return;
    }
    if (n > ORB_LOG_LINE) {
        n = ORB_LOG_LINE;
    }

    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < n; i++) {
        const char c = line[i];

        /* Drop an ANSI color sequence whole: ESC '[' through the terminating 'm'.
         * An escape cut off by the line cap is stored rather than swallowed, so a
         * truncated record cannot silently eat the text behind it. */
        if (c == 0x1b && (i + 1) < n && line[i + 1] == '[') {
            int j = i + 2;
            while (j < n && line[j] != 'm') {
                j++;
            }
            if (j < n) {
                i = j;
                continue;
            }
        }

        s_ring[s_head] = c;
        s_head = (s_head + 1) % ORB_LOG_BYTES;
        if (s_len < ORB_LOG_BYTES) {
            s_len++;
        } else {
            s_dropped++;   /* wrapped: the oldest byte is gone */
        }
    }
    portEXIT_CRITICAL(&s_mux);
}

static int ring_vprintf(const char *fmt, va_list args)
{
    char     line[ORB_LOG_LINE];
    va_list  again;

    /* vsnprintf consumes args, and the chained sink below consumes it too. */
    va_copy(again, args);
    const int n = vsnprintf(line, sizeof line, fmt, again);
    va_end(again);

    if (n > 0) {
        ring_push(line, n < (int)sizeof line ? n : (int)sizeof line - 1);
    }
    return s_prev ? s_prev(fmt, args) : n;
}

esp_err_t orb_log_init(void)
{
    if (s_prev != NULL) {
        return ESP_OK;   /* already hooked; chaining twice would double the ring */
    }
    s_prev = esp_log_set_vprintf(ring_vprintf);
    return ESP_OK;
}

size_t orb_log_tail(char *out, size_t outsz)
{
    if (out == NULL || outsz == 0) {
        return 0;
    }
    out[0] = '\0';

    portENTER_CRITICAL(&s_mux);
    const size_t len  = s_len;
    const size_t head = s_head;
    portEXIT_CRITICAL(&s_mux);

    /* Newest bytes win: when the caller's buffer is smaller than the ring, the
     * slice taken is the tail of it, which is the part that just happened. */
    size_t n = len < (outsz - 1) ? len : (outsz - 1);
    if (n == 0) {
        return 0;
    }
    const size_t start = (head + ORB_LOG_BYTES - n) % ORB_LOG_BYTES;
    const size_t first = ORB_LOG_BYTES - start;    /* bytes before the wrap */
    int whole_head;

    portENTER_CRITICAL(&s_mux);
    if (first >= n) {
        memcpy(out, &s_ring[start], n);
    } else {
        memcpy(out, &s_ring[start], first);
        memcpy(out + first, &s_ring[0], n - first);
    }
    /* Whether the slice begins at a line boundary is a fact about the ring, not
     * about the text: it is aligned if nothing older was ever recorded, and
     * otherwise whatever byte sits just before the oldest retained one. Guessing
     * this from the text instead drops a whole leading line on every read. */
    whole_head = (len < ORB_LOG_BYTES) ||
                 (s_ring[(start + ORB_LOG_BYTES - 1) % ORB_LOG_BYTES] == '\n');
    portEXIT_CRITICAL(&s_mux);

    out[n] = '\0';

    /* A wrap cuts mid-line. Start reporting at the first whole line so a reader
     * never sees the tail of one message welded onto the head of the next. */
    if (!whole_head) {
        const char *nl = memchr(out, '\n', n);
        if (nl == NULL) {
            out[0] = '\0';   /* one partial line and nothing else */
            return 0;
        }
        const size_t skip = (size_t)(nl - out) + 1;
        memmove(out, out + skip, n - skip);
        out[n - skip] = '\0';
        n -= skip;
    }
    return n;
}

uint32_t orb_log_dropped(void)
{
    return s_dropped;
}
