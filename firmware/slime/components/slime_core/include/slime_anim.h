/*
 * Slime animation: state machine + jelly springs.
 * Direct port of design/slime_preview.html (state functions, hop, breathe, jelly).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SL_IDLE = 0,
    SL_GREET,
    SL_POKE_L,
    SL_POKE_R,
    SL_DIZZY,
    SL_SLEEP,
    SL_THINK,
    SL_WAIT,
    SL_LEVELUP,
    SL_HURT,
    SL_CHARGE,
    SL_MELT,
    SL_METAL,
    SL_WORK,
    SL_SULK, /* sad droop: you shook your head at it */
    SL_STATE_COUNT,
} sl_state_t;

typedef enum { SL_PAL_BLUE = 0, SL_PAL_METAL, SL_PAL_RED, SL_PAL_GOLD, SL_PAL_COUNT } sl_pal_t;

typedef enum { SL_EYE_OPEN = 0, SL_EYE_SAD, SL_EYE_HALF, SL_EYE_DIZZY, SL_EYE_CLOSED, SL_EYE_HAPPY, SL_EYE_SQUEEZE } sl_eye_t;

typedef enum { SL_MOUTH_GRIN = 0, SL_MOUTH_BIG, SL_MOUTH_SMALL, SL_MOUTH_O, SL_MOUTH_FLAT, SL_MOUTH_WAVY, SL_MOUTH_CHOMP } sl_mouth_t;

typedef enum { SL_FX_HEART = 0, SL_FX_BANG, SL_FX_Z, SL_FX_SPARKLE, SL_FX_STAR, SL_FX_DROP, SL_FX_BOLT, SL_FX_DOT } sl_fx_kind_t;

#define SL_MAX_FX 12
#define SL_MAX_MINIS 4 /* helper slimes, one per running Claude Code subagent */

typedef struct {
    sl_fx_kind_t kind;
    float x, y; /* grid units (x6 px), top-left of the legacy pixel pattern */
    float r;    /* SL_FX_DOT radius, grid units */
    uint32_t rgb;
    float alpha;
} sl_fx_t;

/* A helper slime: a small copy that stands beside the pet while a subagent runs. */
typedef struct {
    float x;    /* screen px, centre of its base */
    float jump; /* px above the ground */
    float s;    /* size relative to the pet */
    float sx, sy;
    float look; /* -1..1, horizontal gaze */
    sl_eye_t eyes; /* SL_EYE_OPEN, SL_EYE_CLOSED or SL_EYE_HAPPY */
    bool front;    /* in flight: drawn over the pet, not behind it */
} sl_mini_t;

/* Everything the renderer needs for one frame. */
typedef struct {
    float dx, jump, sx, sy, sway, curl, puddle, dim, blink, rip;
    float look[2];
    bool flash, has_sweep, has_glow;
    float sweep;
    uint32_t glow_rgb;
    sl_pal_t pal;
    sl_eye_t eyes;
    sl_mouth_t mouth;
    sl_fx_t fx[SL_MAX_FX];
    int nfx;
    sl_mini_t mini[SL_MAX_MINIS];
    int nmini;
    double T; /* global seconds */
    float t;  /* seconds since state entry */
} sl_pose_t;

typedef struct {
    sl_state_t state;
    double t0;
    int lv;
    float hp; /* 0..20 */
    bool hp_external; /* true: HP comes from the fuel gauge, states don't change it */
    bool flag_up, flag_hit;
    /* external physics, all default 0: device tilt (gravity along screen x, in g, + = right),
     * and one-shot velocity kicks from bumps (consumed by the next step) */
    float ext_tilt, ext_kick_y, ext_kick_x;
    /* external gaze (camera face tracking): replaces the state's own look target while on */
    bool ext_look_on;
    float ext_look_x, ext_look_y;
    /* external lean added to the state's sway (camera head-tilt mirror), + = top to screen right */
    float ext_sway;
    /* helper slimes: how many should be out (set by the caller), and each slot's life */
    int ext_minis;
    uint8_t mini_st[SL_MAX_MINIS]; /* 0 = off, 1 = out (popping in, then hopping), 2 = hopping back */
    double mini_t[SL_MAX_MINIS];   /* time of the last change */
    double mini_last;              /* last pop in or out: they come and go one at a time */
    /* jelly springs */
    bool j_init;
    float j_sx, j_sy, j_vsx, j_vsy, j_sw, j_vsw, j_rip, j_pj, j_pdx, j_lx, j_ly, j_vlx, j_vly;
    float j_tdx, j_vtdx; /* tilt slide offset (grid units) and velocity */
} sl_anim_t;

void sl_anim_init(sl_anim_t *a, double now);
void sl_anim_set_state(sl_anim_t *a, sl_state_t s, double now);
/* Advance to `now`; fills `out` with the jelly-filtered pose. */
void sl_anim_step(sl_anim_t *a, double now, float dt, sl_pose_t *out);
/* Add the helper slimes to `out` (sl_anim_step does this; exposed for snapshots). */
void sl_anim_minis(sl_anim_t *a, double now, sl_pose_t *out);
/* Raw state pose without springs (for deterministic snapshots). */
void sl_anim_pose_raw(sl_anim_t *a, sl_state_t s, float t, double T, float dt, sl_pose_t *out);

float sl_state_duration(sl_state_t s); /* 0 = persistent */
const char *sl_state_name(sl_state_t s);
/* UTF-8 dialog message for the current state. */
void sl_anim_message(const sl_anim_t *a, char *buf, size_t len);

#ifdef __cplusplus
}
#endif
