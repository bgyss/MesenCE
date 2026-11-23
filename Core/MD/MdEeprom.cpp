#include "pch.h"
#include "MD/MdEeprom.h"
#include "Utilities/Serializer.h"

MdEepromDevice::MdEepromDevice(MdEepromChip chip)
{
	_chip = chip;
	_size = GetSize(chip);
	//Write page sizes from the datasheets: X24C01 4 bytes, 24C01/02 8, 24C04-16 16, 24C64/65 32
	switch(chip) {
		case MdEepromChip::X24C01: _pageMask = 3; break;
		case MdEepromChip::C24C01:
		case MdEepromChip::C24C02: _pageMask = 7; break;
		case MdEepromChip::C24C64:
		case MdEepromChip::C24C65: _pageMask = 31; break;
		default: _pageMask = 15; break;
	}
	_memory = new uint8_t[_size];
	memset(_memory, 0xFF, _size);
}

MdEepromDevice::~MdEepromDevice()
{
	delete[] _memory;
}

uint32_t MdEepromDevice::GetSize(MdEepromChip chip)
{
	switch(chip) {
		case MdEepromChip::X24C01:
		case MdEepromChip::C24C01: return 128;
		case MdEepromChip::C24C02: return 256;
		case MdEepromChip::C24C04: return 512;
		case MdEepromChip::C24C08: return 1024;
		case MdEepromChip::C24C16: return 2048;
		case MdEepromChip::C24C64:
		case MdEepromChip::C24C65: return 8192;
		default: return 0;
	}
}

void MdEepromDevice::SetLines(bool scl, bool sda)
{
	bool oldScl = _scl;
	bool oldSda = _sda;
	_scl = scl;
	_sda = sda;

	if(oldScl && scl) {
		//Data changing while the clock is high: START (falling) or STOP (rising)
		if(oldSda && !sda) {
			_phase = Phase::Device;
			_ackCycle = false;
			_bit = 0;
			_shift = 0;
			_chipSda = true;
		} else if(!oldSda && sda) {
			_phase = Phase::Idle;
			_ackCycle = false;
			_chipSda = true;
		}
	} else if(!oldScl && scl) {
		OnClockRise();
	} else if(oldScl && !scl) {
		OnClockFall();
	}
}

void MdEepromDevice::OnClockRise()
{
	if(_phase == Phase::Idle) {
		return;
	}
	_bit++;
	if(_phase == Phase::Read && !_ackCycle) {
		if(_bit == 9) {
			_masterAck = !_sda;
		}
	} else if(_bit <= 8) {
		_shift = (_shift << 1) | (_sda ? 1 : 0);
	}
}

void MdEepromDevice::OnClockFall()
{
	if(_phase == Phase::Idle) {
		return;
	}

	if(_ackCycle) {
		//End of the EEPROM's ACK clock: release SDA; a read starts driving its first bit now
		_ackCycle = false;
		_chipSda = true;
		_bit = 0;
		_shift = 0;
		if(_phase == Phase::Read) {
			LoadReadByte();
		}
		return;
	}

	if(_phase == Phase::Read) {
		if(_bit < 8) {
			_chipSda = (_shift >> (7 - _bit)) & 0x01;
		} else if(_bit == 8) {
			_chipSda = true; //released for the console's ACK
		} else {
			if(_masterAck) {
				_address = (_address + 1) & (_size - 1);
				LoadReadByte();
			} else {
				_phase = Phase::Idle;
				_chipSda = true;
			}
		}
		return;
	}

	if(_bit == 8) {
		//Byte received: acknowledge it during the 9th clock
		bool ack = ReceiveByte(_shift);
		_chipSda = !ack;
		if(ack) {
			_ackCycle = true;
		} else {
			_phase = Phase::Idle;
		}
	}
}

void MdEepromDevice::LoadReadByte()
{
	_bit = 0;
	_shift = _memory[_address & (_size - 1)];
	_chipSda = _shift & 0x80;
}

