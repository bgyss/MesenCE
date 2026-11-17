-- Justifier (blue gun) on port 2, aimed at pixel (100, 50); nothing pressed.
emu.addEventCallback(function()
  emu.setInput({ x = 100, y = 50 }, 1, 0)
end, emu.eventType.inputPolled)
