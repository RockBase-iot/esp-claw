/*
 * SPDX-FileCopyrightText: 2026 Chengdu RockBase Technology Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "meshtastic_proto.h"

#include <stdio.h>
#include <string.h>

/* Protobuf wire types. */
#define PB_WIRE_VARINT 0
#define PB_WIRE_64BIT 1
#define PB_WIRE_LEN 2
#define PB_WIRE_32BIT 5

/* ---- FromRadio field numbers ---- */
#define FR_ID 1
#define FR_PACKET 2
#define FR_PACKET_LEGACY 1
#define FR_MY_INFO 3
#define FR_NODE_INFO 4
#define FR_CONFIG 5
#define FR_LOG_RECORD 6
#define FR_CONFIG_COMPLETE_ID 7
#define FR_REBOOTED 8
#define FR_MODULE_CONFIG 9
#define FR_CHANNEL 10
#define FR_QUEUE_STATUS 11
#define FR_XMODEM_PACKET 12
#define FR_METADATA 13
#define FR_METADATA_LEGACY 14
#define FR_MQTT_PROXY_MSG 14
#define FR_FILE_INFO 15
#define FR_CLIENT_NOTIFICATION 16
#define FR_DEVICE_UI_CONFIG 17
#define FR_LOCKDOWN_STATUS 18

/* ---- ToRadio field numbers ---- */
#define TR_PACKET 1
#define TR_WANT_CONFIG_ID 3
#define TR_HEARTBEAT 7

/* ---- MeshPacket field numbers ---- */
#define MP_FROM 1
#define MP_TO 2
#define MP_CHANNEL 3
#define MP_DECODED 4
#define MP_ENCRYPTED 5
#define MP_ID 6
#define MP_RX_TIME 7
#define MP_RX_SNR 8
#define MP_HOP_LIMIT 9
#define MP_WANT_ACK 10
#define MP_RX_RSSI 12

/* ---- Data field numbers ---- */
#define DATA_PORTNUM 1
#define DATA_PAYLOAD 2
#define DATA_WANT_RESPONSE 3
#define DATA_DEST 4
#define DATA_SOURCE 5
#define DATA_REQUEST_ID 6
#define DATA_REPLY_ID 7

/* ---- NodeInfo field numbers ---- */
#define NI_NUM 1
#define NI_USER 2
#define NI_POSITION 3
#define NI_SNR 4
#define NI_LAST_HEARD 5
#define NI_DEVICE_METRICS 6

/* ---- User field numbers ---- */
#define USER_ID 1
#define USER_LONG_NAME 2
#define USER_SHORT_NAME 3
#define USER_HW_MODEL 5
#define USER_ROLE 7

/* ---- Position field numbers ---- */
#define POS_LATITUDE_I 1
#define POS_LONGITUDE_I 2
#define POS_ALTITUDE 3
#define POS_TIME 4

/* ---- MyNodeInfo field numbers ---- */
#define MNI_MY_NODE_NUM 1
#define MNI_REBOOT_COUNT 8
#define MNI_REBOOT_COUNT_LEGACY 4
#define MNI_MIN_APP_VERSION 11
#define MNI_MIN_APP_VERSION_LEGACY 8

/* ---- DeviceMetrics field numbers ---- */
#define DM_BATTERY_LEVEL 1
#define DM_VOLTAGE 2
#define DM_CHANNEL_UTILIZATION 3
#define DM_AIR_UTIL_TX 4
#define DM_UPTIME_SECONDS 5

/* ---- Telemetry field numbers ---- */
#define TELEM_DEVICE_METRICS 2

/* ---- DeviceMetadata field numbers ---- */
#define MD_FIRMWARE_VERSION 1
#define MD_ROLE 7
#define MD_HW_MODEL 9

/* Unknown FromRadio top-level payload tag diagnostics. */
static uint32_t s_fromradio_unknown_count = 0;
static uint32_t s_fromradio_unknown_last_tag = 0;

/* ===================================================================== */
/* Reader                                                                */
/* ===================================================================== */

