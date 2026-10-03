#include "sensors.h"

#include <math.h>
#include <string.h>

#include "bsp/esp_mosaico.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mosaico_module_interact.h"

static const char *TAG = "sensors";

/* Board -> screen axes: screen_x = IMU_X_SIGN * accel[IMU_X_FROM], etc. (x right, y up, z out of
 * the screen). Unverified guess; flip a sign if the slime slides uphill. */
#define IMU_X_FROM 0
#define IMU_X_SIGN 1.0f
#define IMU_Y_FROM 1
#define IMU_Y_SIGN 1.0f
#define IMU_Z_FROM 2
#define IMU_Z_SIGN 1.0f

#define IMU_PERIOD_MS 10
#define GRAVITY_LP 0.08f   /* per sample; ~120 ms time constant */
#define LEVEL_LP 0.0004f   /* per sample; ~25 s: whatever angle the device rests at becomes "level" */
#define BUMP_G 0.22f       /* dynamic acceleration that wobbles the jelly */
#define BUMP_GAP_MS 80
#define SHAKE_G 0.9f       /* a peak this strong counts towards a shake */
#define SHAKE_PEAKS 4      /* ... this many within SHAKE_WINDOW_MS */
#define SHAKE_WINDOW_MS 1200
#define SHAKE_COOLDOWN_MS 3000
#define FACE_DOWN_G (-0.7f)
#define FACE_DOWN_MS 1000

#define MODULE_PERIOD_MS 50
#define MODULE_RETRY_MS 3000

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static sensors_state_t s_state;
static QueueHandle_t s_events;
static volatile led_mood_t s_mood;
static volatile uint8_t s_led_bright = 128;

static void push(sensor_ev_kind_t kind, float vx, float vy)
{
    const sensor_ev_t ev = {kind, vx, vy};
    if (s_events) xQueueSend(s_events, &ev, 0);
}

static inline uint32_t ms_now(void) { return (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount()); }
static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* ---------------- IMU ---------------- */

static void imu_task(void *arg)
{
    const bsp_imu_config_t cfg = BSP_IMU_CONFIG_DEFAULT();
    esp_err_t e = bsp_imu_init();
    if (e == ESP_OK) e = bsp_imu_start(&cfg);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "IMU unavailable: %s", esp_err_to_name(e));
        vTaskDelete(NULL);
    }
    vTaskDelay(pdMS_TO_TICKS(80));

    float g[3] = {0, 0, 1}, level_x = 0;
    bool first = true;
    uint32_t last_bump = 0, last_peak = 0, shake_t0 = 0, cool_until = 0, face_since = 0;
    int peaks = 0;
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(IMU_PERIOD_MS));
        float raw[3];
        if (bsp_imu_get_accel(&raw[0], &raw[1], &raw[2]) != ESP_OK) continue;
        const float a[3] = {IMU_X_SIGN * raw[IMU_X_FROM], IMU_Y_SIGN * raw[IMU_Y_FROM], IMU_Z_SIGN * raw[IMU_Z_FROM]};
        if (first) {
            memcpy(g, a, sizeof g);
            level_x = a[0];
            first = false;
        }
        float hp[3], m2 = 0;
        for (int i = 0; i < 3; i++) {
            g[i] += (a[i] - g[i]) * GRAVITY_LP;
            hp[i] = a[i] - g[i];
            m2 += hp[i] * hp[i];
        }
        level_x += (g[0] - level_x) * LEVEL_LP;
        const float mag = sqrtf(m2);
        const uint32_t now = ms_now();

        /* bumps wobble the jelly; the body lags behind the device's sideways motion */
        if (mag > BUMP_G && now - last_bump >= BUMP_GAP_MS) {
            last_bump = now;
            push(SEV_BUMP, clampf(-hp[0] * 1.5f, -1.5f, 1.5f), clampf(-mag * 3.0f, -3.0f, 0));
        }
        if (mag > SHAKE_G && now - last_peak > 100) {
            last_peak = now;
            if (now - shake_t0 > SHAKE_WINDOW_MS) {
                shake_t0 = now;
                peaks = 0;
            }
            if (++peaks >= SHAKE_PEAKS && (int32_t)(now - cool_until) >= 0) {
                push(SEV_SHAKE, 0, 0);
                cool_until = now + SHAKE_COOLDOWN_MS;
                peaks = 0;
            }
        }
        if (g[2] < FACE_DOWN_G) {
            if (!face_since) face_since = now | 1;
        } else {
            face_since = 0;
        }

        portENTER_CRITICAL(&s_lock);
        s_state.imu_ok = true;
        s_state.ax = g[0];
        s_state.ay = g[1];
        s_state.az = g[2];
        /* right edge lower -> +x axis points down -> accel x < 0; relative to the resting angle, so a
         * stand or a sloped desk does not pin the slime to a wall */
        s_state.tilt = -(g[0] - level_x);
        s_state.face_down = face_since && now - face_since > FACE_DOWN_MS;
        portEXIT_CRITICAL(&s_lock);
    }
}

