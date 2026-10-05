#pragma once
/* espressif/mqtt, as far as main/ha_mqtt.c uses it. The simulator's "broker" (sim_mqtt.c) accepts
 * every connection, keeps what was published for GET /sim/mqtt, and delivers what a test posts
 * to /sim/mqtt as if Home Assistant had published it. */
#include "sim_idf.h"

typedef const char *esp_event_base_t;
typedef struct esp_mqtt_client *esp_mqtt_client_handle_t;
typedef enum {
    MQTT_EVENT_ANY = -1,
    MQTT_EVENT_ERROR = 0,
    MQTT_EVENT_CONNECTED,
    MQTT_EVENT_DISCONNECTED,
    MQTT_EVENT_SUBSCRIBED,
    MQTT_EVENT_UNSUBSCRIBED,
    MQTT_EVENT_PUBLISHED,
    MQTT_EVENT_DATA,
} esp_mqtt_event_id_t;

typedef enum { MQTT_ERROR_TYPE_NONE = 0, MQTT_ERROR_TYPE_TCP_TRANSPORT, MQTT_ERROR_TYPE_CONNECTION_REFUSED } esp_mqtt_error_type_t;
typedef struct {
    esp_mqtt_error_type_t error_type;
    int connect_return_code; /* 4: bad user name or password, 5: not authorized */
} esp_mqtt_error_codes_t;

typedef struct {
    esp_mqtt_event_id_t event_id;
    esp_mqtt_client_handle_t client;
    char *data;
    int data_len, total_data_len, current_data_offset;
    char *topic;
    int topic_len;
    esp_mqtt_error_codes_t *error_handle;
} esp_mqtt_event_t;
typedef esp_mqtt_event_t *esp_mqtt_event_handle_t;

typedef struct {
    struct {
        struct { const char *uri; } address;
        struct { esp_err_t (*crt_bundle_attach)(void *conf); } verification;
    } broker;
    struct {
        const char *username, *client_id;
        struct { const char *password; } authentication;
    } credentials;
    struct {
        struct { const char *topic, *msg; int qos, retain; } last_will;
        int keepalive;
    } session;
    struct { int reconnect_timeout_ms; } network;
    struct { int priority, stack_size; } task;
    struct { int size, out_size; } buffer;
} esp_mqtt_client_config_t;

typedef void (*esp_event_handler_t)(void *arg, esp_event_base_t base, int32_t id, void *data);

esp_mqtt_client_handle_t esp_mqtt_client_init(const esp_mqtt_client_config_t *config);
esp_err_t esp_mqtt_client_register_event(esp_mqtt_client_handle_t c, esp_mqtt_event_id_t ev, esp_event_handler_t fn, void *arg);
esp_err_t esp_mqtt_client_start(esp_mqtt_client_handle_t c);
esp_err_t esp_mqtt_client_stop(esp_mqtt_client_handle_t c);
esp_err_t esp_mqtt_client_destroy(esp_mqtt_client_handle_t c);
int esp_mqtt_client_subscribe_single(esp_mqtt_client_handle_t c, const char *topic, int qos);
int esp_mqtt_client_publish(esp_mqtt_client_handle_t c, const char *topic, const char *data, int len, int qos, int retain);
