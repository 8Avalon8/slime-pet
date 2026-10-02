/*
 * sg: tiny anti-aliased software rasterizer for RGB565 framebuffers.
 * Pure C, no ESP-IDF dependency, so it builds on the host for testing.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SG_MAX_W 480

typedef struct {
    float r, g, b; /* 0..1 */
} sg_rgb_t;

#define SG_MAX_SEGS 256

/* Coverage accumulator shared by span-based fills (body, polygons). */
typedef struct {
    int32_t acc[SG_MAX_W + 2]; /* coverage, 256 = full pixel */
    int x0, x1;                /* touched range, half-open */
} sg_rowacc_t;

/* Fixed-point colour, 0..65535 per channel. The CPU is in-order with long FPU
 * latencies: a float pack costs ~115 cycles/px, the integer path a fraction. */
typedef struct {
    int32_t r, g, b;
} sg_fix_t;

/* Per-renderer scratch memory, so two cores can draw into one framebuffer at once. */
typedef struct {
    sg_rowacc_t ra;
    float segs[SG_MAX_SEGS][9];
    int act[SG_MAX_SEGS];
    /* row cache for smooth shading evaluated once per 2x2 block (slime body) */
    sg_fix_t shade[SG_MAX_W + 4];
    int shade_y, shade_x0, shade_x1;
    /* optional per-stage profiling, seconds (see sl_render_set_clock) */
    double prof[8];
} sg_scratch_t;

typedef struct {
    uint16_t *px;
    int w, h, stride;
    int clip_x0, clip_y0, clip_x1, clip_y1; /* half-open [x0, x1) */
    sg_scratch_t *scratch;
    /* split != 0: this canvas only owns 8-row bands where ((y >> 3) & 1) == owner */
    uint8_t split, owner;
} sg_canvas_t;

static inline bool sg_row_mine(const sg_canvas_t *cv, int y)
{
    return !cv->split || (((y >> 3) & 1) == cv->owner);
}

typedef enum {
    SG_PAINT_SOLID = 0,
    SG_PAINT_VGRAD3, /* vertical gradient: c0 at y0, c1 at y0+(y1-y0)*mid, c2 at y1 */
} sg_paint_kind_t;

typedef struct {
    sg_paint_kind_t kind;
    sg_rgb_t c0, c1, c2;
    float y0, y1, mid;
} sg_paint_t;

/* fminf/fmaxf in this libc call __issignalingf on every use; these compile to two instructions. */
static inline float sg_minf(float a, float b) { return a < b ? a : b; }
static inline float sg_maxf(float a, float b) { return a > b ? a : b; }

/* libm sinf costs ~2700 cycles on this target (measured); a Taylor polynomial on
 * [-pi/2, pi/2] after range reduction is accurate to ~4e-6 and ~30x cheaper. */
static inline float sg_sinf(float x)
{
    float k = x * 0.15915494f; /* turns */
    k -= (float)(int)(k + (k >= 0 ? 0.5f : -0.5f));
    x = k * 6.2831853f; /* [-pi, pi] */
    if (x > 1.5707963f) x = 3.1415927f - x;
    else if (x < -1.5707963f) x = -3.1415927f - x;
    const float x2 = x * x;
    return x * (1.0f + x2 * (-0.16666667f + x2 * (0.0083333310f + x2 * (-0.00019840874f + x2 * 2.7525562e-06f))));
}
static inline float sg_cosf(float x) { return sg_sinf(x + 1.5707963f); }

static inline sg_rgb_t sg_hex(uint32_t c)
{
    sg_rgb_t o = {((c >> 16) & 0xff) / 255.0f, ((c >> 8) & 0xff) / 255.0f, (c & 0xff) / 255.0f};
    return o;
}

extern const int32_t SG_DITHER[4][4]; /* ordered dither offsets, (bayer + 0.5) / 16 in 16.16 */

static inline int32_t sg_f16(float v) { return v <= 0 ? 0 : (v >= 1 ? 65535 : (int32_t)(v * 65535.0f)); }
static inline int32_t sg_a256(float a) { return a <= 0 ? 0 : (a >= 1 ? 256 : (int32_t)(a * 256.0f + 0.5f)); }

static inline sg_fix_t sg_tofix(sg_rgb_t c)
{
    sg_fix_t f = {sg_f16(c.r), sg_f16(c.g), sg_f16(c.b)};
    return f;
}

static inline uint16_t sg_pack_fix(sg_fix_t c, int x, int y)
{
    const int32_t d = SG_DITHER[y & 3][x & 3];
    return (uint16_t)((((c.r * 31 + d) >> 16) << 11) | (((c.g * 63 + d) >> 16) << 5) | ((c.b * 31 + d) >> 16));
}

