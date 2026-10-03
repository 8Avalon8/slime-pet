#include "slime_anim.h"

#include "sg.h"
#include "slime_text.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const float DUR[SL_STATE_COUNT] = {
    [SL_GREET] = 2.6f, [SL_POKE_L] = 1.8f, [SL_POKE_R] = 1.8f, [SL_DIZZY] = 3.5f, [SL_LEVELUP] = 3.0f, [SL_HURT] = 2.4f,
    [SL_SULK] = 2.4f,
};

static const char *NAMES[SL_STATE_COUNT] = {
    "idle", "greet", "pokeL", "pokeR", "dizzy", "sleep", "think", "wait", "levelup", "hurt", "charge", "melt", "metal", "work", "sulk",
};

float sl_state_duration(sl_state_t s) { return DUR[s]; }
const char *sl_state_name(sl_state_t s) { return NAMES[s]; }

static inline double fmodp(double a, double b) { return fmod(a, b); }

static void base(sl_pose_t *S)
{
    memset(S, 0, sizeof(*S));
    S->sx = S->sy = 1.0f;
    S->curl = 0.6f;
    S->dim = 1.0f;
    S->pal = SL_PAL_BLUE;
    S->eyes = SL_EYE_OPEN;
    S->mouth = SL_MOUTH_GRIN;
}

static void fx(sl_pose_t *S, sl_fx_kind_t k, float x, float y, uint32_t rgb)
{
    if (S->nfx < SL_MAX_FX) {
        sl_fx_t *f = &S->fx[S->nfx++];
        f->kind = k;
        f->x = x;
        f->y = y;
        f->r = 0;
        f->rgb = rgb;
        f->alpha = 1.0f;
    }
}

static void breathe(sl_pose_t *S, double T, double sp)
{
    const double ph = T * sp;
    S->sy = (float)(1 + 0.04 * sin(ph));
    S->sx = (float)(1 - 0.032 * sin(ph));
}

static float blink_at(double T, double per)
{
    const double bt = fmodp(T, per);
    const long cyc = (long)floor(T / per);
    double b = bt < 0.22 ? sin(bt / 0.22 * M_PI) : 0;
    if (cyc % 3 == 2 && bt > 0.32 && bt < 0.54) {
        b = sin((bt - 0.32) / 0.22 * M_PI);
    }
    return (float)b;
}

static void hop_vals(double t, double period, double h, float *jump, float *sx, float *sy)
{
    const double u = fmodp(t, period) / period;
    *jump = 0;
    if (u < 0.2) {
        const double k = sin(u / 0.2 * M_PI);
        *sx = (float)(1 + 0.16 * k);
        *sy = (float)(1 - 0.18 * k);
    } else if (u < 0.8) {
        const double a = (u - 0.2) / 0.6;
        *jump = (float)(h * 4 * a * (1 - a));
        *sx = 0.92f;
        *sy = 1.1f;
    } else {
        const double k = sin((u - 0.8) / 0.2 * M_PI);
        *sx = (float)(1 + 0.18 * k);
        *sy = (float)(1 - 0.2 * k);
    }
}

static void hop(sl_pose_t *S, double t, double period, double h) { hop_vals(t, period, h, &S->jump, &S->sx, &S->sy); }

static void st_idle(sl_anim_t *a, sl_pose_t *S, float t, double T, float dt)
{
    (void)a; (void)t; (void)dt;
    breathe(S, T, 2.2);
    S->sway = (float)(0.06 * sin(T * 1.1));
    const double lt = fmodp(T, 7);
    if (lt > 5 && lt < 5.7) {
        hop(S, lt - 5, 0.7, 6);
    }
    S->blink = blink_at(T, 3.6);
    static const float LOOK[5][2] = {{0, 0}, {-1, 0}, {0, 0}, {1, 0}, {0, -1}};
    const int li = (int)((long)floor(T / 2.2) % 5);
    S->look[0] = LOOK[li][0];
    S->look[1] = LOOK[li][1];
}

