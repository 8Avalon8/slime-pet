#pragma once

#include <stdbool.h>

#include "sg.h"

/* On-device settings screen (long-press the screen). Changes apply live, persist on close. */
void sui_open(const char *wifi, const char *addr, int lv, int exp, int need, double now);
bool sui_active(void);
/* Returns true when the screen needs a redraw. */
bool sui_tap(int x, int y, double now);
/* Auto-close after a quiet spell; returns true when it just closed. */
bool sui_tick(double now);
void sui_close(void);
void sui_render(sg_canvas_t *cv);
bool sui_dirty(void);

#define SUI_ACT_CAMVIEW 1
#define SUI_ACT_BGM 2
#define SUI_ACT_FOCUS 3
#define SUI_ACT_HELP 4
/* An action picked in the menu (once), or -1. */
int sui_take_action(void);
