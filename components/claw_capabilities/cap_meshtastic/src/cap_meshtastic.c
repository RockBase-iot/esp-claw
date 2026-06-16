/*
 * SPDX-FileCopyrightText: 2026 Chengdu RockBase Technology Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_meshtastic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "cJSON.h"
#include "claw_cap.h"
#include "claw_event_publisher.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "meshtastic_proto.h"
#include "meshtastic_store.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "cap_meshtastic";

#define CAP_MESHTASTIC_GROUP_ID "cap_meshtastic"
#define CAP_MESHTASTIC_CHANNEL "meshtastic"
#define CAP_MESHTASTIC_SOURCE_CAP "cap_meshtastic"
/* Event types published to the router for proactive IM delivery. */
#define CAP_MESHTASTIC_EVENT_INBOUND "meshtastic_inbound"
#define CAP_MESHTASTIC_EVENT_NODE_UPDATE "meshtastic_node_update"

#define CAP_MESHTASTIC_MAX_NODES 32
#define CAP_MESHTASTIC_MAX_MESSAGES 16
/* NVS namespace used to persist per-IM push preferences. */
#define CAP_MESHTASTIC_NVS_NS "claw_mesh_im"
#define CAP_MESHTASTIC_NVS_KEY_EN "en_mask"
#define CAP_MESHTASTIC_UART_RX_BUF 2048
#define CAP_MESHTASTIC_READ_CHUNK 256
#define CAP_MESHTASTIC_TASK_STACK 6144
/*
 * Keep this BELOW the IM gateway / HTTP server tasks (priority 5) and the
 * lwIP/Wi-Fi tasks. On the single-core ESP32-C5 a higher-priority UART polling
 * task would starve networking and IM while the radio streams, killing the web
 * UI and IM connectivity. The loop also yields explicitly under sustained data.
 */
#define CAP_MESHTASTIC_TASK_PRIO 3
#define CAP_MESHTASTIC_HEARTBEAT_MS 60000
/* Re-issue want_config until the radio answers, in case it booted late or the
 * first request was lost. The Stream API only starts streaming after it sees a
 * valid want_config_id, so a single startup request is not robust. After a few
 * fast attempts we back off to avoid hammering the radio (and ourselves) with
 * full node-DB dumps when the link cannot be established. */
#define CAP_MESHTASTIC_WANT_CONFIG_RETRY_MS 5000
#define CAP_MESHTASTIC_WANT_CONFIG_BACKOFF_MS 30000
#define CAP_MESHTASTIC_WANT_CONFIG_FAST_TRIES 6
/* Periodically log RX statistics so wiring/baud/mode problems are diagnosable. */
#define CAP_MESHTASTIC_RX_STAT_LOG_MS 15000

/* Kconfig fallbacks so the component builds even without project config. */
#ifndef CONFIG_CAP_MESHTASTIC_UART_PORT
#define CONFIG_CAP_MESHTASTIC_UART_PORT 1
#endif
#ifndef CONFIG_CAP_MESHTASTIC_TX_GPIO
#define CONFIG_CAP_MESHTASTIC_TX_GPIO 5
#endif
#ifndef CONFIG_CAP_MESHTASTIC_RX_GPIO
#define CONFIG_CAP_MESHTASTIC_RX_GPIO 4
#endif
#ifndef CONFIG_CAP_MESHTASTIC_BAUD
#define CONFIG_CAP_MESHTASTIC_BAUD 115200
#endif

typedef struct {
    bool used;
    uint32_t num;
    char id[16];
    char long_name[40];
    char short_name[8];
    uint32_t hw_model;
    uint32_t role;
    bool has_position;
    meshtastic_position_t position;
    bool has_metrics;
    meshtastic_device_metrics_t metrics;
    bool has_snr;
    float snr;
    uint32_t last_heard;
    int64_t updated_ms;
} cap_meshtastic_node_t;

typedef struct {
    bool used;
    uint32_t from;
    char from_id[16];
    uint32_t channel;
    uint32_t packet_id;
    char text[240];
    int64_t received_ms;
} cap_meshtastic_message_t;

/* Stream API receive state machine. */
typedef enum {
    RX_WAIT_START1 = 0,
    RX_WAIT_START2,
    RX_LEN_HI,
    RX_LEN_LO,
    RX_PAYLOAD,
} cap_meshtastic_rx_state_t;

static struct {
    cap_meshtastic_uart_config_t uart;
    bool uart_installed;
    bool running;
    TaskHandle_t task;
    SemaphoreHandle_t lock;
    SemaphoreHandle_t tx_lock;

    bool connected;
    bool config_complete;
    uint32_t want_config_id;
    meshtastic_mynodeinfo_t my_info;
    meshtastic_metadata_t metadata;
    bool has_metadata;

    /* Diagnostics: help distinguish wiring/baud/mode faults from parse faults. */
    uint32_t rx_bytes;
    uint32_t frames_decoded;
    uint32_t frames_failed;     /* protobuf decode failures */
    uint32_t packets_received;  /* FromRadio.packet count */
    uint32_t packets_encrypted; /* has_decoded == false */
    uint32_t config_requests;
    int64_t last_rx_ms;

    /* Proactive IM notification target for inbound mesh traffic. */
    char notify_channel[16];
    char notify_chat_id[96];
    bool notify_target_explicit;   /* set via API -> never overwritten by auto-learn */

    /* Per-IM push preferences (parallel to s_im_channel_names). When a channel
     * is enabled and has a learned conversation, inbound mesh messages are
     * forwarded to it via the event router. */
    bool im_push_enabled[CAP_MESHTASTIC_IM_PUSH_MAX];
    char im_push_chat[CAP_MESHTASTIC_IM_PUSH_MAX][96];
    bool im_push_loaded;

    cap_meshtastic_node_t nodes[CAP_MESHTASTIC_MAX_NODES];
    cap_meshtastic_message_t messages[CAP_MESHTASTIC_MAX_MESSAGES];
    size_t message_head;     /* next write slot */
    size_t message_count;

    /* RX assembly state */
    cap_meshtastic_rx_state_t rx_state;
    uint16_t rx_expected;
    uint16_t rx_filled;
    uint8_t rx_buf[MESHTASTIC_FRAME_MAX_PAYLOAD];
} s_ctx = {
    .uart = {
        .uart_port = CONFIG_CAP_MESHTASTIC_UART_PORT,
        .tx_gpio = CONFIG_CAP_MESHTASTIC_TX_GPIO,
        .rx_gpio = CONFIG_CAP_MESHTASTIC_RX_GPIO,
        .baud_rate = CONFIG_CAP_MESHTASTIC_BAUD,
    },
};

/* ===================================================================== */
/* Wall-clock + IM push helpers                                          */
/* ===================================================================== */

/* Canonical IM channels we can push inbound mesh messages to. Index order is
 * stable and used as the NVS bit/key index, so never reorder. */
static const char *const s_im_channel_names[CAP_MESHTASTIC_IM_PUSH_MAX] = {
    "feishu", "qq", "telegram", "wechat",
};

static int im_channel_index(const char *channel)
{
    if (!channel) {
        return -1;
    }
    for (int i = 0; i < CAP_MESHTASTIC_IM_PUSH_MAX; i++) {
        if (strcmp(channel, s_im_channel_names[i]) == 0) {
            return i;
        }
    }
    return -1;
}

