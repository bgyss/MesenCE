#pragma once
#include "pch.h"
#include "Debugger/DebugTypes.h"
#include "Debugger/BaseEventManager.h"

class MdVdp;
class Debugger;

struct MdEventViewerConfig : public BaseEventViewerConfig
{
	EventViewerCategoryCfg Irq;
	EventViewerCategoryCfg Nmi;
	EventViewerCategoryCfg MarkedBreakpoints;
	EventViewerCategoryCfg VdpWrite;
	EventViewerCategoryCfg VdpRead;
	bool ShowPreviousFrameEvents;
};

// Event list for both Mega Drive CPUs (interrupts, breakpoints, VDP port accesses). The event
// viewer grid is one row per scanline and one column per 2 master clocks.
class MdEventManager final : public BaseEventManager
{
private:
	static constexpr int ScanlineWidth = 3420 / 4;

	MdEventViewerConfig _config = {};
	MdVdp* _vdp = nullptr;
	Debugger* _debugger = nullptr;
	CpuType _cpuType;
	uint32_t _scanlineCount = 262;

protected:
	bool ShowPreviousFrameEvents() override { return _config.ShowPreviousFrameEvents; }
	void ConvertScanlineCycleToRowColumn(int32_t& x, int32_t& y) override { x /= 4; }
	void DrawScreen(uint32_t* buffer) override {}

public:
	MdEventManager(Debugger* debugger, MdVdp* vdp, CpuType cpuType);

	void AddEvent(DebugEventType type, MemoryOperationInfo& operation, int32_t breakpointId = -1) override;
	void AddEvent(DebugEventType type) override;

	EventViewerCategoryCfg GetEventConfig(DebugEventInfo& evt) override;
	uint32_t TakeEventSnapshot(bool forAutoRefresh) override;
	DebugEventInfo GetEvent(uint16_t y, uint16_t x) override;
	FrameInfo GetDisplayBufferSize() override;
	void SetConfiguration(BaseEventViewerConfig& config) override { _config = (MdEventViewerConfig&)config; }
};