static void st_greet(sl_anim_t *a, sl_pose_t *S, float t, double T, float dt)
{
    (void)a; (void)dt;
    if (t < 1.86f) {
        hop(S, t, 0.62, 11);
    } else {
        breathe(S, T, 2.2);
    }
    S->eyes = SL_EYE_HAPPY;
    S->mouth = SL_MOUTH_BIG;
    if (t > 0.2f) {
        fx(S, SL_FX_HEART, 56 + sinf(t * 4) * 2, 22 - t * 5, 0xff5d8f);
    }
}

static void st_poke(sl_anim_t *a, sl_pose_t *S, float t, double T, float side)
{
    (void)a;
    const float s = expf(-4 * t) * cosf(10 * t);
    S->sx = 1 + 0.18f * s;
    S->sy = 1 - 0.18f * s;
    S->sway = -side * 0.22f * expf(-3 * t) * cosf(8 * t);
    if (t < 0.55f) {
        S->eyes = SL_EYE_SQUEEZE;
        S->mouth = SL_MOUTH_O;
    } else {
        S->look[0] = side;
        S->blink = blink_at(T, 3.6);
    }
    if (t < 0.35f) {
        fx(S, SL_FX_SPARKLE, 38 + side * 25, 40, 0xffffff);
    }
}

static void st_dizzy(sl_anim_t *a, sl_pose_t *S, float t, double T, float dt)
{
    (void)a; (void)t; (void)dt;
    S->sway = (float)(0.22 * sin(T * 6));
    S->sx = (float)(1 + 0.04 * sin(T * 9));
    S->sy = (float)(1 - 0.04 * sin(T * 9));
    S->eyes = SL_EYE_DIZZY;
    S->mouth = SL_MOUTH_WAVY;
    for (int k = 0; k < 3; k++) {
        const double an = T * 5 + k * 2.094;
        fx(S, SL_FX_STAR, (float)(39 + 16 * cos(an)), (float)(16 + 3.5 * sin(an)), 0xffd84a);
    }
}

static void st_sleep(sl_anim_t *a, sl_pose_t *S, float t, double T, float dt)
{
    (void)a; (void)t; (void)dt;
    S->sy = (float)(0.84 + 0.03 * sin(T * 1.4));
    S->sx = (float)(1.1 - 0.03 * sin(T * 1.4));
    S->curl = 0.25f;
    S->eyes = SL_EYE_CLOSED;
    S->mouth = SL_MOUTH_SMALL;
    S->dim = 0.55f;
    for (int k = 0; k < 3; k++) {
        const float u = (float)(fmodp(T + k * 0.9, 2.7) / 2.7);
        if (u < 0.92f) {
            fx(S, SL_FX_Z, 52 + u * 12, 24 - u * 16, 0xcfe0f5);
        }
    }
}

static void st_think(sl_anim_t *a, sl_pose_t *S, float t, double T, float dt)
{
    (void)a; (void)t; (void)dt;
    breathe(S, T, 1.6);
    S->sway = (float)(0.05 * sin(T * 1.5));
    S->look[0] = 0.8f;
    S->look[1] = -1.0f;
    S->mouth = SL_MOUTH_SMALL;
    S->blink = blink_at(T, 4.2);
    const int n = (int)floor(fmodp(T, 2) / 0.5);
    static const float D[3][3] = {{54, 20, 1.1f}, {59, 15, 1.5f}, {65, 9, 2.0f}};
    for (int i = 0; i < 3 && i < n; i++) {
        fx(S, SL_FX_DOT, D[i][0], D[i][1], 0xf0f4fa);
        S->fx[S->nfx - 1].r = D[i][2];
        S->fx[S->nfx - 1].alpha = 0.92f;
    }
}

static void st_wait(sl_anim_t *a, sl_pose_t *S, float t, double T, float dt)
{
    (void)a; (void)t; (void)dt;
    hop(S, T, 0.42, 4);
    S->mouth = SL_MOUTH_O;
    fx(S, SL_FX_BANG, 39, (float)(5 + round(sin(T * 8))), 0xffd84a);
    fx(S, SL_FX_DROP, 57, (float)(26 + fmodp(T, 1) * 6), 0x9fd6ff);
}

