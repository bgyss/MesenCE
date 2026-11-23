#if (defined(DUMMYCPU) && !defined(__DUMMYSmsCpu__H)) || (!defined(DUMMYCPU) && !defined(__SmsCpu__H))
	#ifdef DUMMYCPU
		#define __DUMMYSmsCpu__H
	#else
		#define __SmsCpu__H
	#endif

	#include "pch.h"
	#include "SMS/SmsTypes.h"
	#include "Shared/MemoryOperationType.h"
	#include "Shared/MemoryType.h"
	#include "Utilities/ISerializable.h"
	#include "Shared/Processors/Z80/Z80Engine.h"

class Emulator;
class SmsConsole;
class SmsMemoryManager;

// SMS/Game Gear/SG-1000/ColecoVision Z80: the shared Z80Engine plus the SMS host interface
// (memory manager, I/O ports, debugger hooks, cheats, model quirks).
class SmsCpu final : public Z80Engine<SmsCpu>, public ISerializable
{
	friend class Z80Engine<SmsCpu>;

private:
	Emulator* _emu = nullptr;
	SmsConsole* _console = nullptr;
	SmsMemoryManager* _memoryManager = nullptr;

	void InitPostBiosState();

	// Z80Engine host interface
	template<MemoryOperationType type>
	__forceinline uint8_t BusRead(uint16_t addr);
	__forceinline void BusWrite(uint16_t addr, uint8_t value);
	__forceinline uint8_t BusReadPort(uint8_t port);
	__forceinline void BusWritePort(uint8_t port, uint8_t value);
	__forceinline void OnExecCycles(uint8_t cycles);
	uint8_t GetInterruptDataBus();
	bool HasNmosLdIrQuirk();
	__forceinline void OnInstruction();
	void OnHaltedCycle();
	void OnHaltStarted();
	void OnHaltEnded();
	void OnInterruptAccepted(uint16_t originalPc, uint16_t handlerPc, bool nmi);

public:
	void Init(Emulator* emu, SmsConsole* console, SmsMemoryManager* memoryManager);

	void Serialize(Serializer& s) override;

	#ifdef DUMMYCPU
private:
	uint32_t _memOpCounter = 0;
	MemoryOperationInfo _memOperations[10] = {};

public:
	void SetDummyState(SmsCpuState& state);
	uint32_t GetOperationCount();
	void LogMemoryOperation(uint32_t addr, uint8_t value, MemoryOperationType type, MemoryType memType);
	MemoryOperationInfo GetOperationInfo(uint32_t index);
	#endif
};
#endif
