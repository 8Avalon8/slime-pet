#include "audio.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "bsp/esp_mosaico.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sensors.h"
#include "sfxr.h"

static const char *TAG = "audio";

/* Same format as the BSP's audio_loopback example: the I2S bus is shared by mic and speaker. */
#define RATE 48000
#define BLOCK_FRAMES 480 /* 10 ms */
#define BLOCK_BYTES (BLOCK_FRAMES * 2 * 2)
#define MIC_GAIN_DB 30.0f

#define CLAP_MAX_MS 150     /* a clap decays within this */
#define DOUBLE_MIN_MS 150   /* second clap window */
#define DOUBLE_MAX_MS 650
#define AFTER_PLAY_MS 250   /* the mic hears our own speaker */
#define BEAT_GAP_MS 160
#define ISOLATE_MS 500 /* a clap follows silence; typing is a train of clicks */

/* ---------------- music: chiptune jingles and background tunes ---------------- */

/* Two kinds of voice: pitch glides (note_t: a few notes, each sliding exponentially from f0 to
 * f1 Hz) and scores written in a small MML dialect, so a jingle is a line of notes per voice:
 *   t<bpm> o<octave> < > l<len> v<0-15> q<gate 1-8> k<decay, x10 ms; 0 = hold>
 *   @<wave: 0 square, 1 25%, 2 12.5%, 3 triangle, 4 noise>  [ ... ]<repeat>
 *   c d e f g a b (+ # sharp, - flat) [len][.]   r rest
 *   ^ after a note: tie it into the next note of the same pitch (no re-attack; commands such
 *     as a tempo change may sit in between), e.g. "f1.^ f1." or "c1.^ t1440 c2."
 */
typedef enum { W_SQ = 0, W_SQ25, W_SQ12, W_TRI, W_NOISE, W_REST } wave_t;
typedef struct {
    uint16_t f0, f1; /* Hz, exponential glide */
    uint16_t ms;
    uint8_t wave, vol; /* vol 0-100 */
    uint8_t decay;     /* x10 ms time constant, like MML k; 0 = hold */
} note_t;

/* glide notes: N(from Hz, to Hz, ms, wave, volume 0-100), NK(..., decay) fades out like MML k.
 * Noise: the "Hz" is the shift-register clock (4x the MML note frequency). */
#define N(f0, f1, ms, w, v) {f0, f1, ms, w, v, 0}
#define NK(f0, f1, ms, w, v, k) {f0, f1, ms, w, v, k}
#define REST(ms) {0, 0, ms, W_REST, 0, 0}
#define MAX_VOICES 3
#define GLIDE_LEN 12

typedef struct {
    note_t glide[GLIDE_LEN];    /* used when mml[0] is NULL */
    const char *mml[MAX_VOICES];
} sound_t;

/* tunes_local.h (untracked) overrides the bundled original tunes */
#if __has_include("tunes_local.h")
#include "tunes_local.h"
#else
#include "tunes_original.h"
#endif

typedef struct {
    /* source */
    const note_t *glide;
    int gi;
    const char *p;
    const char *loop_p[2];
    int loop_n[2], depth;
    int tempo, oct, len, vol, gate, wave, decay;
    /* current note */
    int s, total, on; /* sample within the note, note length, sounding part (gate) */
    float f, k;       /* frequency, per-sample glide factor */
    float amp, dk;    /* level 0..1 and per-sample decay factor */
    uint8_t cur_wave;
    float phase;
    uint16_t lfsr;
    float noise;
    bool done, onset;
    bool cont, hold;  /* this note continues a tie (no attack) / ties into the next (no release) */
    bool tie_pending;
    int tie_midi;
} voice_t;

typedef struct {
    voice_t v[MAX_VOICES];
    int n;
    float gain;
    bool bgm;
} synth_t;

static int mml_num(const char **p, int def)
{
    if (**p < '0' || **p > '9') return def;
    int v = 0;
    while (**p >= '0' && **p <= '9') v = v * 10 + (*(*p)++ - '0');
    return v;
}

