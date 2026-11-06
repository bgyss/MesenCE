// singlestep.cpp - differential test of MesenCE's GBA CPU (GbaCpu, ARM7TDMI) against the
// SingleStepTests ARM7TDMI suite (https://github.com/SingleStepTests/ARM7TDMI).
//
// The suite is generated from NanoBoyAdvance and marked EXPERIMENTAL by its author. Each
// .json.bin file holds 20,000 tests of one encoding category (ARM or Thumb). Per test:
// the initial and final CPU state (user-bank R0-R15, the FIQ/SVC/ABT/IRQ/UND banks, CPSR,
// the five SPSRs, and the two-entry pipeline), the memory transactions in order, the
// opcode and its address. The binary format is read directly (see transcode_json.py).
// Its README says cycle counts are not recorded reliably, so timing is not checked.
//
// The harness compiles the real GbaCpu sources (GbaCpu.cpp, .Arm.cpp, .Thumb.cpp) as
// "TestGbaCpu", with GbaMemoryManager replaced by TestGbaBus and GbaRomPrefetch by a
// no-op. TestGbaBus answers reads from the test's transactions: code reads return the
// opcode at base_addr and otherwise the address itself (the suite's convention), data
// reads return the recorded value. Alignment, sign extension and rotation of misaligned
// reads are done exactly as GbaMemoryManager::Read does them.
//
// Checked:
//   - data reads and writes, in order: address, size, direction, and value for writes;
//   - code fetches: MesenCE keeps one more fetched word than NanoBoyAdvance (its Fetch
//     stage), so its last fetch is the next instruction's, and NBA's first fetch (at R15)
//     is the one MesenCE already did before the instruction. Those two are dropped and
//     the remaining fetch addresses compared in order;
//   - final registers in every bank, CPSR and SPSRs (bits 31-28, 7-0), and the two
//     pipeline entries.
//
// Usage: singlestep [--details N] <file.json.bin | dir> ...

#include "pch.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include "Shared/Emulator.h"
#include "Shared/EmuSettings.h"
#include "Shared/EventType.h"
#include "Shared/MemoryOperationType.h"
#include "Utilities/Serializer.h"
#include "Utilities/FolderUtilities.h"
#include "GBA/GbaTypes.h"
#include "GBA/GbaMemoryManager.h"
#include "GBA/GbaRomPrefetch.h"
#include "GBA/GbaCpuMultiply.h"
#include "GBA/GbaCpu.h"

namespace fs = std::filesystem;

struct Transaction
{
	uint32_t Kind; // 0 code read, 1 data read, 2 write
	uint32_t Size;
	uint32_t Addr;
	uint32_t Data;
};

struct BusOp
{
	uint32_t Kind;
	uint32_t Size;
	uint32_t Addr;
	uint32_t Data;
};

// Stands in for GbaMemoryManager: the members GbaCpu calls.
class TestGbaBus
{
public:
	vector<Transaction>* Expected = nullptr;
	vector<bool> Used;
	uint32_t BaseAddr = 0;
	uint32_t Opcode = 0;
	vector<BusOp> Ops;
	uint64_t Clock = 0;

	static uint32_t SizeOf(GbaAccessModeVal mode) { return (mode & GbaAccessMode::Byte) ? 1 : ((mode & GbaAccessMode::HalfWord) ? 2 : 4); }

	uint32_t RawRead(bool code, uint32_t addr, uint32_t size)
	{
		if(code) {
			uint32_t v = addr == BaseAddr ? Opcode : addr;
			return size == 2 ? (v & 0xFFFF) : v;
		}
		// Next unused data read at this address
		for(size_t i = 0; i < Expected->size(); i++) {
			const Transaction& t = (*Expected)[i];
			if(!Used[i] && t.Kind == 1 && t.Addr == addr) {
				Used[i] = true;
				return t.Data;
			}
		}
		return 0;
	}