static void st_levelup(sl_anim_t *a, sl_pose_t *S, float t, double T, float dt)
{
    (void)dt;
    if (t < 0.25f) {
        const float k = sinf(t / 0.25f * (float)M_PI / 2);
        S->sx = 1 + 0.2f * k;
        S->sy = 1 - 0.22f * k;
    } else if (t < 0.95f) {
        const float u = (t - 0.25f) / 0.7f;
        S->jump = 12 * 4 * u * (1 - u);
        S->sx = 0.88f;
        S->sy = 1.14f;
        S->pal = ((int)floorf(t * 9) % 2) ? SL_PAL_GOLD : SL_PAL_BLUE;
    } else if (t < 1.25f) {
        const float k = sinf((t - 0.95f) / 0.3f * (float)M_PI);
        S->sx = 1 + 0.22f * k;
        S->sy = 1 - 0.24f * k;
    } else {
        breathe(S, T, 2.2);
    }
    if (t >= 0.95f && !a->flag_up) {
        a->flag_up = true;
        a->lv++;
    }
    S->eyes = SL_EYE_HAPPY;
    S->mouth = SL_MOUTH_BIG;
    if (t > 0.3f && t < 1.8f) {
        const float r = 8 + (t - 0.3f) * 26;
        for (int k = 0; k < 8; k++) {
            const float an = k * (float)M_PI / 4 + 0.3f;
            fx(S, SL_FX_SPARKLE, 38 + r * cosf(an), 38 - r * 0.8f * sinf(an), (k % 2) ? 0xffd84a : 0xffffff);
        }
    }
}

static void st_hurt(sl_anim_t *a, sl_pose_t *S, float t, double T, float dt)
{
    (void)T; (void)dt;
    if (!a->flag_hit) {
        a->flag_hit = true;
        if (!a->hp_external) a->hp = sg_maxf(1, a->hp - 3);
    }
    if (t < 0.5f) {
        S->dx = (((int)floorf(t * 30) % 2) ? 1.0f : -1.0f) * 2;
        S->flash = ((int)floorf(t * 16) % 2) == 0;
    }
    S->pal = (t > 0.4f && t < 1.5f) ? SL_PAL_RED : SL_PAL_BLUE;
    S->sy = 1 - 0.1f * expf(-3 * t);
    S->eyes = t < 0.7f ? SL_EYE_SQUEEZE : SL_EYE_SAD;
    S->mouth = SL_MOUTH_WAVY;
    if (t > 0.7f) {
        fx(S, SL_FX_DROP, 51, 42 + fmodf(t - 0.7f, 0.9f) * 10, 0x9fd6ff);
    }
}

static void st_sulk(sl_anim_t *a, sl_pose_t *S, float t, double T, float dt)
{
    (void)a; (void)dt;
    breathe(S, T, 3.5);
    const float d = sg_minf(1, t / 0.35f); /* deflate a little */
    S->sy *= 1 - 0.1f * d;
    S->sx *= 1 + 0.07f * d;
    S->eyes = SL_EYE_SAD;
    S->mouth = SL_MOUTH_WAVY;
    if (t > 0.5f) fx(S, SL_FX_DROP, 51, 42 + fmodf(t - 0.5f, 0.9f) * 10, 0x9fd6ff);
}

static void st_charge(sl_anim_t *a, sl_pose_t *S, float t, double T, float dt)
{
    (void)t;
    breathe(S, T, 3);
    if (!a->hp_external) a->hp = sg_minf(20, a->hp + dt * 2);
    if ((long)floor(T * 2.5) % 2) {
        S->has_glow = true;
        S->glow_rgb = 0xffe066;
    }
    S->eyes = SL_EYE_HAPPY;
    S->mouth = SL_MOUTH_CHOMP;
    fx(S, SL_FX_BOLT, 56, (float)(10 + round(sin(T * 4))), 0xffe066);
}

