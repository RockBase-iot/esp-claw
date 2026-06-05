-- --------------------------------------------------------------
-- Record audio (WAV) and, when a camera is available, a synchronized
-- sequence of JPEG video frames covering the same wall-clock window.
--
-- Storage strategy:
--   * If an SD card is mounted (/sdcard), recordings go there.
--   * Otherwise they go to the on-flash storage root (/fatfs).
-- Before recording, the required space is estimated from the sample rate /
-- channels / bit depth (and an estimated per-frame JPEG size when video is
-- enabled). If the target volume does not have enough free space the script
-- stops up front and tells the user instead of half-writing files.
-- --------------------------------------------------------------

-- 1. Requires
local arg_schema = require("arg_schema")
local audio = require("audio")
local avi_mux = require("avi_mux")
local board_manager = require("board_manager")
local json = require("json")
local storage = require("storage")
local thread = require("thread")

-- 2. Constants
local SD_MOUNT_POINT = "/sdcard"
local AUDIO_DEVICE_NAME = "audio_adc"
local VIDEO_JOB_NAME = "record_av_video"
-- {CUR_SKILL_DIR} is only expanded inside SKILL.md, never inside this file, so
-- the absolute frames-script path is passed in as an arg. The default is the
-- canonical on-flash skill location used as a fallback.
local DEFAULT_FRAMES_SCRIPT = "/fatfs/skills/record_av/scripts/record_frames.lua"

local DEFAULT_DURATION_MS = 5000
local DEFAULT_FPS = 5
local DEFAULT_TIMEOUT_MS = 3000
-- Capture at a modest resolution by default: encoding video frames while audio
-- recording is running pushes PSRAM hard (each frame needs a resized RGB565
-- buffer plus a same-size JPEG output buffer). 480x360 keeps that peak well
-- below 640x480 so frames don't get dropped to out-of-memory.
local DEFAULT_MAX_WIDTH = 480
local DEFAULT_MAX_HEIGHT = 360
local DEFAULT_EST_FRAME_BYTES = 30 * 1024
local DEFAULT_DIR = "recordings"
local DEFAULT_BASENAME = "rec"

local WAV_HEADER_BYTES = 44
local SAFETY_MARGIN_NUM = 12 -- require ~120% of the estimate as free space
local SAFETY_MARGIN_DEN = 10
local MIN_FREE_BYTES = 256 * 1024
local VIDEO_JOIN_WAIT_MS = 8000

-- 3. Args
local function raw_arg(name, default)
  if type(args) == "table" and args[name] ~= nil then
    return args[name]
  end
  return default
end

local ARG_SCHEMA = {
  duration_ms     = arg_schema.int({ default = DEFAULT_DURATION_MS, min = 500, max = 600000 }),
  fps             = arg_schema.int({ default = DEFAULT_FPS, min = 1, max = 15 }),
  timeout_ms      = arg_schema.int({ default = DEFAULT_TIMEOUT_MS, min = 0 }),
  max_width       = arg_schema.int({ default = DEFAULT_MAX_WIDTH, min = 0 }),
  max_height      = arg_schema.int({ default = DEFAULT_MAX_HEIGHT, min = 0 }),
  est_frame_bytes = arg_schema.int({ default = DEFAULT_EST_FRAME_BYTES, min = 1024 }),
  with_video      = arg_schema.bool({ default = true }),
}

local ctx = arg_schema.parse(args, ARG_SCHEMA)
ctx.dir = raw_arg("dir", DEFAULT_DIR)
ctx.basename = raw_arg("basename", DEFAULT_BASENAME)
ctx.gain_db = raw_arg("gain_db", nil)
ctx.frames_script = raw_arg("frames_script", DEFAULT_FRAMES_SCRIPT)

-- 4. Validation helpers
local function reject_path_part(name, value)
  if type(value) ~= "string" or value == "" then
    error(name .. " must be a non-empty string")
  end
  if string.find(value, "%.%.") then
    error(name .. " must not contain '..'")
  end
  if string.find(value, "/", 1, true) or string.find(value, "\\", 1, true) then
    error(name .. " must be a single name without path separators")
  end
end

