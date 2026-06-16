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

/* Meshtastic Stream API framing markers. */
#define MESHTASTIC_FRAME_START1 0x94
#define MESHTASTIC_FRAME_START2 0xc3
#define MESHTASTIC_FRAME_HEADER_LEN 4
#define MESHTASTIC_FRAME_MAX_PAYLOAD 512

/* Reserved broadcast destination address. */
#define MESHTASTIC_ADDR_BROADCAST 0xFFFFFFFFu

/* Subset of meshtastic.PortNum relevant for this bridge. */
typedef enum {
    MESHTASTIC_PORTNUM_UNKNOWN = 0,
    MESHTASTIC_PORTNUM_TEXT_MESSAGE = 1,
    MESHTASTIC_PORTNUM_REMOTE_HARDWARE = 2,
    MESHTASTIC_PORTNUM_POSITION = 3,
    MESHTASTIC_PORTNUM_NODEINFO = 4,
    MESHTASTIC_PORTNUM_ROUTING = 5,
    MESHTASTIC_PORTNUM_ADMIN = 6,
    MESHTASTIC_PORTNUM_WAYPOINT = 8,
    MESHTASTIC_PORTNUM_TELEMETRY = 67,
    MESHTASTIC_PORTNUM_TRACEROUTE = 70,
} meshtastic_portnum_t;

/* meshtastic.Telemetry -> DeviceMetrics. */
typedef struct {
    bool has_battery_level;
    uint32_t battery_level;
    bool has_voltage;
    float voltage;
    bool has_channel_utilization;
    float channel_utilization;
    bool has_air_util_tx;
    float air_util_tx;
    bool has_uptime_seconds;
    uint32_t uptime_seconds;
} meshtastic_device_metrics_t;

/* meshtastic.Position (only the commonly used fields). */
typedef struct {
    bool valid;
    int32_t latitude_i;     /* degrees * 1e7 */
    int32_t longitude_i;    /* degrees * 1e7 */
    int32_t altitude;       /* meters */
    uint32_t time;          /* unix seconds */
} meshtastic_position_t;

/* meshtastic.User. */
typedef struct {
    bool valid;
    char id[16];
    char long_name[40];
    char short_name[8];
    uint32_t hw_model;
    uint32_t role;
} meshtastic_user_t;

/* meshtastic.Data (the decoded payload of a MeshPacket). */
typedef struct {
    uint32_t portnum;
    const uint8_t *payload;     /* points into the source frame buffer */
    size_t payload_len;
    bool want_response;
    uint32_t dest;
    uint32_t source;
    uint32_t request_id;
    uint32_t reply_id;
} meshtastic_data_t;

/* meshtastic.MeshPacket. */
typedef struct {
    uint32_t from;
    uint32_t to;
    uint32_t channel;
    uint32_t id;
    uint32_t rx_time;
    float rx_snr;
    int32_t rx_rssi;
    uint32_t hop_limit;
    bool want_ack;
    bool has_decoded;
    meshtastic_data_t decoded;
    bool is_encrypted;          /* payload was the encrypted variant */
} meshtastic_meshpacket_t;

/* meshtastic.NodeInfo. */
typedef struct {
    uint32_t num;
    meshtastic_user_t user;
    meshtastic_position_t position;
    bool has_snr;
    float snr;
    uint32_t last_heard;
    bool has_metrics;
    meshtastic_device_metrics_t metrics;
} meshtastic_nodeinfo_t;

/* meshtastic.MyNodeInfo. */
typedef struct {
    uint32_t my_node_num;
    uint32_t reboot_count;
    uint32_t min_app_version;
} meshtastic_mynodeinfo_t;

/* meshtastic.DeviceMetadata. */
typedef struct {
    char firmware_version[24];
    uint32_t hw_model;
    uint32_t role;
} meshtastic_metadata_t;

/* Discriminator for a decoded FromRadio message. */
typedef enum {
    MESHTASTIC_FROMRADIO_NONE = 0,
    MESHTASTIC_FROMRADIO_PACKET,
    MESHTASTIC_FROMRADIO_MY_INFO,
    MESHTASTIC_FROMRADIO_NODE_INFO,
    MESHTASTIC_FROMRADIO_CONFIG_COMPLETE_ID,
    MESHTASTIC_FROMRADIO_METADATA,
    MESHTASTIC_FROMRADIO_OTHER,
} meshtastic_fromradio_which_t;

/* meshtastic.FromRadio (the variants this bridge understands). */
typedef struct {
    meshtastic_fromradio_which_t which;
    meshtastic_meshpacket_t packet;
    meshtastic_mynodeinfo_t my_info;
    meshtastic_nodeinfo_t node_info;
    uint32_t config_complete_id;
    meshtastic_metadata_t metadata;
} meshtastic_fromradio_t;

/* Parameters for building a ToRadio text packet. */
typedef struct {
    uint32_t to;            /* destination node num, or MESHTASTIC_ADDR_BROADCAST */
    uint32_t channel;       /* channel index */
    uint32_t id;            /* packet id (0 lets the radio assign one) */
    bool want_ack;
    const char *text;
} meshtastic_text_packet_t;

/*
 * Decode a single FromRadio protobuf message (the payload after the 4-byte
 * Stream API header). The returned structure may reference memory inside
 * `data`, so it must not outlive the source buffer.
 */
esp_err_t meshtastic_decode_fromradio(const uint8_t *data, size_t len,
                                      meshtastic_fromradio_t *out);

/*
 * Get statistics for FromRadio frames whose top-level payload tag is unknown.
 * `count` is cumulative since boot; `last_tag` is the most recent unknown tag.
 */
void meshtastic_get_fromradio_unknown_stats(uint32_t *count, uint32_t *last_tag);

/* Encode a ToRadio text message and prepend the 4-byte Stream API header. */
esp_err_t meshtastic_encode_text_frame(const meshtastic_text_packet_t *packet,
                                       uint8_t *out, size_t out_size,
                                       size_t *out_len);

/* Encode a ToRadio want_config_id request, prepended with the Stream header. */
esp_err_t meshtastic_encode_want_config_frame(uint32_t config_id,
                                              uint8_t *out, size_t out_size,
                                              size_t *out_len);

/* Encode a ToRadio heartbeat, prepended with the Stream header. */
esp_err_t meshtastic_encode_heartbeat_frame(uint8_t *out, size_t out_size,
                                            size_t *out_len);

/* Format a node number as the canonical Meshtastic "!aabbccdd" id string. */
void meshtastic_format_node_id(uint32_t num, char *out, size_t out_size);

/* Human-readable label for a PortNum value. */
const char *meshtastic_portnum_to_string(uint32_t portnum);

/* Decode a TELEMETRY_APP payload into DeviceMetrics (best effort). */
void meshtastic_decode_telemetry_payload(const uint8_t *data, size_t len,
                                         meshtastic_device_metrics_t *out,
                                         bool *has_metrics);

/* Decode a POSITION_APP payload into a Position (best effort). */
void meshtastic_decode_position_payload(const uint8_t *data, size_t len,
                                        meshtastic_position_t *out);

/* Decode a NODEINFO_APP payload (a meshtastic.User) into a User (best effort). */
void meshtastic_decode_user_payload(const uint8_t *data, size_t len,
                                    meshtastic_user_t *out);

#ifdef __cplusplus
}
#endif
