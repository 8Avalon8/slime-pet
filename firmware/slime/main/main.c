/*
 * Slime pet firmware for ESP-Mosaico (ESP32-S31).
 *
 * Renders slime_core frames into a PSRAM framebuffer and pushes only the
 * dirty rectangle to the CO5300 over QSPI. Rendering of frame N+1 overlaps
 * the DMA transfer of frame N.
 *
 * Inputs: AI button = mute / push-to-talk (or the help page), BOOT button = focus timer / settings;
 * touch left/right = poke; Claude Code hook lines over Wi-Fi (web.h) or USB CDC (cc_track.h);
 * IMU tilt/bumps/shakes and the Interaction module (sensors.h).
 * HP follows the fuel gauge. The "brain" picks the persistent state every frame:
 * face-down > Claude Code wait > work > think > melt > sleep > charge > idle; one-shot
 * reactions (done, fail, compact, pokes) play on top and then fall back.
 */
#include <math.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp/esp_mosaico.h"
#include "esp_cache.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "button_gpio.h"
#include "iot_button.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "audio.h"
#include "bootlog.h"
#include "cc_track.h"
#include "config.h"
#include "haptic.h"
#include "inbox.h"
#include "net.h"
#include "ota.h"
#include "sensors.h"
#include "settings_ui.h"
#include "vision.h"
#include "slime_anim.h"
#include "slime_render.h"
#include "slime_text.h"
#include "usb_link.h"
#include "web.h"

static const char *TAG = "slime";

#define SCR SL_SCREEN
#define FRAME_MS 16 /* cap at ~60 fps */
#define BUF_ALIGN 64
#define TYPE_CPS 24 /* dialog typewriter, characters per second */

static uint16_t *s_fb;   /* native RGB565, render target */
static uint16_t *s_wire; /* byte-swapped packed rectangle for DMA */
static size_t s_wire_size;
static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_xfer_done;
static bool s_xfer_busy;
static esp_lcd_touch_handle_t s_touch;

/* dual-core rendering: core 0 (main) and core 1 (worker) each own alternate 8-row bands */
static sg_canvas_t s_cv[2];
static sg_scratch_t s_scratch[2];
static SemaphoreHandle_t s_go, s_done;
static const sl_pose_t *s_job_pose;
static sl_rect_t s_job_dirty;

/* perf counters, seconds accumulated since the last stats line */
static double s_t_render, s_t_copy, s_t_wait, s_t_touch, s_t_anim;
static long s_px_pushed;

typedef enum { EV_NONE = 0, EV_AI_CLICK, EV_AI_LONG, EV_BOOT_CLICK, EV_BOOT_LONG } ev_t;
static atomic_int s_event;
static atomic_bool s_ai_up; /* AI key released: its own flag, a click arrives right behind it */

static bool IRAM_ATTR on_trans_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *e, void *ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)ctx, &woken);
    return woken == pdTRUE;
}

static double now_s(void) { return esp_timer_get_time() / 1e6; }

static void wait_xfer(void)
{
    if (s_xfer_busy) {
        const double t0 = now_s();
        while (xSemaphoreTake(s_xfer_done, pdMS_TO_TICKS(200)) != pdTRUE) {
            ESP_LOGW(TAG, "LCD transfer > 200 ms");
        }
        s_xfer_busy = false;
        s_t_wait += now_s() - t0;
    }
}

/* Copy rect from the framebuffer into the wire buffer (byte-swapped) and start DMA. */
static void push_rect(sl_rect_t r)
{
    r.x0 &= ~1;
    r.y0 &= ~1;
    r.x1 = (r.x1 + 1) & ~1;
    r.y1 = (r.y1 + 1) & ~1;
    if (r.x1 > SCR) r.x1 = SCR;
    if (r.y1 > SCR) r.y1 = SCR;
    if (r.x1 <= r.x0 || r.y1 <= r.y0) return;
    wait_xfer();
    const double t0 = now_s();
    const int w = r.x1 - r.x0;
    uint16_t *dst = s_wire;
    for (int y = r.y0; y < r.y1; y++) {
        const uint16_t *src = &s_fb[y * SCR + r.x0];
        for (int x = 0; x < w; x++) {
            dst[x] = __builtin_bswap16(src[x]);
        }
        dst += w;
    }
    const size_t bytes = (size_t)w * (r.y1 - r.y0) * 2;
    esp_cache_msync(s_wire, (bytes + BUF_ALIGN - 1) & ~(BUF_ALIGN - 1), ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    s_t_copy += now_s() - t0;
    s_px_pushed += (long)w * (r.y1 - r.y0);
    xSemaphoreTake(s_xfer_done, 0);
    if (esp_lcd_panel_draw_bitmap(s_panel, r.x0, r.y0, r.x1, r.y1, s_wire) == ESP_OK) {
        s_xfer_busy = true;
    }
}

/* Board buttons: ctx carries the event to raise (AI key, BOOT key; single click / long press). */
static void on_button(void *btn, void *ctx) { atomic_store(&s_event, (int)(intptr_t)ctx); }
static void on_ai_up(void *btn, void *ctx) { atomic_store(&s_ai_up, true); }

/* Fatal part: power + display + framebuffers. */
static esp_err_t init_display(void)
{
    ESP_RETURN_ON_ERROR(bsp_power_init(), TAG, "power");

    bsp_display_config_t dc = BSP_DISPLAY_DEFAULT_CONFIG();
#if CONFIG_BSP_DISPLAY_LVGL_ENABLE
    dc.enable_touch = false;
#endif
    ESP_RETURN_ON_ERROR(bsp_display_new(&dc, &s_panel), TAG, "display");

    const uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT;
    s_wire_size = (SCR * SCR * 2 + BUF_ALIGN - 1) & ~(BUF_ALIGN - 1);
    s_fb = heap_caps_aligned_calloc(BUF_ALIGN, 1, SCR * SCR * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_wire = heap_caps_aligned_calloc(BUF_ALIGN, 1, s_wire_size, caps);
    ESP_RETURN_ON_FALSE(s_fb && s_wire, ESP_ERR_NO_MEM, TAG, "framebuffers");

    s_xfer_done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_xfer_done, ESP_ERR_NO_MEM, TAG, "semaphore");
    const esp_lcd_panel_io_callbacks_t cbs = {.on_color_trans_done = on_trans_done};
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_register_event_callbacks(bsp_display_get_panel_io(), &cbs, s_xfer_done), TAG, "lcd cb");

    ESP_RETURN_ON_ERROR(bsp_display_on(), TAG, "display on");
    ESP_RETURN_ON_ERROR(bsp_display_brightness_set(100), TAG, "brightness");
    return ESP_OK;
}

/* Non-fatal part: each failure is logged and listed in `err` for the on-screen notice. */
static void init_inputs(char *err, size_t len)
{
    err[0] = 0;
    if (bsp_touch_new(BSP_LCD_ROTATION_DEFAULT, &s_touch) != ESP_OK) {
        ESP_LOGW(TAG, "touch unavailable");
        s_touch = NULL;
        strlcat(err, " touch", len);
    }

    button_handle_t btns[BSP_BUTTON_NUM] = {0};
    int n = 0;
    if (bsp_iot_button_create(btns, &n, BSP_BUTTON_NUM) == ESP_OK && n > 0) {
        iot_button_register_cb(btns[BSP_BUTTON_AI], BUTTON_SINGLE_CLICK, NULL, on_button, (void *)EV_AI_CLICK);
        iot_button_register_cb(btns[BSP_BUTTON_AI], BUTTON_LONG_PRESS_START, NULL, on_button, (void *)EV_AI_LONG);
        iot_button_register_cb(btns[BSP_BUTTON_AI], BUTTON_PRESS_UP, NULL, on_ai_up, NULL);
    } else {
        ESP_LOGW(TAG, "buttons unavailable");
        strlcat(err, " button", len);
    }
    /* BOOT is a strapping pin only at reset; after boot it is an ordinary button */
    const button_config_t bc = {0};
    const button_gpio_config_t gc = {.gpio_num = BSP_BUTTON_BOOT_GPIO, .active_level = BSP_BUTTON_ACTIVE_LEVEL};
    button_handle_t boot = NULL;
    if (iot_button_new_gpio_device(&bc, &gc, &boot) == ESP_OK) {
        iot_button_register_cb(boot, BUTTON_SINGLE_CLICK, NULL, on_button, (void *)EV_BOOT_CLICK);
        iot_button_register_cb(boot, BUTTON_LONG_PRESS_START, NULL, on_button, (void *)EV_BOOT_LONG);
    } else {
        ESP_LOGW(TAG, "BOOT button unavailable");
    }

    if (bsp_battery_init() != ESP_OK) {
        ESP_LOGW(TAG, "fuel gauge unavailable");
        strlcat(err, " battery", len);
    }
}

/* No display and (until USB CDC lands) no console: blink the v1.0 status LED forever. */
static void __attribute__((noreturn)) fail_blink(esp_err_t e)
{
    ESP_LOGE(TAG, "display init failed: %s", esp_err_to_name(e));
    bsp_board_variant_t v;
    const bool led = bsp_board_variant_get(&v) == ESP_OK && v == BSP_BOARD_VARIANT_V1_0 && bsp_led_init() == ESP_OK;
    for (bool on = true;; on = !on) {
        if (led) bsp_led_set(on);
        vTaskDelay(pdMS_TO_TICKS(on ? 100 : 400));
    }
}

/* Poll touch: a new press on the left/right half pokes that side. */
typedef struct {
    bool down, pressed; /* pressed: went down this frame */
    int x, y;
} touch_t;

static touch_t poll_touch(void)
{
    static bool was_down;
    touch_t t = {0};
    if (!s_touch) return t;
    uint16_t x[1], y[1];
    uint8_t cnt = 0;
    esp_lcd_touch_read_data(s_touch);
    t.down = esp_lcd_touch_get_coordinates(s_touch, x, y, NULL, &cnt, 1) && cnt > 0;
    if (t.down) {
        t.x = x[0];
        t.y = y[0];
    }
    t.pressed = t.down && !was_down;
    was_down = t.down;
    return t;
}

#define LONG_PRESS_S 1.0 /* hold the screen this long to open the settings */

/* ---------------- brain: which state the pet should be in ---------------- */

#define MANUAL_HOLD_S 15        /* a state picked with the AI button sticks this long */
#define DARK_LEVEL 6            /* Interaction module light level (0-100) that counts as lights off */
#define DARK_AFTER_S 30
#define AWAY_S (5 * 60)         /* PIR quiet this long, then motion -> "welcome back" */

typedef struct {
    cc_t cc;
    double last_activity;
    double manual_until;
    /* one-shot reaction: its message lives as long as that state entry (a.t0 == react_t0) */
    double react_t0;
    char react_msg[96];
    bool bat_valid, charging;
    int soc, bat_mv, bat_ma;
    slime_cfg_t cfg; /* snapshot, refreshed when cfg_gen() changes */
    /* sensors */
    bool face_down;
    double dark_since; /* 0 = not dark */
    double last_motion;
    double slide_until; /* "whoa, sliding" message */
    char notice[96];    /* short-lived message for calm states (Wi-Fi joined, ...) */
    double notice_until;
    /* camera */
    double last_face, face_since; /* face_since: start of the current sitting session, 0 = none */
    double close_since, close_cool, last_sit_nag, last_wait_nudge;
    double front_since, nonfront_since, contact_cool; /* eye contact */
    double next_hum; /* background tune */
    int hum_idx;
    bool night;                 /* quiet hours right now */
    double focus_end, break_end; /* focus timer: 0 = not running */
    /* push-to-talk: recording while the AI key is held, then waiting for the bridge's answer */
    bool listening;
    double voice_wait_until; /* 0 = not waiting */
    /* demo: a scripted tour of every feature, for recording a video */
    bool demo, demo_pip;
    int demo_i; /* -1 = countdown */
    double demo_t0;
    char demo_msg[128];
    float tilt_ref;
    /* progress, persisted in NVS */
    int lv, exp;
    bool nvs_ok;
    nvs_handle_t nvs;
} brain_t;

