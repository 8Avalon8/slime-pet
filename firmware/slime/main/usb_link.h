#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

/* Sent by tools/backup_and_flash.py; must stay in sync with DL_MAGIC there. */
#define USB_LINK_DL_MAGIC "SLIME-ENTER-DOWNLOAD"

/* TinyUSB CDC console on the Type-C port. Non-fatal: the pet works without it. */
esp_err_t usb_link_init(void);
/* Call from the main loop: drains USB RX and performs a requested reboot into download mode. */
void usb_link_poll(void);
