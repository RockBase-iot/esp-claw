/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "claw_event.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef esp_err_t (*claw_event_publish_fn)(const claw_event_t *event);

esp_err_t claw_event_router_publish(const claw_event_t *event);
esp_err_t claw_event_router_publish_message(const char *source_cap,
                                            const char *channel,
                                            const char *chat_id,
                                            const char *text,
                                            const char *sender_id,
                                            const char *message_id);
esp_err_t claw_event_router_publish_trigger(const char *source_cap,
                                            const char *event_type,
                                            const char *event_key,
                                            const char *payload_json);

/**
 * Optional observer invoked synchronously from claw_event_router_publish_message
 * for every inbound IM message. Intended for lightweight UI / notification
 * sinks; do not block. Pass NULL to clear.
 */
typedef void (*claw_event_router_message_observer_fn)(const char *channel,
                                                      const char *chat_id,
                                                      const char *sender_id,
                                                      const char *text,
                                                      int64_t timestamp_ms,
                                                      void *user_ctx);

void claw_event_router_set_message_observer(claw_event_router_message_observer_fn observer,
                                            void *user_ctx);

#ifdef __cplusplus
}
#endif