typedef struct {
    const uint8_t *p;
    const uint8_t *end;
} pb_reader_t;

static bool pb_read_varint(pb_reader_t *r, uint64_t *out)
{
    uint64_t value = 0;
    int shift = 0;

    while (r->p < r->end) {
        uint8_t byte = *r->p++;
        if (shift < 64) {
            value |= ((uint64_t)(byte & 0x7F)) << shift;
        }
        shift += 7;
        if ((byte & 0x80) == 0) {
            *out = value;
            return true;
        }
        if (shift > 70) {
            return false;
        }
    }
    return false;
}

static bool pb_read_fixed32(pb_reader_t *r, uint32_t *out)
{
    if (r->end - r->p < 4) {
        return false;
    }
    *out = (uint32_t)r->p[0] | ((uint32_t)r->p[1] << 8) |
           ((uint32_t)r->p[2] << 16) | ((uint32_t)r->p[3] << 24);
    r->p += 4;
    return true;
}

static bool pb_read_fixed64(pb_reader_t *r, uint64_t *out)
{
    if (r->end - r->p < 8) {
        return false;
    }
    uint64_t value = 0;
    for (int i = 0; i < 8; i++) {
        value |= ((uint64_t)r->p[i]) << (8 * i);
    }
    r->p += 8;
    *out = value;
    return true;
}

static bool pb_read_tag(pb_reader_t *r, uint32_t *field_no, uint32_t *wire_type)
{
    uint64_t tag = 0;
    if (!pb_read_varint(r, &tag)) {
        return false;
    }
    *field_no = (uint32_t)(tag >> 3);
    *wire_type = (uint32_t)(tag & 0x07);
    return true;
}

/* Read a length-delimited field; returns pointer/len into the buffer. */
static bool pb_read_bytes(pb_reader_t *r, const uint8_t **data, size_t *len)
{
    uint64_t length = 0;
    if (!pb_read_varint(r, &length)) {
        return false;
    }
    if ((uint64_t)(r->end - r->p) < length) {
        return false;
    }
    *data = r->p;
    *len = (size_t)length;
    r->p += length;
    return true;
}

/* Skip a field whose wire type is not consumed by the caller. */
static bool pb_skip_field(pb_reader_t *r, uint32_t wire_type)
{
    switch (wire_type) {
    case PB_WIRE_VARINT: {
        uint64_t tmp;
        return pb_read_varint(r, &tmp);
    }
    case PB_WIRE_64BIT: {
        uint64_t tmp;
        return pb_read_fixed64(r, &tmp);
    }
    case PB_WIRE_LEN: {
        const uint8_t *d;
        size_t l;
        return pb_read_bytes(r, &d, &l);
    }
    case PB_WIRE_32BIT: {
        uint32_t tmp;
        return pb_read_fixed32(r, &tmp);
    }
    default:
        return false;
    }
}

static float pb_u32_to_float(uint32_t bits)
{
    float f;
    memcpy(&f, &bits, sizeof(f));
    return f;
}

static void pb_copy_string(const uint8_t *data, size_t len, char *out, size_t out_size)
{
    if (out_size == 0) {
        return;
    }
    size_t copy = len;
    if (copy >= out_size) {
        copy = out_size - 1;
    }
    memcpy(out, data, copy);
    out[copy] = '\0';
}

/* ===================================================================== */
/* Sub-message decoders                                                  */
/* ===================================================================== */

