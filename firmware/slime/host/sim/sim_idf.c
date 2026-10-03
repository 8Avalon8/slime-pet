/* POSIX stand-ins for the ESP-IDF calls declared in idf/sim_idf.h. */
#include "sim.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* ---------------- errors, log, time ---------------- */

const char *esp_err_to_name(esp_err_t e)
{
    switch (e) {
    case ESP_OK: return "ESP_OK";
    case ESP_FAIL: return "ESP_FAIL";
    case ESP_ERR_NO_MEM: return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_ARG: return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_INVALID_SIZE: return "ESP_ERR_INVALID_SIZE";
    case ESP_ERR_NOT_FOUND: return "ESP_ERR_NOT_FOUND";
    case ESP_ERR_NOT_SUPPORTED: return "ESP_ERR_NOT_SUPPORTED";
    case ESP_ERR_TIMEOUT: return "ESP_ERR_TIMEOUT";
    case ESP_ERR_NVS_NOT_FOUND: return "ESP_ERR_NVS_NOT_FOUND";
    default: return "ESP_ERR_UNKNOWN";
    }
}

static struct timespec s_t0;

static void time_init(void)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, sim_time_start);
}

void sim_time_start(void) { clock_gettime(CLOCK_MONOTONIC, &s_t0); }

int64_t esp_timer_get_time(void)
{
    time_init();
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)(ts.tv_sec - s_t0.tv_sec) * 1000000 + (ts.tv_nsec - s_t0.tv_nsec) / 1000;
}

void sim_log(char level, const char *tag, const char *fmt, ...)
{
    if (level == 'I' && sim_opt.quiet) return;
    static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&m);
    fprintf(stderr, "%c (%lld) %s: ", level, (long long)(esp_timer_get_time() / 1000), tag);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    pthread_mutex_unlock(&m);
}

void sim_sleep_ms(uint32_t ms)
{
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) && errno == EINTR) {
    }
}

static struct timespec deadline(TickType_t ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    return ts;
}

/* ---------------- heap, random ---------------- */

void *heap_caps_malloc(size_t size, uint32_t caps) { return malloc(size); }
void *heap_caps_calloc(size_t n, size_t size, uint32_t caps) { return calloc(n, size); }
void *heap_caps_aligned_calloc(size_t align, size_t n, size_t size, uint32_t caps)
{
    void *p = NULL;
    if (posix_memalign(&p, align < sizeof(void *) ? sizeof(void *) : align, n * size)) return NULL;
    memset(p, 0, n * size);
    return p;
}
size_t heap_caps_get_free_size(uint32_t caps) { return caps & MALLOC_CAP_SPIRAM ? 24u << 20 : 300u << 10; }
uint32_t esp_random(void) { return ((uint32_t)rand() << 16) ^ (uint32_t)rand(); }

#if defined(__GLIBC__) && (__GLIBC__ < 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ < 38))
size_t strlcpy(char *dst, const char *src, size_t size)
{
    const size_t n = strlen(src);
    if (size) {
        const size_t c = n < size - 1 ? n : size - 1;
        memcpy(dst, src, c);
        dst[c] = 0;
    }
    return n;
}
size_t strlcat(char *dst, const char *src, size_t size)
{
    const size_t d = strnlen(dst, size);
    return d == size ? size + strlen(src) : d + strlcpy(dst + d, src, size - d);
}
#endif

/* ---------------- FreeRTOS ---------------- */

struct sim_sem {
    pthread_mutex_t m;
    pthread_cond_t c;
    int count, max;
};

static SemaphoreHandle_t sem_new(int count, int max)
{
    SemaphoreHandle_t s = calloc(1, sizeof *s);
    pthread_mutex_init(&s->m, NULL);
    pthread_cond_init(&s->c, NULL);
    s->count = count;
    s->max = max;
    return s;
}
SemaphoreHandle_t xSemaphoreCreateBinary(void) { return sem_new(0, 1); }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return sem_new(1, 1); }

BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t ticks)
{
    pthread_mutex_lock(&s->m);
    const struct timespec dl = deadline(ticks);
    while (s->count == 0) {
        if (ticks == 0) break;
        if (ticks == portMAX_DELAY) pthread_cond_wait(&s->c, &s->m);
        else if (pthread_cond_timedwait(&s->c, &s->m, &dl) == ETIMEDOUT) break;
    }
    const bool ok = s->count > 0;
    if (ok) s->count--;
    pthread_mutex_unlock(&s->m);
    return ok ? pdTRUE : pdFALSE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t s)
{
    pthread_mutex_lock(&s->m);
    const bool ok = s->count < s->max;
    if (ok) s->count++;
    pthread_cond_signal(&s->c);
    pthread_mutex_unlock(&s->m);
    return ok ? pdTRUE : pdFALSE;
}

BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t s, BaseType_t *woken)
{
    if (woken) *woken = pdFALSE;
    return xSemaphoreGive(s);
}

struct sim_queue {
    pthread_mutex_t m;
    pthread_cond_t c;
    size_t len, size, head, n;
    uint8_t *buf;
};

QueueHandle_t xQueueCreate(UBaseType_t len, UBaseType_t item_size)
{
    QueueHandle_t q = calloc(1, sizeof *q);
    pthread_mutex_init(&q->m, NULL);
    pthread_cond_init(&q->c, NULL);
    q->len = len;
    q->size = item_size;
    q->buf = calloc(len, item_size);
    return q;
}

BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t ticks)
{
    pthread_mutex_lock(&q->m);
    const bool ok = q->n < q->len; /* never blocks: main/ only sends with a 0 timeout */
    if (ok) {
        memcpy(q->buf + ((q->head + q->n) % q->len) * q->size, item, q->size);
        q->n++;
        pthread_cond_signal(&q->c);
    }
    pthread_mutex_unlock(&q->m);
    return ok ? pdTRUE : pdFALSE;
}

BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t ticks)
{
    pthread_mutex_lock(&q->m);
    const struct timespec dl = deadline(ticks);
    while (q->n == 0 && ticks) {
        if (ticks == portMAX_DELAY) pthread_cond_wait(&q->c, &q->m);
        else if (pthread_cond_timedwait(&q->c, &q->m, &dl) == ETIMEDOUT) break;
    }
    const bool ok = q->n > 0;
    if (ok) {
        memcpy(item, q->buf + q->head * q->size, q->size);
        q->head = (q->head + 1) % q->len;
        q->n--;
    }
    pthread_mutex_unlock(&q->m);
    return ok ? pdTRUE : pdFALSE;
}

typedef struct {
    TaskFunction_t fn;
    void *arg;
} task_t;

static void *task_entry(void *p)
{
    task_t t = *(task_t *)p;
    free(p);
    t.fn(t.arg);
    return NULL;
}

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, uint32_t stack, void *arg, UBaseType_t prio,
                                   TaskHandle_t *out, BaseType_t core)
{
    task_t *t = malloc(sizeof *t);
    t->fn = fn;
    t->arg = arg;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&at, stack < 65536 ? 1 << 20 : stack * 4);
    pthread_t th;
    const int e = pthread_create(&th, &at, task_entry, t);
    pthread_attr_destroy(&at);
    if (e) {
        free(t);
        return pdFAIL;
    }
    if (out) *out = (TaskHandle_t)(uintptr_t)1;
    return pdPASS;
}

void vTaskDelay(TickType_t ticks) { sim_sleep_ms(ticks ? ticks : 1); }
void vTaskDelete(TaskHandle_t t)
{
    if (!t) pthread_exit(NULL);
}

/* ---------------- esp_timer: one thread per timer ---------------- */

struct sim_timer {
    esp_timer_create_args_t args;
    pthread_mutex_t m;
    pthread_cond_t c;
    uint64_t due_us; /* 0 = stopped */
};

