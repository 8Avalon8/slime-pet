#include "config.h"

#include <stddef.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "cfg";

#define CFG_VERSION 9 /* v1 = everything up to show_fps; later fields are appended */

typedef struct {
    uint8_t version;
    slime_cfg_t c;
} blob_t;

static const slime_cfg_t DEFAULTS = {
    .screen_bright = 100,
    .sleep_bright = 25,
    .led_bright = 128,
    .idle_breath = true,
    .motor = true,
    .sleep_min = 10,
    .tilt = true,
    .mic = true,
    .clap_sens = 5,
    .dance = true,
    .sound = true,
    .volume = 45,
    .text_blip = false,
    .show_fps = false,
    .camera = true,
    .sit_min = 50,
    .bgm = true,
    .night_start = 23,
    .night_end = 7,
    .focus_min = 25,
    .ai_comment = true,
    .voice = true,
    .lang = 0,
    .wake = true,
};

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static slime_cfg_t s_cfg;
static volatile uint32_t s_gen;

static uint8_t clamp8(int v, int lo, int hi) { return (uint8_t)(v < lo ? lo : (v > hi ? hi : v)); }

static void sanitize(slime_cfg_t *c)
{
    c->screen_bright = clamp8(c->screen_bright, 10, 100);
    c->sleep_bright = clamp8(c->sleep_bright, 0, 100);
    c->sleep_min = clamp8(c->sleep_min, 1, 120);
    c->clap_sens = clamp8(c->clap_sens, 1, 10);
    c->volume = clamp8(c->volume, 0, 100);
    c->sit_min = clamp8(c->sit_min, 0, 120);
    c->night_start = clamp8(c->night_start, 0, 23);
    c->night_end = clamp8(c->night_end, 0, 23);
    c->focus_min = clamp8(c->focus_min, 5, 90);
    c->lang = clamp8(c->lang, 0, 1);
}

/* ---- AI endpoints for the bridge: plain NVS strings next to the settings blob ---- */

static const struct {
    const char *name, *key; /* JSON name, NVS key */
    uint8_t max;
    bool url, secret;
} AI_FIELDS[AI_FIELD_COUNT] = {
    [AI_LLM_URL] = {"llm_url", "ai_lurl", 128, true, false},
    [AI_LLM_MODEL] = {"llm_model", "ai_lmodel", 64, false, false},
    [AI_LLM_KEY] = {"llm_key", "ai_lkey", AI_VALUE_MAX, false, true},
    [AI_STT_URL] = {"stt_url", "ai_surl", 128, true, false},
    [AI_STT_MODEL] = {"stt_model", "ai_smodel", 64, false, false},
    [AI_STT_KEY] = {"stt_key", "ai_skey", AI_VALUE_MAX, false, true},
};
static char s_ai[AI_FIELD_COUNT][AI_VALUE_MAX + 1];

static void ai_load(nvs_handle_t h)
{
    for (int f = 0; f < AI_FIELD_COUNT; f++) {
        size_t len = sizeof s_ai[f];
        if (nvs_get_str(h, AI_FIELDS[f].key, s_ai[f], &len) != ESP_OK) s_ai[f][0] = 0;
    }
}

const char *ai_field_name(ai_field_t f) { return AI_FIELDS[f].name; }
bool ai_field_secret(ai_field_t f) { return AI_FIELDS[f].secret; }
const char *ai_get(ai_field_t f) { return s_ai[f]; }

esp_err_t ai_set(ai_field_t f, const char *val)
{
    if (strlen(val) > AI_FIELDS[f].max) return ESP_ERR_INVALID_ARG;
    for (const char *p = val; *p; p++) {
        if ((unsigned char)*p < 0x20 || *p == 0x7f) return ESP_ERR_INVALID_ARG;
    }
    if (AI_FIELDS[f].url && val[0] && strncmp(val, "http://", 7) && strncmp(val, "https://", 8)) return ESP_ERR_INVALID_ARG;
    if (!strcmp(val, s_ai[f])) return ESP_OK;
    nvs_handle_t h;
    esp_err_t e = nvs_open("slime", NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = val[0] ? nvs_set_str(h, AI_FIELDS[f].key, val) : nvs_erase_key(h, AI_FIELDS[f].key);
    if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK; /* clearing what was never stored */
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    if (e == ESP_OK) strlcpy(s_ai[f], val, sizeof s_ai[f]);
    else ESP_LOGW(TAG, "saving %s failed", AI_FIELDS[f].name);
    return e;
}

esp_err_t cfg_init(void)
{
    s_cfg = DEFAULTS;
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        e = nvs_flash_init();
    }
    if (e != ESP_OK) return e;
    nvs_handle_t h;
    if (nvs_open("slime", NVS_READONLY, &h) != ESP_OK) return ESP_OK; /* first boot */
    ai_load(h);
    blob_t b;
    size_t len = sizeof b;
    if (nvs_get_blob(h, "cfg", &b, &len) == ESP_OK && len >= 2 && b.version >= 1 && b.version <= CFG_VERSION) {
        /* older blobs are a prefix: keep their fields, defaults for the rest */
        memcpy(&s_cfg, &b.c, len - offsetof(blob_t, c) < sizeof s_cfg ? len - offsetof(blob_t, c) : sizeof s_cfg);
        sanitize(&s_cfg);
    }
    nvs_close(h);
    return ESP_OK;
}

void cfg_get(slime_cfg_t *out)
{
    portENTER_CRITICAL(&s_lock);
    *out = s_cfg;
    portEXIT_CRITICAL(&s_lock);
}

void cfg_set(const slime_cfg_t *in) { cfg_set_ex(in, true); }

void cfg_set_ex(const slime_cfg_t *in, bool persist)
{
    blob_t b = {.version = CFG_VERSION, .c = *in};
    sanitize(&b.c);
    portENTER_CRITICAL(&s_lock);
    s_cfg = b.c;
    portEXIT_CRITICAL(&s_lock);
    s_gen++;
    if (!persist) return;
    nvs_handle_t h;
    if (nvs_open("slime", NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_set_blob(h, "cfg", &b, sizeof b) != ESP_OK || nvs_commit(h) != ESP_OK) ESP_LOGW(TAG, "save failed");
        nvs_close(h);
    }
}

uint32_t cfg_gen(void) { return s_gen; }
