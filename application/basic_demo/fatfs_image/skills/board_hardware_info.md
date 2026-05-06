# Current Board Hardware: nm_cyd_c5

Read this skill before operating hardware, assigning GPIOs, or writing Lua and board-specific code.

## Rules
- Before operating any hardware, read this skill first.
- Before assigning a GPIO, check whether it is already occupied below.
- When writing Lua or board-specific code, use the listed device names instead of guessing hardware wiring.

## Board Summary
- Board: `nm_cyd_c5`
- Chip: `esp32c5`
- Version: `1.0.0`
- Manufacturer: `RockBase-IoT`
- Description: NM-CYD-C5 Cheap Yellow Display (ESP32-C5, 2.8 inch 320x240 ST7789 + XPT2046 touch)

## Device Inventory

### display_lcd
- Occupied IO:
  - `spi.cs` -> `GPIO23`
  - `spi.dc` -> `GPIO24`
  - `mosi` -> `GPIO7`
  - `miso` -> `GPIO2`
  - `sclk` -> `GPIO6`

### led_strip
- Occupied IO: none declared

### fs_sdcard
- Occupied IO:
  - `spi.cs` -> `GPIO10`

### lcd_touch
- Occupied IO:
  - `cs` -> `GPIO1`
  - `mosi` -> `GPIO7`
  - `miso` -> `GPIO2`
  - `sclk` -> `GPIO6`

## Notes
- If a device has no explicit IO mapping here, treat it as unknown instead of guessing.
