/* Port of sfxr by Tomas Pettersson (DrPetter), public domain; structure follows the original
 * SynthSample/ResetSample, with jsfxr's output gain so pasted jsfxr sounds keep their level. */
#include "sfxr.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SAMPLES (8 * SFXR_RATE) /* nothing we play should be longer */

void sfxr_defaults(sfxr_params_t *p)
{
    memset(p, 0, sizeof *p);
    p->p_env_sustain = 0.3f;
    p->p_env_decay = 0.4f;
    p->p_base_freq = 0.3f;
    p->p_lpf_freq = 1.0f;
    p->sound_vol = 0.5f;
}

bool sfxr_sanitize(sfxr_params_t *p)
{
#define SFXR_CLAMP(name, lo, hi) p->name = isfinite(p->name) ? (p->name < (lo) ? (lo) : (p->name > (hi) ? (hi) : p->name)) : 0;
    SFXR_FIELDS(SFXR_CLAMP)
#undef SFXR_CLAMP
    if (p->wave_type < 0 || p->wave_type > 3) {
        p->wave_type = 0;
        return false;
    }
    return true;
}

static inline float frnd(sfxr_t *s, float range)
{
    s->rng ^= s->rng << 13; /* xorshift32 */
    s->rng ^= s->rng >> 17;
    s->rng ^= s->rng << 5;
    return (float)(s->rng >> 8) * (1.0f / 16777216.0f) * range;
}

static void reset(sfxr_t *s, bool restart)
{
    const sfxr_params_t *p = &s->p;
    if (!restart) s->phase = 0;
    s->fperiod = 100.0 / (p->p_base_freq * p->p_base_freq + 0.001);
    s->period = (int)s->fperiod;
    s->fmaxperiod = 100.0 / (p->p_freq_limit * p->p_freq_limit + 0.001);
    s->fslide = 1.0 - pow(p->p_freq_ramp, 3.0) * 0.01;
    s->fdslide = -pow(p->p_freq_dramp, 3.0) * 0.000001;
    s->square_duty = 0.5f - p->p_duty * 0.5f;
    s->square_slide = -p->p_duty_ramp * 0.00005f;
    s->arp_mod = p->p_arp_mod >= 0 ? 1.0 - pow(p->p_arp_mod, 2.0) * 0.9 : 1.0 + pow(p->p_arp_mod, 2.0) * 10.0;
    s->arp_time = 0;
    s->arp_limit = (int)(powf(1.0f - p->p_arp_speed, 2.0f) * 20000 + 32);
    if (p->p_arp_speed == 1.0f) s->arp_limit = 0;
    if (restart) return;
    s->fltp = s->fltdp = 0;
    s->fltw = powf(p->p_lpf_freq, 3.0f) * 0.1f;
    s->fltw_d = 1.0f + p->p_lpf_ramp * 0.0001f;
    s->fltdmp = 5.0f / (1.0f + powf(p->p_lpf_resonance, 2.0f) * 20.0f) * (0.01f + s->fltw);
    if (s->fltdmp > 0.8f) s->fltdmp = 0.8f;
    s->fltphp = 0;
    s->flthp = powf(p->p_hpf_freq, 2.0f) * 0.1f;
    s->flthp_d = 1.0f + p->p_hpf_ramp * 0.0003f;
    s->vib_phase = 0;
    s->vib_speed = powf(p->p_vib_speed, 2.0f) * 0.01f;
    s->vib_amp = p->p_vib_strength * 0.5f;
    s->env_vol = 0;
    s->env_stage = 0;
    s->env_time = 0;
    s->env_length[0] = (int)(p->p_env_attack * p->p_env_attack * 100000.0f);
    s->env_length[1] = (int)(p->p_env_sustain * p->p_env_sustain * 100000.0f);
    s->env_length[2] = (int)(p->p_env_decay * p->p_env_decay * 100000.0f);
    s->fphase = powf(p->p_pha_offset, 2.0f) * 1020.0f;
    if (p->p_pha_offset < 0) s->fphase = -s->fphase;
    s->fdphase = powf(p->p_pha_ramp, 2.0f);
    if (p->p_pha_ramp < 0) s->fdphase = -s->fdphase;
    s->iphase = abs((int)s->fphase);
    s->ipp = 0;
    memset(s->phaser_buffer, 0, sizeof s->phaser_buffer);
    for (int i = 0; i < 32; i++) s->noise_buffer[i] = frnd(s, 2.0f) - 1.0f;
    s->rep_time = 0;
    s->rep_limit = (int)(powf(1.0f - p->p_repeat_speed, 2.0f) * 20000 + 32);
    if (p->p_repeat_speed == 0) s->rep_limit = 0;
}

