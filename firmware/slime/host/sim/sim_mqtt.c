#include "sim_idf.h" /* the simulator stands in for every ESP-IDF header main/ uses */
/*
 * Stand-in for espressif/mqtt and the broker behind it, so that the real main/ha_mqtt.c runs in
 * the simulator. Every connection succeeds a moment after esp_mqtt_client_start(), unless the
 * address has "unreachable" in it, or the password is "wrong" (refused, as a real broker would).
 * Nothing leaves the process:
 *   GET  /sim/mqtt   the connection as the broker saw it, and every message published so far
 *   POST /sim/mqtt   "<topic>\n<payload>": deliver that message to the pet (Home Assistant's side);
 *                    "@drop": lose the connection and get it back; "@clear": forget the messages
 */
#include <pthread.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "mqtt_client.h"
#include "sim.h"

#define LOG_MAX 512

struct esp_mqtt_client {
    char uri[160], user[80], pass[160], client_id[64], will_topic[96], will_msg[32];
    int will_retain, keepalive;
    esp_event_handler_t fn;
    void *arg;
    bool started, connected;
    int generation; /* a connect thread from before a stop() must not connect the next start() */
};

static pthread_mutex_t s_mu = PTHREAD_MUTEX_INITIALIZER;
static struct esp_mqtt_client s_c;
static bool s_exists;
static char s_subs[8][96];
static int s_nsubs, s_connects;
static struct {
    char *topic, *payload;
    bool retain;
} s_log[LOG_MAX];
static int s_nlog;

static void set(char *dst, size_t len, const char *src) { snprintf(dst, len, "%s", src ? src : ""); }

static void emit(esp_mqtt_event_id_t id, const char *topic, const char *data)
{
    esp_mqtt_event_t e = {.event_id = id, .client = &s_c, .topic = (char *)topic, .topic_len = topic ? (int)strlen(topic) : 0,
                          .data = (char *)data, .data_len = data ? (int)strlen(data) : 0};
    e.total_data_len = e.data_len;
    if (s_c.fn) s_c.fn(s_c.arg, "MQTT_EVENTS", id, &e);
}

static void *connect_thread(void *arg)
{
    const int gen = (int)(intptr_t)arg;
    usleep(50 * 1000);
    pthread_mutex_lock(&s_mu);
    const bool live = s_exists && s_c.started && s_c.generation == gen && !strstr(s_c.uri, "unreachable");
    const bool go = live && strcmp(s_c.pass, "wrong");
    if (go) {
        s_c.connected = true;
        s_nsubs = 0;
        s_connects++;
    }
    pthread_mutex_unlock(&s_mu);
    if (go) emit(MQTT_EVENT_CONNECTED, NULL, NULL);
    else if (live) {
        esp_mqtt_error_codes_t err = {.error_type = MQTT_ERROR_TYPE_CONNECTION_REFUSED, .connect_return_code = 4};
        esp_mqtt_event_t e = {.event_id = MQTT_EVENT_ERROR, .client = &s_c, .error_handle = &err};
        if (s_c.fn) s_c.fn(s_c.arg, "MQTT_EVENTS", MQTT_EVENT_ERROR, &e);
        emit(MQTT_EVENT_DISCONNECTED, NULL, NULL);
    }
    return NULL;
}

static void connect_soon(void)
{
    pthread_t th;
    pthread_create(&th, NULL, connect_thread, (void *)(intptr_t)s_c.generation);
    pthread_detach(th);
}

esp_mqtt_client_handle_t esp_mqtt_client_init(const esp_mqtt_client_config_t *c)
{
    const char *uri = c->broker.address.uri;
    if (!uri || (strncmp(uri, "mqtt://", 7) && strncmp(uri, "mqtts://", 8)) || !uri[strcspn(uri, "/") + 2]) return NULL;
    pthread_mutex_lock(&s_mu);
    const int gen = s_c.generation + 1;
    memset(&s_c, 0, sizeof s_c);
    s_c.generation = gen;
    set(s_c.uri, sizeof s_c.uri, uri);
    set(s_c.user, sizeof s_c.user, c->credentials.username);
    set(s_c.pass, sizeof s_c.pass, c->credentials.authentication.password);
    set(s_c.client_id, sizeof s_c.client_id, c->credentials.client_id);
    set(s_c.will_topic, sizeof s_c.will_topic, c->session.last_will.topic);
    set(s_c.will_msg, sizeof s_c.will_msg, c->session.last_will.msg);
    s_c.will_retain = c->session.last_will.retain;
    s_c.keepalive = c->session.keepalive;
    s_exists = true;
    pthread_mutex_unlock(&s_mu);
    return &s_c;
}

esp_err_t esp_mqtt_client_register_event(esp_mqtt_client_handle_t c, esp_mqtt_event_id_t ev, esp_event_handler_t fn, void *arg)
{
    c->fn = fn;
    c->arg = arg;
    return ESP_OK;
}

esp_err_t esp_mqtt_client_start(esp_mqtt_client_handle_t c)
{
    pthread_mutex_lock(&s_mu);
    c->started = true;
    connect_soon();
    pthread_mutex_unlock(&s_mu);
    return ESP_OK;
}

esp_err_t esp_mqtt_client_stop(esp_mqtt_client_handle_t c)
{
    pthread_mutex_lock(&s_mu);
    c->started = c->connected = false;
    c->generation++;
    pthread_mutex_unlock(&s_mu);
    return ESP_OK;
}

