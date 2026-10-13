/*
 * The slice of ESP-IDF, FreeRTOS, the board support package, iot_button and esp_http_server
 * that firmware/slime/main uses, re-implemented on POSIX threads so the real main.c, web.c,
 * config.c, inbox.c and settings_ui.c compile and run unchanged on a computer (see host/README.md).
 *
 * Only what main/ calls is here. When the firmware starts using something new from IDF, the
 * simulator build fails on it; add the declaration below and its stand-in to sim_idf.c.
 */
#pragma once
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- esp_err / esp_check / esp_log ---------------- */
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_NOT_SUPPORTED 0x106
#define ESP_ERR_TIMEOUT 0x107
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define ESP_ERR_NVS_NO_FREE_PAGES 0x110d
#define ESP_ERR_NVS_NEW_VERSION_FOUND 0x1110
const char *esp_err_to_name(esp_err_t e);

#define IRAM_ATTR

void sim_log(char level, const char *tag, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
#define ESP_LOGE(tag, ...) sim_log('E', tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) sim_log('W', tag, __VA_ARGS__)
#define ESP_LOGI(tag, ...) sim_log('I', tag, __VA_ARGS__)
#define ESP_LOGD(tag, ...) ((void)0)
#define ESP_LOGV(tag, ...) ((void)0)

#define ESP_RETURN_ON_ERROR(x, tag, ...)                                                                               \
    do {                                                                                                               \
        const esp_err_t err_rc_ = (x);                                                                                 \
        if (err_rc_ != ESP_OK) {                                                                                       \
            ESP_LOGE(tag, __VA_ARGS__);                                                                                \
            return err_rc_;                                                                                            \
        }                                                                                                              \
    } while (0)
#define ESP_RETURN_ON_FALSE(a, err_code, tag, ...)                                                                     \
    do {                                                                                                               \
        if (!(a)) {                                                                                                    \
            ESP_LOGE(tag, __VA_ARGS__);                                                                                \
            return err_code;                                                                                           \
        }                                                                                                              \
    } while (0)

/* ---------------- heap, cache, random, watchdog, timer ---------------- */
#define MALLOC_CAP_SPIRAM (1 << 10)
#define MALLOC_CAP_INTERNAL (1 << 11)
#define MALLOC_CAP_DMA (1 << 3)
#define MALLOC_CAP_8BIT (1 << 2)
void *heap_caps_malloc(size_t size, uint32_t caps);
void *heap_caps_calloc(size_t n, size_t size, uint32_t caps);
void *heap_caps_aligned_calloc(size_t align, size_t n, size_t size, uint32_t caps);
size_t heap_caps_get_free_size(uint32_t caps);

#define ESP_CACHE_MSYNC_FLAG_DIR_C2M (1 << 0)
#define ESP_CACHE_MSYNC_FLAG_UNALIGNED (1 << 2)
static inline esp_err_t esp_cache_msync(void *addr, size_t size, int flags) { return ESP_OK; }

uint32_t esp_random(void);
static inline esp_err_t esp_task_wdt_add(void *task) { return ESP_OK; }
static inline esp_err_t esp_task_wdt_reset(void) { return ESP_OK; }

int64_t esp_timer_get_time(void); /* microseconds since the simulator started */
typedef struct sim_timer *esp_timer_handle_t;
typedef void (*esp_timer_cb_t)(void *arg);
typedef struct {
    esp_timer_cb_t callback;
    void *arg;
    int dispatch_method;
    const char *name;
    bool skip_unhandled_events;
} esp_timer_create_args_t;
esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *out);
esp_err_t esp_timer_start_once(esp_timer_handle_t t, uint64_t timeout_us);
esp_err_t esp_timer_stop(esp_timer_handle_t t);

/* ---------------- FreeRTOS (1 tick = 1 ms) ---------------- */
typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef uint32_t TickType_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define pdFAIL 0
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define portTICK_PERIOD_MS 1

typedef pthread_mutex_t portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
#define portENTER_CRITICAL(m) pthread_mutex_lock(m)
#define portEXIT_CRITICAL(m) pthread_mutex_unlock(m)

typedef struct sim_sem *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateBinary(void);
SemaphoreHandle_t xSemaphoreCreateMutex(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t ticks);
BaseType_t xSemaphoreGive(SemaphoreHandle_t s);
BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t s, BaseType_t *woken);

typedef struct sim_queue *QueueHandle_t;
QueueHandle_t xQueueCreate(UBaseType_t len, UBaseType_t item_size);
BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t ticks);
BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t ticks);

