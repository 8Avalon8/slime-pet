#include "slime_text.h"

#include <stdbool.h>
#include <stddef.h>

volatile sl_lang_t sl_lang = SL_LANG_ZH;

static const char *utf8_next(const char *s, uint32_t *cp)
{
    const unsigned char c = (unsigned char)*s;
    if (c < 0x80) {
        *cp = c;
        return s + 1;
    }
    if ((c & 0xe0) == 0xc0 && s[1]) {
        *cp = ((c & 0x1f) << 6) | (s[1] & 0x3f);
        return s + 2;
    }
    if ((c & 0xf0) == 0xe0 && s[1] && s[2]) {
        *cp = ((c & 0x0f) << 12) | ((s[1] & 0x3f) << 6) | (s[2] & 0x3f);
        return s + 3;
    }
    if ((c & 0xf8) == 0xf0 && s[1] && s[2] && s[3]) {
        *cp = ((c & 0x07) << 18) | ((s[1] & 0x3f) << 12) | ((s[2] & 0x3f) << 6) | (s[3] & 0x3f);
        return s + 4;
    }
    *cp = 0xfffd;
    return s + 1;
}

static const sl_glyph_t *find(uint32_t cp, sl_font_t font)
{
    int lo = 0, hi = sl_glyph_count - 1;
    const uint32_t key = ((uint32_t)font << 24) | cp;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        const uint32_t k = ((uint32_t)sl_glyphs[mid].font << 24) | sl_glyphs[mid].cp;
        if (k == key) return &sl_glyphs[mid];
        if (k < key) lo = mid + 1;
        else hi = mid - 1;
    }
    return NULL;
}

/* Main atlas first (all sizes, 8-bit), then the 22 px common-character table (4-bit). */
static const sl_glyph_t *find_any(uint32_t cp, sl_font_t font, bool *a4)
{
    *a4 = false;
    const sl_glyph_t *g = find(cp, font);
    if (g || font != SL_FONT_22) return g;
    int lo = 0, hi = sl_glyph_ext_count - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (sl_glyphs_ext[mid].cp == cp) {
            *a4 = true;
            return &sl_glyphs_ext[mid];
        }
        if (sl_glyphs_ext[mid].cp < cp) lo = mid + 1;
        else hi = mid - 1;
    }
    return NULL;
}

int sl_text_count(const char *s)
{
    int n = 0;
    uint32_t cp;
    while (*s) {
        s = utf8_next(s, &cp);
        n++;
    }
    return n;
}

int sl_text_width(const char *s, sl_font_t font)
{
    int w = 0;
    uint32_t cp;
    while (*s) {
        s = utf8_next(s, &cp);
        bool a4;
        const sl_glyph_t *g = find_any(cp, font, &a4);
        w += g ? g->adv : 10;
    }
    return w;
}

int sl_text_missing(const char *s, sl_font_t font)
{
    int n = 0;
    uint32_t cp;
    while (*s) {
        s = utf8_next(s, &cp);
        bool a4;
        if (!find_any(cp, font, &a4)) n++;
    }
    return n;
}

int sl_text_draw(sg_canvas_t *cv, const char *s, float x, float baseline, sl_font_t font, sg_rgb_t c, int max_chars)
{
    int n = 0;
    float pen = x;
    uint32_t cp;
    const sg_fix_t cf = sg_tofix(c);
    while (*s) {
        s = utf8_next(s, &cp);
        if (max_chars >= 0 && n >= max_chars) {
            n++;
            continue;
        }
        n++;
        bool is4;
        const sl_glyph_t *g = find_any(cp, font, &is4);
        if (!g) {
            pen += 10;
            continue;
        }
        const int gx = (int)(pen + 0.5f) + g->ox, gy = (int)(baseline + 0.5f) + g->oy;
        const uint8_t *a8 = &sl_glyph_a8[g->off], *a4 = &sl_glyph_a4[g->off];
        for (int yy = 0; yy < g->h; yy++) {
            for (int xx = 0; xx < g->w; xx++) {
                const int i = yy * g->w + xx;
                const uint8_t a = is4 ? ((a4[i >> 1] >> ((i & 1) ? 0 : 4)) & 15) * 17 : a8[i];
                if (a) sg_blend_fix(cv, gx + xx, gy + yy, cf, (a * 257 + 128) >> 8);
            }
        }
        pen += g->adv;
    }
    return n;
}
