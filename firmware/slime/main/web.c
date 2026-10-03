#include "web.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "config.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "inbox.h"
#include "net.h"
#include "esp_heap_caps.h"
#include "vision.h"
#include "audio.h"
#include "ota.h"

static const char *TAG = "web";

#define STATUS_MAX 4096
#define BODY_MAX 1024

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

static SemaphoreHandle_t s_mutex;
static char s_status[STATUS_MAX] = "{}";
static char s_wifi_ssid[33], s_wifi_psk[64];
static esp_timer_handle_t s_wifi_timer;

void web_publish_status(const char *json)
{
    if (!s_mutex || xSemaphoreTake(s_mutex, 0) != pdTRUE) return; /* a request is copying it; next time */
    strlcpy(s_status, json, sizeof s_status);
    xSemaphoreGive(s_mutex);
}

static esp_err_t read_body(httpd_req_t *req, char *buf, size_t len)
{
    if (req->content_len >= len) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body too large");
        return ESP_FAIL;
    }
    size_t got = 0;
    while (got < req->content_len) {
        const int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
            return ESP_FAIL;
        }
        got += r;
    }
    buf[got] = 0;
    return ESP_OK;
}

static esp_err_t send_json(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

/* Nod/shake tuning: GET /api/head?since=<ms> -> recent head samples. */
static esp_err_t h_head(httpd_req_t *req)
{
    char q[32], v[16];
    uint32_t since = 0;
    if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK && httpd_query_key_value(q, "since", v, sizeof v) == ESP_OK)
        since = (uint32_t)strtoul(v, NULL, 10);
    static vision_head_sample_t smp[96];
    const int n = vision_head_trace(smp, 96, since);
    static char js[96 * 96 + 16];
    int len = snprintf(js, sizeof js, "[");
    for (int i = 0; i < n && len < (int)sizeof js - 80; i++)
        len += snprintf(js + len, sizeof js - len, "%s[%lu,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%u]", i ? "," : "",
                        (unsigned long)smp[i].t, smp[i].yaw, smp[i].nod, smp[i].bx, smp[i].by, smp[i].f_yaw, smp[i].f_nod,
                        smp[i].thr_yaw, smp[i].thr_nod, smp[i].flags);
    snprintf(js + len, sizeof js - len, "]");
    return send_json(req, js);
}

/* Push-to-talk: GET /api/voice.wav?seq=<n> -> that recording once (16 kHz mono WAV), then 404.
 * Only the bridge's slime_buddy.py asks; a recording exists only after someone held the AI key. */
static esp_err_t h_voice(httpd_req_t *req)
{
    char q[32], v[16];
    uint32_t seq = 0;
    if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK && httpd_query_key_value(q, "seq", v, sizeof v) == ESP_OK)
        seq = (uint32_t)strtoul(v, NULL, 10);
    size_t n = 0;
    const int16_t *pcm = audio_rec_take(seq, &n);
    if (!pcm) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such recording");
    const uint32_t bytes = n * 2, rate = AUDIO_REC_RATE, brate = AUDIO_REC_RATE * 2, riff = 36 + bytes;
    uint8_t h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 1, 0,
                     0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 16, 0, 'd', 'a', 't', 'a'};
    memcpy(h + 4, &riff, 4); /* little-endian, like the ESP32 */
    memcpy(h + 24, &rate, 4);
    memcpy(h + 28, &brate, 4);
    memcpy(h + 40, &bytes, 4);
    httpd_resp_set_type(req, "audio/wav");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (httpd_resp_send_chunk(req, (const char *)h, sizeof h) != ESP_OK) return ESP_FAIL;
    for (size_t off = 0; off < bytes; off += 4096) {
        const size_t len = bytes - off < 4096 ? bytes - off : 4096;
        if (httpd_resp_send_chunk(req, (const char *)pcm + off, len) != ESP_OK) return ESP_FAIL;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* Spoken answer: POST /api/speak, body = 16 kHz mono 16-bit little-endian PCM (no header), up to
 * AUDIO_SPEAK_MAX_S. 409 when it will not be played (speech off, night mode, no codec): the bridge
 * then shows the text alone. An empty body stops what is being said. */
static esp_err_t h_speak(httpd_req_t *req)
{
    const size_t len = req->content_len;
    if (len > AUDIO_SPEAK_RATE * 2 * AUDIO_SPEAK_MAX_S || len % 2) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "need 16 kHz mono s16le PCM, at most 20 s");
    }
    slime_cfg_t c;
    cfg_get(&c);
    if (len == 0) {
        audio_speak_stop();
        return send_json(req, "{\"ok\":true}");
    }
    char *pcm = c.speak ? heap_caps_malloc(len, MALLOC_CAP_SPIRAM) : NULL;
    size_t got = 0;
    while (got < len) { /* read it all even when it is not wanted: the connection stays usable */
        char sink[256];
        const size_t want = pcm ? len - got : (len - got < sizeof sink ? len - got : sizeof sink);
        const int r = httpd_req_recv(req, pcm ? pcm + got : sink, want);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
            free(pcm);
            return ESP_FAIL;
        }
        got += r;
    }
    if (!pcm || !audio_speak((int16_t *)pcm, len / 2)) { /* audio_speak frees it when it says no */
        httpd_resp_set_status(req, "409 Conflict");
        return send_json(req, c.speak ? "{\"ok\":false,\"why\":\"muted\"}" : "{\"ok\":false,\"why\":\"off\"}");
    }
    char out[48];
    snprintf(out, sizeof out, "{\"ok\":true,\"ms\":%u}", (unsigned)(len / 2 * 1000 / AUDIO_SPEAK_RATE));
    return send_json(req, out);
}