static void *timer_thread(void *p)
{
    struct sim_timer *t = p;
    pthread_mutex_lock(&t->m);
    for (;;) {
        while (!t->due_us) pthread_cond_wait(&t->c, &t->m);
        const int64_t left = (int64_t)t->due_us - esp_timer_get_time();
        if (left > 0) {
            const struct timespec dl = deadline((TickType_t)(left / 1000 + 1));
            pthread_cond_timedwait(&t->c, &t->m, &dl);
            continue;
        }
        t->due_us = 0;
        pthread_mutex_unlock(&t->m);
        t->args.callback(t->args.arg);
        pthread_mutex_lock(&t->m);
    }
    return NULL;
}

esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *out)
{
    struct sim_timer *t = calloc(1, sizeof *t);
    t->args = *args;
    pthread_mutex_init(&t->m, NULL);
    pthread_cond_init(&t->c, NULL);
    pthread_t th;
    if (pthread_create(&th, NULL, timer_thread, t)) return ESP_ERR_NO_MEM;
    pthread_detach(th);
    *out = t;
    return ESP_OK;
}

static esp_err_t timer_set(esp_timer_handle_t t, uint64_t due)
{
    pthread_mutex_lock(&t->m);
    t->due_us = due;
    pthread_cond_signal(&t->c);
    pthread_mutex_unlock(&t->m);
    return ESP_OK;
}
esp_err_t esp_timer_start_once(esp_timer_handle_t t, uint64_t us) { return timer_set(t, esp_timer_get_time() + us + 1); }
esp_err_t esp_timer_stop(esp_timer_handle_t t) { return timer_set(t, 0); }

/* ---------------- NVS ---------------- */

#define NVS_MAX 64
#define NVS_VAL_MAX 1024
typedef struct {
    bool used;
    char ns[16], key[16];
    uint32_t len;
    uint8_t val[NVS_VAL_MAX];
} nvs_entry_t;
static nvs_entry_t s_nvs[NVS_MAX];
static char s_nvs_ns[8][16]; /* handle - 1 -> namespace */
static pthread_mutex_t s_nvs_m = PTHREAD_MUTEX_INITIALIZER;

static void nvs_save(void)
{
    if (!sim_opt.nvs_path) return;
    FILE *f = fopen(sim_opt.nvs_path, "wb");
    if (!f) return;
    fwrite(s_nvs, sizeof s_nvs, 1, f);
    fclose(f);
}

esp_err_t nvs_flash_init(void)
{
    if (sim_opt.nvs_path) {
        FILE *f = fopen(sim_opt.nvs_path, "rb");
        if (f) {
            if (fread(s_nvs, sizeof s_nvs, 1, f) != 1) memset(s_nvs, 0, sizeof s_nvs);
            fclose(f);
        }
    }
    return ESP_OK;
}

esp_err_t nvs_flash_erase(void)
{
    memset(s_nvs, 0, sizeof s_nvs);
    nvs_save();
    return ESP_OK;
}

esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out)
{
    pthread_mutex_lock(&s_nvs_m);
    bool exists = false;
    for (int i = 0; i < NVS_MAX; i++) exists |= s_nvs[i].used && !strcmp(s_nvs[i].ns, ns);
    int h = -1;
    for (int i = 0; i < 8 && h < 0; i++)
        if (!strcmp(s_nvs_ns[i], ns) || !s_nvs_ns[i][0]) h = i;
    if (h >= 0) snprintf(s_nvs_ns[h], sizeof s_nvs_ns[h], "%s", ns);
    pthread_mutex_unlock(&s_nvs_m);
    if (h < 0) return ESP_ERR_NO_MEM;
    if (mode == NVS_READONLY && !exists) return ESP_ERR_NVS_NOT_FOUND;
    *out = (nvs_handle_t)h + 1;
    return ESP_OK;
}

void nvs_close(nvs_handle_t h) {}

esp_err_t nvs_commit(nvs_handle_t h)
{
    pthread_mutex_lock(&s_nvs_m);
    nvs_save();
    pthread_mutex_unlock(&s_nvs_m);
    return ESP_OK;
}

