#include "ha_mqtt.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "config.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "inbox.h"
#include "mqtt_client.h"
#include "net.h"
#include "slime_text.h"

static const char *TAG = "ha";

#ifndef HA_SLOW_S
#define HA_SLOW_S 30 /* light, battery, level: at most this often; everything else at once */
#endif
#define HA_PREFIX "homeassistant" /* Home Assistant's discovery prefix (its default) */
#define HA_STACK 6144             /* PSRAM, like the client's own task: see sdkconfig.defaults */
#define HA_TICK_MS 200
#define HA_SAY_MAX 95 /* bytes of a "say" that are shown: the dialog's buffers hold 96 */
_Static_assert(sizeof "talk happy " + HA_SAY_MAX <= INBOX_LINE_MAX, "a say has to fit one inbox line");

/* ---- the entities ---- */

typedef enum { V_NONE, V_ONOFF, V_NUM, V_CLAUDE } kind_t;

static const char *const CLAUDE_STATES[] = {"idle", "think", "work", "wait"};

/* State comes from the JSON on slime/<id>/state under key; commands arrive on slime/<id>/set/<key>.
 * The names are only ever shown by Home Assistant, never on the pet's own screen. */
static const struct {
    const char *comp, *key, *zh, *en, *extra; /* extra: more discovery config, as JSON members */
    kind_t kind;
    bool slow;
} ENT[] = {
    {"binary_sensor", "motion", "有人", "Someone there", "\"device_class\":\"occupancy\"", V_ONOFF, false},
    {"binary_sensor", "face", "看到你", "Sees you", "\"device_class\":\"occupancy\"", V_ONOFF, false},
    {"sensor", "light", "环境光", "Ambient light", "\"state_class\":\"measurement\",\"icon\":\"mdi:brightness-5\"", V_NUM, true},
    {"sensor", "battery", "电量", "Battery",
     "\"device_class\":\"battery\",\"unit_of_measurement\":\"%\",\"state_class\":\"measurement\"", V_NUM, true},
    {"binary_sensor", "charging", "充电中", "Charging", "\"device_class\":\"battery_charging\"", V_ONOFF, false},
    {"sensor", "claude", "Claude 状态", "Claude",
     "\"device_class\":\"enum\",\"options\":[\"idle\",\"think\",\"work\",\"wait\"],\"icon\":\"mdi:robot\"", V_CLAUDE, false},
    {"binary_sensor", "waiting", "Claude 在等你", "Claude waiting for you", "\"icon\":\"mdi:account-clock\"", V_ONOFF, false},
    {"sensor", "level", "等级", "Level", "\"icon\":\"mdi:star\"", V_NUM, true},
    {"binary_sensor", "night", "夜间勿扰中", "Quiet hours now", "\"icon\":\"mdi:weather-night\"", V_ONOFF, false},
    {"sensor", "focus", "专注剩余", "Focus left", "\"unit_of_measurement\":\"min\",\"icon\":\"mdi:timer-sand\"", V_NUM, false},
    {"notify", "say", "说话", "Say", NULL, V_NONE, false},
    {"button", "focus_start", "开始专注", "Start focus", "\"icon\":\"mdi:timer-play\"", V_NONE, false},
    {"button", "focus_stop", "结束专注", "Stop focus", "\"icon\":\"mdi:timer-off\"", V_NONE, false},
    {"number", "volume", "音量", "Volume", "\"min\":0,\"max\":100,\"step\":5,\"icon\":\"mdi:volume-high\"", V_NUM, false},
    {"number", "screen_bright", "屏幕亮度", "Screen brightness",
     "\"min\":10,\"max\":100,\"step\":5,\"icon\":\"mdi:brightness-6\"", V_NUM, false},
    {"switch", "bgm", "背景音乐", "Background music", "\"icon\":\"mdi:music\"", V_ONOFF, false},
    {"switch", "quiet", "夜间勿扰", "Quiet hours", "\"icon\":\"mdi:bell-sleep\"", V_ONOFF, false},
};
#define ENT_COUNT ((int)(sizeof ENT / sizeof ENT[0]))
enum { E_MOTION, E_FACE, E_LIGHT, E_BATTERY, E_CHARGING, E_CLAUDE, E_WAITING, E_LEVEL, E_NIGHT, E_FOCUS,
       E_SAY, E_FOCUS_START, E_FOCUS_STOP, E_VOLUME, E_SCREEN_BRIGHT, E_BGM, E_QUIET };
