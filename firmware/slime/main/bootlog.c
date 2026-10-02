#include "bootlog.h"

#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_core_dump.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs.h"

static const char *TAG = "bootlog";
static char s_crash[200] = "null";

#define MAGIC 0x511AE5u
#define RING 8

typedef struct {
    uint32_t magic;
    uint32_t uptime_s;
    int16_t mv, ma;
    uint8_t soc;
} snap_t;

typedef struct {
    uint8_t reason; /* esp_reset_reason_t */
    uint8_t has_prev;
    uint8_t soc;
    uint32_t prev_up;
    int16_t mv, ma;
} entry_t;

static RTC_NOINIT_ATTR snap_t s_snap;
static entry_t s_ring[RING];
static int s_count;

static const char *reason_name(int r)
{
    switch (r) {
    case ESP_RST_POWERON: return "power_on";
    case ESP_RST_EXT: return "external";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "int_wdt";
    case ESP_RST_TASK_WDT: return "task_wdt";
    case ESP_RST_WDT: return "wdt";
    case ESP_RST_DEEPSLEEP: return "deep_sleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "sdio";
    case ESP_RST_USB: return "usb";
    case ESP_RST_JTAG: return "jtag";
    default: return "unknown";
    }
}

static void crash_check(void)
{
    if (esp_core_dump_image_check() != ESP_OK) return; /* nothing recorded */
    esp_core_dump_summary_t *sm = heap_caps_calloc(1, sizeof *sm, MALLOC_CAP_SPIRAM);
    if (!sm) return;
    if (esp_core_dump_get_summary(sm) == ESP_OK) {
        snprintf(s_crash, sizeof s_crash,
                 "{\"task\":\"%.15s\",\"pc\":\"0x%08lx\",\"ra\":\"0x%08lx\",\"mcause\":%lu,\"mtval\":\"0x%08lx\"}",
                 sm->exc_task, (unsigned long)sm->exc_pc, (unsigned long)sm->ex_info.ra, (unsigned long)sm->ex_info.mcause,
                 (unsigned long)sm->ex_info.mtval);
        ESP_LOGW(TAG, "last crash: %s", s_crash);
    }
    heap_caps_free(sm);
}

void bootlog_crash_json(char *buf, size_t len) { snprintf(buf, len, "%s", s_crash); }

void bootlog_init(void)
{
    crash_check();
    entry_t e = {.reason = (uint8_t)esp_reset_reason()};
    if (s_snap.magic == MAGIC) {
        e.has_prev = 1;
        e.prev_up = s_snap.uptime_s;
        e.mv = s_snap.mv;
        e.ma = s_snap.ma;
        e.soc = s_snap.soc;
    }
    memset(&s_snap, 0, sizeof s_snap);

    nvs_handle_t h;
    if (nvs_open("slime", NVS_READWRITE, &h) == ESP_OK) {
        size_t len = sizeof s_ring;
        if (nvs_get_blob(h, "boots", s_ring, &len) != ESP_OK || len != sizeof s_ring) memset(s_ring, 0, sizeof s_ring);
        memmove(&s_ring[1], &s_ring[0], sizeof(entry_t) * (RING - 1));
        s_ring[0] = e;
        nvs_set_blob(h, "boots", s_ring, sizeof s_ring);
        nvs_commit(h);
        nvs_close(h);
    } else {
        s_ring[0] = e;
    }
    for (s_count = 0; s_count < RING && (s_ring[s_count].reason || s_ring[s_count].has_prev); s_count++) {
    }
    if (e.has_prev) {
        ESP_LOGW(TAG, "reset: %s; previous session ran %u s, last seen %d mV %d mA SOC %u%%", reason_name(e.reason),
                 (unsigned)e.prev_up, e.mv, e.ma, e.soc);
    } else {
        ESP_LOGI(TAG, "reset: %s (no snapshot: cold power-up)", reason_name(e.reason));
    }
}

void bootlog_snapshot(uint32_t uptime_s, int voltage_mv, int current_ma, int soc)
{
    s_snap.uptime_s = uptime_s;
    s_snap.mv = (int16_t)voltage_mv;
    s_snap.ma = (int16_t)current_ma;
    s_snap.soc = (uint8_t)soc;
    s_snap.magic = MAGIC;
}

void bootlog_json(char *buf, size_t len)
{
    size_t n = (size_t)snprintf(buf, len, "[");
    for (int i = 0; i < s_count && n < len; i++) {
        const entry_t *e = &s_ring[i];
        if (e->has_prev) {
            n += snprintf(buf + n, len - n, "%s{\"reason\":\"%s\",\"prev_up\":%u,\"mv\":%d,\"ma\":%d,\"soc\":%u}", i ? "," : "",
                          reason_name(e->reason), (unsigned)e->prev_up, e->mv, e->ma, e->soc);
        } else {
            n += snprintf(buf + n, len - n, "%s{\"reason\":\"%s\"}", i ? "," : "", reason_name(e->reason));
        }
    }
    if (n < len) snprintf(buf + n, len - n, "]");
}