/*
 * Current wall-clock time in epoch milliseconds. Falls back to monotonic boot
 * time if the system clock has not been set yet (SNTP not synced), so message
 * ordering still works; once time is synced the stored timestamps are real
 * epoch values the web UI can render correctly.
 */
static int64_t cap_meshtastic_now_ms(void)
{
    struct timeval tv;
    if (gettimeofday(&tv, NULL) == 0 && tv.tv_sec > 1609459200 /* 2021-01-01 */) {
        return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
    }
    return esp_timer_get_time() / 1000;
}

/* Load persisted IM push preferences from NVS (best effort). */
static void im_push_load(void)
{
    if (s_ctx.im_push_loaded) {
        return;
    }
    s_ctx.im_push_loaded = true;

    nvs_handle_t h;
    if (nvs_open(CAP_MESHTASTIC_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint8_t mask = 0;
    nvs_get_u8(h, CAP_MESHTASTIC_NVS_KEY_EN, &mask);
    for (int i = 0; i < CAP_MESHTASTIC_IM_PUSH_MAX; i++) {
        s_ctx.im_push_enabled[i] = (mask >> i) & 0x1;
        char key[8];
        snprintf(key, sizeof(key), "cid%d", i);
        size_t len = sizeof(s_ctx.im_push_chat[i]);
        if (nvs_get_str(h, key, s_ctx.im_push_chat[i], &len) != ESP_OK) {
            s_ctx.im_push_chat[i][0] = '\0';
        }
    }
    nvs_close(h);
}

/* Persist the enabled bitmask to NVS (best effort). */
static void im_push_save_enabled(void)
{
    nvs_handle_t h;
    if (nvs_open(CAP_MESHTASTIC_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    uint8_t mask = 0;
    for (int i = 0; i < CAP_MESHTASTIC_IM_PUSH_MAX; i++) {
        if (s_ctx.im_push_enabled[i]) {
            mask |= (uint8_t)(1u << i);
        }
    }
    if (nvs_set_u8(h, CAP_MESHTASTIC_NVS_KEY_EN, mask) == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
}

/* Persist a single channel's learned conversation id to NVS (best effort). */
static void im_push_save_chat(int idx)
{
    if (idx < 0 || idx >= CAP_MESHTASTIC_IM_PUSH_MAX) {
        return;
    }
    nvs_handle_t h;
    if (nvs_open(CAP_MESHTASTIC_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    char key[8];
    snprintf(key, sizeof(key), "cid%d", idx);
    if (nvs_set_str(h, key, s_ctx.im_push_chat[idx]) == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
}

/* ===================================================================== */
/* Node database helpers (caller must hold s_ctx.lock)                   */
/* ===================================================================== */

static cap_meshtastic_node_t *node_find_locked(uint32_t num)
{
    for (size_t i = 0; i < CAP_MESHTASTIC_MAX_NODES; i++) {
        if (s_ctx.nodes[i].used && s_ctx.nodes[i].num == num) {
            return &s_ctx.nodes[i];
        }
    }
    return NULL;
}

static cap_meshtastic_node_t *node_get_or_create_locked(uint32_t num, bool *created)
{
    if (created) {
        *created = false;
    }
    cap_meshtastic_node_t *node = node_find_locked(num);
    if (node) {
        return node;
    }

    /* Find a free slot, else evict the least-recently updated entry. */
    cap_meshtastic_node_t *target = NULL;
    int64_t oldest = INT64_MAX;
    for (size_t i = 0; i < CAP_MESHTASTIC_MAX_NODES; i++) {
        if (!s_ctx.nodes[i].used) {
            target = &s_ctx.nodes[i];
            break;
        }
        if (s_ctx.nodes[i].updated_ms < oldest) {
            oldest = s_ctx.nodes[i].updated_ms;
            target = &s_ctx.nodes[i];
        }
    }
    if (!target) {
        return NULL;
    }
    memset(target, 0, sizeof(*target));
    target->used = true;
    target->num = num;
    meshtastic_format_node_id(num, target->id, sizeof(target->id));
    if (created) {
        *created = true;
    }
    return target;
}

static void node_touch_locked(cap_meshtastic_node_t *node, uint32_t rx_time)
{
    node->updated_ms = esp_timer_get_time() / 1000;
    if (rx_time) {
        node->last_heard = rx_time;
    }
}

/* ===================================================================== */
/* Inbound handling                                                      */
/* ===================================================================== */

/*
 * Publish one inbound-mesh notification event to a specific IM conversation.
 * Builds a self-contained claw_event_t carrying the target channel/chat so the
 * default `meshtastic_inbound_im_notify` rule can forward `{{event.text}}`
 * without invoking the LLM.
 */
static void meshtastic_publish_to(const char *channel, const char *chat_id,
                                  const char *event_type, const char *text)
{
    if (!channel || !channel[0] || !chat_id || !chat_id[0]) {
        return;
    }

    /* Heap-allocate the (large) event instead of placing it on the RX task
     * stack: this runs at the bottom of the deep parse->handle->publish call
     * chain, and claw_event_t is ~800 B. Keeping it off the stack prevents RX
     * task stack overflow. */
    claw_event_t *event = calloc(1, sizeof(*event));
    if (!event) {
        ESP_LOGW(TAG, "IM notify (%s) dropped: out of memory", event_type);
        return;
    }
    strlcpy(event->source_cap, CAP_MESHTASTIC_SOURCE_CAP, sizeof(event->source_cap));
    strlcpy(event->event_type, event_type, sizeof(event->event_type));
    strlcpy(event->source_channel, CAP_MESHTASTIC_CHANNEL, sizeof(event->source_channel));
    strlcpy(event->target_channel, channel, sizeof(event->target_channel));
    strlcpy(event->chat_id, chat_id, sizeof(event->chat_id));
    strlcpy(event->content_type, "text", sizeof(event->content_type));
    event->session_policy = CLAW_EVENT_SESSION_POLICY_NOSAVE;
    event->timestamp_ms = cap_meshtastic_now_ms();
    event->text = (char *)text;   /* publish() clones the event, including text */

    esp_err_t err = claw_event_router_publish(event);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "publish IM notify (%s->%s) failed: %s",
                 event_type, channel, esp_err_to_name(err));
    }
    free(event);
}

/*
 * Dispatch an inbound-mesh notification to every IM channel the user explicitly
 * enabled for push (and that has a known conversation). The per-channel toggles
 * are the single source of truth: if no channel is enabled, nothing is sent, so
 * unchecking a channel reliably stops mesh messages from reaching that IM. The
 * message itself is still buffered and retrievable via the HTTP API regardless.
 */
static void meshtastic_dispatch_im_notify(const char *event_type, const char *text)
{
    char channels[CAP_MESHTASTIC_IM_PUSH_MAX][16];
    char chats[CAP_MESHTASTIC_IM_PUSH_MAX][96];
    int count = 0;

    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    for (int i = 0; i < CAP_MESHTASTIC_IM_PUSH_MAX; i++) {
        if (s_ctx.im_push_enabled[i] && s_ctx.im_push_chat[i][0]) {
            strlcpy(channels[count], s_im_channel_names[i], sizeof(channels[count]));
            strlcpy(chats[count], s_ctx.im_push_chat[i], sizeof(chats[count]));
            count++;
        }
    }
    xSemaphoreGive(s_ctx.lock);

    if (count == 0) {
        /* No IM channel is enabled for push (or none has a known conversation
         * yet); honour the user's choice and do not forward. The message stays
         * buffered and is retrievable via the HTTP API / meshtastic_get_messages. */
        ESP_LOGD(TAG, "no IM push channel enabled; skipping push for %s", event_type);
        return;
    }

    for (int i = 0; i < count; i++) {
        meshtastic_publish_to(channels[i], chats[i], event_type, text);
    }
}

static void store_message(uint32_t from, const char *from_id, uint32_t channel,
                          uint32_t packet_id, const char *text)
{
    int64_t ts_ms = cap_meshtastic_now_ms();

    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    cap_meshtastic_message_t *slot = &s_ctx.messages[s_ctx.message_head];
    memset(slot, 0, sizeof(*slot));
    slot->used = true;
    slot->from = from;
    strlcpy(slot->from_id, from_id, sizeof(slot->from_id));
    slot->channel = channel;
    slot->packet_id = packet_id;
    strlcpy(slot->text, text, sizeof(slot->text));
    slot->received_ms = ts_ms;
    s_ctx.message_head = (s_ctx.message_head + 1) % CAP_MESHTASTIC_MAX_MESSAGES;
    if (s_ctx.message_count < CAP_MESHTASTIC_MAX_MESSAGES) {
        s_ctx.message_count++;
    }
    xSemaphoreGive(s_ctx.lock);

    /* Persist to JSONL store (non-fatal on failure). */
    if (meshtastic_store_is_ready()) {
        esp_err_t err = meshtastic_store_append(from, from_id, channel,
                                                 packet_id, text, ts_ms);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "store append failed: %s", esp_err_to_name(err));
        }
    }
}

static void handle_text_message(const meshtastic_meshpacket_t *pkt)
{
    char text[240] = {0};
    size_t copy = pkt->decoded.payload_len;
    if (copy >= sizeof(text)) {
        copy = sizeof(text) - 1;
    }
    if (pkt->decoded.payload && copy) {
        memcpy(text, pkt->decoded.payload, copy);
    }
    text[copy] = '\0';

    char from_id[16];
    meshtastic_format_node_id(pkt->from, from_id, sizeof(from_id));

    ESP_LOGI(TAG, "text from %s (ch %u): %s", from_id,
             (unsigned int)pkt->channel, text);

    store_message(pkt->from, from_id, pkt->channel, pkt->id, text);

    /* Resolve a friendly sender label (long name if known, else node id). */
    char sender_label[40];
    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    cap_meshtastic_node_t *node = node_find_locked(pkt->from);
    if (node && node->long_name[0]) {
        strlcpy(sender_label, node->long_name, sizeof(sender_label));
    } else {
        strlcpy(sender_label, from_id, sizeof(sender_label));
    }
    xSemaphoreGive(s_ctx.lock);

    /* Push the message to every IM conversation the user enabled for mesh
     * notifications (falls back to the last active conversation). */
    char notify[320];
    snprintf(notify, sizeof(notify), "\xF0\x9F\x93\xA1 Mesh %s (ch %u): %s",
             sender_label, (unsigned int)pkt->channel, text);
    meshtastic_dispatch_im_notify(CAP_MESHTASTIC_EVENT_INBOUND, notify);
}

static void apply_packet_payload(const meshtastic_meshpacket_t *pkt)
{
    if (!pkt->has_decoded) {
        s_ctx.packets_encrypted++;
        ESP_LOGW(TAG, "packet from 0x%08x: encrypted (has_decoded=false), dropping",
                 (unsigned int)pkt->from);
        return;
    }

    switch (pkt->decoded.portnum) {
    case MESHTASTIC_PORTNUM_TEXT_MESSAGE:
        handle_text_message(pkt);
        break;
    case MESHTASTIC_PORTNUM_POSITION: {
        meshtastic_position_t pos;
        meshtastic_decode_position_payload(pkt->decoded.payload,
                                           pkt->decoded.payload_len, &pos);
        xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
        cap_meshtastic_node_t *node = node_get_or_create_locked(pkt->from, NULL);
        if (node && pos.valid) {
            node->has_position = true;
            node->position = pos;
        }
        xSemaphoreGive(s_ctx.lock);
        break;
    }
    case MESHTASTIC_PORTNUM_NODEINFO: {
        meshtastic_user_t user;
        meshtastic_decode_user_payload(pkt->decoded.payload,
                                       pkt->decoded.payload_len, &user);
        xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
        cap_meshtastic_node_t *node = node_get_or_create_locked(pkt->from, NULL);
        if (node && user.valid) {
            strlcpy(node->long_name, user.long_name, sizeof(node->long_name));
            strlcpy(node->short_name, user.short_name, sizeof(node->short_name));
            if (user.id[0]) {
                strlcpy(node->id, user.id, sizeof(node->id));
            }
            node->hw_model = user.hw_model;
            node->role = user.role;
        }
        xSemaphoreGive(s_ctx.lock);
        break;
    }
    case MESHTASTIC_PORTNUM_TELEMETRY: {
        meshtastic_device_metrics_t metrics;
        bool has_metrics = false;
        meshtastic_decode_telemetry_payload(pkt->decoded.payload,
                                            pkt->decoded.payload_len,
                                            &metrics, &has_metrics);
        if (has_metrics) {
            xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
            cap_meshtastic_node_t *node = node_get_or_create_locked(pkt->from, NULL);
            if (node) {
                node->has_metrics = true;
                node->metrics = metrics;
            }
            xSemaphoreGive(s_ctx.lock);
        }
        break;
    }
    default:
        ESP_LOGD(TAG, "packet from 0x%08x portnum %s",
                 (unsigned int)pkt->from,
                 meshtastic_portnum_to_string(pkt->decoded.portnum));
        break;
    }
}

static void handle_packet(const meshtastic_meshpacket_t *pkt)
{
    s_ctx.packets_received++;
    ESP_LOGI(TAG, "FromRadio.packet: from=0x%08x to=0x%08x ch=%u decoded=%d portnum=%s",
             (unsigned int)pkt->from, (unsigned int)pkt->to, (unsigned int)pkt->channel,
             pkt->has_decoded,
             pkt->has_decoded ? meshtastic_portnum_to_string(pkt->decoded.portnum) : "encrypted");
    if (pkt->from) {
        xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
        cap_meshtastic_node_t *node = node_get_or_create_locked(pkt->from, NULL);
        if (node) {
            if (pkt->rx_snr != 0.0f) {
                node->has_snr = true;
                node->snr = pkt->rx_snr;
            }
            node_touch_locked(node, pkt->rx_time);
        }
        xSemaphoreGive(s_ctx.lock);
    }

    apply_packet_payload(pkt);
}

static void handle_nodeinfo(const meshtastic_nodeinfo_t *ni)
{
    bool created = false;
    bool config_complete;
    char label[40] = {0};

    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    config_complete = s_ctx.config_complete;
    cap_meshtastic_node_t *node = node_get_or_create_locked(ni->num, &created);
    if (node) {
        if (ni->user.valid) {
            if (ni->user.id[0]) {
                strlcpy(node->id, ni->user.id, sizeof(node->id));
            }
            strlcpy(node->long_name, ni->user.long_name, sizeof(node->long_name));
            strlcpy(node->short_name, ni->user.short_name, sizeof(node->short_name));
            node->hw_model = ni->user.hw_model;
            node->role = ni->user.role;
        }
        if (ni->position.valid) {
            node->has_position = true;
            node->position = ni->position;
        }
        if (ni->has_metrics) {
            node->has_metrics = true;
            node->metrics = ni->metrics;
        }
        if (ni->has_snr) {
            node->has_snr = true;
            node->snr = ni->snr;
        }
        node_touch_locked(node, ni->last_heard);
        strlcpy(label, node->long_name[0] ? node->long_name : node->id, sizeof(label));
    }
    xSemaphoreGive(s_ctx.lock);
    ESP_LOGI(TAG, "node 0x%08x (%s) updated", (unsigned int)ni->num,
             ni->user.long_name[0] ? ni->user.long_name : "?");

    /* Announce nodes that appear after the initial config sync so the user is
     * told when the mesh topology changes, without spamming on every boot. */
    if (created && config_complete && label[0]) {
        char notify[120];
        snprintf(notify, sizeof(notify), "\xF0\x9F\x9F\xA2 Mesh node joined: %s", label);
        meshtastic_dispatch_im_notify(CAP_MESHTASTIC_EVENT_NODE_UPDATE, notify);
    }
}

static void handle_fromradio(const meshtastic_fromradio_t *fr)
{
    switch (fr->which) {
    case MESHTASTIC_FROMRADIO_PACKET:
        handle_packet(&fr->packet);
        break;
    case MESHTASTIC_FROMRADIO_MY_INFO:
        xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
        s_ctx.my_info = fr->my_info;
        s_ctx.connected = true;
        xSemaphoreGive(s_ctx.lock);
        ESP_LOGI(TAG, "my_node_num=0x%08x", (unsigned int)fr->my_info.my_node_num);
        break;
    case MESHTASTIC_FROMRADIO_NODE_INFO:
        handle_nodeinfo(&fr->node_info);
        break;
    case MESHTASTIC_FROMRADIO_METADATA:
        xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
        s_ctx.metadata = fr->metadata;
        s_ctx.has_metadata = true;
        xSemaphoreGive(s_ctx.lock);
        ESP_LOGI(TAG, "firmware %s", fr->metadata.firmware_version);
        break;
    case MESHTASTIC_FROMRADIO_CONFIG_COMPLETE_ID:
        xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
        if (fr->config_complete_id == s_ctx.want_config_id) {
            s_ctx.config_complete = true;
        }
        xSemaphoreGive(s_ctx.lock);
        ESP_LOGI(TAG, "config complete id=%u", (unsigned int)fr->config_complete_id);
        break;
    default:
        break;
    }
}

/* ===================================================================== */
/* UART transport                                                        */
/* ===================================================================== */

static esp_err_t uart_write_frame(const uint8_t *frame, size_t len)
{
    if (!s_ctx.uart_installed) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_ctx.tx_lock, portMAX_DELAY);
    int written = uart_write_bytes(s_ctx.uart.uart_port, frame, len);
    xSemaphoreGive(s_ctx.tx_lock);
    return (written == (int)len) ? ESP_OK : ESP_FAIL;
}

static void rx_feed_byte(uint8_t b)
{
    switch (s_ctx.rx_state) {
    case RX_WAIT_START1:
        if (b == MESHTASTIC_FRAME_START1) {
            s_ctx.rx_state = RX_WAIT_START2;
        }
        break;
    case RX_WAIT_START2:
        if (b == MESHTASTIC_FRAME_START2) {
            s_ctx.rx_state = RX_LEN_HI;
        } else if (b == MESHTASTIC_FRAME_START1) {
            s_ctx.rx_state = RX_WAIT_START2;
        } else {
            s_ctx.rx_state = RX_WAIT_START1;
        }
        break;
    case RX_LEN_HI:
        s_ctx.rx_expected = (uint16_t)b << 8;
        s_ctx.rx_state = RX_LEN_LO;
        break;
    case RX_LEN_LO:
        s_ctx.rx_expected |= b;
        s_ctx.rx_filled = 0;
        if (s_ctx.rx_expected == 0 || s_ctx.rx_expected > MESHTASTIC_FRAME_MAX_PAYLOAD) {
            /* Length out of range: likely debug text, resync. */
            s_ctx.rx_state = RX_WAIT_START1;
        } else {
            s_ctx.rx_state = RX_PAYLOAD;
        }
        break;
    case RX_PAYLOAD:
        s_ctx.rx_buf[s_ctx.rx_filled++] = b;
        if (s_ctx.rx_filled >= s_ctx.rx_expected) {
            /* Decode into a file-scope struct rather than the stack: this
             * function runs in the deep RX call chain (parse -> handle ->
             * publish) on a single task, and meshtastic_fromradio_t is large
             * (~250 B). Keeping it off the stack avoids overflowing the RX
             * task stack (which previously could corrupt adjacent task memory
             * and silently wedge networking/IM). Only ever touched by the one
             * RX task, so a static is safe here. */
            static meshtastic_fromradio_t s_decode_fr;
            if (meshtastic_decode_fromradio(s_ctx.rx_buf, s_ctx.rx_expected, &s_decode_fr) == ESP_OK) {
                s_ctx.frames_decoded++;
                handle_fromradio(&s_decode_fr);
            } else {
                s_ctx.frames_failed++;
                ESP_LOGW(TAG, "frame decode failed (len=%u, total_fail=%u)",
                         (unsigned)s_ctx.rx_expected, (unsigned)s_ctx.frames_failed);
            }
            s_ctx.rx_state = RX_WAIT_START1;
        }
        break;
    default:
        s_ctx.rx_state = RX_WAIT_START1;
        break;
    }
}

esp_err_t cap_meshtastic_request_config(void)
{
    uint8_t frame[16];
    size_t frame_len = 0;

    uint32_t config_id = esp_random();
    if (config_id == 0) {
        config_id = 1;
    }

    esp_err_t err = meshtastic_encode_want_config_frame(config_id, frame,
                                                        sizeof(frame), &frame_len);
    if (err != ESP_OK) {
        return err;
    }

    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    s_ctx.want_config_id = config_id;
    s_ctx.config_complete = false;
    s_ctx.config_requests++;
    xSemaphoreGive(s_ctx.lock);

    return uart_write_frame(frame, frame_len);
}

static void cap_meshtastic_task(void *arg)
{
    (void)arg;
    uint8_t chunk[CAP_MESHTASTIC_READ_CHUNK];
    int64_t last_heartbeat_ms = 0;
    int64_t last_want_config_ms = 0;
    int64_t last_stat_log_ms = 0;

    /* Give the radio a moment to boot, then start the config handshake. */
    vTaskDelay(pdMS_TO_TICKS(500));

    while (s_ctx.running) {
        int n = uart_read_bytes(s_ctx.uart.uart_port, chunk, sizeof(chunk),
                                pdMS_TO_TICKS(50));
        if (n > 0) {
            s_ctx.rx_bytes += (uint32_t)n;
            s_ctx.last_rx_ms = esp_timer_get_time() / 1000;
            for (int i = 0; i < n; i++) {
                rx_feed_byte(chunk[i]);
            }
            /* When a full chunk is read the radio is mid-burst (e.g. a node-DB
             * dump) and uart_read_bytes would return immediately again. Yield a
             * tick so lower/equal-priority tasks (and the idle task) still run
             * on this single-core part, preventing CPU starvation. */
            if (n == (int)sizeof(chunk)) {
                vTaskDelay(1);
            }
        }

        int64_t now_ms = esp_timer_get_time() / 1000;

        /* Keep requesting config until the radio answers. The Stream API only
         * starts streaming after it accepts a want_config_id, so this is what
         * actually establishes the link. Back off after a few fast tries to
         * avoid flooding the link (and our parser) when it never connects. */
        bool config_done;
        xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
        config_done = s_ctx.config_complete;
        xSemaphoreGive(s_ctx.lock);
        uint32_t want_interval = (s_ctx.config_requests < CAP_MESHTASTIC_WANT_CONFIG_FAST_TRIES)
                                 ? CAP_MESHTASTIC_WANT_CONFIG_RETRY_MS
                                 : CAP_MESHTASTIC_WANT_CONFIG_BACKOFF_MS;
        if (!config_done && now_ms - last_want_config_ms >= want_interval) {
            cap_meshtastic_request_config();
            last_want_config_ms = now_ms;
        }

        /* Periodically log RX stats so wiring/baud/mode faults are visible. */
        if (now_ms - last_stat_log_ms >= CAP_MESHTASTIC_RX_STAT_LOG_MS) {
            uint32_t unknown_count = 0;
            uint32_t unknown_last_tag = 0;
            meshtastic_get_fromradio_unknown_stats(&unknown_count, &unknown_last_tag);
            if (!config_done) {
                ESP_LOGW(TAG,
                         "not connected: rx_bytes=%u frames=%u cfg_reqs=%u "
                         "fr_unknown=%u last_unknown_tag=%u "
                         "(check baud=%d, serial.mode=PROTO, TX<->RX crossover on GPIO%d/%d)",
                         (unsigned)s_ctx.rx_bytes, (unsigned)s_ctx.frames_decoded,
                         (unsigned)s_ctx.config_requests,
                         (unsigned)unknown_count, (unsigned)unknown_last_tag,
                         s_ctx.uart.baud_rate,
                         s_ctx.uart.tx_gpio, s_ctx.uart.rx_gpio);
            } else {
                size_t node_count = 0;
                for (size_t i = 0; i < CAP_MESHTASTIC_MAX_NODES; i++) {
                    if (s_ctx.nodes[i].used) {
                        node_count++;
                    }
                }
                int64_t since_rx = now_ms - s_ctx.last_rx_ms;
                ESP_LOGI(TAG,
                         "stat: rx_bytes=%u frames=%u fail=%u pkts=%u encrypted=%u "
                         "nodes=%u msgs=%u fr_unknown=%u last_unknown_tag=%u "
                         "last_rx=%lldms_ago",
                         (unsigned)s_ctx.rx_bytes, (unsigned)s_ctx.frames_decoded,
                         (unsigned)s_ctx.frames_failed, (unsigned)s_ctx.packets_received,
                         (unsigned)s_ctx.packets_encrypted,
                         (unsigned)node_count, (unsigned)s_ctx.message_count,
                         (unsigned)unknown_count, (unsigned)unknown_last_tag,
                         (long long)since_rx);
            }
            last_stat_log_ms = now_ms;
        }

        if (now_ms - last_heartbeat_ms >= CAP_MESHTASTIC_HEARTBEAT_MS) {
            uint8_t hb[8];
            size_t hb_len = 0;
            if (meshtastic_encode_heartbeat_frame(hb, sizeof(hb), &hb_len) == ESP_OK) {
                uart_write_frame(hb, hb_len);
            }
            last_heartbeat_ms = now_ms;
        }
    }

    s_ctx.task = NULL;
    vTaskDelete(NULL);
}

static esp_err_t cap_meshtastic_uart_start(void)
{
    if (s_ctx.uart_installed) {
        return ESP_OK;
    }

    uart_config_t uart_config = {
        .baud_rate = s_ctx.uart.baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_driver_install(s_ctx.uart.uart_port,
                                        CAP_MESHTASTIC_UART_RX_BUF, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }
    err = uart_param_config(s_ctx.uart.uart_port, &uart_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(err));
        uart_driver_delete(s_ctx.uart.uart_port);
        return err;
    }
    err = uart_set_pin(s_ctx.uart.uart_port, s_ctx.uart.tx_gpio, s_ctx.uart.rx_gpio,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin failed: %s", esp_err_to_name(err));
        uart_driver_delete(s_ctx.uart.uart_port);
        return err;
    }

    s_ctx.uart_installed = true;
    ESP_LOGI(TAG, "UART%d started (tx=%d rx=%d baud=%d)", s_ctx.uart.uart_port,
             s_ctx.uart.tx_gpio, s_ctx.uart.rx_gpio, s_ctx.uart.baud_rate);
    return ESP_OK;
}

/* ===================================================================== */
/* Public send API                                                       */
/* ===================================================================== */

esp_err_t cap_meshtastic_send_text(uint32_t dest, uint32_t channel,
                                   bool want_ack, const char *text)
{
    if (!text || !text[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t frame[MESHTASTIC_FRAME_MAX_PAYLOAD + MESHTASTIC_FRAME_HEADER_LEN];
    size_t frame_len = 0;
    uint32_t id = esp_random();
    if (id == 0) {
        id = 1;
    }

    meshtastic_text_packet_t packet = {
        .to = dest,
        .channel = channel,
        .id = id,
        .want_ack = want_ack,
        .text = text,
    };

    esp_err_t err = meshtastic_encode_text_frame(&packet, frame, sizeof(frame), &frame_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "encode text failed: %s", esp_err_to_name(err));
        return err;
    }

    err = uart_write_frame(frame, frame_len);
    if (err == ESP_OK) {
        char dest_id[16];
        meshtastic_format_node_id(dest, dest_id, sizeof(dest_id));
        ESP_LOGI(TAG, "sent text to %s (ch %u): %s",
                 dest == MESHTASTIC_ADDR_BROADCAST ? "broadcast" : dest_id,
                 (unsigned int)channel, text);
    }
    return err;
}

bool cap_meshtastic_is_connected(void)
{
    bool connected;
    if (!s_ctx.lock) {
        return false;
    }
    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    connected = s_ctx.connected;
    xSemaphoreGive(s_ctx.lock);
    return connected;
}

esp_err_t cap_meshtastic_set_notify_target(const char *channel, const char *chat_id)
{
    if (!s_ctx.lock) {
        s_ctx.lock = xSemaphoreCreateMutex();
        if (!s_ctx.lock) {
            return ESP_ERR_NO_MEM;
        }
    }
    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    if (channel && channel[0] && chat_id && chat_id[0]) {
        strlcpy(s_ctx.notify_channel, channel, sizeof(s_ctx.notify_channel));
        strlcpy(s_ctx.notify_chat_id, chat_id, sizeof(s_ctx.notify_chat_id));
        s_ctx.notify_target_explicit = true;
    } else {
        s_ctx.notify_channel[0] = '\0';
        s_ctx.notify_chat_id[0] = '\0';
        s_ctx.notify_target_explicit = false;
    }
    xSemaphoreGive(s_ctx.lock);
    ESP_LOGI(TAG, "IM notify target set: channel=%s chat=%s",
             channel ? channel : "(none)", chat_id ? chat_id : "(none)");
    return ESP_OK;
}

void cap_meshtastic_note_im_target(const char *channel, const char *chat_id)
{
    if (!channel || !channel[0] || !chat_id || !chat_id[0]) {
        return;
    }
    /* Do not auto-learn the device's own mesh channel. */
    if (strcmp(channel, CAP_MESHTASTIC_CHANNEL) == 0) {
        return;
    }
    if (!s_ctx.lock) {
        return;
    }
    int idx = im_channel_index(channel);
    bool chat_changed = false;
    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    if (!s_ctx.notify_target_explicit) {
        strlcpy(s_ctx.notify_channel, channel, sizeof(s_ctx.notify_channel));
        strlcpy(s_ctx.notify_chat_id, chat_id, sizeof(s_ctx.notify_chat_id));
    }
    /* Remember the latest conversation per IM channel so an enabled push has a
     * concrete destination even across reboots. */
    if (idx >= 0 && strcmp(s_ctx.im_push_chat[idx], chat_id) != 0) {
        strlcpy(s_ctx.im_push_chat[idx], chat_id, sizeof(s_ctx.im_push_chat[idx]));
        chat_changed = true;
    }
    xSemaphoreGive(s_ctx.lock);

    if (chat_changed) {
        im_push_save_chat(idx);
    }
}

size_t cap_meshtastic_get_im_push(cap_meshtastic_im_push_t *out, size_t max)
{
    if (!out || max == 0) {
        return 0;
    }
    size_t n = 0;
    bool locked = s_ctx.lock != NULL;
    if (locked) {
        xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    }
    for (int i = 0; i < CAP_MESHTASTIC_IM_PUSH_MAX && n < max; i++) {
        strlcpy(out[n].channel, s_im_channel_names[i], sizeof(out[n].channel));
        out[n].enabled = s_ctx.im_push_enabled[i];
        out[n].has_target = s_ctx.im_push_chat[i][0] != '\0';
        n++;
    }
    if (locked) {
        xSemaphoreGive(s_ctx.lock);
    }
    return n;
}

esp_err_t cap_meshtastic_set_im_push_enabled(const char *channel, bool enabled)
{
    int idx = im_channel_index(channel);
    if (idx < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    bool locked = s_ctx.lock != NULL;
    if (locked) {
        xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    }
    s_ctx.im_push_enabled[idx] = enabled;
    if (locked) {
        xSemaphoreGive(s_ctx.lock);
    }
    im_push_save_enabled();
    ESP_LOGI(TAG, "IM push %s for channel %s", enabled ? "enabled" : "disabled", channel);
    return ESP_OK;
}

/* ===================================================================== */
/* JSON helpers for tool output                                          */
/* ===================================================================== */

static void node_to_json(const cap_meshtastic_node_t *node, cJSON *obj)
{
    cJSON_AddStringToObject(obj, "id", node->id);
    cJSON_AddNumberToObject(obj, "num", node->num);
    if (node->long_name[0]) {
        cJSON_AddStringToObject(obj, "long_name", node->long_name);
    }
    if (node->short_name[0]) {
        cJSON_AddStringToObject(obj, "short_name", node->short_name);
    }
    if (node->hw_model) {
        cJSON_AddNumberToObject(obj, "hw_model", node->hw_model);
    }
    if (node->role) {
        cJSON_AddNumberToObject(obj, "role", node->role);
    }
    if (node->has_snr) {
        cJSON_AddNumberToObject(obj, "snr", node->snr);
    }
    if (node->last_heard) {
        cJSON_AddNumberToObject(obj, "last_heard", node->last_heard);
    }
    if (node->has_position && node->position.valid) {
        cJSON *pos = cJSON_AddObjectToObject(obj, "position");
        if (pos) {
            cJSON_AddNumberToObject(pos, "latitude", node->position.latitude_i * 1e-7);
            cJSON_AddNumberToObject(pos, "longitude", node->position.longitude_i * 1e-7);
            cJSON_AddNumberToObject(pos, "altitude", node->position.altitude);
        }
    }
    if (node->has_metrics) {
        cJSON *m = cJSON_AddObjectToObject(obj, "metrics");
        if (m) {
            if (node->metrics.has_battery_level) {
                cJSON_AddNumberToObject(m, "battery_level", node->metrics.battery_level);
            }
            if (node->metrics.has_voltage) {
                cJSON_AddNumberToObject(m, "voltage", node->metrics.voltage);
            }
            if (node->metrics.has_channel_utilization) {
                cJSON_AddNumberToObject(m, "channel_utilization",
                                        node->metrics.channel_utilization);
            }
            if (node->metrics.has_air_util_tx) {
                cJSON_AddNumberToObject(m, "air_util_tx", node->metrics.air_util_tx);
            }
            if (node->metrics.has_uptime_seconds) {
                cJSON_AddNumberToObject(m, "uptime_seconds", node->metrics.uptime_seconds);
            }
        }
    }
}

static void append_ram_messages(cJSON *arr, size_t max_count)
{
    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);

    size_t count = s_ctx.message_count;
    size_t start = (s_ctx.message_head + CAP_MESHTASTIC_MAX_MESSAGES - count) %
                   CAP_MESHTASTIC_MAX_MESSAGES;

    if (max_count > 0 && count > max_count) {
        start = (start + (count - max_count)) % CAP_MESHTASTIC_MAX_MESSAGES;
        count = max_count;
    }

    for (size_t i = 0; i < count; i++) {
        const cap_meshtastic_message_t *msg =
            &s_ctx.messages[(start + i) % CAP_MESHTASTIC_MAX_MESSAGES];
        if (!msg->used) {
            continue;
        }
        cJSON *obj = cJSON_CreateObject();
        if (obj) {
            cJSON_AddStringToObject(obj, "from", msg->from_id);
            cJSON_AddNumberToObject(obj, "from_num", msg->from);
            cJSON_AddNumberToObject(obj, "channel", msg->channel);
            cJSON_AddNumberToObject(obj, "packet_id", msg->packet_id);
            cJSON_AddStringToObject(obj, "text", msg->text);
            cJSON_AddNumberToObject(obj, "ts", (double)msg->received_ms);
            cJSON_AddItemToArray(arr, obj);
        }
    }

    xSemaphoreGive(s_ctx.lock);
}

static esp_err_t emit_json(cJSON *root, char *output, size_t output_size)
{
    if (!root) {
        snprintf(output, output_size, "{\"error\":\"out of memory\"}");
        return ESP_ERR_NO_MEM;
    }
    char *str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!str) {
        snprintf(output, output_size, "{\"error\":\"serialize failed\"}");
        return ESP_FAIL;
    }
    int written = snprintf(output, output_size, "%s", str);
    free(str);
    if (written < 0 || (size_t)written >= output_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

/* ===================================================================== */
/* Tool implementations                                                  */
/* ===================================================================== */

static uint32_t parse_dest(const cJSON *root, bool *is_broadcast)
{
    *is_broadcast = true;
    const cJSON *dest = cJSON_GetObjectItemCaseSensitive(root, "dest");
    if (!dest) {
        return MESHTASTIC_ADDR_BROADCAST;
    }
    if (cJSON_IsNumber(dest)) {
        *is_broadcast = false;
        return (uint32_t)dest->valuedouble;
    }
    if (cJSON_IsString(dest) && dest->valuestring && dest->valuestring[0]) {
        const char *s = dest->valuestring;
        if (strcmp(s, "broadcast") == 0 || strcmp(s, "all") == 0) {
            return MESHTASTIC_ADDR_BROADCAST;
        }
        if (s[0] == '!') {
            s++;
        } else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
            s += 2;
        }
        char *endptr = NULL;
        unsigned long v = strtoul(s, &endptr, 16);
        if (endptr && *endptr == '\0') {
            *is_broadcast = false;
            return (uint32_t)v;
        }
    }
    return MESHTASTIC_ADDR_BROADCAST;
}

static esp_err_t tool_send_text(const char *input_json, const claw_cap_call_context_t *ctx,
                                char *output, size_t output_size)
{
    (void)ctx;

    if (!s_ctx.uart_installed) {
        snprintf(output, output_size, "Error: Meshtastic bridge is not running");
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *root = input_json ? cJSON_Parse(input_json) : NULL;
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    const cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
    if (!cJSON_IsString(text) || !text->valuestring || !text->valuestring[0]) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: 'text' is required");
        return ESP_ERR_INVALID_ARG;
    }

    bool is_broadcast = true;
    uint32_t dest = parse_dest(root, &is_broadcast);

    uint32_t channel = 0;
    const cJSON *ch = cJSON_GetObjectItemCaseSensitive(root, "channel");
    if (cJSON_IsNumber(ch)) {
        channel = (uint32_t)ch->valuedouble;
    }

    bool want_ack = false;
    const cJSON *ack = cJSON_GetObjectItemCaseSensitive(root, "want_ack");
    if (cJSON_IsBool(ack)) {
        want_ack = cJSON_IsTrue(ack);
    }

    esp_err_t err = cap_meshtastic_send_text(dest, channel, want_ack, text->valuestring);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: send failed (%s)", esp_err_to_name(err));
        return err;
    }

    char dest_id[16];
    meshtastic_format_node_id(dest, dest_id, sizeof(dest_id));
    snprintf(output, output_size, "Sent to %s on channel %u",
             is_broadcast ? "broadcast" : dest_id, (unsigned int)channel);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t tool_list_nodes(const char *input_json, const claw_cap_call_context_t *ctx,
                                 char *output, size_t output_size)
{
    (void)input_json;
    (void)ctx;

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return emit_json(NULL, output, output_size);
    }
    cJSON *arr = cJSON_AddArrayToObject(root, "nodes");

    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    int count = 0;
    for (size_t i = 0; i < CAP_MESHTASTIC_MAX_NODES; i++) {
        if (!s_ctx.nodes[i].used) {
            continue;
        }
        cJSON *obj = cJSON_CreateObject();
        if (obj) {
            node_to_json(&s_ctx.nodes[i], obj);
            cJSON_AddItemToArray(arr, obj);
            count++;
        }
    }
    xSemaphoreGive(s_ctx.lock);

    cJSON_AddNumberToObject(root, "count", count);
    return emit_json(root, output, output_size);
}

static esp_err_t tool_get_status(const char *input_json, const claw_cap_call_context_t *ctx,
                                 char *output, size_t output_size)
{
    (void)input_json;
    (void)ctx;

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return emit_json(NULL, output, output_size);
    }

    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    cJSON_AddBoolToObject(root, "running", s_ctx.uart_installed);
    cJSON_AddBoolToObject(root, "connected", s_ctx.connected);
    cJSON_AddBoolToObject(root, "config_complete", s_ctx.config_complete);
    if (s_ctx.connected) {
        char id[16];
        meshtastic_format_node_id(s_ctx.my_info.my_node_num, id, sizeof(id));
        cJSON_AddStringToObject(root, "my_node_id", id);
        cJSON_AddNumberToObject(root, "my_node_num", s_ctx.my_info.my_node_num);
        cJSON_AddNumberToObject(root, "reboot_count", s_ctx.my_info.reboot_count);
    }
    if (s_ctx.has_metadata) {
        if (s_ctx.metadata.firmware_version[0]) {
            cJSON_AddStringToObject(root, "firmware_version", s_ctx.metadata.firmware_version);
        }
        if (s_ctx.metadata.hw_model) {
            cJSON_AddNumberToObject(root, "hw_model", s_ctx.metadata.hw_model);
        }
    }
    int node_count = 0;
    for (size_t i = 0; i < CAP_MESHTASTIC_MAX_NODES; i++) {
        if (s_ctx.nodes[i].used) {
            node_count++;
        }
    }
    cJSON_AddNumberToObject(root, "node_count", node_count);
    cJSON_AddNumberToObject(root, "uart_port", s_ctx.uart.uart_port);
    cJSON_AddNumberToObject(root, "uart_tx_gpio", s_ctx.uart.tx_gpio);
    cJSON_AddNumberToObject(root, "uart_rx_gpio", s_ctx.uart.rx_gpio);
    cJSON_AddNumberToObject(root, "baud_rate", s_ctx.uart.baud_rate);
    /* IM notification target for inbound mesh traffic. */
    cJSON_AddBoolToObject(root, "notify_configured", s_ctx.notify_channel[0] != '\0');
    if (s_ctx.notify_channel[0]) {
        cJSON_AddStringToObject(root, "notify_channel", s_ctx.notify_channel);
        cJSON_AddStringToObject(root, "notify_chat_id", s_ctx.notify_chat_id);
    }
    /* Diagnostics: rx_bytes==0 => wiring/power/baud problem; rx_bytes>0 but
     * frames_decoded==0 => radio not in PROTO mode (or baud mismatch). */
    cJSON_AddNumberToObject(root, "rx_bytes", s_ctx.rx_bytes);
    cJSON_AddNumberToObject(root, "frames_decoded", s_ctx.frames_decoded);
    cJSON_AddNumberToObject(root, "config_requests", s_ctx.config_requests);
    /* Packet-level counters: text/position/telemetry all arrive as
     * FromRadio.packet AFTER config sync. packets_received==0 with a high
     * frames_decoded means only the node-DB dump was seen and no live mesh
     * traffic has arrived yet, so an empty message queue is expected. */
    cJSON_AddNumberToObject(root, "packets_received", s_ctx.packets_received);
    cJSON_AddNumberToObject(root, "packets_encrypted", s_ctx.packets_encrypted);
    cJSON_AddNumberToObject(root, "messages_buffered", s_ctx.message_count);
    if (s_ctx.config_complete && s_ctx.packets_received == 0) {
        cJSON_AddStringToObject(root, "messages_hint",
                                "link is up and the node database synced, but no live "
                                "mesh packets have arrived yet; send a text from another "
                                "node to populate the message queue");
    }
    if (!s_ctx.connected) {
        const char *hint;
        if (s_ctx.rx_bytes == 0) {
            hint = "no bytes received: check power, baud (must match serial.baud), "
                   "and TX<->RX crossover wiring";
        } else if (s_ctx.frames_decoded == 0) {
            hint = "bytes received but no protobuf frames: set the radio Serial module "
                   "to PROTO mode (not TEXTMSG) and match the baud rate";
        } else {
            hint = "frames received; waiting for radio to finish config handshake";
        }
        cJSON_AddStringToObject(root, "hint", hint);
    }
    xSemaphoreGive(s_ctx.lock);

    return emit_json(root, output, output_size);
}

static esp_err_t tool_get_messages(const char *input_json, const claw_cap_call_context_t *ctx,
                                   char *output, size_t output_size)
{
    (void)input_json;
    (void)ctx;

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return emit_json(NULL, output, output_size);
    }
    cJSON *arr = cJSON_AddArrayToObject(root, "messages");

    /* Try persistent store first (newest first, up to 200). */
    size_t stored_count = 0;
    if (meshtastic_store_is_ready()) {
        if (meshtastic_store_read(arr, 200) == ESP_OK) {
            stored_count = (size_t)cJSON_GetArraySize(arr);
        }
    }

    /* If the persistent store is empty or unavailable, fall back to RAM. */
    if (stored_count == 0) {
        append_ram_messages(arr, 200);
        stored_count = (size_t)cJSON_GetArraySize(arr);
    }

    cJSON_AddNumberToObject(root, "count", (double)stored_count);
    cJSON_AddBoolToObject(root, "from_store", stored_count > 0 && meshtastic_store_is_ready());

    return emit_json(root, output, output_size);
}

static esp_err_t tool_request_config(const char *input_json, const claw_cap_call_context_t *ctx,
                                     char *output, size_t output_size)
{
    (void)input_json;
    (void)ctx;

    if (!s_ctx.uart_installed) {
        snprintf(output, output_size, "Error: Meshtastic bridge is not running");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = cap_meshtastic_request_config();
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: request failed (%s)", esp_err_to_name(err));
        return err;
    }
    snprintf(output, output_size, "Config request sent; node database will refresh shortly");
    return ESP_OK;
}

/* ===================================================================== */
/* Lifecycle / registration                                              */
/* ===================================================================== */

esp_err_t cap_meshtastic_set_uart_config(const cap_meshtastic_uart_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_ctx.uart_installed) {
        ESP_LOGW(TAG, "UART already started; config change ignored");
        return ESP_ERR_INVALID_STATE;
    }
    s_ctx.uart = *config;
    if (s_ctx.uart.baud_rate <= 0) {
        s_ctx.uart.baud_rate = CONFIG_CAP_MESHTASTIC_BAUD;
    }
    return ESP_OK;
}

