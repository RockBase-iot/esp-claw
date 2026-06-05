-- --------------------------------------------------------------
-- avi_mux.lua
--
-- Mux a sequence of JPEG frames (Motion-JPEG) plus an optional PCM WAV audio
-- track into a single AVI 1.0 (RIFF) container. The whole file is written
-- incrementally with the `io` library so only one frame (or a small audio
-- chunk) is ever held in memory -- a multi-MB recording never needs a multi-MB
-- Lua string.
--
-- The board has no hardware video encoder, so this is pure container muxing:
-- the existing JPEG frames become the video stream (fccHandler "MJPG") and the
-- WAV PCM payload becomes the audio stream. The result is a normal .avi that
-- most desktop players (and ffmpeg-based pipelines) can open directly, and that
-- can be sent as a single file over IM.
-- --------------------------------------------------------------

local storage = require("storage")

local M = {}

local FRAME_CKID = "00dc" -- stream 0, compressed video
local AUDIO_CKID = "01wb" -- stream 1, audio (waveform bytes)
local AVIIF_KEYFRAME = 0x10
local AVIF_HASINDEX = 0x10
local COPY_CHUNK = 16384

local function u32(n)
  return string.pack("<I4", n)
end

local function u16(n)
  return string.pack("<I2", n)
end

-- Parse a canonical PCM WAV file. Returns a table with the audio format and the
-- byte range of the `data` chunk, or nil when the file is not usable.
local function read_wav_info(path)
  local f = io.open(path, "rb")
  if not f then
    return nil
  end
  local ok, info = pcall(function()
    local riff = f:read(12)
    if not riff or #riff < 12 or riff:sub(1, 4) ~= "RIFF" or riff:sub(9, 12) ~= "WAVE" then
      return nil
    end
    local fmt = nil
    while true do
      local hdr = f:read(8)
      if not hdr or #hdr < 8 then
        break
      end
      local ckid = hdr:sub(1, 4)
      local cksize = string.unpack("<I4", hdr, 5)
      if ckid == "fmt " then
        local body = f:read(cksize)
        if body and #body >= 16 then
          fmt = {
            channels = string.unpack("<I2", body, 3),
            rate = string.unpack("<I4", body, 5),
            avg_bytes = string.unpack("<I4", body, 9),
            block_align = string.unpack("<I2", body, 13),
            bits = string.unpack("<I2", body, 15),
          }
        end
        if cksize % 2 == 1 then
          f:read(1)
        end
      elseif ckid == "data" then
        fmt = fmt or {}
        fmt.data_off = f:seek()
        fmt.data_size = cksize
        return fmt
      else
        f:seek("cur", cksize + (cksize % 2))
      end
    end
    return nil
  end)
  f:close()
  if not ok or type(info) ~= "table" or not info.data_off or not info.block_align then
    return nil
  end
  if info.block_align <= 0 then
    return nil
  end
  -- Keep only whole sample blocks.
  info.data_size = info.data_size - (info.data_size % info.block_align)
  if info.data_size <= 0 then
    return nil
  end
  return info
end

