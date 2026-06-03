local board_manager = require("board_manager")
local camera = require("camera")
local display = require("display")
local delay = require("delay")
local image = require("image")

local FRAME_TIMEOUT_MS = 1000
local FRAME_INTERVAL_MS = 30

local function close_camera()
    local ok, err = pcall(camera.close)
    if not ok then
        print("[camera_preview_demo] WARN: close failed: " .. tostring(err))
    end
end

local function cleanup_display()
    pcall(display.end_frame)
    pcall(display.deinit)
end

local camera_paths, path_err = board_manager.get_camera_paths()
if not camera_paths then
    print("[camera_preview_demo] ERROR: get_camera_paths failed: " .. tostring(path_err))
    return
end

local panel_handle, io_handle, lcd_w, lcd_h, panel_if = board_manager.get_display_lcd_params("display_lcd")
if not panel_handle then
    print("[camera_preview_demo] ERROR: get_display_lcd_params failed: " .. tostring(io_handle))
    return
end

local panel_if_name = "io"
if panel_if == board_manager.PANEL_IF_MIPI_DSI then
    panel_if_name = "mipi_dsi"
elseif panel_if == board_manager.PANEL_IF_RGB then
    panel_if_name = "rgb"
end

local ok, err = pcall(display.init, panel_handle, io_handle, lcd_w, lcd_h, panel_if)
if not ok then
    print("[camera_preview_demo] ERROR: display.init failed: " .. tostring(err))
    return
end

local opened, open_err = pcall(camera.open, camera_paths.dev_path)
if not opened then
    print("[camera_preview_demo] ERROR: " .. tostring(open_err))
    cleanup_display()
    return
end

local info_ok, info_or_err = pcall(camera.info)
if not info_ok then
    print("[camera_preview_demo] ERROR: " .. tostring(info_or_err))
    close_camera()
    cleanup_display()
    return
end

local pixel_format = tostring(info_or_err.pixel_format)
local width = display.width
local height = display.height

local function draw_frame(frame)
    local draw_src = frame
    local resized = nil
    local info = frame:info()
    if info.pixel_format ~= "JPEG" and info.pixel_format ~= "MJPG" and info.width > 0 and info.height > 0 then
        local ratio = math.min(width / info.width, height / info.height)
        local target_w = math.max(1, math.floor(info.width * ratio))
        local target_h = math.max(1, math.floor(info.height * ratio))
        resized = image.resize(frame, {
            width = target_w,
            height = target_h,
            format = image.RGB565,
            filter = "nearest",
        })
        draw_src = resized:data()
    end
    display.draw_pixels(0, 0, draw_src, {
        mode = "fit",
        width = resized and resized:info().width or info.width,
        height = resized and resized:info().height or info.height,
        dst_width = width,
        dst_height = height,
        format = "rgb565",
    })
    display.present()
    if resized then
        resized:release()
    end
end

print(string.format(
    "[camera_preview_demo] preview start camera=%dx%d format=%s lcd=%dx%d panel_if=%s",
    info_or_err.width, info_or_err.height, pixel_format, width, height, panel_if_name
))

display.begin_frame({ clear = true, color = "black" })

local MAX_CONSECUTIVE_ERRORS = 5
local consecutive_errors = 0

while true do
    local frame_ok, frame_or_err = pcall(camera.get_frame, FRAME_TIMEOUT_MS)
    if not frame_ok then
        consecutive_errors = consecutive_errors + 1
        print(string.format("[camera_preview_demo] WARN: get_frame failed (%d/%d): %s",
            consecutive_errors, MAX_CONSECUTIVE_ERRORS, tostring(frame_or_err)))
        if consecutive_errors >= MAX_CONSECUTIVE_ERRORS then
            print("[camera_preview_demo] ERROR: too many consecutive get_frame failures, stopping")
            break
        end
        delay.delay_ms(FRAME_INTERVAL_MS)
    else
        consecutive_errors = 0

        local draw_ok, draw_err = pcall(draw_frame, frame_or_err)

        -- The camera frame is an image.frame userdata; it must be released with
        -- frame:release() so the underlying V4L2 buffer is returned to the
        -- driver. (There is no camera.release_frame helper.)
        pcall(frame_or_err.release, frame_or_err)

        if not draw_ok then
            print("[camera_preview_demo] ERROR: draw failed: " .. tostring(draw_err))
            break
        end

        delay.delay_ms(FRAME_INTERVAL_MS)
    end
end

close_camera()
cleanup_display()