static void st_melt(sl_anim_t *a, sl_pose_t *S, float t, double T, float dt)
{
    const float m = sg_minf(1, t / 2.5f);
    S->sx = 1 + 0.32f * m;
    S->sy = (float)(1 - 0.48 * m + 0.02 * sin(T * 2));
    S->puddle = m;
    S->curl = 0.6f - 0.5f * m;
    S->eyes = SL_EYE_SAD;
    S->mouth = SL_MOUTH_WAVY;
    S->dim = 0.85f;
    if (!a->hp_external) a->hp = sg_maxf(2, a->hp - dt * 8);
    if (m > 0.4f) {
        for (int k = -1; k <= 1; k += 2) {
            const float u = (float)(fmodp(T + (k > 0 ? 0.6 : 0), 1.2) / 1.2);
            fx(S, SL_FX_DROP, 39 + k * roundf(21 * (1 + 0.32f * m) * 0.85f), sg_minf(57, 61 - 38 * (1 - 0.48f * m) * 0.45f + u * 12), 0x2f88f0);
        }
    }
}

static void st_metal(sl_anim_t *a, sl_pose_t *S, float t, double T, float dt)
{
    (void)a; (void)t; (void)dt;
    S->pal = SL_PAL_METAL;
    S->sy = (float)(1 + 0.01 * sin(T));
    S->eyes = SL_EYE_HALF;
    S->blink = blink_at(T, 6);
    S->mouth = SL_MOUTH_FLAT;
    S->has_sweep = true;
    S->sweep = (float)(fmodp(T, 3) / 3 * 110 - 30);
}

/* Busy: brisk little hops, eyes darting over the work, sweat and sparks. */
static void st_work(sl_anim_t *a, sl_pose_t *S, float t, double T, float dt)
{
    (void)a; (void)t; (void)dt;
    hop(S, T, 0.36, 3);
    S->sway = (float)(0.07 * sin(T * 8.7));
    S->look[0] = ((long)floor(T / 0.9) % 2) ? 0.7f : -0.7f;
    S->look[1] = 0.6f;
    S->mouth = SL_MOUTH_FLAT;
    S->blink = blink_at(T, 3.1);
    fx(S, SL_FX_DROP, 57, (float)(25 + fmodp(T, 0.8) / 0.8 * 7), 0x9fd6ff);
    const double ph = fmodp(T, 0.72);
    if (ph < 0.14 || (ph > 0.36 && ph < 0.5)) {
        const float side = ph < 0.36 ? -1.0f : 1.0f;
        fx(S, SL_FX_SPARKLE, 38 + side * 25, 52, side < 0 ? 0xffd84a : 0xffffff);
    }
}

void sl_anim_pose_raw(sl_anim_t *a, sl_state_t s, float t, double T, float dt, sl_pose_t *S)
{
    base(S);
    switch (s) {
    case SL_IDLE: st_idle(a, S, t, T, dt); break;
    case SL_GREET: st_greet(a, S, t, T, dt); break;
    case SL_POKE_L: st_poke(a, S, t, T, -1); break;
    case SL_POKE_R: st_poke(a, S, t, T, 1); break;
    case SL_DIZZY: st_dizzy(a, S, t, T, dt); break;
    case SL_SLEEP: st_sleep(a, S, t, T, dt); break;
    case SL_THINK: st_think(a, S, t, T, dt); break;
    case SL_WAIT: st_wait(a, S, t, T, dt); break;
    case SL_LEVELUP: st_levelup(a, S, t, T, dt); break;
    case SL_HURT: st_hurt(a, S, t, T, dt); break;
    case SL_CHARGE: st_charge(a, S, t, T, dt); break;
    case SL_MELT: st_melt(a, S, t, T, dt); break;
    case SL_METAL: st_metal(a, S, t, T, dt); break;
    case SL_WORK: st_work(a, S, t, T, dt); break;
    case SL_SULK: st_sulk(a, S, t, T, dt); break;
    default: break;
    }
    S->T = T;
    S->t = t;
}

void sl_anim_init(sl_anim_t *a, double now)
{
    memset(a, 0, sizeof(*a));
    a->lv = 12;
    a->hp = 18;
    a->mini_last = now - 1;
    sl_anim_set_state(a, SL_IDLE, now);
}

void sl_anim_set_state(sl_anim_t *a, sl_state_t s, double now)
{
    a->state = s;
    a->t0 = now;
    a->flag_up = a->flag_hit = false;
}

