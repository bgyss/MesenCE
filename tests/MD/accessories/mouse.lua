-- Sega Mouse on port 1: every frame, 5 units right, 3 units up (screen y -3), left button held.
emu.addEventCallback(function()
  emu.setInput({ x = 5, y = -3, left = true }, 0, 0)
end, emu.eventType.inputPolled)
