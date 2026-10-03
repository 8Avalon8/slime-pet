#include "slime_bg.h"

#include <math.h>

#include "slime_render.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define HORIZON (SL_GROUND - 6) /* top of the grass; the pet stands a little below it */

typedef struct {
    uint32_t sky[3]; /* top, middle, horizon */
    float sky_mid;
    uint32_t far_hill, near_hill, grass, grass_edge;
    uint32_t orb, orb_glow; /* sun or moon */
    float orb_x, orb_y, orb_r, glow_a;
    uint32_t cloud;
    float cloud_a; /* 0 = no clouds */
    int stars;
} scene_def_t;

static const scene_def_t SCENES[SL_SCENE_COUNT] = {
    [SL_SCENE_DAWN] = {{0x3a4f8c, 0xd99ab6, 0xffd9a8}, 0.62f, 0xa48fb4, 0x6f8f86, 0x5d8a55, 0x86b06a,
                       0xfff0c8, 0xffc98a, 84, 296, 26, 0.55f, 0xffd8e2, 0.75f, 6},
    [SL_SCENE_DAY] = {{0x4c9fe6, 0x8fcff4, 0xdcf2ff}, 0.55f, 0x9ccfb0, 0x66b267, 0x4f9a42, 0x7cc65c,
                      0xfff6c4, 0xfff2a8, 384, 104, 28, 0.45f, 0xffffff, 0.92f, 0},
    [SL_SCENE_DUSK] = {{0x29306e, 0xa9557e, 0xff9c5c}, 0.58f, 0x6e4b72, 0x3f3b56, 0x3c4a3c, 0x55664a,
                       0xffc27a, 0xff8a4a, 380, 322, 32, 0.6f, 0xffb6a0, 0.6f, 10},
    [SL_SCENE_NIGHT] = {{0x040818, 0x0c1838, 0x1c2c58}, 0.6f, 0x17244a, 0x0f1a34, 0x0e1c22, 0x1e3532,
                        0xf3f0d6, 0xbcd0ff, 372, 104, 24, 0.35f, 0, 0, 70},
};

static const char *const NAMES[SL_SCENE_COUNT] = {"dawn", "day", "dusk", "night"};

sl_scene_t sl_scene_at(int hour)
{
    if (hour >= 5 && hour < 8) return SL_SCENE_DAWN;
    if (hour >= 8 && hour < 17) return SL_SCENE_DAY;
    if (hour >= 17 && hour < 19) return SL_SCENE_DUSK;
    return SL_SCENE_NIGHT;
}

const char *sl_scene_name(sl_scene_t s) { return (unsigned)s < SL_SCENE_COUNT ? NAMES[s] : "?"; }

/* deterministic, so the scene looks the same after every redraw */
static uint32_t rnd(uint32_t *s)
{
    *s = *s * 1664525u + 1013904223u;
    return *s >> 8;
}

/* A rolling ridge from x = 0 to SL_SCREEN, closed down to the horizon. */
static void hill(sg_canvas_t *cv, float base, float amp, float f1, float f2, float ph, uint32_t col)
{
    float buf[2 * 72];
    sg_path_t p;
    sg_path_init(&p, buf, 72);
    sg_path_move(&p, 0, HORIZON + 1);
    for (int x = 0; x <= SL_SCREEN; x += 8) {
        const float y = base - amp * (0.6f * sg_sinf(x * f1 + ph) + 0.4f * sg_sinf(x * f2 + ph * 2.3f));
        sg_path_line(&p, (float)x, y);
    }
    sg_path_line(&p, SL_SCREEN, HORIZON + 1);
    const sg_paint_t paint = sg_solid(sg_hex(col));
    sg_fill_poly(cv, p.xy, p.n, &paint, 1);
}

/* a1 moved by whole turns so that going from a0 to a1 passes the angle `via` */
static float sweep_through(float a0, float a1, float via)
{
    const float T = 2 * (float)M_PI;
    while (a1 < a0) a1 += T;
    while (via < a0) via += T;
    return via <= a1 ? a1 : a1 - T;
}

static void cloud(sg_canvas_t *cv, float x, float y, float s, sg_rgb_t c, float a)
{
    const sg_paint_t p = sg_solid(c);
    sg_fill_round_rect(cv, x - 34 * s, y - 2 * s, 68 * s, 16 * s, 8 * s, c, a);
    sg_fill_ellipse(cv, x - 14 * s, y, 18 * s, 15 * s, &p, a);
    sg_fill_ellipse(cv, x + 10 * s, y - 4 * s, 22 * s, 19 * s, &p, a);
}

