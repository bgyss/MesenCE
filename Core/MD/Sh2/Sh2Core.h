#pragma once
#include <cstdint>
#include <string>

class Serializer;

// Hitachi SH-2 (SH7604) as used in the Sega 32X: the CPU plus the on-chip modules games use
// (cache, interrupt controller, DMAC, DIVU, FRT, WDT, SCI; BSC/UBC/SBYCR registers are stored).
//
// The instruction semantics, cache and on-chip register map are ported from ares
// (ares/component/processor/sh2, ISC licence, Copyright (c) 2004-2025 ares team, Near et al).
// Changes from ares are listed in docs/research/32x-verification-findings.md (S1); the main ones
// are per-instruction timing from the Hitachi SH-1/SH-2 programming manual (ares charges one
// cycle per instruction), architectural PC (ares keeps PC = instruction + 4), on-chip interrupt
// priority arbitration that includes the external IRL level, and no recompiler.
//
// Bus model: the core decodes the SH7604 address areas itself (cache, cache-through, purge,
// address array, data array, on-chip I/O). External accesses (areas 0 and 1, with the 29-bit
// physical address) go to the virtual Bus* functions, which may call AddCycles() for wait states.

struct Sh2State
{
	uint32_t R[16];
	uint32_t PC; //address of the next instruction to execute
	uint32_t PR;
	uint32_t GBR;
	uint32_t VBR;
	uint32_t MACH;
	uint32_t MACL;
	uint32_t SR; //M Q I3-I0 . . S T (mask 0x3F3)

	uint64_t CycleCount;

	bool SlotPending; //the next instruction is a delay slot; PC moves to SlotTarget after it
	uint32_t SlotTarget;
	uint32_t SlotBranchPc; //address of the delayed branch instruction
	bool NoInterrupt; //LDC/STC/LDS/STS were just executed: interrupts are not accepted before the next instruction
	bool Sleeping;
	uint8_t PendingReset; //0 = none, 1 = power-on (vectors 0/1), 2 = manual (vectors 2/3)
};

class Sh2Core
{
public:
	static constexpr uint32_t SrMask = 0x3F3;
	enum SrBits : uint32_t
	{
		SrT = 1 << 0,
		SrS = 1 << 1,
		SrQ = 1 << 8,
		SrM = 1 << 9,
	};

	struct CacheState
	{
		static constexpr uint32_t Invalid = 1 << 19;
		uint8_t Lru[64];
		uint32_t Tags[4 * 64];
		uint8_t Lines[4 * 64][16];
		bool Enable;
		bool DisableCode; //ID: instruction fills disabled
		bool DisableData; //OD: data fills disabled
		uint8_t TwoWay; //0 = 4-way, 2 = 2-way (ways 2 and 3 cache; 0 and 1 become RAM)
		uint8_t WaySelect;
	};

	struct IntcState
	{
		bool Nmie;
		bool Nmil;
		bool Vecmd;
		uint8_t WdtIp, DmacIp, DivuIp;
		uint8_t FrtIp, SciIp;
		uint8_t SciErrVec, SciRxVec, SciTxEndVec, SciTxVec;
		uint8_t FrtIcVec, FrtOcVec, FrtOvVec;
		uint8_t BscCmVec, WdtItVec;
	};

	struct DmaChannel
	{
		uint32_t Sar;
		uint32_t Dar;
		uint32_t Tcr; //24 bits; 0 means 2^24
		bool De, Te, Ie, Ta, Tb, Dl, Ds, Al, Am, Ar;
		uint8_t Ts; //0 = byte, 1 = word, 2 = long, 3 = 16 bytes
		uint8_t Sm, Dm; //0 = fixed, 1 = increment, 2 = decrement
		uint8_t Vector;
		uint8_t Drcr;
		bool Dreq; //external request line (32X: DREQ0 = 68000 FIFO, DREQ1 = PWM)
	};

	struct DmacState
	{
		DmaChannel Ch[2];
		bool Dme, Nmif, Ae, Pr;
	};

	struct DivuState
	{
		uint32_t Dvsr;
		uint32_t Dvdnth;
		uint32_t Dvdntl;
		bool Ovf, Ovfie;
		uint8_t Vector;
	};

	struct FrtState
	{
		bool Ovie, Ociae, Ocibe, Icie;
		bool Cclra, Ovf, Ocfa, Ocfb, Icf;
		uint16_t Frc, Ocra, Ocrb, Ficr;
		uint8_t Cks;
		bool Iedg;
		bool Olvla, Olvlb, Ocrs;
		uint32_t Prescaler;
	};

	struct WdtState
	{
		uint8_t Cks;
		bool Tme, Wtit, Ovf;
		bool Rsts, Rste, Wovf;
		uint8_t Wtcnt;
		uint32_t Prescaler;
	};

	struct SciState
	{
		uint8_t Smr, Brr, Scr, Tdr, Rdr;
		bool Tend, Rdrf, Tdre, Orer, Fer, Per, Mpb, Mpbt;
	};

	struct BscState
	{
		uint16_t Bcr1, Bcr2, Wcr, Mcr, Rtcsr, Rtcnt, Rtcor;
	};

