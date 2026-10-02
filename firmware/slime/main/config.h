#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* User settings, persisted in NVS and editable from the web panel. */
typedef struct {
    uint8_t screen_bright; /* 10-100, awake */
    uint8_t sleep_bright;  /* 0-100, while asleep */
    uint8_t led_bright;    /* 0-255, Interaction module LEDs */
    bool idle_breath;      /* LEDs breathe while idle/charging */
    bool motor;
    uint8_t sleep_min; /* 1-120: minutes without activity before sleeping */
    bool tilt;
    bool mic;
    uint8_t clap_sens; /* 1-10 */
    bool dance;        /* jelly bobs to sound */
    bool sound;        /* speaker effects */
    uint8_t volume;    /* 0-100 */
    bool text_blip;    /* typewriter blips in the dialog */
    bool show_fps;
    /* v2 */
    bool camera;       /* face tracking, presence, wave */
    uint8_t sit_min;   /* 0 = off: remind to get up after this many minutes in front of the camera */
    /* v3 */
    bool bgm;          /* hum a background tune now and then while idle */
    /* v4 */
    bool cam_pip;      /* camera thumbnail in the corner all the time (it also pops up for a while on plug-in) */
    /* v5 */
    uint8_t night_start, night_end; /* quiet hours, 0-23 o'clock; equal = off */
    uint8_t focus_min;              /* focus timer length, minutes */
    /* v6 */
    bool ai_comment; /* show the one-line comments the bridge's local model writes after a Claude turn */
    /* v7 */
    bool voice; /* hold the AI key to talk (bridge/slime_buddy.py answers); off = hold for the help page */
} slime_cfg_t;

/* Initializes NVS (shared with the progress store) and loads settings; defaults on any error. */
esp_err_t cfg_init(void);
void cfg_get(slime_cfg_t *out);
/* Clamps, stores and bumps the generation counter. */
void cfg_set(const slime_cfg_t *in);
/* Same, but persist=false only applies it (live preview); call again with true to save. */
void cfg_set_ex(const slime_cfg_t *in, bool persist);
/* Incremented on every cfg_set(); cheap to poll each frame. */
uint32_t cfg_gen(void);