/* Reads up to the next note or rest; false at the end of the line. */
static bool mml_next(voice_t *v)
{
    static const int8_t SEMI[7] = {9, 11, 0, 2, 4, 5, 7}; /* a b c d e f g */
    for (;;) {
        const char c = *v->p;
        if (!c) return false;
        v->p++;
        switch (c) {
        case 't': v->tempo = mml_num(&v->p, 120); break;
        case 'o': v->oct = mml_num(&v->p, 4); break;
        case '<': v->oct--; break;
        case '>': v->oct++; break;
        case 'l': v->len = mml_num(&v->p, 8); break;
        case 'v': v->vol = mml_num(&v->p, 10); break;
        case 'q': v->gate = mml_num(&v->p, 8); break;
        case 'k': v->decay = mml_num(&v->p, 0); break;
        case '@': v->wave = mml_num(&v->p, 0); break;
        case '[':
            if (v->depth < 2) {
                v->loop_p[v->depth] = v->p;
                v->loop_n[v->depth] = -1;
                v->depth++;
            }
            break;
        case ']': {
            const int n = mml_num(&v->p, 2);
            if (!v->depth) break;
            const int d = v->depth - 1;
            if (v->loop_n[d] < 0) v->loop_n[d] = n;
            if (--v->loop_n[d] > 0) v->p = v->loop_p[d];
            else v->depth--;
            break;
        }
        case 'r':
        case 'a': case 'b': case 'c': case 'd': case 'e': case 'f': case 'g': {
            int semi = 0;
            if (c != 'r') {
                semi = SEMI[c - 'a'];
                if (*v->p == '+' || *v->p == '#') semi++, v->p++;
                else if (*v->p == '-') semi--, v->p++;
            }
            const int len = mml_num(&v->p, v->len);
            int total = (int)((int64_t)RATE * 60 * 4 / ((int64_t)v->tempo * (len ? len : 4)));
            if (*v->p == '.') {
                total += total / 2;
                v->p++;
            }
            v->s = 0;
            v->total = total;
            v->k = 1;
            const bool tied = v->tie_pending;
            v->tie_pending = v->cont = v->hold = false;
            if (c == 'r') {
                v->on = 0;
                v->cur_wave = W_REST;
            } else {
                const int midi = 12 * (v->oct + 1) + semi;
                v->cont = tied && midi == v->tie_midi;
                const char *q = v->p; /* a tie mark after this note? */
                while (*q == ' ' || *q == '|') q++;
                if (*q == '^') {
                    v->p = q + 1;
                    v->hold = v->tie_pending = true;
                    v->tie_midi = midi;
                }
                v->f = 440.0f * powf(2.0f, (midi - 69) / 12.0f);
                if (v->wave == W_NOISE) v->f *= 4; /* noise: the pitch is the shift-register clock */
                v->on = v->hold ? total : total * v->gate / 8;
                v->cur_wave = (uint8_t)(v->wave <= W_NOISE ? v->wave : W_SQ);
                if (!v->cont) v->amp = v->vol / 15.0f; /* a tied note keeps fading from where it was */
                v->dk = v->decay ? expf(-1.0f / (v->decay * 0.01f * RATE)) : 1.0f;
                v->onset = !v->cont;
            }
            return true;
        }
        default: break; /* spaces, bar lines */
        }
    }
}

static bool glide_next(voice_t *v)
{
    if (v->gi >= GLIDE_LEN || v->glide[v->gi].ms == 0) return false;
    const note_t *n = &v->glide[v->gi++];
    v->s = 0;
    v->total = n->ms * RATE / 1000;
    v->on = n->wave == W_REST ? 0 : v->total;
    v->f = n->f0;
    v->k = (n->f0 && n->f1 && n->f0 != n->f1) ? powf((float)n->f1 / n->f0, 1.0f / v->total) : 1.0f;
    v->cur_wave = n->wave;
    v->amp = n->vol / 100.0f;
    v->dk = n->decay ? expf(-1.0f / (n->decay * 0.01f * RATE)) : 1.0f;
    return true;
}