esp_err_t esp_mqtt_client_destroy(esp_mqtt_client_handle_t c)
{
    pthread_mutex_lock(&s_mu);
    c->started = c->connected = false;
    c->generation++;
    c->fn = NULL;
    s_exists = false;
    pthread_mutex_unlock(&s_mu);
    return ESP_OK;
}

int esp_mqtt_client_subscribe_single(esp_mqtt_client_handle_t c, const char *topic, int qos)
{
    pthread_mutex_lock(&s_mu);
    if (s_nsubs < 8) set(s_subs[s_nsubs++], sizeof s_subs[0], topic);
    pthread_mutex_unlock(&s_mu);
    return 1;
}

int esp_mqtt_client_publish(esp_mqtt_client_handle_t c, const char *topic, const char *data, int len, int qos, int retain)
{
    pthread_mutex_lock(&s_mu);
    const bool up = s_exists && c->connected;
    if (up) {
        if (s_nlog == LOG_MAX) { /* keep the newest */
            free(s_log[0].topic);
            free(s_log[0].payload);
            memmove(s_log, s_log + 1, sizeof s_log[0] * --s_nlog);
        }
        s_log[s_nlog].topic = strdup(topic);
        s_log[s_nlog].payload = len > 0 ? strndup(data, (size_t)len) : strdup(data ? data : "");
        s_log[s_nlog++].retain = retain;
    }
    pthread_mutex_unlock(&s_mu);
    if (up && !sim_opt.quiet) fprintf(stderr, "[mqtt] %s%s %s\n", retain ? "(retained) " : "", topic, data ? data : "");
    return up ? 0 : -1;
}

/* ---------------- /sim/mqtt ---------------- */

static esp_err_t h_get(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    pthread_mutex_lock(&s_mu);
    cJSON_AddBoolToObject(o, "client", s_exists);
    cJSON_AddBoolToObject(o, "connected", s_exists && s_c.connected);
    cJSON_AddNumberToObject(o, "connects", s_connects);
    if (s_exists) {
        cJSON_AddStringToObject(o, "uri", s_c.uri);
        cJSON_AddStringToObject(o, "user", s_c.user);
        cJSON_AddStringToObject(o, "pass", s_c.pass);
        cJSON_AddStringToObject(o, "client_id", s_c.client_id);
        cJSON_AddNumberToObject(o, "keepalive", s_c.keepalive);
        cJSON *w = cJSON_AddObjectToObject(o, "will");
        cJSON_AddStringToObject(w, "topic", s_c.will_topic);
        cJSON_AddStringToObject(w, "payload", s_c.will_msg);
        cJSON_AddBoolToObject(w, "retain", s_c.will_retain);
    }
    cJSON *subs = cJSON_AddArrayToObject(o, "subs");
    for (int i = 0; i < s_nsubs; i++) cJSON_AddItemToArray(subs, cJSON_CreateString(s_subs[i]));
    cJSON *msgs = cJSON_AddArrayToObject(o, "msgs");
    for (int i = 0; i < s_nlog; i++) {
        cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "topic", s_log[i].topic);
        cJSON_AddStringToObject(m, "payload", s_log[i].payload);
        cJSON_AddBoolToObject(m, "retain", s_log[i].retain);
        cJSON_AddItemToArray(msgs, m);
    }
    pthread_mutex_unlock(&s_mu);
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    const esp_err_t e = httpd_resp_sendstr(req, s ? s : "{}");
    cJSON_free(s);
    return e;
}

static esp_err_t h_post(httpd_req_t *req)
{
    char body[1024];
    if (req->content_len >= sizeof body) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body too large");
    size_t got = 0;
    while (got < req->content_len) {
        const int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r <= 0) return ESP_FAIL;
        got += (size_t)r;
    }
    body[got] = 0;
    if (!strcmp(body, "@clear")) {
        pthread_mutex_lock(&s_mu);
        while (s_nlog) {
            free(s_log[--s_nlog].topic);
            free(s_log[s_nlog].payload);
        }
        pthread_mutex_unlock(&s_mu);
    } else if (!strcmp(body, "@drop")) {
        pthread_mutex_lock(&s_mu);
        const bool was = s_exists && s_c.connected;
        s_c.connected = false;
        pthread_mutex_unlock(&s_mu);
        if (was) {
            emit(MQTT_EVENT_DISCONNECTED, NULL, NULL);
            pthread_mutex_lock(&s_mu);
            connect_soon(); /* the real client reconnects by itself */
            pthread_mutex_unlock(&s_mu);
        }
    } else {
        char *nl = strchr(body, '\n');
        pthread_mutex_lock(&s_mu);
        const bool up = s_exists && s_c.connected;
        pthread_mutex_unlock(&s_mu);
        if (!nl || !up) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, nl ? "the pet is not connected" : "expected <topic>\\n<payload>");
        *nl = 0;
        emit(MQTT_EVENT_DATA, body, nl + 1);
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

void sim_mqtt_http(void)
{
    const httpd_uri_t u[] = {
        {.uri = "/sim/mqtt", .method = HTTP_GET, .handler = h_get},
        {.uri = "/sim/mqtt", .method = HTTP_POST, .handler = h_post},
    };
    for (size_t i = 0; i < sizeof u / sizeof u[0]; i++) httpd_register_uri_handler(NULL, &u[i]);
}
