local ok, err = pcall(dofile, "/fatfs/scripts/builtin/advance_camera_preview.lua")
if not ok then
    print("[camera_preview] ERROR: " .. tostring(err))
end