static brain_t s_b;

static int exp_need(int lv) { return 8 + lv * 4; }

static void progress_load(brain_t *b)
{
    b->lv = 1;
    b->exp = 0;
    b->nvs_ok = nvs_open("slime", NVS_READWRITE, &b->nvs) == ESP_OK; /* nvs_flash_init: cfg_init() */
    if (!b->nvs_ok) {
        ESP_LOGW(TAG, "NVS unavailable, progress will not persist");
        return;
    }
    int32_t v;
    if (nvs_get_i32(b->nvs, "lv", &v) == ESP_OK && v >= 1 && v <= 99) b->lv = (int)v;
    if (nvs_get_i32(b->nvs, "exp", &v) == ESP_OK && v >= 0) b->exp = (int)v;
    ESP_LOGI(TAG, "progress: Lv %d, %d/%d exp", b->lv, b->exp, exp_need(b->lv));
}

static void progress_save(brain_t *b)
{
    if (!b->nvs_ok) return;
    nvs_set_i32(b->nvs, "lv", b->lv);
    nvs_set_i32(b->nvs, "exp", b->exp);
    nvs_commit(b->nvs);
}

static void focus_start(brain_t *b, sl_anim_t *a, double now);
static void focus_stop(brain_t *b, sl_anim_t *a, double now);
static void demo_start(brain_t *b, sl_anim_t *a, double now);
static void demo_stop(brain_t *b, sl_anim_t *a, double now);

/* Quiet hours (needs the clock; off until SNTP has synced). */
static bool night_now(const brain_t *b)
{
    struct tm tm;
    const int s = b->cfg.night_start, e = b->cfg.night_end;
    if (s == e || !net_local_time(&tm)) return false;
    const int h = tm.tm_hour;
    return s < e ? (h >= s && h < e) : (h >= s || h < e);
}

static const char *greet_text(void)
{
    struct tm tm;
    if (!net_local_time(&tm)) return "欢迎回来！";
    const int h = tm.tm_hour;
    if (h >= 5 && h < 11) return "早上好！今天也一起加油！";
    if (h >= 11 && h < 13) return "中午好！记得吃饭哦。";
    if (h >= 13 && h < 18) return "下午好！欢迎回来～";
    if (h >= 18 && h < 23) return "晚上好！辛苦啦。";
    return "这么晚了……早点休息哦。";
}

static void react(brain_t *b, sl_anim_t *a, sl_state_t st, double now, const char *fmt, ...)
{
    sl_anim_set_state(a, st, now);
    b->react_t0 = a->t0;
    b->react_msg[0] = 0;
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(b->react_msg, sizeof b->react_msg, fmt, ap);
        va_end(ap);
    }
}

/* Effects respect the sleeping slime: only waking up or dozing off makes a sound then. */
static void sfx(const sl_anim_t *a, sfx_t s)
{
    if (a->state != SL_SLEEP || s == SFX_WAKE || s == SFX_SLEEP) audio_play(s);
}

/* Motor + tell the mic it is not a clap. */
static void buzz(haptic_t h)
{
    haptic_play(h);
    audio_hold_off(450);
}

static void on_cc(brain_t *b, sl_anim_t *a, const cc_reaction_t *r, double now)
{
    const bool fanfare = a->state == SL_LEVELUP; /* never cut a level-up short */
    switch (r->fx) {
    case CC_FX_HELLO:
        if (sl_state_duration(a->state) == 0) react(b, a, SL_GREET, now, "Claude Code 来啦！");
        sfx(a, SFX_HELLO);
        break;
    case CC_FX_DONE: {
        const int gain = 2 + (r->tools < 30 ? r->tools : 30);
        b->exp += gain;
        if (b->exp >= exp_need(b->lv) && b->lv < 99) {
            b->exp -= exp_need(b->lv);
            b->lv++;
            react(b, a, SL_LEVELUP, now, NULL);
            a->lv = b->lv - 1; /* the animation counts up at the top of its jump */
            buzz(HAPTIC_FANFARE);
            sfx(a, SFX_LEVELUP);
        } else if (!fanfare) {
            react(b, a, SL_GREET, now, "任务完成！获得了 %d 点经验值。", gain);
            buzz(HAPTIC_TICK);
            sfx(a, SFX_DONE);
        }
        progress_save(b);
        break;
    }
    case CC_FX_FAIL:
        if (!fanfare && a->state != SL_HURT) {
            react(b, a, SL_HURT, now, "史莱姆受到了 %d 点伤害！\n%s", 1 + (int)(strlen(r->detail) % 5), r->detail);
            buzz(HAPTIC_HURT);
            sfx(a, SFX_HURT);
        }
        break;
    case CC_FX_COMPACT:
        if (!fanfare) react(b, a, SL_DIZZY, now, "脑袋装满了，正在整理记忆……");
        sfx(a, SFX_DIZZY);
        break;
    case CC_FX_ASK:
        buzz(HAPTIC_NUDGE);
        sfx(a, SFX_ASK);
        break;
    default:
        break;
    }
}

static void handle_line(brain_t *b, sl_anim_t *a, const char *line, inbox_src_t src, double now)
{
    cc_reaction_t r;
    if (cc_apply(&b->cc, line, now, &r)) {
        b->last_activity = now;
        on_cc(b, a, &r, now);
        ESP_LOGI(TAG, "cc[%s]: %s", inbox_src_name(src), line);
        return;
    }
    if (src == SRC_USB && !strcmp(line, "otatoken")) { /* tools/ota_flash.py; never over Wi-Fi */
        printf("OTATOKEN %s\n", ota_token());
        fflush(stdout);
        return;
    }
    if (src == SRC_USB && !strncmp(line, "wifi\t", 5)) { /* bridge/wifi_setup.py; never log the line */
        char ssid[33] = "", psk[64] = "";
        const char *t1 = line + 5, *t2 = strchr(t1, '\t');
        if (t2 && (size_t)(t2 - t1) < sizeof ssid && strlen(t2 + 1) < sizeof psk) {
            memcpy(ssid, t1, t2 - t1);
            strcpy(psk, t2 + 1);
            if (net_set_wifi(ssid, psk) == ESP_OK) {
                snprintf(b->notice, sizeof b->notice, "正在连接 Wi-Fi……\n%s", ssid);
                b->notice_until = now + 15;
            }
        }
        memset(psk, 0, sizeof psk);
        return;
    }
    char name[16];
    if (sscanf(line, "sfx %15s", name) == 1) { /* panel: music preview */
        const int i = audio_sfx_find(name);
        if (i >= 0) audio_play((sfx_t)i);
        return;
    }
    /* bridge: "say <mood> <one line>", a model's comment or a proactive line (mood "remind": Claude
     * still waits for you, so it nudges even in the wait state); "talk <mood> <line>", a voice answer */
    const bool talk = !strncmp(line, "talk ", 5);
    if (talk || !strncmp(line, "say ", 4)) {
        char mood[12] = "";
        int off = 0;
        const char *rest = line + (talk ? 5 : 4);
        if (sscanf(rest, "%11s %n", mood, &off) == 1 && off > 0 && (talk || b->cfg.ai_comment) && !b->demo) {
            const char *text = rest + off;
            if (talk && b->voice_wait_until) {
                b->voice_wait_until = 0;
                b->manual_until = 0;
            }
            if (!strcmp(mood, "remind")) {
                if (a->state == SL_WAIT) {
                    react(b, a, SL_WAIT, now, "%s", text);
                    buzz(HAPTIC_NUDGE);
                    sfx(a, SFX_ASK);
                }
                ESP_LOGI(TAG, "remind: %s", text);
                return;
            }
            const sl_state_t st = !strcmp(mood, "worried") ? SL_SULK : (!strcmp(mood, "neutral") ? SL_POKE_R : SL_GREET);
            if (talk || (a->state != SL_SLEEP && a->state != SL_WAIT)) {
                react(b, a, st, now, "%s", text);
                sfx(a, SFX_HELLO);
            }
            snprintf(b->notice, sizeof b->notice, "%s", text); /* stays up after the reaction ends */
            b->notice_until = now + 20;
            ESP_LOGI(TAG, "%s (%s): %s", talk ? "talk" : "comment", mood, text);
        }
        return;
    }
    if (sscanf(line, "focus %15s", name) == 1) {
        if (!strcmp(name, "start")) focus_start(b, a, now);
        else focus_stop(b, a, now);
        return;
    }
    if (!strcmp(line, "demo")) {
        demo_start(b, a, now);
        return;
    }
    if (!strcmp(line, "demo stop")) {
        demo_stop(b, a, now);
        return;
    }
    if (sscanf(line, "bgm %15s", name) == 1) {
        if (!strcmp(name, "stop")) audio_stop_bgm();
        else audio_play_bgm((bgm_t)(atoi(name) % BGM_COUNT));
        return;
    }
    if (sscanf(line, "state %15s", name) == 1) { /* manual test hook */
        for (int i = 0; i < SL_STATE_COUNT; i++) {
            if (!strcmp(name, sl_state_name((sl_state_t)i))) {
                sl_anim_set_state(a, (sl_state_t)i, now);
                b->manual_until = now + MANUAL_HOLD_S;
                b->last_activity = now;
                /* the panel's demo buttons double as a sound check */
                static const int8_t DEMO_SFX[SL_STATE_COUNT] = {
                    [SL_IDLE] = -1, [SL_GREET] = SFX_GREET, [SL_POKE_L] = SFX_POKE, [SL_POKE_R] = SFX_POKE,
                    [SL_DIZZY] = SFX_DIZZY, [SL_SLEEP] = SFX_SLEEP, [SL_THINK] = -1, [SL_WAIT] = SFX_ASK,
                    [SL_LEVELUP] = SFX_LEVELUP, [SL_HURT] = SFX_HURT, [SL_CHARGE] = SFX_DONE, [SL_MELT] = -1,
                    [SL_METAL] = SFX_HELLO, [SL_WORK] = -1, [SL_SULK] = SFX_SULK,
                };
                if (DEMO_SFX[i] >= 0) audio_play((sfx_t)DEMO_SFX[i]);
            }
        }
    }
}

/* Battery -> HP and the charge/melt inputs of want_base(). */
static void poll_battery(brain_t *b, sl_anim_t *a, double now)
{
    static double next;
    if (now < next) return;
    next = now + 5;
    bsp_battery_status_t st;
    if (bsp_battery_read(&st) != ESP_OK) return;
    a->hp = st.state_of_charge * 20.0f / 100.0f;
    a->hp_external = true;
    b->bat_valid = true;
    b->charging = st.current_ma > 5;
    b->soc = st.state_of_charge;
    b->bat_mv = st.voltage_mv;
    b->bat_ma = st.current_ma;
    bootlog_snapshot((uint32_t)now, st.voltage_mv, st.current_ma, st.state_of_charge);
}