void sfxr_start(sfxr_t *s, const sfxr_params_t *p, uint32_t seed)
{
    s->p = *p;
    sfxr_sanitize(&s->p);
    s->rng = seed ? seed : 0x9E3779B9u;
    s->gain = expf(s->p.sound_vol) - 1.0f; /* jsfxr's volume curve */
    s->samples = 0;
    reset(s, false);
    s->playing = true;
}

float sfxr_next(sfxr_t *s)
{
    if (!s->playing) return 0;
    const sfxr_params_t *p = &s->p;
    if (++s->samples > MAX_SAMPLES) {
        s->playing = false;
        return 0;
    }
    s->rep_time++;
    if (s->rep_limit != 0 && s->rep_time >= s->rep_limit) {
        s->rep_time = 0;
        reset(s, true);
    }
    s->arp_time++;
    if (s->arp_limit != 0 && s->arp_time >= s->arp_limit) {
        s->arp_limit = 0;
        s->fperiod *= s->arp_mod;
    }
    s->fslide += s->fdslide;
    s->fperiod *= s->fslide;
    if (s->fperiod > s->fmaxperiod) {
        s->fperiod = s->fmaxperiod;
        if (p->p_freq_limit > 0) {
            s->playing = false;
            return 0;
        }
    }
    float rfperiod = (float)s->fperiod;
    if (s->vib_amp > 0) {
        s->vib_phase += s->vib_speed;
        rfperiod = (float)s->fperiod * (1.0f + sinf(s->vib_phase) * s->vib_amp);
    }
    s->period = (int)rfperiod;
    if (s->period < 8) s->period = 8;
    s->square_duty += s->square_slide;
    if (s->square_duty < 0) s->square_duty = 0;
    if (s->square_duty > 0.5f) s->square_duty = 0.5f;
    /* volume envelope: attack, sustain (+punch), decay */
    s->env_time++;
    if (s->env_time > s->env_length[s->env_stage]) {
        s->env_time = 0;
        if (++s->env_stage == 3) {
            s->playing = false;
            return 0;
        }
    }
    const int len = s->env_length[s->env_stage] > 0 ? s->env_length[s->env_stage] : 1;
    const float et = (float)s->env_time / len;
    if (s->env_stage == 0) s->env_vol = et;
    else if (s->env_stage == 1) s->env_vol = 1.0f + (1.0f - et) * 2.0f * p->p_env_punch;
    else s->env_vol = 1.0f - et;
    /* phaser */
    s->fphase += s->fdphase;
    s->iphase = abs((int)s->fphase);
    if (s->iphase > 1023) s->iphase = 1023;
    if (s->flthp_d != 0) {
        s->flthp *= s->flthp_d;
        if (s->flthp < 0.00001f) s->flthp = 0.00001f;
        if (s->flthp > 0.1f) s->flthp = 0.1f;
    }
    float ssample = 0;
    for (int si = 0; si < 8; si++) { /* 8x supersampling */
        float sample = 0;
        if (++s->phase >= s->period) {
            s->phase %= s->period;
            if (p->wave_type == 3)
                for (int i = 0; i < 32; i++) s->noise_buffer[i] = frnd(s, 2.0f) - 1.0f;
        }
        const float fp = (float)s->phase / s->period;
        switch (p->wave_type) {
        case 0: sample = fp < s->square_duty ? 0.5f : -0.5f; break;
        case 1: sample = 1.0f - fp * 2; break;
        case 2: sample = sinf(fp * 2 * (float)M_PI); break;
        default: sample = s->noise_buffer[s->phase * 32 / s->period]; break;
        }
        /* low-pass, high-pass */
        const float pp = s->fltp;
        s->fltw *= s->fltw_d;
        if (s->fltw < 0) s->fltw = 0;
        if (s->fltw > 0.1f) s->fltw = 0.1f;
        if (p->p_lpf_freq != 1.0f) {
            s->fltdp += (sample - s->fltp) * s->fltw;
            s->fltdp -= s->fltdp * s->fltdmp;
        } else {
            s->fltp = sample;
            s->fltdp = 0;
        }
        s->fltp += s->fltdp;
        s->fltphp += s->fltp - pp;
        s->fltphp -= s->fltphp * s->flthp;
        sample = s->fltphp;
        /* phaser */
        s->phaser_buffer[s->ipp & 1023] = sample;
        sample += s->phaser_buffer[(s->ipp - s->iphase + 1024) & 1023];
        s->ipp = (s->ipp + 1) & 1023;
        ssample += sample * s->env_vol;
    }
    ssample = ssample / 8 * s->gain;
    return ssample > 1 ? 1 : (ssample < -1 ? -1 : ssample);
}
