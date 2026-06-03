---
{
  "name": "camera_preview",
  "description": "Preview the board camera on the LCD display. Requires board_hardware_info skill.",
  "metadata": {
    "cap_groups": [
      "cap_lua"
    ],
    "manage_mode": "readonly"
  }
}
---

# Camera Preview

Use this skill when the user asks to preview the camera, show the camera feed, open the camera, or run a live camera display on a board with both camera and LCD display hardware.

Run exactly one async Lua script after reading `board_hardware_info`.

If `lua_run_script_async` returns an error, report that error directly to the user. Do not retry with another camera script unless the user explicitly asks.

## Tool Call

```json
{"path":"{CUR_SKILL_DIR}/scripts/camera_preview.lua","args":{}}
```

## Recommended Flow

1. Activate the `board_hardware_info` skill and confirm that both `camera` and `display_lcd` devices are listed.
2. Run `{CUR_SKILL_DIR}/scripts/camera_preview.lua` with `lua_run_script_async` and an empty `args` object.
3. Report the async job result or error directly to the user.