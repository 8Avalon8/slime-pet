/*
 * Stand-ins for the firmware's hardware modules (audio, haptic, sensors, vision, net, ota,
 * bootlog, usb_link), behind the same headers as main/. Sounds and vibrations are printed;
 * sensors are driven by sim_input(); there is no camera and Wi-Fi is always "connected".
 */
#include <ctype.h>
#include <math.h>
#include <time.h>

#include "sim.h"

#include "audio.h"
#include "wake.h"
#include "bootlog.h"
#include "haptic.h"
#include "inbox.h"
#include "net.h"
#include "ota.h"
#include "sensors.h"
#include "usb_link.h"
#include "vision.h"

static const char *TAG = "sim";

/* ---------------- audio: printed, not played ---------------- */

static const char *const SFX_NAMES[SFX_COUNT] = {"levelup", "done", "hurt", "ask", "poke", "greet", "dizzy", "startle",
                                                 "sleep", "wake", "hello", "blip", "boot", "sulk", "shy"};
static bool s_sound = true, s_muted, s_mic = true, s_custom[SFX_COUNT];

void audio_start(void) {}
void audio_configure(bool mic, uint8_t clap_sens, bool dance, bool sound, uint8_t volume)
{
    s_mic = mic;
    s_sound = sound;
}
void audio_play(sfx_t s)
{
    if (s_sound && !s_muted && s != SFX_BLIP) ESP_LOGI(TAG, "sound: %s%s", audio_sfx_name(s), s_custom[s] ? " (custom)" : "");
}
void audio_play_bgm(bgm_t b)
{
    if (s_sound && !s_muted) ESP_LOGI(TAG, "sound: background tune %d", (int)b);
}
void audio_stop_bgm(void) {}
void audio_set_muted(bool muted) { s_muted = muted; }
bool audio_set_custom(int slot, const sfxr_params_t *p)
{
    if (slot < 0 || slot >= SFX_COUNT) return false;
    s_custom[slot] = p != NULL;
    return true;
}
bool audio_has_custom(int slot) { return slot >= 0 && slot < SFX_COUNT && s_custom[slot]; }
const char *audio_sfx_name(int s) { return s >= 0 && s < SFX_COUNT ? SFX_NAMES[s] : "?"; }
int audio_sfx_find(const char *name)
{
    for (int i = 0; i < SFX_COUNT; i++)
        if (!strcmp(name, SFX_NAMES[i])) return i;
    return -1;
}
void audio_preview(const sfxr_params_t *p) { ESP_LOGI(TAG, "sound: sfxr preview"); }
void audio_hold_off(uint32_t ms) {}
void audio_get(audio_state_t *out) { *out = (audio_state_t){.ok = true, .mic = s_mic, .db = -60, .floor = -60}; }

/* push-to-talk: records silence for as long as the button is held, so the bridge's voice path runs */
static portMUX_TYPE s_rec_m = portMUX_INITIALIZER_UNLOCKED;
static int16_t s_rec_buf[AUDIO_REC_RATE * AUDIO_REC_MAX_S];
static audio_rec_state_t s_rec;
static int64_t s_rec_t0;
static size_t s_rec_len;

bool audio_rec_start(void)
{
    portENTER_CRITICAL(&s_rec_m);
    s_rec.rec = true;
    s_rec_t0 = esp_timer_get_time();
    portEXIT_CRITICAL(&s_rec_m);
    ESP_LOGI(TAG, "mic: recording (silence in the simulator)");
    return true;
}
uint32_t audio_rec_stop(uint32_t min_ms)
{
    portENTER_CRITICAL(&s_rec_m);
    int64_t ms = s_rec.rec ? (esp_timer_get_time() - s_rec_t0) / 1000 : 0;
    if (ms > AUDIO_REC_MAX_S * 1000) ms = AUDIO_REC_MAX_S * 1000;
    s_rec.rec = false;
    if (ms >= min_ms && ms > 0) {
        s_rec_len = (size_t)ms * AUDIO_REC_RATE / 1000;
        s_rec.seq++;
        s_rec.ready = true;
    }
    portEXIT_CRITICAL(&s_rec_m);
    return (uint32_t)ms;
}
void audio_rec_voice(uint32_t *speech_ms, uint32_t *quiet_ms) { *speech_ms = *quiet_ms = 0; }

