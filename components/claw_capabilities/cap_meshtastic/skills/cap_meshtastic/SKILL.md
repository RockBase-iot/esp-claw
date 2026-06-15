---
{
  "name": "cap_meshtastic",
  "description": "Bridge to a Meshtastic LoRa device over UART: list mesh nodes, read received messages, and send text into the mesh.",
  "metadata": {
    "cap_groups": [
      "cap_meshtastic"
    ],
    "manage_mode": "readonly"
  }
}
---

# Meshtastic Bridge

Use this skill to interact with a Meshtastic LoRa network through a radio (e.g. Heltec
LoRa32 V3) wired to the mainboard UART. The bridge speaks the Meshtastic Stream API
(protobuf) so it can read the node database, receive text messages, and transmit text
back into the mesh.

## When to use
- The user wants to send a message to the LoRa mesh or to a specific mesh node.
- The user asks who is on the mesh, node signal/battery/position, or recent mesh chatter.
- The user wants to refresh the list of known nodes.

## Available capabilities
- `meshtastic_send_text`: send a text message (broadcast or to one node).
- `meshtastic_list_nodes`: list known nodes with names, SNR, position and telemetry.
- `meshtastic_get_status`: link state, local node id, firmware version, node count.
- `meshtastic_get_messages`: recently received text messages (oldest first).
- `meshtastic_request_config`: ask the radio to re-stream its node database.

## Sending a message
Input for `meshtastic_send_text`:

```json
{
  "text": "hello mesh",
  "dest": "broadcast",
  "channel": 0,
  "want_ack": false
}
```

- `text` (required): message body, up to ~237 bytes.
- `dest` (optional): `"broadcast"` (default) or a node id like `"!aabbccdd"` (you may
  also pass the decimal node number).
- `channel` (optional): channel index, default `0` (primary channel).
- `want_ack` (optional): request a delivery acknowledgement.

## Reading the mesh
- `meshtastic_list_nodes` returns `{ "nodes": [...], "count": N }`. Each node has `id`,
  `num`, optional `long_name` / `short_name`, `snr`, `last_heard`, `position`
  (latitude/longitude/altitude) and `metrics` (battery, voltage, channel utilization).
- `meshtastic_get_messages` returns buffered inbound text with `from`, `channel`,
  `packet_id` and `text`.

## Inbound messages
Received text messages are pushed **proactively** to an IM conversation so the user is
notified immediately, without spending an LLM turn. The bridge delivers them via the
router rule `meshtastic_inbound_im_notify` (event `meshtastic_inbound`). When a new node
joins the mesh after the initial sync, a short `meshtastic_node_update` notification is
sent the same way.

The notification target is, in priority order:
1. an explicit target set through `cap_meshtastic_set_notify_target(channel, chat_id)`;
2. otherwise the most recent IM conversation that messaged the device (auto-learned).

`meshtastic_get_status` reports `notify_configured`, `notify_channel` and
`notify_chat_id` so you can confirm where notifications go. Buffered messages remain
available through `meshtastic_get_messages` regardless of notification delivery.

## Important setup note
Full protobuf parsing requires the Meshtastic **Serial module in PROTO mode** (not
TEXTMSG). Configure the radio with:

```bash
meshtastic --set serial.enabled true
meshtastic --set serial.mode PROTO
meshtastic --set serial.baud BAUD_115200
meshtastic --set serial.rxd 19
meshtastic --set serial.txd 20
meshtastic --reboot
```

Wiring is cross-connected (mainboard TX -> Heltec RX, mainboard RX -> Heltec TX). Pins
default to GPIO43/TX and GPIO44/RX (NM-Display-28inch) or GPIO5/TX and GPIO4/RX
(NM-CYD-C5).

## Error strings
- `Error: Meshtastic bridge is not running` — UART not started.
- `Error: 'text' is required` — missing message body.
- `Error: invalid JSON input` — malformed input.
- `Error: send failed (...)` — encode or UART write error.