	// Same alignment, sign extension and rotation as GbaMemoryManager::Read
	uint32_t Read(GbaAccessModeVal mode, uint32_t addr)
	{
		Clock++;
		bool code = mode & GbaAccessMode::Prefetch;
		uint32_t size = SizeOf(mode);
		uint32_t raw = RawRead(code, addr, size);
		Ops.push_back({ code ? 0u : 1u, size, addr, raw });
		bool isSigned = mode & GbaAccessMode::Signed;
		uint32_t value;
		if(size == 1) {
			value = isSigned ? (uint32_t)(int8_t)raw : (uint8_t)raw;
		} else if(size == 2) {
			value = isSigned ? (uint32_t)(int16_t)raw : (uint16_t)raw;
			if(!(mode & GbaAccessMode::NoRotate) && (addr & 0x01)) {
				value = Rotate(mode, addr, value, isSigned);
			}
		} else {
			value = raw;
			if(!(mode & GbaAccessMode::NoRotate) && (addr & 0x03)) {
				value = Rotate(mode, addr, value, isSigned);
			}
		}
		return value;
	}

	static uint32_t Rotate(GbaAccessModeVal mode, uint32_t addr, uint32_t value, bool isSigned)
	{
		uint8_t shift = (addr & ((mode & GbaAccessMode::HalfWord) ? 0x01 : 0x03)) << 3;
		if(shift) {
			return isSigned ? (uint32_t)((int32_t)value >> shift) : ((value << (32 - shift)) | (value >> shift));
		}
		return value;
	}

	void Write(GbaAccessModeVal mode, uint32_t addr, uint32_t value)
	{
		Clock++;
		uint32_t size = SizeOf(mode);
		uint32_t mask = size == 1 ? 0xFF : (size == 2 ? 0xFFFF : 0xFFFFFFFF);
		Ops.push_back({ 2, size, addr, value & mask });
	}

	void ProcessIdleCycle() { Clock++; }
	bool HasPendingIrq() { return false; }
	bool IsHaltOver() { return true; }
	bool IsSystemStopped() { return false; }
	void ProcessStoppedCycle() {}
	void ProcessDma() {}
	uint64_t GetMasterClock() { return Clock; }
	void LockBus() {}
	void UnlockBus() {}
	uint32_t ReadCoprocessor() { return 0; }
	void WriteCoprocessor(uint32_t) {}
	uint32_t DebugCpuRead(GbaAccessModeVal mode, uint32_t addr) { return RawRead(true, addr, SizeOf(mode)); }
};

class TestPrefetch
{
public:
	void ForceNonSequential(uint32_t) {}
};

// Compile the real GbaCpu against TestGbaBus. All headers the GbaCpu sources include
// were included above (they are guarded), so the renames only affect GbaCpu itself.
#undef __GBACPU__H
#define GbaCpu TestGbaCpu
#define GbaMemoryManager TestGbaBus
#define GbaRomPrefetch TestPrefetch
#define private public
#include "GBA/GbaCpu.h"
#undef private
#include "GBA/GbaCpu.cpp"
#include "GBA/GbaCpu.Arm.cpp"
#include "GBA/GbaCpu.Thumb.cpp"
#undef GbaRomPrefetch
#undef GbaMemoryManager
#undef GbaCpu

static string Hex(uint32_t v)
{
	std::ostringstream ss;
	ss << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << v;
	return ss.str();
}

struct ArmState
{
	uint32_t R[16];
	uint32_t Fiq[7];
	uint32_t Svc[2], Abt[2], Irq[2], Und[2];
	uint32_t Cpsr;
	uint32_t Spsr[5]; // fiq, svc, abt, irq, und
	uint32_t Pipe[2];
};

struct ArmTest
{
	ArmState Initial, Final;
	vector<Transaction> Transactions;
	uint32_t Opcode, BaseAddr;
};