static void decode_device_metrics(const uint8_t *data, size_t len,
                                  meshtastic_device_metrics_t *out)
{
    pb_reader_t r = { data, data + len };
    uint32_t field, wire;

    while (pb_read_tag(&r, &field, &wire)) {
        if (wire == PB_WIRE_VARINT) {
            uint64_t v;
            if (!pb_read_varint(&r, &v)) {
                return;
            }
            switch (field) {
            case DM_BATTERY_LEVEL:
                out->has_battery_level = true;
                out->battery_level = (uint32_t)v;
                break;
            case DM_UPTIME_SECONDS:
                out->has_uptime_seconds = true;
                out->uptime_seconds = (uint32_t)v;
                break;
            default:
                break;
            }
        } else if (wire == PB_WIRE_32BIT) {
            uint32_t bits;
            if (!pb_read_fixed32(&r, &bits)) {
                return;
            }
            switch (field) {
            case DM_VOLTAGE:
                out->has_voltage = true;
                out->voltage = pb_u32_to_float(bits);
                break;
            case DM_CHANNEL_UTILIZATION:
                out->has_channel_utilization = true;
                out->channel_utilization = pb_u32_to_float(bits);
                break;
            case DM_AIR_UTIL_TX:
                out->has_air_util_tx = true;
                out->air_util_tx = pb_u32_to_float(bits);
                break;
            default:
                break;
            }
        } else if (!pb_skip_field(&r, wire)) {
            return;
        }
    }
}

static void decode_telemetry(const uint8_t *data, size_t len,
                             meshtastic_device_metrics_t *out, bool *has_metrics)
{
    pb_reader_t r = { data, data + len };
    uint32_t field, wire;

    while (pb_read_tag(&r, &field, &wire)) {
        if (wire == PB_WIRE_LEN) {
            const uint8_t *sub;
            size_t sub_len;
            if (!pb_read_bytes(&r, &sub, &sub_len)) {
                return;
            }
            if (field == TELEM_DEVICE_METRICS) {
                decode_device_metrics(sub, sub_len, out);
                *has_metrics = true;
            }
        } else if (!pb_skip_field(&r, wire)) {
            return;
        }
    }
}

static void decode_user(const uint8_t *data, size_t len, meshtastic_user_t *out)
{
    pb_reader_t r = { data, data + len };
    uint32_t field, wire;

    out->valid = true;
    while (pb_read_tag(&r, &field, &wire)) {
        if (wire == PB_WIRE_LEN) {
            const uint8_t *sub;
            size_t sub_len;
            if (!pb_read_bytes(&r, &sub, &sub_len)) {
                return;
            }
            switch (field) {
            case USER_ID:
                pb_copy_string(sub, sub_len, out->id, sizeof(out->id));
                break;
            case USER_LONG_NAME:
                pb_copy_string(sub, sub_len, out->long_name, sizeof(out->long_name));
                break;
            case USER_SHORT_NAME:
                pb_copy_string(sub, sub_len, out->short_name, sizeof(out->short_name));
                break;
            default:
                break;
            }
        } else if (wire == PB_WIRE_VARINT) {
            uint64_t v;
            if (!pb_read_varint(&r, &v)) {
                return;
            }
            switch (field) {
            case USER_HW_MODEL:
                out->hw_model = (uint32_t)v;
                break;
            case USER_ROLE:
                out->role = (uint32_t)v;
                break;
            default:
                break;
            }
        } else if (!pb_skip_field(&r, wire)) {
            return;
        }
    }
}

static void decode_position(const uint8_t *data, size_t len, meshtastic_position_t *out)
{
    pb_reader_t r = { data, data + len };
    uint32_t field, wire;

    while (pb_read_tag(&r, &field, &wire)) {
        if (wire == PB_WIRE_32BIT) {
            uint32_t bits;
            if (!pb_read_fixed32(&r, &bits)) {
                return;
            }
            switch (field) {
            case POS_LATITUDE_I:
                out->latitude_i = (int32_t)bits;
                out->valid = true;
                break;
            case POS_LONGITUDE_I:
                out->longitude_i = (int32_t)bits;
                out->valid = true;
                break;
            case POS_TIME:
                out->time = bits;
                break;
            default:
                break;
            }
        } else if (wire == PB_WIRE_VARINT) {
            uint64_t v;
            if (!pb_read_varint(&r, &v)) {
                return;
            }
            if (field == POS_ALTITUDE) {
                /* int32 stored as varint (two's complement). */
                out->altitude = (int32_t)(uint32_t)v;
                out->valid = true;
            }
        } else if (!pb_skip_field(&r, wire)) {
            return;
        }
    }
}