static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

#define SLIDE_DEAD 0.08f /* g; below this the slime stays put */
#define SLIDE_ACC 70.0f  /* grid units/s^2 per g */
#define SLIDE_MAX 11.0f  /* grid units from centre; keeps the body on screen */

static void jelly(sl_anim_t *a, sl_pose_t *S, float dt)
{
    if (!a->j_init) {
        a->j_init = true;
        a->j_sx = S->sx;
        a->j_sy = S->sy;
        a->j_vsx = a->j_vsy = a->j_vsw = 0;
        a->j_sw = S->sway;
        a->j_rip = 0;
        a->j_pj = S->jump;
        a->j_pdx = S->dx;
        a->j_lx = a->j_ly = a->j_vlx = a->j_vly = 0;
    }
    const float idt = 1.0f / sg_maxf(dt, 1e-3f);
    const float vj = (S->jump - a->j_pj) * idt, vdx = (S->dx - a->j_pdx) * idt + a->j_vtdx;
    a->j_pj = S->jump;
    a->j_pdx = S->dx;
    const float st = sg_minf(0.12f, fabsf(vj) * 0.0015f);
    const float tsy = S->sy * (1 + st), tsx = S->sx / (1 + st * 0.8f);
    /* tilt: dead zone (static friction), then the top leans downhill */
    const float ta = fabsf(a->ext_tilt) < SLIDE_DEAD ? 0 : a->ext_tilt - (a->ext_tilt > 0 ? SLIDE_DEAD : -SLIDE_DEAD);
    const float tsw = S->sway + clampf(-vdx * 0.004f, -0.25f, 0.25f) + clampf(ta * 0.2f, -0.12f, 0.12f) +
                      clampf(a->ext_sway, -0.3f, 0.3f);
    const float gx = a->ext_look_on ? a->ext_look_x : S->look[0], gy = a->ext_look_on ? a->ext_look_y : S->look[1];
    const float tlx = clampf(gx + a->j_vtdx * 0.06f, -1, 1), tly = clampf(gy, -1, 1);
    a->j_vsy += a->ext_kick_y;
    a->j_vsw += a->ext_kick_x * 0.5f;
    a->j_vtdx += a->ext_kick_x * 25;
    a->ext_kick_x = a->ext_kick_y = 0;
    const int n = (int)sg_maxf(1, ceilf(dt / 0.006f));
    const float h = dt / n;
    for (int i = 0; i < n; i++) {
        a->j_vsx += (-330 * (a->j_sx - tsx) - 11 * a->j_vsx) * h;
        a->j_sx += a->j_vsx * h;
        a->j_vsy += (-330 * (a->j_sy - tsy) - 11 * a->j_vsy) * h;
        a->j_sy += a->j_vsy * h;
        a->j_vsw += (-150 * (a->j_sw - tsw) - 5.5f * a->j_vsw) * h;
        a->j_sw += a->j_vsw * h;
        /* slide: gravity along the slope, viscous drag, slow walk back home when level */
        a->j_vtdx += (ta * SLIDE_ACC - 2.6f * a->j_vtdx - (ta == 0 ? 0.8f * a->j_tdx : 0)) * h;
        a->j_tdx += a->j_vtdx * h;
        if (fabsf(a->j_tdx) > SLIDE_MAX) {
            const float v = a->j_vtdx;
            a->j_tdx = a->j_tdx > 0 ? SLIDE_MAX : -SLIDE_MAX;
            if (v * a->j_tdx > 0) { /* hit the wall: squash sideways, top keeps going */
                a->j_vsx -= fabsf(v) * 0.03f;
                a->j_vsy += fabsf(v) * 0.025f;
                a->j_vsw += v * 0.04f;
                a->j_vtdx = -v * 0.2f;
            }
        }
        a->j_vlx += (-90 * (a->j_lx - tlx) - 15 * a->j_vlx) * h;
        a->j_lx += a->j_vlx * h;
        a->j_vly += (-90 * (a->j_ly - tly) - 15 * a->j_vly) * h;
        a->j_ly += a->j_vly * h;
    }
    const float tgt = sg_minf(0.045f, 0.004f + (fabsf(a->j_vsy) + fabsf(a->j_vsx) + fabsf(a->j_vsw) * 0.5f) * 0.012f);
    a->j_rip += (tgt - a->j_rip) * sg_minf(1, dt * (tgt > a->j_rip ? 12 : 2.5f));
    S->sx = a->j_sx;
    S->sy = a->j_sy;
    S->sway = a->j_sw;
    S->look[0] = a->j_lx;
    S->look[1] = a->j_ly;
    S->rip = a->j_rip;
    S->dx += a->j_tdx;
    for (int i = 0; i < S->nfx; i++) S->fx[i].x += a->j_tdx; /* effects ride along with the slide */
}