void sl_bg_draw(sg_canvas_t *cv, sl_scene_t s)
{
    if ((unsigned)s >= SL_SCENE_COUNT) s = SL_SCENE_DAY;
    const scene_def_t *d = &SCENES[s];
    const float W = SL_SCREEN;

    /* sky */
    const sg_paint_t sky = {SG_PAINT_VGRAD3, sg_hex(d->sky[0]), sg_hex(d->sky[1]), sg_hex(d->sky[2]), 0, HORIZON, d->sky_mid};
    const float rect[8] = {0, 0, W, 0, W, HORIZON + 1, 0, HORIZON + 1};
    sg_fill_poly(cv, rect, 4, &sky, 1);

    /* stars, fewer and fainter towards the horizon */
    uint32_t seed = 0x51a7e + (uint32_t)s;
    for (int i = 0; i < d->stars; i++) {
        const float x = (float)(rnd(&seed) % SL_SCREEN), y = (float)(rnd(&seed) % (int)(HORIZON * 0.7f));
        const float r = 0.7f + (rnd(&seed) % 100) / 100.0f * (s == SL_SCENE_NIGHT ? 1.0f : 0.5f);
        const float a = (0.35f + (rnd(&seed) % 100) / 154.0f) * (1 - y / HORIZON);
        const sg_paint_t p = sg_solid(sg_hex(0xfff8e0));
        sg_fill_ellipse(cv, x, y, r, r, &p, a);
    }

    /* sun or moon with a soft glow */
    sg_radial_blob(cv, d->orb_x, d->orb_y, d->orb_r * 3.2f, d->orb_r * 3.2f, 0, sg_hex(d->orb_glow), d->glow_a);
    const sg_paint_t orb = sg_solid(sg_hex(d->orb));
    if (s != SL_SCENE_NIGHT) {
        sg_fill_ellipse(cv, d->orb_x, d->orb_y, d->orb_r, d->orb_r, &orb, 1);
    } else { /* crescent: the disc minus a slightly smaller one up and to the right */
        const float R = d->orb_r, r = R * 0.9f, dx = R * 0.45f, dy = -R * 0.25f;
        const float dd = sqrtf(dx * dx + dy * dy), a = (dd * dd + R * R - r * r) / (2 * dd), h = sqrtf(R * R - a * a);
        const float mx = a * dx / dd, my = a * dy / dd; /* chord midpoint, relative to the moon's centre */
        const float p1x = mx - h * dy / dd, p1y = my + h * dx / dd, p2x = mx + h * dy / dd, p2y = my - h * dx / dd;
        /* both arcs bulge away from the cut: the outer one from p1 to p2, the inner one back */
        const float away = atan2f(-dy, -dx), o1 = atan2f(p1y, p1x), i1 = atan2f(p2y - dy, p2x - dx);
        const float o2 = sweep_through(o1, atan2f(p2y, p2x), away), i2 = sweep_through(i1, atan2f(p1y - dy, p1x - dx), away);
        float buf[2 * 80];
        sg_path_t pt;
        sg_path_init(&pt, buf, 80);
        sg_path_arc(&pt, d->orb_x, d->orb_y, R, o1, o2, 44);
        sg_path_arc(&pt, d->orb_x + dx, d->orb_y + dy, r, i1, i2, 30);
        sg_fill_poly(cv, pt.xy, pt.n, &orb, 1);
    }

    /* clouds, away from the HUD (top left) and the pet's head */
    if (d->cloud_a > 0) {
        const sg_rgb_t c = sg_hex(d->cloud);
        cloud(cv, 96, 132, 1.0f, c, d->cloud_a);
        cloud(cv, 286, 64, 0.8f, c, d->cloud_a * 0.9f);
        if (s == SL_SCENE_DAY) cloud(cv, 420, 214, 0.7f, c, d->cloud_a * 0.85f);
    }

    /* hills: a pale far ridge, a darker near one */
    hill(cv, HORIZON - 58, 26, 0.011f, 0.027f, 0.8f, d->far_hill);
    hill(cv, HORIZON - 22, 16, 0.017f, 0.041f, 2.1f, d->near_hill);

    /* grass */
    const sg_paint_t grass = {SG_PAINT_VGRAD3, sg_hex(d->grass_edge), sg_hex(d->grass), sg_lerp(sg_hex(d->grass), (sg_rgb_t){0, 0, 0}, 0.35f),
                              HORIZON, SL_SCREEN, 0.12f};
    const float ground[8] = {0, HORIZON, W, HORIZON, W, W, 0, W};
    sg_fill_poly(cv, ground, 4, &grass, 1);
    /* tufts along the top edge */
    seed = 0x9a55 + (uint32_t)s;
    const sg_rgb_t tuft = sg_lerp(sg_hex(d->grass_edge), (sg_rgb_t){1, 1, 1}, s == SL_SCENE_NIGHT ? 0.05f : 0.2f);
    for (int i = 0; i < 26; i++) {
        const float x = 6 + i * 18.4f + (rnd(&seed) % 9), h = 4 + (rnd(&seed) % 5);
        const float t[6] = {x - 3, HORIZON + 1, x + 3, HORIZON + 1, x + (rnd(&seed) % 5) - 2.0f, HORIZON - h};
        const sg_paint_t p = sg_solid(tuft);
        sg_fill_poly(cv, t, 3, &p, 0.9f);
    }
}
