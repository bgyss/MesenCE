-- A Lua error in a headless run must fail the run (stop code 65), not be ignored.
emu.addEventCallback(function()
  local t = nil
  return t.missing
end, emu.eventType.endFrame)
