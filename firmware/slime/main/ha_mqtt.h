#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * The slime in Home Assistant, over MQTT, straight from the device (no computer in between).
 * Off until a broker address is saved in the web panel (config.h: AI_MQTT_*).
 *
 *   slime/<id>/state     retained JSON, one key per entity
 *   slime/<id>/status    retained "online"; the broker publishes "offline" for us (last will)
 *   slime/<id>/set/<key> commands from Home Assistant
 *   homeassistant/<component>/slime_<id>/<key>/config   retained discovery, one per entity;
 *                        sent again whenever Home Assistant says "online" on homeassistant/status
 *
 * Entities: someone there (PIR), sees you (camera), ambient light, battery, charging, Claude's
 * state, Claude waiting for you, level, quiet hours now, focus minutes left; say (notify),
 * start / stop focus, volume, screen brightness, background music, quiet hours.
 */

/* What the main loop knows and the entities show. Sensors the pet cannot read right now
 * (no module, camera off, no fuel gauge) are published as null: "unknown" in Home Assistant. */
typedef struct {
    bool mod_ok, motion; /* Interaction module */
    unsigned light;
    bool cam_ok, face; /* camera running with face detection on */
    bool bat_ok, charging;
    int soc;
    uint8_t claude; /* cc_status_t: idle, think, work, wait */
    int level;
    bool night;  /* quiet hours right now */
    int focus_s; /* seconds left on the focus timer, 0 = not running */
} ha_state_t;

/* Call once at boot, after cfg_init() and inbox_init(). Does nothing more until a broker is set. */
void ha_start(void);
/* The broker settings may have changed (web server task): reconnect with the new ones. */
void ha_reload(void);
/* Main loop, any rate: only stores the snapshot; a task of its own decides what to send. */
void ha_update(const ha_state_t *st);
/* "off" (no broker set), "connecting", "connected", or "refused" (the broker turned the user
 * name or password down; the client keeps trying), for /api/status. */
const char *ha_status(void);
