#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "sfxr.h"

/*
 * ES8311 microphone + speaker, one task on core 0.
 * Mic: 10 ms RMS level, adaptive noise floor, clap / double-clap / beat detection (posted as
 * sensor events). Speaker: chiptune jingles and background tunes synthesized on the fly
 * (up to 3 voices: square, triangle, noise; scores in a small MML dialect, see tunes_original.h).
 */
typedef enum {
    SFX_LEVELUP = 0,
    SFX_DONE,
    SFX_HURT,
    SFX_ASK,
    SFX_POKE,
    SFX_GREET,
    SFX_DIZZY,
    SFX_STARTLE,
    SFX_SLEEP,
    SFX_WAKE,
    SFX_HELLO,
    SFX_BLIP,
    SFX_BOOT,
    SFX_SULK,
    SFX_SHY,
    SFX_COUNT,
} sfx_t;

typedef enum { BGM_TOWN = 0, BGM_ROAD, BGM_COUNT } bgm_t;

typedef struct {
    bool ok;     /* codec up */
    bool mic;    /* listening */
    float db;    /* last 10 ms block, dBFS */
    float floor; /* adaptive noise floor, dBFS */
    int claps;   /* since boot */
    bool playing;
    bool bgm; /* a background tune is playing */
} audio_state_t;

void audio_start(void);
void audio_configure(bool mic, uint8_t clap_sens, bool dance, bool sound, uint8_t volume);
/* Queued; SFX_BLIP is dropped when anything else is playing or queued. */
void audio_play(sfx_t s);
/* Background tune at a lower level; any effect cuts it short. Ignored while an effect plays. */
void audio_play_bgm(bgm_t b);
void audio_stop_bgm(void);
/* Night mode: drops every effect and tune until unmuted (the sound setting is left alone). */
void audio_set_muted(bool muted);
/* sfxr sound replacing built-in effect `slot` (an sfx_t); NULL restores the original. Persisted. */
bool audio_set_custom(int slot, const sfxr_params_t *p);
bool audio_has_custom(int slot);
/* Effect names used by the panel and the "sfx <name>" command. */
const char *audio_sfx_name(int s);
int audio_sfx_find(const char *name); /* -1 if unknown */
/* Plays an sfxr sound once (panel preview), interrupting nothing but the background tune. */
void audio_preview(const sfxr_params_t *p);
/* Ignore the mic for a while: the motor, a touch or a bump is not a clap. */
void audio_hold_off(uint32_t ms);
void audio_get(audio_state_t *out);
