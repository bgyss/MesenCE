#include "pch.h"
#include "Debugger/ExpressionEvaluator.h"
#include "MD/Debugger/MdDebugger.h"
#include "MD/MdTypes.h"

unordered_map<string, int64_t>& ExpressionEvaluator::GetMdM68kTokens()
{
	static unordered_map<string, int64_t> supportedTokens = {
		{ "d0", EvalValues::R0 },
		{ "d1", EvalValues::R1 },
		{ "d2", EvalValues::R2 },
		{ "d3", EvalValues::R3 },
		{ "d4", EvalValues::R4 },
		{ "d5", EvalValues::R5 },
		{ "d6", EvalValues::R6 },
		{ "d7", EvalValues::R7 },
		{ "a0", EvalValues::R8 },
		{ "a1", EvalValues::R9 },
		{ "a2", EvalValues::R10 },
		{ "a3", EvalValues::R11 },
		{ "a4", EvalValues::R12 },
		{ "a5", EvalValues::R13 },
		{ "a6", EvalValues::R14 },
		{ "a7", EvalValues::R15 },
		{ "sp", EvalValues::RegSP },
		{ "sr", EvalValues::RegSR },
		{ "pc", EvalValues::RegPC },
		{ "frame", EvalValues::PpuFrameCount },
		{ "cycle", EvalValues::PpuCycle },
		{ "scanline", EvalValues::PpuScanline },
	};
	return supportedTokens;
}

int64_t ExpressionEvaluator::GetMdM68kTokenValue(int64_t token, EvalResultType& resultType)
{
	MdM68kState& s = (MdM68kState&)((MdDebugger*)_cpuDebugger)->GetState();
	auto vdp = [this]() -> MdVdpState {
		MdVdpState state;
		((MdDebugger*)_cpuDebugger)->GetPpuState(state);
		return state;
	};
	if(token >= EvalValues::R0 && token <= EvalValues::R7) {
		return s.D[token - EvalValues::R0];
	} else if(token >= EvalValues::R8 && token <= EvalValues::R15) {
		return s.A[token - EvalValues::R8];
	}
	switch(token) {
		case EvalValues::RegSP: return s.A[7];
		case EvalValues::RegSR: return s.SR;
		case EvalValues::RegPC: return s.PC;
		case EvalValues::PpuFrameCount: return vdp().FrameCount;
		case EvalValues::PpuCycle: return vdp().HClock;
		case EvalValues::PpuScanline: return vdp().Scanline;
		default: return 0;
	}
}

unordered_map<string, int64_t>& ExpressionEvaluator::GetMdZ80Tokens()
{
	static unordered_map<string, int64_t> supportedTokens = {
		{ "a", EvalValues::RegA },
		{ "b", EvalValues::RegB },
		{ "c", EvalValues::RegC },
		{ "d", EvalValues::RegD },
		{ "e", EvalValues::RegE },
		{ "f", EvalValues::RegF },
		{ "h", EvalValues::RegH },
		{ "l", EvalValues::RegL },
		{ "af", EvalValues::RegAF },
		{ "bc", EvalValues::RegBC },
		{ "de", EvalValues::RegDE },
		{ "hl", EvalValues::RegHL },
		{ "a'", EvalValues::RegAltA },
		{ "b'", EvalValues::RegAltB },
		{ "c'", EvalValues::RegAltC },
		{ "d'", EvalValues::RegAltD },
		{ "e'", EvalValues::RegAltE },
		{ "f'", EvalValues::RegAltF },
		{ "h'", EvalValues::RegAltH },
		{ "l'", EvalValues::RegAltL },
		{ "i", EvalValues::RegI },
		{ "r", EvalValues::RegR },
		{ "ix", EvalValues::RegIX },
		{ "iy", EvalValues::RegIY },
		{ "sp", EvalValues::RegSP },
		{ "pc", EvalValues::RegPC },
		{ "frame", EvalValues::PpuFrameCount },
		{ "cycle", EvalValues::PpuCycle },
		{ "scanline", EvalValues::PpuScanline },
	};
	return supportedTokens;
}

int64_t ExpressionEvaluator::GetMdZ80TokenValue(int64_t token, EvalResultType& resultType)
{
	MdZ80State& s = (MdZ80State&)((MdDebugger*)_cpuDebugger)->GetState();
	auto vdp = [this]() -> MdVdpState {
		MdVdpState state;
		((MdDebugger*)_cpuDebugger)->GetPpuState(state);
		return state;
	};
	switch(token) {
		case EvalValues::RegA: return s.A;
		case EvalValues::RegB: return s.B;
		case EvalValues::RegC: return s.C;
		case EvalValues::RegD: return s.D;
		case EvalValues::RegE: return s.E;
		case EvalValues::RegF: return s.Flags;
		case EvalValues::RegH: return s.H;
		case EvalValues::RegL: return s.L;
		case EvalValues::RegAF: return (s.A << 8) | s.Flags;
		case EvalValues::RegBC: return (s.B << 8) | s.C;
		case EvalValues::RegDE: return (s.D << 8) | s.E;
		case EvalValues::RegHL: return (s.H << 8) | s.L;
		case EvalValues::RegAltA: return s.AltA;
		case EvalValues::RegAltB: return s.AltB;
		case EvalValues::RegAltC: return s.AltC;
		case EvalValues::RegAltD: return s.AltD;
		case EvalValues::RegAltE: return s.AltE;
		case EvalValues::RegAltF: return s.AltFlags;
		case EvalValues::RegAltH: return s.AltH;
		case EvalValues::RegAltL: return s.AltL;
		case EvalValues::RegIX: return (s.IXH << 8) | s.IXL;
		case EvalValues::RegIY: return (s.IYH << 8) | s.IYL;
		case EvalValues::RegI: return s.I;
		case EvalValues::RegR: return s.R;
		case EvalValues::RegSP: return s.SP;
		case EvalValues::RegPC: return s.PC;
		case EvalValues::PpuFrameCount: return vdp().FrameCount;
		case EvalValues::PpuCycle: return vdp().HClock;
		case EvalValues::PpuScanline: return vdp().Scanline;
		default: return 0;
	}
}