#define V_NULL (-1) /* a value the pet cannot read right now */

typedef struct {
    int v[sizeof ENT / sizeof ENT[0]];
} values_t;

/* ---- settings and state shared between tasks ---- */

typedef struct {
    char url[129], user[65], pass[AI_VALUE_MAX + 1], id[25];
} settings_t;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static settings_t s_set;   /* under s_lock; written by ha_start / ha_reload */
static ha_state_t s_state; /* under s_lock; written by the main loop */
static atomic_bool s_have_state, s_reload, s_connected, s_announce, s_task;
static atomic_bool s_refused; /* the broker answered and said no: wrong user name or password */
static atomic_int s_status; /* 0 off, 1 connecting, 2 connected */

/* only the ha task */
static esp_mqtt_client_handle_t s_client;
static char s_base[40];        /* "slime/<id>" */
static char s_set_prefix[48];  /* "slime/<id>/set/" */

static void read_settings(settings_t *out)
{
    strlcpy(out->url, ai_get(AI_MQTT_URL), sizeof out->url);
    strlcpy(out->user, ai_get(AI_MQTT_USER), sizeof out->user);
    strlcpy(out->pass, ai_get(AI_MQTT_PASS), sizeof out->pass);
    strlcpy(out->id, ai_get(AI_MQTT_ID)[0] ? ai_get(AI_MQTT_ID) : "slime", sizeof out->id);
}

/* ---- commands from Home Assistant (the client's task: no NVS here, its stack is PSRAM) ---- */

static bool topic_is(const esp_mqtt_event_handle_t e, const char *topic)
{
    return (size_t)e->topic_len == strlen(topic) && !memcmp(e->topic, topic, e->topic_len);
}

static void on_command(const char *key, const char *text)
{
    char line[INBOX_LINE_MAX];
    if (!strcmp(key, "say")) {
        /* as much as the dialog holds (main.c: react_msg, notice), cut between characters, not inside one */
        const int head = snprintf(line, sizeof line, "talk happy ");
        int n = head;
        for (const unsigned char *p = (const unsigned char *)text; *p;) {
            const int len = *p < 0x80 ? 1 : *p < 0xE0 ? 2 : *p < 0xF0 ? 3 : 4;
            if (n - head + len > HA_SAY_MAX || strnlen((const char *)p, len) < (size_t)len) break;
            if (*p < 0x20) line[n++] = ' '; /* a line break would end the inbox line */
            else memcpy(line + n, p, len), n += len;
            p += len;
        }
        line[n] = 0;
        if (n == head) return; /* nothing to say */
    } else if (!strcmp(key, "focus_start") || !strcmp(key, "focus_stop")) {
        snprintf(line, sizeof line, "focus %s", key + 6);
    } else if (!strcmp(key, "volume") || !strcmp(key, "screen_bright")) {
        char *end;
        const double v = strtod(text, &end);
        if (end == text) return;
        snprintf(line, sizeof line, "set %s %d", key, v < 0 ? 0 : v > 100 ? 100 : (int)v);
    } else if (!strcmp(key, "bgm") || !strcmp(key, "quiet")) {
        snprintf(line, sizeof line, "set %s %d", key, !strcasecmp(text, "ON"));
    } else {
        return;
    }
    if (!inbox_push(line, SRC_MQTT)) ESP_LOGW(TAG, "command dropped: the inbox is full");
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    const esp_mqtt_event_handle_t e = data;
    if (id == MQTT_EVENT_CONNECTED) {
        char topic[56];
        snprintf(topic, sizeof topic, "%s#", s_set_prefix);
        esp_mqtt_client_subscribe_single(e->client, topic, 0);
        esp_mqtt_client_subscribe_single(e->client, HA_PREFIX "/status", 0);
        atomic_store(&s_announce, true);
        atomic_store(&s_connected, true);
        atomic_store(&s_refused, false);
        atomic_store(&s_status, 2);
        ESP_LOGI(TAG, "connected, topics %s/...", s_base);
    } else if (id == MQTT_EVENT_DISCONNECTED) {
        if (atomic_exchange(&s_connected, false)) ESP_LOGW(TAG, "connection lost; the client retries by itself");
        atomic_store(&s_status, 1);
    } else if (id == MQTT_EVENT_ERROR) {
        /* the client keeps trying either way; the panel can at least say why it will not work */
        if (e->error_handle && e->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED &&
            !atomic_exchange(&s_refused, true)) {
            ESP_LOGW(TAG, "the broker refused the connection (code %d): check the user name and password",
                     (int)e->error_handle->connect_return_code);
        }
    } else if (id == MQTT_EVENT_DATA) {
        if (e->current_data_offset || e->data_len != e->total_data_len) return; /* longer than the buffer: not ours */
        char text[400];
        int n = e->data_len < (int)sizeof text - 1 ? e->data_len : (int)sizeof text - 1;
        memcpy(text, e->data, n);
        while (n > 0 && (unsigned char)text[n - 1] <= ' ') n--;
        text[n] = 0;
        const char *t = text;
        while (*t && (unsigned char)*t <= ' ') t++;
        if (topic_is(e, HA_PREFIX "/status")) {
            if (!strcmp(t, "online")) atomic_store(&s_announce, true); /* Home Assistant restarted: it wants the discovery again */
            return;
        }
        const size_t pl = strlen(s_set_prefix);
        char key[24];
        if ((size_t)e->topic_len <= pl || (size_t)e->topic_len - pl >= sizeof key || memcmp(e->topic, s_set_prefix, pl)) return;
        memcpy(key, e->topic + pl, e->topic_len - pl);
        key[e->topic_len - pl] = 0;
        on_command(key, t);
    }
}