static nvs_entry_t *nvs_find(nvs_handle_t h, const char *key, bool create)
{
    if (h < 1 || h > 8) return NULL;
    const char *ns = s_nvs_ns[h - 1];
    nvs_entry_t *free_e = NULL;
    for (int i = 0; i < NVS_MAX; i++) {
        if (s_nvs[i].used && !strcmp(s_nvs[i].ns, ns) && !strcmp(s_nvs[i].key, key)) return &s_nvs[i];
        if (!s_nvs[i].used && !free_e) free_e = &s_nvs[i];
    }
    if (!create || !free_e) return NULL;
    memset(free_e, 0, sizeof *free_e);
    free_e->used = true;
    snprintf(free_e->ns, sizeof free_e->ns, "%s", ns);
    snprintf(free_e->key, sizeof free_e->key, "%s", key);
    return free_e;
}

static esp_err_t nvs_get(nvs_handle_t h, const char *key, void *out, size_t *len, bool exact)
{
    pthread_mutex_lock(&s_nvs_m);
    const nvs_entry_t *e = nvs_find(h, key, false);
    esp_err_t r = ESP_ERR_NVS_NOT_FOUND;
    if (e) {
        if (!out) {
            *len = e->len;
            r = ESP_OK;
        } else if (*len < e->len || (exact && *len != e->len)) {
            r = ESP_ERR_INVALID_SIZE;
        } else {
            memcpy(out, e->val, e->len);
            *len = e->len;
            r = ESP_OK;
        }
    }
    pthread_mutex_unlock(&s_nvs_m);
    return r;
}

static esp_err_t nvs_set(nvs_handle_t h, const char *key, const void *val, size_t len)
{
    if (len > NVS_VAL_MAX) return ESP_ERR_INVALID_SIZE;
    pthread_mutex_lock(&s_nvs_m);
    nvs_entry_t *e = nvs_find(h, key, true);
    if (e) {
        memcpy(e->val, val, len);
        e->len = (uint32_t)len;
    }
    pthread_mutex_unlock(&s_nvs_m);
    return e ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len) { return nvs_get(h, key, out, len, false); }
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *val, size_t len) { return nvs_set(h, key, val, len); }
esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *len) { return nvs_get(h, key, out, len, false); }
esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *val) { return nvs_set(h, key, val, strlen(val) + 1); }
#define NVS_INT(suffix, type)                                                                                          \
    esp_err_t nvs_get_##suffix(nvs_handle_t h, const char *key, type *out)                                            \
    {                                                                                                                  \
        size_t len = sizeof(type);                                                                                     \
        return nvs_get(h, key, out, &len, true);                                                                       \
    }                                                                                                                  \
    esp_err_t nvs_set_##suffix(nvs_handle_t h, const char *key, type val) { return nvs_set(h, key, &val, sizeof val); }
NVS_INT(i32, int32_t)
NVS_INT(u32, uint32_t)
NVS_INT(u8, uint8_t)

esp_err_t nvs_erase_key(nvs_handle_t h, const char *key)
{
    pthread_mutex_lock(&s_nvs_m);
    nvs_entry_t *e = nvs_find(h, key, false);
    if (e) e->used = false;
    pthread_mutex_unlock(&s_nvs_m);
    return e ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}

/* ---------------- display, touch, buttons, board ---------------- */

static uint16_t s_screen[SIM_SCREEN * SIM_SCREEN];
static pthread_mutex_t s_screen_m = PTHREAD_MUTEX_INITIALIZER;
static volatile uint32_t s_frames;
static volatile int s_brightness = 100;
static esp_lcd_panel_io_color_trans_done_cb_t s_trans_cb;
static void *s_trans_ctx;
static int s_panel_dummy, s_io_dummy, s_touch_dummy;

