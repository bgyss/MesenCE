-- Sega CD debugger integration (S4), run on the mcd_boot_comm cartridge with --md-mcd:
--   - an exec callback on the sub CPU (cpuType mcdM68k) fires at its entry point ($000200);
--   - the sub CPU's memory reads the program the main CPU copied into program RAM, through the
--     sub CPU's address space (mcdMemory) and program RAM (mcdPrgRam);
--   - backup RAM, PCM RAM, word RAM and the CDC buffer are exposed with their sizes.
-- Stops with code 0 when everything holds, 1 otherwise (printing what failed).
local hits = 0
local failures = {}

local function check(cond, what)
  if not cond then
    table.insert(failures, what)
  end
end

emu.addMemoryCallback(function()
  hits = hits + 1
end, emu.callbackType.exec, 0x200, 0x200, emu.cpuType.mcdM68k, emu.memType.mcdMemory)

local frames = 0
emu.addEventCallback(function()
  frames = frames + 1
  if frames < 3 then
    return
  end
  -- move.w #$1234,$FF8020 = 33 FC 12 34 00 FF 80 20 (tests/MD/synthetic/mcd_cases.py, _boot_and_comm)
  check(emu.read(0x200, emu.memType.mcdPrgRam) == 0x33, "mcdPrgRam[$200]")
  check(emu.read(0x201, emu.memType.mcdPrgRam) == 0xFC, "mcdPrgRam[$201]")
  check(emu.read(0x202, emu.memType.mcdMemory) == 0x12, "mcdMemory[$202]")
  check(emu.read16(0x202, emu.memType.mcdMemory) == 0x3412, "mcdMemory word at $202 (little-endian read16)")
  check(emu.getMemorySize(emu.memType.mcdPrgRam) == 0x80000, "program RAM size")
  check(emu.getMemorySize(emu.memType.mcdWordRam) == 0x40000, "word RAM size")
  check(emu.getMemorySize(emu.memType.mcdBackupRam) == 0x2000, "backup RAM size")
  check(emu.getMemorySize(emu.memType.mcdPcmRam) == 0x10000, "PCM RAM size")
  check(emu.getMemorySize(emu.memType.mcdCdcRam) == 0x4000, "CDC buffer size")
  check(hits == 1, "sub CPU exec callback at $200 fired " .. hits .. " times")
  -- A write through the sub CPU's address space lands in program RAM
  emu.write(0x7F00, 0x5A, emu.memType.mcdMemory)
  check(emu.read(0x7F00, emu.memType.mcdPrgRam) == 0x5A, "write through mcdMemory")
  for _, f in ipairs(failures) do
    emu.log("FAIL " .. f)
  end
  emu.stop(#failures == 0 and 0 or 1)
end, emu.eventType.endFrame)
