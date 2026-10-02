#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * sfxr sound effects: a port of DrPetter's public-domain sfxr synthesizer (the engine behind
 * jsfxr/Bfxr). One effect is ~23 parameters; the field names match jsfxr's JSON export, so a
 * sound tuned on the jsfxr site can be pasted into the web panel as is.
 * Runs at sfxr's native 44.1 kHz with 8x supersampling; audio.c resamples to the codec rate.
 */
#define SFXR_FIELDS(X)                                                                                                 \
    X(p_env_attack, 0, 1) X(p_env_sustain, 0, 1) X(p_env_punch, 0, 1) X(p_env_decay, 0, 1) X(p_base_freq, 0, 1)        \
    X(p_freq_limit, 0, 1) X(p_freq_ramp, -1, 1) X(p_freq_dramp, -1, 1) X(p_vib_strength, 0, 1) X(p_vib_speed, 0, 1)   \
    X(p_arp_mod, -1, 1) X(p_arp_speed, 0, 1) X(p_duty, 0, 1) X(p_duty_ramp, -1, 1) X(p_repeat_speed, 0, 1)            \
    X(p_pha_offset, -1, 1) X(p_pha_ramp, -1, 1) X(p_lpf_freq, 0, 1) X(p_lpf_ramp, -1, 1) X(p_lpf_resonance, 0, 1)     \
    X(p_hpf_freq, 0, 1) X(p_hpf_ramp, -1, 1) X(sound_vol, 0, 1)

typedef struct {
    int32_t wave_type; /* 0 square, 1 sawtooth, 2 sine, 3 noise */
#define SFXR_DECL(name, lo, hi) float name;
    SFXR_FIELDS(SFXR_DECL)
#undef SFXR_DECL
} sfxr_params_t;

#define SFXR_RATE 44100

typedef struct {
    sfxr_params_t p;
    bool playing;
    int phase;
    double fperiod, fmaxperiod, fslide, fdslide;
    int period;
    float square_duty, square_slide;
    int env_stage, env_time, env_length[3];
    float env_vol;
    float fphase, fdphase;
    int iphase, ipp;
    float noise_buffer[32];
    float fltp, fltdp, fltw, fltw_d, fltdmp, fltphp, flthp, flthp_d;
    float vib_phase, vib_speed, vib_amp;
    int rep_time, rep_limit;
    int arp_time, arp_limit;
    double arp_mod;
    float gain;
    uint32_t rng;
    int samples; /* produced so far: hard cap against runaway parameter sets */
    float phaser_buffer[1024];
} sfxr_t;

/* jsfxr defaults (its "reset"). */
void sfxr_defaults(sfxr_params_t *p);
/* Clamps every field into its range; returns false if wave_type was invalid (set to square). */
bool sfxr_sanitize(sfxr_params_t *p);
void sfxr_start(sfxr_t *s, const sfxr_params_t *p, uint32_t seed);
/* Next 44.1 kHz sample, about -1..1; 0 once finished (s->playing false). */
float sfxr_next(sfxr_t *s);

#ifdef __cplusplus
}
#endif