static void decode_data(const uint8_t *data, size_t len, meshtastic_data_t *out)
{
    pb_reader_t r = { data, data + len };
    uint32_t field, wire;

    while (pb_read_tag(&r, &field, &wire)) {
        if (wire == PB_WIRE_VARINT) {
            uint64_t v;
            if (!pb_read_varint(&r, &v)) {
                return;
            }
            switch (field) {
            case DATA_PORTNUM:
                out->portnum = (uint32_t)v;
                break;
            case DATA_WANT_RESPONSE:
                out->want_response = v != 0;
                break;
            /* Legacy compatibility: older schemas could be decoded as varint. */
            case DATA_DEST:
                out->dest = (uint32_t)v;
                break;
            case DATA_SOURCE:
                out->source = (uint32_t)v;
                break;
            case DATA_REQUEST_ID:
                out->request_id = (uint32_t)v;
                break;
            case DATA_REPLY_ID:
                out->reply_id = (uint32_t)v;
                break;
            default:
                break;
            }
        } else if (wire == PB_WIRE_32BIT) {
            uint32_t bits;
            if (!pb_read_fixed32(&r, &bits)) {
                return;
            }
            switch (field) {
            case DATA_DEST:
                out->dest = bits;
                break;
            case DATA_SOURCE:
                out->source = bits;
                break;
            case DATA_REQUEST_ID:
                out->request_id = bits;
                break;
            case DATA_REPLY_ID:
                out->reply_id = bits;
                break;
            default:
                break;
            }
        } else if (wire == PB_WIRE_LEN) {
            const uint8_t *sub;
            size_t sub_len;
            if (!pb_read_bytes(&r, &sub, &sub_len)) {
                return;
            }
            if (field == DATA_PAYLOAD) {
                out->payload = sub;
                out->payload_len = sub_len;
            }
        } else if (!pb_skip_field(&r, wire)) {
            return;
        }
    }
}

static void decode_meshpacket(const uint8_t *data, size_t len, meshtastic_meshpacket_t *out)
{
    pb_reader_t r = { data, data + len };
    uint32_t field, wire;

    while (pb_read_tag(&r, &field, &wire)) {
        if (wire == PB_WIRE_32BIT) {
            uint32_t bits;
            if (!pb_read_fixed32(&r, &bits)) {
                return;
            }
            switch (field) {
            case MP_FROM:
                out->from = bits;
                break;
            case MP_TO:
                out->to = bits;
                break;
            case MP_ID:
                out->id = bits;
                break;
            case MP_RX_TIME:
                out->rx_time = bits;
                break;
            case MP_RX_SNR:
                out->rx_snr = pb_u32_to_float(bits);
                break;
            default:
                break;
            }
        } else if (wire == PB_WIRE_VARINT) {
            uint64_t v;
            if (!pb_read_varint(&r, &v)) {
                return;
            }
            switch (field) {
            case MP_CHANNEL:
                out->channel = (uint32_t)v;
                break;
            case MP_HOP_LIMIT:
                out->hop_limit = (uint32_t)v;
                break;
            case MP_WANT_ACK:
                out->want_ack = v != 0;
                break;
            case MP_RX_RSSI:
                out->rx_rssi = (int32_t)(uint32_t)v;
                break;
            default:
                break;
            }
        } else if (wire == PB_WIRE_LEN) {
            const uint8_t *sub;
            size_t sub_len;
            if (!pb_read_bytes(&r, &sub, &sub_len)) {
                return;
            }
            switch (field) {
            case MP_DECODED:
                out->has_decoded = true;
                decode_data(sub, sub_len, &out->decoded);
                break;
            case MP_ENCRYPTED:
                out->is_encrypted = true;
                break;
            default:
                break;
            }
        } else if (!pb_skip_field(&r, wire)) {
            return;
        }
    }
}