/* ---- publishing (the ha task) ---- */

static bool publish(const char *topic, const char *payload)
{
    return esp_mqtt_client_publish(s_client, topic, payload, 0, 0, 1) >= 0; /* QoS 0, retained */
}

static void json_esc(char *dst, size_t len, const char *src)
{
    size_t n = 0;
    for (; *src && n + 2 < len; src++) {
        if (*src == '"' || *src == '\\') dst[n++] = '\\';
        dst[n++] = *src;
    }
    dst[n] = 0;
}

/* Every entity's discovery config, then "online". */
static bool announce(const settings_t *set)
{
    const bool en = sl_lang == SL_LANG_EN;
    net_status_t ns;
    net_get(&ns);
    char *topic = heap_caps_malloc(128 + 1024, MALLOC_CAP_SPIRAM), *js = topic ? topic + 128 : NULL;
    if (!topic) return false;
    bool ok = true;
    for (int i = 0; i < ENT_COUNT && ok; i++) {
        const char *c = ENT[i].comp, *k = ENT[i].key;
        const bool has_state = strcmp(c, "notify") && strcmp(c, "button");
        const bool has_cmd = strcmp(c, "sensor") && strcmp(c, "binary_sensor");
        char name[64];
        json_esc(name, sizeof name, en ? ENT[i].en : ENT[i].zh);
        int n = snprintf(js, 1024,
                         "{\"name\":\"%s\",\"unique_id\":\"slime_%s_%s\",\"device\":{\"identifiers\":[\"slime_pet_%s\"],"
                         "\"name\":\"%s\",\"manufacturer\":\"slime-pet\",\"model\":\"ESP-Mosaico\","
                         "\"configuration_url\":\"http://%s/\"},\"availability_topic\":\"%s/status\"",
                         name, set->id, k, set->id, en ? "Slime Pet" : "史莱姆桌宠", ns.ip[0] ? ns.ip : "slime.local", s_base);
        if (has_state) n += snprintf(js + n, 1024 - n, ",\"state_topic\":\"%s/state\",\"value_template\":\"{{ value_json.%s }}\"", s_base, k);
        if (has_cmd) n += snprintf(js + n, 1024 - n, ",\"command_topic\":\"%s%s\"", s_set_prefix, k);
        if (ENT[i].extra) n += snprintf(js + n, 1024 - n, ",%s", ENT[i].extra);
        snprintf(js + n, 1024 - n, "}");
        snprintf(topic, 128, HA_PREFIX "/%s/slime_%s/%s/config", c, set->id, k);
        ok = publish(topic, js);
    }
    snprintf(topic, 128, "%s/status", s_base);
    ok = ok && publish(topic, "online");
    free(topic);
    return ok;
}