esp_err_t bsp_power_init(void) { return ESP_OK; }
esp_err_t bsp_display_new(const bsp_display_config_t *cfg, esp_lcd_panel_handle_t *out)
{
    *out = (esp_lcd_panel_handle_t)&s_panel_dummy;
    return ESP_OK;
}
esp_lcd_panel_io_handle_t bsp_display_get_panel_io(void) { return (esp_lcd_panel_io_handle_t)&s_io_dummy; }
esp_err_t bsp_display_on(void) { return ESP_OK; }
esp_err_t bsp_display_brightness_set(int percent)
{
    s_brightness = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    return ESP_OK;
}

esp_err_t esp_lcd_panel_io_register_event_callbacks(esp_lcd_panel_io_handle_t io, const esp_lcd_panel_io_callbacks_t *cbs,
                                                    void *ctx)
{
    s_trans_cb = cbs->on_color_trans_done;
    s_trans_ctx = ctx;
    return ESP_OK;
}

/* data: packed rectangle of byte-swapped RGB565, as the CO5300 wants it over QSPI */
esp_err_t esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t p, int x0, int y0, int x1, int y1, const void *data)
{
    const uint16_t *src = data;
    const int w = x1 - x0;
    pthread_mutex_lock(&s_screen_m);
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) s_screen[y * SIM_SCREEN + x] = __builtin_bswap16(src[(y - y0) * w + (x - x0)]);
    s_frames++;
    pthread_mutex_unlock(&s_screen_m);
    if (s_trans_cb) s_trans_cb(bsp_display_get_panel_io(), NULL, s_trans_ctx); /* "DMA" done at once */
    return ESP_OK;
}

uint32_t sim_screen_copy(uint16_t *dst)
{
    pthread_mutex_lock(&s_screen_m);
    memcpy(dst, s_screen, sizeof s_screen);
    const uint32_t n = s_frames;
    pthread_mutex_unlock(&s_screen_m);
    return n;
}
int sim_screen_brightness(void) { return s_brightness; }

static pthread_mutex_t s_touch_m = PTHREAD_MUTEX_INITIALIZER;
static bool s_touch_down, s_touch_seen = true;
static int s_touch_x, s_touch_y;

esp_err_t bsp_touch_new(int rotation, esp_lcd_touch_handle_t *out)
{
    *out = (esp_lcd_touch_handle_t)&s_touch_dummy;
    return ESP_OK;
}
esp_err_t esp_lcd_touch_read_data(esp_lcd_touch_handle_t tp) { return ESP_OK; }
bool esp_lcd_touch_get_coordinates(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y, uint16_t *strength,
                                   uint8_t *point_num, uint8_t max_point_num)
{
    pthread_mutex_lock(&s_touch_m);
    /* a tap shorter than one frame still shows up as one "down" sample */
    const bool down = s_touch_down || !s_touch_seen;
    s_touch_seen = true;
    x[0] = (uint16_t)s_touch_x;
    y[0] = (uint16_t)s_touch_y;
    pthread_mutex_unlock(&s_touch_m);
    *point_num = down ? 1 : 0;
    return down;
}

void sim_touch(bool down, int x, int y)
{
    pthread_mutex_lock(&s_touch_m);
    if (down && !s_touch_down) s_touch_seen = false;
    s_touch_down = down;
    if (down) {
        s_touch_x = x < 0 ? 0 : x >= SIM_SCREEN ? SIM_SCREEN - 1 : x;
        s_touch_y = y < 0 ? 0 : y >= SIM_SCREEN ? SIM_SCREEN - 1 : y;
    }
    pthread_mutex_unlock(&s_touch_m);
}

struct sim_button {
    button_cb_t cb[BUTTON_EVENT_MAX];
    void *usr[BUTTON_EVENT_MAX];
};
static struct sim_button s_btn[2]; /* SIM_BTN_AI, SIM_BTN_BOOT */

