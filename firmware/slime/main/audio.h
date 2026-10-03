#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>


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
    bool speaking; /* a spoken answer is playing */
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
/* Effect names used by the panel and the "sfx <name>" command. */
const char *audio_sfx_name(int s);
int audio_sfx_find(const char *name); /* -1 if unknown */
/* Ignore the mic for a while: the motor, a touch or a bump is not a clap. */
void audio_hold_off(uint32_t ms);
void audio_get(audio_state_t *out);

/* Push-to-talk: 16 kHz mono 16-bit, up to AUDIO_REC_MAX_S, kept in PSRAM until the next one.
 * Recording listens even with the mic setting off (the user asked for it) and ignores claps. */
#define AUDIO_REC_RATE 16000
#define AUDIO_REC_MAX_S 10
bool audio_rec_start(void);
/* Ends the recording; returns its length in ms. A recording of at least min_ms becomes the
 * new "ready" one with a new sequence number (the bridge fetches it from /api/voice.wav). */
uint32_t audio_rec_stop(uint32_t min_ms);
/* While recording: how much voice has been heard so far, and for how long it has been quiet. */
void audio_rec_voice(uint32_t *speech_ms, uint32_t *quiet_ms);
bool audio_rec_active(void); /* false again once AUDIO_REC_MAX_S is full */
typedef struct {
    uint32_t seq; /* last finished recording, 0 = none yet */
    bool ready;   /* not fetched yet */
    bool rec;     /* recording now */
} audio_rec_state_t;
void audio_rec_get(audio_rec_state_t *out);
/* The ready recording with this seq, marked as fetched; NULL if there is none. */
const int16_t *audio_rec_take(uint32_t seq, size_t *samples);

/* Spoken answers: the bridge synthesizes the speech (slime_tts.py) and POSTs it to /api/speak as
 * 16 kHz mono 16-bit PCM. Played upsampled to 48 kHz, mixed with the effects; the background tune
 * stops. A new one replaces the one playing; holding the AI key to talk cuts it short. */
#define AUDIO_SPEAK_RATE 16000
#define AUDIO_SPEAK_MAX_S 20
/* Takes ownership of pcm (heap_caps_malloc'd, freed by the audio task). False when it cannot
 * play (no codec, night mode); pcm is freed then too. */
bool audio_speak(int16_t *pcm, size_t samples);
void audio_speak_stop(void);
/* Loudness of the speech now playing, 0..1 (for the mouth); -1 when nothing is being said. */
float audio_speak_level(void);
