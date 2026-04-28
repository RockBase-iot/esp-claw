local i2c = require("i2c")

local M = {}

local DEFAULT_ADDR = 0x55
local DEFAULT_FREQ_HZ = 400000
local REG_VOLTAGE = 0x08
local REG_CURRENT = 0x0C
local REG_SOC = 0x2C

local mt = {}
mt.__index = mt

local function u16le(data)
    local lo = string.byte(data, 1) or 0
    local hi = string.byte(data, 2) or 0
    return lo | (hi << 8)
end

local function i16le(data)
    local v = u16le(data)
    if v >= 0x8000 then
        return v - 0x10000
    end
    return v
end

local function new_device_from_opts(opts)
    local bus
    local owns_bus = false

    if opts.bus ~= nil then
        bus = opts.bus
    else
        bus = i2c.new(
            assert(opts.port, "bq27220.new: missing 'port'"),
            assert(opts.sda, "bq27220.new: missing 'sda'"),
            assert(opts.scl, "bq27220.new: missing 'scl'"),
            opts.frequency or opts.freq_hz or DEFAULT_FREQ_HZ
        )
        owns_bus = true
    end

    local dev = bus:device(opts.addr or DEFAULT_ADDR, 0)
    return bus, dev, owns_bus
end

function M.new(opts)
    opts = type(opts) == "table" and opts or {}
    local bus, dev, owns_bus = new_device_from_opts(opts)
    return setmetatable({
        _bus = bus,
        _dev = dev,
        _owns_bus = owns_bus,
        _addr = opts.addr or DEFAULT_ADDR,
    }, mt)
end

function mt:address()
    return self._addr
end

function mt:read_voltage_mv()
    return u16le(self._dev:read(2, REG_VOLTAGE))
end

function mt:read_current_ma()
    return i16le(self._dev:read(2, REG_CURRENT))
end

function mt:read_soc()
    return u16le(self._dev:read(2, REG_SOC))
end

function mt:read()
    return {
        voltage_mv = self:read_voltage_mv(),
        current_ma = self:read_current_ma(),
        soc = self:read_soc(),
    }
end

function mt:close()
    if self._dev then
        self._dev:close()
        self._dev = nil
    end
    if self._owns_bus and self._bus then
        self._bus:close()
        self._bus = nil
    end
end

function mt:__gc()
    pcall(function()
        self:close()
    end)
end

return M
