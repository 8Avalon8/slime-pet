#include "sg.h"

#include <math.h>
#include <string.h>

#define D(b) (((b) * 2 + 1) * 2048) /* (b + 0.5) / 16 in 16.16 */
const int32_t SG_DITHER[4][4] = {{D(0), D(8), D(2), D(10)}, {D(12), D(4), D(14), D(6)}, {D(3), D(11), D(1), D(9)}, {D(15), D(7), D(13), D(5)}};
#undef D

static inline float clamp01(float v)
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }

static sg_scratch_t s_default_scratch;

void sg_canvas_init(sg_canvas_t *cv, uint16_t *px, int w, int h, int stride)
{
    cv->scratch = &s_default_scratch;
    cv->split = 0;
    cv->owner = 0;
    cv->px = px;
    cv->w = w;
    cv->h = h;
    cv->stride = stride;
    sg_reset_clip(cv);
}

void sg_canvas_set_split(sg_canvas_t *cv, sg_scratch_t *scratch, int owner)
{
    cv->scratch = scratch;
    cv->split = 1;
    cv->owner = (uint8_t)(owner & 1);
}

void sg_set_clip(sg_canvas_t *cv, int x0, int y0, int x1, int y1)
{
    cv->clip_x0 = imax(0, x0);
    cv->clip_y0 = imax(0, y0);
    cv->clip_x1 = imin(cv->w, x1);
    cv->clip_y1 = imin(cv->h, y1);
}

void sg_reset_clip(sg_canvas_t *cv)
{
    sg_set_clip(cv, 0, 0, cv->w, cv->h);
}

uint16_t sg_pack(sg_rgb_t c, int x, int y)
{
    return sg_pack_fix(sg_tofix(c), x, y);
}

sg_rgb_t sg_unpack(uint16_t v)
{
    sg_rgb_t o = {((v >> 11) & 31) / 31.0f, ((v >> 5) & 63) / 63.0f, (v & 31) / 31.0f};
    return o;
}

void sg_blend(sg_canvas_t *cv, int x, int y, sg_rgb_t c, float a)
{
    sg_blend_fix(cv, x, y, sg_tofix(c), sg_a256(a));
}

sg_rgb_t sg_paint_at(const sg_paint_t *p, float y)
{
    if (p->kind == SG_PAINT_SOLID) {
        return p->c0;
    }
    const float t = clamp01((y - p->y0) / (p->y1 - p->y0));
    if (t < p->mid) {
        return sg_lerp(p->c0, p->c1, t / p->mid);
    }
    return sg_lerp(p->c1, p->c2, (t - p->mid) / (1.0f - p->mid));
}

void sg_fill_rect(sg_canvas_t *cv, int x0, int y0, int x1, int y1, uint16_t c)
{
    x0 = imax(x0, cv->clip_x0);
    y0 = imax(y0, cv->clip_y0);
    x1 = imin(x1, cv->clip_x1);
    y1 = imin(y1, cv->clip_y1);
    for (int y = y0; y < y1; y++) {
        if (!sg_row_mine(cv, y)) continue;
        uint16_t *row = &cv->px[y * cv->stride];
        for (int x = x0; x < x1; x++) {
            row[x] = c;
        }
    }
}

void sg_dim_rect(sg_canvas_t *cv, int x0, int y0, int x1, int y1, float k)
{
    const int32_t k256 = sg_a256(k);
    x0 = imax(x0, cv->clip_x0);
    y0 = imax(y0, cv->clip_y0);
    x1 = imin(x1, cv->clip_x1);
    y1 = imin(y1, cv->clip_y1);
    for (int y = y0; y < y1; y++) {
        if (!sg_row_mine(cv, y)) continue;
        uint16_t *row = &cv->px[y * cv->stride];
        for (int x = x0; x < x1; x++) {
            if (row[x]) {
                sg_fix_t c = sg_unpack_fix(row[x]);
                c.r = (c.r * k256) >> 8;
                c.g = (c.g * k256) >> 8;
                c.b = (c.b * k256) >> 8;
                row[x] = sg_pack_fix(c, x, y);
            }
        }
    }
}

