#include "ota.h"

#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"

static const char *TAG = "ota";

#define CHUNK 4096
#define HEAD_BYTES (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t))
#define RECV_RETRIES 20 /* x the server's 5 s receive timeout */
#define WRITE_PAUSE_MS 10 /* after each 4 KB chunk: about 15 s more for a 6 MB image */

static char s_token[17];
static volatile int s_progress = -1;

void ota_init(void)
{
    nvs_handle_t h;
    size_t len = sizeof s_token;
    if (nvs_open("slime", NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_get_str(h, "ota_tok", s_token, &len) != ESP_OK || strlen(s_token) != 16) {
        uint8_t r[8];
        esp_fill_random(r, sizeof r);
        for (int i = 0; i < 8; i++) snprintf(s_token + i * 2, 3, "%02x", r[i]);
        if (nvs_set_str(h, "ota_tok", s_token) == ESP_OK) nvs_commit(h);
        ESP_LOGI(TAG, "new update token created");
    }
    nvs_close(h);
}

const char *ota_token(void) { return s_token; }

int ota_progress(void) { return s_progress; }

static void restart_cb(void *arg)
{
    (void)arg;
    esp_restart();
}

static esp_err_t fail(httpd_req_t *req, esp_ota_handle_t h, const char *why)
{
    if (h) esp_ota_abort(h);
    s_progress = -1;
    ESP_LOGW(TAG, "update rejected: %s", why);
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, why);
}

esp_err_t ota_http_handler(httpd_req_t *req)
{
    char tok[24];
    if (!s_token[0] || httpd_req_get_hdr_value_str(req, "X-OTA-Token", tok, sizeof tok) != ESP_OK || strcmp(tok, s_token)) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "bad or missing X-OTA-Token");
    }
    if (s_progress >= 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "an update is already running");
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    const size_t total = req->content_len;
    if (!part) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no update slot (partition table without OTA)");
    if (total < HEAD_BYTES || total > part->size) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad image size");

    uint8_t *buf = heap_caps_malloc(CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    s_progress = 0;
    ESP_LOGI(TAG, "receiving %u bytes into %s", (unsigned)total, part->label);

    esp_ota_handle_t h = 0;
    size_t got = 0;
    bool checked = false;
    int retries = 0;
    esp_err_t e = ESP_OK;
    const char *why = NULL;
    while (got < total) {
        /* the first chunk must hold the app description, so it can be checked before writing */
        size_t fill = 0;
        const size_t want = total - got < CHUNK ? total - got : CHUNK;
        while (fill < want) {
            const int r = httpd_req_recv(req, (char *)buf + fill, want - fill);
            if (r == HTTPD_SOCK_ERR_TIMEOUT && ++retries < RECV_RETRIES) continue;
            if (r <= 0) {
                why = "connection lost";
                break;
            }
            fill += r;
            if (checked || fill >= HEAD_BYTES) break;
        }
        if (why) break;
        if (!checked) {
            const esp_app_desc_t *d = (const esp_app_desc_t *)(buf + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t));
            if (fill < HEAD_BYTES || buf[0] != ESP_IMAGE_HEADER_MAGIC || d->magic_word != ESP_APP_DESC_MAGIC_WORD) {
                why = "not an app image";
                break;
            }
            if (strncmp(d->project_name, esp_app_get_description()->project_name, sizeof d->project_name)) {
                why = "image is for another project";
                break;
            }
            ESP_LOGI(TAG, "image: %s %s built %s %s", d->project_name, d->version, d->date, d->time);
            /* sequential: erases each sector as it is reached instead of 5 MB up front */
            if ((e = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &h)) != ESP_OK) {
                why = "could not start writing";
                break;
            }
            checked = true;
        }
        if ((e = esp_ota_write(h, buf, fill)) != ESP_OK) {
            why = "flash write failed";
            break;
        }
        got += fill;
        s_progress = (int)(got * 100 / total);
        /* Flash writes busy-wait in this task (priority 5, core 0), above the main loop and the
         * audio task on the same core: without a pause they are starved for the whole update, a
         * frame takes seconds and the task watchdog resets the device mid-write. */
        vTaskDelay(pdMS_TO_TICKS(WRITE_PAUSE_MS));
    }
    heap_caps_free(buf);
    if (why) return fail(req, h, why);
    if ((e = esp_ota_end(h)) != ESP_OK) { /* checks segments and the SHA-256 */
        h = 0;
        return fail(req, 0, e == ESP_ERR_OTA_VALIDATE_FAILED ? "image failed verification" : "could not finish");
    }
    if (esp_ota_set_boot_partition(part) != ESP_OK) return fail(req, 0, "could not switch to the new image");

    ESP_LOGI(TAG, "update written to %s, restarting", part->label);
    char js[96];
    snprintf(js, sizeof js, "{\"ok\":true,\"part\":\"%s\",\"bytes\":%u}", part->label, (unsigned)total);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, js);
    s_progress = 100;
    static esp_timer_handle_t t;
    const esp_timer_create_args_t ta = {.callback = restart_cb, .name = "ota_restart"};
    if (!t) esp_timer_create(&ta, &t);
    esp_timer_start_once(t, 1500 * 1000); /* let the response go out first */
    return ESP_OK;
}

void ota_confirm_if_pending(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) ESP_LOGI(TAG, "new firmware confirmed in %s", run->label);
    }
}

void ota_status_json(char *buf, size_t len)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_app_desc_t *d = esp_app_get_description();
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    if (run) esp_ota_get_state_partition(run, &st);
    const char *sn = st == ESP_OTA_IMG_VALID ? "valid"
                   : st == ESP_OTA_IMG_PENDING_VERIFY ? "pending"
                   : st == ESP_OTA_IMG_NEW ? "new"
                   : st == ESP_OTA_IMG_UNDEFINED ? "undefined" : "other";
    snprintf(buf, len, "{\"part\":\"%s\",\"state\":\"%s\",\"ver\":\"%.31s\",\"built\":\"%s %s\",\"progress\":%d}",
             run ? run->label : "?", sn, d->version, d->date, d->time, s_progress);
}
