# Lua BQ27220

This skill describes how to read `BQ27220` battery data from Lua using the existing `i2c` module.
When a request mentions `bq27220`, `battery voltage`, `battery current`, or `battery percentage`, use this module by default.

## How to call
- Import it with `local bq27220 = require("lib_bq27220")`
- Create or reuse an I2C bus, then open the gauge with `local gauge = bq27220.new({ bus = bus })`
- Read all three values with `local sample = gauge:read()`
- Or read one value at a time:
  - `gauge:read_voltage_mv()`
  - `gauge:read_current_ma()`
  - `gauge:read_soc()`
- Call `gauge:close()` when needed

## Options table
| Field      | Type    | Meaning                                    |
|------------|---------|--------------------------------------------|
| `port`     | integer | I2C port number                            |
| `sda`      | integer | SDA GPIO number                            |
| `scl`      | integer | SCL GPIO number                            |
| `freq_hz`  | integer | I2C clock in Hz (default `400000`)         |
| `frequency`| integer | Alias of `freq_hz`                         |
| `addr`     | integer | BQ27220 7-bit I2C address (default `0x55`) |
| `bus`      | userdata| Existing `i2c` bus handle, recommended     |

## Data format
- `sample.voltage_mv`
- `sample.current_ma`
- `sample.soc`

## Example
```lua
local bq27220 = require("lib_bq27220")
local i2c = require("i2c")

local bus = i2c.new(0, 14, 13, 400000)
local gauge = bq27220.new({
    bus = bus,
})

local sample = gauge:read()
print(sample.voltage_mv, sample.current_ma, sample.soc)
gauge:close()
bus:close()
```