static esp_err_t h_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start - 1); /* EMBED_TXTFILES adds a NUL */
}

static esp_err_t h_status(httpd_req_t *req)
{
    static char copy[STATUS_MAX];
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    strlcpy(copy, s_status, sizeof copy);
    xSemaphoreGive(s_mutex);
    return send_json(req, copy);
}

#define CFG_FIELDS(X)                                                                                                  \
    X(screen_bright, num) X(sleep_bright, num) X(led_bright, num) X(idle_breath, bool) X(motor, bool) X(sleep_min, num) \
    X(tilt, bool) X(mic, bool) X(clap_sens, num) X(dance, bool) X(sound, bool) X(volume, num) X(text_blip, bool)        \
    X(show_fps, bool) X(camera, bool) X(sit_min, num) X(bgm, bool) X(cam_pip, bool) X(night_start, num) X(night_end, num) X(focus_min, num) X(ai_comment, bool) X(voice, bool) X(lang, num) X(wake, bool) X(scene, num) \
    X(speak, bool) X(speak_pitch, num) X(face, bool) X(gesture, bool)

static esp_err_t send_config(httpd_req_t *req)
{
    slime_cfg_t c;
    cfg_get(&c);
    cJSON *o = cJSON_CreateObject();
#define ADD_num(k) cJSON_AddNumberToObject(o, #k, c.k);
#define ADD_bool(k) cJSON_AddBoolToObject(o, #k, c.k);
#define ADD(k, t) ADD_##t(k)
    CFG_FIELDS(ADD)
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    const esp_err_t e = send_json(req, s ? s : "{}");
    cJSON_free(s);
    return e;
}

static esp_err_t h_config_get(httpd_req_t *req) { return send_config(req); }

static esp_err_t h_config_post(httpd_req_t *req)
{
    char body[BODY_MAX];
    if (read_body(req, body, sizeof body) != ESP_OK) return ESP_FAIL;
    cJSON *o = cJSON_Parse(body);
    if (!cJSON_IsObject(o)) {
        cJSON_Delete(o);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "expected a JSON object");
    }
    slime_cfg_t c;
    cfg_get(&c);
    const cJSON *v;
#define SET_num(k) if ((v = cJSON_GetObjectItem(o, #k)) && cJSON_IsNumber(v)) c.k = (typeof(c.k))(v->valuedouble < 0 ? 0 : v->valuedouble > 255 ? 255 : v->valuedouble);
#define SET_bool(k) if ((v = cJSON_GetObjectItem(o, #k)) && cJSON_IsBool(v)) c.k = cJSON_IsTrue(v);
#define SET(k, t) SET_##t(k)
    CFG_FIELDS(SET)
    cJSON_Delete(o);
    cfg_set(&c);
    return send_config(req);
}