static void decode_nodeinfo(const uint8_t *data, size_t len, meshtastic_nodeinfo_t *out)
{
    pb_reader_t r = { data, data + len };
    uint32_t field, wire;

    while (pb_read_tag(&r, &field, &wire)) {
        if (wire == PB_WIRE_VARINT) {
            uint64_t v;
            if (!pb_read_varint(&r, &v)) {
                return;
            }
            if (field == NI_NUM) {
                out->num = (uint32_t)v;
            }
        } else if (wire == PB_WIRE_32BIT) {
            uint32_t bits;
            if (!pb_read_fixed32(&r, &bits)) {
                return;
            }
            switch (field) {
            case NI_SNR:
                out->has_snr = true;
                out->snr = pb_u32_to_float(bits);
                break;
            case NI_LAST_HEARD:
                out->last_heard = bits;
                break;
            default:
                break;
            }
        } else if (wire == PB_WIRE_LEN) {
            const uint8_t *sub;
            size_t sub_len;
            if (!pb_read_bytes(&r, &sub, &sub_len)) {
                return;
            }
            switch (field) {
            case NI_USER:
                decode_user(sub, sub_len, &out->user);
                break;
            case NI_POSITION:
                decode_position(sub, sub_len, &out->position);
                break;
            case NI_DEVICE_METRICS:
                out->has_metrics = true;
                decode_device_metrics(sub, sub_len, &out->metrics);
                break;
            default:
                break;
            }
        } else if (!pb_skip_field(&r, wire)) {
            return;
        }
    }
}

static void decode_mynodeinfo(const uint8_t *data, size_t len, meshtastic_mynodeinfo_t *out)
{
    pb_reader_t r = { data, data + len };
    uint32_t field, wire;
    bool saw_legacy_reboot = false;

    while (pb_read_tag(&r, &field, &wire)) {
        if (wire == PB_WIRE_VARINT) {
            uint64_t v;
            if (!pb_read_varint(&r, &v)) {
                return;
            }
            if (field == MNI_MY_NODE_NUM) {
                out->my_node_num = (uint32_t)v;
            } else if (field == MNI_REBOOT_COUNT_LEGACY) {
                out->reboot_count = (uint32_t)v;
                saw_legacy_reboot = true;
            } else if (field == MNI_MIN_APP_VERSION) {
                out->min_app_version = (uint32_t)v;
            } else if (field == MNI_REBOOT_COUNT) {
                /* Tag 8 is ambiguous across firmware generations:
                 * - legacy: min_app_version
                 * - current: reboot_count
                 * If legacy reboot tag (4) was observed, treat tag 8 as legacy
                 * min_app_version; otherwise treat it as current reboot_count. */
                if (saw_legacy_reboot) {
                    out->min_app_version = (uint32_t)v;
                } else {
                    out->reboot_count = (uint32_t)v;
                }
            }
        } else if (!pb_skip_field(&r, wire)) {
            return;
        }
    }
}

static void decode_metadata(const uint8_t *data, size_t len, meshtastic_metadata_t *out)
{
    pb_reader_t r = { data, data + len };
    uint32_t field, wire;

    while (pb_read_tag(&r, &field, &wire)) {
        if (wire == PB_WIRE_VARINT) {
            uint64_t v;
            if (!pb_read_varint(&r, &v)) {
                return;
            }
            switch (field) {
            case MD_ROLE:
                out->role = (uint32_t)v;
                break;
            case MD_HW_MODEL:
                out->hw_model = (uint32_t)v;
                break;
            default:
                break;
            }
        } else if (wire == PB_WIRE_LEN) {
            const uint8_t *sub;
            size_t sub_len;
            if (!pb_read_bytes(&r, &sub, &sub_len)) {
                return;
            }
            if (field == MD_FIRMWARE_VERSION) {
                pb_copy_string(sub, sub_len, out->firmware_version, sizeof(out->firmware_version));
            }
        } else if (!pb_skip_field(&r, wire)) {
            return;
        }
    }
}

