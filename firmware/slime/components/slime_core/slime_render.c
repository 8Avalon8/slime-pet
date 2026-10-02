#include "slime_render.h"

#include <math.h>
#include <string.h>

#include "slime_text.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define U 6.0f
#define W0 128.0f
#define H0 230.0f
#define PROF_N 1024

typedef struct {
    uint32_t hi, l, b, d, e, edge;
    uint32_t rim;
    float rim_a;
} pal_def_t;

static const pal_def_t PAL[SL_PAL_COUNT] = {
    [SL_PAL_BLUE] = {0xd4f2ff, 0x74c6ff, 0x2f88f0, 0x1a5bcc, 0x0d368c, 0x0a2c74, 0x82dcff, 0.55f},
    [SL_PAL_METAL] = {0xffffff, 0xe6ecf3, 0xaab5c3, 0x6d7887, 0x353d48, 0x262c35, 0xffffff, 0.40f},
    [SL_PAL_RED] = {0xffe0d4, 0xff8d75, 0xec4436, 0xb51e22, 0x650a10, 0x45060a, 0xffaf91, 0.50f},
    [SL_PAL_GOLD] = {0xfffbe6, 0xffe886, 0xf2bf1d, 0xc38905, 0x714b00, 0x523500, 0xfff2af, 0.55f},
};

static sg_rgb_t s_grad[SL_PAL_COUNT][256];
#define GLOW_LUT_N 72 /* gaussian glow alpha in half-pixel steps out to 36 px */
static int32_t s_glow_lut[GLOW_LUT_N];
static double (*s_clock)(void);

void sl_render_set_clock(double (*clock)(void)) { s_clock = clock; }

#define PROF_BEGIN double _pt = s_clock ? s_clock() : 0
#define PROF_LAP(stage)                                   \
    do {                                                  \
        if (s_clock) {                                    \
            const double _n = s_clock();                  \
            cv->scratch->prof[stage] += _n - _pt;          \
            _pt = _n;                                     \
        }                                                 \
    } while (0)
static float s_prof[PROF_N + 1];
static bool s_inited;

static inline float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }

/* ---- profile: measured half-width vs height, PCHIP-interpolated ---- */

static const float PV[] = {0, 0.0092f, 0.036f, 0.079f, 0.135f, 0.20f, 0.27f, 0.38f, 0.48f, 0.57f, 0.64f, 0.70f, 0.78f, 0.87f, 0.94f, 0.98f, 1.0f};
static const float PR[] = {0.45f, 0.592f, 0.725f, 0.839f, 0.926f, 0.981f, 1.0f, 0.97f, 0.84f, 0.64f, 0.46f, 0.32f, 0.21f, 0.145f, 0.10f, 0.055f, 0};
#define PN ((int)(sizeof(PV) / sizeof(PV[0])))

static float pchip_eval(float x)
{
    static float m[PN], h[PN], d[PN];
    static bool ready;
    if (!ready) {
        for (int i = 0; i < PN - 1; i++) {
            h[i] = PV[i + 1] - PV[i];
            d[i] = (PR[i + 1] - PR[i]) / h[i];
        }
        m[0] = d[0];
        m[PN - 1] = d[PN - 2];
        for (int i = 1; i < PN - 1; i++) {
            if (d[i - 1] * d[i] <= 0) {
                m[i] = 0;
            } else {
                const float w1 = 2 * h[i] + h[i - 1], w2 = h[i] + 2 * h[i - 1];
                m[i] = (w1 + w2) / (w1 / d[i - 1] + w2 / d[i]);
            }
        }
        ready = true;
    }
    if (x <= PV[0]) return PR[0];
    if (x >= PV[PN - 1]) return PR[PN - 1];
    int i = 0;
    while (x > PV[i + 1]) i++;
    const float t = (x - PV[i]) / h[i], t2 = t * t, t3 = t2 * t;
    return (2 * t3 - 3 * t2 + 1) * PR[i] + (t3 - 2 * t2 + t) * h[i] * m[i] + (-2 * t3 + 3 * t2) * PR[i + 1] + (t3 - t2) * h[i] * m[i + 1];
}

static inline float prof(float v)
{
    if (v <= 0) return s_prof[0];
    if (v >= 1) return 0;
    const float f = v * PROF_N;
    const int i = (int)f;
    return s_prof[i] + (s_prof[i + 1] - s_prof[i]) * (f - i);
}

void sl_render_init(void)
{
    if (s_inited) return;
    const float tip = pchip_eval(0.96f);
    for (int i = 0; i <= PROF_N; i++) {
        const float v = (float)i / PROF_N;
        s_prof[i] = v > 0.96f ? tip * sqrtf(sg_maxf(0, (1 - v) / 0.04f)) : pchip_eval(v);
    }
    static const float STOP[5] = {0, 0.14f, 0.48f, 0.8f, 1.0f};
    for (int p = 0; p < SL_PAL_COUNT; p++) {
        const sg_rgb_t c[5] = {sg_hex(PAL[p].hi), sg_hex(PAL[p].l), sg_hex(PAL[p].b), sg_hex(PAL[p].d), sg_hex(PAL[p].e)};
        for (int i = 0; i < 256; i++) {
            const float t = i / 255.0f;
            int k = 0;
            while (k < 3 && t > STOP[k + 1]) k++;
            s_grad[p][i] = sg_lerp(c[k], c[k + 1], (t - STOP[k]) / (STOP[k + 1] - STOP[k]));
        }
    }
    for (int i = 0; i < GLOW_LUT_N; i++) {
        const float d = (i + 0.5f) * 0.5f;
        s_glow_lut[i] = sg_a256(0.6f * expf(-(d * d) / 256.0f));
    }
    s_inited = true;
}

