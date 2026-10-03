/*
 * Scenery behind the pet: sky, sun or moon, hills and grass for four times of day.
 * Drawn once into a full-screen buffer; the firmware copies from it wherever it
 * used to clear to black.
 */
#pragma once

#include "sg.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { SL_SCENE_DAWN = 0, SL_SCENE_DAY, SL_SCENE_DUSK, SL_SCENE_NIGHT, SL_SCENE_COUNT } sl_scene_t;

/* The scene for a local time: dawn 5-8, day 8-17, dusk 17-19, night otherwise. */
sl_scene_t sl_scene_at(int hour);
const char *sl_scene_name(sl_scene_t s);
/* Paints the whole canvas (SL_SCREEN x SL_SCREEN). */
void sl_bg_draw(sg_canvas_t *cv, sl_scene_t s);

#ifdef __cplusplus
}
#endif