static sl_state_t want_base(const brain_t *b, cc_status_t cs, double now)
{
    if (b->face_down) return SL_SLEEP;
    switch (cs) {
    case CC_WAIT: return SL_WAIT;
    case CC_WORK: return SL_WORK;
    case CC_THINK: return SL_THINK;
    default: break;
    }
    if (b->bat_valid && !b->charging && b->soc < 15) return SL_MELT;
    if (now - b->last_activity > (b->night ? 120.0 : b->cfg.sleep_min * 60.0)) return SL_SLEEP; /* nights: soon */
    if (b->dark_since && now - b->dark_since > DARK_AFTER_S) return SL_SLEEP;
    if (b->charging) return SL_CHARGE;
    return SL_IDLE;
}

static bool calm_state(sl_state_t s) { return s == SL_IDLE || s == SL_CHARGE || s == SL_SLEEP || s == SL_MELT; }

/* Dialog text for this frame. Claude Code states show what is being worked on. */
static void compose_msg(const brain_t *b, const sl_anim_t *a, cc_status_t cs, const char *detail, int busy, double now,
                        char *msg, size_t len)
{
    const int up = ota_progress();
    if (up >= 0) {
        snprintf(msg, len, "正在无线更新固件…… %d%%\n请不要断电", up);
        return;
    }
    if (b->demo) {
        snprintf(msg, len, "%s", b->demo_msg);
        return;
    }
    if (a->t0 == b->react_t0 && b->react_msg[0]) {
        snprintf(msg, len, "%s", b->react_msg);
        return;
    }
    const bool calm = a->state == SL_IDLE || a->state == SL_CHARGE || a->state == SL_SLEEP || a->state == SL_MELT;
    if (calm && now < b->slide_until) {
        snprintf(msg, len, "哇——要滑倒啦！");
        return;
    }
    if (calm && now < b->notice_until) {
        snprintf(msg, len, "%s", b->notice);
        return;
    }
    char tag[16] = "";
    if (busy > 1) snprintf(tag, sizeof tag, "[%d] ", busy);
    if (a->state == SL_WAIT && cs == CC_WAIT) {
        snprintf(msg, len, "Claude 在等你确认！\n%s%s", tag, detail);
    } else if (a->state == SL_WORK && cs == CC_WORK) {
        snprintf(msg, len, "史莱姆正在努力干活！\n%s%s", tag, detail);
    } else if (a->state == SL_THINK && cs == CC_THINK && busy > 1) {
        snprintf(msg, len, "史莱姆正在思考……\n%s", tag);
    } else {
        sl_anim_message(a, msg, len);
        struct tm tm;
        if (calm && net_local_time(&tm)) { /* second line: the time, and what is going on */
            const size_t n = strlen(msg);
            char extra[48] = "";
            if (b->focus_end) snprintf(extra, sizeof extra, " · 专注中，还剩 %d 分钟", (int)((b->focus_end - now) / 60) + 1);
            else if (b->break_end) snprintf(extra, sizeof extra, " · 休息中");
            else if (b->night) snprintf(extra, sizeof extra, " · 夜间勿扰");
            snprintf(msg + n, len - n, "\n%02d:%02d%s", tm.tm_hour, tm.tm_min, extra);
        }
    }
}

static led_mood_t mood_for(sl_state_t s, bool breath)
{
    switch (s) {
    case SL_IDLE: return breath ? MOOD_IDLE : MOOD_OFF;
    case SL_CHARGE: return breath ? MOOD_CHARGE : MOOD_OFF;
    case SL_POKE_L:
    case SL_POKE_R: return MOOD_POKE;
    case SL_MELT: return MOOD_MELT;
    case SL_METAL: return MOOD_METAL;
    case SL_THINK: return MOOD_THINK;
    case SL_WORK: return MOOD_WORK;
    case SL_WAIT: return MOOD_WAIT;
    case SL_HURT: return MOOD_HURT;
    case SL_GREET: return MOOD_HAPPY;
    case SL_LEVELUP: return MOOD_LEVELUP;
    case SL_DIZZY: return MOOD_DIZZY;
    case SL_SULK: return MOOD_MELT;
    default: return MOOD_OFF;
    }
}

/* Sensor events and levels -> jelly physics, reactions and the brain's inputs. */
static void handle_sensors(brain_t *b, sl_anim_t *a, double now)
{
    sensor_ev_t ev;
    while (sensors_next_event(&ev)) {
        switch (ev.kind) {
        case SEV_BUMP:
            a->ext_kick_x += ev.vx;
            a->ext_kick_y += ev.vy;
            audio_hold_off(350); /* a knock on the desk is not a clap */
            b->last_activity = now;
            break;
        case SEV_SHAKE:
            if (a->state != SL_LEVELUP && a->state != SL_DIZZY) {
                react(b, a, SL_DIZZY, now, "别晃啦，头好晕……");
                sfx(a, SFX_DIZZY);
            }
            b->last_activity = now;
            break;
        case SEV_BTN_L: /* Interaction module: volume down / up */
        case SEV_BTN_R: {
            slime_cfg_t c;
            cfg_get(&c);
            const int v = c.volume + (ev.kind == SEV_BTN_R ? 10 : -10);
            c.volume = (uint8_t)(v < 0 ? 0 : (v > 100 ? 100 : v));
            cfg_set(&c);
            react(b, a, ev.kind == SEV_BTN_L ? SL_POKE_L : SL_POKE_R, now, "音量 %d%%", c.volume);
            buzz(HAPTIC_TICK);
            sfx(a, SFX_HELLO); /* at the new level (the settings apply before it plays) */
            b->last_activity = now;
            break;
        }
        case SEV_MOTION:
            if (now - b->last_motion > AWAY_S && sl_state_duration(a->state) == 0 && a->state != SL_WAIT &&
                a->state != SL_WORK && a->state != SL_THINK) {
                react(b, a, SL_GREET, now, "%s", greet_text());
                sfx(a, SFX_GREET);
            }
            b->last_motion = now;
            b->last_activity = now;
            break;
        case SEV_CLAP:
            if (a->state != SL_LEVELUP) {
                const bool was_asleep = a->state == SL_SLEEP;
                react(b, a, (b->lv & 1) ? SL_POKE_L : SL_POKE_R, now, was_asleep ? "啊！谁在拍手？" : "啊！吓我一跳！");
                audio_play(SFX_STARTLE);
            }
            b->last_activity = now;
            break;
        case SEV_DOUBLE_CLAP:
            if (a->state != SL_LEVELUP) {
                react(b, a, SL_GREET, now, "啪啪！我在呢！");
                audio_play(SFX_GREET);
            }
            b->last_activity = now;
            break;
        case SEV_WAVE_L:
        case SEV_WAVE_R:
            if (a->state != SL_LEVELUP && a->state != SL_SLEEP) {
                react(b, a, ev.kind == SEV_WAVE_L ? SL_POKE_L : SL_POKE_R, now, "看到你在挥手！");
                sfx(a, SFX_POKE);
            }
            b->last_activity = now;
            break;
        case SEV_NOD:
            if (a->state != SL_LEVELUP && a->state != SL_SLEEP) {
                static const char *const YES[] = {"嗯嗯！", "你也这么觉得吧？", "好耶！说定了！"};
                react(b, a, SL_GREET, now, "%s", YES[(int)(now * 7) % 3]);
                sfx(a, SFX_HELLO);
            }
            b->last_activity = now;
            break;
        case SEV_HEAD_SHAKE:
            if (a->state != SL_LEVELUP && a->state != SL_SLEEP) {
                static const char *const NO[] = {"不要嘛……", "呜，不行吗……", "史莱姆有点委屈……"};
                react(b, a, SL_SULK, now, "%s", NO[(int)(now * 7) % 3]);
                sfx(a, SFX_SULK);
            }
            b->last_activity = now;
            break;
        case SEV_COVER:
            if (a->state != SL_LEVELUP && a->state != SL_SLEEP) react(b, a, SL_POKE_L, now, "咦？天怎么黑了？");
            b->last_activity = now;
            break;
        case SEV_UNCOVER:
            if (a->state != SL_LEVELUP && a->state != SL_SLEEP) {
                react(b, a, SL_GREET, now, "躲猫猫！看到你啦！");
                sfx(a, SFX_GREET);
            }
            b->last_activity = now;
            break;
        case SEV_BEAT: { /* the jelly bobs to the sound */
            const float k = ev.vy > 18 ? 1.0f : ev.vy / 18;
            a->ext_kick_y -= 1.1f * k;
            a->ext_kick_x += ((int)(now * 4) & 1 ? 0.12f : -0.12f) * k;
            break;
        }
        }
    }
    sensors_state_t ss;
    sensors_get(&ss);
    a->ext_tilt = ss.imu_ok && b->cfg.tilt ? ss.tilt : 0;
    if (fabsf(ss.tilt - b->tilt_ref) > 0.2f) { /* picked up or tilted: someone is here */
        b->tilt_ref = ss.tilt;
        b->last_activity = now;
    }
    b->face_down = ss.imu_ok && ss.face_down;
    if (ss.mod_ok && ss.motion) {
        b->last_motion = now;
        b->last_activity = now;
    }
    const bool dark = ss.mod_ok && ss.light < DARK_LEVEL;
    if (!dark) {
        b->dark_since = 0;
    } else if (!b->dark_since) {
        b->dark_since = now;
    }
    if (fabsf(a->j_vtdx) > 6) b->slide_until = now + 1.2;
}

/* JSON string body: escapes quotes, backslashes and control characters. */
static void json_esc(char *dst, size_t len, const char *src)
{
    size_t n = 0;
    for (; *src && n + 7 < len; src++) {
        const unsigned char ch = (unsigned char)*src;
        if (ch == '"' || ch == '\\') {
            dst[n++] = '\\';
            dst[n++] = (char)ch;
        } else if (ch == '\n') {
            dst[n++] = '\\';
            dst[n++] = 'n';
        } else if (ch < 0x20) {
            n += snprintf(dst + n, len - n, "\\u%04x", ch);
        } else {
            dst[n++] = (char)ch;
        }
    }
    dst[n] = 0;
}

typedef struct {
    float fps, render_ms;
} perf_t;