/* no wake word in the simulator: start a conversation with the AI key */
esp_err_t wake_start(void) { return ESP_ERR_NOT_SUPPORTED; }
bool wake_available(void) { return false; }
void wake_enable(bool on) {}
void wake_feed(const int16_t *pcm, size_t samples) {}
bool wake_take(void) { return false; }

bool audio_rec_active(void)
{
    portENTER_CRITICAL(&s_rec_m);
    const bool on = s_rec.rec && esp_timer_get_time() - s_rec_t0 < AUDIO_REC_MAX_S * 1000000LL;
    portEXIT_CRITICAL(&s_rec_m);
    return on;
}
void audio_rec_get(audio_rec_state_t *out)
{
    portENTER_CRITICAL(&s_rec_m);
    *out = s_rec;
    portEXIT_CRITICAL(&s_rec_m);
}
const int16_t *audio_rec_take(uint32_t seq, size_t *samples)
{
    portENTER_CRITICAL(&s_rec_m);
    const bool ok = s_rec.ready && seq == s_rec.seq;
    if (ok) {
        s_rec.ready = false;
        *samples = s_rec_len;
    }
    portEXIT_CRITICAL(&s_rec_m);
    return ok ? s_rec_buf : NULL;
}

/* ---------------- haptic ---------------- */

static bool s_motor = true;
esp_err_t haptic_init(void) { return ESP_OK; }
void haptic_play(haptic_t h)
{
    static const char *const N[] = {"tick", "nudge", "hurt", "fanfare"};
    if (s_motor) ESP_LOGI(TAG, "buzz: %s", h < HAPTIC_COUNT ? N[h] : "?");
}
void haptic_set_enabled(bool on) { s_motor = on; }

/* ---------------- sensors: IMU and Interaction module, driven by sim_input() ---------------- */

static portMUX_TYPE s_sens_m = portMUX_INITIALIZER_UNLOCKED;
static sensors_state_t s_sens = {.imu_ok = true, .az = 1, .mod_ok = true, .light = 60};
#define EVQ 16
static sensor_ev_t s_evq[EVQ];
static int s_ev_head, s_ev_n;
static volatile int s_mood, s_led_bright = 128;

void sensors_start(void) {}
void sensors_get(sensors_state_t *out)
{
    portENTER_CRITICAL(&s_sens_m);
    *out = s_sens;
    portEXIT_CRITICAL(&s_sens_m);
}
bool sensors_next_event(sensor_ev_t *ev)
{
    portENTER_CRITICAL(&s_sens_m);
    const bool ok = s_ev_n > 0;
    if (ok) {
        *ev = s_evq[s_ev_head];
        s_ev_head = (s_ev_head + 1) % EVQ;
        s_ev_n--;
    }
    portEXIT_CRITICAL(&s_sens_m);
    return ok;
}
void sensors_post(sensor_ev_kind_t kind, float vx, float vy)
{
    portENTER_CRITICAL(&s_sens_m);
    if (s_ev_n < EVQ) {
        s_evq[(s_ev_head + s_ev_n) % EVQ] = (sensor_ev_t){kind, vx, vy};
        s_ev_n++;
    }
    portEXIT_CRITICAL(&s_sens_m);
}
void sensors_set_mood(led_mood_t mood) { s_mood = mood; }
void sensors_set_led_brightness(uint8_t level) { s_led_bright = level; }
int sim_led_mood(void) { return s_mood; }
int sim_led_brightness(void) { return s_led_bright; }

/* Colours from sensors.c, as one steady or breathing colour per mood (the real strip animates more). */
void sim_mood_rgb(int mood, double t, uint8_t rgb[3])
{
    static const uint8_t C[MOOD_COUNT][3] = {
        [MOOD_OFF] = {0, 0, 0},         [MOOD_IDLE] = {50, 130, 255},  [MOOD_CHARGE] = {255, 160, 30},
        [MOOD_POKE] = {255, 255, 255},  [MOOD_MELT] = {255, 40, 20},   [MOOD_METAL] = {190, 205, 230},
        [MOOD_THINK] = {40, 90, 255},   [MOOD_WORK] = {0, 200, 255},   [MOOD_WAIT] = {255, 140, 0},
        [MOOD_HURT] = {255, 0, 0},      [MOOD_HAPPY] = {60, 255, 120}, [MOOD_LEVELUP] = {255, 220, 60},
        [MOOD_DIZZY] = {200, 80, 255},
    };
    float k = 0.35f + 0.65f * (0.5f - 0.5f * cosf((float)t * 1.5708f));
    if (mood == MOOD_WAIT) k = fmod(t, 0.5) < 0.25 ? 1 : 0;
    const int m = mood >= 0 && mood < MOOD_COUNT ? mood : 0;
    for (int i = 0; i < 3; i++) rgb[i] = (uint8_t)(C[m][i] * k * s_led_bright / 255);
}

