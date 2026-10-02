#include "net.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "mdns.h"

static const char *TAG = "net";

#define HOSTNAME "slime"

bool net_local_time(struct tm *out)
{
    static bool tz_set;
    if (!tz_set) {
        tz_set = true;
        setenv("TZ", "CST-8", 1);
        tzset();
    }
    const time_t now = time(NULL);
    if (now < 1735689600) return false; /* not synced yet (before 2025) */
    localtime_r(&now, out);
    return true;
}

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static net_status_t s_st;
static esp_netif_t *s_netif;
static esp_timer_handle_t s_retry;
static bool s_started;

static void set_state(net_state_t st)
{
    portENTER_CRITICAL(&s_lock);
    s_st.state = st;
    if (st != NET_CONNECTED) s_st.ip[0] = 0;
    portEXIT_CRITICAL(&s_lock);
}

static void retry_cb(void *arg) { esp_wifi_connect(); }

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_st.state == NET_CONNECTING) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        portENTER_CRITICAL(&s_lock);
        s_st.fails++;
        s_st.last_reason = d->reason;
        const int fails = s_st.fails;
        portEXIT_CRITICAL(&s_lock);
        if (s_st.state == NET_NO_CONFIG) return;
        set_state(NET_CONNECTING);
        /* back off 1, 2, 4 ... 30 s */
        const int shift = fails < 6 ? fails - 1 : 5;
        int64_t us = (int64_t)1000000 << (shift > 0 ? shift : 0);
        if (us > 30000000) us = 30000000;
        esp_timer_stop(s_retry);
        esp_timer_start_once(s_retry, us);
        ESP_LOGW(TAG, "disconnected (reason %d), retry in %d s", d->reason, (int)(us / 1000000));
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        portENTER_CRITICAL(&s_lock);
        s_st.state = NET_CONNECTED;
        s_st.fails = 0;
        esp_ip4addr_ntoa(&e->ip_info.ip, s_st.ip, sizeof s_st.ip);
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGI(TAG, "connected to \"%s\", ip %s, http://%s.local/", s_st.ssid, s_st.ip, HOSTNAME);
        static bool sntp_started;
        if (!sntp_started) { /* clock for the night mode and greetings; mainland servers */
            sntp_started = true;
            esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
            sc.wait_for_sync = false;
            if (esp_netif_sntp_init(&sc) != ESP_OK) ESP_LOGW(TAG, "sntp init failed");
        }
    }
}

esp_err_t net_start(void)
{
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
    const esp_err_t le = esp_event_loop_create_default();
    ESP_RETURN_ON_FALSE(le == ESP_OK || le == ESP_ERR_INVALID_STATE, le, TAG, "event loop");
    s_netif = esp_netif_create_default_wifi_sta();
    ESP_RETURN_ON_FALSE(s_netif, ESP_FAIL, TAG, "sta netif");
    esp_netif_set_hostname(s_netif, HOSTNAME);

    const wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&wcfg), TAG, "wifi init");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_FLASH), TAG, "storage");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL), TAG, "wifi ev");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL), TAG, "ip ev");
    const esp_timer_create_args_t ta = {.callback = retry_cb, .name = "wifi_retry"};
    ESP_RETURN_ON_ERROR(esp_timer_create(&ta, &s_retry), TAG, "timer");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "mode");

    wifi_config_t cfg = {0};
    esp_wifi_get_config(WIFI_IF_STA, &cfg);
    strlcpy(s_st.ssid, (const char *)cfg.sta.ssid, sizeof s_st.ssid);
    set_state(s_st.ssid[0] ? NET_CONNECTING : NET_NO_CONFIG);
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start");
    s_started = true;

    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(HOSTNAME);
        mdns_instance_name_set("Slime Pet");
        mdns_service_add("Slime Pet", "_http", "_tcp", 80, NULL, 0);
    } else {
        ESP_LOGW(TAG, "mDNS unavailable; use the IP address");
    }
    if (s_st.ssid[0]) {
        ESP_LOGI(TAG, "Wi-Fi: joining \"%s\"", s_st.ssid);
    } else {
        ESP_LOGI(TAG, "Wi-Fi: not configured (run bridge/wifi_setup.py)");
    }
    return ESP_OK;
}

esp_err_t net_set_wifi(const char *ssid, const char *psk)
{
    ESP_RETURN_ON_FALSE(s_started, ESP_ERR_INVALID_STATE, TAG, "wifi not started");
    ESP_RETURN_ON_FALSE(ssid && ssid[0] && strlen(ssid) < 33 && psk && strlen(psk) < 64, ESP_ERR_INVALID_ARG, TAG,
                        "bad credentials");
    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, ssid, sizeof cfg.sta.ssid);
    strlcpy((char *)cfg.sta.password, psk, sizeof cfg.sta.password);
    cfg.sta.threshold.authmode = psk[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    cfg.sta.pmf_cfg.capable = true;
    esp_timer_stop(s_retry);
    esp_wifi_disconnect();
    portENTER_CRITICAL(&s_lock);
    strlcpy(s_st.ssid, ssid, sizeof s_st.ssid);
    s_st.fails = 0;
    s_st.last_reason = 0;
    portEXIT_CRITICAL(&s_lock);
    set_state(NET_CONNECTING);
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &cfg), TAG, "set config");
    ESP_LOGI(TAG, "Wi-Fi credentials updated for \"%s\"", ssid);
    return esp_wifi_connect();
}

void net_get(net_status_t *out)
{
    portENTER_CRITICAL(&s_lock);
    *out = s_st;
    portEXIT_CRITICAL(&s_lock);
    wifi_ap_record_t ap;
    out->rssi = (out->state == NET_CONNECTED && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : 0;
}

const char *net_state_name(net_state_t s)
{
    static const char *N[] = {"off", "no_config", "connecting", "connected"};
    return s <= NET_CONNECTED ? N[s] : "?";
}