esp_err_t meshtastic_decode_fromradio(const uint8_t *data, size_t len,
                                      meshtastic_fromradio_t *out)
{
    if (!data || !out) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    out->which = MESHTASTIC_FROMRADIO_OTHER;

    pb_reader_t r = { data, data + len };
    uint32_t field, wire;
    bool decoded_known = false;
    uint32_t first_unknown_tag = 0;

    while (pb_read_tag(&r, &field, &wire)) {
        if (wire == PB_WIRE_LEN) {
            const uint8_t *sub;
            size_t sub_len;
            if (!pb_read_bytes(&r, &sub, &sub_len)) {
                return ESP_ERR_INVALID_SIZE;
            }
            switch (field) {
            case FR_PACKET_LEGACY:
            case FR_PACKET:
                out->which = MESHTASTIC_FROMRADIO_PACKET;
                decode_meshpacket(sub, sub_len, &out->packet);
                decoded_known = true;
                break;
            case FR_MY_INFO:
                out->which = MESHTASTIC_FROMRADIO_MY_INFO;
                decode_mynodeinfo(sub, sub_len, &out->my_info);
                decoded_known = true;
                break;
            case FR_NODE_INFO:
                out->which = MESHTASTIC_FROMRADIO_NODE_INFO;
                decode_nodeinfo(sub, sub_len, &out->node_info);
                decoded_known = true;
                break;
            case FR_METADATA_LEGACY:
            case FR_METADATA:
                out->which = MESHTASTIC_FROMRADIO_METADATA;
                decode_metadata(sub, sub_len, &out->metadata);
                decoded_known = true;
                break;
            /* Known but currently ignored variants. */
            case FR_CONFIG:
            case FR_LOG_RECORD:
            case FR_MODULE_CONFIG:
            case FR_CHANNEL:
            case FR_QUEUE_STATUS:
            case FR_XMODEM_PACKET:
            case FR_FILE_INFO:
            case FR_CLIENT_NOTIFICATION:
            case FR_DEVICE_UI_CONFIG:
            case FR_LOCKDOWN_STATUS:
                break;
            default:
                if (first_unknown_tag == 0) {
                    first_unknown_tag = field;
                }
                break;
            }
            if (decoded_known) {
                return ESP_OK;
            }
        } else if (wire == PB_WIRE_VARINT) {
            uint64_t v;
            if (!pb_read_varint(&r, &v)) {
                return ESP_ERR_INVALID_SIZE;
            }
            if (field == FR_CONFIG_COMPLETE_ID) {
                out->which = MESHTASTIC_FROMRADIO_CONFIG_COMPLETE_ID;
                out->config_complete_id = (uint32_t)v;
                return ESP_OK;
            }
            /* Known but currently ignored scalar fields. */
            if (field == FR_ID || field == FR_REBOOTED) {
                continue;
            }
            if (first_unknown_tag == 0) {
                first_unknown_tag = field;
            }
        } else if (!pb_skip_field(&r, wire)) {
            return ESP_ERR_INVALID_SIZE;
        }
    }

    if (first_unknown_tag != 0) {
        s_fromradio_unknown_count++;
        s_fromradio_unknown_last_tag = first_unknown_tag;
    }

    return ESP_OK;
}

void meshtastic_get_fromradio_unknown_stats(uint32_t *count, uint32_t *last_tag)
{
    if (count) {
        *count = s_fromradio_unknown_count;
    }
    if (last_tag) {
        *last_tag = s_fromradio_unknown_last_tag;
    }
}

/* ===================================================================== */
/* Writer                                                                */
/* ===================================================================== */

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t len;
    bool overflow;
} pb_writer_t;

static void pb_w_byte(pb_writer_t *w, uint8_t b)
{
    if (w->len >= w->cap) {
        w->overflow = true;
        return;
    }
    w->buf[w->len++] = b;
}

static void pb_w_varint(pb_writer_t *w, uint64_t value)
{
    do {
        uint8_t byte = value & 0x7F;
        value >>= 7;
        if (value) {
            byte |= 0x80;
        }
        pb_w_byte(w, byte);
    } while (value);
}