bool MdEepromDevice::ReceiveByte(uint8_t value)
{
	//A switch to Read takes effect after the ACK clock (_ackCycle)
	switch(_phase) {
		case Phase::Device:
			if(IsX24C01()) {
				//X24C01: 7-bit word address + R/W bit
				_address = (value >> 1) & 0x7F;
				_phase = (value & 0x01) ? Phase::Read : Phase::Write;
				return true;
			}
			if((value >> 4) != 0x0A) {
				return false;
			}
			_deviceBlock = (value >> 1) & 0x07;
			if(value & 0x01) {
				if(!HasWordAddress()) {
					_address = ((_deviceBlock << 8) | (_address & 0xFF)) & (_size - 1);
				}
				_phase = Phase::Read;
			} else {
				_phase = HasWordAddress() ? Phase::AddressHigh : Phase::AddressLow;
			}
			return true;

		case Phase::AddressHigh:
			_addressHigh = value;
			_phase = Phase::AddressLow;
			return true;

		case Phase::AddressLow:
			if(HasWordAddress()) {
				_address = ((_addressHigh << 8) | value) & (_size - 1);
			} else {
				_address = ((_deviceBlock << 8) | value) & (_size - 1);
			}
			_phase = Phase::Write;
			return true;

		case Phase::Write:
			_memory[_address & (_size - 1)] = value;
			_dirty = true;
			_address = (_address & ~_pageMask) | ((_address + 1) & _pageMask);
			return true;

		default:
			return false;
	}
}

void MdEepromDevice::Serialize(Serializer& s)
{
	SVArray(_memory, _size);
	SV(_scl);
	SV(_sda);
	SV(_chipSda);
	SV(_phase);
	SV(_bit);
	SV(_shift);
	SV(_deviceBlock);
	SV(_addressHigh);
	SV(_address);
	SV(_masterAck);
	SV(_ackCycle);
}

namespace
{
	struct EepromEntry
	{
		const char* Sha256;
		MdEepromChip Chip;
		MdEepromBoard Board;
	};