static inline float ell_dist(float dx, float dy, float rx, float ry)
{
    const float irx2 = 1.0f / (rx * rx), iry2 = 1.0f / (ry * ry);
    const float F = dx * dx * irx2 + dy * dy * iry2 - 1.0f;
    const float gx = 2.0f * dx * irx2, gy = 2.0f * dy * iry2;
    const float g = sqrtf(gx * gx + gy * gy);
    if (g < 1e-6f) {
        return -sg_minf(rx, ry);
    }
    return F / g;
}

void sg_fill_ellipse(sg_canvas_t *cv, float cx, float cy, float rx, float ry, const sg_paint_t *paint, float alpha)
{
    if (rx < 0.3f || ry < 0.3f) {
        return;
    }
    const int x0 = imax(cv->clip_x0, (int)floorf(cx - rx - 1)), x1 = imin(cv->clip_x1, (int)ceilf(cx + rx + 1));
    const int y0 = imax(cv->clip_y0, (int)floorf(cy - ry - 1)), y1 = imin(cv->clip_y1, (int)ceilf(cy + ry + 1));
    for (int y = y0; y < y1; y++) {
        if (!sg_row_mine(cv, y)) continue;
        const float py = y + 0.5f - cy;
        const sg_fix_t c = sg_tofix(sg_paint_at(paint, y + 0.5f));
        for (int x = x0; x < x1; x++) {
            const float cov = clamp01(0.5f - ell_dist(x + 0.5f - cx, py, rx, ry));
            if (cov > 0) {
                sg_blend_fix(cv, x, y, c, sg_a256(alpha * cov));
            }
        }
    }
}

void sg_stroke_ellipse(sg_canvas_t *cv, float cx, float cy, float rx, float ry, float width, sg_rgb_t c, float alpha)
{
    const float hw = width * 0.5f;
    const sg_fix_t cf = sg_tofix(c);
    const int x0 = imax(cv->clip_x0, (int)floorf(cx - rx - hw - 1)), x1 = imin(cv->clip_x1, (int)ceilf(cx + rx + hw + 1));
    const int y0 = imax(cv->clip_y0, (int)floorf(cy - ry - hw - 1)), y1 = imin(cv->clip_y1, (int)ceilf(cy + ry + hw + 1));
    for (int y = y0; y < y1; y++) {
        if (!sg_row_mine(cv, y)) continue;
        const float py = y + 0.5f - cy;
        for (int x = x0; x < x1; x++) {
            const float d = ell_dist(x + 0.5f - cx, py, rx, ry);
            const float cov = clamp01(hw + 0.5f - fabsf(d));
            if (cov > 0) {
                sg_blend_fix(cv, x, y, cf, sg_a256(alpha * cov));
            }
        }
    }
}

void sg_radial_blob(sg_canvas_t *cv, float cx, float cy, float rx, float ry, float rot, sg_rgb_t c, float a)
{
    const float R = sg_maxf(rx, ry) + 1.0f;
    const float cs = sg_cosf(rot), sn = sg_sinf(rot);
    const int x0 = imax(cv->clip_x0, (int)floorf(cx - R)), x1 = imin(cv->clip_x1, (int)ceilf(cx + R));
    const int y0 = imax(cv->clip_y0, (int)floorf(cy - R)), y1 = imin(cv->clip_y1, (int)ceilf(cy + R));
    const float irx = 1.0f / rx, iry = 1.0f / ry;
    const sg_fix_t cf = sg_tofix(c);
    for (int y = y0; y < y1; y++) {
        if (!sg_row_mine(cv, y)) continue;
        const float dy = y + 0.5f - cy;
        for (int x = x0; x < x1; x++) {
            const float dx = x + 0.5f - cx;
            const float lx = (dx * cs + dy * sn) * irx, ly = (-dx * sn + dy * cs) * iry;
            const float q2 = lx * lx + ly * ly;
            if (q2 >= 1.0f) {
                continue;
            }
            const float q = sqrtf(q2);
            const float al = q < 0.55f ? a * (1.0f - 0.45f * q / 0.55f) : a * 0.55f * (1.0f - q) / 0.45f;
            sg_blend_fix(cv, x, y, cf, sg_a256(al));
        }
    }
}