/* ---- helper slimes ---- */

#define MINI_SCALE 0.24f
#define MINI_IN_S 0.5f   /* pop out of the pet and land on its spot */
#define MINI_OUT_S 0.45f /* hop back into the pet */
#define MINI_GAP_S 0.18f /* between two arrivals or departures */
#define MINI_ARC_PX 120.0f
/* inner spots first; the outer two stand just behind them */
static const float MINI_X[SL_MAX_MINIS] = {76, 404, 30, 450};

static inline float lerpf(float a, float b, float u) { return a + (b - a) * u; }

void sl_anim_minis(sl_anim_t *a, double now, sl_pose_t *S)
{
    const int want = a->ext_minis < 0 ? 0 : (a->ext_minis > SL_MAX_MINIS ? SL_MAX_MINIS : a->ext_minis);
    int out = 0;
    for (int i = 0; i < SL_MAX_MINIS; i++) {
        if (a->mini_st[i] == 2 && now - a->mini_t[i] >= MINI_OUT_S) a->mini_st[i] = 0;
        out += a->mini_st[i] == 1;
    }
    if (out != want && now - a->mini_last >= MINI_GAP_S) {
        if (out < want) { /* the lowest free spot */
            for (int i = 0; i < SL_MAX_MINIS; i++) {
                if (a->mini_st[i] == 0) {
                    a->mini_st[i] = 1;
                    a->mini_t[i] = now;
                    a->mini_last = now;
                    break;
                }
            }
        } else { /* the outermost one goes home first */
            for (int i = SL_MAX_MINIS - 1; i >= 0; i--) {
                if (a->mini_st[i] == 1) {
                    a->mini_st[i] = 2;
                    a->mini_t[i] = now;
                    a->mini_last = now;
                    break;
                }
            }
        }
    }

    /* they leave from, and return to, the middle of the pet's body */
    const float ox = 240 + S->dx * 6, oj = S->jump * 6 + 0.35f * 230 * S->sy;
    const bool sleepy = a->state == SL_SLEEP || a->state == SL_MELT;
    const bool happy = a->state == SL_GREET || a->state == SL_LEVELUP;
    const double period = a->state == SL_WORK ? 0.42 : (a->state == SL_THINK || a->state == SL_WAIT ? 0.8 : 1.3);
    S->nmini = 0;
    for (int k = 0; k < SL_MAX_MINIS; k++) {
        const int i = SL_MAX_MINIS - 1 - k; /* outer spots first: the inner ones overlap them */
        if (!a->mini_st[i]) continue;
        sl_mini_t *m = &S->mini[S->nmini++];
        const float age = (float)(now - a->mini_t[i]), X = MINI_X[i];
        m->eyes = sleepy ? SL_EYE_CLOSED : (happy ? SL_EYE_HAPPY : SL_EYE_OPEN);
        m->look = ox > X ? 0.6f : -0.6f; /* they keep an eye on the big one */
        m->sx = 0.9f;
        m->sy = 1.12f;
        m->front = a->mini_st[i] == 2 || age < MINI_IN_S;
        if (a->mini_st[i] == 1 && age < MINI_IN_S) {
            const float u = age / MINI_IN_S, e = 1 - (1 - u) * (1 - u);
            m->x = lerpf(ox, X, e);
            m->jump = lerpf(oj, 0, u) + sg_sinf(u * (float)M_PI) * MINI_ARC_PX;
            m->s = MINI_SCALE * (0.45f + 0.55f * e);
        } else if (a->mini_st[i] == 2) {
            const float u = sg_minf(1, age / MINI_OUT_S);
            m->x = lerpf(X, ox, u * u);
            m->jump = lerpf(0, oj, u) + sg_sinf(u * (float)M_PI) * MINI_ARC_PX;
            m->s = MINI_SCALE * (1 - 0.55f * u);
            m->eyes = SL_EYE_HAPPY; /* job done */
        } else {
            m->x = X;
            m->s = MINI_SCALE;
            if (sleepy) {
                m->jump = 0;
                m->sy = (float)(1 + 0.05 * sin(now * 2 + i));
                m->sx = 2 - m->sy;
            } else {
                const float land = age - MINI_IN_S; /* a squash where it landed, then hops from there */
                hop_vals(sg_maxf(0, land - 0.2f), period, 12, &m->jump, &m->sx, &m->sy);
                if (land < 0.2f) {
                    const float q = sg_sinf(land / 0.2f * (float)M_PI);
                    m->sx = 1 + 0.25f * q;
                    m->sy = 1 - 0.25f * q;
                    m->jump = 0;
                }
            }
        }
    }
}

