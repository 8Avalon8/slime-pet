#include "wake.h"

#include <stdatomic.h>

#include "esp_heap_caps.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "model_path.h"

static const char *TAG = "wake";

/* packed by main/CMakeLists.txt from the model chosen in sdkconfig (CONFIG_SR_WN_*) */
extern const uint8_t srmodels_bin_start[] asm("_binary_srmodels_bin_start");
srmodel_list_t *srmodel_load(const void *root); /* esp-sr: model_path.c, not in its header */

#define WAKE_STACK 24576 /* loading the model needs far more than detecting does; it is PSRAM anyway */
static StreamBufferHandle_t s_buf;
static atomic_bool s_ready, s_on, s_hit;

/* Everything esp-sr runs in this task, model loading included: creating the model from the main
 * task left that task running on core 1 afterwards, next to the render worker, and the two halves
 * of a frame were then drawn one after the other instead of side by side. */
static void wake_task(void *arg)
{
    /* The model is hundreds of small allocations, which would normally land in internal RAM
     * (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL) and leave too little for the camera to come back
     * after a hiccup. While it loads, everything goes to PSRAM; the switch is global, but only
     * for these few hundred milliseconds. */
    heap_caps_malloc_extmem_enable(0);
    srmodel_list_t *models = srmodel_load(srmodels_bin_start); /* used in place, never freed */
    char *name = models ? esp_srmodel_filter(models, ESP_WN_PREFIX, NULL) : NULL;
    const esp_wn_iface_t *wn = name ? esp_wn_handle_from_name(name) : NULL;
    model_iface_data_t *data = wn ? wn->create(name, DET_MODE_95) : NULL;
    heap_caps_malloc_extmem_enable(CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL);
    if (!data) {
        ESP_LOGW(TAG, "no usable wake word model in this build");
        vTaskDeleteWithCaps(NULL);
    }
    const int chunk = wn->get_samp_chunksize(data); /* samples per detect call */
    const size_t bytes = (size_t)chunk * sizeof(int16_t);
    int16_t *pcm = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (!pcm) {
        ESP_LOGW(TAG, "no memory");
        vTaskDeleteWithCaps(NULL);
    }
    ESP_LOGI(TAG, "%s: \"%s\", %d samples per step, on core %d", name, esp_wn_wakeword_from_name(name), chunk, xPortGetCoreID());
    atomic_store(&s_ready, true);
    for (;;) {
        size_t got = 0;
        while (got < bytes) got += xStreamBufferReceive(s_buf, (uint8_t *)pcm + got, bytes - got, portMAX_DELAY);
        if (wn->detect(data, pcm) == WAKENET_DETECTED) {
            atomic_store(&s_hit, true);
            ESP_LOGI(TAG, "wake word heard");
        }
    }
}

esp_err_t wake_start(void)
{
    /* half a second of slack; in PSRAM like everything else that is not the LCD's */
    s_buf = xStreamBufferCreateWithCaps(8000 * sizeof(int16_t), 1, MALLOC_CAP_SPIRAM);
    if (!s_buf || xTaskCreatePinnedToCoreWithCaps(wake_task, "wake", WAKE_STACK, NULL, 2, NULL, 1, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGW(TAG, "could not start the detector");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool wake_available(void) { return atomic_load(&s_ready); }

void wake_feed(const int16_t *pcm, size_t samples)
{
    if (!atomic_load(&s_ready) || !atomic_load(&s_on)) return;
    const size_t bytes = samples * sizeof(int16_t);
    if (xStreamBufferSpacesAvailable(s_buf) < bytes) return; /* the detector is behind: drop */
    xStreamBufferSend(s_buf, pcm, bytes, 0);
}

void wake_enable(bool on) { atomic_store(&s_on, on); }

bool wake_take(void) { return atomic_exchange(&s_hit, false); }