static bool voice_next(voice_t *v) { return v->glide ? glide_next(v) : mml_next(v); }

static void synth_load(synth_t *y, const sound_t *snd, float gain, bool bgm)
{
    memset(y, 0, sizeof *y);
    y->gain = gain;
    y->bgm = bgm;
    if (!snd->mml[0]) {
        y->v[0].glide = snd->glide;
        y->n = 1;
    } else {
        while (y->n < MAX_VOICES && snd->mml[y->n]) {
            voice_t *v = &y->v[y->n];
            v->p = snd->mml[y->n];
            v->tempo = 120;
            v->oct = 4;
            v->len = 8;
            v->vol = 10;
            v->gate = 7;
            y->n++;
        }
    }
    for (int i = 0; i < y->n; i++) {
        y->v[i].lfsr = 0xACE1;
        y->v[i].done = !voice_next(&y->v[i]);
    }
}

static bool synth_active(const synth_t *y)
{
    for (int i = 0; i < y->n; i++)
        if (!y->v[i].done) return true;
    return false;
}

static void synth_stop(synth_t *y) { y->n = 0; }

static inline float voice_sample(voice_t *v)
{
    while (!v->done && v->s >= v->total) v->done = !voice_next(v);
    if (v->done) return 0;
    float out = 0;
    if (v->cur_wave != W_REST && v->s < v->on) {
        const float att = v->cont ? 1.0f : v->s / (0.002f * RATE), rel = v->hold ? 1.0f : (v->on - v->s) / (0.012f * RATE);
        const float m = att < rel ? att : rel, env = m < 1.0f ? m : 1.0f;
        v->phase += v->f / RATE;
        if (v->phase >= 1) {
            v->phase -= 1;
            const uint16_t bit = ((v->lfsr >> 0) ^ (v->lfsr >> 2) ^ (v->lfsr >> 3) ^ (v->lfsr >> 5)) & 1;
            v->lfsr = (v->lfsr >> 1) | (bit << 15);
            v->noise = (v->lfsr & 1) ? 1.0f : -1.0f;
        }
        float w;
        switch (v->cur_wave) {
        case W_SQ: w = v->phase < 0.5f ? 1 : -1; break;
        case W_SQ25: w = v->phase < 0.25f ? 1 : -1; break;
        case W_SQ12: w = v->phase < 0.125f ? 1 : -1; break;
        case W_TRI: w = 4 * fabsf(v->phase - 0.5f) - 1; break;
        default: w = v->noise; break;
        }
        out = w * env * v->amp;
        v->f *= v->k;
        v->amp *= v->dk;
    }
    v->s++;
    return out;
}

/* Fills one stereo block; returns true when a bass note started in it (BGM: the jelly bobs along). */
static bool synth_render(synth_t *y, int16_t *out)
{
    bool beat = false;
    for (int i = 0; i < BLOCK_FRAMES; i++) {
        float mix = 0;
        for (int k = 0; k < y->n; k++) mix += voice_sample(&y->v[k]);
        mix *= 0.30f * y->gain;
        if (mix > 0.95f) mix = 0.95f;
        else if (mix < -0.95f) mix = -0.95f;
        const int16_t s = (int16_t)(mix * 32767);
        out[2 * i] = out[2 * i + 1] = s;
    }
    if (y->n > 1 && y->v[1].onset) beat = true;
    for (int k = 0; k < y->n; k++) y->v[k].onset = false;
    return beat;
}

/* ---------------- custom sfxr effects ---------------- */

/* A pasted sfxr sound can replace any built-in effect; slot SFX_COUNT is the panel preview. */
#define SLOT_PREVIEW SFX_COUNT
#define SFXR_LEVEL 0.5f /* sfxr runs hotter than the jingles */
static sfxr_params_t s_custom[SFX_COUNT + 1];
static bool s_has_custom[SFX_COUNT + 1];

typedef struct {
    sfxr_t *eng; /* PSRAM: the phaser buffer alone is 4 KB */
    float acc, prev, cur;
} sfxr_voice_t;

