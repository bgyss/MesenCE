// TestCpus.h - the real SnesCpu and Sa1Cpu sources compiled against flat test
// stand-ins (TestBus, TestDma, TestConsole, TestSa1), as TestSnesCpu and TestSa1Cpu.
// Shared by tools/snes-cpu-singlestep (SingleStepTests) and tools/snes-irq-scenarios.
// Include it once per program, after pch.h and the standard headers.
#pragma once

#include "Shared/Emulator.h"
#include "Shared/CheatManager.h"
#include "Shared/MemoryOperationType.h"
#include "Shared/EventType.h"
#include "Utilities/Serializer.h"
#include "Utilities/FolderUtilities.h"
#include "SNES/SnesCpuTypes.h"
#include "SNES/SnesConsole.h"
#include "SNES/SnesMemoryManager.h"
#include "SNES/SnesDmaController.h"
#include "SNES/SnesCpu.h"
#include "SNES/MemoryMappings.h"
#include "SNES/Coprocessors/SA1/Sa1.h"

struct BusOp
{
	bool Write;
	uint32_t Addr;
	uint8_t Value;
};

// Stands in for SnesMemoryManager: the members SnesCpu.cpp calls.
class TestBus
{
public:
	uint8_t* Ram = new uint8_t[0x1000000]();
	vector<uint32_t> Touched;
	vector<BusOp> Ops;

	void Poke(uint32_t addr, uint8_t value)
	{
		Ram[addr & 0xFFFFFF] = value;
		Touched.push_back(addr & 0xFFFFFF);
	}
	void Clear()
	{
		for(uint32_t a : Touched) {
			Ram[a] = 0;
		}
		Touched.clear();
		Ops.clear();
	}

	uint8_t Read(uint32_t addr, MemoryOperationType)
	{
		addr &= 0xFFFFFF;
		Ops.push_back({ false, addr, Ram[addr] });
		return Ram[addr];
	}
	void Write(uint32_t addr, uint8_t value, MemoryOperationType)
	{
		addr &= 0xFFFFFF;
		Ops.push_back({ true, addr, value });
		Poke(addr, value);
	}
	uint16_t PeekWord(uint32_t addr) { return Ram[addr & 0xFFFFFF] | (Ram[(addr + 1) & 0xFFFFFF] << 8); }
	uint8_t GetCpuSpeed(uint32_t) { return 8; }
	void SetCpuSpeed(uint8_t) {}
	void IncMasterClock4() {}
	void IncMasterClock6() {}
};

// Stands in for SnesDmaController: never has a pending transfer.
class TestDma
{
public:
	bool HasPendingTransfer() { return false; }
	bool ProcessPendingTransfers() { return false; }
};

// Stands in for SnesConsole in SnesCpu's constructor.
class Emulator;
class TestConsole
{
public:
	Emulator* Emu;
	TestBus* Bus;
	TestDma* Dma;
	Emulator* GetEmulator() { return Emu; }
	TestBus* GetMemoryManager() { return Bus; }
	TestDma* GetDmaController() { return Dma; }
};

// Compile the real SnesCpu against TestBus/TestDma. All headers SnesCpu.cpp
// includes were included above (they are guarded), so the renames only affect
// SnesCpu itself.
#undef __CPU__H
#define SnesCpu TestSnesCpu
#define SnesMemoryManager TestBus
#define SnesDmaController TestDma
#define SnesConsole TestConsole
#define private public
#include "SNES/SnesCpu.h"
#undef private
#include "SNES/SnesCpu.cpp"
#undef SnesConsole
#undef SnesDmaController
#undef SnesMemoryManager
#undef SnesCpu

// Stands in for the SA-1's memory mappings (only used to look for PRG ROM).
class TestMappings
{
public:
	IMemoryHandler* GetHandler(uint32_t) { return nullptr; }
};

// Stands in for Sa1: the members Sa1Cpu.cpp calls.
class TestSa1
{
public:
	TestBus* Bus = nullptr;
	TestMappings Mappings;

	uint8_t ReadSa1(uint32_t addr, MemoryOperationType type = MemoryOperationType::Read) { return Bus->Read(addr, type); }
	void WriteSa1(uint32_t addr, uint8_t value, MemoryOperationType type) { Bus->Write(addr, value, type); }
	MemoryType GetSa1MemoryType() { return MemoryType::Sa1InternalRam; }
	MemoryType GetSnesCpuMemoryType() { return MemoryType::SnesWorkRam; }
	bool IsSnesCpuFastRomSpeed() { return false; }
	TestMappings* GetMemoryMappings() { return &Mappings; }

	// SA-1 register vectors (CNV $2205, CIV $2207, CRV $2203)
	uint16_t NmiVector = 0;
	uint16_t IrqVector = 0;
	uint16_t ResetVector = 0;

	// Same as Sa1::ReadVector: NMI/IRQ/reset vectors come from SA-1 registers,
	// BRK/COP vectors are read with ReadSa1.
	uint16_t ReadVector(uint16_t vector)
	{
		switch(vector) {
			case 0xFFEA: return NmiVector;
			case 0xFFFC: return ResetVector;
			case 0xFFEE: return IrqVector;
		}
		uint8_t low = ReadSa1(vector);
		uint8_t high = ReadSa1(vector + 1);
		return (high << 8) | low;
	}
};

// Sa1Cpu.cpp names CpuType::Sa1, which the Sa1 -> TestSa1 rename below turns into
// CpuType::TestSa1; map it (and CpuType::Snes) back to the real values.
struct TestCpuType
{
	static constexpr CpuType TestSa1 = CpuType::Sa1;
	static constexpr CpuType Snes = CpuType::Snes;
};

// Compile the real Sa1Cpu against TestSa1. Sa1.h was included above (#pragma once),
// and Sa1Cpu.h is included here for the first time, so the renames only affect
// Sa1Cpu.
#define Sa1Cpu TestSa1Cpu
#define Sa1 TestSa1
#define CpuType TestCpuType
#define private public
#include "SNES/Coprocessors/SA1/Sa1Cpu.h"
#undef private
#include "SNES/Coprocessors/SA1/Sa1Cpu.cpp"
#undef CpuType
#undef Sa1
#undef Sa1Cpu