void sl_anim_step(sl_anim_t *a, double now, float dt, sl_pose_t *out)
{
    float t = (float)(now - a->t0);
    if (DUR[a->state] > 0 && t > DUR[a->state]) {
        sl_anim_set_state(a, SL_IDLE, now);
        t = 0;
    }
    sl_anim_pose_raw(a, a->state, t, now, dt, out);
    jelly(a, out, dt);
    sl_anim_minis(a, now, out);
}

void sl_anim_message(const sl_anim_t *a, char *buf, size_t len)
{
    static const char *MSG[SL_LANG_COUNT][SL_STATE_COUNT] = {
        [SL_LANG_ZH] = {
            [SL_IDLE] = "史莱姆正在发呆。",
            [SL_GREET] = "史莱姆很高兴见到你！",
            [SL_POKE_L] = "噗叽！史莱姆被戳了一下。",
            [SL_POKE_R] = "噗叽！史莱姆被戳了一下。",
            [SL_DIZZY] = "史莱姆晕头转向……",
            [SL_SLEEP] = "史莱姆睡着了……",
            [SL_THINK] = "史莱姆正在思考……",
            [SL_WAIT] = "史莱姆在等你下令！",
            [SL_HURT] = "史莱姆受到了 3 点伤害！",
            [SL_CHARGE] = "史莱姆正在吃电，好吃！",
            [SL_MELT] = "史莱姆快要化掉了……",
            [SL_METAL] = "史莱姆变成金属的了！",
            [SL_WORK] = "史莱姆正在努力干活！",
            [SL_SULK] = "史莱姆有点委屈……",
        },
        [SL_LANG_EN] = {
            [SL_IDLE] = "The slime is daydreaming.",
            [SL_GREET] = "The slime is happy to see you!",
            [SL_POKE_L] = "Boing! The slime got poked.",
            [SL_POKE_R] = "Boing! The slime got poked.",
            [SL_DIZZY] = "The slime is dizzy...",
            [SL_SLEEP] = "The slime fell asleep...",
            [SL_THINK] = "The slime is thinking...",
            [SL_WAIT] = "The slime awaits your command!",
            [SL_HURT] = "The slime takes 3 damage!",
            [SL_CHARGE] = "The slime is eating power. Yum!",
            [SL_MELT] = "The slime is melting...",
            [SL_METAL] = "The slime turned to metal!",
            [SL_WORK] = "The slime is hard at work!",
            [SL_SULK] = "The slime is a little hurt...",
        },
    };
    if (a->state == SL_LEVELUP) {
        snprintf(buf, len, SL_TR("叮叮叮！升到了 Lv %d！", "Ding ding! Reached Lv %d!"), a->flag_up ? a->lv : a->lv + 1);
    } else {
        snprintf(buf, len, "%s", MSG[sl_lang == SL_LANG_EN][a->state]);
    }
}
