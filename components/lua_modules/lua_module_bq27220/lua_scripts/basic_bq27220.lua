local bq27220 = require("lib_bq27220")
local delay = require("delay")
local i2c = require("i2c")

local a = type(args) == "table" and args or {}
local function int_arg(k, default)
    local v = a[k]
    if type(v) == "number" then
        return math.floor(v)
    end
    return default
end

local I2C_ADDR = int_arg("addr", 0x55)
local FREQ_HZ = int_arg("freq_hz", 400000)
local SAMPLE_COUNT = int_arg("samples", 20)
local INTERVAL_MS = int_arg("interval_ms", 1000)

local gauge
local bus

local function cleanup()
    if gauge then
        pcall(function()
            gauge:close()
        end)
        gauge = nil
    end
    if bus then
        pcall(function()
            bus:close()
        end)
        bus = nil
    end
end

local function run()
    local port = int_arg("port", 0)
    local sda = int_arg("sda", 14)
    local scl = int_arg("scl", 13)
    bus = a.bus or i2c.new(port, sda, scl, FREQ_HZ)
    print(string.format(
        "[bq27220] opening addr=0x%02X freq=%d",
        I2C_ADDR, FREQ_HZ
    ))
    gauge = bq27220.new({
        bus = bus,
        addr = I2C_ADDR,
        freq_hz = FREQ_HZ,
    })

    for i = 1, SAMPLE_COUNT do
        local sample = gauge:read()
        print(string.format(
            "[bq27220] #%d soc=%d%% voltage=%dmV current=%dmA",
            i,
            sample.soc,
            sample.voltage_mv,
            sample.current_ma
        ))
        delay.delay_ms(INTERVAL_MS)
    end
end

local ok, err = xpcall(run, debug.traceback)
cleanup()
if not ok then
    error(err)
end
