-- --------------------------------------------------------------
-- Capture a timestamped sequence of JPEG frames from the board camera.
--
-- This script is meant to run as an async `thread` job alongside an audio
-- recording so that the frames and the WAV cover the same wall-clock window.
-- It writes frame_00001.jpg ... and a manifest.json describing the result.
-- --------------------------------------------------------------

-- 1. Requires
local arg_schema = require("arg_schema")
local camera = require("camera")
local delay = require("delay")
local image = require("image")
local json = require("json")
local storage = require("storage")

-- 2. Constants
local DEFAULT_DURATION_MS = 5000
local DEFAULT_FPS = 5
local DEFAULT_TIMEOUT_MS = 3000
local DEFAULT_MAX_WIDTH = 480
local DEFAULT_MAX_HEIGHT = 360
local DEFAULT_MIN_FREE_BYTES = 256 * 1024
-- Skip at most this many consecutive failed frames (e.g. transient out-of-memory
-- under concurrent audio load) before giving up on the video stream entirely.
local MAX_CONSECUTIVE_FAILURES = 5

-- 3. Args
local function raw_arg(name, default)
  if type(args) == "table" and args[name] ~= nil then
    return args[name]
  end
  return default
end

local ARG_SCHEMA = {
  duration_ms    = arg_schema.int({ default = DEFAULT_DURATION_MS, min = 100 }),
  fps            = arg_schema.int({ default = DEFAULT_FPS, min = 1, max = 30 }),
  timeout_ms     = arg_schema.int({ default = DEFAULT_TIMEOUT_MS, min = 0 }),
  max_width      = arg_schema.int({ default = DEFAULT_MAX_WIDTH, min = 0 }),
  max_height     = arg_schema.int({ default = DEFAULT_MAX_HEIGHT, min = 0 }),
  min_free_bytes = arg_schema.int({ default = DEFAULT_MIN_FREE_BYTES, min = 0 }),
}

local ctx = arg_schema.parse(args, ARG_SCHEMA)
ctx.dir = raw_arg("dir", "")
ctx.space_path = raw_arg("space_path", "")
ctx.dev_path = raw_arg("dev_path", "")

-- 4. Validation
if type(ctx.dir) ~= "string" or ctx.dir == "" then
  error("record_frames: 'dir' (absolute output directory) is required")
end
if type(ctx.dev_path) ~= "string" or ctx.dev_path == "" then
  error("record_frames: 'dev_path' (camera device path) is required")
end
if string.find(ctx.dir, "%.%.") then
  error("record_frames: 'dir' must not contain '..'")
end

-- 5. State / cleanup
local camera_opened = false

-- Progress is hoisted to module scope so the manifest can always be written --
-- even from the epilogue when the job is stopped mid-loop. A cooperative stop
-- (thread.stop from record_av) is raised as an error by the next yielding Lua
-- call; if that call is not pcall-protected the error unwinds past the manifest
-- write, which is exactly why an interrupted run could end with "no result".
local progress = {
  saved = 0,
  total_bytes = 0,
  width = 0,
  height = 0,
  pixel_format = "",
  fps = ctx.fps,
  stopped_reason = "pending",
  dir = ctx.dir,
  manifest_path = nil,
  written = false,
}

local function cap_stop_reason(err)
  if string.find(tostring(err), "stopped", 1, true) then
    return "stopped"
  end
  return "capture_error"
end

local function write_manifest()
  if progress.written or not progress.manifest_path then
    return
  end
  local manifest = {
    frames = progress.saved,
    bytes = progress.total_bytes,
    width = progress.width,
    height = progress.height,
    pixel_format = progress.pixel_format,
    fps = progress.fps,
    stopped_reason = progress.stopped_reason,
    dir = progress.dir,
  }
  local ok, err = pcall(storage.write_file, progress.manifest_path, json.encode(manifest))
  if ok then
    progress.written = true
  else
    print("[record_frames] WARN: manifest write failed: " .. tostring(err))
  end
end

local function cleanup()
  if camera_opened then
    local ok, err = pcall(camera.close)
    if not ok then
      print("[record_frames] WARN: camera.close failed: " .. tostring(err))
    end
    camera_opened = false
  end
end

local function free_bytes()
  if ctx.space_path == "" then
    return nil
  end
  local ok, info = pcall(storage.get_free_space, ctx.space_path)
  if ok and type(info) == "table" and info.free then
    return info.free
  end
  return nil
end

local function frame_path(index)
  return storage.join_path(ctx.dir, string.format("frame_%05d.jpg", index))
end

local function is_oom(err)
  return string.find(tostring(err), "NO_MEM", 1, true) ~= nil
end

-- Resize the borrowed camera frame down to the configured cap (RGB565), unless
-- the frame is already JPEG. The returned image (if any) is a to-be-closed value.
local function maybe_resize(frame, info)
  if ctx.max_width > 0 and ctx.max_height > 0
      and info.pixel_format ~= "JPEG" and info.pixel_format ~= "MJPG" then
    local tw = math.min(info.width, ctx.max_width)
    local th = math.min(info.height, ctx.max_height)
    return image.resize(frame, { width = tw, height = th,
                                 format = image.RGB565, filter = "nearest" })
  end
  return nil
