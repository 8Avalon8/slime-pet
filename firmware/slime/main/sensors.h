#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Background sensing, all optional: the onboard BMI270 IMU (tilt, bumps, shakes, face-down)
 * and the Interaction module (buttons, PIR, light, 6 RGB LEDs), which is hot-pluggable in
 * either slot. Two tasks on core 0; the render loop only reads snapshots and events.
 */

typedef struct {
    bool imu_ok;
    float ax, ay, az; /* low-passed acceleration in screen axes, g (x right, y up, z out of screen) */
    float tilt;       /* gravity along screen x relative to the resting angle: + = right edge lower, g */
    bool face_down;   /* screen towards the floor for > 1 s */
    bool mod_ok;      /* Interaction module present */
    int mod_tries;    /* discovery attempts so far */
    esp_err_t mod_err; /* result of the last attempt (NOT_FOUND/TIMEOUT = nothing plugged in) */
    bool motion;      /* PIR level */
    uint8_t light;    /* relative 0-100 */
} sensors_state_t;

typedef enum {
    SEV_BUMP = 0, /* vx/vy: jelly velocity kicks */
    SEV_SHAKE,
    SEV_BTN_L,
    SEV_BTN_R,
    SEV_MOTION, /* PIR rising edge */
    SEV_CLAP,   /* from audio.c */
    SEV_DOUBLE_CLAP,
    SEV_BEAT, /* vy: strength in dB above the recent level */
    SEV_WAVE_L, /* from vision.cpp, slime's point of view */
    SEV_WAVE_R,
    SEV_COVER,   /* hand over the lens */
    SEV_UNCOVER, /* ... and away again */
    SEV_NOD,        /* head nodded yes */
    SEV_HEAD_SHAKE, /* head shaken no */
    SEV_GESTURE,    /* a hand gesture; vx: which (vision_gesture_t), vy: confidence 0..1 */
} sensor_ev_kind_t;

typedef struct {
    sensor_ev_kind_t kind;
    float vx, vy;
} sensor_ev_t;

/* LED moods for the Interaction module, picked by the pet state. */
typedef enum {
    MOOD_OFF = 0,
    MOOD_IDLE,   /* slime-blue breathing */
    MOOD_CHARGE, /* amber breathing */
    MOOD_POKE,   /* white flash */
    MOOD_MELT,
    MOOD_METAL,
    MOOD_THINK,
    MOOD_WORK,
    MOOD_WAIT,
    MOOD_HURT,
    MOOD_HAPPY,
    MOOD_LEVELUP,
    MOOD_DIZZY,
    MOOD_COUNT,
} led_mood_t;

/* What sits in a module slot, for noticing that something was plugged in or pulled out. */
#define SLOT_PENDING (-1)  /* not known yet: just booted, or the module's descriptor is still being read */
#define SLOT_EMPTY 0
#define SLOT_CAMERA 0x07   /* the board types of mosaico_module_mgr.h */
#define SLOT_INTERACT 0x16
#define SLOT_OTHER 0xff    /* a module that does not say what it is */

/* Starts whatever is available; never fatal. */
void sensors_start(void);
void sensors_get(sensors_state_t *out);
/* slot 0 = left, 1 = right: SLOT_PENDING, SLOT_EMPTY, or the board type of the module in it. Cheap. */
int sensors_slot(int slot);
/* Non-blocking queue pop. */
bool sensors_next_event(sensor_ev_t *ev);
/* For other producers (audio). */
void sensors_post(sensor_ev_kind_t kind, float vx, float vy);
void sensors_set_mood(led_mood_t mood);
/* 0-255, applied to every LED frame. */
void sensors_set_led_brightness(uint8_t level);

#ifdef __cplusplus
}
#endif