/* Everything the web panel shows, as one JSON document (built here, served by web.c). */
static void publish_status(const brain_t *b, const sl_anim_t *a, const char *msg, cc_status_t cs, const char *cc_detail,
                           int cc_busy, perf_t perf, double now)
{
    static char js[4096];
    static const char *CCN[] = {"idle", "think", "work", "wait"};
    char e1[160], e2[CC_DETAIL_LEN * 2];
    sensors_state_t ss;
    sensors_get(&ss);
    net_status_t ns;
    net_get(&ns);
    audio_state_t au;
    audio_get(&au);
    char boots[640], clock[8] = "--:--", crash[200];
    bootlog_crash_json(crash, sizeof crash);
    char fw[160];
    ota_status_json(fw, sizeof fw);
    struct tm tm;
    if (net_local_time(&tm)) snprintf(clock, sizeof clock, "%02d:%02d", tm.tm_hour, tm.tm_min);
    bootlog_json(boots, sizeof boots);
    vision_state_t vs;
    vision_get(&vs);
    audio_rec_state_t vr;
    audio_rec_get(&vr);
    json_esc(e1, sizeof e1, msg);
    json_esc(e2, sizeof e2, cc_detail);
    int n = snprintf(js, sizeof js,
                     "{\"state\":\"%s\",\"msg\":\"%s\",\"lv\":%d,\"exp\":%d,\"need\":%d,\"hp\":%.1f,\"up\":%.0f,"
                     "\"perf\":{\"fps\":%.1f,\"render\":%.1f},"
                     "\"bat\":{\"ok\":%s,\"soc\":%d,\"chg\":%s,\"mv\":%d,\"ma\":%d},"
                     "\"cc\":{\"status\":\"%s\",\"busy\":%d,\"detail\":\"%s\",\"ok\":%u,\"stale\":%u,\"sessions\":[",
                     sl_state_name(a->state), e1, b->lv, b->exp, exp_need(b->lv), a->hp, now, perf.fps, perf.render_ms,
                     b->bat_valid ? "true" : "false", b->soc, b->charging ? "true" : "false", b->bat_mv, b->bat_ma, CCN[cs],
                     cc_busy, e2,
                     (unsigned)b->cc.lines_ok, (unsigned)b->cc.lines_stale);
    bool first = true;
    for (int i = 0; i < CC_MAX_SESSIONS && n < (int)sizeof js - 256; i++) {
        const cc_session_t *se = &b->cc.s[i];
        if (!se->used) continue;
        json_esc(e2, sizeof e2, se->detail);
        n += snprintf(js + n, sizeof js - n, "%s{\"sid\":\"%08x\",\"st\":\"%s\",\"detail\":\"%s\",\"tools\":%d,\"age\":%.0f}",
                      first ? "" : ",", (unsigned)se->sid, CCN[se->st], e2, se->tools, now - se->last);
        first = false;
    }
    json_esc(e2, sizeof e2, ns.ssid);
    snprintf(js + n, sizeof js - n,
             "]},\"imu\":{\"ok\":%s,\"ax\":%.2f,\"ay\":%.2f,\"az\":%.2f,\"tilt\":%.2f,\"face\":%s},"
             "\"mod\":{\"ok\":%s,\"motion\":%s,\"light\":%u},"
             "\"mic\":{\"ok\":%s,\"db\":%.1f,\"floor\":%.1f,\"claps\":%d},"
             "\"net\":{\"state\":\"%s\",\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,\"fails\":%d,\"reason\":%d},"
             "\"voice\":{\"on\":%s,\"seq\":%u,\"ready\":%s,\"rec\":%s},"
             "\"heap\":{\"int\":%u,\"psram\":%u},\"boots\":%s,\"crash\":%s,\"fw\":%s,\"clock\":\"%s\",\"night\":%s,\"focus\":%d,\"rest\":%d,\"demo\":%s,"
             "\"cam\":{\"on\":%s,\"ok\":%s,\"tries\":%d,\"err\":\"%s\",\"face\":%s,\"n\":%d,\"fx\":%.2f,\"fy\":%.2f,"
             "\"size\":%.2f,\"ms\":%.0f,\"raw_x\":%.2f,\"raw_y\":%.2f,\"motion\":%.2f,\"mx\":%.2f,\"luma\":%.0f,"
             "\"kp\":%s,\"roll\":%.0f,\"yaw\":%.2f,\"pitch\":%.2f,\"sway\":%.2f,"
             "\"slot\":{\"presence\":%d,\"desc\":%d,\"owner\":%d,\"type\":%d,\"err\":\"%s\",\"bus_resets\":%d}}}",
             ss.imu_ok ? "true" : "false", ss.ax, ss.ay, ss.az, ss.tilt, ss.face_down ? "true" : "false",
             ss.mod_ok ? "true" : "false", ss.motion ? "true" : "false", ss.light, au.ok && au.mic ? "true" : "false", au.db,
             au.floor, au.claps, net_state_name(ns.state), e2, ns.ip,
             ns.rssi, ns.fails, ns.last_reason, b->cfg.voice ? "true" : "false", (unsigned)vr.seq,
             vr.ready ? "true" : "false", vr.rec ? "true" : "false", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024), boots, crash, fw, clock, b->night ? "true" : "false",
             b->focus_end ? (int)(b->focus_end - now) : 0, b->break_end ? (int)(b->break_end - now) : 0, b->demo ? "true" : "false", vs.enabled ? "true" : "false",
             vs.ok ? "true" : "false", vs.tries, esp_err_to_name(vs.err), vs.face ? "true" : "false", vs.faces, vs.fx, vs.fy,
             vs.fsize, vs.infer_ms, vs.raw_x, vs.raw_y, vs.motion, vs.motion_x, vs.luma, vs.kp_ok ? "true" : "false",
             vs.roll * 57.3f, vs.yaw, vs.pitch, a->ext_sway, vs.slot_presence, vs.slot_desc, vs.slot_owner, vs.slot_type,
             esp_err_to_name(vs.slot_err), vs.bus_resets);
    web_publish_status(js);
}

/* Wi-Fi state changes -> a notice on the dialog. */
static void watch_net(brain_t *b, double now)
{
    static net_state_t prev = NET_OFF;
    static bool warned;
    net_status_t ns;
    net_get(&ns);
    if (ns.state == NET_CONNECTED && prev != NET_CONNECTED) {
        snprintf(b->notice, sizeof b->notice, "已连上 Wi-Fi！\nhttp://slime.local  (%s)", ns.ip);
        b->notice_until = now + 12;
        warned = false;
    } else if (ns.state == NET_CONNECTING && ns.fails >= 4 && !warned) {
        snprintf(b->notice, sizeof b->notice, "Wi-Fi 连不上……\n%s (reason %d)", ns.ssid, ns.last_reason);
        b->notice_until = now + 12;
        warned = true;
    }
    prev = ns.state;
}

/* ---------------- focus timer ---------------- */

#define BREAK_S (5 * 60)
static void focus_start(brain_t *b, sl_anim_t *a, double now)
{
    b->focus_end = now + b->cfg.focus_min * 60.0;
    b->break_end = 0;
    react(b, a, SL_GREET, now, "开始专注 %d 分钟！\n我会安静陪着你。", b->cfg.focus_min);
    sfx(a, SFX_WAKE);
}

static void focus_stop(brain_t *b, sl_anim_t *a, double now)
{
    if (!b->focus_end && !b->break_end) return;
    b->focus_end = b->break_end = 0;
    react(b, a, SL_POKE_R, now, "好的，专注计时结束了。");
}

static void focus_tick(brain_t *b, sl_anim_t *a, double now)
{
    if (b->demo) return;
    if (b->focus_end && now >= b->focus_end) {
        b->focus_end = 0;
        b->break_end = now + BREAK_S;
        react(b, a, SL_GREET, now, "专注 %d 分钟完成，真棒！\n起来休息 5 分钟吧～", b->cfg.focus_min);
        audio_play(SFX_DONE);
        buzz(HAPTIC_NUDGE);
    } else if (b->break_end && now >= b->break_end) {
        b->break_end = 0;
        react(b, a, SL_GREET, now, "休息结束，继续加油！");
        audio_play(SFX_WAKE);
        buzz(HAPTIC_TICK);
    }
}

/* ---------------- demo: every feature in ~2 minutes, for recording a video ---------------- */

typedef enum { DA_NONE = 0, DA_TILT, DA_DANCE, DA_LOOK, DA_SWAY, DA_NUDGE } demo_act_t;
typedef struct {
    sl_state_t st;
    int8_t sfx; /* -1 = none */
    uint8_t act;
    bool cam;   /* needs the camera; skipped without it */
    float min_s;
    const char *msg;
} demo_step_t;

static const demo_step_t DEMO[] = {
    {SL_GREET, SFX_BOOT, DA_NONE, false, 4, "大家好！我是住在你桌上的史莱姆。"},
    {SL_POKE_L, SFX_POKE, DA_NONE, false, 3, "戳我一下，我会软软地弹一下～"},
    {SL_POKE_R, SFX_POKE, DA_NONE, false, 3, "换一边戳也可以！"},
    {SL_THINK, -1, DA_NONE, false, 4, "我和 Claude Code 连在一起。\nClaude 在思考，我也在思考……"},
    {SL_WORK, -1, DA_NONE, false, 4, "Claude 动手干活时，\n我会显示它正在做什么。"},
    {SL_WAIT, SFX_ASK, DA_NUDGE, false, 4, "Claude 需要你批准时，\n我会叫你、闪灯、还会震动。"},
    {SL_LEVELUP, SFX_LEVELUP, DA_NONE, false, 4, "完成任务攒经验，还能升级！"},
    {SL_HURT, SFX_HURT, DA_NONE, false, 3.5f, "命令出错时，我会受到伤害……"},
    {SL_DIZZY, SFX_DIZZY, DA_NONE, false, 4, "使劲摇晃我，就会晕头转向～"},
    {SL_IDLE, -1, DA_TILT, false, 6, "把设备歪过来，我会顺着滑过去！"},
    {SL_IDLE, -1, DA_DANCE, false, 9, "空闲时我会哼歌，\n还会跟着节拍摇摆。"},
    {SL_IDLE, -1, DA_LOOK, true, 6, "装上摄像头，我的眼睛会跟着你转。\n右上角的小窗是我看到的画面。"},
    {SL_IDLE, -1, DA_SWAY, true, 5, "你歪头，我也跟着歪～"},
    {SL_GREET, SFX_SHY, DA_NONE, true, 3.5f, "一直盯着我看，我会害羞的！"},
    {SL_GREET, SFX_HELLO, DA_NONE, true, 3.5f, "冲我点点头，我会很开心！"},
    {SL_SULK, SFX_SULK, DA_NONE, true, 3.5f, "冲我摇摇头……我会有点委屈。"},
    {SL_GREET, SFX_GREET, DA_NONE, true, 3.5f, "盖住镜头再拿开，就是躲猫猫！"},
    {SL_GREET, SFX_GREET, DA_NONE, false, 3.5f, "拍两下手，我会回应你。"},
    {SL_CHARGE, -1, DA_NONE, false, 3.5f, "插上电，我会大口大口地吃电。"},
    {SL_MELT, -1, DA_NONE, false, 4, "电快用完时，我会慢慢化掉……"},
    {SL_METAL, SFX_HELLO, DA_NONE, false, 3.5f, "偶尔还会变成金属的！"},
    {SL_SLEEP, SFX_SLEEP, DA_NONE, false, 4, "没人陪我玩，我就去睡觉。\n夜里还会自动静音。"},
    {SL_GREET, SFX_WAKE, DA_NONE, false, 3.5f, "你一回来，我就醒了！"},
    {SL_IDLE, -1, DA_NONE, false, 5, "长按屏幕打开设置，\n用手机访问 slime.local 有完整面板。"},
    {SL_GREET, SFX_DONE, DA_NONE, false, 4, "谢谢观看！"},
};
#define DEMO_N ((int)(sizeof DEMO / sizeof DEMO[0]))
#define DEMO_COUNTDOWN_S 3

static void demo_clear_fx(brain_t *b, sl_anim_t *a)
{
    a->ext_look_on = false;
    a->ext_sway = 0;
    a->ext_tilt = 0;
    b->demo_pip = false;
}