static void custom_key(int slot, char *key) { snprintf(key, 8, "sx%d", slot); }

static void custom_load(void)
{
    nvs_handle_t h;
    if (nvs_open("slime", NVS_READONLY, &h) != ESP_OK) return;
    for (int i = 0; i < SFX_COUNT; i++) {
        char key[8];
        custom_key(i, key);
        sfxr_params_t p;
        size_t len = sizeof p;
        if (nvs_get_blob(h, key, &p, &len) == ESP_OK && len == sizeof p) {
            sfxr_sanitize(&p);
            s_custom[i] = p;
            s_has_custom[i] = true;
        }
    }
    nvs_close(h);
}

/* Renders one block from the sfxr engine, resampled 44.1 -> 48 kHz; false once it has ended. */
static bool sfxr_render(sfxr_voice_t *v, int16_t *out)
{
    const float step = (float)SFXR_RATE / RATE;
    for (int i = 0; i < BLOCK_FRAMES; i++) {
        v->acc += step;
        while (v->acc >= 1) {
            v->acc -= 1;
            v->prev = v->cur;
            v->cur = sfxr_next(v->eng);
        }
        const float x = (v->prev + (v->cur - v->prev) * v->acc) * SFXR_LEVEL;
        const int16_t s = (int16_t)(x * 32767);
        out[2 * i] = out[2 * i + 1] = s;
    }
    return v->eng->playing || v->cur != 0;
}

/* ---------------- task ---------------- */

static QueueHandle_t s_q;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static audio_state_t s_st;
static volatile bool s_mic_on = true, s_dance = true, s_sound_on = true;
static volatile uint8_t s_sens = 5, s_volume = 45;
static volatile uint32_t s_cfg_gen, s_hold_until;
static volatile int s_bgm_req = -1; /* BGM_COUNT = stop */
static volatile bool s_muted;       /* night mode: nothing plays, whatever the settings say */

static inline uint32_t ms_now(void) { return (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount()); }

/* push-to-talk recording, written by the audio task, read by the web server */
#define REC_CAP (AUDIO_REC_RATE * AUDIO_REC_MAX_S)
#define REC_DECIM (RATE / AUDIO_REC_RATE)
static int16_t *s_rec;
static volatile bool s_rec_on, s_rec_ready;
static volatile size_t s_rec_len;
static volatile uint32_t s_rec_seq;
static inline bool before(uint32_t now, uint32_t t) { return (int32_t)(now - t) < 0; }

typedef struct {
    float floor, prev, slow;
    int state;            /* 0 idle, 1 onset seen */
    float onset_db;
    uint32_t onset, last_clap, pending, last_beat;
    uint32_t last_rise; /* any sharp rise in level, claps and key clicks alike */
    int extra;          /* sharp rises since a clap went pending: typing, not clapping */
    int claps;
} mic_t;

