---
{
  "name": "record_av",
  "description": "Record audio (WAV) and, when the board has a camera, a synchronized sequence of JPEG video frames over the same time window. Stores recordings on the SD card when one is mounted, otherwise on on-flash storage after checking there is enough free space. Requires board_hardware_info skill.",
  "metadata": {
    "cap_groups": [
      "cap_lua"
    ],
    "manage_mode": "readonly"
  }
}
---

# Record Audio and Video

Use this skill when the user asks to record audio, record a voice memo, capture
a video, or record audio and video together on the board.

This board has no hardware video encoder, so a "video" recording is captured as
a WAV file plus a folder of timestamped JPEG frames over the same wall-clock
window. The audio is recorded in the foreground while the frames are captured
concurrently by an async helper job, so both cover the same period. After
capture, the frames and audio are muxed (container only, no re-encoding) into a
single `<basename>.avi` (Motion-JPEG video + PCM audio) for easy delivery.

Run exactly one script with `lua_run_script` after reading `board_hardware_info`.

If `lua_run_script` returns an error, report that error directly to the user.
The most important error to relay verbatim is the "Not enough free space"
message: it means the recording was not started because the target volume does
not have room for the estimated size. Do not retry with the same duration.

## Storage behavior

- If an SD card is mounted (`/sdcard`), recordings are written there.
- Otherwise recordings go to the on-flash storage root (`/fatfs`).
- Before recording, required space is estimated from the sample rate, channel
  count, and bit depth (plus an estimated per-frame JPEG size when video is
  enabled), with a 20% safety margin. If the target volume does not have enough
  free space the script stops up front and reports it instead of half-writing.

Output layout under the chosen root:

```
<root>/<dir>/<basename>/audio.wav
<root>/<dir>/<basename>/video/frame_00001.jpg ...
<root>/<dir>/<basename>/video/manifest.json
<root>/<dir>/<basename>/<basename>.avi   # combined audio+video (Motion-JPEG)
```

When the user asks to receive the recording, send the single combined
`<basename>.avi` file over IM (for example with `feishu_send_file`). The script
prints its absolute path on a line starting with `[record_av] avi:`. The `.avi`
is a normal Motion-JPEG container that desktop players and ffmpeg can open; some
mobile IM clients show it as a downloadable file rather than playing it inline.
The `audio.wav` and the `video/` frames remain available if only one stream is
wanted. If no `[record_av] avi:` line is printed (audio-only recording, or the
mux failed and a warning was logged), fall back to sending `audio.wav`.

## Script Args Schema

```json
{
  "type": "object",
  "properties": {
    "duration_ms": {
      "type": "integer",
      "default": 5000,
      "minimum": 500,
      "maximum": 600000,
      "description": "Recording length in milliseconds."
    },
    "with_video": {
      "type": "boolean",
      "default": true,
      "description": "Capture video frames in addition to audio. Automatically disabled if the board has no camera."
    },
    "fps": {
      "type": "integer",
      "default": 5,
      "minimum": 1,
      "maximum": 15,
      "description": "Target video frame rate."
    },
    "dir": {
      "type": "string",
      "default": "recordings",
      "description": "Single directory name under the storage root. No path separators or '..'."
    },
    "basename": {
      "type": "string",
      "default": "rec",
      "description": "Single directory name for this recording. Pass a unique value to avoid overwriting a previous recording."
    },
    "gain_db": {
      "type": "integer",
      "description": "Optional microphone gain override in dB. Defaults to the board codec's configured gain."
    },
    "max_width": {
      "type": "integer",
      "default": 480,
      "description": "Resize cap (width) applied before JPEG encoding each frame to limit PSRAM use. Lower this (e.g. 320) if frames are dropped to out-of-memory while audio is also recording."
    },
    "max_height": {
      "type": "integer",
      "default": 360,
      "description": "Resize cap (height) applied before JPEG encoding each frame."
    },
    "est_frame_bytes": {
      "type": "integer",
      "default": 30720,
      "description": "Estimated bytes per JPEG frame used for the up-front space check."
    },
    "frames_script": {
      "type": "string",
      "description": "Absolute path of the bundled video-frame helper script. Always pass {CUR_SKILL_DIR}/scripts/record_frames.lua."
    }
  }
}
```

Path rules:
- `dir` and `basename` must be single names with no `/`, `\\`, or `..`.

## Tool Call Inputs

Record 5 seconds of audio and video with defaults:

```json
{"path":"{CUR_SKILL_DIR}/scripts/record_av.lua","args":{"frames_script":"{CUR_SKILL_DIR}/scripts/record_frames.lua"}}
```

Record 10 seconds of audio and video into a uniquely named folder:

```json
{"path":"{CUR_SKILL_DIR}/scripts/record_av.lua","args":{"duration_ms":10000,"basename":"meeting_01","frames_script":"{CUR_SKILL_DIR}/scripts/record_frames.lua"}}
```

Record audio only (no video):

```json
{"path":"{CUR_SKILL_DIR}/scripts/record_av.lua","args":{"duration_ms":8000,"with_video":false,"frames_script":"{CUR_SKILL_DIR}/scripts/record_frames.lua"}}
```

Record at a higher frame rate with a custom mic gain:

```json
{"path":"{CUR_SKILL_DIR}/scripts/record_av.lua","args":{"duration_ms":6000,"fps":10,"gain_db":42,"frames_script":"{CUR_SKILL_DIR}/scripts/record_frames.lua"}}
```

## Recommended Flow

1. Activate the `board_hardware_info` skill and confirm an `audio_adc` device is
   listed. If the user wants video, also confirm a `camera` device is listed.
2. Choose `duration_ms` from the user's request (default 5000). Pick a unique
   `basename` when the user records more than once.
3. Always pass `frames_script` as `{CUR_SKILL_DIR}/scripts/record_frames.lua`.
4. Run `{CUR_SKILL_DIR}/scripts/record_av.lua` with the selected `args`.
   The call blocks for roughly `duration_ms`, so for long recordings prefer a
   generous tool timeout.
5. Report the storage target (SD card or flash), the saved audio path and size,
   the video frame count and folder (when video ran), the remaining free space,
   and any warnings or errors directly from the script output.
6. If the script reports "Not enough free space", relay it and suggest inserting
   or clearing an SD card, shortening the duration, lowering the sample rate, or
   disabling video.
