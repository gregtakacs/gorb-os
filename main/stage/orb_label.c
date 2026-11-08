/* orb_label.c — a label's pixels (docs/theme-text-plan.md §2.3). See orb_label.h. */
#include "orb_label.h"

#include <string.h>

#include "orb_latin.h"

static const uint8_t *glyph(const orb_font_t *f, int gi)
{
    const uint32_t cell = f->sparse ? f->rank[gi] : (uint32_t)gi;
    return f->glyphs + cell * f->w * f->h * (f->bpp == 1 ? 1u : 3u);
}

static bool carries(const orb_font_t *f, int gi)
{
    return gi >= 0 && gi < f->n && (!f->sparse || f->rank[gi] != ORB_GLYPH_NONE);
}

int orb_font_glyph(const orb_font_t *f, int gi)
{
    if (carries(f, gi)) return gi;
    return carries(f, '?' - 0x20) ? '?' - 0x20 : -1;
}

/* A coverage glyph's colour at cell row y: c0 at the top running to c1 at the foot. */
static uint16_t row_colour(const orb_font_t *f, int y)
{
    if (f->c0 == f->c1 || f->h < 2) return f->c0;
    const uint32_t t = (uint32_t)y * 256u / (uint32_t)(f->h - 1), u = 256u - t;
    const uint32_t r = ((f->c0 >> 11) * u + (f->c1 >> 11) * t + 128u) >> 8;
    const uint32_t g = (((f->c0 >> 5) & 0x3Fu) * u + ((f->c1 >> 5) & 0x3Fu) * t + 128u) >> 8;
    const uint32_t b = ((f->c0 & 0x1Fu) * u + (f->c1 & 0x1Fu) * t + 128u) >> 8;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

void orb_font_measure(orb_font_t *f)
{
    const uint32_t n = (uint32_t)f->w * f->h;
    for (int gi = 0; gi < f->n && gi < CLK_FONT_GLYPHS; gi++) {
        if (!carries(f, gi)) { f->adv[gi] = f->space; continue; }   /* drawn as another */
        const uint8_t *a = glyph(f, gi) + (f->bpp == 1 ? 0u : n * 2u);
        int right = -1;
        for (int x = f->w - 1; x >= 0 && right < 0; x--)
            for (int y = 0; y < f->h; y++)
                if (a[(uint32_t)y * f->w + (uint32_t)x]) { right = x; break; }
        const int adv = right < 0 ? f->space : right + 1 + f->track;
        f->adv[gi] = (uint8_t)(adv > 255 ? 255 : adv);
    }
    f->measured = true;
}

int orb_glyph_next(const char **p, uint16_t n)
{
    const uint8_t *s = (const uint8_t *)*p;
    uint32_t cp = *s++;
    if (cp >= 0xC0 && cp < 0xE0 && (s[0] & 0xC0) == 0x80) {
        cp = ((cp & 0x1Fu) << 6) | (s[0] & 0x3Fu);
        s += 1;
    } else if (cp >= 0xE0 && cp < 0xF0 && (s[0] & 0xC0) == 0x80 && (s[1] & 0xC0) == 0x80) {
        cp = ((cp & 0x0Fu) << 12) | ((uint32_t)(s[0] & 0x3F) << 6) | (s[1] & 0x3Fu);
        s += 2;
    } else if (cp >= 0x80) {
        while ((*s & 0xC0) == 0x80) s++;   /* a sequence this map has no glyph for */
        cp = '?';
    }
    *p = (const char *)s;
    if (cp == '\t') cp = ' ';
    int gi;
    switch (cp) {
    case 0x2191: gi = CLK_FONT_ASCII + 0; break;   /* up arrow    */
    case 0x2193: gi = CLK_FONT_ASCII + 1; break;   /* down arrow  */
    case 0x2192: gi = CLK_FONT_ASCII + 2; break;   /* right arrow */
    case 0x2190: gi = CLK_FONT_ASCII + 3; break;   /* left arrow  */
    case 0x00B0: gi = CLK_FONT_ASCII + 4; break;   /* degree sign */
    default:     gi = cp >= 0x20 && cp < 0x7F ? (int)cp - 0x20 : '?' - 0x20; break;
    }
    return gi < n ? gi : '?' - 0x20;
}

static const orb_latin_t *latin(uint32_t cp)
{
    int lo = 0, hi = (int)ORB_LATIN_N - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (ORB_LATIN[mid].cp == cp) return &ORB_LATIN[mid];
        if (ORB_LATIN[mid].cp < cp) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

bool orb_glyph_read(orb_glyph_rd_t *r, const char *stop, const orb_font_t *f, int *gi, int *mark)
{
    *mark = -1;
    if (r->pend) {                           /* the second plain letter of one (ß: s s) */
        *gi = orb_font_glyph(f, r->pend - 1);
        r->pend = 0;
        return true;
    }
    if (r->p >= stop || !*r->p) return false;
    const uint8_t *s = (const uint8_t *)r->p;
    uint32_t cp = *s;
    if (cp >= 0xC0 && cp < 0xE0 && s + 1 < (const uint8_t *)stop && (s[1] & 0xC0) == 0x80) {
        const orb_latin_t *l = latin(((cp & 0x1Fu) << 6) | (s[1] & 0x3Fu));
        if (l) {
            r->p += 2;
            int b = l->base;
            if (b == ORB_DOTLESS_I && !carries(f, b)) b = 'i' - 0x20;   /* an older font */
            *gi = orb_font_glyph(f, b);
            if (l->mark >= 0 && carries(f, l->mark)) *mark = l->mark;
            if (l->more != 255) r->pend = (uint8_t)(l->more + 1);
            return true;
        }
    }
    *gi = orb_font_glyph(f, orb_glyph_next(&r->p, f->n));
    return true;
}

int orb_mark_dx(const orb_font_t *f, int gi, int mark)
{
    if (gi < 0 || mark < ORB_MARK0 || mark >= ORB_MARK0 + ORB_MARKS) return 0;
    const int car = ORB_MARK_CARRIER[mark - ORB_MARK0];
    const int d = (int)f->adv[gi] - (int)f->adv[car];
    return ORB_MARK_RIGHT[mark - ORB_MARK0] ? d : d / 2;
}

/* One glyph cell over the plane at (gx, gy), clipped to [x0..x1] x [y0..y1]. Straight
 * alpha, the glyph over what is there: oa = a + da(1 - a), each colour weighted by its
 * share of oa. */
static void blit(uint8_t *plane, uint32_t pw, uint32_t ph, const orb_font_t *f, int gi,
                 int gx, int gy, int x0, int y0, int x1, int y1)
{
    const uint8_t *g = glyph(f, gi);
    const bool cov = f->bpp == 1;
    const uint16_t *gc = (const uint16_t *)g;
    const uint8_t *ga = cov ? g : g + (uint32_t)f->w * f->h * 2u;
    uint16_t *pc = (uint16_t *)plane;
    uint8_t *pa = plane + pw * ph * 2u;
    for (int y = 0; y < f->h; y++) {
        const int py = gy + y;
        if (py < y0 || py > y1) continue;
        const uint16_t rc = cov ? row_colour(f, y) : 0;
        for (int x = 0; x < f->w; x++) {
            const int px = gx + x;
            if (px < x0 || px > x1) continue;
            const uint32_t si = (uint32_t)y * f->w + (uint32_t)x;
            const uint32_t a = ga[si];
            if (!a) continue;
            const uint32_t di = (uint32_t)py * pw + (uint32_t)px;
            const uint32_t da = pa[di];
            const uint16_t c = cov ? rc : gc[si];
            if (a == 255u || !da) { pc[di] = c; pa[di] = (uint8_t)a; continue; }
            const uint32_t db = da * (255u - a) / 255u, oa = a + db;
            const uint16_t d = pc[di];
            const uint32_t r = ((uint32_t)(c >> 11) * a + (uint32_t)(d >> 11) * db) / oa;
            const uint32_t gg = ((uint32_t)((c >> 5) & 0x3Fu) * a + (uint32_t)((d >> 5) & 0x3Fu) * db) / oa;
            const uint32_t b = ((uint32_t)(c & 0x1Fu) * a + (uint32_t)(d & 0x1Fu) * db) / oa;
            pc[di] = (uint16_t)((r << 11) | (gg << 5) | b);
            pa[di] = (uint8_t)oa;
        }
    }
}

void orb_label_compose(uint8_t *plane, uint32_t w, uint32_t h, const uint8_t *panel,
                       orb_font_t *f, const char *text, int pad, int halign, int valign)
{
    if (panel) memcpy(plane, panel, (size_t)w * h * 3u);
    else memset(plane, 0, (size_t)w * h * 3u);
    if (!f || !f->glyphs || !text) return;
    if (!f->measured) orb_font_measure(f);
    const int x0 = pad, y0 = pad, x1 = (int)w - 1 - pad, y1 = (int)h - 1 - pad;
    int lines = 1;
    for (const char *p = text; *p; p++) lines += *p == '\n';
    const int text_h = lines * f->h;
    int y = valign == 1 ? y0 + ((y1 - y0 + 1) - text_h) / 2
          : valign == 2 ? y1 + 1 - text_h : y0;
    for (const char *line = text; line; ) {
        const char *nl = strchr(line, '\n');
        const char *stop = nl ? nl : line + strlen(line);
        int lw = 0, g, m;
        for (orb_glyph_rd_t r = { line, 0 }; orb_glyph_read(&r, stop, f, &g, &m); )
            lw += g < 0 ? f->space : f->adv[g];
        int x = halign == 1 ? x0 + ((x1 - x0 + 1) - lw) / 2
              : halign == 2 ? x1 + 1 - lw : x0;
        for (orb_glyph_rd_t r = { line, 0 }; x <= x1 && orb_glyph_read(&r, stop, f, &g, &m); ) {
            if (g < 0) { x += f->space; continue; }
            blit(plane, w, h, f, g, x, y, x0, y0, x1, y1);
            if (m >= 0) blit(plane, w, h, f, m, x + orb_mark_dx(f, g, m), y, x0, y0, x1, y1);
            x += f->adv[g];
        }
        y += f->h;
        line = nl ? nl + 1 : NULL;
    }
}