typedef void (*TaskFunction_t)(void *);
typedef void *TaskHandle_t;
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, uint32_t stack, void *arg, UBaseType_t prio,
                                   TaskHandle_t *out, BaseType_t core);
#define xTaskCreate(fn, name, stack, arg, prio, out) xTaskCreatePinnedToCore(fn, name, stack, arg, prio, out, -1)
#define xTaskCreatePinnedToCoreWithCaps(fn, name, stack, arg, prio, out, core, caps) xTaskCreatePinnedToCore(fn, name, stack, arg, prio, out, core)
void vTaskDelay(TickType_t ticks);
void vTaskDelete(TaskHandle_t t);

/* ---------------- NVS (in memory; --nvs <file> keeps it between runs) ---------------- */
typedef uint32_t nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;
esp_err_t nvs_flash_init(void);
esp_err_t nvs_flash_erase(void);
esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out);
void nvs_close(nvs_handle_t h);
esp_err_t nvs_commit(nvs_handle_t h);
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len);
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *val, size_t len);
esp_err_t nvs_get_i32(nvs_handle_t h, const char *key, int32_t *out);
esp_err_t nvs_set_i32(nvs_handle_t h, const char *key, int32_t val);
esp_err_t nvs_get_u8(nvs_handle_t h, const char *key, uint8_t *out);
esp_err_t nvs_set_u8(nvs_handle_t h, const char *key, uint8_t val);
esp_err_t nvs_get_u32(nvs_handle_t h, const char *key, uint32_t *out);
esp_err_t nvs_set_u32(nvs_handle_t h, const char *key, uint32_t val);
esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *len);
esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *val);
esp_err_t nvs_erase_key(nvs_handle_t h, const char *key);

/* ---------------- LCD panel + touch (drawn into the simulator window) ---------------- */
typedef struct sim_panel *esp_lcd_panel_handle_t;
typedef struct sim_panel_io *esp_lcd_panel_io_handle_t;
typedef struct {
    void *unused;
} esp_lcd_panel_io_event_data_t;
typedef bool (*esp_lcd_panel_io_color_trans_done_cb_t)(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *e,
                                                        void *ctx);
typedef struct {
    esp_lcd_panel_io_color_trans_done_cb_t on_color_trans_done;
} esp_lcd_panel_io_callbacks_t;
esp_err_t esp_lcd_panel_io_register_event_callbacks(esp_lcd_panel_io_handle_t io, const esp_lcd_panel_io_callbacks_t *cbs,
                                                    void *ctx);
esp_err_t esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t p, int x0, int y0, int x1, int y1, const void *data);

typedef struct sim_touch *esp_lcd_touch_handle_t;
esp_err_t esp_lcd_touch_read_data(esp_lcd_touch_handle_t tp);
bool esp_lcd_touch_get_coordinates(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y, uint16_t *strength,
                                   uint8_t *point_num, uint8_t max_point_num);

/* ---------------- iot_button ---------------- */
typedef struct sim_button *button_handle_t;
typedef enum {
    BUTTON_PRESS_DOWN = 0,
    BUTTON_PRESS_UP,
    BUTTON_PRESS_REPEAT,
    BUTTON_PRESS_REPEAT_DONE,
    BUTTON_SINGLE_CLICK,
    BUTTON_DOUBLE_CLICK,
    BUTTON_MULTIPLE_CLICK,
    BUTTON_LONG_PRESS_START,
    BUTTON_LONG_PRESS_HOLD,
    BUTTON_LONG_PRESS_UP,
    BUTTON_EVENT_MAX,
} button_event_t;
typedef void (*button_cb_t)(void *button_handle, void *usr_data);
typedef struct {
    int unused;
} button_event_args_t;
typedef struct {
    uint16_t long_press_time;
    uint16_t short_press_time;
} button_config_t;
typedef struct {
    int32_t gpio_num;
    uint8_t active_level;
    bool enable_power_save;
    bool disable_pull;
} button_gpio_config_t;
esp_err_t iot_button_register_cb(button_handle_t b, button_event_t ev, button_event_args_t *args, button_cb_t cb, void *usr);
esp_err_t iot_button_new_gpio_device(const button_config_t *bc, const button_gpio_config_t *gc, button_handle_t *out);