esp_err_t bsp_iot_button_create(button_handle_t out[], int *n, int size)
{
    out[BSP_BUTTON_AI] = &s_btn[SIM_BTN_AI];
    *n = BSP_BUTTON_NUM;
    return ESP_OK;
}
esp_err_t iot_button_new_gpio_device(const button_config_t *bc, const button_gpio_config_t *gc, button_handle_t *out)
{
    *out = &s_btn[SIM_BTN_BOOT];
    return ESP_OK;
}
esp_err_t iot_button_register_cb(button_handle_t b, button_event_t ev, button_event_args_t *args, button_cb_t cb, void *usr)
{
    b->cb[ev] = cb;
    b->usr[ev] = usr;
    return ESP_OK;
}

void sim_button(int which, bool long_press)
{
    struct sim_button *b = &s_btn[which];
    const button_event_t ev = long_press ? BUTTON_LONG_PRESS_START : BUTTON_SINGLE_CLICK;
    if (b->cb[ev]) b->cb[ev](b, b->usr[ev]);
}

static pthread_mutex_t s_bat_m = PTHREAD_MUTEX_INITIALIZER;
static bsp_battery_status_t s_bat = {.voltage_mv = 4050, .current_ma = -120, .state_of_charge = 90};
static bool s_bat_ok = true;

esp_err_t bsp_battery_init(void) { return ESP_OK; }
esp_err_t bsp_battery_read(bsp_battery_status_t *out)
{
    pthread_mutex_lock(&s_bat_m);
    *out = s_bat;
    const bool ok = s_bat_ok;
    pthread_mutex_unlock(&s_bat_m);
    return ok ? ESP_OK : ESP_ERR_NOT_FOUND;
}
void sim_battery(bool present, int soc, int ma)
{
    pthread_mutex_lock(&s_bat_m);
    s_bat_ok = present;
    s_bat.state_of_charge = soc < 0 ? 0 : soc > 100 ? 100 : soc;
    s_bat.current_ma = ma;
    s_bat.voltage_mv = 3400 + s_bat.state_of_charge * 8;
    pthread_mutex_unlock(&s_bat_m);
}
esp_err_t bsp_board_variant_get(bsp_board_variant_t *out)
{
    *out = BSP_BOARD_VARIANT_V1_1;
    return ESP_OK;
}
esp_err_t bsp_led_init(void) { return ESP_OK; }
esp_err_t bsp_led_set(bool on) { return ESP_OK; }

/* ---------------- esp_http_server: one thread, one request per connection ---------------- */

#define URI_MAX 32
static httpd_uri_t s_uris[URI_MAX];
static int s_n_uris;
static char s_uri_names[URI_MAX][64];

struct sim_req_priv {
    int fd;
    const char *req_hdrs; /* request line and headers, NUL-terminated */
    const char *body; /* already read part of the body */
    size_t body_have, body_used;
    char query[256];
    char type[64];
    char hdrs[512];
    char status[48];
    bool chunked; /* headers sent, chunked encoding */
    bool sent;
};

static bool send_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    while (len) {
        const ssize_t n = send(fd, p, len, 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return false;
        }
        p += n;
        len -= (size_t)n;
    }
    return true;
}

static void send_head(httpd_req_t *req, long len)
{
    sim_req_priv_t *r = req->priv;
    char h[1024];
    int n = snprintf(h, sizeof h, "HTTP/1.1 %s\r\nContent-Type: %s\r\n%sConnection: close\r\n", r->status[0] ? r->status : "200 OK",
                     r->type[0] ? r->type : "text/html", r->hdrs);
    if (len >= 0) n += snprintf(h + n, sizeof h - n, "Content-Length: %ld\r\n\r\n", len);
    else n += snprintf(h + n, sizeof h - n, "Transfer-Encoding: chunked\r\n\r\n");
    send_all(r->fd, h, (size_t)n);
}

int httpd_req_recv(httpd_req_t *req, char *buf, size_t len)
{
    sim_req_priv_t *r = req->priv;
    if (r->body_used < r->body_have) {
        size_t n = r->body_have - r->body_used;
        if (n > len) n = len;
        memcpy(buf, r->body + r->body_used, n);
        r->body_used += n;
        return (int)n;
    }
    const ssize_t n = recv(r->fd, buf, len, 0);
    return n > 0 ? (int)n : HTTPD_SOCK_ERR_FAIL;
}