static void mic_block(mic_t *m, const int16_t *pcm, bool quiet, uint32_t now)
{
    uint64_t acc = 0; /* integer: the FPU is single precision, doubles would be soft-float */
    for (int i = 0; i < BLOCK_FRAMES; i++) {
        const int32_t v = pcm[2 * i];
        acc += (uint32_t)(v * v);
    }
    const float rms = sqrtf((float)(acc / BLOCK_FRAMES));
    const float db = 20 * log10f(rms / 32768.0f + 1e-6f);
    m->floor += (db - m->floor) * (db < m->floor ? 0.1f : 0.002f); /* falls fast, rises over ~5 s */
    if (m->floor < -90) m->floor = -90;
    m->slow += (db - m->slow) * 0.05f;

    const float th = 34.0f - 2.0f * s_sens; /* dB above the floor: sens 1..10 -> 32..14 */
    const bool rise = db - m->prev > 6 && db - m->floor > 8;
    if (quiet) {
        m->state = 0;
        m->pending = 0;
        m->last_rise = now;
    } else if (m->state == 0) {
        if (rise) {
            const bool isolated = now - m->last_rise > ISOLATE_MS;
            const bool second = m->pending && m->extra == 0; /* may be the 2nd of a double clap */
            if (db - m->floor > th && db - m->prev > 9 && (isolated || second)) {
                m->state = 1;
                m->onset_db = db;
                m->onset = now;
            } else if (m->pending) {
                m->extra++;
            }
            m->last_rise = now;
        }
    } else if (now - m->onset > CLAP_MAX_MS) {
        m->state = 0; /* still loud: speech or music, not a clap */
        if (m->pending) m->extra++;
    } else if (db < m->onset_db - 12 || db - m->floor < th * 0.5f) {
        m->state = 0;
        m->claps++;
        if (m->pending && now - m->pending >= DOUBLE_MIN_MS && now - m->pending <= DOUBLE_MAX_MS) {
            m->pending = 0;
            sensors_post(SEV_DOUBLE_CLAP, 0, 0);
        } else {
            m->pending = now | 1; /* a single clap is reported once no second one follows */
            m->extra = 0;
        }
    }
    if (m->pending && now - m->pending > DOUBLE_MAX_MS) {
        if (m->extra == 0) sensors_post(SEV_CLAP, 0, 0); /* more clicks followed: it was typing */
        m->pending = 0;
    }
    if (s_dance && !quiet && db - m->floor > 12 && db - m->slow > 6 && now - m->last_beat > BEAT_GAP_MS) {
        m->last_beat = now;
        sensors_post(SEV_BEAT, 0, db - m->slow);
    }
    m->prev = db;

    portENTER_CRITICAL(&s_lock);
    s_st.db = db;
    s_st.floor = m->floor;
    s_st.claps = m->claps;
    portEXIT_CRITICAL(&s_lock);
}

