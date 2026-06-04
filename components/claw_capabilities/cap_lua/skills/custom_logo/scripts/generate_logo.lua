-- --------------------------------------------------------------
-- Generate and save a custom SVG logo to device storage.
-- The logo is written to /fatfs/logo/custom.svg and will be
-- displayed on the device LCD, replacing the emote idle animation.
-- --------------------------------------------------------------

local storage = require("storage")

-- 1. Constants
local LOGO_DIR  = "/fatfs/logo"
local LOGO_PATH = "/fatfs/logo/custom.svg"

-- 2. Args
local function get_arg(name, default)
  if type(args) == "table" and args[name] ~= nil then
    return args[name]
  end
  return default
end

local svg_content = get_arg("svg_content", nil)

-- 3. Validation
if type(svg_content) ~= "string" or svg_content == "" then
  error("svg_content argument is required and must be a non-empty SVG string")
end

-- Basic sanity check: must start with <svg (possibly after whitespace)
local trimmed = svg_content:match("^%s*(.+)")
if not trimmed or not trimmed:match("^<%?xml") and not trimmed:match("^<svg") then
  error("svg_content must be valid SVG markup (starting with <svg or <?xml)")
end

-- 4. Ensure directory exists
local ok, mkdir_err = pcall(storage.mkdir, LOGO_DIR)
if not ok then
  -- mkdir may fail if directory already exists; that is fine
  local stat_ok, stat_result = pcall(storage.stat, LOGO_DIR)
  if not stat_ok or stat_result.type ~= "dir" then
    error("Failed to create logo directory: " .. tostring(mkdir_err))
  end
end

-- 5. Write SVG file
storage.write_file(LOGO_PATH, svg_content)

-- 6. Verify
local stat_ok, stat_result = pcall(storage.stat, LOGO_PATH)
local file_size = 0
if stat_ok and stat_result then
  file_size = stat_result.size or 0
end

-- 7. Report
local msg = string.format(
  "Logo saved to %s (%d bytes). " ..
  "The logo will automatically update on the device LCD within a few seconds. " ..
  "You can also preview it in the web config UI Logo page.",
  LOGO_PATH, file_size
)

if type(print) == "function" then
  print(msg)
end

return msg