end

-- Capture one frame and write it to frame_path(out_idx). Every intermediate
-- image buffer uses <close> so it is freed on EVERY exit path -- including an
-- out-of-memory error during JPEG encoding. (The previous explicit release()
-- was skipped when convert() raised, leaking the resized buffer and starving
-- the following frames.)
local function capture_frame(out_idx)
  return pcall(function()
    local frame <close> = camera.get_frame(ctx.timeout_ms)
    local info = frame:info()
    local save_src <close> = maybe_resize(frame, info)
    local jpeg <close> = image.convert(save_src or frame, image.JPEG)
    image.save_file(frame_path(out_idx), jpeg)
  end)
end

-- 6. Run
local function run()
  if not storage.exists(ctx.dir) then
    storage.mkdir(ctx.dir)
  end

  -- Drop a stale manifest so a partial result is never mistaken for a fresh one.
  progress.manifest_path = storage.join_path(ctx.dir, "manifest.json")
  if storage.exists(progress.manifest_path) then
    pcall(storage.remove, progress.manifest_path)
  end

  local opened, open_err = pcall(camera.open, ctx.dev_path)
  if not opened then
    error("camera.open failed: " .. tostring(open_err))
  end
  camera_opened = true

  local stream = camera.info()
  progress.width = stream.width
  progress.height = stream.height
  progress.pixel_format = tostring(stream.pixel_format)
  print(string.format("[record_frames] stream: %dx%d format=%s",
    stream.width, stream.height, tostring(stream.pixel_format)))

  camera.flush()

  local interval_ms = math.floor(1000 / ctx.fps)
  if interval_ms < 1 then
    interval_ms = 1
  end
  local target_frames = math.max(1, math.floor(ctx.duration_ms / 1000 * ctx.fps + 0.5))

  local consec_fail = 0
  for i = 1, target_frames do
    local remaining = free_bytes()
    if remaining ~= nil and remaining < ctx.min_free_bytes then
      progress.stopped_reason = "low_space"
      print(string.format("[record_frames] stopping early: free=%d < min_free=%d",
        remaining, ctx.min_free_bytes))
      break
    end

    -- Frames are named by a contiguous output index (not the loop counter) so a
    -- skipped frame never leaves a gap that would break the AVI muxer.
    local out_idx = progress.saved + 1
    local ok, err = capture_frame(out_idx)
    if not ok and cap_stop_reason(err) ~= "stopped" and is_oom(err) then
      -- Transient out-of-memory under concurrent audio load: reclaim and retry.
      collectgarbage("collect")
      ok, err = capture_frame(out_idx)
    end

    if ok then
      consec_fail = 0
      local stat = storage.stat(frame_path(out_idx))
      if stat then
        progress.total_bytes = progress.total_bytes + stat.size
      end
      progress.saved = out_idx
    else
      pcall(storage.remove, frame_path(out_idx)) -- drop any partial frame
      if cap_stop_reason(err) == "stopped" then
        progress.stopped_reason = "stopped"
        print(string.format("[record_frames] stopped at frame %d: %s", out_idx, tostring(err)))
        break
      end
      consec_fail = consec_fail + 1
      print(string.format("[record_frames] frame %d failed (skipped %d/%d): %s",
        out_idx, consec_fail, MAX_CONSECUTIVE_FAILURES, tostring(err)))
      if consec_fail >= MAX_CONSECUTIVE_FAILURES then
        progress.stopped_reason = "capture_error"
        print("[record_frames] too many consecutive failures; stopping video capture")
        break
      end
    end

    if i < target_frames then
      -- A cooperative stop is delivered as an error from the next yielding call.
      -- delay.delay_ms() is the most likely landing point, so catch it here and
      -- break gracefully -- otherwise the error unwinds past the manifest write
      -- and record_av reports "no result".
      local ok_delay, derr = pcall(delay.delay_ms, interval_ms)
      if not ok_delay then
        progress.stopped_reason = cap_stop_reason(derr)
        print(string.format("[record_frames] stopped during interval: %s", tostring(derr)))
        break
      end
    end
  end

  if progress.stopped_reason == "pending" then
    progress.stopped_reason = "completed"
  end

  write_manifest()

  print(string.format("[record_frames] done: frames=%d bytes=%d reason=%s dir=%s",
    progress.saved, progress.total_bytes, progress.stopped_reason, ctx.dir))
end

-- 7. Epilogue
local ok, err = xpcall(run, debug.traceback)
cleanup()
-- Always persist whatever was captured. If the job was stopped by an unprotected
-- yielding call, run() unwound before writing the manifest; write it now so
-- record_av still gets a usable result.
if not progress.written then
  if progress.stopped_reason == "pending" then
    progress.stopped_reason = cap_stop_reason(err)
  end
  write_manifest()
end
if not ok then
  -- A cooperative user stop is the expected way this job ends once the manifest
  -- exists; only re-raise genuine capture failures.
  if cap_stop_reason(err) == "stopped" then
    print(string.format("[record_frames] stopped by user; manifest written (frames=%d)",
      progress.saved))
  else
    print("[record_frames] ERROR: " .. tostring(err))
    error(err)
  end
end