static void demo_start(brain_t *b, sl_anim_t *a, double now)
{
    b->demo = true;
    b->demo_i = -1;
    b->demo_t0 = now;
    b->manual_until = now + 1e6; /* the brain keeps its hands off */
    audio_stop_bgm();
    demo_clear_fx(b, a);
    sl_anim_set_state(a, SL_IDLE, now);
}

static void demo_stop(brain_t *b, sl_anim_t *a, double now)
{
    if (!b->demo) return;
    b->demo = false;
    b->manual_until = 0;
    audio_stop_bgm();
    demo_clear_fx(b, a);
    sl_anim_set_state(a, SL_IDLE, now);
    b->last_activity = now;
}

static float demo_len(const demo_step_t *d) /* long enough for the caption to finish typing and be read */
{
    const float type_s = (float)sl_text_count(d->msg) / TYPE_CPS + 1.8f;
    return type_s > d->min_s ? type_s : d->min_s;
}

static void demo_begin_step(brain_t *b, sl_anim_t *a, bool cam_ok, double now)
{
    while (b->demo_i < DEMO_N && DEMO[b->demo_i].cam && !cam_ok) b->demo_i++;
    if (b->demo_i >= DEMO_N) {
        demo_stop(b, a, now);
        return;
    }
    const demo_step_t *d = &DEMO[b->demo_i];
    b->demo_t0 = now;
    snprintf(b->demo_msg, sizeof b->demo_msg, "%s", d->msg);
    sl_anim_set_state(a, d->st, now);
    if (d->sfx >= 0) audio_play((sfx_t)d->sfx);
    if (d->act == DA_NUDGE) buzz(HAPTIC_NUDGE);
    if (d->act == DA_DANCE) audio_play_bgm(BGM_TOWN);
}

/* Runs the current step; true while the demo owns the pet (the brain and camera stay out). */
static bool demo_tick(brain_t *b, sl_anim_t *a, bool cam_ok, double now)
{
    if (!b->demo) return false;
    const double t = now - b->demo_t0;
    if (b->demo_i < 0) { /* 3, 2, 1: time to step back and hit record */
        const int left = DEMO_COUNTDOWN_S - (int)t;
        if (left > 0) {
            snprintf(b->demo_msg, sizeof b->demo_msg, "功能演示马上开始…… %d", left);
            return true;
        }
        b->demo_i = 0;
        demo_begin_step(b, a, cam_ok, now);
        return b->demo;
    }
    const demo_step_t *d = &DEMO[b->demo_i];
    const float ph = (float)(t * 2 * M_PI);
    switch (d->act) {
    case DA_TILT: a->ext_tilt = 0.5f * sinf(ph / 3.0f); break;     /* tip it right, then left */
    case DA_LOOK:
        a->ext_look_on = true;
        a->ext_look_x = sinf(ph / 2.5f);
        a->ext_look_y = 0.15f;
        b->demo_pip = true;
        break;
    case DA_SWAY: a->ext_sway = 0.28f * sinf(ph / 2.5f); break;
    default: break;
    }
    if (t >= demo_len(d)) {
        if (d->act == DA_DANCE) audio_stop_bgm();
        demo_clear_fx(b, a);
        b->demo_i++;
        demo_begin_step(b, a, cam_ok, now);
    }
    return b->demo;
}

/* ---------------- push-to-talk ---------------- */

#define VOICE_MIN_MS 600
#define VOICE_WAIT_S 25 /* speech to text plus a model answer, on the computer */

static void voice_start(brain_t *b, sl_anim_t *a, double now)
{
    if (!audio_rec_start()) {
        react(b, a, SL_SULK, now, "没有内存录音了……");
        return;
    }
    b->listening = true;
    b->voice_wait_until = 0;
    b->manual_until = now + AUDIO_REC_MAX_S + 5; /* the brain keeps its hands off while we listen */
    react(b, a, SL_THINK, now, "我在听，说完松开 AI 键～");
    buzz(HAPTIC_TICK);
}

static void voice_stop(brain_t *b, sl_anim_t *a, double now)
{
    b->listening = false;
    const uint32_t ms = audio_rec_stop(VOICE_MIN_MS);
    buzz(HAPTIC_TICK);
    if (ms < VOICE_MIN_MS) {
        b->manual_until = 0;
        react(b, a, SL_POKE_R, now, "太短了，按住 AI 键再说一次吧");
        return;
    }
    b->voice_wait_until = now + VOICE_WAIT_S;
    b->manual_until = b->voice_wait_until;
    react(b, a, SL_THINK, now, "嗯嗯，让我想想……");
    ESP_LOGI(TAG, "voice: %u ms recorded", (unsigned)ms);
}

static void voice_tick(brain_t *b, sl_anim_t *a, double now)
{
    if (!b->voice_wait_until || now < b->voice_wait_until) return;
    b->voice_wait_until = 0;
    b->manual_until = 0;
    react(b, a, SL_SULK, now, "电脑那边没有回应……\nslime_buddy.py");
}

/* ---------------- help screen ---------------- */

static void draw_help(sg_canvas_t *cv)
{
    static const char *const LINES[][2] = {
        {"点一下", "戳它，它会弹一下"},
        {"长按 1 秒", "打开设置"},
        {"拍两下手", "它会回应你"},
        {"倾斜 / 摇晃", "它会滑动 / 晕头转向"},
        {"扣过来放", "让它睡觉"},
        {"盖住镜头", "躲猫猫"},
        {"点头 / 摇头", "开心 / 委屈"},
        {"歪头", "它跟着你歪"},
        {"盯着它看", "它会害羞"},
        {"点右上角小窗", "看摄像头全屏画面"},
        {"AI 键", "单击静音，长按说话"},
        {"BOOT 键", "单击专注计时，长按设置"},
        {"模块左右键", "音量减 / 加"},
        {"手机浏览器", "slime.local 完整面板"},
    };
    const sg_rgb_t gold = sg_hex(0xffd84a), white = {1, 1, 1}, dim = sg_hex(0x8d97ad);
    sg_fill_rect(cv, 0, 0, SCR, SCR, 0);
    sl_text_draw(cv, "玩法说明", 190, 40, SL_FONT_22, gold, -1);
    for (int i = 0; i < (int)(sizeof LINES / sizeof LINES[0]); i++) {
        const float y = 76 + i * 27;
        sl_text_draw(cv, LINES[i][0], 40, y, SL_FONT_18, gold, -1);
        sl_text_draw(cv, LINES[i][1], 190, y, SL_FONT_18, white, -1);
    }
    sl_text_draw(cv, "点屏幕返回", 195, 462, SL_FONT_14, dim, -1);
}

/* Background tune: now and then, while idle and someone is around. */
#define HUM_FIRST_S (3 * 60)
#define HUM_GAP_S (15 * 60)
static void maybe_hum(brain_t *b, const sl_anim_t *a, cc_status_t cs, double now)
{
    audio_state_t au;
    audio_get(&au);
    if (au.bgm && (a->state == SL_SLEEP || cs == CC_WAIT || cs == CC_WORK || cs == CC_THINK)) {
        audio_stop_bgm(); /* not while it sleeps or works */
        return;
    }
    if (b->demo) return;
    if (!b->next_hum) b->next_hum = now + HUM_FIRST_S;
    if (now < b->next_hum) return;
    if (!b->cfg.bgm || !b->cfg.sound || b->night || b->focus_end || a->state != SL_IDLE || cs != CC_IDLE || au.playing ||
        now - b->last_motion > AWAY_S) {
        if (now - b->next_hum > 60) b->next_hum = now + 60; /* check again in a minute */
        return;
    }
    audio_play_bgm((bgm_t)(b->hum_idx++ % BGM_COUNT));
    b->next_hum = now + HUM_GAP_S + (esp_random() % 600);
}

/* Camera: gaze, presence, too-close, sitting reminder, and nudges while Claude waits for an absent you. */
static void handle_vision(brain_t *b, sl_anim_t *a, cc_status_t cs, double now)
{
    vision_state_t vs;
    vision_get(&vs);
    const bool seen = vs.ok && vs.face;
    if (seen) {
        if (now - b->last_motion > AWAY_S && calm_state(a->state) && a->state != SL_SLEEP) {
            react(b, a, SL_GREET, now, "%s", greet_text());
            sfx(a, SFX_GREET);
        }
        b->last_motion = now; /* same presence notion as the PIR */
        b->last_activity = now;
        b->last_face = now;
        if (!b->face_since) b->face_since = now;
    } else if (b->face_since && now - b->last_face > 180) {
        b->face_since = 0; /* away for 3 minutes: the sitting session is over */
    }

    /* eyes follow you */
    const bool recent = vs.ok && now - b->last_face < 3.0; /* detection misses frames up close: hold the gaze */
    const sl_state_t st = a->state;
    a->ext_look_on = recent && st != SL_SLEEP && st != SL_DIZZY && st != SL_HURT; /* those eyes are closed/spinning */
    if (seen) {
        /* the camera's view is narrow: a small move of your head should swing the eyes fully */
        const float gx = fabsf(vs.fx) < 0.02f ? 0 : vs.fx * 5.0f, gy = fabsf(vs.fy) < 0.02f ? 0 : vs.fy * 4.0f;
        a->ext_look_x = gx > 1 ? 1 : (gx < -1 ? -1 : gx);
        a->ext_look_y = gy > 1 ? 1 : (gy < -1 ? -1 : gy);
    }

    /* head-tilt mirror: the body leans the way your head does (both as seen on the screen) */
    float lean = 0;
    if (recent && vs.kp_ok && st != SL_SLEEP && st != SL_DIZZY) {
        const float r = vs.roll, dead = 0.05f; /* ~3 degrees: nobody holds their head perfectly level */
        lean = fabsf(r) < dead ? 0 : (r - (r > 0 ? dead : -dead)) * 1.1f;
    }
    a->ext_sway += (lean - a->ext_sway) * (lean ? 0.25f : 0.06f); /* settle back slowly when you leave */

    /* eye contact: you turn to face it after looking elsewhere (or being away) for a while */
    const bool frontal = seen && vs.kp_ok && fabsf(vs.yaw) < 0.12f;
    if (frontal) {
        if (!b->front_since) b->front_since = now;
    } else if ((seen && vs.kp_ok) || !recent) { /* a turned head, or gone: missed frames don't count */
        if (b->front_since || !b->nonfront_since) b->nonfront_since = now;
        b->front_since = 0;
    }
    if (b->front_since && now - b->front_since > 1.2 && b->nonfront_since && b->front_since - b->nonfront_since > 4 &&
        now > b->contact_cool && calm_state(st) && st != SL_SLEEP) {
        static const char *const SHY[] = {"被你盯得害羞了～", "你在看我吗？嘿嘿", "四目相对！"};
        react(b, a, SL_GREET, now, "%s", SHY[(int)(now * 7) % 3]);
        sfx(a, SFX_SHY);
        b->contact_cool = now + 45;
        b->nonfront_since = 0; /* needs another look-away before the next one */
    }

    if (seen && vs.fsize > 0.9f) { /* at desk distance a face already spans ~70% of the frame */
        if (!b->close_since) b->close_since = now;
        if (now - b->close_since > 1.5 && now > b->close_cool && calm_state(st)) {
            react(b, a, SL_POKE_R, now, "太近啦，看不清你了！");
            sfx(a, SFX_STARTLE);
            b->close_cool = now + 20;
        }
    } else {
        b->close_since = 0;
    }

    if (b->cfg.sit_min && !b->focus_end && b->face_since && now - b->face_since > b->cfg.sit_min * 60.0 && now - b->last_sit_nag > 600 &&
        calm_state(st) && st != SL_SLEEP) {
        react(b, a, SL_GREET, now, "坐了 %d 分钟啦，起来活动一下吧！", (int)((now - b->face_since) / 60));
        buzz(HAPTIC_TICK);
        sfx(a, SFX_WAKE); /* not SFX_ASK: that one means Claude needs you */
        b->last_sit_nag = now;
    }

    if (cs == CC_WAIT && vs.ok && now - b->last_face > 30 && now - b->last_wait_nudge > 60) {
        buzz(HAPTIC_NUDGE); /* Claude is waiting and nobody is in front of the camera */
        sfx(a, SFX_ASK);
        b->last_wait_nudge = now;
    }
}