/* ---------------- vision: no camera plugged in ---------------- */

static bool s_cam_on;
int vision_head_trace(vision_head_sample_t *out, int max, uint32_t since) { return 0; }
void vision_start(void) {}
void vision_enable(bool on) { s_cam_on = on; }
void vision_get(vision_state_t *out)
{
    *out = (vision_state_t){.enabled = s_cam_on, .err = ESP_ERR_NOT_FOUND, .slot_presence = -1, .slot_desc = -1,
                            .slot_owner = -1, .slot_type = -1};
}
bool vision_snapshot(uint8_t *dst, size_t cap, int *w, int *h) { return false; }
void vision_set_preview(int mode) {}
bool vision_preview_copy(uint16_t *dst, int stride, int x0, int max_w, int max_h, uint32_t *seq, int *w, int *h) { return false; }

/* ---------------- net: always on the "simulator" network ---------------- */

static char s_ssid[33] = "simulator";
esp_err_t net_start(void) { return ESP_OK; }
esp_err_t net_set_wifi(const char *ssid, const char *psk)
{
    strlcpy(s_ssid, ssid, sizeof s_ssid);
    ESP_LOGI(TAG, "Wi-Fi set to \"%s\" (not really: the simulator stays where it is)", ssid);
    return ESP_OK;
}
void net_get(net_status_t *out)
{
    *out = (net_status_t){.state = NET_CONNECTED, .rssi = -40};
    strlcpy(out->ssid, s_ssid, sizeof out->ssid);
    strlcpy(out->ip, sim_opt.bind, sizeof out->ip);
}
const char *net_state_name(net_state_t s)
{
    static const char *const N[] = {"off", "no_config", "connecting", "connected"};
    return s <= NET_CONNECTED ? N[s] : "?";
}
bool net_local_time(struct tm *out)
{
    const time_t t = time(NULL);
    return localtime_r(&t, out) != NULL;
}

/* ---------------- ota, bootlog, usb_link ---------------- */

void ota_init(void) {}
const char *ota_token(void) { return "simulator"; }
esp_err_t ota_http_handler(httpd_req_t *req) { return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "no OTA in the simulator"); }
void ota_confirm_if_pending(void) {}
int ota_progress(void) { return -1; }
void ota_status_json(char *buf, size_t len) { snprintf(buf, len, "{\"part\":\"sim\",\"state\":\"valid\",\"ver\":\"simulator\",\"built\":\"%s\"}", __DATE__); }

void bootlog_init(void) {}
void bootlog_snapshot(uint32_t uptime_s, int voltage_mv, int current_ma, int soc) {}
void bootlog_json(char *buf, size_t len) { snprintf(buf, len, "[]"); }
void bootlog_crash_json(char *buf, size_t len) { snprintf(buf, len, "null"); }

esp_err_t usb_link_init(void) { return ESP_OK; }
void usb_link_poll(void) {}

/* stdin plays the USB console: lines go to the inbox as SRC_USB; "sim ..." drives the simulator */
static void stdin_task(void *arg)
{
    char line[256];
    while (fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0]) continue;
        char err[96];
        if (!strncmp(line, "sim ", 4)) {
            if (!sim_input(line + 4, err, sizeof err)) fprintf(stderr, "sim: %s\n%s", err, sim_help());
        } else if (!strcmp(line, "help") || !strcmp(line, "sim")) {
            fputs(sim_help(), stderr);
        } else if (!inbox_push(line, SRC_USB)) {
            fprintf(stderr, "sim: inbox full or line too long\n");
        }
    }
}
void sim_stdin_start(void) { xTaskCreate(stdin_task, "stdin", 8192, NULL, 1, NULL); }

