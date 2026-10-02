#pragma once

#include "esp_err.h"

/*
 * HTTP server on port 80 (LAN only, no authentication):
 *   GET  /             settings panel (web/index.html)
 *   GET  /api/status   JSON published by the main loop (web_publish_status)
 *   GET  /api/config   current settings
 *   POST /api/config   JSON subset of settings to change
 *   POST /api/cmd      text lines for the inbox ("cc ...", "state think"); the Claude Code hook uses this
 *   POST /api/wifi     {"ssid": "...", "psk": "..."}; applied after the response is sent
 */
esp_err_t web_start(void);
/* Copies a ready-made JSON document for /api/status. */
void web_publish_status(const char *json);
