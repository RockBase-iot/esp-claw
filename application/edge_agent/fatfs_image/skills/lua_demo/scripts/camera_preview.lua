local board_manager = require("board_manager")
local camera = require("camera")
local delay = require("delay")
local display = require("display")
local image = require("image")

local TAG = "[camera_preview]"
local FRAME_TIMEOUT_MS = 3000
local FRAME_INTERVAL_MS = 30
local SCRIPT_ARGS = rawget(_G, "args") or {}
local PREVIEW_FRAME_COUNT = tonumber(SCRIPT_ARGS.frames) or 300 -- Set to 0 for continuous preview.
-- Prefer native RGB565 formats to avoid JPEG decode overhead in preview.
-- IMPORTANT: list the big-endian variant ("RGBR" = V4L2_PIX_FMT_RGB565X) before
-- the little-endian one ("RGBP" = V4L2_PIX_FMT_RGB565). Many DVP sensors (e.g.
-- OV5640) emit RGB565 big-endian natively; without the optional hardware byte
-- swap (CONFIG_ESP_VIDEO_ENABLE_SWAP_BYTE) the driver streams those native
-- bytes unchanged. Requesting "RGBP" then mislabels big-endian data as
-- little-endian, so display.draw_image swaps the bytes and the preview shows
-- corrupted (rainbow) colours. Asking for the sensor-native endianness first
-- keeps the FOURCC label consistent with the actual byte order. The list still
-- falls back to "RGBP" for genuinely little-endian sensors that don't expose
-- "RGBR".
local DEFAULT_FORMATS = { "RGBR", "RGBP", "YUYV", "UYVY", "JPEG", "YU12" }

local camera_started = false
local display_started = false

local function cleanup()
    if display_started then
        pcall(display.end_frame)
        pcall(display.deinit)
        display_started = false
    end
    if camera_started then
        pcall(camera.close)
        camera_started = false
    end
end

local function panel_if_name(panel_if)
    if panel_if == board_manager.PANEL_IF_MIPI_DSI then
        return "mipi_dsi"
    end
    if panel_if == board_manager.PANEL_IF_RGB then
        return "rgb"
    end
    return "io"
end

local function camera_format_list()
    if type(SCRIPT_ARGS.format) == "string" and SCRIPT_ARGS.format ~= "" then
        return { SCRIPT_ARGS.format }
    end
    if type(SCRIPT_ARGS.formats) == "table" then
        return SCRIPT_ARGS.formats
    end
    return DEFAULT_FORMATS
end

local function draw_preview_frame(frame, lcd_w, lcd_h)
    -- Convert non-JPEG camera frames to RGB565 little-endian through the image
    -- module before drawing. The image module correctly handles the sensor's
    -- native byte order (OV5640 emits RGB565 big-endian / "RGBR"), producing a
    -- properly byte-ordered RGB565LE frame. Passing the raw big-endian camera
    -- frame straight to display.draw_image renders it as a corrupted rainbow
    -- pattern, so we mirror the proven take_picture pipeline here. The resize
    -- also pre-scales to fit the LCD, which keeps per-frame work low.
    local draw_src = frame
    local resized = nil
    local info = frame:info()
    local fmt = info.pixel_format
    if fmt ~= "JPEG" and fmt ~= "MJPG" and info.width > 0 and info.height > 0 then
        local ratio = math.min(lcd_w / info.width, lcd_h / info.height)
        local target_w = math.max(1, math.floor(info.width * ratio))
        local target_h = math.max(1, math.floor(info.height * ratio))
        resized = image.resize(frame, {
            width = target_w, height = target_h,
            format = image.RGB565, filter = "nearest",
        })
        draw_src = resized:data()
    end
    display.begin_frame({ clear = true, color = "black" })
    local draw_w, draw_h = display.draw_pixels(0, 0, draw_src, {
        mode = "fit",
        width = resized and resized:info().width or info.width,
        height = resized and resized:info().height or info.height,
        dst_width = lcd_w,
        dst_height = lcd_h,
        format = "rgb565",
    })
    display.present()
    display.end_frame()
    if resized then
        resized:release()
    end
    return draw_w, draw_h
end

local panel_handle, io_handle, lcd_width, lcd_height, panel_if = board_manager.get_display_lcd_params("display_lcd")
if not panel_handle then
    print(TAG .. " ERROR: get_display_lcd_params failed: " .. tostring(io_handle))
    return
end

local camera_paths, path_err = board_manager.get_camera_paths()
if not camera_paths then
    print(TAG .. " ERROR: get_camera_paths failed: " .. tostring(path_err))
    return
end

local ok, err = pcall(display.init, panel_handle, io_handle, lcd_width, lcd_height, panel_if)
if not ok then
    print(TAG .. " ERROR: display.init failed: " .. tostring(err))
    return
end
display_started = true

local requested_formats = camera_format_list()
print(string.format("%s request formats=%s frames=%d", TAG, table.concat(requested_formats, ","), PREVIEW_FRAME_COUNT))

ok, err = pcall(camera.open, camera_paths.dev_path, {
    format = requested_formats,
    width = 320, height = 240, nearest = true,
})
if not ok then
    print(TAG .. " ERROR: camera.open failed: " .. tostring(err))
    cleanup()
    return
end
camera_started = true
print(string.format("%s using format=%s", TAG, camera.info().pixel_format))

local run_ok, run_err = xpcall(function()
    local stream = camera.info()
    local lcd_w = display.width
    local lcd_h = display.height
    local frames = 0

    print(string.format("%s start camera=%dx%d format=%s lcd=%dx%d panel_if=%s",
        TAG, stream.width, stream.height, tostring(stream.pixel_format), lcd_w, lcd_h, panel_if_name(panel_if)))

    while PREVIEW_FRAME_COUNT == 0 or frames < PREVIEW_FRAME_COUNT do
        local frame <close> = camera.get_frame(FRAME_TIMEOUT_MS)
        local draw_w, draw_h = draw_preview_frame(frame, lcd_w, lcd_h)
        frames = frames + 1

        if frames == 1 or frames % 30 == 0 then
            local info = frame:info()
            print(string.format("%s frame=%d source=%dx%d %s bytes=%d drawn=%dx%d",
                TAG, frames, info.width, info.height, tostring(info.pixel_format), info.bytes, draw_w, draw_h))
        end

        if FRAME_INTERVAL_MS > 0 then
            delay.delay_ms(FRAME_INTERVAL_MS)
        end
    end

    print(string.format("%s stopped after %d frame(s)", TAG, frames))
end, debug.traceback)

cleanup()

if not run_ok then
    error(run_err)
end
