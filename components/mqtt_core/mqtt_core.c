#include "mqtt_core.h"

#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

#include "config_store.h"
#include "event_bus.h"
#include "automation_engine.h"
#include "dm_template_runtime.h"

// Минимальный MQTT 3.1.1 брокер: QoS0/1, retain, LWT, простая ACL (prefix-based), без QoS2/username/password/TLS.

static const char *TAG = "mqtt_core";

#define MQTT_MAX_CLIENTS       12
#define MQTT_MAX_SUBS          8
#define MQTT_MAX_TOPIC         96
#define MQTT_MAX_PAYLOAD       512
#define MQTT_MAX_PACKET        1024
#define MQTT_RETAIN_MAX        32
#define MQTT_CLIENT_STACK      6144
#define MQTT_ACCEPT_STACK      4096

typedef struct {
    bool in_use;
    char topic[MQTT_MAX_TOPIC];
    char *payload;
    size_t payload_len;
    uint8_t qos;
} retain_entry_t;

typedef struct {
    char topic[MQTT_MAX_TOPIC];
    uint8_t qos;
} mqtt_subscription_t;

typedef struct {
    bool has;
    char topic[MQTT_MAX_TOPIC];
    char payload[MQTT_MAX_PAYLOAD];
    uint8_t qos;
    bool retain;
} will_t;

typedef struct {
    int sock;
    TaskHandle_t task;
    bool active;
    bool closing;
    char client_id[CONFIG_STORE_CLIENT_ID_MAX];
    uint16_t keepalive;
    int64_t last_rx_ms;
    mqtt_subscription_t subs[MQTT_MAX_SUBS];
    size_t sub_count;
    will_t will;
} mqtt_session_t;

typedef struct {
    const char *client_id;   // "*" == любой клиент
    const char *pub_prefix;  // разрешённый префикс публикации ("*" == любой)
    const char *sub_prefix;  // разрешённый префикс подписки
} acl_entry_t;

static const acl_entry_t k_acl[] = {
    {"pn532",  "access/", "access/"},
    {"laser",  "laser/",  "laser/"},
    {"relay",  "relay/",  "relay/"},
    {"puppet", "puppet/", "puppet/"},
    {"webui",  "web/",    "web/"},
    {"*",      "*",       "*"}, // дефолт: разрешить всё (можно убрать в проде)
};

typedef struct {
    event_bus_type_t type;
    const char *topic;
} event_topic_map_t;

static const event_topic_map_t k_outgoing_map[] = {
    {EVENT_AUDIO_PLAY, "audio/play"},
    {EVENT_SYSTEM_STATUS, "sys/broker/metrics"},
    {EVENT_CARD_OK, "access/card/ok"},
    {EVENT_CARD_BAD, "access/card/bad"},
    {EVENT_RELAY_CMD, "relay/cmd"},
    {EVENT_WEB_COMMAND, "web/cmd"},
};

static const event_topic_map_t k_incoming_map[] = {
    {EVENT_AUDIO_PLAY, "audio/play"},
    {EVENT_RELAY_CMD, "relay/"},
    {EVENT_WEB_COMMAND, "web/cmd"},
};

static mqtt_session_t *s_sessions = NULL;
static StackType_t *s_session_stacks[MQTT_MAX_CLIENTS];
static StaticTask_t *s_session_tcbs[MQTT_MAX_CLIENTS];
static uint8_t *s_session_tx_bufs[MQTT_MAX_CLIENTS];
static retain_entry_t *s_retain = NULL;
static SemaphoreHandle_t s_lock = NULL;
static uint8_t s_client_count = 0;
static int s_listen_sock = -1;
static TaskHandle_t s_accept_task = NULL;
static StackType_t *s_accept_stack = NULL;
static StaticTask_t *s_accept_tcb = NULL;
static esp_timer_handle_t s_sweep_timer = NULL;

static void lock(void);
static void unlock(void);

static inline size_t session_index(const mqtt_session_t *sess)
{
    if (!sess || !s_sessions) {
        return MQTT_MAX_CLIENTS;
    }
    return (size_t)(sess - s_sessions);
}