/* Camera view mode: the live (mirrored) picture with what the detectors make of it. */
#define CAMVIEW_TIMEOUT_S 60
static bool draw_camview(sg_canvas_t *cv, uint16_t *fb, uint32_t *seq)
{
    int w = 0, h = 0;
    const int x0 = (SCR - 270) / 2;
    if (!vision_preview_copy(fb, SCR, x0, 270, SCR, seq, &w, &h)) return false;
    vision_state_t vs;
    vision_get(&vs);
    const sg_rgb_t green = sg_hex(0x6ee36e), amber = sg_hex(0xffb020), white = {1, 1, 1};
    if (vs.face) { /* face box */
        const float bx = x0 + vs.box[0] * w, by = vs.box[1] * h, bw = (vs.box[2] - vs.box[0]) * w, bh = (vs.box[3] - vs.box[1]) * h;
        sg_stroke_round_rect(cv, bx, by, bw, bh, 6, 3, green, 1);
    }
    if (vs.face && vs.kp_ok) { /* landmarks: eyes green, nose amber, mouth corners white */
        for (int i = 0; i < 5; i++) {
            const float px = x0 + vs.kp[i * 2] * w, py = vs.kp[i * 2 + 1] * h;
            sg_fill_round_rect(cv, px - 5, py - 5, 10, 10, 5, i < 2 ? green : (i == 2 ? amber : white), 1);
        }
    }
    /* motion: where (vertical line) and how much (bar at the bottom) */
    const float mx = x0 + vs.motion_x * w;
    if (vs.motion > 0.02f) {
        const float line[4] = {mx, 40, mx, (float)h - 30};
        sg_stroke_poly(cv, line, 2, false, 3, amber, 0.8f);
    }
    sg_fill_rect(cv, x0, h - 14, x0 + w, h, 0);
    const int bar = (int)(w * (vs.motion > 1 ? 1 : vs.motion));
    if (bar > 0) sg_fill_rect(cv, x0, h - 12, x0 + bar, h - 2, vs.motion >= 0.08f ? 0x07e0 : 0x8410);
    char buf[64];
    if (vs.face && vs.kp_ok)
        snprintf(buf, sizeof buf, "歪%+d° 转%+.2f 点%.2f", (int)(vs.roll * 57.3f), vs.yaw, vs.pitch);
    else
        snprintf(buf, sizeof buf, "%s  运动 %d%%", vs.face ? "看到人脸" : "没看到人脸", (int)(vs.motion * 100));
    sg_fill_rect(cv, x0, 0, x0 + w, 34, 0);
    sl_text_draw(cv, buf, x0 + 8, 24, SL_FONT_18, vs.face ? green : white, -1);
    if (vs.luma < 40) sl_text_draw(cv, "镜头被遮住了", x0 + 70, 240, SL_FONT_22, amber, -1);
    sl_text_draw(cv, "点屏幕退出", x0 + 90, h - 22, SL_FONT_14, white, -1);
    return true;
}

/* Camera thumbnail in the top-right corner (under the FPS readout), so you can see whether it
 * has you in frame. Drawn over the pet like the HUD: whenever the pet's dirty area touches it
 * or a new picture arrives. */
#define PIP_W 90
#define PIP_H 160
#define PIP_RECT ((sl_rect_t){SCR - 12 - PIP_W - 2, 42, SCR - 12, 42 + PIP_H + 4})
#define PIP_POPUP_S 20 /* shown this long after the camera comes up */
static uint16_t *s_pip; /* last thumbnail, PSRAM */
static bool s_pip_shown, s_open_camview;
static bool s_open_settings, s_open_help; /* raised by the board buttons, acted on in the main loop */

