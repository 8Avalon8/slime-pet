#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * Why did the last sessions end? Each boot appends {reset reason, the previous session's last
 * snapshot} to an NVS ring (8 entries). The snapshot lives in RTC no-init memory, so it survives
 * brownout / panic / watchdog resets but not a real power cut (then it reads as "none").
 */
void bootlog_init(void);
/* Call every few seconds. */
void bootlog_snapshot(uint32_t uptime_s, int voltage_mv, int current_ma, int soc);
/* JSON array, newest first: [{"reason":"brownout","prev_up":123,"mv":3550,"ma":-900,"soc":40}, ...] */
void bootlog_json(char *buf, size_t len);
/* Last crash or hang saved in the core dump partition (task, PC, RA, cause), as a JSON object,
 * or "null". The record stays until the next crash replaces it. */
void bootlog_crash_json(char *buf, size_t len);