void sg_rowacc_clear(sg_rowacc_t *r)
{
    if (r->x1 > r->x0) {
        memset(&r->acc[r->x0], 0, (size_t)(r->x1 - r->x0 + 1) * sizeof(r->acc[0]));
    }
    r->x0 = SG_MAX_W;
    r->x1 = 0;
}

void sg_rowacc_span(sg_rowacc_t *r, float xa, float xb, float w, int clip_x0, int clip_x1)
{
    if (xa < clip_x0) xa = (float)clip_x0;
    if (xb > clip_x1) xb = (float)clip_x1;
    if (xb <= xa) {
        return;
    }
    const int ia = (int)xa, ib = (int)xb; /* both >= 0 after clipping, so truncation == floor */
    const float wf = w * 256.0f;
    const int32_t wi = (int32_t)(wf + 0.5f);
    if (ia == ib) {
        r->acc[ia] += (int32_t)((xb - xa) * wf + 0.5f);
    } else {
        r->acc[ia] += (int32_t)((ia + 1 - xa) * wf + 0.5f);
        int32_t *a = &r->acc[ia + 1];
        for (int i = ia + 1; i < ib; i++) {
            *a++ += wi;
        }
        if (ib < clip_x1) {
            r->acc[ib] += (int32_t)((xb - ib) * wf + 0.5f);
        }
    }
    if (ia < r->x0) r->x0 = ia;
    if (ib + 1 > r->x1) r->x1 = imin(ib + 1, clip_x1);
}

#define SG_MAX_XINGS 64

static void sort_floats(float *v, int n)
{
    for (int i = 1; i < n; i++) {
        const float k = v[i];
        int j = i - 1;
        while (j >= 0 && v[j] > k) {
            v[j + 1] = v[j];
            j--;
        }
        v[j + 1] = k;
    }
}

void sg_fill_poly(sg_canvas_t *cv, const float *xy, int n, const sg_paint_t *paint, float alpha)
{
    sg_rowacc_t *const rap = &cv->scratch->ra;
    const int32_t a256 = sg_a256(alpha);
    if (n < 3) {
        return;
    }
    float ymin = xy[1], ymax = xy[1];
    for (int i = 1; i < n; i++) {
        ymin = sg_minf(ymin, xy[2 * i + 1]);
        ymax = sg_maxf(ymax, xy[2 * i + 1]);
    }
    const int y0 = imax(cv->clip_y0, (int)floorf(ymin)), y1 = imin(cv->clip_y1, (int)ceilf(ymax) + 1);
    float xs[SG_MAX_XINGS];
    for (int y = y0; y < y1; y++) {
        if (!sg_row_mine(cv, y)) continue;
        sg_rowacc_clear(rap);
        for (int s = 0; s < 4; s++) {
            const float ys = y + (s + 0.5f) * 0.25f;
            int k = 0;
            for (int i = 0, j = n - 1; i < n; j = i++) {
                const float yi = xy[2 * i + 1], yj = xy[2 * j + 1];
                if ((yi <= ys && yj > ys) || (yj <= ys && yi > ys)) {
                    if (k < SG_MAX_XINGS) {
                        xs[k++] = xy[2 * i] + (ys - yi) * (xy[2 * j] - xy[2 * i]) / (yj - yi);
                    }
                }
            }
            sort_floats(xs, k);
            for (int i = 0; i + 1 < k; i += 2) {
                sg_rowacc_span(rap, xs[i], xs[i + 1], 0.25f, cv->clip_x0, cv->clip_x1);
            }
        }
        if (rap->x1 <= rap->x0) {
            continue;
        }
        const sg_fix_t c = sg_tofix(sg_paint_at(paint, y + 0.5f));
        for (int x = rap->x0; x < rap->x1; x++) {
            const int32_t cov = rap->acc[x] > 256 ? 256 : rap->acc[x];
            if (cov > 0) {
                sg_blend_fix(cv, x, y, c, (cov * a256) >> 8);
            }
        }
    }
}