/* The bridge's language model and speech-to-text endpoints (see config.h). API keys are write-only
 * for the panel: a GET shows whether one is set, and returns it only to a caller that has the
 * update token (the bridge reads bridge/.ota_token). */
static esp_err_t send_ai(httpd_req_t *req)
{
    char tok[24];
    const bool trusted = ota_token()[0] && httpd_req_get_hdr_value_str(req, "X-OTA-Token", tok, sizeof tok) == ESP_OK &&
                         !strcmp(tok, ota_token());
    cJSON *o = cJSON_CreateObject();
    for (int f = 0; f < AI_FIELD_COUNT; f++) {
        const char *name = ai_field_name(f), *val = ai_get(f);
        if (ai_field_secret(f)) {
            char set[24];
            snprintf(set, sizeof set, "%s_set", name);
            cJSON_AddBoolToObject(o, set, val[0] != 0);
            if (!trusted) continue;
        }
        cJSON_AddStringToObject(o, name, val);
    }
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const esp_err_t e = send_json(req, s ? s : "{}");
    if (s) memset(s, 0, strlen(s));
    cJSON_free(s);
    return e;
}

static esp_err_t h_ai_get(httpd_req_t *req) { return send_ai(req); }

static esp_err_t h_ai_post(httpd_req_t *req)
{
    static char body[BODY_MAX * 3]; /* ten fields of up to AI_VALUE_MAX; static: one request at a time */
    if (read_body(req, body, sizeof body) != ESP_OK) return ESP_FAIL;
    cJSON *o = cJSON_Parse(body);
    memset(body, 0, sizeof body);
    const char *bad = cJSON_IsObject(o) ? NULL : "expected a JSON object";
    for (int f = 0; f < AI_FIELD_COUNT && !bad; f++) {
        const cJSON *v = cJSON_GetObjectItem(o, ai_field_name(f));
        if (v && !cJSON_IsString(v)) bad = "values must be strings";
    }
    /* A key belongs to the service it was entered for: when the address changes and no key comes
     * with it, the old key is dropped, so that nobody on the network can redirect it elsewhere. */
    static const struct { ai_field_t url, key; } PAIRS[] = {{AI_LLM_URL, AI_LLM_KEY}, {AI_STT_URL, AI_STT_KEY}, {AI_TTS_URL, AI_TTS_KEY}};
    for (size_t i = 0; i < sizeof PAIRS / sizeof PAIRS[0] && !bad; i++) {
        const cJSON *u = cJSON_GetObjectItem(o, ai_field_name(PAIRS[i].url));
        if (u && strcmp(u->valuestring, ai_get(PAIRS[i].url)) && !cJSON_GetObjectItem(o, ai_field_name(PAIRS[i].key))) {
            ai_set(PAIRS[i].key, "");
        }
    }
    for (int f = 0; f < AI_FIELD_COUNT && !bad; f++) {
        const cJSON *v = cJSON_GetObjectItem(o, ai_field_name(f));
        if (!v) continue;
        const esp_err_t e = ai_set(f, v->valuestring);
        if (e == ESP_ERR_INVALID_ARG) bad = "too long, or an address that does not start with http:// or https://";
        else if (e != ESP_OK) bad = "could not save";
    }
    cJSON_Delete(o);
    if (bad) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, bad);
    return send_ai(req);
}

