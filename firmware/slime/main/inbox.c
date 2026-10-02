#include "inbox.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

typedef struct {
    inbox_src_t src;
    char line[INBOX_LINE_MAX];
} item_t;

static QueueHandle_t s_q;

bool inbox_init(void)
{
    if (!s_q) s_q = xQueueCreate(24, sizeof(item_t));
    return s_q != NULL;
}

bool inbox_push(const char *line, inbox_src_t src)
{
    item_t it = {.src = src};
    const size_t n = strlen(line);
    if (!s_q || n == 0 || n >= sizeof it.line) return false;
    memcpy(it.line, line, n + 1);
    return xQueueSend(s_q, &it, 0) == pdTRUE;
}

bool inbox_pop(char *buf, size_t len, inbox_src_t *src)
{
    item_t it;
    if (!s_q || xQueueReceive(s_q, &it, 0) != pdTRUE) return false;
    strlcpy(buf, it.line, len);
    if (src) *src = it.src;
    return true;
}

const char *inbox_src_name(inbox_src_t src) { return src == SRC_WIFI ? "wifi" : "usb"; }
