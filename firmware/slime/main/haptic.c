#include "haptic.h"

#include "bsp/esp_mosaico.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "haptic";
static QueueHandle_t s_q;
static volatile bool s_on = true;

/* Alternating on/off durations in ms, 0-terminated. */
static const uint16_t PATTERN[HAPTIC_COUNT][8] = {
    [HAPTIC_TICK] = {22},
    [HAPTIC_NUDGE] = {70, 110, 70},
    [HAPTIC_HURT] = {160},
    [HAPTIC_FANFARE] = {35, 70, 35, 70, 35, 120, 180},
};

static void haptic_task(void *arg)
{
    for (;;) {
        haptic_t h;
        xQueueReceive(s_q, &h, portMAX_DELAY);
        const uint16_t *p = PATTERN[h];
        for (int i = 0; i < 8 && p[i]; i++) {
            bsp_motor_set(i % 2 == 0);
            vTaskDelay(pdMS_TO_TICKS(p[i]));
        }
        bsp_motor_set(false);
        vTaskDelay(pdMS_TO_TICKS(60));
    }
}

esp_err_t haptic_init(void)
{
    ESP_RETURN_ON_ERROR(bsp_motor_init(), TAG, "motor");
    s_q = xQueueCreate(4, sizeof(haptic_t));
    ESP_RETURN_ON_FALSE(s_q, ESP_ERR_NO_MEM, TAG, "queue");
    ESP_RETURN_ON_FALSE(xTaskCreatePinnedToCore(haptic_task, "haptic", 2048, NULL, 4, NULL, 0) == pdPASS, ESP_ERR_NO_MEM, TAG,
                        "task");
    return ESP_OK;
}

void haptic_play(haptic_t h)
{
    if (s_on && s_q && h < HAPTIC_COUNT) xQueueSend(s_q, &h, 0);
}

void haptic_set_enabled(bool on) { s_on = on; }