static bool LoadTests(const fs::path& path, vector<ArmTest>& tests)
{
	std::ifstream in(path, std::ios::binary);
	vector<uint8_t> buf((std::istreambuf_iterator<char>(in)), {});
	auto u32 = [&](size_t p) {
		uint32_t v;
		memcpy(&v, buf.data() + p, 4);
		return v;
	};
	if(buf.size() < 8 || u32(0) != 0xD33DBAE0) {
		return false;
	}
	uint32_t count = u32(4);
	size_t p = 8;
	auto loadState = [&](size_t q, ArmState& s) {
		uint32_t v[40];
		for(int i = 0; i < 40; i++) {
			v[i] = u32(q + 8 + i * 4);
		}
		memcpy(s.R, v, 16 * 4);
		memcpy(s.Fiq, v + 16, 7 * 4);
		memcpy(s.Svc, v + 23, 8);
		memcpy(s.Abt, v + 25, 8);
		memcpy(s.Irq, v + 27, 8);
		memcpy(s.Und, v + 29, 8);
		s.Cpsr = v[31];
		memcpy(s.Spsr, v + 32, 5 * 4);
		memcpy(s.Pipe, v + 37, 8);
		return (size_t)u32(q);
	};
	for(uint32_t i = 0; i < count; i++) {
		ArmTest t;
		size_t full = u32(p);
		size_t q = p + 4;
		q += loadState(q, t.Initial);
		q += loadState(q, t.Final);
		size_t tsz = u32(q);
		uint32_t n = u32(q + 8);
		for(uint32_t k = 0; k < n; k++) {
			size_t r = q + 12 + k * 24;
			t.Transactions.push_back({ u32(r), u32(r + 4), u32(r + 8), u32(r + 12) });
		}
		q += tsz;
		t.Opcode = u32(q + 8);
		t.BaseAddr = u32(q + 12);
		tests.push_back(std::move(t));
		p += full;
	}
	return true;
}

static GbaCpuFlags ToFlags(uint32_t v)
{
	GbaCpuFlags f = {};
	f.Mode = (GbaCpuMode)(v & 0x1F);
	f.Thumb = v & 0x20;
	f.FiqDisable = v & 0x40;
	f.IrqDisable = v & 0x80;
	f.Overflow = v & (1u << 28);
	f.Carry = v & (1u << 29);
	f.Zero = v & (1u << 30);
	f.Negative = v & (1u << 31);
	return f;
}

enum FailKind
{
	FailRegs = 1,
	FailBus = 2,
	FailPipe = 4,
};

struct Harness
{
	TestGbaBus Bus;
	TestPrefetch Prefetch;
	TestGbaCpu Cpu;
	// Set by Run: something other than R15, the pipeline or the code fetch addresses differed
	bool OtherThanPcDiffers = false;

	// Load the suite's banks into MesenCE's layout: R is the live view of the current mode,
	// UserRegs holds the user R8-R14, and each mode's bank its own registers.
	void Load(const ArmTest& t)
	{
		const ArmState& s = t.Initial;
		GbaCpuState& st = Cpu._state;
		st = {};
		// CPU members outside _state that carry over between instructions
		Cpu._ldmGlitch = 0;
		Cpu._hasPendingIrq = false;
		memcpy(st.UserRegs, &s.R[8], 7 * 4);
		memcpy(st.FiqRegs, s.Fiq, 7 * 4);
		memcpy(st.SupervisorRegs, s.Svc, 8);
		memcpy(st.AbortRegs, s.Abt, 8);
		memcpy(st.IrqRegs, s.Irq, 8);
		memcpy(st.UndefinedRegs, s.Und, 8);
		st.CPSR = ToFlags(s.Cpsr);
		st.FiqSpsr = ToFlags(s.Spsr[0]);
		st.SupervisorSpsr = ToFlags(s.Spsr[1]);
		st.AbortSpsr = ToFlags(s.Spsr[2]);
		st.IrqSpsr = ToFlags(s.Spsr[3]);
		st.UndefinedSpsr = ToFlags(s.Spsr[4]);

		memcpy(st.R, s.R, 16 * 4);
		switch(st.CPSR.Mode) {
			case GbaCpuMode::Fiq: memcpy(&st.R[8], s.Fiq, 7 * 4); break;
			case GbaCpuMode::Supervisor: memcpy(&st.R[13], s.Svc, 8); break;
			case GbaCpuMode::Abort: memcpy(&st.R[13], s.Abt, 8); break;
			case GbaCpuMode::Irq: memcpy(&st.R[13], s.Irq, 8); break;
			case GbaCpuMode::Undefined: memcpy(&st.R[13], s.Und, 8); break;
			default: break;
		}

		bool thumb = st.CPSR.Thumb;
		uint32_t step = thumb ? 2 : 4;
		GbaCpuPipeline& pipe = st.Pipeline;
		pipe.Mode = GbaAccessMode::Prefetch | (thumb ? GbaAccessMode::HalfWord : GbaAccessMode::Word) | GbaAccessMode::Sequential;
		pipe.ReloadRequested = false;
		pipe.Execute = { t.BaseAddr, s.Pipe[0] };
		pipe.Decode = { t.BaseAddr + step, s.Pipe[1] };
		// MesenCE has already fetched the word at R15 (NBA fetches it during this instruction)
		pipe.Fetch = { s.R[15], thumb ? (s.R[15] & 0xFFFF) : s.R[15] };
		if(s.R[15] == t.BaseAddr) {
			pipe.Fetch.OpCode = t.Opcode;
		}

		Bus.Expected = const_cast<vector<Transaction>*>(&t.Transactions);
		Bus.Used.assign(t.Transactions.size(), false);
		Bus.BaseAddr = t.BaseAddr;
		Bus.Opcode = t.Opcode;
		Bus.Ops.clear();
	}

