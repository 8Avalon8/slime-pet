/* Wake word on the device: esp-sr WakeNet listens to the microphone stream the audio task
 * already reads. Nothing leaves the device; the model is part of the app image. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Loads the model and starts the detector task. */
esp_err_t wake_start(void);
bool wake_available(void);
/* Off (the default) = samples are ignored and the detector sleeps: the setting really stops the listening. */
void wake_enable(bool on);
/* 16 kHz mono samples from the audio task. Never blocks: when the detector is behind, they are dropped. */
void wake_feed(const int16_t *pcm, size_t samples);
/* True once after each detection. */
bool wake_take(void);
#ifdef __cplusplus
}
#endif
