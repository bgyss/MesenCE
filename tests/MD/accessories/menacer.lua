-- Menacer on port 2, aimed at pixel (100, 50); trigger (B) and Start held.
emu.addEventCallback(function()
  emu.setInput({ x = 100, y = 50, trigger = true, start = true }, 1, 0)
end, emu.eventType.inputPolled)