/* ---------------- Interaction module ---------------- */

static mosaico_interact_rgb_t rgb(float r, float g, float b, float k)
{
    k = clampf(k, 0, 1);
    return (mosaico_interact_rgb_t){(uint8_t)(r * k), (uint8_t)(g * k), (uint8_t)(b * k)};
}

static mosaico_interact_rgb_t hue(float h)
{
    h = (h - floorf(h)) * 6;
    const float x = 1 - fabsf(fmodf(h, 2) - 1);
    const int i = (int)h;
    const float r = (i == 0 || i == 5) ? 1 : (i == 1 || i == 4) ? x : 0;
    const float g = (i == 1 || i == 2) ? 1 : (i == 0 || i == 3) ? x : 0;
    const float b = (i == 3 || i == 4) ? 1 : (i == 2 || i == 5) ? x : 0;
    return rgb(r * 255, g * 255, b * 255, 1);
}

/* One LED frame for a mood; f counts module periods since the mood started.
 * Returns false when the frame is identical to the previous one (static moods). */
static bool led_frame(led_mood_t mood, uint32_t f, mosaico_interact_rgb_t *c, int n)
{
    const float t = f * (MODULE_PERIOD_MS / 1000.0f);
    for (int i = 0; i < n; i++) {
        switch (mood) {
        case MOOD_IDLE: /* slow breathing in the slime's blue, 4 s */
            c[i] = rgb(50, 130, 255, 0.12f + 0.88f * (0.5f - 0.5f * cosf(t * 1.5708f)));
            break;
        case MOOD_CHARGE: /* amber breathing, 3 s */
            c[i] = rgb(255, 160, 30, 0.15f + 0.85f * (0.5f - 0.5f * cosf(t * 2.0944f)));
            break;
        case MOOD_POKE: /* white flash fading over 0.4 s */
            c[i] = rgb(255, 255, 255, 1 - t / 0.4f);
            break;
        case MOOD_MELT: /* weak red pulse */
            c[i] = rgb(255, 40, 20, 0.08f + 0.3f * (0.5f - 0.5f * cosf(t * 1.2566f)));
            break;
        case MOOD_METAL: /* cool silver breathing */
            c[i] = rgb(190, 205, 230, 0.2f + 0.8f * (0.5f - 0.5f * cosf(t * 1.5708f)));
            break;
        case MOOD_THINK: { /* slow blue comet */
            const float pos = fmodf(t * 3.0f, (float)n), d = fminf(fabsf(i - pos), n - fabsf(i - pos));
            c[i] = rgb(40, 90, 255, 1 - d / 1.8f);
            break;
        }
        case MOOD_WORK: { /* cyan chase; kept calm: a fast, white-headed one lit faces at ~2 Hz,
                         * which the camera's head-motion tracker took for nodding */
            const float pos = fmodf(t * 4.0f, (float)n), d = fminf(fabsf(i - pos), n - fabsf(i - pos));
            c[i] = rgb(0, 200, 255, 1 - d / 2.5f);
            break;
        }
        case MOOD_WAIT: /* amber blink, 2 Hz */
            c[i] = rgb(255, 140, 0, fmodf(t, 0.5f) < 0.25f ? 1 : 0);
            break;
        case MOOD_HURT: /* three red flashes, then a dim glow */
            c[i] = t < 0.9f ? rgb(255, 0, 0, fmodf(t, 0.3f) < 0.15f ? 1 : 0) : rgb(255, 0, 0, 0.15f);
            break;
        case MOOD_HAPPY: /* green breathing */
            c[i] = rgb(60, 255, 120, 0.25f + 0.75f * (0.5f + 0.5f * sinf(t * 6)));
            break;
        case MOOD_LEVELUP: /* spinning rainbow */
            c[i] = hue(t * 0.8f + (float)i / n);
            break;
        case MOOD_DIZZY: { /* random sparkles */
            const uint32_t h = (f / 3 + 1) * 2654435761u ^ (uint32_t)(i * 40503u);
            c[i] = (h >> 7) % 3 == 0 ? hue((h & 255) / 255.0f) : rgb(0, 0, 0, 0);
            break;
        }
        default:
            c[i] = rgb(0, 0, 0, 0);
            break;
        }
    }
    /* The setting is how bright it should look, and eyes are far from linear: an LED driven at
     * 20% still looks nearly full on. Squaring it (20% -> 4% drive) makes the slider do what it
     * says. Rounded up, so that a dim setting does not turn faint colours into gaps. */
    const uint32_t lvl = s_led_bright, k = lvl * lvl / 255;
    for (int i = 0; i < n; i++) {
        c[i].r = (uint8_t)((c[i].r * k + 254) / 255);
        c[i].g = (uint8_t)((c[i].g * k + 254) / 255);
        c[i].b = (uint8_t)((c[i].b * k + 254) / 255);
    }
    return mood != MOOD_OFF || f == 0;
}