-- opts = {
--   frames_dir  = directory holding frame_00001.jpg ...
--   frame_count = number of frames written (from manifest)
--   width, height, fps
--   wav_path    = optional path to a canonical PCM WAV
--   out_path    = output .avi path
-- }
function M.mux(opts)
  assert(type(opts) == "table", "avi_mux.mux: opts table required")
  local frames_dir = assert(opts.frames_dir, "avi_mux: frames_dir required")
  local out_path = assert(opts.out_path, "avi_mux: out_path required")
  local fps = opts.fps or 5
  if fps < 1 then
    fps = 1
  end
  local width = opts.width or 0
  local height = opts.height or 0

  -- 1. Collect frame sizes up front so all container sizes are known before we
  --    start writing (avoids any seek/back-patching).
  local frames = {}
  local max_frame = 0
  for i = 1, (opts.frame_count or 0) do
    local fp = string.format("%s/frame_%05d.jpg", frames_dir, i)
    local st = storage.stat(fp)
    if not st or not st.size or st.size <= 0 then
      break
    end
    frames[#frames + 1] = { path = fp, size = st.size }
    if st.size > max_frame then
      max_frame = st.size
    end
  end
  if #frames == 0 then
    error("avi_mux: no frames found in " .. frames_dir)
  end

  -- 2. Optional audio track.
  local audio = nil
  if opts.wav_path and storage.exists(opts.wav_path) then
    audio = read_wav_info(opts.wav_path)
  end

  -- 3. Build the hdrl header block in memory (small, a few hundred bytes).
  local streams = audio and 2 or 1
  local avih = table.concat({
    u32(math.floor(1000000 / fps)), -- dwMicroSecPerFrame
    u32(0),                         -- dwMaxBytesPerSec
    u32(0),                         -- dwPaddingGranularity
    u32(AVIF_HASINDEX),             -- dwFlags
    u32(#frames),                   -- dwTotalFrames
    u32(0),                         -- dwInitialFrames
    u32(streams),                   -- dwStreams
    u32(max_frame),                 -- dwSuggestedBufferSize
    u32(width),                     -- dwWidth
    u32(height),                    -- dwHeight
    u32(0), u32(0), u32(0), u32(0), -- dwReserved[4]
  })

  local vstrh = table.concat({
    "vids", "MJPG",
    u32(0),                         -- dwFlags
    u16(0), u16(0),                 -- wPriority, wLanguage
    u32(0),                         -- dwInitialFrames
    u32(1),                         -- dwScale
    u32(fps),                       -- dwRate
    u32(0),                         -- dwStart
    u32(#frames),                   -- dwLength
    u32(max_frame),                 -- dwSuggestedBufferSize
    u32(0),                         -- dwQuality
    u32(0),                         -- dwSampleSize
    u16(0), u16(0), u16(width), u16(height), -- rcFrame
  })
  local vstrf = table.concat({
    u32(40),                        -- biSize
    u32(width), u32(height),
    u16(1), u16(24),                -- biPlanes, biBitCount
    "MJPG",                         -- biCompression
    u32(width * height * 3),        -- biSizeImage
    u32(0), u32(0), u32(0), u32(0), -- pels/clr
  })
  local strl_v = "strl"
    .. "strh" .. u32(#vstrh) .. vstrh
    .. "strf" .. u32(#vstrf) .. vstrf
  strl_v = "LIST" .. u32(#strl_v) .. strl_v

  local hdrl_payload = "hdrl" .. "avih" .. u32(#avih) .. avih .. strl_v
  if audio then
    local astrh = table.concat({
      "auds",
      u32(0),                       -- fccHandler
      u32(0),                       -- dwFlags
      u16(0), u16(0),               -- wPriority, wLanguage
      u32(0),                       -- dwInitialFrames
      u32(audio.block_align),       -- dwScale
      u32(audio.avg_bytes),         -- dwRate
      u32(0),                       -- dwStart
      u32(math.floor(audio.data_size / audio.block_align)), -- dwLength (blocks)
      u32(audio.data_size),         -- dwSuggestedBufferSize
      u32(0),                       -- dwQuality
      u32(audio.block_align),       -- dwSampleSize
      u16(0), u16(0), u16(0), u16(0), -- rcFrame
    })
    local astrf = table.concat({
      u16(1),                       -- wFormatTag = PCM
      u16(audio.channels),
      u32(audio.rate),
      u32(audio.avg_bytes),
      u16(audio.block_align),
      u16(audio.bits),
      u16(0),                       -- cbSize
    })
    local strl_a = "strl"
      .. "strh" .. u32(#astrh) .. astrh
      .. "strf" .. u32(#astrf) .. astrf
    hdrl_payload = hdrl_payload .. "LIST" .. u32(#strl_a) .. strl_a
  end
  local hdrl = "LIST" .. u32(#hdrl_payload) .. hdrl_payload

  -- 4. Pre-compute movi layout and the idx1 entries. Offsets are relative to
  --    the 'movi' fourcc, so the first chunk's ckid sits at offset 4.
  local index = {}
  local cur = 4
  for _, fr in ipairs(frames) do
    index[#index + 1] = { ckid = FRAME_CKID, flags = AVIIF_KEYFRAME, offset = cur, size = fr.size }
    cur = cur + 8 + fr.size + (fr.size % 2)
  end
  if audio then
    index[#index + 1] = { ckid = AUDIO_CKID, flags = AVIIF_KEYFRAME, offset = cur, size = audio.data_size }
    cur = cur + 8 + audio.data_size + (audio.data_size % 2)
  end
  local movi_list_size = cur            -- 'movi' fourcc + every chunk
  local idx1_size = 16 * #index
  local riff_size = 4                    -- 'AVI '
    + #hdrl
    + (8 + movi_list_size)               -- movi LIST chunk
    + (8 + idx1_size)                    -- idx1 chunk

  -- 5. Write the file.
  local out = io.open(out_path, "wb")
  if not out then
    error("avi_mux: cannot open output " .. out_path)
  end
  local ok, err = pcall(function()
    out:write("RIFF", u32(riff_size), "AVI ")
    out:write(hdrl)
    out:write("LIST", u32(movi_list_size), "movi")

    for _, fr in ipairs(frames) do
      local ff = io.open(fr.path, "rb")
      if not ff then
        error("avi_mux: cannot read frame " .. fr.path)
      end
      local data = ff:read("a")
      ff:close()
      if not data or #data ~= fr.size then
        error("avi_mux: frame size mismatch for " .. fr.path)
      end
      out:write(FRAME_CKID, u32(#data), data)
      if #data % 2 == 1 then
        out:write("\0")
      end
    end

    if audio then
      out:write(AUDIO_CKID, u32(audio.data_size))
      local af = io.open(opts.wav_path, "rb")
      if not af then
        error("avi_mux: cannot reopen wav " .. opts.wav_path)
      end
      af:seek("set", audio.data_off)
      local remaining = audio.data_size
      while remaining > 0 do
        local want = remaining < COPY_CHUNK and remaining or COPY_CHUNK
        local chunk = af:read(want)
        if not chunk or #chunk == 0 then
          break
        end
        out:write(chunk)
        remaining = remaining - #chunk
      end
      af:close()
      if remaining > 0 then
        error("avi_mux: short audio read")
      end
      if audio.data_size % 2 == 1 then
        out:write("\0")
      end
    end

    out:write("idx1", u32(idx1_size))
    for _, e in ipairs(index) do
      out:write(e.ckid, u32(e.flags), u32(e.offset), u32(e.size))
    end
  end)
  out:close()
  if not ok then
    pcall(storage.remove, out_path)
    error(err)
  end

  return {
    path = out_path,
    bytes = riff_size + 8,
    frames = #frames,
    has_audio = audio ~= nil,
    width = width,
    height = height,
  }
end

return M