	struct UbcState
	{
		uint32_t Bara, Bamra, Barb, Bamrb, Bdrb, Bdmrb;
		uint16_t Bbra, Bbrb, Brcr;
	};

protected:
	Sh2State _state = {};
	CacheState _cache = {};
	IntcState _intc = {};
	DmacState _dmac = {};
	DivuState _divu = {};
	FrtState _frt = {};
	WdtState _wdt = {};
	SciState _sci = {};
	BscState _bsc = {};
	UbcState _ubc = {};
	uint8_t _sbycr = 0;

	//External interrupt request (IRL): level 0 = none, and the vector to use
	uint8_t _irlLevel = 0;
	uint8_t _irlVector = 0;
	bool _nmiPending = false;

	Sh2Core* _sciLink = nullptr;

	//Test hook: no area decoding, every access goes to the bus with the full 32-bit address
	bool _flatBus = false;

	//Set while an exception is being entered, so a delay slot's branch doesn't override it
	bool _exceptionTaken = false;
	bool _inDelaySlot = false;
	uint32_t _curSlotTarget = 0;
	uint32_t _instPc = 0;

	uint8_t _lruSelect[64] = {};
	uint8_t _lruUpdate[4][64] = {};

	void InitLruTables();

	//Bus with area decoding
	uint32_t ReadByte(uint32_t addr);
	uint32_t ReadWord(uint32_t addr);
	uint32_t ReadLong(uint32_t addr);
	uint16_t FetchWord(uint32_t addr);
	void WriteByte(uint32_t addr, uint32_t value);
	void WriteWord(uint32_t addr, uint32_t value);
	void WriteLong(uint32_t addr, uint32_t value);

	template<int Size>
	uint32_t CacheRead(uint32_t addr, bool fetch);
	template<int Size>
	void CacheWrite(uint32_t addr, uint32_t value);
	void CachePurgeLine(uint32_t addr);
	void CachePurgeAll(int ways);

	uint8_t ReadIo8(uint32_t addr);
	void WriteIo8(uint32_t addr, uint8_t value);
	uint32_t ReadIo(uint32_t addr, int size);
	void WriteIo(uint32_t addr, uint32_t value, int size);
	void DivuStart32();
	void DivuStart64();

	void Push(uint32_t value);
	void EnterException(uint32_t vector, uint32_t returnPc);
	void IllegalInstruction();
	void IllegalSlotInstruction();
	bool CheckInterrupts();

	void DelayedBranch(uint32_t target);
	uint32_t PcRelBase();

	void Execute(uint16_t op);
	void RunPeripherals(uint32_t cycles);
	void RunDma();
	void DmaTransfer(int ch);
	void SciRun();

	void Exec0(uint16_t op);
	void Exec4(uint16_t op);
	void Exec8(uint16_t op);
	void ExecC(uint16_t op);

public:
	Sh2Core();
	virtual ~Sh2Core() = default;

	virtual uint8_t BusRead8(uint32_t addr) = 0;
	virtual uint16_t BusRead16(uint32_t addr) = 0;
	virtual uint32_t BusRead32(uint32_t addr) = 0;
	virtual void BusWrite8(uint32_t addr, uint8_t value) = 0;
	virtual void BusWrite16(uint32_t addr, uint16_t value) = 0;
	virtual void BusWrite32(uint32_t addr, uint32_t value) = 0;
	//Instruction fetch from an external area (default: a word read)
	virtual uint16_t BusFetch16(uint32_t addr) { return BusRead16(addr); }

	//Hooks for the owner (debugger, interrupt acknowledge)
	virtual void OnInstructionStart(uint32_t /*pc*/, uint16_t /*op*/) {}
	virtual void OnExternalInterruptAccepted(uint8_t /*level*/) {}

	void PowerOn();
	//Manual reset (vectors 2/3) when manual is true, otherwise power-on reset (vectors 0/1).
	//The vectors are fetched by the next Step().
	void Reset(bool manual);

	//Executes one instruction, an exception entry, or one sleep cycle
	void Step();

	void AddCycles(uint32_t cycles) { _state.CycleCount += cycles; }
	uint64_t GetCycleCount() const { return _state.CycleCount; }

	//External interrupt request: IRL level (0 = none) and the vector the owner supplies
	//(the 32X uses auto-vectors 64 + level / 2). Level-held until the owner lowers it.
	void SetIrl(uint8_t level, uint8_t vector)
	{
		_irlLevel = level;
		_irlVector = vector;
	}
	void SetNmi() { _nmiPending = true; }
	void SetDreq(int ch, bool active) { _dmac.Ch[ch].Dreq = active; }
	bool IsDmaActive(int ch) const;
	void LinkSci(Sh2Core* other) { _sciLink = other; }

	Sh2State& GetState() { return _state; }
	const CacheState& GetCacheState() const { return _cache; }
	const DmacState& GetDmacState() const { return _dmac; }
	const DivuState& GetDivuState() const { return _divu; }
	const FrtState& GetFrtState() const { return _frt; }
	bool IsSleeping() const { return _state.Sleeping; }
	void SetFlatBus(bool flat) { _flatBus = flat; }

	//Side-effect-free read of a CPU address (debugger): on-chip registers read as their value,
	//external areas through Peek (cache contents are ignored)
	virtual uint8_t Peek8(uint32_t /*addr*/) { return 0; }

	static std::string Disassemble(uint32_t pc, uint16_t op);

	void Serialize(Serializer& s);
};