static void module_task(void *arg)
{
    mosaico_interact_handle_t h = NULL;
    mosaico_interact_info_t info = {0};
    mosaico_interact_inputs_t prev = {0};
    bool logged_err = false;
    led_mood_t shown = MOOD_COUNT;
    uint32_t mood_f = 0, tick = 0;
    for (;;) {
        if (!h) {
            mosaico_interact_config_t cfg = MOSAICO_INTERACT_DEFAULT_CONFIG();
            cfg.led_brightness = 255; /* scaled per frame instead: the driver has no runtime setter */
            const esp_err_t e = mosaico_interact_open(&cfg, &h);
            portENTER_CRITICAL(&s_lock);
            s_state.mod_tries++;
            s_state.mod_err = e;
            portEXIT_CRITICAL(&s_lock);
            if (e != ESP_OK) {
                if (h) {
                    mosaico_interact_close(h);
                    h = NULL;
                }
                if (e != ESP_ERR_TIMEOUT && e != ESP_ERR_NOT_FOUND && !logged_err) {
                    ESP_LOGW(TAG, "interaction module init failed: %s", esp_err_to_name(e));
                    logged_err = true;
                }
                vTaskDelay(pdMS_TO_TICKS(MODULE_RETRY_MS));
                continue;
            }
            mosaico_interact_get_info(h, &info);
            ESP_LOGI(TAG, "interaction module on %s slot, %s input, %u LEDs", mosaico_module_mgr_slot_to_name(info.slot),
                     info.button_mode == MOSAICO_INTERACT_BUTTON_MODE_TOUCH ? "touch" : "GPIO", info.led_count);
            memset(&prev, 0, sizeof prev);
            shown = MOOD_COUNT;
            logged_err = false;
        }

        if (tick++ % 10 == 0) { /* hot-unplug check, every 0.5 s */
            mosaico_module_mgr_info_t m;
            if (mosaico_module_mgr_get_info(info.slot, &m) == ESP_OK && m.presence == MOSAICO_MODULE_PRESENCE_ABSENT) {
                ESP_LOGI(TAG, "interaction module removed");
                mosaico_interact_close(h);
                h = NULL;
                portENTER_CRITICAL(&s_lock);
                s_state.mod_ok = false;
                s_state.motion = false;
                portEXIT_CRITICAL(&s_lock);
                continue;
            }
        }

        mosaico_interact_inputs_t in;
        if (mosaico_interact_read_inputs(h, &in) == ESP_OK) {
            if (in.left_pressed && !prev.left_pressed) push(SEV_BTN_L, 0, 0);
            if (in.right_pressed && !prev.right_pressed) push(SEV_BTN_R, 0, 0);
            if (in.motion_detected && !prev.motion_detected) push(SEV_MOTION, 0, 0);
            prev = in;
            portENTER_CRITICAL(&s_lock);
            s_state.mod_ok = true;
            s_state.motion = in.motion_detected;
            s_state.light = in.light_level;
            portEXIT_CRITICAL(&s_lock);
        }

        const led_mood_t mood = s_mood;
        if (mood != shown) {
            shown = mood;
            mood_f = 0;
        }
        mosaico_interact_rgb_t c[MOSAICO_INTERACT_LED_COUNT];
        const int n = info.led_count && info.led_count <= MOSAICO_INTERACT_LED_COUNT ? info.led_count : MOSAICO_INTERACT_LED_COUNT;
        if (led_frame(mood, mood_f++, c, n)) mosaico_interact_led_write(h, c, n);

        vTaskDelay(pdMS_TO_TICKS(MODULE_PERIOD_MS));
    }
}