/* ---- body shape ---- */

typedef struct {
    float cx, by, h, sx, sway, curl, rip, phase;
} shape_t;

typedef struct {
    float x, y, r;
} at_t;

static inline at_t shape_at(const shape_t *s, float v)
{
    const float spread = 1 + (s->sx - 1) * (1.25f - 0.5f * v);
    const float rip = 1 + s->rip * sg_sinf(v * 9 - s->phase) * (0.3f + v);
    const float c = sg_maxf(0, (v - 0.6f) / 0.4f);
    at_t a = {s->cx + s->sway * W0 * v * v + s->curl * W0 * 0.16f * c * c, s->by - v * s->h, prof(v) * W0 * spread * rip};
    return a;
}

/* Span of the body at screen y (sub-row). Returns false when the row misses the body. */
static inline bool shape_span(const shape_t *s, float y, float *xc, float *r)
{
    const float v = (s->by - y) / s->h;
    if (v > 1) return false;
    if (v >= 0) {
        const at_t a = shape_at(s, v);
        *xc = a.x;
        *r = a.r;
        return *r > 0;
    }
    const float depth = y - s->by, maxd = 0.01f * s->h;
    if (depth >= maxd) return false;
    const at_t a = shape_at(s, 0);
    *xc = a.x;
    *r = a.r * sqrtf(1 - depth / maxd);
    return true;
}

/* ---- primitives local to the renderer ---- */

static void linear_radial_ellipse(sg_canvas_t *cv, float cx, float cy, float rx, float ry, sg_rgb_t c, float a)
{
    const sg_fix_t cf = sg_tofix(c);
    const int x0 = imax(cv->clip_x0, (int)floorf(cx - rx)), x1 = imin(cv->clip_x1, (int)ceilf(cx + rx));
    const int y0 = imax(cv->clip_y0, (int)floorf(cy - ry)), y1 = imin(cv->clip_y1, (int)ceilf(cy + ry));
    for (int y = y0; y < y1; y++) {
        if (!sg_row_mine(cv, y)) continue;
        const float dy = (y + 0.5f - cy) / ry;
        for (int x = x0; x < x1; x++) {
            const float dx = (x + 0.5f - cx) / rx;
            const float q2 = dx * dx + dy * dy;
            if (q2 < 1) {
                sg_blend_fix(cv, x, y, cf, sg_a256(a * (1 - sqrtf(q2))));
            }
        }
    }
}

typedef struct {
    float cx, cy, cs, sn, irx, iry, a, bx0, by0, bx1, by1;
} blob_t;

static void blob_make(blob_t *b, float cx, float cy, float rx, float ry, float rot, float a)
{
    const float R = sg_maxf(rx, ry);
    b->cx = cx;
    b->cy = cy;
    b->cs = sg_cosf(rot);
    b->sn = sg_sinf(rot);
    b->irx = 1 / rx;
    b->iry = 1 / ry;
    b->a = a;
    b->bx0 = cx - R;
    b->bx1 = cx + R;
    b->by0 = cy - R;
    b->by1 = cy + R;
}

static inline float blob_alpha(const blob_t *b, float px, float py)
{
    if (px < b->bx0 || px > b->bx1 || py < b->by0 || py > b->by1) return 0;
    const float dx = px - b->cx, dy = py - b->cy;
    const float lx = (dx * b->cs + dy * b->sn) * b->irx, ly = (-dx * b->sn + dy * b->cs) * b->iry;
    const float q2 = lx * lx + ly * ly;
    if (q2 >= 1) return 0;
    const float q = sqrtf(q2);
    return q < 0.55f ? b->a * (1 - 0.45f * q / 0.55f) : b->a * 0.55f * (1 - q) / 0.45f;
}

/* ---- body ---- */