static bool pip_update(sg_canvas_t *cv, uint32_t *seq, bool force)
{
    if (!s_pip) s_pip = heap_caps_calloc(PIP_W * PIP_H, sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (!s_pip) return false;
    int w = 0, h = 0;
    const bool fresh = vision_preview_copy(s_pip, PIP_W, 0, PIP_W, PIP_H, seq, &w, &h);
    if (!fresh && !force) return false;
    const sl_rect_t r = PIP_RECT;
    const int x0 = r.x0 + 2, y0 = r.y0 + 2;
    for (int y = 0; y < PIP_H; y++) memcpy(&s_fb[(y0 + y) * SCR + x0], &s_pip[y * PIP_W], PIP_W * 2);
    vision_state_t vs;
    vision_get(&vs);
    const sg_rgb_t frame = vs.face ? sg_hex(0x6ee36e) : sg_hex(0x8d97ad);
    sg_stroke_round_rect(cv, r.x0 + 1, r.y0 + 1, PIP_W + 2, PIP_H + 2, 3, 2, frame, 1);
    if (vs.face) { /* where it sees your face */
        const float bx = x0 + vs.box[0] * PIP_W, by = y0 + vs.box[1] * PIP_H;
        sg_stroke_round_rect(cv, bx, by, (vs.box[2] - vs.box[0]) * PIP_W, (vs.box[3] - vs.box[1]) * PIP_H, 2, 1.5f, frame, 1);
    }
    return true;
}

/* Code points shared at the start of two UTF-8 strings (the typewriter keeps that part). */
static int common_prefix(const char *x, const char *y)
{
    int n = 0;
    while (*x && *x == *y) {
        if (((unsigned char)*x & 0xc0) != 0x80) n++;
        x++;
        y++;
    }
    /* a partially matching multi-byte character does not count */
    if (*x && ((unsigned char)*x & 0xc0) == 0x80) n--;
    return n;
}


#define PERF_PROBE 0
#if PERF_PROBE
#include "esp_cpu.h"
static volatile float s_sink;
/* One-shot micro benchmarks: cycles per operation, logged at boot. */
static void perf_probe(void)
{
    enum { N = 20000 };
    static uint16_t iram_buf[N];
    uint16_t *psram_buf = heap_caps_malloc(N * 2, MALLOC_CAP_SPIRAM);
    sg_rgb_t c = {0.3f, 0.5f, 0.8f};
    uint32_t t0, t;
    float acc = 0;

    t0 = esp_cpu_get_cycle_count();
    for (int i = 0; i < N; i++) iram_buf[i] = sg_pack(c, i, i >> 5);
    t = esp_cpu_get_cycle_count() - t0;
    ESP_LOGI(TAG, "probe sg_pack->internal  %5.1f cyc/px", (float)t / N);

    t0 = esp_cpu_get_cycle_count();
    for (int i = 0; i < N; i++) psram_buf[i] = sg_pack(c, i, i >> 5);
    t = esp_cpu_get_cycle_count() - t0;
    ESP_LOGI(TAG, "probe sg_pack->PSRAM     %5.1f cyc/px", (float)t / N);

    t0 = esp_cpu_get_cycle_count();
    for (int i = 0; i < N; i++) psram_buf[i] = (uint16_t)i;
    t = esp_cpu_get_cycle_count() - t0;
    ESP_LOGI(TAG, "probe plain store PSRAM  %5.1f cyc/px", (float)t / N);

    t0 = esp_cpu_get_cycle_count();
    for (int i = 0; i < N; i++) acc += sqrtf((float)i + acc * 1e-9f);
    t = esp_cpu_get_cycle_count() - t0;
    ESP_LOGI(TAG, "probe sqrtf              %5.1f cyc", (float)t / N);

    t0 = esp_cpu_get_cycle_count();
    for (int i = 0; i < N; i++) acc += 1.0f / ((float)i + 1.5f);
    t = esp_cpu_get_cycle_count() - t0;
    ESP_LOGI(TAG, "probe fdiv               %5.1f cyc", (float)t / N);

    t0 = esp_cpu_get_cycle_count();
    for (int i = 0; i < N; i++) acc += sinf((float)i * 0.001f);
    t = esp_cpu_get_cycle_count() - t0;
    ESP_LOGI(TAG, "probe sinf               %5.1f cyc", (float)t / N);

    t0 = esp_cpu_get_cycle_count();
    for (int i = 0; i < N; i++) {
        const sg_rgb_t d = sg_lerp(c, (sg_rgb_t){1, 1, 1}, (float)(i & 255) / 255.0f);
        acc += d.r + d.g + d.b;
    }
    t = esp_cpu_get_cycle_count() - t0;
    ESP_LOGI(TAG, "probe sg_lerp            %5.1f cyc", (float)t / N);

    sg_canvas_t pc;
    sg_canvas_init(&pc, s_fb, SCR, SCR, SCR);
    t0 = esp_cpu_get_cycle_count();
    for (int i = 0; i < N; i++) sg_blend(&pc, i % SCR, 100 + (i / SCR) % 50, c, 0.5f);
    t = esp_cpu_get_cycle_count() - t0;
    ESP_LOGI(TAG, "probe sg_blend 50%% PSRAM %5.1f cyc/px", (float)t / N);

    s_sink = acc;
    free(psram_buf);
}
#endif

static void render_part(sg_canvas_t *cv, const sl_pose_t *p, sl_rect_t d)
{
    double t0 = now_s();
    sg_set_clip(cv, d.x0, d.y0, d.x1, d.y1);
    sg_fill_rect(cv, d.x0, d.y0, d.x1, d.y1, 0);
    cv->scratch->prof[SL_PROF_CLEAR] += now_s() - t0;
    sl_render_slime(cv, p);
    t0 = now_s();
    if (p->dim < 1) sg_dim_rect(cv, d.x0, d.y0, d.x1, d.y1, p->dim);
    cv->scratch->prof[SL_PROF_CLEAR] += now_s() - t0;
    sg_reset_clip(cv);
}

static void render_worker(void *arg)
{
    for (;;) {
        xSemaphoreTake(s_go, portMAX_DELAY);
        render_part(&s_cv[1], s_job_pose, s_job_dirty);
        xSemaphoreGive(s_done);
    }
}

#define FPS_RECT ((sl_rect_t){376, 12, 468, 38})

static void draw_fps(sg_canvas_t *cv, float fps)
{
    const sl_rect_t r = FPS_RECT;
    sg_fill_rect(cv, r.x0, r.y0, r.x1, r.y1, 0);
    char buf[16];
    snprintf(buf, sizeof buf, "%4.1f fps", fps);
    const sg_rgb_t c = fps >= 28 ? sg_hex(0x6ee36e) : (fps >= 18 ? sg_hex(0xffd84a) : sg_hex(0xff5050));
    sl_text_draw(cv, buf, r.x0 + 6, r.y0 + 19, SL_FONT_14, c, -1);
}

static inline bool rect_hit(sl_rect_t a, sl_rect_t b)
{
    return !(a.x1 <= b.x0 || a.x0 >= b.x1 || a.y1 <= b.y0 || a.y0 >= b.y1);
}

void app_main(void)
{
    /* first thing: on battery the PWR_SW shutdown line must be released (see bootloader_components) */
    if (bsp_power_init() != ESP_OK) ESP_LOGW(TAG, "power controls unavailable");
    const esp_err_t ue = usb_link_init();
    if (ue != ESP_OK) ESP_LOGW(TAG, "USB console unavailable: %s", esp_err_to_name(ue));

    const esp_err_t de = init_display();
    if (de != ESP_OK) fail_blink(de);
    if (cfg_init() != ESP_OK) ESP_LOGW(TAG, "NVS unavailable, settings and progress will not persist");
    ota_init();
    bootlog_init();
    cfg_get(&s_b.cfg);
    char init_err[48];
    init_inputs(init_err, sizeof init_err);
    bsp_board_variant_t variant;
    if (bsp_board_variant_get(&variant) == ESP_OK) ESP_LOGI(TAG, "board variant %d", (int)variant);
    if (haptic_init() != ESP_OK) ESP_LOGW(TAG, "motor unavailable");
    sensors_start();
    audio_start();
    vision_enable(s_b.cfg.camera);
    vision_start();
    inbox_init();
    const esp_err_t ne = net_start();
    if (ne != ESP_OK) ESP_LOGW(TAG, "Wi-Fi unavailable: %s", esp_err_to_name(ne));
    else if (web_start() != ESP_OK) ESP_LOGW(TAG, "web panel unavailable");
    progress_load(&s_b);
    cc_init(&s_b.cc);
    sl_render_init();
    sl_render_set_clock(now_s);
#if PERF_PROBE
    vTaskDelay(pdMS_TO_TICKS(2500)); /* give the host time to open the USB console */
    perf_probe();
#endif

    sg_canvas_t cv; /* full-frame canvas for HUD, dialog and overlays (main core only) */
    sg_canvas_init(&cv, s_fb, SCR, SCR, SCR);
    sg_fill_rect(&cv, 0, 0, SCR, SCR, 0);
    for (int i = 0; i < 2; i++) {
        sg_canvas_init(&s_cv[i], s_fb, SCR, SCR, SCR);
        sg_canvas_set_split(&s_cv[i], &s_scratch[i], i);
    }
    s_go = xSemaphoreCreateBinary();
    s_done = xSemaphoreCreateBinary();
    const bool dual = s_go && s_done && xTaskCreatePinnedToCore(render_worker, "render1", 12288, NULL, 5, NULL, 1) == pdPASS;
    if (!dual) ESP_LOGW(TAG, "render worker unavailable, single-core rendering");

    sl_anim_t a;
    double prev = now_s();
    sl_anim_init(&a, prev);
    a.lv = s_b.lv;
    s_b.last_activity = prev;
    s_b.last_motion = prev;
    s_b.react_t0 = -1;

    sl_rect_t last = {0, 0, SCR, SCR}; /* first frame pushes everything */
    char msg[128] = "", shown_msg[128] = "", typed_msg[128] = "", line[INBOX_LINE_MAX];
    double next_pub = 0;
    perf_t perf = {0};
    int shown_chars = -1, typed_keep = 0;
    double msg_t0 = prev;
    int brightness = 100;
    bool shown_cursor = false;
    int hud_lv = -1, hud_hp = -1;
    uint32_t stat_frames = 0;
    double stat_t = prev;
    float fps_shown = -1, fps_now = 0;

    /* the main loop runs ~20 times a second; if it ever stops for 5 s (a spin, or a driver waiting
     * forever) the task watchdog reboots and the core dump says where it was stuck */
    esp_task_wdt_add(NULL);
    while (true) {
        esp_task_wdt_reset();
        static bool confirmed;
        if (!confirmed && now_s() > 30) { /* 30 s of a running main loop: a fresh update is good */
            confirmed = true;
            ota_confirm_if_pending();
        }
        const double now = now_s();
        const float dt = fminf(0.05f, (float)(now - prev));
        prev = now;

        /* buttons: AI click = mute / unmute, AI hold = talk (help page with voice off); BOOT click = focus timer,
         * BOOT hold = settings. Any of them closes the settings when open. */
        const int ev = atomic_exchange(&s_event, EV_NONE);
        const bool ai_up = atomic_exchange(&s_ai_up, false);
        if (s_b.listening && (ai_up || !audio_rec_active())) voice_stop(&s_b, &a, now); /* released, or 10 s full */
        voice_tick(&s_b, &a, now);
        if (ev != EV_NONE && sui_active()) {
            sui_close();
        } else if (ev == EV_AI_CLICK) {
            slime_cfg_t c;
            cfg_get(&c);
            c.sound = !c.sound;
            cfg_set(&c);
            react(&s_b, &a, SL_POKE_R, now, c.sound ? "声音打开了！" : "嘘……已经静音了。");
            buzz(HAPTIC_TICK);
            if (c.sound) audio_play(SFX_HELLO);
        } else if (ev == EV_AI_LONG && s_b.cfg.voice && !s_b.demo) {
            voice_start(&s_b, &a, now);
        } else if (ev == EV_AI_LONG) {
            s_open_help = true;
            buzz(HAPTIC_TICK);
        } else if (ev == EV_BOOT_CLICK) {
            if (s_b.focus_end || s_b.break_end) focus_stop(&s_b, &a, now);
            else focus_start(&s_b, &a, now);
            buzz(HAPTIC_TICK);
        } else if (ev == EV_BOOT_LONG) {
            s_open_settings = true;
        }
        if (ev != EV_NONE) s_b.last_activity = now;
        double ti = now_s();
        const touch_t tp = poll_touch();
        s_t_touch += now_s() - ti;
        static double press_t0;
        static bool long_done;
        static bool camview;
        static double camview_t0;
        static uint32_t camview_seq;
        static bool helpview;
        static double help_t0;
        if (helpview) {
            if (tp.pressed || ev != EV_NONE || now - help_t0 > 60) helpview = false;
        } else if (camview) {
            if (tp.pressed || ev != EV_NONE || now - camview_t0 > CAMVIEW_TIMEOUT_S) {
                camview = false;
                vision_set_preview(VISION_PV_OFF);
            }
        } else if (sui_active()) {
            if (tp.pressed) ESP_LOGI(TAG, "touch %d,%d (menu)", tp.x, tp.y);
            if (tp.pressed && sui_tap(tp.x, tp.y, now)) buzz(HAPTIC_TICK);
        } else {
            if (tp.pressed) ESP_LOGI(TAG, "touch %d,%d", tp.x, tp.y);
            const sl_rect_t pr = PIP_RECT;
            if (tp.pressed && s_pip_shown && tp.x >= pr.x0 && tp.x < pr.x1 && tp.y >= pr.y0 && tp.y < pr.y1) {
                s_open_camview = true; /* tap the thumbnail: full camera view */
                buzz(HAPTIC_TICK);
            } else if (tp.pressed) { /* a tap pokes; holding on opens the settings */
                sl_anim_set_state(&a, tp.x < SCR / 2 ? SL_POKE_L : SL_POKE_R, now);
                s_b.last_activity = now;
                buzz(HAPTIC_TICK);
                sfx(&a, SFX_POKE);
                press_t0 = now;
                long_done = false;
            }
            if (tp.down && !long_done && now - press_t0 > LONG_PRESS_S && s_b.demo) {
                long_done = true; /* during the demo a long press ends it (taps do not: you may be filming) */
                demo_stop(&s_b, &a, now);
                buzz(HAPTIC_NUDGE);
            } else if ((tp.down && !long_done && now - press_t0 > LONG_PRESS_S) || s_open_settings) {
                long_done = true;
                s_open_settings = false;
                net_status_t ns;
                net_get(&ns);
                char wifi[48], addr[64];
                if (ns.state == NET_CONNECTED) {
                    snprintf(wifi, sizeof wifi, "%s  %d dBm", ns.ssid, ns.rssi);
                    snprintf(addr, sizeof addr, "%s  %s", ns.ssid, ns.ip);
                } else {
                    snprintf(wifi, sizeof wifi, "%s", ns.state == NET_NO_CONFIG ? "未设置" : "未连接");
                    snprintf(addr, sizeof addr, "-");
                }
                sui_open(wifi, addr, s_b.lv, s_b.exp, exp_need(s_b.lv), now);
                buzz(HAPTIC_NUDGE);
                audio_play(SFX_HELLO);
            }
        }
        inbox_src_t src;
        while (inbox_pop(line, sizeof line, &src)) handle_line(&s_b, &a, line, src, now);
        watch_net(&s_b, now);
        handle_sensors(&s_b, &a, now);
        poll_battery(&s_b, &a, now);
        usb_link_poll();

        /* brain: fall back to the persistent state once reactions and manual picks are over */
        const char *cc_detail;
        int cc_busy;
        const cc_status_t cs = cc_status(&s_b.cc, now, &cc_detail, &cc_busy);
        vision_state_t vsn;
        vision_get(&vsn);
        const bool in_demo = demo_tick(&s_b, &a, vsn.ok, now);
        if (!in_demo) {
            handle_vision(&s_b, &a, cs, now);
            maybe_hum(&s_b, &a, cs, now);
            focus_tick(&s_b, &a, now);
        }
        const sl_state_t want = want_base(&s_b, cs, now);
        if (!in_demo && sl_state_duration(a.state) == 0 && now >= s_b.manual_until && a.state != want) {
            if (a.state == SL_SLEEP && want == SL_IDLE) {
                react(&s_b, &a, SL_GREET, now, "史莱姆醒过来了！");
                sfx(&a, SFX_WAKE);
            } else {
                if (want == SL_SLEEP) sfx(&a, SFX_SLEEP);
                sl_anim_set_state(&a, want, now);
            }
        }
        if (a.state != SL_LEVELUP) a.lv = s_b.lv;
        static uint32_t cfg_seen = UINT32_MAX;
        if (cfg_gen() != cfg_seen) {
            cfg_seen = cfg_gen();
            cfg_get(&s_b.cfg);
            haptic_set_enabled(s_b.cfg.motor);
            sensors_set_led_brightness(s_b.cfg.led_bright);
            audio_configure(s_b.cfg.mic, s_b.cfg.clap_sens, s_b.cfg.dance, s_b.cfg.sound, s_b.cfg.volume);
            vision_enable(s_b.cfg.camera);
            static bool greeted;
            if (!greeted) { /* the first time the settings are applied: title-screen fanfare */
                greeted = true;
                audio_play(SFX_BOOT);
            }
        }
        { /* quiet hours: no sound, no LEDs (unless Claude needs you), dimmer screen */
            const bool night = !s_b.demo && night_now(&s_b);
            if (night != s_b.night) {
                s_b.night = night;
                audio_set_muted(night);
                ESP_LOGI(TAG, "quiet hours %s", night ? "on" : "off");
            }
        }
        sensors_set_mood(s_b.night && a.state != SL_WAIT ? MOOD_OFF : mood_for(a.state, s_b.cfg.idle_breath));
        int want_bright = a.state == SL_SLEEP ? s_b.cfg.sleep_bright : s_b.cfg.screen_bright;
        if (s_b.night && a.state != SL_SLEEP) want_bright = want_bright / 2 > 10 ? want_bright / 2 : 10;
        if (want_bright != brightness && bsp_display_brightness_set(want_bright) == ESP_OK) brightness = want_bright;

        /* settings screen replaces the pet; everything above keeps running underneath */
        static bool menu_shown;
        sui_tick(now);
        const int sui_act = sui_take_action();
        if (sui_act == SUI_ACT_BGM) audio_play_bgm((bgm_t)(s_b.hum_idx++ % BGM_COUNT));
        if (sui_act == SUI_ACT_FOCUS) {
            if (s_b.focus_end || s_b.break_end) focus_stop(&s_b, &a, now);
            else focus_start(&s_b, &a, now);
        }
        if (sui_act == SUI_ACT_DEMO) demo_start(&s_b, &a, now);
        if (sui_act == SUI_ACT_HELP || s_open_help) {
            s_open_help = false;
            helpview = true;
            help_t0 = now;
            draw_help(&cv);
            push_rect((sl_rect_t){0, 0, SCR, SCR});
        }
        if (helpview) {
            menu_shown = true; /* repaint the pet on exit */
            s_b.last_activity = now;
            vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        }
        if (sui_act == SUI_ACT_CAMVIEW || s_open_camview) {
            s_open_camview = false;
            s_pip_shown = false;
            camview = true;
            camview_t0 = now;
            camview_seq = 0;
            vision_set_preview(VISION_PV_FULL);
            sg_fill_rect(&cv, 0, 0, SCR, SCR, 0);
            push_rect((sl_rect_t){0, 0, SCR, SCR});
        }
        if (camview) {
            if (draw_camview(&cv, s_fb, &camview_seq)) push_rect((sl_rect_t){(SCR - 270) / 2, 0, (SCR + 270) / 2, SCR});
            menu_shown = true; /* repaint the pet on exit */
            s_b.last_activity = now;
            vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        }
        if (sui_active()) {
            if (sui_dirty()) {
                sui_render(&cv);
                push_rect((sl_rect_t){0, 0, SCR, SCR});
            }
            menu_shown = true;
            s_b.last_activity = now;
            vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        }
        if (menu_shown) { /* just closed: repaint everything */
            menu_shown = false;
            sg_fill_rect(&cv, 0, 0, SCR, SCR, 0);
            last = (sl_rect_t){0, 0, SCR, SCR};
            hud_lv = -1;
            shown_msg[0] = 0;
            shown_chars = -1;
            fps_shown = -2;
        }

        sl_pose_t p;
        ti = now_s();
        sl_anim_step(&a, now, dt, &p);
        s_t_anim += now_s() - ti;

        const double r0 = now_s();
        /* slime region never reaches the dialog window */
        sl_rect_t cur = sl_render_bounds(&p);
        if (cur.y1 > SL_DIALOG_RECT.y0) cur.y1 = SL_DIALOG_RECT.y0;
        sl_rect_t dirty = sl_rect_union(last, cur);
        if (dirty.y1 > SL_DIALOG_RECT.y0) dirty.y1 = SL_DIALOG_RECT.y0;
        /* DMA only reads s_wire, so rendering into s_fb overlaps the previous transfer */
        if (dual) {
            s_job_pose = &p;
            s_job_dirty = dirty;
            xSemaphoreGive(s_go);
            render_part(&s_cv[0], &p, dirty);
            const double tj = now_s();
            xSemaphoreTake(s_done, portMAX_DELAY);
            s_scratch[0].prof[SL_PROF_JOIN] += now_s() - tj;
        } else {
            render_part(&cv, &p, dirty);
        }

        /* HUD: redraw when it changed or the slime region overlapped it */
        const sl_rect_t hud = SL_HUD_RECT;
        const bool hud_hit = rect_hit(dirty, hud);
        if (hud_hit || hud_lv != a.lv || hud_hp != (int)a.hp) {
            if (!hud_hit) sg_fill_rect(&cv, hud.x0, hud.y0, hud.x1, hud.y1, 0);
            sl_render_hud(&cv, a.lv, a.hp);
            hud_lv = a.lv;
            hud_hp = (int)a.hp;
            dirty = sl_rect_union(dirty, hud);
        }
        const sl_rect_t fr = FPS_RECT;
        if (s_b.cfg.show_fps && (rect_hit(dirty, fr) || (int)(fps_now * 10) != (int)(fps_shown * 10))) {
            draw_fps(&cv, fps_now);
            fps_shown = fps_now;
            dirty = sl_rect_union(dirty, fr);
        } else if (!s_b.cfg.show_fps && fps_shown >= 0) { /* switched off: erase once */
            if (!rect_hit(dirty, fr)) sg_fill_rect(&cv, fr.x0, fr.y0, fr.x1, fr.y1, 0);
            fps_shown = -1;
            dirty = sl_rect_union(dirty, fr);
        }
        bool pip_push = false; /* pushed on its own: a union with the pet's area would drag in the gap between */
        { /* camera thumbnail */
            static bool cam_was_ok;
            static double pip_until;
            static uint32_t pip_seq;
            vision_state_t vs;
            vision_get(&vs);
            if (vs.ok && !cam_was_ok) pip_until = now + PIP_POPUP_S; /* plugged in (or booted): show what it sees */
            cam_was_ok = vs.ok;
            const bool want = vs.ok && (s_b.cfg.cam_pip || now < pip_until || s_b.demo_pip);
            const sl_rect_t pr = PIP_RECT;
            if (want) {
                if (!s_pip_shown) {
                    vision_set_preview(VISION_PV_PIP);
                    pip_seq = 0;
                }
                const bool hit = rect_hit(dirty, pr); /* the pet's repaint covered it: redraw, ride along */
                if (pip_update(&cv, &pip_seq, !s_pip_shown || hit)) {
                    if (hit) dirty = sl_rect_union(dirty, pr);
                    else pip_push = true; /* only a new picture: a second, small push */
                }
                s_pip_shown = true;
            } else if (s_pip_shown) { /* gone: give the corner back to the pet */
                s_pip_shown = false;
                vision_set_preview(VISION_PV_OFF);
                sg_fill_rect(&cv, pr.x0, pr.y0, pr.x1, pr.y1, 0);
                pip_push = true;
                cur = sl_rect_union(cur, pr); /* next frame repaints the pet there */
                fps_shown = -2;               /* the FPS readout sits right above */
            }
        }
        s_t_render += now_s() - r0;
        push_rect(dirty);
        if (pip_push) push_rect(PIP_RECT);
        last = cur;

        /* dialog: typewriter, blinking cursor when complete; new text keeps the shared prefix */
        compose_msg(&s_b, &a, cs, cc_detail, cc_busy, now, msg, sizeof msg);
        if (init_err[0] && now < 8) snprintf(msg, sizeof msg, "init failed:%s", init_err);
        if (strcmp(msg, typed_msg)) {
            const int keep = common_prefix(msg, typed_msg);
            typed_keep = shown_chars < keep ? (shown_chars > 0 ? shown_chars : 0) : keep;
            strcpy(typed_msg, msg);
            msg_t0 = now;
        }
        const int total = sl_text_count(msg);
        const int vis = typed_keep + (int)((now - msg_t0) * TYPE_CPS);
        const bool cursor = vis >= total && ((long)floor(now * 2.5) % 2);
        const int vis_c = vis > total ? total : vis;
        if (strcmp(msg, shown_msg) || vis_c != shown_chars || cursor != shown_cursor) {
            const double td = now_s();
            const sl_rect_t dr = SL_DIALOG_RECT;
            sg_fill_rect(&cv, dr.x0, dr.y0, dr.x1, dr.y1, 0);
            sl_render_dialog(&cv, msg, vis_c, cursor);
            if (s_b.cfg.text_blip && vis_c > shown_chars && shown_chars >= 0 && (vis_c & 1)) sfx(&a, SFX_BLIP);
            push_rect(dr);
            strcpy(shown_msg, msg);
            shown_chars = vis_c;
            shown_cursor = cursor;
            s_scratch[0].prof[SL_PROF_DIALOG] += now_s() - td;
        }

        if (now >= next_pub) {
            next_pub = now + 0.5;
            publish_status(&s_b, &a, msg, cs, cc_detail, cc_busy, perf, now);
        }

        stat_frames++;
        if (now - stat_t >= 2.0) {
            const double n = stat_frames;
            fps_now = (float)(n / (now - stat_t));
            perf.fps = fps_now;
            perf.render_ms = (float)(s_t_render / n * 1000);
            ESP_LOGI(TAG, "%.1f fps | render %.1f ms | copy %.1f ms | dma wait %.1f ms | pushed %ld kpx/frame | %s | %s | PSRAM free %u KB | internal free %u KB",
                     fps_now, s_t_render / n * 1000, s_t_copy / n * 1000, s_t_wait / n * 1000, (long)(s_px_pushed / n / 1000),
                     dual ? "2 cores" : "1 core", sl_state_name(a.state), (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
            const double *q0 = s_scratch[0].prof, *q1 = s_scratch[1].prof;
            ESP_LOGI(TAG, "  ms/frame core0|core1: shadow %.1f|%.1f body %.1f|%.1f face %.1f|%.1f fx %.1f|%.1f clear %.1f|%.1f join %.1f dialog %.1f",
                     q0[0] / n * 1e3, q1[0] / n * 1e3, q0[2] / n * 1e3, q1[2] / n * 1e3, q0[3] / n * 1e3, q1[3] / n * 1e3,
                     q0[4] / n * 1e3, q1[4] / n * 1e3, q0[5] / n * 1e3, q1[5] / n * 1e3, q0[6] / n * 1e3, q0[7] / n * 1e3);
            ESP_LOGI(TAG, "  touch %.1f ms  anim %.1f ms | cc %d busy %d lines ok %u bad %u stale %u | Lv %d %d/%d",
                     s_t_touch / n * 1e3, s_t_anim / n * 1e3, (int)cs, cc_busy, (unsigned)s_b.cc.lines_ok,
                     (unsigned)s_b.cc.lines_bad, (unsigned)s_b.cc.lines_stale, s_b.lv, s_b.exp, exp_need(s_b.lv));
            sensors_state_t ss;
            sensors_get(&ss);
            ESP_LOGI(TAG, "  imu %s acc %+.2f %+.2f %+.2f tilt %+.2f%s slide %+.1f | module %s (tries %d, last %s) motion %d light %u",
                     ss.imu_ok ? "ok" : "--", ss.ax, ss.ay, ss.az, ss.tilt, ss.face_down ? " FACE-DOWN" : "", a.j_tdx,
                     ss.mod_ok ? "ok" : "--", ss.mod_tries, esp_err_to_name(ss.mod_err), ss.motion, ss.light);
            s_t_touch = s_t_anim = 0;
            for (int i = 0; i < 2; i++) memset(s_scratch[i].prof, 0, sizeof s_scratch[i].prof);
            stat_t = now;
            stat_frames = 0;
            s_t_render = s_t_copy = s_t_wait = 0;
            s_px_pushed = 0;
        }
        const int spent = (int)((now_s() - now) * 1000);
        vTaskDelay(pdMS_TO_TICKS(spent < FRAME_MS ? FRAME_MS - spent : 1));
    }
}
