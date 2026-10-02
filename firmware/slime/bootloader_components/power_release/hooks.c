/*
 * GPIO57 (PWR_SW) is the board's open-drain shutdown request: low = power off. Until the app's
 * bsp_power_init() claims it, the pad sits in its reset state, and on battery the power switch
 * reads that as a shutdown request and cuts power before the app runs (on USB nothing happens).
 * The factory "Recovery" bootloader releases it immediately; so do we, before anything else.
 */
#include "esp_rom_gpio.h"

#define PWR_SW_GPIO 57

void bootloader_hooks_include(void) {}

void bootloader_before_init(void)
{
    esp_rom_gpio_pad_select_gpio(PWR_SW_GPIO);
    esp_rom_gpio_pad_pullup_only(PWR_SW_GPIO); /* released: no pull-down, weak pull-up */
}
