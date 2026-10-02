#pragma once

#include <stdbool.h>
#include <time.h>

#include <stdint.h>

#include "esp_err.h"

/* Wi-Fi station + mDNS ("slime.local"). Credentials live in the Wi-Fi driver's NVS storage and
 * are only ever set by the user (bridge/wifi_setup.py over USB, or the web panel). */
typedef enum { NET_OFF = 0, NET_NO_CONFIG, NET_CONNECTING, NET_CONNECTED } net_state_t;

typedef struct {
    net_state_t state;
    char ssid[33];
    char ip[16];
    int rssi;
    int fails;       /* consecutive failed attempts */
    int last_reason; /* wifi_err_reason_t of the last disconnect */
} net_status_t;

esp_err_t net_start(void);
/* Stores the credentials and reconnects. Never logs the passphrase. */
esp_err_t net_set_wifi(const char *ssid, const char *psk);
void net_get(net_status_t *out);
const char *net_state_name(net_state_t s);

/* Local (China) time once SNTP has synced; false before that. */
bool net_local_time(struct tm *out);