/* Debug: what the face detector sees, as a BMP (BGR888 rows bottom-up, 4-byte padded). */
static esp_err_t h_cam(httpd_req_t *req)
{
    const size_t cap = 400 * 400 * 3;
    uint8_t *px = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    int w = 0, h = 0;
    if (!px || !vision_snapshot(px, cap, &w, &h)) {
        free(px);
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no camera frame yet");
    }
    const int row = (w * 3 + 3) & ~3;
    uint8_t hdr[54] = {'B', 'M'};
    const uint32_t size = 54 + row * h;
    memcpy(hdr + 2, &size, 4);
    hdr[10] = 54;
    hdr[14] = 40;
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1;
    hdr[28] = 24;
    httpd_resp_set_type(req, "image/bmp");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send_chunk(req, (const char *)hdr, sizeof hdr);
    uint8_t line[400 * 3 + 4] = {0};
    for (int y = h - 1; y >= 0; y--) {
        memcpy(line, px + (size_t)y * w * 3, w * 3);
        httpd_resp_send_chunk(req, (const char *)line, row);
    }
    free(px);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t h_cmd(httpd_req_t *req)
{
    char body[BODY_MAX];
    if (read_body(req, body, sizeof body) != ESP_OK) return ESP_FAIL;
    int queued = 0;
    for (char *save = NULL, *line = strtok_r(body, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
        if (!strncmp(line, "wifi", 4)) continue; /* credentials only via /api/wifi */
        queued += inbox_push(line, SRC_WIFI);
    }
    char out[32];
    snprintf(out, sizeof out, "{\"queued\":%d}", queued);
    return send_json(req, out);
}

static void wifi_apply(void *arg)
{
    net_set_wifi(s_wifi_ssid, s_wifi_psk);
    memset(s_wifi_psk, 0, sizeof s_wifi_psk);
}

static esp_err_t h_wifi(httpd_req_t *req)
{
    char body[256];
    if (read_body(req, body, sizeof body) != ESP_OK) return ESP_FAIL;
    cJSON *o = cJSON_Parse(body);
    const cJSON *ssid = cJSON_GetObjectItem(o, "ssid"), *psk = cJSON_GetObjectItem(o, "psk");
    const bool ok = cJSON_IsString(ssid) && cJSON_IsString(psk) && ssid->valuestring[0] &&
                    strlen(ssid->valuestring) < sizeof s_wifi_ssid && strlen(psk->valuestring) < sizeof s_wifi_psk;
    if (ok) {
        strlcpy(s_wifi_ssid, ssid->valuestring, sizeof s_wifi_ssid);
        strlcpy(s_wifi_psk, psk->valuestring, sizeof s_wifi_psk);
        memset(psk->valuestring, 0, strlen(psk->valuestring));
    }
    memset(body, 0, sizeof body);
    cJSON_Delete(o);
    if (!ok) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "need ssid (1-32 bytes) and psk (0-63 bytes)");
    /* reply first: switching networks drops this connection */
    esp_timer_stop(s_wifi_timer);
    esp_timer_start_once(s_wifi_timer, 500 * 1000);
    return send_json(req, "{\"ok\":true}");
}

esp_err_t web_start(void)
{
    s_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_mutex, ESP_ERR_NO_MEM, TAG, "mutex");
    const esp_timer_create_args_t ta = {.callback = wifi_apply, .name = "wifi_apply"};
    ESP_RETURN_ON_ERROR(esp_timer_create(&ta, &s_wifi_timer), TAG, "timer");

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.core_id = 0; /* core 1 belongs to the renderer */
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 16;
    httpd_handle_t srv;
    ESP_RETURN_ON_ERROR(httpd_start(&srv, &cfg), TAG, "httpd");
    const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = h_index},
        {.uri = "/api/status", .method = HTTP_GET, .handler = h_status},
        {.uri = "/api/config", .method = HTTP_GET, .handler = h_config_get},
        {.uri = "/api/config", .method = HTTP_POST, .handler = h_config_post},
        {.uri = "/api/cmd", .method = HTTP_POST, .handler = h_cmd},
        {.uri = "/api/wifi", .method = HTTP_POST, .handler = h_wifi},
        {.uri = "/api/cam.bmp", .method = HTTP_GET, .handler = h_cam},
        {.uri = "/api/head", .method = HTTP_GET, .handler = h_head},
        {.uri = "/api/voice.wav", .method = HTTP_GET, .handler = h_voice},
        {.uri = "/api/speak", .method = HTTP_POST, .handler = h_speak},
        {.uri = "/api/ota", .method = HTTP_POST, .handler = ota_http_handler},
        {.uri = "/api/ai", .method = HTTP_GET, .handler = h_ai_get},
        {.uri = "/api/ai", .method = HTTP_POST, .handler = h_ai_post},
    };
    for (size_t i = 0; i < sizeof uris / sizeof uris[0]; i++) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(srv, &uris[i]), TAG, "uri %s", uris[i].uri);
    }
    return ESP_OK;
}
