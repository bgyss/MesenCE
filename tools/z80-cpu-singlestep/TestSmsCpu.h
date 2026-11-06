// TestSmsCpu.h - the real SmsCpu source (Core/SMS/SmsCpu.cpp) compiled against a flat
// test bus (TestBus) as TestSmsCpu. Shared by tools/z80-cpu-singlestep (SingleStepTests)
// and tools/z80-irq-scenarios. Include it once per program, after pch.h and the standard
// headers.
#pragma once

#include "Shared/Emulator.h"
#include "Shared/MemoryOperationType.h"
#include "Shared/EventType.h"
#include "SMS/SmsTypes.h"
#include "SMS/SmsConsole.h"
#include "SMS/SmsMemoryManager.h"
#include "SMS/SmsCpu.h"
#include "Utilities/Serializer.h"
#include "Utilities/FolderUtilities.h"

struct BusOp
{
	bool Io;
	bool Write;
	uint16_t Addr;
	uint8_t Value;
};

// Stands in for SmsMemoryManager: the members SmsCpu.cpp calls.
class TestBus
{
public:
	uint8_t Ram[0x10000] = {};
	vector<BusOp> Ops;
	vector<uint8_t> PortReads;
	size_t PortReadIndex = 0;

	uint8_t Read(uint16_t addr, MemoryOperationType)
	{
		Ops.push_back({ false, false, addr, Ram[addr] });
		return Ram[addr];
	}
	void Write(uint16_t addr, uint8_t value)
	{
		Ops.push_back({ false, true, addr, value });
		Ram[addr] = value;
	}
	uint8_t ReadPort(uint8_t port)
	{
		uint8_t v = PortReadIndex < PortReads.size() ? PortReads[PortReadIndex++] : 0xFF;
		Ops.push_back({ true, false, port, v });
		return v;
	}
	void WritePort(uint8_t port, uint8_t value) { Ops.push_back({ true, true, port, value }); }
	void Exec(uint32_t) {}
	uint8_t GetOpenBus() { return 0xFF; }
	uint8_t DebugRead(uint16_t addr) { return Ram[addr]; }
	uint8_t DebugReadPort(uint8_t) { return 0xFF; }
};

// Compile the real SmsCpu against TestBus. All headers SmsCpu.cpp includes were
// included above (they are guarded), so the renames only affect SmsCpu itself.
#undef __SmsCpu__H
#define SmsCpu TestSmsCpu
#define SmsMemoryManager TestBus
#define private public
#include "SMS/SmsCpu.h"
#undef private
#include "SMS/SmsCpu.cpp"
#undef SmsMemoryManager
#undef SmsCpu
