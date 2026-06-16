/*
 * SPDX-FileCopyrightText: 2026 Chengdu RockBase Technology Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* Forward declaration to avoid pulling in cJSON.h for every includer. */
typedef struct cJSON cJSON;

#ifdef __cplusplus
extern "C" {
#endif

/* UART transport configuration for the Meshtastic bridge. */
typedef struct {
    int uart_port;     /* UART peripheral number (e.g. 1) */
    int tx_gpio;       /* mainboard TX -> Heltec RX */
    int rx_gpio;       /* mainboard RX -> Heltec TX */
    int baud_rate;     /* must match Meshtastic serial.baud (default 115200) */
} cap_meshtastic_uart_config_t;

/* Override the UART configuration before the group starts. */
esp_err_t cap_meshtastic_set_uart_config(const cap_meshtastic_uart_config_t *config);

/* Register the Meshtastic capability group with claw_cap. */
esp_err_t cap_meshtastic_register_group(void);

/*
 * Send a text message over the mesh.
 *   dest    - destination node number, or 0xFFFFFFFF for broadcast.
 *   channel - channel index (0 = primary).
 *   want_ack- request a delivery acknowledgement.
 * Returns ESP_OK once the frame has been written to the UART.
 */
esp_err_t cap_meshtastic_send_text(uint32_t dest, uint32_t channel,
                                   bool want_ack, const char *text);

/* Ask the radio to (re)stream its node database and config. */
esp_err_t cap_meshtastic_request_config(void);

/* True once the link to the radio has produced a MyNodeInfo. */
bool cap_meshtastic_is_connected(void);

/*
 * Set the IM conversation that should receive proactive notifications for
 * inbound mesh messages and node updates. `channel` is a logical IM channel
 * (e.g. "feishu", "qq", "telegram", "wechat", "web") and `chat_id` is the
 * conversation id. Pass NULL/empty to clear. An explicit target set here is
 * sticky and is never overwritten by auto-learning.
 */
esp_err_t cap_meshtastic_set_notify_target(const char *channel, const char *chat_id);

/*
 * Hint the most recent inbound IM conversation. The bridge uses this to deliver
 * mesh notifications to "whoever last talked to the device" when no explicit
 * target has been configured. Safe to call from the IM message observer.
 */
void cap_meshtastic_note_im_target(const char *channel, const char *chat_id);

/* ===================================================================== */
/* Per-IM push preferences                                                */
/* ===================================================================== */

/* IM channels that inbound mesh messages can be pushed to. */
#define CAP_MESHTASTIC_IM_PUSH_MAX 4

/* Runtime state of one IM push channel. */
typedef struct {
    char channel[16];   /* logical IM channel ("feishu"/"qq"/"telegram"/"wechat") */
    bool enabled;       /* user opted to push inbound mesh messages here */
    bool has_target;    /* a destination conversation has been learned */
} cap_meshtastic_im_push_t;

/*
 * Fill `out` with the push state of every known IM channel (up to `max`).
 * Returns the number of entries written. The "configured/available" check
 * (compiled-in + credentials present) is the caller's responsibility.
 */
size_t cap_meshtastic_get_im_push(cap_meshtastic_im_push_t *out, size_t max);

/*
 * Enable or disable proactive IM push for a specific channel. The preference
 * is persisted in NVS. Unknown channels return ESP_ERR_INVALID_ARG.
 */
esp_err_t cap_meshtastic_set_im_push_enabled(const char *channel, bool enabled);

/* ===================================================================== */
/* Persistent message store                                               */
/* ===================================================================== */

/**
 * Configure the persistent message store path.
 * @param base_path  VFS mount point (e.g. "/fatfs" or "/sdcard").
 * @param max_bytes  Max file size in bytes; 0 = unlimited.
 */
esp_err_t cap_meshtastic_set_store_path(const char *base_path, size_t max_bytes);

/**
 * Read persisted messages into a cJSON array (newest first).
 * @param array      Pre-created cJSON array; objects are appended.
 * @param max_count  Maximum messages to return (0 = all).
 */
esp_err_t cap_meshtastic_read_stored_messages(cJSON *array, size_t max_count);

/** Delete all persisted messages. */
esp_err_t cap_meshtastic_clear_stored_messages(void);

/** Number of persisted messages. */
size_t cap_meshtastic_stored_count(void);

/** Store file size in bytes. */
size_t cap_meshtastic_store_file_size(void);

/** Absolute path to the store file, or NULL if not initialised. */
const char *cap_meshtastic_store_path(void);

#ifdef __cplusplus
}
#endif
