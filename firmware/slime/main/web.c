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

/* Custom sfxr effects: GET lists which effects are replaced; POST
 * {"slot":"<effect>|preview","params":{jsfxr JSON}} sets/previews, {"slot":"<effect>","reset":true} restores. */
static esp_err_t h_sfxr_get(httpd_req_t *req)
{
    char js[512];
    int n = snprintf(js, sizeof js, "{\"slots\":[");
    for (int i = 0; i < SFX_COUNT && n < (int)sizeof js - 48; i++)
        n += snprintf(js + n, sizeof js - n, "%s{\"name\":\"%s\",\"custom\":%s}", i ? "," : "", audio_sfx_name(i),
                      audio_has_custom(i) ? "true" : "false");
    snprintf(js + n, sizeof js - n, "]}");
    return send_json(req, js);
}

static esp_err_t h_sfxr_post(httpd_req_t *req)
{
    char body[BODY_MAX * 2];
    if (read_body(req, body, sizeof body) != ESP_OK) return ESP_FAIL;
    cJSON *o = cJSON_Parse(body);
    const cJSON *slot = cJSON_GetObjectItem(o, "slot");
    if (!cJSON_IsString(slot)) {
        cJSON_Delete(o);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "need slot");
    }
    const bool preview = !strcmp(slot->valuestring, "preview");
    const int idx = preview ? -1 : audio_sfx_find(slot->valuestring);
    if (!preview && idx < 0) {
        cJSON_Delete(o);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unknown slot");
    }
    bool ok;
    if (!preview && cJSON_IsTrue(cJSON_GetObjectItem(o, "reset"))) {
        ok = audio_set_custom(idx, NULL);
    } else {
        const cJSON *ps = cJSON_GetObjectItem(o, "params");
        if (!cJSON_IsObject(ps)) ps = o; /* the jsfxr JSON itself is accepted too */
        sfxr_params_t p;
        sfxr_defaults(&p);
        const cJSON *v = cJSON_GetObjectItem(ps, "wave_type");
        if (cJSON_IsNumber(v)) p.wave_type = v->valueint;
#define SFXR_GET(name, lo, hi) if ((v = cJSON_GetObjectItem(ps, #name)) && cJSON_IsNumber(v)) p.name = (float)v->valuedouble;
        SFXR_FIELDS(SFXR_GET)
#undef SFXR_GET
        sfxr_sanitize(&p);
        if (preview) {
            audio_preview(&p);
            ok = true;
        } else {
            ok = audio_set_custom(idx, &p);
            if (ok) audio_play((sfx_t)idx);
        }
    }
    cJSON_Delete(o);
    return ok ? send_json(req, "{\"ok\":true}") : httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
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
    X(show_fps, bool) X(camera, bool) X(sit_min, num) X(bgm, bool) X(cam_pip, bool) X(night_start, num) X(night_end, num) X(focus_min, num)

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
        {.uri = "/api/sfxr", .method = HTTP_GET, .handler = h_sfxr_get},
        {.uri = "/api/head", .method = HTTP_GET, .handler = h_head},
        {.uri = "/api/ota", .method = HTTP_POST, .handler = ota_http_handler},
        {.uri = "/api/sfxr", .method = HTTP_POST, .handler = h_sfxr_post},
    };
    for (size_t i = 0; i < sizeof uris / sizeof uris[0]; i++) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(srv, &uris[i]), TAG, "uri %s", uris[i].uri);
    }
    return ESP_OK;
}