	//Games, chips and boards from ares' database (mia/medium/mega-drive.cpp, ISC license). ares
	//lists NBA Jam's chip as "X24C02"; MAME's table lists it as a 24C02 with the standard device +
	//address protocol, which is what is used here.
	const EepromEntry _entries[] = {
		{ "b90e9d7ecd74980c422ed72575a36d4d378302136faafbc365474e5d037b30fa", MdEepromChip::X24C01, MdEepromBoard::Ea }, //Bill Walsh College Football (USA, Europe)
		{ "b13f1e31bf6ba739e4cffd98374263b3421b3a61c4873e30dbc866198736e880", MdEepromChip::X24C01, MdEepromBoard::Sega }, //Evander Holyfield's 'Real Deal' Boxing (World)
		{ "81a30f887132792896796d0aebce628d7c092f3b2642b83bf2fc78f7c940ce34", MdEepromChip::X24C01, MdEepromBoard::Sega }, //Game Toshokan (Japan) (1.1)
		{ "c2daef4bb84f6d4e52caf9a28d9f2561642d3b7e749dc62f12a2587ee77d1222", MdEepromChip::X24C01, MdEepromBoard::Sega }, //Greatest Heavyweights (Japan)
		{ "2007029b0fb227feaa7ef2de7e189270e52c81bc291241a4b5130c78993bc7b9", MdEepromChip::X24C01, MdEepromBoard::Sega }, //Greatest Heavyweights (USA)
		{ "18920dea1147e1cc0fbafc0ee15524627c47f4211eebf14d4eff36d0b4d7e3f8", MdEepromChip::X24C01, MdEepromBoard::Sega }, //Greatest Heavyweights (Europe)
		{ "0d476b9f1c7245d408a2c1a6d22bc0f37e7fe3be9850b7d9c9fac5b18f616511", MdEepromChip::X24C01, MdEepromBoard::Sega }, //Honoo no Toukyuuji: Dodge Danpei (Japan)
		{ "8958d7d382aa99d91122564397a5aa438c26f4bab2c20626756f7802b2467624", MdEepromChip::X24C01, MdEepromBoard::Ea }, //John Madden Football '93 (USA, Europe)
		{ "cfb45fad6dada893ae2ac93eb5bbd36d2de6c5973430f4018cdcb1a53fdfdb12", MdEepromChip::X24C01, MdEepromBoard::Ea }, //John Madden Football '93: Championship Edition (USA)
		{ "98f36274e4d73feefaba0e812024a6cecda2d864a2cbed7363b9d357f2e6a582", MdEepromChip::X24C01, MdEepromBoard::Sega }, //Mega Man: The Wily Wars (Europe)
		{ "524e7e61c6096e920ca263a4ed208c002a4ef6cfc17bfda60bf9ecb63c975951", MdEepromChip::X24C01, MdEepromBoard::Ea }, //NHLPA Hockey '93 (USA, Europe) (1.0)
		{ "69a05a0f73bc6b3f5b5c7aff53bb0a1db8fab4af8eee0c84d92e503214a156a1", MdEepromChip::X24C01, MdEepromBoard::Ea }, //NHLPA Hockey '93 (USA, Europe) (1.1)
		{ "212c70be20c8a29f11c5392a06207e43c2bff07e716a4d3cbf2d046f684a3cef", MdEepromChip::X24C01, MdEepromBoard::Sega }, //Ninja Burai Densetsu (Japan)
		{ "36303fc447c433ebc69c3d4df86c783c86b383e0acead1c19595f13269e248f5", MdEepromChip::X24C01, MdEepromBoard::Ea }, //Rings of Power (USA, Europe)
		{ "390dcd13bbde5e5aab352c456c3ab7d40df60edaae65687cda4598cb1387a3d6", MdEepromChip::X24C01, MdEepromBoard::Sega }, //Rockman: Mega World (Japan) (1.1)
		{ "553eb97741c52e2c3ce668960c1f70231514f7fa3532f7cadcf370bb28ff1efe", MdEepromChip::X24C01, MdEepromBoard::Sega }, //Sports Talk Baseball (USA)
		{ "d758864efa18da7a8a87dce32a5b61a8067fb60846696a9588956bc316e480f9", MdEepromChip::X24C01, MdEepromBoard::Sega }, //Wonder Boy V - Monster World III (Japan)
		{ "5aa7e5ccb1931c688f4f0cadea5d4b41440c6c4bbaad83b6bab547ceede31c1d", MdEepromChip::C24C02, MdEepromBoard::Acclaim16 }, //NBA Jam (Japan)
		{ "9ee77c144505a10940976bb48e32600c83d5910c737612868ff6088faf3eb7f4", MdEepromChip::C24C02, MdEepromBoard::Acclaim16 }, //NBA Jam (USA, Europe) (1.0)
		{ "d90d4ca8d00b4dade6d21650da2264fa64016daa779215ac06adfc04da31d610", MdEepromChip::C24C02, MdEepromBoard::Acclaim16 }, //NBA Jam (USA, Europe) (1.1)
		{ "a24174cdbe188efabda887f4c7ad492d0cb36b74265a90b2af8c2a5b480f4e7f", MdEepromChip::C24C02, MdEepromBoard::Acclaim32 }, //NFL Quarterback Club (World)
		{ "8bdd2fa68e70f727f6c86f8266ec703457e09c2940cbd8f82c3760e2177b88fe", MdEepromChip::C24C04, MdEepromBoard::Acclaim32 }, //Blockbuster World Video Game Championship II (USA)
		{ "5b735b443102f771bc7db49dcf34de640c10d212f5cb4af1251d956751f21072", MdEepromChip::C24C04, MdEepromBoard::Acclaim32 }, //NBA Jam: Tournament Edition (World)
		//32X cartridges with the same EEPROM wiring (reached with RV = 1). NBA Jam TE: MAME's 32X software
		//list notes "same I2C type as plain MD version"; NFL Quarterback Club: the Mega Drive version's
		//chip, a judgment call (32x-verification-findings.md S5)
		{ "7f7e1059671a99405e3782cd9b4e5d4025ee4bb122a89357b95b0fcac512f74f", MdEepromChip::C24C04, MdEepromBoard::Acclaim32 }, //NBA Jam: Tournament Edition (32X) (World)
		{ "092ad8896ea7784d2feb7f17b1a348562588a361c50de6ade722a5c5cce698d5", MdEepromChip::C24C02, MdEepromBoard::Acclaim32 }, //NFL Quarterback Club (32X) (World)
		{ "d52223c45d0c29a00003a9782227b775b66640dbfc92bdb9346565c4f97b8cad", MdEepromChip::C24C08, MdEepromBoard::Codemasters }, //Brian Lara Cricket (Europe) (1.0)
		{ "1b28d7f1a8eb4c43afa06de8ea98290368b6ec00ec0c1c16de27ce33d892a051", MdEepromChip::C24C08, MdEepromBoard::Codemasters }, //Brian Lara Cricket (Europe) (1.1)
		{ "43e68f99620bb3dd0ff3b28d232ae43c5b6489716c4cde09725ab4b71704937e", MdEepromChip::C24C08, MdEepromBoard::Codemasters }, //Micro Machines: Military (Europe)
		{ "ec3b97adf166b2e4e9ef402ac79ce9d458a932996b7fd259cd55db81adb496a6", MdEepromChip::C24C16, MdEepromBoard::Codemasters }, //Micro Machines: Turbo Tournament '96 (Europe) (1.0)
		{ "77ff023aa9f88aeb1605d3060b1331b816752b12b64ff2fe95d4a9f3c5dff946", MdEepromChip::C24C16, MdEepromBoard::Codemasters }, //Micro Machines: Turbo Tournament '96 (Europe) (1.1)
		{ "57fa3662f589929d180648090104f8c802894602a6f660755121cb44918f382c", MdEepromChip::C24C16, MdEepromBoard::Codemasters }, //Micro Machines 2: Turbo Tournament (Europe) (1.0)
		{ "9209241472f0e78f23405bb265c7c108c25413d9a01ec1455f4e2d17d80c188c", MdEepromChip::C24C16, MdEepromBoard::Codemasters }, //Micro Machines 2: Turbo Tournament (Europe) (1.1)
		{ "162edaaaf77a3ccdb693c46cf63fdd273eab31ac6b64bc2f450b0c09db9ceef7", MdEepromChip::C24C16, MdEepromBoard::Acclaim32 }, //NFL Quarterback Club '96 (USA, Europe)
		{ "2de062b59934a4dc7a4bbc0a012b86a7674597be7ef04f8cb78d088cee8a9eee", MdEepromChip::C24C65, MdEepromBoard::Codemasters }, //Brian Lara Cricket '96 (Europe) (1.0)
		{ "1d36e581171c29cae09457954589916d9e0cfeee193a049f358be428fc7c6421", MdEepromChip::C24C65, MdEepromBoard::Codemasters }, //Brian Lara Cricket '96 (Europe) (1.1)
		{ "52cc1c3e7abed5bb3887fe5a5af96a763a8072faad19a1042a61a2567026d705", MdEepromChip::C24C65, MdEepromBoard::Acclaim32 }, //College Slam (USA)
		{ "8aa5b0aa8f550a42aa0dfe5c200dcf4bc344e9b114a0c056acced3374441ece0", MdEepromChip::C24C65, MdEepromBoard::Acclaim32 }, //Frank Thomas' Big Hurt Baseball (USA, Europe)
	};

	MdEepromInfo _override = {};
}

MdEepromInfo MdEepromDatabase::Find(const string& sha256)
{
	if(_override.Chip != MdEepromChip::None) {
		return _override;
	}
	for(const EepromEntry& entry : _entries) {
		if(sha256 == entry.Sha256) {
			return { entry.Chip, entry.Board };
		}
	}
	return {};
}

void MdEepromDatabase::SetOverride(MdEepromInfo info)
{
	_override = info;
}

MdEepromInfo MdEepromDatabase::GetOverride()
{
	return _override;
}
