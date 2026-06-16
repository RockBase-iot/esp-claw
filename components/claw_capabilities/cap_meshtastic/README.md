# cap_meshtastic

Meshtastic bridge capability. Connects an external Meshtastic-capable LoRa radio
(e.g. Heltec LoRa32 V3) to ESP-Claw over UART and speaks the Meshtastic **Stream API**
(protobuf), allowing the agent to read the mesh node database, receive text messages,
and transmit text back into the mesh.

## Features

- Full hand-rolled protobuf decoder for `FromRadio` (MeshPacket, MyNodeInfo, NodeInfo,
  DeviceMetadata, config-complete) and the embedded `Data`, `User`, `Position`,
  `DeviceMetrics` sub-messages.
- Protobuf encoder for `ToRadio` (text packets, `want_config_id`, heartbeat) with the
  4-byte Stream API framing header (`0x94 0xc3 <len_hi> <len_lo>`).
- Background RX task that reassembles frames, maintains a node database and a ring buffer
  of recent text messages, and publishes inbound text as router events on channel
  `meshtastic`.
- LLM-callable tools: `meshtastic_send_text`, `meshtastic_list_nodes`,
  `meshtastic_get_status`, `meshtastic_get_messages`, `meshtastic_request_config`.
- Console command `mesh` (status / nodes / messages / send / config).

## Radio configuration

Full protobuf parsing requires the Serial module in **PROTO** mode:

```bash
meshtastic --set serial.enabled true
meshtastic --set serial.mode PROTO
meshtastic --set serial.baud BAUD_115200
meshtastic --set serial.rxd 19
meshtastic --set serial.txd 20
meshtastic --reboot
```

## Wiring

UART is cross-connected (mainboard TX -> Heltec RX, mainboard RX -> Heltec TX).

| Mainboard | Default TX | Default RX |
| --- | --- | --- |
| NM-Display-28inch | GPIO43 | GPIO44 |
| NM-CYD-C5 | GPIO5 | GPIO4 |

Pins, UART port and baud rate are configurable under
`Component config -> Claw Meshtastic Capability` (`CONFIG_CAP_MESHTASTIC_*`).

## Interface reference

For the full interface reference (HTTP REST API `/api/mesh/*`, LLM tools, console
commands, C API, config and troubleshooting) see
[docs/meshtastic-interface.md](docs/meshtastic-interface.md).

## Programmatic API

See [include/cap_meshtastic.h](include/cap_meshtastic.h):

- `cap_meshtastic_set_uart_config()` — override UART pins/port/baud before start.
- `cap_meshtastic_register_group()` — register the capability group.
- `cap_meshtastic_send_text()` — send a text message.
- `cap_meshtastic_request_config()` — re-stream the node database.
- `cap_meshtastic_is_connected()` — link state.
