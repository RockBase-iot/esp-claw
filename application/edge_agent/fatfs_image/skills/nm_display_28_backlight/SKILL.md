---
{
  "name": "nm_display_28_backlight",
  "description": "Adjust the NM-Display-28inch LCD backlight brightness: dim or brighten the screen, set brightness percent, turn the backlight on/off, or restore full brightness.",
  "metadata": {
    "cap_groups": [
      "cap_lua"
    ],
    "manage_mode": "readonly"
  }
}
---

# NM-Display-28inch Backlight

Use this skill for LCD backlight and brightness requests on the NM-Display-28inch board: dim screen, brighten screen, set screen brightness, backlight off, screen off, turn display backlight on, full brightness, or `背光 30%`.

This skill changes only the LEDC PWM backlight duty cycle. It does not draw screen content.

## Tool Call

Always use synchronous `lua_run_script`:

```json
{"path":"/fatfs/skills/lua_demo/scripts/nm_display_28_backlight.lua","args":{}}
```

Do not activate display module docs, do not write LEDC/GPIO scripts, and do not use any screen-clear command to mean screen off.

## Args

- `percent`: integer `0..100`, target brightness.
- `mode`: `on` means `100`, `off` means `0`, `set` uses `percent`.

## Examples

Set 30% brightness:
```json
{"path":"/fatfs/skills/lua_demo/scripts/nm_display_28_backlight.lua","args":{"percent":30}}
```

Dim to 5%:
```json
{"path":"/fatfs/skills/lua_demo/scripts/nm_display_28_backlight.lua","args":{"percent":5}}
```

Turn screen/backlight off:
```json
{"path":"/fatfs/skills/lua_demo/scripts/nm_display_28_backlight.lua","args":{"mode":"off"}}
```

Full brightness:
```json
{"path":"/fatfs/skills/lua_demo/scripts/nm_display_28_backlight.lua","args":{"mode":"on"}}
```

## Response Rules

Run exactly one `lua_run_script` call. If it succeeds, confirm the brightness value. If it fails, report the script error and stop.