/* ---------------- API ---------------- */

void sensors_start(void)
{
    s_events = xQueueCreate(16, sizeof(sensor_ev_t));
    if (!s_events) return;
    if (xTaskCreatePinnedToCore(imu_task, "imu", 4096, NULL, 3, NULL, 0) != pdPASS) ESP_LOGW(TAG, "imu task");
    if (xTaskCreatePinnedToCore(module_task, "module", 6144, NULL, 2, NULL, 0) != pdPASS) ESP_LOGW(TAG, "module task");
}

void sensors_get(sensors_state_t *out)
{
    portENTER_CRITICAL(&s_lock);
    *out = s_state;
    portEXIT_CRITICAL(&s_lock);
}

int sensors_slot(int slot)
{
    mosaico_module_mgr_info_t m;
    if (mosaico_module_mgr_get_info(slot ? MOSAICO_MODULE_MGR_SLOT_RIGHT : MOSAICO_MODULE_MGR_SLOT_LEFT, &m) != ESP_OK) return SLOT_PENDING;
    if (m.presence == MOSAICO_MODULE_PRESENCE_ABSENT) return SLOT_EMPTY;
    if (m.presence != MOSAICO_MODULE_PRESENCE_PRESENT || m.descriptor_state == MOSAICO_MODULE_DESCRIPTOR_UNKNOWN) return SLOT_PENDING;
    if (m.descriptor_state != MOSAICO_MODULE_DESCRIPTOR_VALID || !m.eeprom.board_type) return SLOT_OTHER;
    return m.eeprom.board_type;
}

bool sensors_next_event(sensor_ev_t *ev) { return s_events && xQueueReceive(s_events, ev, 0) == pdTRUE; }

void sensors_set_mood(led_mood_t mood) { s_mood = mood; }

void sensors_set_led_brightness(uint8_t level) { s_led_bright = level; }

void sensors_post(sensor_ev_kind_t kind, float vx, float vy) { push(kind, vx, vy); }