void sg_stroke_poly(sg_canvas_t *cv, const float *xy, int n, bool closed, float width, sg_rgb_t c, float alpha)
{
    float (*const sa)[9] = cv->scratch->segs; /* ax, ay, dx, dy, inv_len2, ymin, ymax, xmin, xmax */
    int *const act = cv->scratch->act;
    const int m = closed ? n : n - 1;
    if (m < 1) {
        return;
    }
    const float hw = width * 0.5f;
    const sg_fix_t cf = sg_tofix(c);
    float bx0 = 1e9f, by0 = 1e9f, bx1 = -1e9f, by1 = -1e9f;
    int ns = 0;
    for (int i = 0; i < m && ns < SG_MAX_SEGS; i++) {
        const int j = (i + 1) % n;
        const float ax = xy[2 * i], ay = xy[2 * i + 1], ex = xy[2 * j], ey = xy[2 * j + 1];
        const float dx = ex - ax, dy = ey - ay, l2 = dx * dx + dy * dy;
        sa[ns][0] = ax;
        sa[ns][1] = ay;
        sa[ns][2] = dx;
        sa[ns][3] = dy;
        sa[ns][4] = l2 > 1e-9f ? 1.0f / l2 : 0.0f;
        sa[ns][5] = sg_minf(ay, ey) - hw - 1.0f;
        sa[ns][6] = sg_maxf(ay, ey) + hw + 1.0f;
        sa[ns][7] = sg_minf(ax, ex) - hw - 1.0f;
        sa[ns][8] = sg_maxf(ax, ex) + hw + 1.0f;
        bx0 = sg_minf(bx0, sg_minf(ax, ex));
        bx1 = sg_maxf(bx1, sg_maxf(ax, ex));
        by0 = sg_minf(by0, sg_minf(ay, ey));
        by1 = sg_maxf(by1, sg_maxf(ay, ey));
        ns++;
    }
    const int x0 = imax(cv->clip_x0, (int)floorf(bx0 - hw - 1)), x1 = imin(cv->clip_x1, (int)ceilf(bx1 + hw + 1));
    const int y0 = imax(cv->clip_y0, (int)floorf(by0 - hw - 1)), y1 = imin(cv->clip_y1, (int)ceilf(by1 + hw + 1));
    for (int y = y0; y < y1; y++) {
        if (!sg_row_mine(cv, y)) continue;
        const float py = y + 0.5f;
        int na = 0;
        for (int i = 0; i < ns; i++) {
            if (py >= sa[i][5] && py <= sa[i][6]) {
                act[na++] = i;
            }
        }
        if (!na) {
            continue;
        }
        for (int x = x0; x < x1; x++) {
            const float px = x + 0.5f;
            float best = 1e9f;
            for (int k = 0; k < na; k++) {
                const float *s = sa[act[k]];
                if (px < s[7] || px > s[8]) continue;
                const float wx = px - s[0], wy = py - s[1];
                float t = (wx * s[2] + wy * s[3]) * s[4];
                t = t < 0 ? 0 : (t > 1 ? 1 : t);
                const float ex = wx - t * s[2], ey = wy - t * s[3];
                const float d2 = ex * ex + ey * ey;
                if (d2 < best) {
                    best = d2;
                }
            }
            if (best >= (hw + 0.5f) * (hw + 0.5f)) continue;
            const float cov = clamp01(hw + 0.5f - sqrtf(best));
            sg_blend_fix(cv, x, y, cf, sg_a256(alpha * cov));
        }
    }
}

static inline float rrect_dist(float px, float py, float cx, float cy, float hx, float hy, float r)
{
    const float qx = fabsf(px - cx) - (hx - r), qy = fabsf(py - cy) - (hy - r);
    const float mx = sg_maxf(qx, 0.0f), my = sg_maxf(qy, 0.0f);
    return sqrtf(mx * mx + my * my) + sg_minf(sg_maxf(qx, qy), 0.0f) - r;
}