size_t httpd_req_get_hdr_value_len(httpd_req_t *req, const char *field) { return 0; }
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *req, const char *field, char *val, size_t len)
{
    const size_t fl = strlen(field);
    for (const char *l = strstr(req->priv->req_hdrs, "\r\n"); l; l = strstr(l + 2, "\r\n")) {
        if (strncasecmp(l + 2, field, fl) || l[2 + fl] != ':') continue;
        const char *v = l + 3 + fl;
        while (*v == ' ') v++;
        const char *e = strstr(v, "\r\n");
        const size_t n = e ? (size_t)(e - v) : strlen(v);
        if (n >= len) return ESP_ERR_INVALID_SIZE;
        memcpy(val, v, n);
        val[n] = 0;
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t httpd_req_get_url_query_str(httpd_req_t *req, char *buf, size_t len)
{
    if (!req->priv->query[0]) return ESP_ERR_NOT_FOUND;
    strlcpy(buf, req->priv->query, len);
    return ESP_OK;
}

esp_err_t httpd_query_key_value(const char *q, const char *key, char *val, size_t len)
{
    const size_t kl = strlen(key);
    for (const char *p = q; p && *p; p = strchr(p, '&') ? strchr(p, '&') + 1 : NULL) {
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            const char *v = p + kl + 1, *e = strchr(v, '&');
            const size_t n = e ? (size_t)(e - v) : strlen(v);
            if (n >= len) return ESP_ERR_INVALID_SIZE;
            memcpy(val, v, n);
            val[n] = 0;
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t httpd_resp_set_type(httpd_req_t *req, const char *type)
{
    strlcpy(req->priv->type, type, sizeof req->priv->type);
    return ESP_OK;
}
esp_err_t httpd_resp_set_hdr(httpd_req_t *req, const char *field, const char *value)
{
    sim_req_priv_t *r = req->priv;
    const size_t n = strlen(r->hdrs);
    snprintf(r->hdrs + n, sizeof r->hdrs - n, "%s: %s\r\n", field, value);
    return ESP_OK;
}
esp_err_t httpd_resp_set_status(httpd_req_t *req, const char *status)
{
    strlcpy(req->priv->status, status, sizeof req->priv->status);
    return ESP_OK;
}

esp_err_t httpd_resp_send(httpd_req_t *req, const char *buf, ssize_t len)
{
    if (len == HTTPD_RESP_USE_STRLEN) len = buf ? (ssize_t)strlen(buf) : 0;
    send_head(req, len);
    if (len) send_all(req->priv->fd, buf, (size_t)len);
    req->priv->sent = true;
    return ESP_OK;
}
esp_err_t httpd_resp_sendstr(httpd_req_t *req, const char *str) { return httpd_resp_send(req, str, HTTPD_RESP_USE_STRLEN); }

esp_err_t httpd_resp_send_chunk(httpd_req_t *req, const char *buf, ssize_t len)
{
    sim_req_priv_t *r = req->priv;
    if (len == HTTPD_RESP_USE_STRLEN) len = buf ? (ssize_t)strlen(buf) : 0;
    if (!r->chunked) {
        send_head(req, -1);
        r->chunked = true;
    }
    char h[24];
    const int n = snprintf(h, sizeof h, "%zx\r\n", (size_t)len);
    send_all(r->fd, h, (size_t)n);
    if (len) send_all(r->fd, buf, (size_t)len);
    send_all(r->fd, "\r\n", 2);
    if (!len) r->sent = true;
    return ESP_OK;
}

esp_err_t httpd_resp_send_err(httpd_req_t *req, httpd_err_code_t code, const char *msg)
{
    const char *txt = code == 400 ? "Bad Request" : code == 403 ? "Forbidden" : code == 404 ? "Not Found" : code == 408 ? "Request Timeout" : "Internal Server Error";
    snprintf(req->priv->status, sizeof req->priv->status, "%d %s", (int)code, txt);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, msg ? msg : txt);
    return ESP_FAIL;
}

esp_err_t httpd_register_uri_handler(httpd_handle_t h, const httpd_uri_t *uri)
{
    if (s_n_uris >= URI_MAX) return ESP_ERR_NO_MEM;
    strlcpy(s_uri_names[s_n_uris], uri->uri, sizeof s_uri_names[0]);
    s_uris[s_n_uris] = *uri;
    s_uris[s_n_uris].uri = s_uri_names[s_n_uris];
    s_n_uris++;
    return ESP_OK;
}

static void serve(int fd)
{
    static char buf[8192];
    size_t have = 0;
    char *end = NULL;
    while (!end && have < sizeof buf - 1) {
        const ssize_t n = recv(fd, buf + have, sizeof buf - 1 - have, 0);
        if (n <= 0) return;
        have += (size_t)n;
        buf[have] = 0;
        end = strstr(buf, "\r\n\r\n");
    }
    if (!end) return;
    *end = 0;
    char method[8] = "", path[512] = "";
    if (sscanf(buf, "%7s %511s", method, path) != 2) return;
    httpd_req_t req = {.method = !strcmp(method, "POST") ? HTTP_POST : !strcmp(method, "GET") ? HTTP_GET : HTTP_PUT};
    sim_req_priv_t priv = {.fd = fd, .req_hdrs = buf, .body = end + 4, .body_have = have - (size_t)(end + 4 - buf)};
    req.priv = &priv;
    for (char *l = strstr(buf, "\r\n"); l; l = strstr(l + 2, "\r\n"))
        if (!strncasecmp(l + 2, "Content-Length:", 15)) req.content_len = strtoul(l + 17, NULL, 10);
    char *q = strchr(path, '?');
    if (q) {
        *q = 0;
        strlcpy(priv.query, q + 1, sizeof priv.query);
    }
    strlcpy(req.uri, path, sizeof req.uri);
    for (int i = 0; i < s_n_uris; i++) {
        if (s_uris[i].method == req.method && !strcmp(s_uris[i].uri, path)) {
            req.user_ctx = s_uris[i].user_ctx;
            s_uris[i].handler(&req);
            if (!priv.sent && !priv.chunked) httpd_resp_send_err(&req, HTTPD_500_INTERNAL_SERVER_ERROR, "handler sent nothing");
            return;
        }
    }
    httpd_resp_send_err(&req, HTTPD_404_NOT_FOUND, "Nothing matches the given URI");
}

static void *httpd_thread(void *arg)
{
    const int ls = (int)(intptr_t)arg;
    for (;;) {
        const int fd = accept(ls, NULL, NULL);
        if (fd < 0) continue;
        const struct timeval tv = {5, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        serve(fd);
        shutdown(fd, SHUT_WR);
        close(fd);
    }
    return NULL;
}

esp_err_t httpd_start(httpd_handle_t *out, const httpd_config_t *cfg)
{
    signal(SIGPIPE, SIG_IGN);
    const int ls = socket(AF_INET, SOCK_STREAM, 0);
    const int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)sim_opt.port)};
    inet_pton(AF_INET, sim_opt.bind, &a.sin_addr);
    if (ls < 0 || bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 8)) {
        sim_log('E', "httpd", "cannot listen on %s:%d: %s", sim_opt.bind, sim_opt.port, strerror(errno));
        return ESP_FAIL;
    }
    sim_http_extras(); /* /sim/... endpoints next to the firmware's own */
    pthread_t th;
    pthread_create(&th, NULL, httpd_thread, (void *)(intptr_t)ls);
    pthread_detach(th);
    sim_log('I', "httpd", "listening on http://%s:%d/", sim_opt.bind, sim_opt.port);
    *out = (httpd_handle_t)(intptr_t)1;
    return ESP_OK;
}
