# NM-Display-28inch Backlight Control

Controls the NM-Display-28inch on-board ST7789 LCD backlight (LEDC PWM, GPIO 6, 5 kHz). **Never write custom Lua scripts to manipulate LEDC / GPIO for brightness** — always use this script.

## When to use
When the user requests "dim screen / brighten screen / screen brightness / backlight X% / turn off backlight / screen off / turn off display / set screen off / full brightness / dim screen / set brightness / backlight off / set backlight to N%" or any other request targeting LCD **backlight brightness**. This is the **only correct entry** for turning the screen on/off — do not use a screen-clear command as a substitute for turning off the backlight.

Note: This skill only adjusts brightness (PWM duty cycle). It does not draw any content. For text / colors on screen, use the corresponding screen skill.

## How to use
**Must** use synchronous execution:

```
run_lua_script script="nm_display_28_backlight" args=<JSON object>
```

Do not `activate_skill lua_module_display` first — this script is self-contained. Do not attempt to manipulate `gpio` / `ledc` / `board_manager`.

### Parameters
- `percent` (integer 0..100): target brightness percentage. **Only required field** (unless using `mode=off`).
- `mode` (optional): `set` (default) | `off` (= percent=0) | `on` (= percent=100).

### Examples
- Set to 30%: `{"percent":30}`
- Full brightness: `{"percent":100}` or `{"mode":"on"}`
- Turn off backlight: `{"percent":0}` or `{"mode":"off"}`
- Half brightness: `{"mode":"set","percent":50}`

### Return
The script prints a line like `[nm_display_28_backlight] set 30%`. If the LEDC setting fails, it raises an error — it will **not** silently report success.
