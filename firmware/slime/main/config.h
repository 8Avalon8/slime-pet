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
    /* v8 */
    uint8_t lang; /* sl_lang_t: 0 = Chinese, 1 = English (device and web panel) */
    /* v9 */
    bool wake; /* the wake word starts a conversation without the AI key (needs voice and the microphone) */
    /* v10 */
    uint8_t scene; /* background: CFG_SCENE_AUTO follows the clock, CFG_SCENE_OFF is plain black, else one fixed sl_scene_t + 2 */
    /* v11 */
    bool speak;          /* read voice answers aloud (the bridge synthesizes them, see audio_speak) */
    uint8_t speak_pitch; /* 80-150 %: the bridge raises (or lowers) the synthesized voice by this much */
    /* v12: what the camera looks for. Each has its own model, loaded only while switched on; both do not fit, gestures win */
    bool face;    /* faces: eye contact, presence, nod and shake, the sitting reminder */
    bool gesture; /* hand gestures as commands: thumb up/down, open palm, OK, "call me" */
    /* v13 */
    bool quiet; /* quiet hours (night_start..night_end: no sound, no LEDs, dimmer screen, early sleep) are in use at all */
} slime_cfg_t;

#define CFG_SCENE_AUTO 0
#define CFG_SCENE_OFF 1
#define CFG_SCENE_MAX 5 /* dawn, day, dusk, night = 2..5 */

/* Initializes NVS (shared with the progress store) and loads settings; defaults on any error. */
esp_err_t cfg_init(void);
void cfg_get(slime_cfg_t *out);
/* Clamps, stores and bumps the generation counter. */
void cfg_set(const slime_cfg_t *in);
/* Same, but persist=false only applies it (live preview); call again with true to save. */
void cfg_set_ex(const slime_cfg_t *in, bool persist);
/* Incremented on every cfg_set(); cheap to poll each frame. */
uint32_t cfg_gen(void);

/* Where the computer side (bridge/slime_brain.py, slime_tts.py) finds its language model,
 * speech-to-text and text-to-speech services. The device only keeps these strings so that the
 * web panel is the one place to set them; it never calls the services itself. Stored as separate NVS strings, not in slime_cfg_t.
 * Only the web server task reads or writes them. */
typedef enum {
    AI_LLM_URL, AI_LLM_MODEL, AI_LLM_KEY,
    AI_STT_URL, AI_STT_MODEL, AI_STT_KEY,
    AI_TTS_URL, AI_TTS_MODEL, AI_TTS_KEY, AI_TTS_VOICE, /* voice: a preset name, or a description for a voice-design model */
    AI_FIELD_COUNT
} ai_field_t;
#define AI_VALUE_MAX 200 /* longest value of any field, without the NUL */
const char *ai_field_name(ai_field_t f); /* JSON name, e.g. "llm_url" */
bool ai_field_secret(ai_field_t f);      /* API keys: never sent back without the update token */
const char *ai_get(ai_field_t f);        /* "" when unset */
/* Empty clears it. ESP_ERR_INVALID_ARG: too long, control characters, or a URL that is not http(s). */
esp_err_t ai_set(ai_field_t f, const char *val);
