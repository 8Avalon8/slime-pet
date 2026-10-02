/*
 * USB link over the Type-C OTG port: TinyUSB CDC-ACM text console, plus a
 * magic string that reboots the chip into ROM download mode, so flashing no
 * longer needs the BOOT button.
 *
 * Complete text lines (e.g. "cc ..." from the Claude Code hook) go to the shared inbox.
 *
 * Only the explicit magic string triggers a reboot. DTR/RTS are ignored on
 * purpose: terminals toggle them on open/close in host-specific orders, and an
 * accidental reboot every time a monitor closes would be worse than useless.
 */
#include "usb_link.h"

#include "inbox.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "soc/lp_system_reg.h"
#include "soc/soc.h"
#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_console.h"
#include "tinyusb_default_config.h"

static const char *TAG = "usb_link";

static volatile bool s_dl_requested;
static char s_tail[sizeof(USB_LINK_DL_MAGIC)];
static SemaphoreHandle_t s_rx_mutex;
static bool s_up;
static char s_line[INBOX_LINE_MAX];
static size_t s_line_len;
static bool s_line_overflow;
static uint32_t s_lines_dropped;

static void on_byte(char ch)
{
    if (ch == '\r') return;
    if (ch != '\n') {
        if (s_line_len < sizeof s_line - 1) {
            s_line[s_line_len++] = ch;
        } else {
            s_line_overflow = true;
        }
        return;
    }
    s_line[s_line_len] = 0;
    if (s_line_len && !s_line_overflow && !inbox_push(s_line, SRC_USB)) {
        s_lines_dropped++;
    }
    s_line_len = 0;
    s_line_overflow = false;
}

static void scan(const uint8_t *buf, size_t n)
{
    const size_t m = sizeof(s_tail) - 1;
    for (size_t i = 0; i < n; i++) {
        memmove(s_tail, s_tail + 1, m - 1);
        s_tail[m - 1] = (char)buf[i];
        s_tail[m] = 0;
        if (!memcmp(s_tail, USB_LINK_DL_MAGIC, m)) {
            s_dl_requested = true;
        }
        on_byte((char)buf[i]);
    }
}

/* Always drain to empty: on this high-speed port one transfer can carry up to 512 bytes, and
 * leftovers accumulate until TinyUSB stops accepting OUT data for good (that also kills the
 * download-mode magic). Called from the RX callback and, as a safety net, every frame. */
static void drain(int itf)
{
    if (xSemaphoreTake(s_rx_mutex, 0) != pdTRUE) return; /* the other context is on it */
    uint8_t buf[64];
    size_t n = 0;
    while (tinyusb_cdcacm_read(itf, buf, sizeof buf, &n) == ESP_OK && n > 0) {
        scan(buf, n);
    }
    xSemaphoreGive(s_rx_mutex);
}

static void on_rx(int itf, cdcacm_event_t *event) { drain(itf); }

esp_err_t usb_link_init(void)
{
    s_rx_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(inbox_init() && s_rx_mutex, ESP_ERR_NO_MEM, TAG, "line queue");
    const tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    ESP_RETURN_ON_ERROR(tinyusb_driver_install(&tusb_cfg), TAG, "tinyusb driver");
    const tinyusb_config_cdcacm_t acm_cfg = {
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_rx = on_rx,
    };
    ESP_RETURN_ON_ERROR(tinyusb_cdcacm_init(&acm_cfg), TAG, "cdc acm");
    ESP_RETURN_ON_ERROR(tinyusb_console_init(TINYUSB_CDC_ACM_0), TAG, "console");
    s_up = true;
    ESP_LOGI(TAG, "USB CDC console up; send \"%s\" to reboot into download mode", USB_LINK_DL_MAGIC);
    return ESP_OK;
}

void usb_link_poll(void)
{
    if (s_up) drain(TINYUSB_CDC_ACM_0);
    if (!s_dl_requested) {
        return;
    }
    ESP_LOGW(TAG, "rebooting into ROM download mode");
    vTaskDelay(pdMS_TO_TICKS(100)); /* let the log line reach the host */
    REG_SET_BIT(LP_SYSTEM_REG_SYS_CTRL_REG, LP_SYSTEM_REG_FORCE_DOWNLOAD_BOOT);
    esp_restart();
}