/* ---------------- board support package (bsp/esp_mosaico.h) ---------------- */
#define CONFIG_BSP_DISPLAY_LVGL_ENABLE 0
typedef struct {
    bool enable_touch;
} bsp_display_config_t;
#define BSP_DISPLAY_DEFAULT_CONFIG() ((bsp_display_config_t){.enable_touch = true})
#define BSP_LCD_ROTATION_DEFAULT 0
enum { BSP_BUTTON_AI = 0, BSP_BUTTON_NUM };
#define BSP_BUTTON_BOOT_GPIO 0
#define BSP_BUTTON_ACTIVE_LEVEL 0
typedef enum { BSP_BOARD_VARIANT_V1_0 = 0, BSP_BOARD_VARIANT_V1_1 } bsp_board_variant_t;
typedef struct {
    int voltage_mv;
    int current_ma; /* + = charging */
    int state_of_charge;
} bsp_battery_status_t;
esp_err_t bsp_power_init(void);
esp_err_t bsp_display_new(const bsp_display_config_t *cfg, esp_lcd_panel_handle_t *out);
esp_lcd_panel_io_handle_t bsp_display_get_panel_io(void);
esp_err_t bsp_display_on(void);
esp_err_t bsp_display_brightness_set(int percent);
esp_err_t bsp_touch_new(int rotation, esp_lcd_touch_handle_t *out);
esp_err_t bsp_iot_button_create(button_handle_t out[], int *n, int size);
esp_err_t bsp_battery_init(void);
esp_err_t bsp_battery_read(bsp_battery_status_t *out);
esp_err_t bsp_board_variant_get(bsp_board_variant_t *out);
esp_err_t bsp_led_init(void);
esp_err_t bsp_led_set(bool on);

/* ---------------- esp_http_server ---------------- */
typedef enum { HTTP_DELETE = 0, HTTP_GET = 1, HTTP_HEAD = 2, HTTP_POST = 3, HTTP_PUT = 4 } httpd_method_t;
typedef struct sim_req_priv sim_req_priv_t;
typedef struct httpd_req {
    httpd_method_t method;
    char uri[512];
    size_t content_len;
    void *user_ctx;
    sim_req_priv_t *priv;
} httpd_req_t;
typedef struct {
    const char *uri;
    httpd_method_t method;
    esp_err_t (*handler)(httpd_req_t *req);
    void *user_ctx;
} httpd_uri_t;
typedef void *httpd_handle_t;
typedef struct {
    uint16_t server_port;
    uint32_t stack_size;
    int core_id;
    bool lru_purge_enable;
    uint16_t max_uri_handlers;
    uint16_t max_open_sockets;
    uint16_t recv_wait_timeout, send_wait_timeout;
} httpd_config_t;
#define HTTPD_DEFAULT_CONFIG() ((httpd_config_t){.server_port = 80, .stack_size = 4096, .max_uri_handlers = 8})
typedef enum {
    HTTPD_400_BAD_REQUEST = 400,
    HTTPD_403_FORBIDDEN = 403,
    HTTPD_404_NOT_FOUND = 404,
    HTTPD_408_REQ_TIMEOUT = 408,
    HTTPD_500_INTERNAL_SERVER_ERROR = 500,
} httpd_err_code_t;
#define HTTPD_SOCK_ERR_FAIL -1
#define HTTPD_SOCK_ERR_INVALID -2
#define HTTPD_SOCK_ERR_TIMEOUT -3
#define HTTPD_RESP_USE_STRLEN -1
esp_err_t httpd_start(httpd_handle_t *out, const httpd_config_t *cfg);
esp_err_t httpd_register_uri_handler(httpd_handle_t h, const httpd_uri_t *uri);
int httpd_req_recv(httpd_req_t *req, char *buf, size_t len);
size_t httpd_req_get_hdr_value_len(httpd_req_t *req, const char *field);
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *req, const char *field, char *val, size_t len);
esp_err_t httpd_req_get_url_query_str(httpd_req_t *req, char *buf, size_t len);
esp_err_t httpd_query_key_value(const char *q, const char *key, char *val, size_t len);
esp_err_t httpd_resp_set_type(httpd_req_t *req, const char *type);
esp_err_t httpd_resp_set_hdr(httpd_req_t *req, const char *field, const char *value);
esp_err_t httpd_resp_set_status(httpd_req_t *req, const char *status);
esp_err_t httpd_resp_send(httpd_req_t *req, const char *buf, ssize_t len);
esp_err_t httpd_resp_sendstr(httpd_req_t *req, const char *str);
esp_err_t httpd_resp_send_chunk(httpd_req_t *req, const char *buf, ssize_t len);
esp_err_t httpd_resp_send_err(httpd_req_t *req, httpd_err_code_t code, const char *msg);

/* ---------------- libc bits ESP-IDF has and older glibc does not ---------------- */
#if defined(__GLIBC__) && (__GLIBC__ < 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ < 38))
size_t strlcpy(char *dst, const char *src, size_t size);
size_t strlcat(char *dst, const char *src, size_t size);
#endif

#ifdef __cplusplus
}
#endif
