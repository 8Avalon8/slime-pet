/*
 * Slime renderer: draws one sl_pose_t into an RGB565 canvas.
 * Port of drawSlime/eye/mouth/drawFx from design/slime_preview.html.
 */
#pragma once

#include "sg.h"
#include "slime_anim.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SL_SCREEN 480
#define SL_GROUND 366.0f

typedef struct {
    int x0, y0, x1, y1; /* half-open; empty when x1 <= x0 */
} sl_rect_t;

void sl_render_init(void);

/* Profiling: with a clock set, sl_render_slime accumulates seconds per stage into
 * cv->scratch->prof[SL_PROF_*]. */
enum { SL_PROF_SHADOW = 0, SL_PROF_GLOW, SL_PROF_BODY, SL_PROF_FACE, SL_PROF_FX, SL_PROF_CLEAR, SL_PROF_JOIN, SL_PROF_DIALOG };
void sl_render_set_clock(double (*clock)(void));
/* Draw shadow, body, face and effects. Returns the touched bounding box. */
sl_rect_t sl_render_slime(sg_canvas_t *cv, const sl_pose_t *p);
/* Conservative bounding box of what sl_render_slime would touch. */
sl_rect_t sl_render_bounds(const sl_pose_t *p);

/* Retro RPG style windows (white frame on black). */
void sl_render_hud(sg_canvas_t *cv, int lv, float hp);
void sl_render_dialog(sg_canvas_t *cv, const char *utf8, int visible_chars, bool cursor);

static inline sl_rect_t sl_rect_union(sl_rect_t a, sl_rect_t b)
{
    if (a.x1 <= a.x0) return b;
    if (b.x1 <= b.x0) return a;
    sl_rect_t r = {a.x0 < b.x0 ? a.x0 : b.x0, a.y0 < b.y0 ? a.y0 : b.y0, a.x1 > b.x1 ? a.x1 : b.x1, a.y1 > b.y1 ? a.y1 : b.y1};
    return r;
}

#define SL_HUD_RECT ((sl_rect_t){14, 12, 186, 80})
#define SL_DIALOG_RECT ((sl_rect_t){14, 384, 466, 468})

#ifdef __cplusplus
}
#endif