static void pb_w_tag(pb_writer_t *w, uint32_t field, uint32_t wire)
{
    pb_w_varint(w, ((uint64_t)field << 3) | wire);
}

static void pb_w_fixed32(pb_writer_t *w, uint32_t value)
{
    pb_w_byte(w, value & 0xFF);
    pb_w_byte(w, (value >> 8) & 0xFF);
    pb_w_byte(w, (value >> 16) & 0xFF);
    pb_w_byte(w, (value >> 24) & 0xFF);
}

static void pb_w_uint32_field(pb_writer_t *w, uint32_t field, uint32_t value)
{
    pb_w_tag(w, field, PB_WIRE_VARINT);
    pb_w_varint(w, value);
}

static void pb_w_bool_field(pb_writer_t *w, uint32_t field, bool value)
{
    pb_w_tag(w, field, PB_WIRE_VARINT);
    pb_w_varint(w, value ? 1 : 0);
}

static void pb_w_fixed32_field(pb_writer_t *w, uint32_t field, uint32_t value)
{
    pb_w_tag(w, field, PB_WIRE_32BIT);
    pb_w_fixed32(w, value);
}

static void pb_w_bytes_field(pb_writer_t *w, uint32_t field,
                             const uint8_t *data, size_t len)
{
    pb_w_tag(w, field, PB_WIRE_LEN);
    pb_w_varint(w, len);
    for (size_t i = 0; i < len; i++) {
        pb_w_byte(w, data[i]);
    }
}

/* Build the meshtastic.Data sub-message for a text payload. */
static size_t build_text_data(uint8_t *buf, size_t cap, const char *text, bool want_response)
{
    pb_writer_t w = { buf, cap, 0, false };
    pb_w_uint32_field(&w, DATA_PORTNUM, MESHTASTIC_PORTNUM_TEXT_MESSAGE);
    pb_w_bytes_field(&w, DATA_PAYLOAD, (const uint8_t *)text, strlen(text));
    if (want_response) {
        pb_w_bool_field(&w, DATA_WANT_RESPONSE, true);
    }
    return w.overflow ? 0 : w.len;
}

/* Build the meshtastic.MeshPacket sub-message wrapping a Data payload. */
static size_t build_meshpacket(uint8_t *buf, size_t cap,
                               const meshtastic_text_packet_t *packet,
                               const uint8_t *data, size_t data_len)
{
    pb_writer_t w = { buf, cap, 0, false };
    pb_w_fixed32_field(&w, MP_TO, packet->to);
    if (packet->channel) {
        pb_w_uint32_field(&w, MP_CHANNEL, packet->channel);
    }
    pb_w_bytes_field(&w, MP_DECODED, data, data_len);
    if (packet->id) {
        pb_w_fixed32_field(&w, MP_ID, packet->id);
    }
    if (packet->want_ack) {
        pb_w_bool_field(&w, MP_WANT_ACK, true);
    }
    return w.overflow ? 0 : w.len;
}