static void audio_task(void *arg)
{
    esp_codec_dev_handle_t mic = bsp_audio_codec_microphone_init();
    esp_codec_dev_handle_t spk = bsp_audio_codec_speaker_init();
    esp_codec_dev_sample_info_t fmt = {.sample_rate = RATE, .bits_per_sample = 16, .channel = 2};
    if (!mic || !spk || esp_codec_dev_open(mic, &fmt) != ESP_CODEC_DEV_OK || esp_codec_dev_open(spk, &fmt) != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "codec unavailable (mic %p, speaker %p)", mic, spk);
        vTaskDelete(NULL);
    }
    esp_codec_dev_set_in_gain(mic, MIC_GAIN_DB);
    s_hold_until = ms_now() + 1500; /* amplifier pop and the noise floor settling are not claps */
    portENTER_CRITICAL(&s_lock);
    s_st.ok = true;
    portEXIT_CRITICAL(&s_lock);

    /* PSRAM: internal RAM is reserved for LCD DMA */
    int16_t *in = heap_caps_calloc(BLOCK_FRAMES * 2, sizeof(int16_t), MALLOC_CAP_SPIRAM);
    int16_t *out = heap_caps_calloc(BLOCK_FRAMES * 2, sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!in || !out) {
        ESP_LOGW(TAG, "no memory for audio buffers");
        vTaskDelete(NULL);
    }
    mic_t m = {.floor = -60, .prev = -90, .slow = -60};
    synth_t y = {0};
    sfxr_voice_t fx = {.eng = heap_caps_calloc(1, sizeof(sfxr_t), MALLOC_CAP_SPIRAM)};
    static const float BGM_GAIN = 0.55f; /* humming, not a concert */
    uint32_t gen_seen = UINT32_MAX, play_end = 0;
    int tail = 0;
    esp_task_wdt_add(NULL); /* blocks every 10-20 ms; a stall here is what leaves the speaker buzzing */
    for (;;) {
        esp_task_wdt_reset();
        if (gen_seen != s_cfg_gen) {
            gen_seen = s_cfg_gen;
            esp_codec_dev_set_out_vol(spk, s_volume);
        }
        const int req = s_bgm_req;
        if (req >= 0) {
            s_bgm_req = -1;
            if (req < BGM_COUNT && s_sound_on && (!synth_active(&y) || y.bgm)) synth_load(&y, &BGM[req], BGM_GAIN, true);
            else if (req >= BGM_COUNT && y.bgm) synth_stop(&y);
        }
        const bool fx_on = fx.eng && fx.eng->playing;
        if ((!synth_active(&y) || y.bgm) && !fx_on) { /* effects cut the background tune short */
            int s;
            if (xQueueReceive(s_q, &s, 0) == pdTRUE && s_sound_on) {
                sfxr_params_t p;
                bool custom = false;
                portENTER_CRITICAL(&s_lock);
                if (s >= 0 && s <= SLOT_PREVIEW && s_has_custom[s]) {
                    p = s_custom[s];
                    custom = true;
                }
                portEXIT_CRITICAL(&s_lock);
                if (custom && fx.eng) {
                    synth_stop(&y);
                    sfxr_start(fx.eng, &p, esp_random());
                    fx.acc = fx.prev = fx.cur = 0;
                } else if (s < SFX_COUNT) {
                    synth_load(&y, &SOUNDS[s], 1.0f, false);
                }
            }
        }
        if (y.bgm && !s_sound_on) synth_stop(&y);
        if (!s_sound_on && fx.eng) fx.eng->playing = false;
        const bool playing = synth_active(&y) || (fx.eng && fx.eng->playing) || tail > 0;
        const bool rec = s_rec_on;
        const bool listen = s_mic_on || rec;
        if (!listen && !playing) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        const uint32_t now = ms_now();
        if (listen && esp_codec_dev_read(mic, in, BLOCK_BYTES) == ESP_CODEC_DEV_OK) {
            const bool quiet = rec || playing || before(now, play_end + AFTER_PLAY_MS) || before(now, s_hold_until);
            mic_block(&m, in, quiet, now);
            if (rec) { /* 48 kHz stereo -> 16 kHz mono: average each run of 3 left-channel samples */
                size_t n = s_rec_len;
                for (int i = 0; i + REC_DECIM <= BLOCK_FRAMES && n < REC_CAP; i += REC_DECIM) {
                    int32_t acc = 0;
                    for (int k = 0; k < REC_DECIM; k++) acc += in[2 * (i + k)];
                    s_rec[n++] = (int16_t)(acc / REC_DECIM);
                }
                s_rec_len = n;
                if (n >= REC_CAP) s_rec_on = false; /* full: the main loop notices and wraps up */
            }
        }
        if (playing) {
            if (fx.eng && fx.eng->playing) {
                sfxr_render(&fx, out);
                tail = 4;
            } else if (synth_active(&y)) {
                if (synth_render(&y, out) && y.bgm && s_dance) sensors_post(SEV_BEAT, 0, 7);
                tail = 4; /* a few blocks of silence flush the DMA ring */
            } else {
                memset(out, 0, BLOCK_BYTES);
                tail--;
            }
            esp_codec_dev_write(spk, out, BLOCK_BYTES);
            play_end = ms_now();
        }
        portENTER_CRITICAL(&s_lock);
        s_st.mic = listen;
        s_st.playing = playing;
        s_st.bgm = y.bgm && synth_active(&y);
        portEXIT_CRITICAL(&s_lock);
    }
}

bool audio_rec_start(void)
{
    if (!s_rec) s_rec = heap_caps_malloc(REC_CAP * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!s_rec) return false;
    portENTER_CRITICAL(&s_lock);
    s_rec_ready = false; /* the buffer is about to be overwritten */
    s_rec_len = 0;
    s_rec_on = true;
    portEXIT_CRITICAL(&s_lock);
    return true;
}

uint32_t audio_rec_stop(uint32_t min_ms)
{
    portENTER_CRITICAL(&s_lock);
    s_rec_on = false;
    const uint32_t ms = (uint32_t)(s_rec_len * 1000ULL / AUDIO_REC_RATE);
    if (ms >= min_ms) {
        s_rec_seq++;
        s_rec_ready = true;
    }
    portEXIT_CRITICAL(&s_lock);
    return ms;
}

bool audio_rec_active(void) { return s_rec_on; }