static void current_values(values_t *out)
{
    ha_state_t s;
    portENTER_CRITICAL(&s_lock);
    s = s_state;
    portEXIT_CRITICAL(&s_lock);
    slime_cfg_t c;
    cfg_get(&c);
    int *v = out->v;
    for (int i = 0; i < ENT_COUNT; i++) v[i] = V_NULL;
    if (s.mod_ok) v[E_MOTION] = s.motion, v[E_LIGHT] = (int)s.light;
    if (s.cam_ok) v[E_FACE] = s.face;
    if (s.bat_ok) v[E_BATTERY] = s.soc, v[E_CHARGING] = s.charging;
    v[E_CLAUDE] = s.claude < 4 ? s.claude : V_NULL;
    v[E_WAITING] = s.claude == 3;
    v[E_LEVEL] = s.level;
    v[E_NIGHT] = s.night;
    v[E_FOCUS] = s.focus_s > 0 ? (s.focus_s + 59) / 60 : 0;
    v[E_VOLUME] = c.volume;
    v[E_SCREEN_BRIGHT] = c.screen_bright;
    v[E_BGM] = c.bgm;
    v[E_QUIET] = c.quiet;
}

static bool publish_values(const values_t *val)
{
    char topic[56], js[400];
    int n = 0;
    for (int i = 0; i < ENT_COUNT; i++) {
        if (ENT[i].kind == V_NONE) continue;
        const int v = val->v[i];
        n += snprintf(js + n, sizeof js - n, "%c\"%s\":", n ? ',' : '{', ENT[i].key);
        if (v == V_NULL) n += snprintf(js + n, sizeof js - n, "null");
        else if (ENT[i].kind == V_ONOFF) n += snprintf(js + n, sizeof js - n, "\"%s\"", v ? "ON" : "OFF");
        else if (ENT[i].kind == V_CLAUDE) n += snprintf(js + n, sizeof js - n, "\"%s\"", CLAUDE_STATES[v]);
        else n += snprintf(js + n, sizeof js - n, "%d", v);
    }
    snprintf(js + n, sizeof js - n, "}");
    snprintf(topic, sizeof topic, "%s/state", s_base);
    return publish(topic, js);
}

static void client_stop(void)
{
    if (!s_client) return;
    if (atomic_load(&s_connected)) { /* leaving on purpose: the last will is not sent, so say it ourselves */
        char topic[56];
        snprintf(topic, sizeof topic, "%s/status", s_base);
        publish(topic, "offline");
    }
    esp_mqtt_client_stop(s_client);
    esp_mqtt_client_destroy(s_client);
    s_client = NULL;
    atomic_store(&s_connected, false);
}

static void client_start(const settings_t *set)
{
    static char will[56], client_id[40];
    snprintf(s_base, sizeof s_base, "slime/%s", set->id);
    snprintf(s_set_prefix, sizeof s_set_prefix, "slime/%s/set/", set->id);
    snprintf(will, sizeof will, "%s/status", s_base);
    snprintf(client_id, sizeof client_id, "slime-%s", set->id);
    const esp_mqtt_client_config_t cfg = {
        .broker.address.uri = set->url,
        .broker.verification.crt_bundle_attach = esp_crt_bundle_attach, /* mqtts://: certificates from public authorities */
        .credentials.username = set->user[0] ? set->user : NULL,
        .credentials.client_id = client_id,
        .credentials.authentication.password = set->user[0] && set->pass[0] ? set->pass : NULL,
        .session.last_will = {.topic = will, .msg = "offline", .qos = 0, .retain = 1},
        .session.keepalive = 60,
        .network.reconnect_timeout_ms = 15000,
        .task = {.priority = 3, .stack_size = HA_STACK}, /* below the web server and the renderer */
        .buffer = {.size = 1024, .out_size = 1024},
    };
    s_client = esp_mqtt_client_init(&cfg); /* copies every string */
    if (!s_client || esp_mqtt_client_register_event(s_client, MQTT_EVENT_ANY, on_event, NULL) != ESP_OK ||
        esp_mqtt_client_start(s_client) != ESP_OK) {
        ESP_LOGW(TAG, "could not start the client (is the address right?)");
        if (s_client) esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        return;
    }
    ESP_LOGI(TAG, "connecting to %s as \"%s\"", set->url, set->id); /* the address never carries a password: see ai_set() */
}