local function human_bytes(n)
  if n >= 1024 * 1024 then
    return string.format("%.2f MiB", n / (1024 * 1024))
  end
  if n >= 1024 then
    return string.format("%.2f KiB", n / 1024)
  end
  return string.format("%d B", n)
end

local function free_space_of(path)
  local ok, info = pcall(storage.get_free_space, path)
  if ok and type(info) == "table" and info.free then
    return info.free, info.total
  end
  return nil
end

local function ensure_dir(path)
  if not storage.exists(path) then
    storage.mkdir(path)
  end
  return path
end

-- 5. State / cleanup
local input_handle = nil
local video_started = false

local function cleanup()
  if video_started then
    -- Join (or stop) the video job so the camera is released before we return.
    pcall(thread.stop, VIDEO_JOB_NAME, VIDEO_JOIN_WAIT_MS)
    video_started = false
  end
  if input_handle then
    pcall(audio.close, input_handle)
    input_handle = nil
  end
end

-- 6. Run
local function run()
  reject_path_part("dir", ctx.dir)
  reject_path_part("basename", ctx.basename)

  -- 6.1 Pick the storage target: SD card when mounted, else the flash root.
  local warnings = {}
  local sd_free = free_space_of(SD_MOUNT_POINT)
  local target_root, space_path, storage_kind
  if sd_free ~= nil then
    target_root = SD_MOUNT_POINT
    space_path = SD_MOUNT_POINT
    storage_kind = "sdcard"
  else
    target_root = storage.get_root_dir()
    space_path = target_root
    storage_kind = "flash"
    table.insert(warnings, "no SD card mounted; recording to on-flash storage (" .. target_root .. ")")
  end

  -- 6.2 Audio input parameters from the board codec.
  local ok_codec, codec_dev, rate, channels, bits, init_gain =
    pcall(board_manager.get_audio_codec_input_params, AUDIO_DEVICE_NAME)
  if not ok_codec or not codec_dev then
    error("get_audio_codec_input_params('" .. AUDIO_DEVICE_NAME .. "') failed: " .. tostring(codec_dev))
  end
  local gain = ctx.gain_db
  if gain == nil then
    gain = init_gain
  end

  -- 6.3 Camera availability gates the video part.
  local with_video = ctx.with_video
  local dev_path = nil
  if with_video then
    local ok_cam, cam_paths = pcall(board_manager.get_camera_paths)
    if ok_cam and type(cam_paths) == "table" and cam_paths.dev_path then
      dev_path = cam_paths.dev_path
    else
      with_video = false
      table.insert(warnings, "no camera available on this board; recording audio only")
    end
  end

  -- 6.4 Estimate required space and warn the user up front if it will not fit.
  local duration_s = ctx.duration_ms / 1000
  local bytes_per_sec = rate * channels * (bits // 8)
  local audio_bytes = math.floor(bytes_per_sec * duration_s) + WAV_HEADER_BYTES

  local video_frames_est = 0
  local video_bytes_est = 0
  if with_video then
    video_frames_est = math.max(1, math.floor(duration_s * ctx.fps + 0.5))
    video_bytes_est = video_frames_est * ctx.est_frame_bytes
  end

  local required_bytes = audio_bytes + video_bytes_est
  local required_with_margin = required_bytes * SAFETY_MARGIN_NUM // SAFETY_MARGIN_DEN

  local free_now = free_space_of(space_path)
  if free_now == nil then
    error("cannot query free space of " .. space_path)
  end

  if free_now < required_with_margin then
    error(string.format(
      "Not enough free space on %s storage (%s). Estimated need ~%s " ..
      "(audio %s%s, +20%% margin), only %s free. " ..
      "Insert/clear an SD card, shorten the duration, lower the sample rate, or disable video.",
      storage_kind, space_path,
      human_bytes(required_with_margin),
      human_bytes(audio_bytes),
      with_video and (", video ~" .. human_bytes(video_bytes_est)) or "",
      human_bytes(free_now)))
  end

  -- 6.5 Prepare output directory: <root>/<dir>/<basename>/
  ensure_dir(storage.join_path(target_root, ctx.dir))
  local out_dir = ensure_dir(storage.join_path(target_root, ctx.dir, ctx.basename))
  local wav_path = storage.join_path(out_dir, "audio.wav")
  local video_dir = storage.join_path(out_dir, "video")

  print(string.format(
    "[record_av] target=%s dir=%s duration_ms=%d rate=%d ch=%d bits=%d gain=%s video=%s",
    storage_kind, out_dir, ctx.duration_ms, rate, channels, bits, tostring(gain),
    with_video and "on" or "off"))

  -- 6.6 Start the video job (async) so frames overlap the audio window.
  if with_video then
    local job_args = {
      dir            = video_dir,
      dev_path       = dev_path,
      duration_ms    = ctx.duration_ms,
      fps            = ctx.fps,
      timeout_ms     = ctx.timeout_ms,
      max_width      = ctx.max_width,
      max_height     = ctx.max_height,
      space_path     = space_path,
      min_free_bytes = MIN_FREE_BYTES,
    }
    local ok_start, start_err = thread.start(ctx.frames_script, job_args, {
      name = VIDEO_JOB_NAME,
      replace = true,
      timeout_ms = 0,
    })
    if not ok_start then
      table.insert(warnings, "failed to start video job: " .. tostring(start_err) .. "; recording audio only")
      with_video = false
    else
      video_started = true
    end
  end

  -- 6.7 Record audio (blocks for the full duration).
  input_handle = audio.new_input(codec_dev, rate, channels, bits, gain)
  local rec = audio.record_wav(input_handle, wav_path, ctx.duration_ms)
  audio.close(input_handle)
  input_handle = nil

  -- 6.8 Join the video job and read its manifest.
  local video_summary = nil
  if video_started then
    pcall(thread.stop, VIDEO_JOB_NAME, VIDEO_JOIN_WAIT_MS)
    video_started = false
    local manifest_path = storage.join_path(video_dir, "manifest.json")
    if storage.exists(manifest_path) then
      local ok_read, text = pcall(storage.read_file, manifest_path)
      if ok_read then
        local ok_dec, decoded = pcall(json.decode, text)
        if ok_dec then
          video_summary = decoded
        end
      end
    end
    if not video_summary then
      table.insert(warnings, "video job finished but no manifest was found")
    end
  end

  -- 6.85 Mux the JPEG frames + WAV audio into a single AVI (Motion-JPEG video +
  -- PCM audio) so the recording can be delivered as one file over IM. The board
  -- has no hardware video encoder, so this is container muxing only.
  local avi_path = nil
  if video_summary and (video_summary.frames or 0) > 0 then
    local out_avi = storage.join_path(out_dir, ctx.basename .. ".avi")
    local ok_mux, mux_res = pcall(avi_mux.mux, {
      frames_dir  = video_dir,
      frame_count = video_summary.frames,
      width       = video_summary.width,
      height      = video_summary.height,
      fps         = ctx.fps,
      wav_path    = wav_path,
      out_path    = out_avi,
    })
    if ok_mux and type(mux_res) == "table" then
      avi_path = mux_res.path
    else
      table.insert(warnings, "failed to build combined AVI: " .. tostring(mux_res))
    end
  end

  -- 6.9 Report.
  local free_after = free_space_of(space_path)
  print("[record_av] ===== recording complete =====")
  print(string.format("[record_av] storage: %s (%s)", storage_kind, space_path))
  print(string.format("[record_av] audio: path=%s bytes=%d duration_ms=%d",
    rec.path, rec.bytes, rec.duration_ms))
  if video_summary then
    print(string.format("[record_av] video: dir=%s frames=%d bytes=%d %dx%d reason=%s",
      video_dir, video_summary.frames or 0, video_summary.bytes or 0,
      video_summary.width or 0, video_summary.height or 0,
      tostring(video_summary.stopped_reason)))
  elseif with_video then
    print("[record_av] video: requested but no result")
  else
    print("[record_av] video: skipped (audio only)")
  end
  if avi_path then
    print(string.format("[record_av] avi: path=%s (combined audio+video; send THIS single file via IM)",
      avi_path))
  end
  if free_after then
    print(string.format("[record_av] free space remaining on %s: %s", space_path, human_bytes(free_after)))
  end
  for _, w in ipairs(warnings) do
    print("[record_av] WARN: " .. w)
  end
end

-- 7. Epilogue
local ok, err = xpcall(run, debug.traceback)
cleanup()
if not ok then
  print("[record_av] ERROR: " .. tostring(err))
  error(err)
end