	// The suite's view (user bank + per-mode banks) of MesenCE's current state
	ArmState Snapshot()
	{
		GbaCpuState& st = Cpu._state;
		ArmState s = {};
		GbaCpuMode mode = st.CPSR.Mode;
		memcpy(s.R, st.R, 16 * 4);
		memcpy(&s.R[8], st.UserRegs, 7 * 4);
		memcpy(s.Fiq, st.FiqRegs, 7 * 4);
		memcpy(s.Svc, st.SupervisorRegs, 8);
		memcpy(s.Abt, st.AbortRegs, 8);
		memcpy(s.Irq, st.IrqRegs, 8);
		memcpy(s.Und, st.UndefinedRegs, 8);
		if(mode == GbaCpuMode::Fiq) {
			memcpy(s.Fiq, &st.R[8], 7 * 4);
		} else {
			memcpy(&s.R[8], &st.R[8], 5 * 4);
			switch(mode) {
				case GbaCpuMode::Supervisor: memcpy(s.Svc, &st.R[13], 8); break;
				case GbaCpuMode::Abort: memcpy(s.Abt, &st.R[13], 8); break;
				case GbaCpuMode::Irq: memcpy(s.Irq, &st.R[13], 8); break;
				case GbaCpuMode::Undefined: memcpy(s.Und, &st.R[13], 8); break;
				default: memcpy(&s.R[13], &st.R[13], 8); break;
			}
		}
		s.Cpsr = st.CPSR.ToInt32();
		s.Spsr[0] = st.FiqSpsr.ToInt32();
		s.Spsr[1] = st.SupervisorSpsr.ToInt32();
		s.Spsr[2] = st.AbortSpsr.ToInt32();
		s.Spsr[3] = st.IrqSpsr.ToInt32();
		s.Spsr[4] = st.UndefinedSpsr.ToInt32();
		s.Pipe[0] = st.Pipeline.Execute.OpCode;
		s.Pipe[1] = st.Pipeline.Decode.OpCode;
		return s;
	}