static void draw_body(sg_canvas_t *cv, const sl_pose_t *P, const shape_t *s)
{
    sg_rowacc_t *const ra = &cv->scratch->ra;
    const pal_def_t *pd = &PAL[P->pal];
    const sg_rgb_t *lut = s_grad[P->pal];
    const sg_rgb_t white = {1, 1, 1}, rim = sg_hex(pd->rim);
    const sg_fix_t whitef = {65535, 65535, 65535}, edgef = sg_tofix(sg_hex(pd->edge));
    const float h = s->h, by = s->by, cx = s->cx;

    /* conic gradient: start circle at the highlight, end circle around the body */
    const at_t hl = shape_at(s, 0.6f);
    const float c0x = hl.x - 0.55f * hl.r, c0y = hl.y, r0 = 4;
    const float c1x = cx + 0.12f * W0, c1y = by - 0.36f * h, r1 = h * 1.05f;
    const float gdx = c1x - c0x, gdy = c1y - c0y, gdr = r1 - r0;
    const float gA = gdx * gdx + gdy * gdy - gdr * gdr, giA = 1 / gA;

    /* specular blobs, sampled inside the body shader so they are clipped by it */
    blob_t b1, b2;
    const at_t a1 = shape_at(s, 0.47f), a2 = shape_at(s, 0.82f), a3 = shape_at(s, 0.64f);
    blob_make(&b1, a1.x - 0.66f * a1.r, a1.y, 0.1f * W0, 0.19f * h, 0.5f, 0.9f);
    blob_make(&b2, a2.x - 0.4f * a2.r, a2.y, 0.03f * W0, 0.07f * h, 0.25f, 0.85f);
    const float dotx = a3.x - 0.5f * a3.r, doty = a3.y, dotr = 0.028f * W0;
    const float dot_lim2 = (dotr + 0.5f) * (dotr + 0.5f);

    const float rl_x1 = cx + W0 * s->sx, rl_x0 = cx + 0.55f * W0 * s->sx;
    const float sw_c = sg_cosf(0.6f), sw_s = sg_sinf(0.6f), sw_y = by - h / 2;
    const float sw_x0 = P->has_sweep ? -1.6f * W0 + (P->sweep + 30) / 110 * 3.2f * W0 : 0;

    sg_scratch_t *const sc = cv->scratch;
    sc->shade_y = -1000000; /* invalidate the block-shading cache for this frame */
    const int y0 = imax(cv->clip_y0, (int)floorf(by - h) - 1), y1 = imin(cv->clip_y1, (int)ceilf(by + 0.01f * h) + 1);
    for (int y = y0; y < y1; y++) {
        if (!sg_row_mine(cv, y)) continue;
        sg_rowacc_clear(ra);
        float sxc[4], sr[4];
        bool sok[4];
        for (int k = 0; k < 4; k++) {
            sok[k] = shape_span(s, y + (k + 0.5f) * 0.25f, &sxc[k], &sr[k]);
            if (sok[k]) {
                sg_rowacc_span(ra, sxc[k] - sr[k], sxc[k] + sr[k], 0.25f, cv->clip_x0, cv->clip_x1);
            }
        }
        if (ra->x1 <= ra->x0) continue;
        /* edge slope -> perpendicular distance factor for the soft outline */
        float xc = 0, r = 0;
        const bool mid = shape_span(s, y + 0.5f, &xc, &r);
        float kl = 0, kr = 0;
        if (sok[0] && sok[3]) {
            kr = fabsf((sxc[3] + sr[3]) - (sxc[0] + sr[0])) / 0.75f;
            kl = fabsf((sxc[3] - sr[3]) - (sxc[0] - sr[0])) / 0.75f;
        } else {
            kr = kl = 4;
        }
        const float pr = 1 / sqrtf(1 + kr * kr), pl = 1 / sqrtf(1 + kl * kl);
        /* outline fades out 1.5 px inside the edge; along steep slopes that is wider horizontally */
        const float edge_band = 1.6f / sg_minf(pr, pl);
        const float py = y + 0.5f;
        /* The base shading (gradient, rim, side light, speculars) is smooth: evaluate it
         * once per 2x2 block and reuse it for the odd row. Bands are 8 rows, so both rows
         * of a pair always belong to the same core. Coverage, outline, dot and dither stay
         * per pixel. */
        const int pair = y & ~1, bx0 = ra->x0 & ~1, bx1 = (ra->x1 + 1) & ~1;
        if (!P->flash && !(sc->shade_y == pair && sc->shade_x0 <= bx0 && sc->shade_x1 >= bx1)) {
            const float qy = pair + 1.0f;
            const float rim_a = pd->rim_a * clamp01((qy - (by - 0.3f * h)) / (0.3f * h));
            const float pdy = qy - c0y;
            for (int bx = bx0; bx < bx1; bx += 2) {
                const float qx = bx + 1.0f;
                const float pdx = qx - c0x;
                const float B = pdx * gdx + pdy * gdy + r0 * gdr, C = pdx * pdx + pdy * pdy - r0 * r0;
                const float disc = sg_maxf(0, B * B - gA * C);
                const float w = clamp01((B - sqrtf(disc)) * giA);
                sg_rgb_t c = lut[(int)(w * 255 + 0.5f)];
                if (rim_a > 0) c = sg_lerp(c, rim, rim_a);
                if (qx > rl_x0) c = sg_lerp(c, white, 0.2f * clamp01((qx - rl_x0) / (rl_x1 - rl_x0)));
                const float s1 = blob_alpha(&b1, qx, qy), s2 = blob_alpha(&b2, qx, qy);
                if (s1 > 0) c = sg_lerp(c, white, s1);
                if (s2 > 0) c = sg_lerp(c, white, s2);
                sc->shade[bx] = sg_tofix(c);
            }
            sc->shade_y = pair;
            sc->shade_x0 = bx0;
            sc->shade_x1 = bx1;
        }
        uint16_t *const row = &cv->px[y * cv->stride];
        /* per-row integer windows for the rare per-pixel extras */
        int ex_l = -1, ex_r = SG_MAX_W; /* outline zone: x < ex_l or x > ex_r */
        if (mid) {
            ex_l = (int)floorf(xc - r + edge_band);
            ex_r = (int)floorf(xc + r - edge_band);
        }
        int dot_x0 = 1, dot_x1 = 0;
        const float dyd = py - doty;
        if (dyd * dyd < dot_lim2) {
            const float hwx = sqrtf(dot_lim2 - dyd * dyd);
            dot_x0 = (int)floorf(dotx - hwx);
            dot_x1 = (int)ceilf(dotx + hwx);
        }
        int sw_x0i = 1, sw_x1i = 0;
        if (P->has_sweep) {
            /* lx = (px - cx) * cos - (py - sw_y) * sin, band lx in [sw_x0, sw_x0 + 0.12 W0) */
            const float off = (py - sw_y) * sw_s;
            sw_x0i = (int)ceilf(cx + (sw_x0 + off) / sw_c - 0.5f);
            sw_x1i = (int)ceilf(cx + (sw_x0 + 0.12f * W0 + off) / sw_c - 0.5f) - 1;
        }
        for (int x = ra->x0; x < ra->x1; x++) {
            const int32_t cov = ra->acc[x] > 256 ? 256 : ra->acc[x];
            if (cov <= 0) continue;
            if (P->flash) {
                sg_blend_fix(cv, x, y, whitef, cov);
                continue;
            }
            sg_fix_t c = sc->shade[x & ~1];
            if (x >= sw_x0i && x <= sw_x1i) c = sg_lerp_fix(c, whitef, 179);
            if (x >= dot_x0 && x <= dot_x1) {
                const float ddx = x + 0.5f - dotx, dd2 = ddx * ddx + dyd * dyd;
                if (dd2 < dot_lim2) c = sg_lerp_fix(c, whitef, sg_a256(0.95f * clamp01(dotr + 0.5f - sqrtf(dd2))));
            }
            if (x <= ex_l || x >= ex_r) {
                const float px = x + 0.5f;
                const float de = sg_minf((xc + r - px) * pr, (px - (xc - r)) * pl);
                const float ea = 0.45f * clamp01(1.5f - de);
                if (ea > 0) c = sg_lerp_fix(c, edgef, sg_a256(ea));
            }
            if (cov >= 256) {
                row[x] = sg_pack_fix(c, x, y); /* interior: row/clip ownership already guaranteed */
            } else {
                sg_blend_fix(cv, x, y, c, cov);
            }
        }
    }
}

