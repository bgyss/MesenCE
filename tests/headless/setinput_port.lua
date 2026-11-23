-- Regression test for emu.setInput's port argument (Core/Debugger/LuaApi.cpp).
-- Presses Start on port 2 only (port index 1) from the input-polled event; after a few
-- frames port 2 must read Start pressed and port 1 must not. Before the fix the port was
-- read from the subport slot, so the press landed on port 1.
local frames = 0
emu.addEventCallback(function()
  emu.setInput({ start = true }, 1)
end, emu.eventType.inputPolled)
emu.addEventCallback(function()
  frames = frames + 1
  if frames == 5 then
    local p1 = emu.getInput(0)
    local p2 = emu.getInput(1)
    emu.log("port1.start=" .. tostring(p1.start) .. " port2.start=" .. tostring(p2.start))
    if p2.start and not p1.start then emu.stop(0) else emu.stop(1) end
  end
end, emu.eventType.endFrame)
