#pragma once
#include "pch.h"

// A device on a Mega Drive control port, as the I/O chip sees it. Pins are numbered as in the
// port's data register: bit 0 UP, 1 DOWN, 2 LEFT, 3 RIGHT, 4 TL, 5 TR, 6 TH.
class IMdPortDevice
{
public:
	virtual ~IMdPortDevice() = default;

	//The console's pin levels (data register bits 6-0) and which pins it drives (control
	//register bits 6-0); called on every data or control register write
	virtual void WritePins(uint8_t levels, uint8_t outputs, uint64_t clock) = 0;

	//The levels the device drives on pins 6-0 (the caller keeps the console's levels on its outputs)
	virtual uint8_t ReadPins(uint64_t clock) = 0;

	//Light guns: the pixel column the gun sees on `line`, or -1 (see MdLightGun.h)
	virtual int32_t GetLightgunX(uint16_t line) { return -1; }
	//Light guns: the beam reached the spot the gun is aimed at
	virtual void OnLightgunHit(uint64_t clock) {}
	//Light guns: master clocks between the beam passing the spot and the gun pulling TH low
	virtual uint32_t GetLightgunLatency(bool h40) { return 0; }
};