	int Run(const ArmTest& t, string& detail)
	{
		Load(t);
		Cpu.Exec<false, false>();
		ArmState a = Snapshot();
		const ArmState& e = t.Final;
		int kinds = 0;
		std::ostringstream d;
		OtherThanPcDiffers = false;

		auto chk = [&](const string& name, uint32_t actual, uint32_t expected) {
			if(actual != expected) {
				kinds |= FailRegs;
				if(name != "R15") {
					OtherThanPcDiffers = true;
				}
				d << " " << name << "=" << Hex(actual) << "(exp " << Hex(expected) << ")";
			}
		};
		for(int i = 0; i < 16; i++) {
			chk("R" + std::to_string(i), a.R[i], e.R[i]);
		}
		for(int i = 0; i < 7; i++) {
			chk("R" + std::to_string(8 + i) + "_fiq", a.Fiq[i], e.Fiq[i]);
		}
		const char* bankNames[] = { "svc", "abt", "irq", "und" };
		const uint32_t* aBanks[] = { a.Svc, a.Abt, a.Irq, a.Und };
		const uint32_t* eBanks[] = { e.Svc, e.Abt, e.Irq, e.Und };
		for(int b = 0; b < 4; b++) {
			for(int i = 0; i < 2; i++) {
				chk(string(i ? "R14_" : "R13_") + bankNames[b], aBanks[b][i], eBanks[b][i]);
			}
		}
		const uint32_t psrMask = 0xF00000FF;
		chk("CPSR", a.Cpsr & psrMask, e.Cpsr & psrMask);
		const char* spsrNames[] = { "SPSR_fiq", "SPSR_svc", "SPSR_abt", "SPSR_irq", "SPSR_und" };
		for(int i = 0; i < 5; i++) {
			chk(spsrNames[i], a.Spsr[i] & psrMask, e.Spsr[i] & psrMask);
		}
		if(a.Pipe[0] != e.Pipe[0] || a.Pipe[1] != e.Pipe[1]) {
			kinds |= FailPipe;
			d << " pipe=" << Hex(a.Pipe[0]) << "," << Hex(a.Pipe[1]) << "(exp " << Hex(e.Pipe[0]) << "," << Hex(e.Pipe[1]) << ")";
		}

		// Bus: data accesses in order, then code fetch addresses (see the header comment)
		vector<BusOp> aData, eData;
		vector<uint32_t> aCode, eCode;
		for(const BusOp& op : Bus.Ops) {
			(op.Kind == 0 ? (void)aCode.push_back(op.Addr) : aData.push_back(op));
		}
		for(const Transaction& tr : t.Transactions) {
			if(tr.Kind == 0) {
				eCode.push_back(tr.Addr);
			} else {
				uint32_t mask = tr.Size == 1 ? 0xFF : (tr.Size == 2 ? 0xFFFF : 0xFFFFFFFF);
				eData.push_back({ tr.Kind, tr.Size, tr.Addr, tr.Data & mask });
			}
		}
		if(!aCode.empty()) {
			aCode.pop_back();
		}
		if(!eCode.empty()) {
			eCode.erase(eCode.begin());
		}
		bool dataOk = aData.size() == eData.size();
		for(size_t i = 0; dataOk && i < aData.size(); i++) {
			const BusOp& x = aData[i];
			const BusOp& y = eData[i];
			dataOk = x.Kind == y.Kind && x.Size == y.Size && x.Addr == y.Addr && (x.Kind == 1 || x.Data == y.Data);
		}
		if(!dataOk) {
			OtherThanPcDiffers = true;
		}
		bool busOk = dataOk && aCode == eCode;
		if(!busOk) {
			kinds |= FailBus;
			auto fmt = [](const vector<BusOp>& ops, const vector<uint32_t>& code) {
				std::ostringstream ss;
				for(const BusOp& op : ops) {
					ss << " " << (op.Kind == 2 ? "W" : "R") << op.Size << ":" << Hex(op.Addr) << "=" << Hex(op.Data);
				}
				ss << " | code";
				for(uint32_t c : code) {
					ss << " " << Hex(c);
				}
				return ss.str();
			};
			d << "\n      actual bus:  " << fmt(aData, aCode) << "\n      expected bus:" << fmt(eData, eCode);
		}

		std::ostringstream name;
		name << Hex(t.Opcode) << (ToFlags(t.Initial.Cpsr).Thumb ? " (thumb)" : "");
		detail = name.str() + ":" + d.str();
		return kinds;
	}
};

// Files where MesenCE deliberately differs from the suite. Reported, not counted as failures.
// See docs/research/gba-verification-findings.md.
static const std::map<string, string> KnownDivergences = {};