static esp_err_t cap_meshtastic_group_start(void)
{
    esp_log_level_set(TAG, ESP_LOG_WARN);

    if (!s_ctx.lock) {
        s_ctx.lock = xSemaphoreCreateMutex();
    }
    if (!s_ctx.tx_lock) {
        s_ctx.tx_lock = xSemaphoreCreateMutex();
    }
    if (!s_ctx.lock || !s_ctx.tx_lock) {
        return ESP_ERR_NO_MEM;
    }

    /* Restore persisted per-IM push preferences before the RX task starts. */
    im_push_load();

    esp_err_t err = cap_meshtastic_uart_start();
    if (err != ESP_OK) {
        return err;
    }

    if (s_ctx.task) {
        return ESP_OK;
    }
    s_ctx.running = true;
    if (xTaskCreate(cap_meshtastic_task, "meshtastic_rx", CAP_MESHTASTIC_TASK_STACK,
                    NULL, CAP_MESHTASTIC_TASK_PRIO, &s_ctx.task) != pdPASS) {
        s_ctx.running = false;
        ESP_LOGE(TAG, "failed to create RX task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t cap_meshtastic_group_stop(void)
{
    s_ctx.running = false;
    /* The task self-deletes on the next loop iteration. */
    return ESP_OK;
}

static const claw_cap_descriptor_t s_descriptors[] = {
    {
        .id = "meshtastic_send_text",
        .name = "meshtastic_send_text",
        .family = "meshtastic",
        .description = "Send a text message over the Meshtastic LoRa mesh. "
        "Defaults to broadcast on channel 0. Set 'dest' to a node id like "
        "'!aabbccdd' or its decimal node number to direct-message a node.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{"
        "\"text\":{\"type\":\"string\",\"description\":\"Message body (<=237 bytes)\"},"
        "\"dest\":{\"type\":\"string\",\"description\":\"Node id '!aabbccdd' or 'broadcast'\"},"
        "\"channel\":{\"type\":\"integer\",\"description\":\"Channel index (default 0)\"},"
        "\"want_ack\":{\"type\":\"boolean\",\"description\":\"Request delivery ack\"}"
        "},\"required\":[\"text\"]}",
        .execute = tool_send_text,
    },
    {
        .id = "meshtastic_list_nodes",
        .name = "meshtastic_list_nodes",
        .family = "meshtastic",
        .description = "List Meshtastic nodes known to the bridge, including names, "
        "signal quality, position and telemetry when available.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = tool_list_nodes,
    },
    {
        .id = "meshtastic_get_status",
        .name = "meshtastic_get_status",
        .family = "meshtastic",
        .description = "Report Meshtastic bridge status: link state, local node id, "
        "firmware version and number of known nodes.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = tool_get_status,
    },
    {
        .id = "meshtastic_get_messages",
        .name = "meshtastic_get_messages",
        .family = "meshtastic",
        .description = "Return recently received Meshtastic text messages buffered by "
        "the bridge (oldest first).",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = tool_get_messages,
    },
    {
        .id = "meshtastic_request_config",
        .name = "meshtastic_request_config",
        .family = "meshtastic",
        .description = "Ask the radio to re-stream its node database and configuration. "
        "Use this to refresh the node list.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = tool_request_config,
    },
};

static const claw_cap_group_t s_group = {
    .group_id = CAP_MESHTASTIC_GROUP_ID,
    .plugin_name = "cap_meshtastic",
    .version = "1.0.0",
    .descriptors = s_descriptors,
    .descriptor_count = sizeof(s_descriptors) / sizeof(s_descriptors[0]),
    .group_start = cap_meshtastic_group_start,
    .group_stop = cap_meshtastic_group_stop,
};

/* ===================================================================== */
/* Persistent store public API wrappers                                   */
/* ===================================================================== */

esp_err_t cap_meshtastic_set_store_path(const char *base_path, size_t max_bytes)
{
    meshtastic_store_config_t cfg = {
        .base_path = base_path,
        .max_file_bytes = max_bytes,
    };
    esp_err_t err = meshtastic_store_init(&cfg);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "persistent store: %s (max %u bytes)",
                 meshtastic_store_path(), (unsigned)max_bytes);
    } else {
        ESP_LOGW(TAG, "persistent store init failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t cap_meshtastic_read_stored_messages(cJSON *array, size_t max_count)
{
    if (!array) {
        return ESP_ERR_INVALID_ARG;
    }

    if (meshtastic_store_is_ready()) {
        esp_err_t err = meshtastic_store_read(array, max_count);
        if (err == ESP_OK) {
            return ESP_OK;
        }
    }

    append_ram_messages(array, max_count);
    return ESP_OK;
}

esp_err_t cap_meshtastic_clear_stored_messages(void)
{
    if (!meshtastic_store_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    return meshtastic_store_clear();
}

size_t cap_meshtastic_stored_count(void)
{
    return meshtastic_store_count();
}

size_t cap_meshtastic_store_file_size(void)
{
    return meshtastic_store_file_size();
}

const char *cap_meshtastic_store_path(void)
{
    return meshtastic_store_path();
}

esp_err_t cap_meshtastic_register_group(void)
{
    esp_log_level_set(TAG, ESP_LOG_WARN);

    if (claw_cap_group_exists(s_group.group_id)) {
        return ESP_OK;
    }
    return claw_cap_register_group(&s_group);
}
