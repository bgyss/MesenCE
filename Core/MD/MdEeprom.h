#pragma once
#include "pch.h"
#include "Utilities/ISerializable.h"

// Serial (I2C) EEPROM saves on Mega Drive cartridges.
//
// Sources:
//  - Chips: Xicor X24C01 (single address byte carrying the R/W bit, 4-byte write page) and the
//    Atmel/ST 24C01-24C16 (device byte with block bits + 8-bit address) and 24C32-24C65 (device
//    byte + 16-bit address) datasheets: START/STOP conditions, ACK on the 9th clock, sequential
//    reads, page writes that wrap inside the page.
//  - Board wiring (which address and data bit carries SDA in, SDA out and SCL): MAME,
//    src/devices/bus/megadrive/eeprom.cpp and jcart.cpp (BSD-3-Clause), from Eke-Eke's notes.
//  - Which games use which chip and board: ares' Mega Drive medium database
//    (mia/medium/mega-drive.cpp, ISC license), keyed by the ROM's SHA-256. The standard header
//    does not describe EEPROM saves, so there is no header-based detection.
enum class MdEepromChip : uint8_t
{
	None,
	X24C01,
	C24C01,
	C24C02,
	C24C04,
	C24C08,
	C24C16,
	C24C64,
	C24C65,
};

enum class MdEepromBoard : uint8_t
{
	None,
	Sega, //$200001: SDA bit 0, SCL bit 1; SDA read at $200001 bit 0
	Ea, //$200001: SDA bit 7, SCL bit 6; SDA read at $200001 bit 7
	Acclaim16, //$200001: SDA bit 0, SCL bit 1; SDA read at $200001 bit 1
	Acclaim32, //SCL $200000 bit 0, SDA $200001 bit 0; SDA read at $200001 bit 0
	Codemasters, //$300000: SDA bit 0, SCL bit 1; SDA read at $380001 bit 7
};

struct MdEepromInfo
{
	MdEepromChip Chip = MdEepromChip::None;
	MdEepromBoard Board = MdEepromBoard::None;
};

class MdEepromDevice final : public ISerializable
{
private:
	enum class Phase : uint8_t
	{
		Idle,
		Device,
		AddressHigh,
		AddressLow,
		Write,
		Read
	};

	MdEepromChip _chip = MdEepromChip::None;
	uint32_t _size = 0;
	uint32_t _pageMask = 0;
	uint8_t* _memory = nullptr;

	bool _scl = true;
	bool _sda = true; //driven by the console
	bool _chipSda = true; //driven by the EEPROM (open drain: low wins)
	Phase _phase = Phase::Idle;
	uint8_t _bit = 0;
	uint8_t _shift = 0;
	uint8_t _deviceBlock = 0;
	uint8_t _addressHigh = 0;
	uint16_t _address = 0;
	bool _masterAck = false;
	bool _ackCycle = false; //the EEPROM is acknowledging a received byte
	bool _dirty = false;

	bool IsX24C01() { return _chip == MdEepromChip::X24C01; }
	bool HasWordAddress() { return _chip == MdEepromChip::C24C64 || _chip == MdEepromChip::C24C65; }
	bool ReceiveByte(uint8_t value);
	void LoadReadByte();
	void OnClockRise();
	void OnClockFall();

public:
	MdEepromDevice(MdEepromChip chip);
	~MdEepromDevice();

	static uint32_t GetSize(MdEepromChip chip);

	uint8_t* GetMemory() { return _memory; }
	uint32_t GetSize() { return _size; }
	bool IsDirty() { return _dirty; }
	void ClearDirty() { _dirty = false; }

	//Updates the console-driven SCL and SDA lines
	void SetLines(bool scl, bool sda);
	//The SDA line as the console reads it
	bool ReadSda() { return _sda && _chipSda; }

	void Serialize(Serializer& s) override;
};

class MdEepromDatabase
{
public:
	static MdEepromInfo Find(const string& sha256);
	//Test hook (headless runner): use this chip/board for the next cartridge whatever its hash
	static void SetOverride(MdEepromInfo info);
	static MdEepromInfo GetOverride();
};
