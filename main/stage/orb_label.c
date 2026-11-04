/* orb_label.c — a label's pixels (docs/theme-text-plan.md §2.3). See orb_label.h. */
#include "orb_label.h"

#include <string.h>

static const uint8_t *glyph(const orb_font_t *f, int gi)
{
    return f->glyphs + (uint32_t)gi * f->w * f->h * 3u;
}

void orb_font_measure(orb_font_t *f)
{
    const uint32_t n = (uint32_t)f->w * f->h;
    for (int gi = 0; gi < f->n && gi < CLK_FONT_GLYPHS; gi++) {
        const uint8_t *a = glyph(f, gi) + n * 2u;
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

/* One glyph cell over the plane at (gx, gy), clipped to [x0..x1] x [y0..y1]. Straight
 * alpha, the glyph over what is there: oa = a + da(1 - a), each colour weighted by its
 * share of oa. */
static void blit(uint8_t *plane, uint32_t pw, uint32_t ph, const orb_font_t *f, int gi,
                 int gx, int gy, int x0, int y0, int x1, int y1)
{
    const uint8_t *g = glyph(f, gi);
    const uint16_t *gc = (const uint16_t *)g;
    const uint8_t *ga = g + (uint32_t)f->w * f->h * 2u;
    uint16_t *pc = (uint16_t *)plane;
    uint8_t *pa = plane + pw * ph * 2u;
    for (int y = 0; y < f->h; y++) {
        const int py = gy + y;
        if (py < y0 || py > y1) continue;
        for (int x = 0; x < f->w; x++) {
            const int px = gx + x;
            if (px < x0 || px > x1) continue;
            const uint32_t si = (uint32_t)y * f->w + (uint32_t)x;
            const uint32_t a = ga[si];
            if (!a) continue;
            const uint32_t di = (uint32_t)py * pw + (uint32_t)px;
            const uint32_t da = pa[di];
            const uint16_t c = gc[si];
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
        int lw = 0;
        for (const char *p = line; p < stop; ) lw += f->adv[orb_glyph_next(&p, f->n)];
        int x = halign == 1 ? x0 + ((x1 - x0 + 1) - lw) / 2
              : halign == 2 ? x1 + 1 - lw : x0;
        for (const char *p = line; p < stop && x <= x1; ) {
            const int gi = orb_glyph_next(&p, f->n);
            blit(plane, w, h, f, gi, x, y, x0, y0, x1, y1);
            x += f->adv[gi];
        }
        y += f->h;
        line = nl ? nl + 1 : NULL;
    }
}
