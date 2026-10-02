/*
 * Touch settings menu drawn in the same retro RPG window style as the HUD.
 * Generic: the firmware maps its settings to items and back.
 */
#pragma once

#include <stdbool.h>

#include "sg.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SL_MENU_ROWS 6

typedef enum { SL_MI_NUM = 0, SL_MI_BOOL, SL_MI_INFO, SL_MI_ACTION } sl_mi_kind_t;

typedef struct {
    const char *label;
    sl_mi_kind_t kind;
    int value, min, max, step; /* SL_MI_NUM; SL_MI_BOOL uses value 0/1 */
    const char *unit;          /* appended to numbers, e.g. "%" */
    char text[48];             /* SL_MI_INFO */
} sl_menu_item_t;

typedef struct {
    sl_menu_item_t *items;
    int n;
    int page;
    int flash_row; /* row index briefly highlighted after a tap, -1 = none */
} sl_menu_t;

typedef enum { SL_MA_NONE = 0, SL_MA_CHANGED, SL_MA_PAGE, SL_MA_CLOSE, SL_MA_ACTION } sl_menu_action_t;

int sl_menu_pages(const sl_menu_t *m);
/* Full-screen draw (480x480). */
void sl_menu_render(sg_canvas_t *cv, const sl_menu_t *m, const char *title);
/* A tap at (x, y): adjusts values, flips pages or asks to close. *changed = item index for SL_MA_CHANGED/ACTION. */
sl_menu_action_t sl_menu_tap(sl_menu_t *m, int x, int y, int *changed);

#ifdef __cplusplus
}
#endif