// Classifies a *failing* test as an encoding whose result the ARM Architecture Reference
// Manual (ARMv4T) leaves UNPREDICTABLE or undefined, or as a deliberate MesenCE model.
// Returns "" when the failure has no such explanation (= an unexpected failure).
// Only failing tests are classified, so passes in these categories still count.
static string ClassifyDivergence(const ArmTest& t, bool otherThanPcDiffers)
{
	uint32_t op = t.Opcode;
	bool thumbBefore = t.Initial.Cpsr & 0x20;
	bool thumbAfter = t.Final.Cpsr & 0x20;
	auto reg = [&](int bit) { return (op >> bit) & 0x0F; };

	if(thumbBefore) {
		if((op & 0xFF00) == 0xDE00) {
			return "thumb-bcc-cond-e";
		}
		bool pcLowBits = (thumbAfter && (t.Final.R[15] & 1)) || (!thumbAfter && (t.Final.R[15] & 3));
		return pcLowBits && !otherThanPcDiffers ? "pc-low-bits" : "";
	}

	// ARM state: PC written with bits[1:0] != 0 (or bit 0 when switching to Thumb), and nothing
	// else differs. Otherwise fall through to the operand checks below.
	bool pcLowBits = (!thumbAfter && (t.Final.R[15] & 3)) || (thumbAfter && (t.Final.R[15] & 1));
	if(pcLowBits && !otherThanPcDiffers) {
		return "pc-low-bits";
	}
	bool isMsr = (op & 0x0DB0F000) == 0x0120F000;
	if(isMsr && thumbBefore != thumbAfter && !otherThanPcDiffers) {
		return "msr-t-bit";
	}
	bool isMrs = (op & 0x0FBF0FFF) == 0x010F0000;
	if(isMrs && reg(12) == 15) {
		return "r15-operand";
	}
	bool isMul = (op & 0x0FC000F0) == 0x00000090;
	bool isMull = (op & 0x0F8000F0) == 0x00800090;
	bool isSwp = (op & 0x0FB00FF0) == 0x01000090;
	if((isMul || isMull || isSwp) && (reg(16) == 15 || reg(12) == 15 || reg(8) == 15 || reg(0) == 15)) {
		return "r15-operand";
	}
	bool isCoproc = ((op >> 25) & 0x07) == 0x06 || ((op >> 24) & 0x0F) == 0x0E;
	if(isCoproc && reg(8) == 14) {
		return "cp14";
	}
	// Data processing with a register-specified shift and R15 as an operand
	bool isDataProcRegShift = ((op >> 25) & 0x07) == 0 && (op & 0x90) == 0x10;
	if(isDataProcRegShift && (reg(16) == 15 || reg(0) == 15 || reg(8) == 15 || reg(12) == 15)) {
		return "r15-operand";
	}
	// Single and halfword transfers with writeback to R15, or Rn == Rd with writeback
	bool isLdrStr = ((op >> 26) & 0x03) == 0x01;
	bool isHalf = ((op >> 25) & 0x07) == 0 && (op & 0x90) == 0x90 && (op & 0x60) != 0;
	if(isLdrStr || isHalf) {
		bool writeback = !((op >> 24) & 1) || ((op >> 21) & 1);
		if(writeback && (reg(16) == 15 || reg(16) == reg(12))) {
			return "r15-operand";
		}
		bool regOffset = isLdrStr ? ((op >> 25) & 1) : !((op >> 22) & 1);
		if(regOffset && reg(0) == 15) {
			return "r15-operand";
		}
	}
	// Block transfers with Rn == R15
	bool isLdmStm = ((op >> 25) & 0x07) == 0x04;
	if(isLdmStm && reg(16) == 15) {
		return "r15-operand";
	}
	// LDM/STM with the S bit (user bank) and writeback, without PC in an LDM list
	bool sBit = (op >> 22) & 1;
	bool wBit = (op >> 21) & 1;
	bool lBit = (op >> 20) & 1;
	if(isLdmStm && sBit && wBit && !(lBit && (op & 0x8000))) {
		return "ldm-stm-user-writeback";
	}
	return "";
}

static const std::map<string, string> DivergenceReasons = {
	{ "pc-low-bits", "PC written with misaligned low bits (ARM: bits 1:0, Thumb: bit 0). UNPREDICTABLE in ARMv4 for ARM state; NanoBoyAdvance keeps the raw bits in R15 and fetches aligned, MesenCE aligns R15. The word fetched is the same" },
	{ "msr-t-bit", "MSR changing the T bit: UNPREDICTABLE (ARM ARM); MesenCE switches the fetch width, NanoBoyAdvance doesn't" },
	{ "r15-operand", "R15 as an operand/destination in a role the ARM ARM marks UNPREDICTABLE (multiply, SWP, MRS, register-shift data processing, writeback base, register offset, LDM/STM base)" },
	{ "thumb-bcc-cond-e", "Thumb conditional branch with cond 1110 (DExx): undefined in ARMv4T. MesenCE takes the undefined-instruction exception; NanoBoyAdvance branches" },
	{ "ldm-stm-user-writeback", "LDM/STM with ^ (user-bank transfer) and writeback: UNPREDICTABLE (ARM ARM LDM(2)/STM(2)). MesenCE writes the base back while in the user bank, NanoBoyAdvance to the current mode's bank" },
	{ "cp14", "MCR/MRC/LDC/STC/CDP on coprocessor 14: MesenCE models the ARM7TDMI debug comms channel (open bus); NanoBoyAdvance raises an undefined-instruction exception" },
};