static void draw_glow(sg_canvas_t *cv, const shape_t *s, sg_rgb_t col)
{
    const sg_fix_t cf = sg_tofix(col);
    const int y0 = imax(cv->clip_y0, (int)floorf(s->by - s->h) - 32), y1 = imin(cv->clip_y1, (int)ceilf(s->by) + 14);
    for (int y = y0; y < y1; y++) {
        if (!sg_row_mine(cv, y)) continue;
        const float py = y + 0.5f;
        const float vy = sg_minf(sg_maxf(py, s->by - s->h), s->by);
        float xc, r;
        if (!shape_span(s, vy, &xc, &r)) {
            const at_t a = shape_at(s, 0.999f);
            xc = a.x;
            r = 0;
        }
        const float oy = fabsf(py - vy);
        const int x0 = imax(cv->clip_x0, (int)floorf(xc - r - 32)), x1 = imin(cv->clip_x1, (int)ceilf(xc + r + 32));
        for (int x = x0; x < x1; x++) {
            const float ox = sg_maxf(0, fabsf(x + 0.5f - xc) - r);
            const float d = sqrtf(ox * ox + oy * oy);
            if (d <= 0) continue;
            const int di = (int)(d * 2);
            if (di < GLOW_LUT_N) sg_blend_fix(cv, x, y, cf, s_glow_lut[di]);
        }
    }
}

/* ---- face ---- */

static const sg_rgb_t INK = {0x12 / 255.0f, 0x14 / 255.0f, 0x20 / 255.0f};

static void stroke_quad(sg_canvas_t *cv, float x0, float y0, float cx, float cy, float x1, float y1, float w, sg_rgb_t c)
{
    float buf[2 * 20];
    sg_path_t p;
    sg_path_init(&p, buf, 20);
    sg_path_move(&p, x0, y0);
    sg_path_quad(&p, cx, cy, x1, y1, 16);
    sg_stroke_poly(cv, p.xy, p.n, false, w, c, 1);
}

static void stroke_line(sg_canvas_t *cv, float x0, float y0, float x1, float y1, float w, sg_rgb_t c, float a)
{
    const float xy[4] = {x0, y0, x1, y1};
    sg_stroke_poly(cv, xy, 2, false, w, c, a);
}

