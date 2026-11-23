-- UI test-runner smoke test for the Mega Drive (Mesen --testRunner md_ui_smoke.lua <rom.md>).
-- Exit codes: 0 ok, 2 wrong console, 3 CPU state unreadable, 4 no frames.
local frames = 0
emu.addEventCallback(function()
	frames = frames + 1
	if frames == 30 then
		local state = emu.getState()
		if state.consoleType ~= "Md" then
			emu.log("console type " .. tostring(state.consoleType))
			emu.stop(2)
		end
		if state["m68k.pc"] == nil then
			emu.stop(3)
		end
		emu.log(string.format("frame %d pc=%06X s=%d", frames, state["m68k.pc"], state["m68k.r.s"] and 1 or 0))
		emu.stop(0)
	end
end, emu.eventType.endFrame)