/* Prepend the 4-byte Stream API header in front of a ToRadio payload. */
static esp_err_t frame_payload(const uint8_t *payload, size_t payload_len,
                               uint8_t *out, size_t out_size, size_t *out_len)
{
    if (payload_len == 0 || payload_len > MESHTASTIC_FRAME_MAX_PAYLOAD) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (out_size < payload_len + MESHTASTIC_FRAME_HEADER_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    out[0] = MESHTASTIC_FRAME_START1;
    out[1] = MESHTASTIC_FRAME_START2;
    out[2] = (uint8_t)((payload_len >> 8) & 0xFF);
    out[3] = (uint8_t)(payload_len & 0xFF);
    memcpy(out + MESHTASTIC_FRAME_HEADER_LEN, payload, payload_len);
    *out_len = payload_len + MESHTASTIC_FRAME_HEADER_LEN;
    return ESP_OK;
}

esp_err_t meshtastic_encode_text_frame(const meshtastic_text_packet_t *packet,
                                       uint8_t *out, size_t out_size,
                                       size_t *out_len)
{
    uint8_t data_buf[300];
    uint8_t packet_buf[340];
    uint8_t toradio_buf[346];

    if (!packet || !packet->text || !out || !out_len) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(packet->text) > 237) {
        /* Meshtastic limits a text payload to ~237 bytes. */
        return ESP_ERR_INVALID_SIZE;
    }

    size_t data_len = build_text_data(data_buf, sizeof(data_buf), packet->text, false);
    if (data_len == 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t packet_len = build_meshpacket(packet_buf, sizeof(packet_buf), packet,
                                         data_buf, data_len);
    if (packet_len == 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    pb_writer_t w = { toradio_buf, sizeof(toradio_buf), 0, false };
    pb_w_bytes_field(&w, TR_PACKET, packet_buf, packet_len);
    if (w.overflow) {
        return ESP_ERR_INVALID_SIZE;
    }

    return frame_payload(toradio_buf, w.len, out, out_size, out_len);
}

esp_err_t meshtastic_encode_want_config_frame(uint32_t config_id,
                                              uint8_t *out, size_t out_size,
                                              size_t *out_len)
{
    uint8_t toradio_buf[16];

    if (!out || !out_len) {
        return ESP_ERR_INVALID_ARG;
    }

    pb_writer_t w = { toradio_buf, sizeof(toradio_buf), 0, false };
    pb_w_uint32_field(&w, TR_WANT_CONFIG_ID, config_id);
    if (w.overflow) {
        return ESP_ERR_INVALID_SIZE;
    }

    return frame_payload(toradio_buf, w.len, out, out_size, out_len);
}

esp_err_t meshtastic_encode_heartbeat_frame(uint8_t *out, size_t out_size,
                                            size_t *out_len)
{
    uint8_t toradio_buf[8];

    if (!out || !out_len) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Heartbeat is an empty embedded message: just the field tag + length 0. */
    pb_writer_t w = { toradio_buf, sizeof(toradio_buf), 0, false };
    pb_w_bytes_field(&w, TR_HEARTBEAT, NULL, 0);
    if (w.overflow) {
        return ESP_ERR_INVALID_SIZE;
    }

    return frame_payload(toradio_buf, w.len, out, out_size, out_len);
}

void meshtastic_format_node_id(uint32_t num, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    snprintf(out, out_size, "!%08x", (unsigned int)num);
}

const char *meshtastic_portnum_to_string(uint32_t portnum)
{
    switch (portnum) {
    case MESHTASTIC_PORTNUM_TEXT_MESSAGE:
        return "text";
    case MESHTASTIC_PORTNUM_REMOTE_HARDWARE:
        return "remote_hardware";
    case MESHTASTIC_PORTNUM_POSITION:
        return "position";
    case MESHTASTIC_PORTNUM_NODEINFO:
        return "nodeinfo";
    case MESHTASTIC_PORTNUM_ROUTING:
        return "routing";
    case MESHTASTIC_PORTNUM_ADMIN:
        return "admin";
    case MESHTASTIC_PORTNUM_WAYPOINT:
        return "waypoint";
    case MESHTASTIC_PORTNUM_TELEMETRY:
        return "telemetry";
    case MESHTASTIC_PORTNUM_TRACEROUTE:
        return "traceroute";
    default:
        return "other";
    }
}

/* Exposed for the service layer to decode telemetry payloads on demand. */
void meshtastic_decode_telemetry_payload(const uint8_t *data, size_t len,
                                         meshtastic_device_metrics_t *out,
                                         bool *has_metrics)
{
    if (!data || !out || !has_metrics) {
        return;
    }
    memset(out, 0, sizeof(*out));
    *has_metrics = false;
    decode_telemetry(data, len, out, has_metrics);
}

void meshtastic_decode_position_payload(const uint8_t *data, size_t len,
                                        meshtastic_position_t *out)
{
    if (!data || !out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    decode_position(data, len, out);
}

void meshtastic_decode_user_payload(const uint8_t *data, size_t len,
                                    meshtastic_user_t *out)
{
    if (!data || !out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    decode_user(data, len, out);
}