void audio_rec_get(audio_rec_state_t *out)
{
    portENTER_CRITICAL(&s_lock);
    out->seq = s_rec_seq;
    out->ready = s_rec_ready;
    out->rec = s_rec_on;
    portEXIT_CRITICAL(&s_lock);
}

const int16_t *audio_rec_take(uint32_t seq, size_t *samples)
{
    const int16_t *p = NULL;
    portENTER_CRITICAL(&s_lock);
    if (s_rec && s_rec_ready && seq == s_rec_seq && !s_rec_on) {
        s_rec_ready = false;
        *samples = s_rec_len;
        p = s_rec;
    }
    portEXIT_CRITICAL(&s_lock);
    return p;
}

void audio_start(void)
{
    custom_load();
    s_q = xQueueCreate(4, sizeof(int));
    if (!s_q || xTaskCreatePinnedToCore(audio_task, "audio", 4096, NULL, 4, NULL, 0) != pdPASS) {
        ESP_LOGW(TAG, "audio task");
    }
}

void audio_configure(bool mic, uint8_t clap_sens, bool dance, bool sound, uint8_t volume)
{
    s_mic_on = mic;
    s_sens = clap_sens;
    s_dance = dance;
    s_sound_on = sound;
    s_volume = volume;
    s_cfg_gen++;
}

void audio_play(sfx_t s)
{
    if (!s_q || !s_sound_on || s_muted || s >= SFX_COUNT) return;
    if (s == SFX_BLIP && (uxQueueMessagesWaiting(s_q) || s_st.playing)) return;
    const int slot = s;
    xQueueSend(s_q, &slot, 0);
}

void audio_play_bgm(bgm_t b)
{
    if (s_sound_on && !s_muted && b < BGM_COUNT) s_bgm_req = b;
}

void audio_stop_bgm(void) { s_bgm_req = BGM_COUNT; }

void audio_set_muted(bool muted)
{
    s_muted = muted;
    if (muted) s_bgm_req = BGM_COUNT;
}

bool audio_set_custom(int slot, const sfxr_params_t *p)
{
    if (slot < 0 || slot > SLOT_PREVIEW) return false;
    sfxr_params_t q;
    if (p) {
        q = *p;
        sfxr_sanitize(&q);
    }
    portENTER_CRITICAL(&s_lock);
    s_has_custom[slot] = p != NULL;
    if (p) s_custom[slot] = q;
    portEXIT_CRITICAL(&s_lock);
    if (slot == SLOT_PREVIEW) return true;
    nvs_handle_t h;
    if (nvs_open("slime", NVS_READWRITE, &h) != ESP_OK) return false;
    char key[8];
    custom_key(slot, key);
    const esp_err_t e = p ? nvs_set_blob(h, key, &q, sizeof q) : nvs_erase_key(h, key);
    const bool ok = (e == ESP_OK || e == ESP_ERR_NVS_NOT_FOUND) && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

static const char *const SFX_NAMES[SFX_COUNT] = {"levelup", "done", "hurt", "ask", "poke", "greet", "dizzy", "startle",
                                                 "sleep", "wake", "hello", "blip", "boot", "sulk", "shy"};

const char *audio_sfx_name(int s) { return s >= 0 && s < SFX_COUNT ? SFX_NAMES[s] : "?"; }

int audio_sfx_find(const char *name)
{
    for (int i = 0; i < SFX_COUNT; i++)
        if (!strcmp(name, SFX_NAMES[i])) return i;
    return -1;
}

bool audio_has_custom(int slot) { return slot >= 0 && slot < SFX_COUNT && s_has_custom[slot]; }

void audio_preview(const sfxr_params_t *p)
{
    if (!s_q || !s_sound_on) return;
    audio_set_custom(SLOT_PREVIEW, p);
    const int slot = SLOT_PREVIEW;
    xQueueSend(s_q, &slot, 0);
}

void audio_hold_off(uint32_t ms) { s_hold_until = ms_now() + ms; }

void audio_get(audio_state_t *out)
{
    portENTER_CRITICAL(&s_lock);
    *out = s_st;
    portEXIT_CRITICAL(&s_lock);
}
