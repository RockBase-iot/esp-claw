/*
 * SPDX-FileCopyrightText: 2026 Chengdu RockBase Technology Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cmd_cap_meshtastic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "argtable3/argtable3.h"
#include "cap_meshtastic.h"
#include "claw_cap.h"
#include "esp_console.h"
#include "meshtastic_proto.h"

static struct {
    struct arg_str *action;
    struct arg_str *text;
    struct arg_str *dest;
    struct arg_int *channel;
    struct arg_lit *ack;
    struct arg_end *end;
} mesh_args;

static uint32_t mesh_parse_dest(const char *s, bool *is_broadcast)
{
    *is_broadcast = true;
    if (!s || !s[0] || strcmp(s, "broadcast") == 0 || strcmp(s, "all") == 0) {
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
    return MESHTASTIC_ADDR_BROADCAST;
}

static int mesh_func(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&mesh_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, mesh_args.end, argv[0]);
        return 1;
    }

    const char *action = mesh_args.action->count ? mesh_args.action->sval[0] : "status";
    char output[1024] = {0};

    if (strcmp(action, "send") == 0) {
        if (!mesh_args.text->count || !mesh_args.text->sval[0][0]) {
            printf("Usage: mesh send <text> [--dest !id] [--ch n] [--ack]\n");
            return 1;
        }
        bool is_broadcast = true;
        uint32_t dest = MESHTASTIC_ADDR_BROADCAST;
        if (mesh_args.dest->count) {
            dest = mesh_parse_dest(mesh_args.dest->sval[0], &is_broadcast);
        }
        uint32_t channel = mesh_args.channel->count ? (uint32_t)mesh_args.channel->ival[0] : 0;
        bool want_ack = mesh_args.ack->count > 0;
        esp_err_t err = cap_meshtastic_send_text(dest, channel, want_ack,
                                                 mesh_args.text->sval[0]);
        printf("send: %s\n", esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }

    if (strcmp(action, "config") == 0) {
        esp_err_t err = cap_meshtastic_request_config();
        printf("config request: %s\n", esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }

    /* Default actions route through the registered tools for parity. */
    const char *cap_name = NULL;
    if (strcmp(action, "nodes") == 0) {
        cap_name = "meshtastic_list_nodes";
    } else if (strcmp(action, "messages") == 0) {
        cap_name = "meshtastic_get_messages";
    } else {
        cap_name = "meshtastic_get_status";
    }

    esp_err_t err = claw_cap_call(cap_name, "{}", NULL, output, sizeof(output));
    if (err != ESP_OK) {
        printf("%s failed: %s\n", action, esp_err_to_name(err));
        return 1;
    }
    printf("%s\n", output);
    return 0;
}

void register_cap_meshtastic(void)
{
    mesh_args.action = arg_str0(NULL, NULL, "<action>",
                                "status | nodes | messages | send | config");
    mesh_args.text = arg_str0(NULL, NULL, "<text>", "message text for 'send'");
    mesh_args.dest = arg_str0(NULL, "dest", "<id>", "destination node id (default broadcast)");
    mesh_args.channel = arg_int0(NULL, "ch", "<n>", "channel index (default 0)");
    mesh_args.ack = arg_lit0(NULL, "ack", "request delivery acknowledgement");
    mesh_args.end = arg_end(4);

    const esp_console_cmd_t mesh_cmd = {
        .command = "mesh",
        .help = "Meshtastic bridge operations.\n"
        "Examples:\n"
        "  mesh status\n"
        "  mesh nodes\n"
        "  mesh messages\n"
        "  mesh send \"hello mesh\"\n"
        "  mesh send \"hi node\" --dest !aabbccdd --ch 0 --ack\n"
        "  mesh config\n",
        .func = mesh_func,
        .argtable = &mesh_args,
    };

    ESP_ERROR_CHECK(esp_console_cmd_register(&mesh_cmd));
}
