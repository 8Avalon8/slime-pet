#pragma once

#include <stdbool.h>

#include "esp_err.h"

/* Short vibration patterns on the onboard motor, played by a background task. */
typedef enum { HAPTIC_TICK = 0, HAPTIC_NUDGE, HAPTIC_HURT, HAPTIC_FANFARE, HAPTIC_COUNT } haptic_t;

/* Non-fatal: without a motor, haptic_play() does nothing. */
esp_err_t haptic_init(void);
/* Non-blocking; a pattern requested while another plays is queued (up to 4). */
void haptic_play(haptic_t h);
void haptic_set_enabled(bool on);