static void draw_eye(sg_canvas_t *cv, float x, float y, float rx, float ry, sl_eye_t type, const float look[2], float side,
                     const pal_def_t *pd, double T, float blink)
{
    float sc = 1;
    if (type == SL_EYE_OPEN && blink > 0.88f) {
        type = SL_EYE_CLOSED;
    } else if (blink > 0 && (type == SL_EYE_OPEN || type == SL_EYE_SAD)) {
        sc = sg_maxf(0.08f, 1 - blink);
    }
    if (type == SL_EYE_OPEN || type == SL_EYE_SAD || type == SL_EYE_HALF || type == SL_EYE_DIZZY) {
        const float ery = ry * sc;
        sg_paint_t paint = {SG_PAINT_VGRAD3, sg_hex(0xffffff), sg_hex(0xf2f5fa), sg_hex(0xc6d0de), y - ery, y + ery, 0.7f};
        sg_fill_ellipse(cv, x, y, rx, ery, &paint, 1);
        sg_stroke_ellipse(cv, x, y, rx, ery, 2.5f, INK, 1);
        if (type == SL_EYE_DIZZY) {
            float buf[2 * 72];
            sg_path_t p;
            sg_path_init(&p, buf, 72);
            const float ph = (float)fmod(T * 7, 2 * M_PI);
            for (float a = 0; a <= 4 * (float)M_PI; a += 0.2f) {
                const float r = a / (4 * (float)M_PI) * rx * 0.78f, an = a + ph;
                sg_path_line(&p, x + r * sg_cosf(an), y + r * sg_sinf(an) * ery / rx);
            }
            sg_stroke_poly(cv, p.xy, p.n, false, 2.5f, INK, 1);
            return;
        }
        const float pr = rx * 0.5f;
        const float px = x + look[0] * rx * 0.32f - side * rx * 0.06f;
        const float py = y + look[1] * ery * 0.3f + (type == SL_EYE_SAD ? ery * 0.22f : 0) + (type == SL_EYE_HALF ? ery * 0.2f : 0);
        sg_paint_t pupil = sg_solid(sg_hex(0x0c0d14));
        sg_fill_ellipse(cv, px, py, pr, pr * 1.08f * sc, &pupil, 1);
        sg_paint_t hl = sg_solid(sg_hex(0xffffff));
        sg_fill_ellipse(cv, px - pr * 0.35f, py - pr * 0.4f * sc, pr * 0.3f, pr * 0.3f * sc, &hl, 1);
        if (type == SL_EYE_HALF) {
            const int sx0 = cv->clip_x0, sy0 = cv->clip_y0, sx1 = cv->clip_x1, sy1 = cv->clip_y1;
            sg_set_clip(cv, sx0, sy0, sx1, imin(sy1, (int)floorf(y - ery * 0.05f)));
            sg_paint_t lid = sg_solid(sg_hex(pd->d));
            sg_fill_ellipse(cv, x, y, rx, ery, &lid, 1);
            sg_set_clip(cv, sx0, sy0, sx1, sy1);
            stroke_line(cv, x - rx, y - ery * 0.05f, x + rx, y - ery * 0.05f, 3, INK, 1);
            sg_stroke_ellipse(cv, x, y, rx, ery, 2.5f, INK, 1);
        }
        if (type == SL_EYE_SAD) {
            stroke_line(cv, x + side * rx * 0.95f, y - ery * 1.25f, x - side * rx * 0.75f, y - ery * 1.6f, 3.5f, INK, 1);
        }
    } else if (type == SL_EYE_CLOSED) {
        stroke_quad(cv, x - rx * 0.85f, y, x, y + ry * 0.6f, x + rx * 0.85f, y, 4, INK);
    } else if (type == SL_EYE_HAPPY) {
        stroke_quad(cv, x - rx * 0.85f, y + ry * 0.25f, x, y - ry * 0.85f, x + rx * 0.85f, y + ry * 0.25f, 4.5f, INK);
    } else if (type == SL_EYE_SQUEEZE) {
        const float s = -side;
        const float xy[6] = {x - s * rx * 0.7f, y - ry * 0.6f, x + s * rx * 0.6f, y, x - s * rx * 0.7f, y + ry * 0.6f};
        sg_stroke_poly(cv, xy, 3, false, 4.5f, INK, 1);
    }
}

static void grin(sg_canvas_t *cv, float x, float y, float mw, float mh)
{
    const sg_rgb_t lip = sg_hex(0x3a0508);
    float buf[2 * 40];
    sg_path_t p;
    sg_path_init(&p, buf, 40);
    sg_path_move(&p, x - mw, y);
    sg_path_quad(&p, x, y + mh * 0.5f, x + mw, y, 12);
    sg_path_cubic(&p, x + mw * 0.78f, y + mh * 1.75f, x - mw * 0.78f, y + mh * 1.75f, x - mw, y, 20);
    sg_paint_t paint = {SG_PAINT_VGRAD3, sg_hex(0x5e070d), sg_hex(0xb81d22), sg_hex(0xef4a3e), y, y + mh * 1.4f, 0.45f};
    sg_fill_poly(cv, p.xy, p.n, &paint, 1);
    sg_stroke_poly(cv, p.xy, p.n, true, 3, lip, 1);
    stroke_line(cv, x - mw, y, x - mw * 1.07f, y - mh * 0.35f, 3, lip, 1);
    stroke_line(cv, x + mw, y, x + mw * 1.07f, y - mh * 0.35f, 3, lip, 1);
}

