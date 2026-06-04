-- --------------------------------------------------------------
-- Check the status of the custom logo on the device.
-- --------------------------------------------------------------

local storage = require("storage")

local LOGO_PATH = "/fatfs/logo/custom.svg"

local stat_ok, stat_result = pcall(storage.stat, LOGO_PATH)

if stat_ok and stat_result and stat_result.type == "file" then
  local msg = string.format(
    "Custom logo exists at %s (%d bytes). " ..
    "The logo is displayed on the device LCD when idle.",
    LOGO_PATH, stat_result.size or 0
  )
  if type(print) == "function" then print(msg) end
  return msg
else
  local msg = "No custom logo is set. Use the custom_logo skill to generate one, " ..
              "or upload an SVG via the web config UI Logo page."
  if type(print) == "function" then print(msg) end
  return msg
end
