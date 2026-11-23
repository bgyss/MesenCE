-- Team Player on port 1 (pads A=6-button, B=3-button, C=none, D=6-button; --md-pads t:63n6,6).
-- Every frame: A holds Up and A, B holds Start, D holds Mode and X.
emu.addEventCallback(function()
  emu.setInput({ up = true, a = true }, 0, 0)
  emu.setInput({ start = true }, 0, 1)
  emu.setInput({ mode = true, x = true }, 0, 3)
end, emu.eventType.inputPolled)