static void draw_mouth(sg_canvas_t *cv, float x, float y, float w, float h, sl_mouth_t type, double T)
{
    const sg_rgb_t lip = sg_hex(0x3a0508);
    switch (type) {
    case SL_MOUTH_GRIN: grin(cv, x, y, w, h); break;
    case SL_MOUTH_BIG: grin(cv, x, y, w, h * 2.1f); break;
    case SL_MOUTH_SMALL: grin(cv, x, y, w * 0.4f, h * 0.8f); break;
    case SL_MOUTH_O: {
        sg_paint_t in = sg_solid(sg_hex(0x8e1219));
        sg_fill_ellipse(cv, x, y + h * 0.7f, w * 0.18f, h * 0.85f, &in, 1);
        sg_stroke_ellipse(cv, x, y + h * 0.7f, w * 0.18f, h * 0.85f, 3, lip, 1);
        break;
    }
    case SL_MOUTH_FLAT: stroke_line(cv, x - w * 0.42f, y + h * 0.45f, x + w * 0.42f, y + h * 0.45f, 3.5f, lip, 1); break;
    case SL_MOUTH_WAVY: {
        float xy[2 * 21];
        for (int i = 0; i <= 20; i++) {
            const float u = i / 20.0f;
            xy[2 * i] = x - w * 0.5f + u * w;
            xy[2 * i + 1] = y + h * 0.5f + sg_sinf(u * (float)M_PI * 4) * h * 0.25f;
        }
        sg_stroke_poly(cv, xy, 21, false, 3.5f, lip, 1);
        break;
    }
    case SL_MOUTH_CHOMP:
        if ((long)floor(T * 5) % 2) {
            grin(cv, x, y, w, h * 1.4f);
        } else {
            stroke_quad(cv, x - w * 0.6f, y + h * 0.3f, x, y + h, x + w * 0.6f, y + h * 0.3f, 3.5f, lip);
        }
        break;
    }
}

/* ---- effects ---- */

static const float FXS[][2] = {
    [SL_FX_HEART] = {7, 6}, [SL_FX_BANG] = {2, 7}, [SL_FX_Z] = {4, 4}, [SL_FX_SPARKLE] = {5, 5},
    [SL_FX_STAR] = {3, 3}, [SL_FX_DROP] = {3, 5}, [SL_FX_BOLT] = {5, 6}, [SL_FX_DOT] = {0, 0},
};