/* ---------------- inputs ---------------- */

static void touch_up(void *arg) { sim_touch(false, 0, 0); }
static esp_timer_handle_t up_timer;
static pthread_once_t s_up_once = PTHREAD_ONCE_INIT;
static void up_timer_init(void)
{
    const esp_timer_create_args_t ta = {.callback = touch_up, .name = "touch_up"};
    esp_timer_create(&ta, &up_timer);
}

static bool held(const char *s)
{
    return !strcmp(s, "1") || !strcmp(s, "on") || !strcmp(s, "down");
}

const char *sim_help(void)
{
    return "simulator inputs (stdin: \"sim <cmd>\", HTTP: POST /sim/input, one command per line):\n"
           "  tap X Y | hold X Y | down X Y | up      touch; hold = press 1.3 s (opens the settings)\n"
           "  ai | ai_long | boot | boot_long         board buttons: click / long press\n"
           "  left | right                            Interaction module buttons\n"
           "  shake | bump L|R | tilt G | facedown 0|1\n"
           "  clap | double_clap | beat | motion | light 0-100\n"
           "  nod | head_shake | wave L|R             camera gestures (events only; there is no picture)\n"
           "  battery SOC [MA] | battery off          fuel gauge; MA > 0 = charging\n"
           "  screenshot FILE.bmp\n"
           "other stdin lines go to the pet as if typed on its USB console (\"cc ...\", \"state think\", \"say happy hi\")\n";
}

bool sim_input(const char *cmd, char *err, size_t len)
{
    char w[16] = "", a1[256] = "", a2[16] = "";
    const int n = sscanf(cmd, "%15s %255s %15s", w, a1, a2);
    pthread_once(&s_up_once, up_timer_init); /* called from the window, HTTP and stdin threads */
    if (n < 1) return snprintf(err, len, "empty command"), false;
    if ((!strcmp(w, "tap") || !strcmp(w, "hold") || !strcmp(w, "down")) && n == 3) {
        sim_touch(true, atoi(a1), atoi(a2));
        if (strcmp(w, "down")) esp_timer_start_once(up_timer, !strcmp(w, "tap") ? 80000 : 1300000);
    } else if (!strcmp(w, "up")) {
        sim_touch(false, 0, 0);
    } else if (!strcmp(w, "ai") || !strcmp(w, "ai_long")) {
        sim_button(SIM_BTN_AI, w[2] == '_');
    } else if (!strcmp(w, "boot") || !strcmp(w, "boot_long")) {
        sim_button(SIM_BTN_BOOT, w[4] == '_');
    } else if (!strcmp(w, "left") || !strcmp(w, "right")) {
        sensors_post(w[0] == 'l' ? SEV_BTN_L : SEV_BTN_R, 0, 0);
    } else if (!strcmp(w, "shake")) {
        sensors_post(SEV_SHAKE, 0, 0);
    } else if (!strcmp(w, "bump")) {
        sensors_post(SEV_BUMP, toupper((unsigned char)a1[0]) == 'L' ? -1.2f : 1.2f, -1.5f);
    } else if (!strcmp(w, "tilt") && n >= 2) {
        portENTER_CRITICAL(&s_sens_m);
        s_sens.tilt = s_sens.ax = (float)atof(a1);
        portEXIT_CRITICAL(&s_sens_m);
    } else if (!strcmp(w, "facedown") && n >= 2) {
        portENTER_CRITICAL(&s_sens_m);
        s_sens.face_down = held(a1);
        s_sens.az = s_sens.face_down ? -1 : 1;
        portEXIT_CRITICAL(&s_sens_m);
    } else if (!strcmp(w, "clap")) {
        sensors_post(SEV_CLAP, 0, 0);
    } else if (!strcmp(w, "double_clap")) {
        sensors_post(SEV_DOUBLE_CLAP, 0, 0);
    } else if (!strcmp(w, "beat")) {
        sensors_post(SEV_BEAT, 0, 8);
    } else if (!strcmp(w, "motion")) {
        portENTER_CRITICAL(&s_sens_m);
        s_sens.motion = true;
        portEXIT_CRITICAL(&s_sens_m);
        sensors_post(SEV_MOTION, 0, 0);
    } else if (!strcmp(w, "light") && n >= 2) {
        portENTER_CRITICAL(&s_sens_m);
        s_sens.light = (uint8_t)atoi(a1);
        portEXIT_CRITICAL(&s_sens_m);
    } else if (!strcmp(w, "nod")) {
        sensors_post(SEV_NOD, 0, 0);
    } else if (!strcmp(w, "head_shake")) {
        sensors_post(SEV_HEAD_SHAKE, 0, 0);
    } else if (!strcmp(w, "wave")) {
        sensors_post(toupper((unsigned char)a1[0]) == 'L' ? SEV_WAVE_L : SEV_WAVE_R, 0, 0);
    } else if (!strcmp(w, "battery") && n >= 2) {
        if (!strcmp(a1, "off")) sim_battery(false, 0, 0);
        else sim_battery(true, atoi(a1), n >= 3 ? atoi(a2) : -120);
    } else if (!strcmp(w, "screenshot") && n >= 2) {
        if (!sim_screenshot(a1)) return snprintf(err, len, "cannot write %s", a1), false;
    } else {
        return snprintf(err, len, "unknown or incomplete command: %s", cmd), false;
    }
    return true;
}

