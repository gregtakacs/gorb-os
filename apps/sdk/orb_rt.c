/**
 * orb_rt.c — the C runtime an app links instead of a libc (apps/sdk/orb.h).
 *
 * Freestanding wasm32 has no libc, and carrying one would cost every app tens of KB
 * of code it never calls. This is the subset the shipped apps use, written for size:
 * the string and memory primitives clang may also emit calls to on its own, and one
 * bounded formatter that covers the conversions an app prints (d i u x X c s p % and
 * f, with flags - 0 +, width, precision, and the l/ll/z length modifiers). Anything
 * unrecognised prints itself, so a format bug shows up on the console rather than
 * reading past the argument list.
 */
#include "orb.h"

#include <stdarg.h>
#include <stdbool.h>

void *memset(void *d, int c, size_t n)
{
    uint8_t *p = d;
    while (n--) *p++ = (uint8_t)c;
    return d;
}

void *memcpy(void *d, const void *s, size_t n)
{
    uint8_t *a = d;
    const uint8_t *b = s;
    while (n--) *a++ = *b++;
    return d;
}

void *memmove(void *d, const void *s, size_t n)
{
    uint8_t *a = d;
    const uint8_t *b = s;
    if (a == b || !n) return d;
    if (a < b) { while (n--) *a++ = *b++; }
    else { a += n; b += n; while (n--) *--a = *--b; }
    return d;
}

int memcmp(const void *x, const void *y, size_t n)
{
    const uint8_t *a = x, *b = y;
    for (; n; n--, a++, b++)
        if (*a != *b) return *a - *b;
    return 0;
}

size_t strlen(const char *s)
{
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (uint8_t)*a - (uint8_t)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        if (*a != *b) return (uint8_t)*a - (uint8_t)*b;
        if (!*a) return 0;
    }
    return 0;
}

void *memchr(const void *s, int c, size_t n)
{
    const uint8_t *p = s;
    for (; n; n--, p++)
        if (*p == (uint8_t)c) return (void *)p;
    return NULL;
}

char *strchr(const char *s, int c)
{
    for (;; s++) {
        if (*s == (char)c) return (char *)s;
        if (!*s) return NULL;
    }
}

/* ---- the formatter ------------------------------------------------------------ */

typedef struct { char *p; size_t cap, n; } out_t;

static void put(out_t *o, char c)
{
    if (o->n + 1 < o->cap) o->p[o->n] = c;
    o->n++;
}

static void put_pad(out_t *o, char c, int n)
{
    while (n-- > 0) put(o, c);
}

/* One converted field: sign/prefix, then digits, padded to `width`. */
static void put_field(out_t *o, const char *pre, const char *dig, int ndig, int width,
                      bool left, bool zero)
{
    const int npre = (int)strlen(pre);
    const int pad = width - npre - ndig;
    if (!left && !zero) put_pad(o, ' ', pad);
    for (int i = 0; i < npre; i++) put(o, pre[i]);
    if (!left && zero) put_pad(o, '0', pad);
    for (int i = 0; i < ndig; i++) put(o, dig[i]);
    if (left) put_pad(o, ' ', pad);
}

static int utoa_rev(uint64_t v, unsigned base, bool upper, char *buf)
{
    const char *d = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int n = 0;
    do { buf[n++] = d[v % base]; v /= base; } while (v);
    return n;
}

static int vfmt(char *dst, size_t cap, const char *f, va_list ap)
{
    out_t o = { dst, cap, 0 };
    for (; *f; f++) {
        if (*f != '%') { put(&o, *f); continue; }
        const char *start = f++;
        bool left = false, zero = false, plus = false;
        for (;; f++) {
            if (*f == '-') left = true;
            else if (*f == '0') zero = true;
            else if (*f == '+') plus = true;
            else break;
        }
        int width = 0, prec = -1;
        if (*f == '*') { width = va_arg(ap, int); f++; }
        else while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
        if (*f == '.') {
            f++; prec = 0;
            if (*f == '*') { prec = va_arg(ap, int); f++; }
            else while (*f >= '0' && *f <= '9') prec = prec * 10 + (*f++ - '0');
        }
        int lng = 0;
        while (*f == 'l') { lng++; f++; }
        if (*f == 'z') { lng = 0; f++; }          /* size_t is 32-bit on wasm32 */
        char rev[24], dig[24];
        int n;
        switch (*f) {
        case 'd': case 'i': {
            int64_t v = lng >= 2 ? va_arg(ap, long long) : lng ? va_arg(ap, long) : va_arg(ap, int);
            const bool neg = v < 0;
            n = utoa_rev(neg ? (uint64_t)0 - (uint64_t)v : (uint64_t)v, 10, false, rev);
            for (int i = 0; i < n; i++) dig[i] = rev[n - 1 - i];
            put_field(&o, neg ? "-" : plus ? "+" : "", dig, n, width, left, zero);
            break;
        }
        case 'u': case 'x': case 'X': case 'p': {
            uint64_t v;
            if (*f == 'p') v = (uintptr_t)va_arg(ap, void *);
            else v = lng >= 2 ? va_arg(ap, unsigned long long)
                   : lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned);
            n = utoa_rev(v, *f == 'u' ? 10 : 16, *f == 'X', rev);
            for (int i = 0; i < n; i++) dig[i] = rev[n - 1 - i];
            put_field(&o, *f == 'p' ? "0x" : "", dig, n, width, left, zero);
            break;
        }
        case 'f': {
            double v = va_arg(ap, double);
            if (prec < 0) prec = 6;
            if (prec > 9) prec = 9;
            const bool neg = v < 0;
            if (neg) v = -v;
            if (v != v) { put_field(&o, "", "nan", 3, width, left, false); break; }
            uint64_t scale = 1;
            for (int i = 0; i < prec; i++) scale *= 10;
            const uint64_t whole = (uint64_t)v;
            uint64_t frac = (uint64_t)((v - (double)whole) * (double)scale + 0.5);
            uint64_t w = whole;
            if (frac >= scale) { frac -= scale; w++; }
            n = utoa_rev(w, 10, false, rev);
            int k = 0;
            for (int i = 0; i < n; i++) dig[k++] = rev[n - 1 - i];
            if (prec) {
                dig[k++] = '.';
                for (int i = prec - 1; i >= 0; i--) { rev[i] = (char)('0' + frac % 10); frac /= 10; }
                for (int i = 0; i < prec; i++) dig[k++] = rev[i];
            }
            put_field(&o, neg ? "-" : plus ? "+" : "", dig, k, width, left, zero);
            break;
        }
        case 'c':
            dig[0] = (char)va_arg(ap, int);
            put_field(&o, "", dig, 1, width, left, false);
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int len = (int)strlen(s);
            if (prec >= 0 && len > prec) len = prec;
            if (!left) put_pad(&o, ' ', width - len);
            for (int i = 0; i < len; i++) put(&o, s[i]);
            if (left) put_pad(&o, ' ', width - len);
            break;
        }
        case '%':
            put(&o, '%');
            break;
        default:                                  /* unknown: print it whole */
            for (const char *q = start; q <= f && *q; q++) put(&o, *q);
            if (!*f) f--;
            break;
        }
    }
    if (cap) dst[o.n < cap ? o.n : cap - 1] = '\0';
    return (int)o.n;
}

int vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap)
{
    return vfmt(buf, cap, fmt, ap);
}

int snprintf(char *buf, size_t cap, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    const int n = vfmt(buf, cap, fmt, ap);
    va_end(ap);
    return n;
}

void orb_logf(const char *fmt, ...)
{
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    vfmt(line, sizeof line, fmt, ap);
    va_end(ap);
    orb_log(line);
}

/* ---- number parsing ------------------------------------------------------------ */

static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

/* Decimal only, which is all JSON carries: sign, digits, fraction, exponent. Exact to
 * the last bit is not the goal; a coordinate good to 1e-9 degrees is. */
double strtod(const char *s, char **end)
{
    const char *p = s;
    while (is_space(*p)) p++;
    bool neg = false;
    if (*p == '-' || *p == '+') neg = *p++ == '-';
    double v = 0.0;
    bool any = false;
    while (*p >= '0' && *p <= '9') { v = v * 10.0 + (*p++ - '0'); any = true; }
    if (*p == '.') {
        p++;
        double scale = 0.1;
        while (*p >= '0' && *p <= '9') { v += (*p++ - '0') * scale; scale *= 0.1; any = true; }
    }
    if (!any) { if (end) *end = (char *)s; return 0.0; }
    if (*p == 'e' || *p == 'E') {
        const char *q = p + 1;
        bool eneg = false;
        if (*q == '-' || *q == '+') eneg = *q++ == '-';
        if (*q >= '0' && *q <= '9') {
            int e = 0;
            while (*q >= '0' && *q <= '9') { if (e < 400) e = e * 10 + (*q - '0'); q++; }
            double m = 1.0;
            while (e--) m *= 10.0;
            v = eneg ? v / m : v * m;
            p = q;
        }
    }
    if (end) *end = (char *)p;
    return neg ? -v : v;
}

long strtol(const char *s, char **end, int base)
{
    const char *p = s;
    while (is_space(*p)) p++;
    bool neg = false;
    if (*p == '-' || *p == '+') neg = *p++ == '-';
    if (base == 0) base = 10;
    long v = 0;
    const char *d0 = p;
    for (;; p++) {
        int dv;
        if (*p >= '0' && *p <= '9') dv = *p - '0';
        else if (*p >= 'a' && *p <= 'z') dv = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'Z') dv = *p - 'A' + 10;
        else break;
        if (dv >= base) break;
        v = v * base + dv;
    }
    if (end) *end = (char *)(p == d0 ? s : p);
    return neg ? -v : v;
}

int atoi(const char *s) { return (int)strtol(s, NULL, 10); }

/* ---- rounding and max/min (apps/sdk/libc/math.h says why these are functions) ---- */

double round(double x)  { return x < 0 ? -__builtin_floor(-x + 0.5) : __builtin_floor(x + 0.5); }
float  roundf(float x)  { return x < 0 ? -__builtin_floorf(-x + 0.5f) : __builtin_floorf(x + 0.5f); }
long   lround(double x) { return (long)round(x); }
long   lroundf(float x) { return (long)roundf(x); }
/* C's rule: a NaN loses to a number. */
float  fmaxf(float a, float b)   { return a != a ? b : b != b ? a : a > b ? a : b; }
float  fminf(float a, float b)   { return a != a ? b : b != b ? a : a < b ? a : b; }
double fmax(double a, double b)  { return a != a ? b : b != b ? a : a > b ? a : b; }
double fmin(double a, double b)  { return a != a ? b : b != b ? a : a < b ? a : b; }