static void draw_fx(sg_canvas_t *cv, const sl_fx_t *f)
{
    const sg_rgb_t col = sg_hex(f->rgb), ol = {0, 0, 0};
    const float OA = 0.55f;
    if (f->kind == SL_FX_DOT) {
        sg_paint_t p = sg_solid(col);
        sg_fill_ellipse(cv, f->x * U, f->y * U, f->r * U, f->r * U, &p, f->alpha);
        return;
    }
    const float ox = (f->x + FXS[f->kind][0] / 2) * U, oy = (f->y + FXS[f->kind][1] / 2) * U;
    float buf[2 * 64];
    sg_path_t p;
    sg_path_init(&p, buf, 64);
    sg_paint_t paint = sg_solid(col);
    switch (f->kind) {
    case SL_FX_HEART:
        sg_path_move(&p, ox, oy + 14);
        sg_path_cubic(&p, ox - 26, oy - 4, ox - 12, oy - 22, ox, oy - 9, 16);
        sg_path_cubic(&p, ox + 12, oy - 22, ox + 26, oy - 4, ox, oy + 14, 16);
        sg_fill_poly(cv, p.xy, p.n, &paint, 1);
        sg_stroke_poly(cv, p.xy, p.n, true, 2.5f, ol, OA);
        break;
    case SL_FX_SPARKLE: {
        const float r = 17, k = r * 0.18f;
        sg_path_move(&p, ox, oy - r);
        sg_path_quad(&p, ox + k, oy - k, ox + r, oy, 8);
        sg_path_quad(&p, ox + k, oy + k, ox, oy + r, 8);
        sg_path_quad(&p, ox - k, oy + k, ox - r, oy, 8);
        sg_path_quad(&p, ox - k, oy - k, ox, oy - r, 8);
        sg_fill_poly(cv, p.xy, p.n, &paint, 1);
        break;
    }
    case SL_FX_STAR:
        for (int i = 0; i < 10; i++) {
            const float a = -(float)M_PI / 2 + i * (float)M_PI / 5, rr = (i % 2) ? 11 * 0.45f : 11;
            sg_path_line(&p, ox + sg_cosf(a) * rr, oy + sg_sinf(a) * rr);
        }
        sg_fill_poly(cv, p.xy, p.n, &paint, 1);
        sg_stroke_poly(cv, p.xy, p.n, true, 2.5f, ol, OA);
        break;
    case SL_FX_Z: {
        const float xy[8] = {ox - 8, oy - 9, ox + 8, oy - 9, ox - 8, oy + 9, ox + 8, oy + 9};
        sg_stroke_poly(cv, xy, 4, false, 7.5f, ol, OA);
        sg_stroke_poly(cv, xy, 4, false, 5, col, 1);
        break;
    }
    case SL_FX_BANG: {
        stroke_line(cv, ox, oy - 17, ox, oy + 5, 13, ol, OA);
        sg_paint_t o = sg_solid(ol);
        sg_fill_ellipse(cv, ox, oy + 15, 7, 7, &o, OA);
        stroke_line(cv, ox, oy - 17, ox, oy + 5, 8, col, 1);
        sg_fill_ellipse(cv, ox, oy + 15, 4.5f, 4.5f, &paint, 1);
        break;
    }
    case SL_FX_DROP: {
        sg_path_move(&p, ox, oy - 16);
        sg_path_quad(&p, ox + 10, oy - 2, ox + 10, oy + 5, 8);
        sg_path_arc(&p, ox, oy + 5, 10, 0, (float)M_PI, 16);
        sg_path_quad(&p, ox - 10, oy - 2, ox, oy - 16, 8);
        sg_fill_poly(cv, p.xy, p.n, &paint, 1);
        sg_stroke_poly(cv, p.xy, p.n, true, 2, ol, OA);
        sg_paint_t w = sg_solid(sg_hex(0xffffff));
        sg_fill_ellipse(cv, ox - 3.5f, oy + 3, 2.5f, 4, &w, 0.85f);
        break;
    }
    case SL_FX_BOLT: {
        static const float B[6][2] = {{6, -22}, {-10, 2}, {-1, 2}, {-6, 22}, {10, -4}, {1, -4}};
        for (int i = 0; i < 6; i++) sg_path_line(&p, ox + B[i][0], oy + B[i][1]);
        sg_fill_poly(cv, p.xy, p.n, &paint, 1);
        sg_stroke_poly(cv, p.xy, p.n, true, 2.5f, ol, OA);
        break;
    }
    default: break;
    }
}

/* ---- public ---- */

static void make_shape(const sl_pose_t *P, shape_t *s)
{
    s->cx = 240 + P->dx * U;
    s->by = SL_GROUND - P->jump * U;
    s->h = H0 * P->sy;
    s->sx = P->sx;
    s->sway = P->sway;
    s->curl = P->curl;
    s->rip = P->rip;
    s->phase = (float)fmod(P->T * 13, 2 * M_PI);
}

sl_rect_t sl_render_bounds(const sl_pose_t *P)
{
    shape_t s;
    make_shape(P, &s);
    const float m = P->has_glow ? 34 : 4;
    const float hw = W0 * (1 + fabsf(P->sx - 1) * 1.25f) * (1 + P->rip) + fabsf(P->sway) * W0 + m;
    const float sr = sg_maxf(30, (W0 + 14) * P->sx * (1 - P->jump / 30)) + P->puddle * 60;
    sl_rect_t r = {(int)floorf(s.cx - sg_maxf(hw, sr + 2)), (int)floorf(s.by - s.h - m), (int)ceilf(s.cx + sg_maxf(hw, sr + 2) + 1),
                   (int)ceilf(sg_maxf(s.by + m, SL_GROUND + 3 + 0.12f * sr + 2))};
    for (int i = 0; i < P->nfx; i++) {
        const sl_fx_t *f = &P->fx[i];
        const float fx = f->kind == SL_FX_DOT ? f->x * U : (f->x + FXS[f->kind][0] / 2) * U;
        const float fy = f->kind == SL_FX_DOT ? f->y * U : (f->y + FXS[f->kind][1] / 2) * U;
        const sl_rect_t e = {(int)fx - 30, (int)fy - 30, (int)fx + 31, (int)fy + 31};
        r = sl_rect_union(r, e);
    }
    r.x0 = imax(0, r.x0 & ~1);
    r.y0 = imax(0, r.y0 & ~1);
    r.x1 = imin(SL_SCREEN, (r.x1 + 1) & ~1);
    r.y1 = imin(SL_SCREEN, (r.y1 + 1) & ~1);
    return r;
}

