#include "pch.h"
#include "MD/Debugger/MdEventManager.h"
#include "MD/MdVdp.h"
#include "Debugger/Debugger.h"
#include "Debugger/DebugBreakHelper.h"

MdEventManager::MdEventManager(Debugger* debugger, MdVdp* vdp, CpuType cpuType)
{
	_debugger = debugger;
	_vdp = vdp;
	_cpuType = cpuType;
}

void MdEventManager::AddEvent(DebugEventType type, MemoryOperationInfo& operation, int32_t breakpointId)
{
	DebugEventInfo evt = {};
	evt.Type = type;
	evt.Flags = (uint32_t)EventFlags::ReadWriteOp;
	evt.Operation = operation;
	evt.Scanline = _vdp->GetState().Scanline;
	evt.Cycle = _vdp->GetState().HClock;
	evt.BreakpointId = breakpointId;
	evt.DmaChannel = -1;
	evt.ProgramCounter = _debugger->GetProgramCounter(_cpuType, true);
	_debugEvents.push_back(evt);
}

void MdEventManager::AddEvent(DebugEventType type)
{
	DebugEventInfo evt = {};
	evt.Type = type;
	evt.Scanline = _vdp->GetState().Scanline;
	evt.Cycle = _vdp->GetState().HClock;
	evt.BreakpointId = -1;
	evt.DmaChannel = -1;
	evt.ProgramCounter = _debugger->GetProgramCounter(_cpuType, false);
	_debugEvents.push_back(evt);
}

EventViewerCategoryCfg MdEventManager::GetEventConfig(DebugEventInfo& evt)
{
	switch(evt.Type) {
		case DebugEventType::Breakpoint: return _config.MarkedBreakpoints;
		case DebugEventType::Irq: return _config.Irq;
		case DebugEventType::Nmi: return _config.Nmi;
		case DebugEventType::Register: return evt.Operation.Type == MemoryOperationType::Write ? _config.VdpWrite : _config.VdpRead;
		default: return {};
	}
}

uint32_t MdEventManager::TakeEventSnapshot(bool forAutoRefresh)
{
	DebugBreakHelper breakHelper(_debugger);
	auto lock = _lock.AcquireSafe();
	_snapshotCurrentFrame = _debugEvents;
	_snapshotPrevFrame = _prevDebugEvents;
	_snapshotScanline = _vdp->GetState().Scanline;
	_snapshotCycle = _vdp->GetState().HClock;
	_forAutoRefresh = forAutoRefresh;
	_scanlineCount = _vdp->GetLinesPerFrame();
	return _scanlineCount;
}

DebugEventInfo MdEventManager::GetEvent(uint16_t y, uint16_t x)
{
	auto lock = _lock.AcquireSafe();
	int scanline = y / 2;
	for(DebugEventInfo& evt : _sentEvents) {
		if(evt.Scanline == scanline && std::abs((int)evt.Cycle / 4 - (int)x) <= 1) {
			return evt;
		}
	}
	DebugEventInfo empty = {};
	empty.ProgramCounter = 0xFFFFFFFF;
	return empty;
}

FrameInfo MdEventManager::GetDisplayBufferSize()
{
	FrameInfo size;
	size.Width = ScanlineWidth;
	size.Height = _scanlineCount * 2;
	return size;
}