static void ha_task(void *arg)
{
    static settings_t set; /* static: the password does not sit on the stack */
    values_t sent = {0};
    bool have_sent = false, failed = false;
    int64_t sent_at = 0;
    sl_lang_t lang = sl_lang;
    atomic_store(&s_reload, true);
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(HA_TICK_MS));
        if (atomic_exchange(&s_reload, false)) {
            client_stop();
            portENTER_CRITICAL(&s_lock);
            set = s_set;
            portEXIT_CRITICAL(&s_lock);
            failed = false;
            atomic_store(&s_refused, false);
            atomic_store(&s_status, set.url[0] ? 1 : 0);
            if (!set.url[0]) ESP_LOGI(TAG, "off: no broker set");
        }
        const int64_t now = esp_timer_get_time();
        if (!s_client) {
            net_status_t ns;
            net_get(&ns);
            /* a client that would not even start (a malformed address) is not retried until the settings change */
            if (set.url[0] && !failed && ns.state == NET_CONNECTED) {
                client_start(&set);
                failed = !s_client;
            }
            continue;
        }
        if (!atomic_load(&s_connected)) continue;
        if (lang != sl_lang) { /* the entity names follow the pet's language */
            lang = sl_lang;
            atomic_store(&s_announce, true);
        }
        if (atomic_exchange(&s_announce, false)) {
            if (!announce(&set)) {
                atomic_store(&s_announce, true);
                continue;
            }
            have_sent = false; /* say it all again */
        }
        if (!atomic_load(&s_have_state)) continue; /* nothing to say about the pet before its first frame */
        values_t cur;
        current_values(&cur);
        if (have_sent && !memcmp(&cur, &sent, sizeof cur)) continue;
        bool fast = !have_sent;
        for (int i = 0; i < ENT_COUNT && !fast; i++) fast = !ENT[i].slow && cur.v[i] != sent.v[i];
        if ((fast || now - sent_at >= (int64_t)HA_SLOW_S * 1000000) && publish_values(&cur)) {
            sent = cur;
            sent_at = now;
            have_sent = true;
        }
    }
}

/* ---- the rest of the firmware ---- */

static void ensure_task(void)
{
    if (atomic_exchange(&s_task, true)) return;
    /* core 0: core 1 belongs to the renderer. The stack is PSRAM: internal RAM is the scarce one. */
    if (xTaskCreatePinnedToCoreWithCaps(ha_task, "ha", HA_STACK, NULL, 2, NULL, 0, MALLOC_CAP_SPIRAM) != pdPASS) {
        atomic_store(&s_task, false);
        ESP_LOGW(TAG, "could not start");
    }
}

void ha_start(void) { ha_reload(); }

void ha_reload(void)
{
    static settings_t next; /* one caller at a time: boot, then the web server task */
    memset(&next, 0, sizeof next); /* compared byte by byte below */
    read_settings(&next);
    const bool on = next.url[0];
    portENTER_CRITICAL(&s_lock);
    const bool same = !memcmp(&next, &s_set, sizeof next);
    if (!same) s_set = next;
    portEXIT_CRITICAL(&s_lock);
    memset(&next, 0, sizeof next);
    if (same) return;
    if (!atomic_load(&s_task) && !on) return; /* never switched on: no task, no client */
    atomic_store(&s_status, on ? 1 : 0);
    atomic_store(&s_reload, true);
    ensure_task();
}

void ha_update(const ha_state_t *st)
{
    portENTER_CRITICAL(&s_lock);
    s_state = *st;
    portEXIT_CRITICAL(&s_lock);
    atomic_store(&s_have_state, true);
}

const char *ha_status(void)
{
    static const char *const N[] = {"off", "connecting", "connected"};
    const int st = atomic_load(&s_status);
    return st == 1 && atomic_load(&s_refused) ? "refused" : N[st];
}