static inline sg_fix_t sg_unpack_fix(uint16_t v)
{
    sg_fix_t f = {((v >> 11) & 31) * 2114, ((v >> 5) & 63) * 1040, (v & 31) * 2114};
    return f;
}

static inline sg_fix_t sg_lerp_fix(sg_fix_t a, sg_fix_t b, int32_t t256)
{
    a.r += ((b.r - a.r) * t256) >> 8;
    a.g += ((b.g - a.g) * t256) >> 8;
    a.b += ((b.b - a.b) * t256) >> 8;
    return a;
}

static inline sg_paint_t sg_solid(sg_rgb_t c)
{
    sg_paint_t p = {SG_PAINT_SOLID, c, c, c, 0, 1, 0.5f};
    return p;
}

static inline sg_rgb_t sg_lerp(sg_rgb_t a, sg_rgb_t b, float t)
{
    sg_rgb_t o = {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
    return o;
}

void sg_canvas_init(sg_canvas_t *cv, uint16_t *px, int w, int h, int stride);
/* Give the canvas its own scratch and make it own every other 8-row band (owner 0 or 1). */
void sg_canvas_set_split(sg_canvas_t *cv, sg_scratch_t *scratch, int owner);
void sg_set_clip(sg_canvas_t *cv, int x0, int y0, int x1, int y1);
void sg_reset_clip(sg_canvas_t *cv);

uint16_t sg_pack(sg_rgb_t c, int x, int y); /* ordered dither */
sg_rgb_t sg_unpack(uint16_t v);
void sg_blend(sg_canvas_t *cv, int x, int y, sg_rgb_t c, float a);

static inline void sg_blend_fix(sg_canvas_t *cv, int x, int y, sg_fix_t c, int32_t a256)
{
    if (a256 <= 0 || x < cv->clip_x0 || x >= cv->clip_x1 || y < cv->clip_y0 || y >= cv->clip_y1 || !sg_row_mine(cv, y)) {
        return;
    }
    uint16_t *p = &cv->px[y * cv->stride + x];
    *p = sg_pack_fix(a256 >= 256 ? c : sg_lerp_fix(sg_unpack_fix(*p), c, a256), x, y);
}
sg_rgb_t sg_paint_at(const sg_paint_t *p, float y);

void sg_fill_rect(sg_canvas_t *cv, int x0, int y0, int x1, int y1, uint16_t c);
void sg_dim_rect(sg_canvas_t *cv, int x0, int y0, int x1, int y1, float k);

void sg_fill_ellipse(sg_canvas_t *cv, float cx, float cy, float rx, float ry, const sg_paint_t *paint, float alpha);
void sg_stroke_ellipse(sg_canvas_t *cv, float cx, float cy, float rx, float ry, float width, sg_rgb_t c, float alpha);
/* Radial alpha blob: alpha a at centre, 0.55a at q=0.55, 0 at the rim; rotated ellipse. */
void sg_radial_blob(sg_canvas_t *cv, float cx, float cy, float rx, float ry, float rot, sg_rgb_t c, float a);

void sg_fill_poly(sg_canvas_t *cv, const float *xy, int n, const sg_paint_t *paint, float alpha);
void sg_stroke_poly(sg_canvas_t *cv, const float *xy, int n, bool closed, float width, sg_rgb_t c, float alpha);

void sg_fill_round_rect(sg_canvas_t *cv, float x, float y, float w, float h, float r, sg_rgb_t c, float alpha);
void sg_stroke_round_rect(sg_canvas_t *cv, float x, float y, float w, float h, float r, float width, sg_rgb_t c, float alpha);

/* Path builder over a caller-owned float buffer (x,y pairs). */
typedef struct {
    float *xy;
    int n, cap;
} sg_path_t;

void sg_path_init(sg_path_t *p, float *buf, int cap_points);
void sg_path_move(sg_path_t *p, float x, float y);
void sg_path_line(sg_path_t *p, float x, float y);
void sg_path_quad(sg_path_t *p, float cx, float cy, float x, float y, int segs);
void sg_path_cubic(sg_path_t *p, float c1x, float c1y, float c2x, float c2y, float x, float y, int segs);
void sg_path_arc(sg_path_t *p, float cx, float cy, float r, float a0, float a1, int segs);

void sg_rowacc_clear(sg_rowacc_t *r);
void sg_rowacc_span(sg_rowacc_t *r, float xa, float xb, float weight, int clip_x0, int clip_x1);

#ifdef __cplusplus
}
#endif