static bool ensure_session_task_storage(size_t idx)
{
    if (idx >= MQTT_MAX_CLIENTS) {
        return false;
    }
    if (!s_session_stacks[idx]) {
        s_session_stacks[idx] = heap_caps_malloc(MQTT_CLIENT_STACK * sizeof(StackType_t),
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_session_stacks[idx]) {
            return false;
        }
    }
    if (!s_session_tcbs[idx]) {
        s_session_tcbs[idx] = heap_caps_malloc(sizeof(StaticTask_t),
                                               MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!s_session_tcbs[idx]) {
            heap_caps_free(s_session_stacks[idx]);
            s_session_stacks[idx] = NULL;
            return false;
        }
    }
    return true;
}

static uint8_t *ensure_session_tx_buffer(size_t idx)
{
    if (idx >= MQTT_MAX_CLIENTS) {
        return NULL;
    }
    if (!s_session_tx_bufs[idx]) {
        s_session_tx_bufs[idx] = heap_caps_malloc(MQTT_MAX_PACKET, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    return s_session_tx_bufs[idx];
}

static bool ensure_accept_task_storage(void)
{
    if (!s_accept_stack) {
        s_accept_stack = heap_caps_malloc(MQTT_ACCEPT_STACK * sizeof(StackType_t),
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_accept_stack) {
            return false;
        }
    }
    if (!s_accept_tcb) {
        s_accept_tcb = heap_caps_malloc(sizeof(StaticTask_t),
                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!s_accept_tcb) {
            heap_caps_free(s_accept_stack);
            s_accept_stack = NULL;
            return false;
        }
    }
    return true;
}

void mqtt_core_get_client_stats(mqtt_client_stats_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    for (size_t i = 0; i < MQTT_MAX_CLIENTS; ++i) {
        if (!s_sessions[i].active) continue;
        out->total++;
    }
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

uint8_t mqtt_core_client_count(void)
{
    uint8_t count = 0;
    lock();
    count = s_client_count;
    unlock();
    return count;
}

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static bool prefix_match(const char *prefix, const char *topic)
{
    if (!prefix || !topic) {
        return false;
    }
    if (strcmp(prefix, "*") == 0) {
        return true;
    }
    size_t len = strlen(prefix);
    return strncmp(topic, prefix, len) == 0;
}

static bool acl_can_publish(const char *client_id, const char *topic)
{
    for (size_t i = 0; i < sizeof(k_acl) / sizeof(k_acl[0]); ++i) {
        if (prefix_match(k_acl[i].client_id, client_id)) {
            return prefix_match(k_acl[i].pub_prefix, topic);
        }
    }
    return false;
}

static bool acl_can_subscribe(const char *client_id, const char *topic)
{
    for (size_t i = 0; i < sizeof(k_acl) / sizeof(k_acl[0]); ++i) {
        if (prefix_match(k_acl[i].client_id, client_id)) {
            return prefix_match(k_acl[i].sub_prefix, topic);
        }
    }
    return false;
}

static const char *find_topic_by_type(event_bus_type_t type)
{
    for (size_t i = 0; i < sizeof(k_outgoing_map) / sizeof(k_outgoing_map[0]); ++i) {
        if (k_outgoing_map[i].type == type) {
            return k_outgoing_map[i].topic;
        }
    }
    return NULL;
}

const char *mqtt_core_topic_for_event(event_bus_type_t type)
{
    return find_topic_by_type(type);
}

static event_bus_type_t find_type_by_topic(const char *topic)
{
    if (!topic) {
        return EVENT_NONE;
    }
    for (size_t i = 0; i < sizeof(k_incoming_map) / sizeof(k_incoming_map[0]); ++i) {
        const char *t = k_incoming_map[i].topic;
        size_t len = strlen(t);
        if (strncmp(topic, t, len) == 0) {
            return k_incoming_map[i].type;
        }
    }
    return EVENT_NONE;
}

static bool topic_matches_filter(const char *filter, const char *topic)
{
    // Поддержка MQTT wildcards: '#' (хвост), '+' (один уровень). Минимально.
    const char *f = filter;
    const char *t = topic;
    while (*f && *t) {
        if (*f == '#') {
            return true;
        }
        if (*f == '+') {
            // пропустить текущий уровень до '/'
            while (*t && *t != '/') {
                t++;
            }
            f++;
            if (*t == '/') {
                t++;
            }
            continue;
        }
        if (*f != *t) {
            return false;
        }
        f++;
        t++;
    }
    if (*f == '\0' && *t == '\0') {
        return true;
    }
    if (*f == '#' && *(f + 1) == '\0') {
        return true;
    }
    return false;
}

static int recv_all(int sock, uint8_t *buf, size_t len)
{
    size_t got = 0;
    while (got < len) {
        int r = recv(sock, buf + got, len - got, 0);
        if (r <= 0) {
            return -1;
        }
        got += (size_t)r;
    }
    return (int)got;
}

static int send_all(int sock, const uint8_t *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        int r = send(sock, buf + sent, len - sent, 0);
        if (r <= 0) {
            return -1;
        }
        sent += (size_t)r;
    }
    return (int)sent;
}

static int read_remaining_length(int sock, int *out_rem)
{
    int multiplier = 1;
    int value = 0;
    uint8_t encoded = 0;
    do {
        if (recv_all(sock, &encoded, 1) != 1) {
            return -1;
        }
        value += (encoded & 127) * multiplier;
        multiplier *= 128;
        if (multiplier > 128 * 128 * 128) {
            return -1;
        }
    } while ((encoded & 128) != 0);
    *out_rem = value;
    return 0;
}

static void lock(void)
{
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void unlock(void)
{
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

static mqtt_session_t *alloc_session(void)
{
    if (!s_sessions) {
        return NULL;
    }
    for (size_t i = 0; i < MQTT_MAX_CLIENTS; ++i) {
        if (!s_sessions[i].active) {
            memset(&s_sessions[i], 0, sizeof(s_sessions[i]));
            s_sessions[i].active = true;
            s_client_count++;
            return &s_sessions[i];
        }
    }
    return NULL;
}

static mqtt_session_t *find_session_by_client_id(const char *client_id)
{
    if (!client_id || !client_id[0]) {
        return NULL;
    }
    for (size_t i = 0; i < MQTT_MAX_CLIENTS; ++i) {
        if (s_sessions[i].active && strcmp(s_sessions[i].client_id, client_id) == 0) {
            return &s_sessions[i];
        }
    }
    return NULL;
}

static void free_session(mqtt_session_t *s)
{
    if (!s) {
        return;
    }
    s->active = false;
    s->closing = false;
    if (s->sock >= 0) {
        shutdown(s->sock, SHUT_RDWR);
        closesocket(s->sock);
    }
    s->sock = -1;
    s->task = NULL;
    if (s_client_count > 0) {
        s_client_count--;
    }
}

static void sweep_idle_sessions(void)
{
    int64_t now = now_ms();
    lock();
    for (size_t i = 0; i < MQTT_MAX_CLIENTS; ++i) {
        mqtt_session_t *s = &s_sessions[i];
        if (!s->active) {
            continue;
        }
        int64_t idle_ms = now - s->last_rx_ms;
        int64_t limit_ms = (s->keepalive > 0) ? (int64_t)s->keepalive * 1500 : 60000;
        if (idle_ms > limit_ms) {
            ESP_LOGW(TAG, "sweep: closing idle client_id=%s idle=%lldms", s->client_id, (long long)idle_ms);
            s->closing = true;
            if (s->sock >= 0) {
                shutdown(s->sock, SHUT_RDWR); // let handler exit and free_session
            }
        }
    }
    unlock();
}

// (placeholder removed; free_session defined above)

static void retain_free_entry(retain_entry_t *slot)
{
    if (!slot) {
        return;
    }
    if (slot->payload) {
        heap_caps_free(slot->payload);
        slot->payload = NULL;
    }
    slot->payload_len = 0;
}

static retain_entry_t *retain_get(const char *topic)
{
    if (!s_retain || !topic) {
        return NULL;
    }
    for (size_t i = 0; i < MQTT_RETAIN_MAX; ++i) {
        if (s_retain[i].in_use && strcmp(s_retain[i].topic, topic) == 0) {
            return &s_retain[i];
        }
    }
    return NULL;
}

static void retain_store(const char *topic, const char *payload, uint8_t qos)
{
    if (!s_retain || !topic || !payload) {
        return;
    }
    retain_entry_t *slot = retain_get(topic);
    if (!slot) {
        for (size_t i = 0; i < MQTT_RETAIN_MAX; ++i) {
            if (!s_retain[i].in_use) {
                slot = &s_retain[i];
                break;
            }
        }
    }
    if (!slot) {
        ESP_LOGW(TAG, "retain table full, dropping %s", topic);
        return;
    }
    size_t len = strnlen(payload, MQTT_MAX_PAYLOAD - 1);
    char *buf = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        ESP_LOGW(TAG, "retain alloc failed for %s", topic);
        return;
    }
    memcpy(buf, payload, len);
    buf[len] = '\0';
    retain_free_entry(slot);
    slot->in_use = true;
    strncpy(slot->topic, topic, sizeof(slot->topic) - 1);
    slot->topic[sizeof(slot->topic) - 1] = '\0';
    slot->payload = buf;
    slot->payload_len = len;
    slot->qos = qos;
}

static size_t encode_remaining_length(uint8_t *out, size_t rem_len)
{
    size_t idx = 0;
    do {
        uint8_t byte = rem_len % 128;
        rem_len /= 128;
        if (rem_len > 0) {
            byte |= 0x80;
        }
        out[idx++] = byte;
    } while (rem_len > 0 && idx < 4);
    return idx;
}

static int send_connack(int sock, uint8_t rc)
{
    uint8_t pkt[4] = {0x20, 0x02, 0x00, rc};
    return send_all(sock, pkt, sizeof(pkt));
}

static int send_suback(mqtt_session_t *sess, uint16_t pid, uint8_t *qos, size_t count)
{
    size_t slot = session_index(sess);
    uint8_t *buf = ensure_session_tx_buffer(slot);
    if (!buf) {
        ESP_LOGE(TAG, "suback buffer alloc failed");
        return -1;
    }
    size_t idx = 0;
    buf[idx++] = 0x90;
    size_t rem_idx = idx++;
    buf[idx++] = (uint8_t)(pid >> 8);
    buf[idx++] = (uint8_t)(pid & 0xFF);
    for (size_t i = 0; i < count; ++i) {
        buf[idx++] = qos[i];
    }
    buf[rem_idx] = (uint8_t)(idx - 2);
    return send_all(sess->sock, buf, idx);
}

static int send_puback(int sock, uint16_t pid)
{
    uint8_t buf[4] = {0x40, 0x02, (uint8_t)(pid >> 8), (uint8_t)(pid & 0xFF)};
    return send_all(sock, buf, sizeof(buf));
}

static int send_pingresp(int sock)
{
    uint8_t buf[2] = {0xD0, 0x00};
    return send_all(sock, buf, sizeof(buf));
}

static int send_publish_packet(mqtt_session_t *sess, const char *topic, const char *payload, uint8_t qos, bool retain, uint16_t pid)
{
    if (!sess) {
        return -1;
    }
    size_t slot = session_index(sess);
    uint8_t *buf = ensure_session_tx_buffer(slot);
    if (!buf) {
        ESP_LOGE(TAG, "publish buffer alloc failed");
        return -1;
    }
    size_t topic_len = strlen(topic);
    size_t payload_len = strlen(payload);
    size_t rem_len = 2 + topic_len + payload_len + (qos ? 2 : 0);

    uint8_t header = 0x30 | (qos << 1) | (retain ? 0x01 : 0x00);
    uint8_t rem_enc[4];
    size_t rem_enc_len = encode_remaining_length(rem_enc, rem_len);

    size_t idx = 0;
    buf[idx++] = header;
    memcpy(&buf[idx], rem_enc, rem_enc_len);
    idx += rem_enc_len;
    buf[idx++] = (uint8_t)(topic_len >> 8);
    buf[idx++] = (uint8_t)(topic_len & 0xFF);
    memcpy(&buf[idx], topic, topic_len);
    idx += topic_len;
    if (qos) {
        buf[idx++] = (uint8_t)(pid >> 8);
        buf[idx++] = (uint8_t)(pid & 0xFF);
    }
    memcpy(&buf[idx], payload, payload_len);
    idx += payload_len;
    return send_all(sess->sock, buf, idx);
}

static void publish_to_subscribers(const char *topic, const char *payload, uint8_t qos, bool retain_flag, mqtt_session_t *exclude)
{
    lock();
    // Retain storage.
    if (retain_flag) {
        retain_store(topic, payload, qos);
    }
    for (size_t i = 0; i < MQTT_MAX_CLIENTS; ++i) {
        mqtt_session_t *s = &s_sessions[i];
        if (!s->active || s == exclude) {
            continue;
        }
        for (size_t j = 0; j < s->sub_count; ++j) {
            if (topic_matches_filter(s->subs[j].topic, topic)) {
                uint16_t pid = (qos ? (uint16_t)(esp_random() & 0xFFFF) : 0);
                if (send_publish_packet(s, topic, payload, qos, retain_flag, pid) < 0) {
                    ESP_LOGW(TAG, "send publish failed to %s", s->client_id);
                }
                break;
            }
        }
    }
    unlock();
}

static void deliver_retain(mqtt_session_t *sess, const char *filter)
{
    if (!s_retain) {
        return;
    }
    lock();
    for (size_t i = 0; i < MQTT_RETAIN_MAX; ++i) {
        if (!s_retain[i].in_use) {
            continue;
        }
        if (topic_matches_filter(filter, s_retain[i].topic)) {
            const char *payload = s_retain[i].payload ? s_retain[i].payload : "";
            send_publish_packet(sess, s_retain[i].topic, payload, s_retain[i].qos, true, 0);
        }
    }
    unlock();
}

static int parse_utf8_str(const uint8_t *buf, size_t len, size_t *offset, char *out, size_t out_len)
{
    if (*offset + 2 > len) {
        return -1;
    }
    uint16_t slen = (buf[*offset] << 8) | buf[*offset + 1];
    *offset += 2;
    if (*offset + slen > len || slen >= out_len) {
        return -1;
    }
    memcpy(out, buf + *offset, slen);
    out[slen] = 0;
    *offset += slen;
    return 0;
}

static void handle_client(void *param);

static bool mqtt_authenticate_client(const char *client_id, const char *username, const char *password)
{
    const app_config_t *cfg = config_store_get();
    if (!cfg) {
        return false;
    }
    if (cfg->mqtt.user_count == 0) {
        return true;
    }
    for (uint8_t i = 0; i < cfg->mqtt.user_count; ++i) {
        const app_mqtt_user_t *user = &cfg->mqtt.users[i];
        if (strcmp(user->client_id, client_id) != 0) {
            continue;
        }
        if (strcmp(user->username, username ? username : "") != 0) {
            continue;
        }
        if (strcmp(user->password, password ? password : "") != 0) {
            continue;
        }
        return true;
    }
    return false;
}

static int handle_connect(mqtt_session_t *sess, const uint8_t *buf, size_t len)
{
    size_t off = 0;
    char proto[8];
    if (parse_utf8_str(buf, len, &off, proto, sizeof(proto)) != 0) {
        return -1;
    }
    if (strcmp(proto, "MQTT") != 0 && strcmp(proto, "MQIsdp") != 0) {
        return -1;
    }
    if (off + 4 > len) {
        return -1;
    }
    uint8_t level = buf[off++];
    uint8_t flags = buf[off++];
    uint16_t keepalive = (buf[off] << 8) | buf[off + 1];
    off += 2;

    char client_id[CONFIG_STORE_CLIENT_ID_MAX];
    if (parse_utf8_str(buf, len, &off, client_id, sizeof(client_id)) != 0) {
        return -1;
    }

    if (level != 4) {
        ESP_LOGW(TAG, "MQTT level %u not 4", level);
    }

    sess->keepalive = keepalive;
    strncpy(sess->client_id, client_id, sizeof(sess->client_id) - 1);
    sess->last_rx_ms = now_ms();

    // Drop existing session with same client_id to avoid ghost clients.
    lock();
    mqtt_session_t *old = find_session_by_client_id(sess->client_id);
    if (old && old != sess) {
        ESP_LOGW(TAG, "Replacing session for client_id=%s", sess->client_id);
        free_session(old);
    }
    unlock();

    bool will_flag = flags & 0x04;
    bool will_retain = flags & 0x20;
    uint8_t will_qos = (flags >> 3) & 0x03;
    if (will_flag) {
        char will_topic[MQTT_MAX_TOPIC];
        char will_payload[MQTT_MAX_PAYLOAD];
        if (parse_utf8_str(buf, len, &off, will_topic, sizeof(will_topic)) != 0) {
            return -1;
        }
        if (parse_utf8_str(buf, len, &off, will_payload, sizeof(will_payload)) != 0) {
            return -1;
        }
        sess->will.has = true;
        strncpy(sess->will.topic, will_topic, sizeof(sess->will.topic) - 1);
        strncpy(sess->will.payload, will_payload, sizeof(sess->will.payload) - 1);
        sess->will.qos = will_qos;
        sess->will.retain = will_retain;
    }

    // username/password игнорируем (офсеты пропускаем).
    char username[CONFIG_STORE_USERNAME_MAX] = {0};
    char password[CONFIG_STORE_PASSWORD_MAX] = {0};
    if (flags & 0x80) {
        if (parse_utf8_str(buf, len, &off, username, sizeof(username)) != 0) {
            return -1;
        }
    }
    if (flags & 0x40) {
        if (parse_utf8_str(buf, len, &off, password, sizeof(password)) != 0) {
            return -1;
        }
    }
    if (!mqtt_authenticate_client(client_id, username, password)) {
        ESP_LOGW(TAG, "MQTT auth failed for client_id=%s", client_id);
        return -1;
    }
    return 0;
}

static int handle_subscribe(mqtt_session_t *sess, const uint8_t *buf, size_t len)
{
    size_t off = 0;
    if (len < 2) {
        return -1;
    }
    uint16_t pid = (buf[off] << 8) | buf[off + 1];
    off += 2;

    uint8_t granted[MQTT_MAX_SUBS];
    size_t granted_count = 0;

    while (off + 3 <= len && granted_count < MQTT_MAX_SUBS) {
        char topic[MQTT_MAX_TOPIC];
        if (parse_utf8_str(buf, len, &off, topic, sizeof(topic)) != 0) {
            return -1;
        }
        uint8_t rqos = buf[off++];
        if (!acl_can_subscribe(sess->client_id, topic)) {
            ESP_LOGW(TAG, "ACL deny sub %s -> %s", sess->client_id, topic);
            granted[granted_count++] = 0x80; // отказ
            continue;
        }
        if (sess->sub_count < MQTT_MAX_SUBS) {
            strncpy(sess->subs[sess->sub_count].topic, topic, sizeof(sess->subs[sess->sub_count].topic) - 1);
            sess->subs[sess->sub_count].qos = rqos > 1 ? 1 : rqos;
            sess->sub_count++;
            granted[granted_count++] = rqos > 1 ? 1 : rqos;
            deliver_retain(sess, topic);
        } else {
            granted[granted_count++] = 0x80;
        }
    }

    return send_suback(sess, pid, granted, granted_count);
}

static int handle_publish(mqtt_session_t *sess, uint8_t header, const uint8_t *buf, size_t len)
{
    size_t off = 0;
    if (len < 2) {
        return -1;
    }
    uint16_t topic_len = (buf[off] << 8) | buf[off + 1];
    off += 2;
    if (off + topic_len > len || topic_len >= MQTT_MAX_TOPIC) {
        return -1;
    }
    char topic[MQTT_MAX_TOPIC];
    memcpy(topic, buf + off, topic_len);
    topic[topic_len] = 0;
    off += topic_len;

    uint16_t pid = 0;
    uint8_t qos = (header >> 1) & 0x03;
    bool retain = header & 0x01;
    if (qos) {
        if (off + 2 > len) {
            return -1;
        }
        pid = (buf[off] << 8) | buf[off + 1];
        off += 2;
    }
    if (!acl_can_publish(sess->client_id, topic)) {
        ESP_LOGW(TAG, "ACL deny pub %s -> %s", sess->client_id, topic);
        return 0;
    }
    size_t payload_len = len - off;
    if (payload_len >= MQTT_MAX_PAYLOAD) {
        payload_len = MQTT_MAX_PAYLOAD - 1;
    }
    char payload[MQTT_MAX_PAYLOAD];
    memcpy(payload, buf + off, payload_len);
    payload[payload_len] = 0;

    mqtt_core_inject_message(topic, payload);
    publish_to_subscribers(topic, payload, qos, retain, NULL);

    if (qos == 1) {
        send_puback(sess->sock, pid);
    }
    return 0;
}

static void on_event_bus_message(const event_bus_message_t *msg)
{
    if (!msg) {
        return;
    }
    const char *topic = msg->topic[0] ? msg->topic : find_topic_by_type(msg->type);
    if (!topic) {
        return;
    }
    mqtt_core_publish(topic, msg->payload);
}

static void send_will_if_needed(mqtt_session_t *sess)
{
    if (sess->will.has) {
        ESP_LOGI(TAG, "sending will for %s", sess->client_id);
        publish_to_subscribers(sess->will.topic, sess->will.payload, sess->will.qos, sess->will.retain, sess);
    }
}

static void handle_client(void *param)
{
    mqtt_session_t *sess = (mqtt_session_t *)param;
    uint8_t header = 0;

    // CONNECT ожидание
    int rem = 0;
    if (recv_all(sess->sock, &header, 1) != 1 || read_remaining_length(sess->sock, &rem) < 0 || rem > MQTT_MAX_PACKET) {
        goto cleanup;
    }
    uint8_t pkt[MQTT_MAX_PACKET];
    if (recv_all(sess->sock, pkt, rem) < 0) {
        goto cleanup;
    }
    if ((header >> 4) != 1 || handle_connect(sess, pkt, rem) != 0) {
        send_connack(sess->sock, 0x02); // protocol error
        goto cleanup;
    }
    send_connack(sess->sock, 0x00);
    ESP_LOGI(TAG, "MQTT CONNECT %s keepalive=%u", sess->client_id, sess->keepalive);

    while (1) {
        int r = recv(sess->sock, &header, 1, 0);
        if (r <= 0) {
            int err = errno;
            if (sess->closing) {
                ESP_LOGW(TAG, "closing session %s", sess->client_id);
                break;
            }
            if (err == EAGAIN || err == EWOULDBLOCK) {
                // recv timeout, check keepalive expiry
                int64_t idle_ms = now_ms() - sess->last_rx_ms;
                int64_t limit_ms = (sess->keepalive > 0) ? (int64_t)sess->keepalive * 1500 : 60000;
                if (idle_ms > limit_ms) {
                    ESP_LOGW(TAG, "keepalive timeout %s", sess->client_id);
                    break;
                }
                continue;
            }
            ESP_LOGW(TAG, "socket closed %s err=%d", sess->client_id, err);
            break;
        }
        if (read_remaining_length(sess->sock, &rem) < 0 || rem > MQTT_MAX_PACKET) {
            ESP_LOGW(TAG, "bad remaining length");
            break;
        }
        if (recv_all(sess->sock, pkt, rem) < 0) {
            break;
        }
        sess->last_rx_ms = now_ms();
        uint8_t type = header >> 4;
        switch (type) {
        case 3: // PUBLISH
            if (handle_publish(sess, header, pkt, rem) != 0) {
                ESP_LOGW(TAG, "publish parse fail");
                goto cleanup;
            }
            break;
        case 8: // SUBSCRIBE
            if (handle_subscribe(sess, pkt, rem) < 0) {
                ESP_LOGW(TAG, "subscribe parse fail");
                goto cleanup;
            }
            break;
        case 12: // PINGREQ
            send_pingresp(sess->sock);
            break;
        case 14: // DISCONNECT
            goto cleanup;
        default:
            ESP_LOGW(TAG, "unsupported packet type %u", type);
            goto cleanup;
        }
        // Keepalive check
        if (sess->keepalive > 0) {
            int64_t idle_ms = now_ms() - sess->last_rx_ms;
            if (idle_ms > (int64_t)(sess->keepalive * 1500)) { // 1.5 * keepalive
                ESP_LOGW(TAG, "keepalive timeout %s", sess->client_id);
                goto cleanup;
            }
        }
    }

cleanup:
    send_will_if_needed(sess);
    lock();
    free_session(sess);
    unlock();
    vTaskDelete(NULL);
}

static void accept_task(void *param)
{
    (void)param;
    while (1) {
        struct sockaddr_in6 source_addr;
        socklen_t addr_len = sizeof(source_addr);
        int sock = accept(s_listen_sock, (struct sockaddr *)&source_addr, &addr_len);
        if (sock < 0) {
            ESP_LOGE(TAG, "accept failed: %d", errno);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        int ka = 1;
        setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &ka, sizeof(ka));
        struct timeval tmo = {.tv_sec = 5, .tv_usec = 0};
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tmo, sizeof(tmo));
        lock();
        mqtt_session_t *sess = alloc_session();
        size_t slot = session_index(sess);
        unlock();
        if (!sess) {
            ESP_LOGW(TAG, "too many clients");
            shutdown(sock, SHUT_RDWR);
            closesocket(sock);
            continue;
        }
        if (slot >= MQTT_MAX_CLIENTS || !ensure_session_task_storage(slot)) {
            ESP_LOGE(TAG, "no memory for client task");
            shutdown(sock, SHUT_RDWR);
            closesocket(sock);
            lock();
            free_session(sess);
            unlock();
            continue;
        }
        sess->sock = sock;
        TaskHandle_t task = xTaskCreateStatic(handle_client, "mqtt_client", MQTT_CLIENT_STACK, sess, 5,
                                              s_session_stacks[slot], s_session_tcbs[slot]);
        if (!task) {
            ESP_LOGE(TAG, "failed to start client task");
            shutdown(sock, SHUT_RDWR);
            closesocket(sock);
            lock();
            free_session(sess);
            unlock();
            continue;
        }
        sess->task = task;
    }
}

esp_err_t mqtt_core_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    if (!s_sessions) {
        s_sessions = heap_caps_calloc(MQTT_MAX_CLIENTS, sizeof(mqtt_session_t),
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_sessions) {
            ESP_LOGE(TAG, "failed to allocate sessions in PSRAM");
            return ESP_ERR_NO_MEM;
        }
    }
    if (!s_retain) {
        s_retain = heap_caps_calloc(MQTT_RETAIN_MAX, sizeof(retain_entry_t),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_retain) {
            ESP_LOGE(TAG, "failed to allocate retain table in PSRAM");
            return ESP_ERR_NO_MEM;
        }
    }
    ESP_ERROR_CHECK(event_bus_register_handler(on_event_bus_message));
    return ESP_OK;
}

esp_err_t mqtt_core_start(void)
{
    const app_config_t *cfg = config_store_get();
    int port = cfg->mqtt.port;
    s_listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (s_listen_sock < 0) {
        return ESP_FAIL;
    }
    int opt = 1;
    setsockopt(s_listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(s_listen_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "bind failed");
        closesocket(s_listen_sock);
        return ESP_FAIL;
    }
    if (listen(s_listen_sock, MQTT_MAX_CLIENTS) != 0) {
        ESP_LOGE(TAG, "listen failed");
        closesocket(s_listen_sock);
        return ESP_FAIL;
    }
    // Periodic sweep of idle sessions
    if (!s_sweep_timer) {
        const esp_timer_create_args_t args = {
            .callback = (esp_timer_cb_t)sweep_idle_sessions,
            .name = "mqtt_sweep",
        };
        esp_timer_create(&args, &s_sweep_timer);
        esp_timer_start_periodic(s_sweep_timer, 10 * 1000 * 1000); // 10s
    }
    if (!ensure_accept_task_storage()) {
        ESP_LOGE(TAG, "failed to allocate accept task stack");
        closesocket(s_listen_sock);
        s_listen_sock = -1;
        return ESP_ERR_NO_MEM;
    }
    s_accept_task = xTaskCreateStatic(accept_task, "mqtt_accept", MQTT_ACCEPT_STACK, NULL, 5,
                                      s_accept_stack, s_accept_tcb);
    if (!s_accept_task) {
        ESP_LOGE(TAG, "failed to create accept task");
        closesocket(s_listen_sock);
        s_listen_sock = -1;
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "MQTT broker started on %d", port);
    return ESP_OK;
}

esp_err_t mqtt_core_publish(const char *topic, const char *payload)
{
    if (!topic || !payload) {
        return ESP_ERR_INVALID_ARG;
    }
    publish_to_subscribers(topic, payload, 0, false, NULL);
    return ESP_OK;
}

esp_err_t mqtt_core_inject_message(const char *topic, const char *payload)
{
    if (!topic || !payload) {
        return ESP_ERR_INVALID_ARG;
    }
    event_bus_type_t type = find_type_by_topic(topic);
    if (type != EVENT_NONE) {
        event_bus_message_t typed = {
            .type = type,
        };
        strncpy(typed.topic, topic, sizeof(typed.topic) - 1);
        strncpy(typed.payload, payload, sizeof(typed.payload) - 1);
#if MQTT_CORE_DEBUG
        ESP_LOGI(TAG, "[MQTT IN] %s -> event %d", topic, type);
#endif
        event_bus_post(&typed, pdMS_TO_TICKS(100));
    }

    event_bus_message_t generic = {
        .type = EVENT_MQTT_MESSAGE,
    };
    strncpy(generic.topic, topic, sizeof(generic.topic) - 1);
    strncpy(generic.payload, payload, sizeof(generic.payload) - 1);
    return event_bus_post(&generic, pdMS_TO_TICKS(100));
}
