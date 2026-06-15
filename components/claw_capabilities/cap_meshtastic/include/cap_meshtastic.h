/*
 * SPDX-FileCopyrightText: 2026 Chengdu RockBase IoT Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

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

#ifdef __cplusplus
}
#endif