/* ---------------- screenshots and /sim/... ---------------- */

/* 24-bit BMP of the panel as shown (brightness applied), into a FILE or a memory buffer */
static size_t bmp_encode(uint8_t **out)
{
    static uint16_t px[SIM_SCREEN * SIM_SCREEN];
    sim_screen_copy(px);
    const int S = SIM_SCREEN, row = S * 3, k = sim_screen_brightness();
    const size_t size = 54 + (size_t)row * S;
    uint8_t *b = calloc(1, size);
    b[0] = 'B';
    b[1] = 'M';
    memcpy(b + 2, &(uint32_t){(uint32_t)size}, 4);
    b[10] = 54;
    b[14] = 40;
    memcpy(b + 18, &(int32_t){S}, 4);
    memcpy(b + 22, &(int32_t){-S}, 4);
    b[26] = 1;
    b[28] = 24;
    for (int i = 0; i < S * S; i++) {
        const uint16_t v = px[i];
        const int r = (v >> 11) & 31, g = (v >> 5) & 63, bl = v & 31;
        uint8_t *d = b + 54 + i * 3;
        d[0] = (uint8_t)(((bl << 3) | (bl >> 2)) * k / 100);
        d[1] = (uint8_t)(((g << 2) | (g >> 4)) * k / 100);
        d[2] = (uint8_t)(((r << 3) | (r >> 2)) * k / 100);
    }
    *out = b;
    return size;
}

bool sim_screenshot(const char *path)
{
    uint8_t *b;
    const size_t n = bmp_encode(&b);
    FILE *f = fopen(path, "wb");
    const bool ok = f && fwrite(b, 1, n, f) == n;
    if (f) fclose(f);
    free(b);
    return ok;
}

static esp_err_t h_screen(httpd_req_t *req)
{
    uint8_t *b;
    const size_t n = bmp_encode(&b);
    httpd_resp_set_type(req, "image/bmp");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, (const char *)b, (ssize_t)n);
    free(b);
    return ESP_OK;
}

static esp_err_t h_input(httpd_req_t *req)
{
    char body[1024];
    if (req->content_len >= sizeof body) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body too large");
    size_t got = 0;
    while (got < req->content_len) {
        const int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r <= 0) return ESP_FAIL;
        got += (size_t)r;
    }
    body[got] = 0;
    for (char *save = NULL, *line = strtok_r(body, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
        char err[96];
        if (!sim_input(line, err, sizeof err)) {
            char msg[2048];
            snprintf(msg, sizeof msg, "%s\n\n%s", err, sim_help());
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, msg);
        }
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t h_help(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, sim_help());
}

void sim_http_extras(void)
{
    const httpd_uri_t u[] = {
        {.uri = "/sim/screen.bmp", .method = HTTP_GET, .handler = h_screen},
        {.uri = "/sim/input", .method = HTTP_POST, .handler = h_input},
        {.uri = "/sim/input", .method = HTTP_GET, .handler = h_help},
    };
    for (size_t i = 0; i < sizeof u / sizeof u[0]; i++) httpd_register_uri_handler(NULL, &u[i]);
}