void sg_fill_round_rect(sg_canvas_t *cv, float x, float y, float w, float h, float r, sg_rgb_t c, float alpha)
{
    const float cx = x + w * 0.5f, cy = y + h * 0.5f, hx = w * 0.5f, hy = h * 0.5f;
    const int x0 = imax(cv->clip_x0, (int)floorf(x)), x1 = imin(cv->clip_x1, (int)ceilf(x + w));
    const int y0 = imax(cv->clip_y0, (int)floorf(y)), y1 = imin(cv->clip_y1, (int)ceilf(y + h));
    const bool black = c.r == 0 && c.g == 0 && c.b == 0;
    const sg_fix_t cf = sg_tofix(c);
    for (int yy = y0; yy < y1; yy++) {
        if (!sg_row_mine(cv, yy)) continue;
        const uint16_t *row = &cv->px[yy * cv->stride];
        for (int xx = x0; xx < x1; xx++) {
            if (black && row[xx] == 0) continue;
            const float cov = clamp01(0.5f - rrect_dist(xx + 0.5f, yy + 0.5f, cx, cy, hx, hy, r));
            if (cov > 0) {
                sg_blend_fix(cv, xx, yy, cf, sg_a256(alpha * cov));
            }
        }
    }
}

void sg_stroke_round_rect(sg_canvas_t *cv, float x, float y, float w, float h, float r, float width, sg_rgb_t c, float alpha)
{
    const float cx = x + w * 0.5f, cy = y + h * 0.5f, hx = w * 0.5f, hy = h * 0.5f, hw = width * 0.5f;
    const sg_fix_t cf = sg_tofix(c);
    const int x0 = imax(cv->clip_x0, (int)floorf(x - hw - 1)), x1 = imin(cv->clip_x1, (int)ceilf(x + w + hw + 1));
    const int y0 = imax(cv->clip_y0, (int)floorf(y - hw - 1)), y1 = imin(cv->clip_y1, (int)ceilf(y + h + hw + 1));
    for (int yy = y0; yy < y1; yy++) {
        if (!sg_row_mine(cv, yy)) continue;
        const bool edge_row = (yy < y + hw + r + 2) || (yy > y + h - hw - r - 2);
        for (int xx = x0; xx < x1; xx++) {
            if (!edge_row && xx > x + hw + 2 && xx < x + w - hw - 2) {
                xx = (int)(x + w - hw - 2);
                continue;
            }
            const float d = rrect_dist(xx + 0.5f, yy + 0.5f, cx, cy, hx, hy, r);
            const float cov = clamp01(hw + 0.5f - fabsf(d));
            if (cov > 0) {
                sg_blend_fix(cv, xx, yy, cf, sg_a256(alpha * cov));
            }
        }
    }
}

void sg_path_init(sg_path_t *p, float *buf, int cap_points)
{
    p->xy = buf;
    p->n = 0;
    p->cap = cap_points;
}

void sg_path_move(sg_path_t *p, float x, float y)
{
    p->n = 0;
    sg_path_line(p, x, y);
}

void sg_path_line(sg_path_t *p, float x, float y)
{
    if (p->n < p->cap) {
        p->xy[2 * p->n] = x;
        p->xy[2 * p->n + 1] = y;
        p->n++;
    }
}

void sg_path_quad(sg_path_t *p, float cx, float cy, float x, float y, int segs)
{
    const float x0 = p->xy[2 * (p->n - 1)], y0 = p->xy[2 * (p->n - 1) + 1];
    for (int i = 1; i <= segs; i++) {
        const float t = (float)i / segs, u = 1.0f - t;
        sg_path_line(p, u * u * x0 + 2 * u * t * cx + t * t * x, u * u * y0 + 2 * u * t * cy + t * t * y);
    }
}

void sg_path_cubic(sg_path_t *p, float c1x, float c1y, float c2x, float c2y, float x, float y, int segs)
{
    const float x0 = p->xy[2 * (p->n - 1)], y0 = p->xy[2 * (p->n - 1) + 1];
    for (int i = 1; i <= segs; i++) {
        const float t = (float)i / segs, u = 1.0f - t;
        const float a = u * u * u, b = 3 * u * u * t, c = 3 * u * t * t, d = t * t * t;
        sg_path_line(p, a * x0 + b * c1x + c * c2x + d * x, a * y0 + b * c1y + c * c2y + d * y);
    }
}

void sg_path_arc(sg_path_t *p, float cx, float cy, float r, float a0, float a1, int segs)
{
    for (int i = 0; i <= segs; i++) {
        const float a = a0 + (a1 - a0) * i / segs;
        sg_path_line(p, cx + r * sg_cosf(a), cy + r * sg_sinf(a));
    }
}