sl_rect_t sl_render_slime(sg_canvas_t *cv, const sl_pose_t *P)
{
    sl_render_init();
    shape_t s;
    make_shape(P, &s);
    const pal_def_t *pd = &PAL[P->pal];
    PROF_BEGIN;

    const float sr = sg_maxf(30, (W0 + 14) * P->sx * (1 - P->jump / 30)) + P->puddle * 60;
    linear_radial_ellipse(cv, s.cx, SL_GROUND + 3, sr, sr * 0.12f, sg_hex(pd->rim), pd->rim_a);
    PROF_LAP(SL_PROF_SHADOW);

    if (P->puddle > 0) {
        const float prx = W0 * P->sx * (0.85f + 0.2f * P->puddle), pry = 8 + 16 * P->puddle;
        sg_paint_t pg = {SG_PAINT_VGRAD3, sg_hex(pd->l), sg_lerp(sg_hex(pd->l), sg_hex(pd->d), 0.5f), sg_hex(pd->d),
                         s.by - pry * 1.5f, s.by + pry * 0.5f, 0.5f};
        sg_fill_ellipse(cv, s.cx, s.by - pry * 0.5f, prx, pry, &pg, 1);
    }
    if (P->has_glow && !P->flash) {
        draw_glow(cv, &s, sg_hex(P->glow_rgb));
    }
    PROF_LAP(SL_PROF_GLOW);
    draw_body(cv, P, &s);
    PROF_LAP(SL_PROF_BODY);
    if (!P->flash) {
        const at_t e = shape_at(&s, 0.42f);
        const float er = 0.19f * W0, erx = er * sqrtf(P->sx), ery = er * 1.05f * sqrtf(P->sy), ex = 0.27f * W0 * sqrtf(P->sx);
        draw_eye(cv, e.x - ex, e.y, erx, ery, P->eyes, P->look, -1, pd, P->T, P->blink);
        draw_eye(cv, e.x + ex, e.y, erx, ery, P->eyes, P->look, 1, pd, P->T, P->blink);
        const at_t m = shape_at(&s, 0.25f);
        draw_mouth(cv, m.x, m.y, 0.54f * W0 * sqrtf(P->sx), 0.12f * W0 * sqrtf(P->sy), P->mouth, P->T);
    }
    PROF_LAP(SL_PROF_FACE);
    for (int i = 0; i < P->nfx; i++) {
        draw_fx(cv, &P->fx[i]);
    }
    PROF_LAP(SL_PROF_FX);
    return sl_render_bounds(P);
}

static void win(sg_canvas_t *cv, float x, float y, float w, float h)
{
    const sg_rgb_t k = {0, 0, 0}, wh = {1, 1, 1};
    sg_fill_round_rect(cv, x, y, w, h, 8, k, 0.88f);
    sg_stroke_round_rect(cv, x, y, w, h, 8, 3, wh, 1);
}

void sl_render_hud(sg_canvas_t *cv, int lv, float hp)
{
    const sg_rgb_t wh = {1, 1, 1};
    win(cv, 16, 14, 168, 64);
    sl_text_draw(cv, SL_TR("史莱姆", "Slime"), 30, 40, SL_FONT_18, wh, -1);
    char buf[16];
    buf[0] = 'L';
    buf[1] = 'v';
    buf[2] = ' ';
    int n = 3, v = lv < 0 ? 0 : lv;
    char tmp[8];
    int k = 0;
    do {
        tmp[k++] = (char)('0' + v % 10);
        v /= 10;
    } while (v && k < 7);
    while (k) buf[n++] = tmp[--k];
    buf[n] = 0;
    sl_text_draw(cv, buf, 116, 40, SL_FONT_18, wh, -1);
    sl_text_draw(cv, "HP", 30, 66, SL_FONT_14, wh, -1);
    const float bar[10] = {58, 55, 168, 55, 168, 66, 58, 66, 58, 55};
    sg_stroke_poly(cv, bar, 5, false, 1.5f, wh, 1);
    const sg_rgb_t hc = hp < 6 ? sg_hex(0xff5050) : sg_hex(0x6ee36e);
    const float bw = 106 * clamp01(hp / 20);
    if (bw > 0) sg_fill_round_rect(cv, 60, 57, bw, 7, 0.01f, hc, 1);
}

void sl_render_dialog(sg_canvas_t *cv, const char *utf8, int visible_chars, bool cursor)
{
    const sg_rgb_t wh = {1, 1, 1};
    win(cv, 16, 386, 448, 80);
    /* "status\ndetail": status line in the large font, detail (tool, file, command) below */
    sg_set_clip(cv, 30, 390, 430, 462);
    const char *nl = strchr(utf8, '\n');
    if (!nl) {
        sl_text_draw(cv, utf8, 36, 434, SL_FONT_22, wh, visible_chars);
    } else {
        char head[96];
        const size_t hn = (size_t)(nl - utf8) < sizeof head - 1 ? (size_t)(nl - utf8) : sizeof head - 1;
        memcpy(head, utf8, hn);
        head[hn] = 0;
        const int n1 = sl_text_draw(cv, head, 36, 420, SL_FONT_22, wh, visible_chars);
        const int v2 = visible_chars < 0 ? -1 : (visible_chars > n1 + 1 ? visible_chars - n1 - 1 : 0);
        sl_text_draw(cv, nl + 1, 37, 451, SL_FONT_18, sg_hex(0xa9c9ff), v2);
    }
    sg_reset_clip(cv);
    if (cursor) {
        const float tri[6] = {434, 444, 446, 444, 440, 452};
        sg_paint_t p = sg_solid(wh);
        sg_fill_poly(cv, tri, 3, &p, 1);
    }
}
