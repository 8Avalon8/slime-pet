#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "esp_http_server.h"

/*
 * Wireless firmware update. POST /api/ota with the raw app image (build/slime_pet.bin) and the
 * header X-OTA-Token. The token is random, kept in NVS, and handed out only over USB ("otatoken"),
 * so updating needs one physical connection first. The image goes to the idle app slot; the
 * running one is untouched until the new one has booted and confirmed itself (rollback).
 */
void ota_init(void);
const char *ota_token(void);
esp_err_t ota_http_handler(httpd_req_t *req);
/* Call once the app has run healthily for a while: a freshly updated image confirms itself,
 * otherwise the bootloader would roll back to the previous one on the next reset. */
void ota_confirm_if_pending(void);
/* Progress while an update is being received: -1 when idle, else 0..100. */
int ota_progress(void);
/* {"part":"ota_0","state":"valid","ver":"...","built":"..."} */
void ota_status_json(char *buf, size_t len);
