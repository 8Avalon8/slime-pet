#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Text command lines for the main loop, from any transport (USB CDC, HTTP). */
#define INBOX_LINE_MAX 128

typedef enum { SRC_USB = 0, SRC_WIFI } inbox_src_t;

bool inbox_init(void);
/* Thread-safe, non-blocking; false when full or too long. */
bool inbox_push(const char *line, inbox_src_t src);
bool inbox_pop(char *buf, size_t len, inbox_src_t *src);
const char *inbox_src_name(inbox_src_t src);
