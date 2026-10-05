/* Shared between the simulator's parts: idf/sim_idf.h stand-ins, the stand-in drivers and the window. */
#pragma once
#include "sim_idf.h"

#define SIM_SCREEN 480
enum { SIM_BTN_AI = 0, SIM_BTN_BOOT = 1 };

typedef struct {
    int port;             /* HTTP, default 8080 */
    const char *bind;     /* default 127.0.0.1 */
    const char *nvs_path; /* NULL: settings and level start fresh every run */
    bool headless, quiet;
} sim_opt_t;
extern sim_opt_t sim_opt;

void sim_time_start(void);
void sim_sleep_ms(uint32_t ms);

/* display: copies the panel's current picture (native RGB565), returns the number of pushes so far */
uint32_t sim_screen_copy(uint16_t *dst);
int sim_screen_brightness(void);
int sim_led_mood(void);       /* led_mood_t the firmware asked the Interaction module for */
int sim_led_brightness(void); /* 0-255 */
void sim_mood_rgb(int mood, double t, uint8_t rgb[3]);

/* inputs */
void sim_touch(bool down, int x, int y);
void sim_button(int which, bool long_press);
void sim_battery(bool present, int soc, int ma);
/* One text command (see sim_help()); also reachable as POST /sim/input and on stdin as "sim <cmd>".
 * Returns false and writes why into err for an unknown or malformed command. */
bool sim_input(const char *cmd, char *err, size_t len);
const char *sim_help(void);
bool sim_screenshot(const char *path);

void sim_http_extras(void); /* registers /sim/input, /sim/screen.bmp and /sim/mqtt */
void sim_mqtt_http(void);    /* sim_mqtt.c: the stand-in MQTT broker, GET and POST /sim/mqtt */
void sim_stdin_start(void); /* stdin lines -> the USB console (inbox), "sim ..." -> sim_input */

void app_main(void); /* firmware/slime/main/main.c */
