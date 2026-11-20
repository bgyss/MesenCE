-- 32X debugger integration (S4), run on the m32x_boot cartridge (tests/MD/synthetic/m32x_cases.py):
--   - an exec callback on the master SH-2 (cpuType m32xMaster) fires at its application entry
--     ($06000400, the first instruction after the vector tables); the slave runs code too;
--   - a write callback on the master sees its write of $A5A5 to comm6 ($0000402C physical);
--   - SDRAM reads the boot ROM's copy of the program, through m32xSdram and through the master's
--     address space (m32xMasterMemory); the 32X memories have their sizes;
--   - a write through the SH-2 address space lands in SDRAM.
-- Stops with code 0 when everything holds, 1 otherwise (printing what failed).
local masterEntry = 0
local slaveExec = 0
local commWrite = 0
local failures = {}

local function check(cond, what)
  if not cond then
    table.insert(failures, what)
  end
end

emu.addMemoryCallback(function()
  masterEntry = masterEntry + 1
end, emu.callbackType.exec, 0x06000400, 0x06000400, emu.cpuType.m32xMaster, emu.memType.m32xMasterMemory)

emu.addMemoryCallback(function()
  slaveExec = slaveExec + 1
end, emu.callbackType.exec, 0x06000400, 0x06001000, emu.cpuType.m32xSlave, emu.memType.m32xSlaveMemory)

emu.addMemoryCallback(function(address, value)
  if value == 0xA5A5 then
    commWrite = commWrite + 1
  end
end, emu.callbackType.write, 0x402C, 0x402D, emu.cpuType.m32xMaster, emu.memType.m32xMasterMemory)

local frames = 0
emu.addEventCallback(function()
  frames = frames + 1
  if frames < 4 then
    return
  end
  -- The master's first instruction is mov.l @(disp,pc),r0 (opcode $D0xx)
  check(emu.read(0x400, emu.memType.m32xSdram) == 0xD0, "m32xSdram[$400] = $D0")
  check(emu.read(0x06000400, emu.memType.m32xMasterMemory) == 0xD0, "m32xMasterMemory[$06000400] = $D0")
  check(emu.getMemorySize(emu.memType.m32xSdram) == 0x40000, "SDRAM size")
  check(emu.getMemorySize(emu.memType.m32xFrameBuffer) == 0x40000, "frame buffer size")
  check(emu.getMemorySize(emu.memType.m32xPalette) == 0x200, "palette size")
  check(masterEntry == 1, "master exec callback at $06000400 fired " .. masterEntry .. " times")
  check(slaveExec > 0, "slave exec callbacks: " .. slaveExec)
  check(commWrite == 1, "master write of $A5A5 to comm6 seen " .. commWrite .. " times")
  emu.write(0x0603E000, 0x5A, emu.memType.m32xMasterMemory)
  check(emu.read(0x3E000, emu.memType.m32xSdram) == 0x5A, "write through m32xMasterMemory")
  for _, f in ipairs(failures) do
    emu.log("FAIL " .. f)
  end
  emu.stop(#failures == 0 and 0 or 1)
end, emu.eventType.endFrame)