int main(int argc, char* argv[])
{
	int maxDetails = 3;
	int64_t dumpOpcode = -1;
	vector<fs::path> files;
	for(int i = 1; i < argc; i++) {
		string a = argv[i];
		if(a == "--details" && i + 1 < argc) {
			maxDetails = std::stoi(argv[++i]);
		} else if(a == "--dump" && i + 1 < argc) {
			dumpOpcode = std::stoll(argv[++i], nullptr, 16);
		} else if(fs::is_directory(a)) {
			for(const auto& e : fs::directory_iterator(a)) {
				if(e.path().string().size() > 9 && e.path().string().substr(e.path().string().size() - 9) == ".json.bin") {
					files.push_back(e.path());
				}
			}
		} else {
			files.push_back(a);
		}
	}
	std::sort(files.begin(), files.end());
	if(files.empty()) {
		std::cerr << "usage: singlestep [--details N] <file.json.bin | dir> ...\n";
		return 2;
	}

	fs::path home = fs::temp_directory_path() / "mesen-gba-singlestep-home";
	fs::create_directories(home);
	FolderUtilities::SetHomeFolder(home.string());
	Emulator emu;
	emu.Initialize(false);
	TestGbaCpu::StaticInit();

	Harness* h = new Harness();
	h->Cpu.Init(&emu, &h->Bus, &h->Prefetch);

	uint64_t total = 0, failed = 0, known = 0;
	int failedFiles = 0;
	std::map<string, uint64_t> divergenceCounts;
	for(const fs::path& f : files) {
		vector<ArmTest> tests;
		if(!LoadTests(f, tests)) {
			std::cerr << "bad test file " << f << "\n";
			continue;
		}
		string op = f.filename().string();
		op = op.substr(0, op.size() - 9);
		int fail = 0, shown = 0, classified = 0;
		std::map<int, int> kindCounts;
		vector<string> details;
		for(const ArmTest& t : tests) {
			string detail;
			int k = h->Run(t, detail);
			total++;
			if(dumpOpcode == (int64_t)t.Opcode) {
				std::cout << "dump " << detail << "\n  ops:";
				for(const BusOp& o : h->Bus.Ops) {
					std::cout << " " << o.Kind << ":" << Hex(o.Addr) << "=" << Hex(o.Data);
				}
				std::cout << "\n  R:";
				for(int r = 0; r < 16; r++) {
					std::cout << " " << Hex(h->Cpu._state.R[r]);
				}
				std::cout << " mode=" << (int)h->Cpu._state.CPSR.Mode << "\n";
				dumpOpcode = -1;
			}
			if(k) {
				string cls = ClassifyDivergence(t, h->OtherThanPcDiffers);
				if(!cls.empty()) {
					divergenceCounts[cls]++;
					classified++;
					known++;
					continue;
				}
				fail++;
				kindCounts[k]++;
				if(shown++ < maxDetails) {
					details.push_back(detail);
				}
			}
		}
		auto knownIt = KnownDivergences.find(op);
		bool isKnown = knownIt != KnownDivergences.end();
		(isKnown ? known : failed) += fail;
		if(!fail) {
			std::cout << "ok   " << op << ": " << (tests.size() - classified) << "/" << tests.size();
			if(classified) {
				std::cout << " (+" << classified << " known divergence)";
			}
			std::cout << "\n";
			continue;
		}
		if(!isKnown) {
			failedFiles++;
		}
		std::cout << (isKnown ? "KNOWN" : "FAIL ") << " " << op << ": " << (tests.size() - fail) << "/" << tests.size() << " pass (";
		bool first = true;
		for(auto& [k, cnt] : kindCounts) {
			std::cout << (first ? "" : ", ") << cnt << "x" << ((k & FailRegs) ? " regs" : "") << ((k & FailPipe) ? " pipe" : "") << ((k & FailBus) ? " bus" : "");
			first = false;
		}
		std::cout << ")";
		if(isKnown) {
			std::cout << " " << knownIt->second;
		}
		std::cout << "\n";
		if(!isKnown) {
			for(const string& s : details) {
				std::cout << "    " << s << "\n";
			}
		}
	}
	for(auto& [cls, cnt] : divergenceCounts) {
		std::cout << "KNOWN " << cls << ": " << cnt << " - " << DivergenceReasons.at(cls) << "\n";
	}
	std::cout << "TOTAL: " << (total - failed - known) << "/" << total << " pass, " << failed << " unexpected fail (" << failedFiles << " of " << files.size()
				 << " files), " << known << " known divergence\n";
	return failed ? 1 : 0;
}
