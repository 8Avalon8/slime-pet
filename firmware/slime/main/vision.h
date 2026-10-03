#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Camera module (left slot, SC101IOT 720p or OV3640): face detection a few times a second and
 * a cheap frame-difference wave detector. Everything stays on the device; frames never leave it.
 * Runs on core 1 at the lowest priority, in the time the render worker leaves idle.
 */
/* Hand gestures the camera can tell apart; posted as SEV_GESTURE with vx = one of these. */
typedef enum {
    VG_NONE = 0,
    VG_ONE,   /* fingers held up: one to five (five = open palm) */
    VG_TWO,
    VG_THREE,
    VG_FOUR,
    VG_FIVE,
    VG_LIKE,    /* thumb up */
    VG_OK,      /* thumb and forefinger make a ring */
    VG_CALL,    /* thumb and little finger out, "call me" */
    VG_DISLIKE, /* thumb down */
    VG_COUNT,
} vision_gesture_t;

typedef struct {
    bool enabled;
    bool ok;          /* camera streaming */
    bool face_on, gesture_on; /* which models are loaded right now (vision_set_features) */
    bool gesture_missing;     /* gestures are switched on, but this device's flash has no hand models (they come with a USB flash) */
    bool hand;        /* a hand in the latest analysed frame */
    int gesture;      /* ... and what it shows (vision_gesture_t), VG_NONE if nothing sure */
    float gesture_score, hand_ms; /* the classifier's confidence 0..1; time for detector + classifier */
    bool plugged;     /* a camera module sits in the left slot, whether or not it works */
    int tries;        /* open attempts so far */
    esp_err_t err;    /* last failure (NOT_FOUND/TIMEOUT = no camera plugged in) */
    bool face;        /* a face in the latest analysed frame */
    int faces;
    float fx, fy;     /* primary face centre, -1..1, from the slime's point of view (x: its right) */
    float fsize;      /* face box width / frame width */
    float raw_x, raw_y; /* centre before mirroring, upright image coords: x right, y down */
    float infer_ms;   /* last face detection */
    float fps;        /* face detections per second */
    float motion, motion_x; /* last frame-difference sample: moving fraction, centroid 0..1 */
    float luma;       /* mean brightness 0-255 of the motion frame */
    float box[4];     /* last face box x0,y0,x1,y1 in 0..1, mirrored picture (as in the preview) */
    /* 5 landmarks of the primary face, mirrored picture 0..1: eye, eye, nose, mouth, mouth
     * (eyes/mouth corners ordered left to right on screen) */
    bool kp_ok;
    float kp[10];
    /* head pose from the landmarks, independent of where the face is and how big:
     * roll: eye line angle in the mirrored picture, radians, + = the slime-side-right eye lower
     * (your head top leans to screen right); yaw: nose offset from the eye midpoint / eye
     * distance, + = nose to screen right; pitch: nose drop below the eye line / eye distance */
    float roll, yaw, pitch;
    uint32_t face_seq; /* bumps with every analysed frame that had a face */
    /* left slot as the module manager sees it (why a hot-plugged camera is not picked up) */
    int slot_presence, slot_desc, slot_owner, slot_type; /* mosaico_module_mgr enums, -1 = unknown */
    esp_err_t slot_err;
    int bus_resets; /* I2C bus recoveries so far */
} vision_state_t;

/* One analysed face, for tuning nod/shake detection. */
typedef struct {
    uint32_t t;             /* ms since boot */
    float yaw, nod;         /* keypoint signals (for comparison) */
    float bx, by;           /* head motion in the face region, face widths / heights: what detection uses */
    float f_yaw, f_nod;     /* filtered box signals */
    float thr_yaw, thr_nod; /* current swing thresholds */
    uint8_t flags;          /* 1 yaw swing done, 2 nod swing done, 4 shake fired, 8 nod fired */
} vision_head_sample_t;
/* Copies samples newer than `since` (oldest first); returns the count. */
int vision_head_trace(vision_head_sample_t *out, int max, uint32_t since);

void vision_start(void);
void vision_enable(bool on);
/* What the camera picture is analysed for. Each model is loaded when its feature is switched on
 * and freed when it is switched off; the wave and cover detectors need neither. */
void vision_set_features(bool face, bool gesture);
void vision_get(vision_state_t *out);
/* Copies the last detector input (BGR888, upright, top row first). Debug only. */
bool vision_snapshot(uint8_t *dst, size_t cap, int *w, int *h);
/* Live preview: every frame is also converted to a mirrored RGB565 picture, full screen height
 * (270x480, camera view mode) or a 90x160 thumbnail (picture-in-picture). */
enum { VISION_PV_OFF = 0, VISION_PV_FULL = 1, VISION_PV_PIP = 2 };
void vision_set_preview(int mode);
/* Copies a preview newer than *seq into dst (row stride in pixels, at column x0). */
bool vision_preview_copy(uint16_t *dst, int stride, int x0, int max_w, int max_h, uint32_t *seq, int *w, int *h);

#ifdef __cplusplus
}
#endif
