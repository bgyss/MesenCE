// SH-2 (SH7604) interpreter. Instruction semantics, cache and the on-chip register map are
// ported from ares (ares/component/processor/sh2, ISC licence, Copyright (c) 2004-2025 ares team,
// Near et al). See Sh2Core.h and docs/research/32x-verification-findings.md for the changes.
#include "MD/Sh2/Sh2Core.h"
#include <cstdio>
#include <cstring>

namespace
{
	enum Area : uint32_t
	{
		Cached = 0,
		Uncached = 1,
		Purge = 2,
		AddressArray = 3,
		DataArray = 6,
		Io = 7,
	};

	inline int64_t ClampSigned(int64_t v, int bits)
	{
		const int64_t hi = (int64_t(1) << (bits - 1)) - 1;
		const int64_t lo = -hi - 1;
		return v > hi ? hi : (v < lo ? lo : v);
	}
}

Sh2Core::Sh2Core()
{
	InitLruTables();
}

void Sh2Core::InitLruTables()
{
	//LRU replacement as in the SH7604 hardware manual (6-bit LRU per entry), via ares
	auto bit = [](uint32_t v, int b) { return (v >> b) & 1; };
	for(uint32_t n = 0; n < 64; n++) {
		if(bit(n, 5) && bit(n, 4) && bit(n, 3)) {
			_lruSelect[n] = 0;
		} else if(!bit(n, 5) && bit(n, 2) && bit(n, 1)) {
			_lruSelect[n] = 1;
		} else if(!bit(n, 4) && !bit(n, 2) && bit(n, 0)) {
			_lruSelect[n] = 2;
		} else {
			_lruSelect[n] = 3; //includes the combinations no rule matches
		}
		_lruUpdate[0][n] = (uint8_t)(n & ~0x38);
		_lruUpdate[1][n] = (uint8_t)((n | 0x20) & ~0x06);
		_lruUpdate[2][n] = (uint8_t)((n | 0x14) & ~0x01);
		_lruUpdate[3][n] = (uint8_t)(n | 0x0B);
	}
}

void Sh2Core::PowerOn()
{
	uint64_t cycles = _state.CycleCount;
	_state = {};
	_state.CycleCount = cycles;
	_state.SR = 0xF0;
	_state.PendingReset = 1;

	_cache = {};
	for(uint32_t& tag : _cache.Tags) {
		tag = CacheState::Invalid;
	}
	_intc = {};
	_dmac = {};
	_divu = {};
	_frt = {};
	_frt.Ocra = 0xFFFF;
	_frt.Ocrb = 0xFFFF;
	_wdt = {};
	_sci = {};
	_sci.Brr = 0xFF;
	_sci.Tdr = 0xFF;
	_sci.Tdre = true;
	_sci.Tend = true;
	_bsc = {};
	_ubc = {};
	_sbycr = 0;
	_irlLevel = 0;
	_irlVector = 0;
	_nmiPending = false;
}

void Sh2Core::Reset(bool manual)
{
	if(!manual) {
		PowerOn();
		return;
	}
	//Manual reset: the CPU restarts from vectors 2/3; on-chip modules keep their state except
	//the ones the SH7604 manual lists as initialised by both resets (BSC is kept).
	_state.PendingReset = 2;
	_state.SlotPending = false;
	_state.Sleeping = false;
	_state.NoInterrupt = false;
}

bool Sh2Core::IsDmaActive(int ch) const
{
	const DmaChannel& c = _dmac.Ch[ch];
	return c.De && !c.Te && _dmac.Dme && !_dmac.Nmif && !_dmac.Ae;
}

// ---------------------------------------------------------------------------------------------
// Bus and cache
// ---------------------------------------------------------------------------------------------

template<int Size>
uint32_t Sh2Core::CacheRead(uint32_t addr, bool fetch)
{
	uint32_t entry = (addr >> 4) & 63;
	uint32_t tag = (addr >> 10) & 0x7FFFF;
	uint8_t lru = _cache.Lru[entry];
	auto read = [&](uint8_t* line) -> uint32_t {
		uint32_t o = addr & 15;
		if constexpr(Size == 1) {
			return line[o];
		} else if constexpr(Size == 2) {
			o &= ~1;
			return (uint32_t)(line[o] << 8 | line[o + 1]);
		} else {
			o &= ~3;
			return (uint32_t)line[o] << 24 | (uint32_t)line[o + 1] << 16 | (uint32_t)line[o + 2] << 8 | line[o + 3];
		}
	};

	for(int way = 3; way >= 0; way--) {
		uint32_t index = (uint32_t)way * 64 + entry;
		if(_cache.Tags[index] == tag) {
			_cache.Lru[entry] = _lruUpdate[way][lru];
			return read(_cache.Lines[index]);
		}
	}

	if((_cache.DisableCode && fetch) || (_cache.DisableData && !fetch)) {
		uint32_t phys = addr & 0x1FFFFFFF;
		if constexpr(Size == 1) {
			return BusRead8(phys);
		} else if constexpr(Size == 2) {
			return fetch ? BusFetch16(phys & ~1) : BusRead16(phys & ~1);
		} else {
			return BusRead32(phys & ~3);
		}
	}

	uint32_t way = _lruSelect[lru] | _cache.TwoWay;
	uint32_t index = way * 64 + entry;
	_cache.Lru[entry] = _lruUpdate[way][lru];
	_cache.Tags[index] = tag;
	uint8_t* line = _cache.Lines[index];
	uint32_t base = addr & 0x1FFFFFF0;
	for(uint32_t i = 0; i < 4; i++) {
		uint32_t v = BusRead32(base + i * 4);
		line[i * 4 + 0] = (uint8_t)(v >> 24);
		line[i * 4 + 1] = (uint8_t)(v >> 16);
		line[i * 4 + 2] = (uint8_t)(v >> 8);
		line[i * 4 + 3] = (uint8_t)v;
	}
	return read(line);
}

template<int Size>
void Sh2Core::CacheWrite(uint32_t addr, uint32_t value)
{
	//Write-through, no allocation: only a hit updates the line
	uint32_t entry = (addr >> 4) & 63;
	uint32_t tag = (addr >> 10) & 0x7FFFF;
	for(int way = 3; way >= 0; way--) {
		uint32_t index = (uint32_t)way * 64 + entry;
		if(_cache.Tags[index] == tag) {
			_cache.Lru[entry] = _lruUpdate[way][_cache.Lru[entry]];
			uint8_t* line = _cache.Lines[index];
			uint32_t o = addr & 15;
			if constexpr(Size == 1) {
				line[o] = (uint8_t)value;
			} else if constexpr(Size == 2) {
				o &= ~1;
				line[o] = (uint8_t)(value >> 8);
				line[o + 1] = (uint8_t)value;
			} else {
				o &= ~3;
				line[o] = (uint8_t)(value >> 24);
				line[o + 1] = (uint8_t)(value >> 16);
				line[o + 2] = (uint8_t)(value >> 8);
				line[o + 3] = (uint8_t)value;
			}
			return;
		}
	}
}

void Sh2Core::CachePurgeLine(uint32_t addr)
{
	uint32_t entry = (addr >> 4) & 63;
	uint32_t tag = (addr >> 10) & 0x7FFFF;
	for(int way = 0; way < 4; way++) {
		if(_cache.Tags[way * 64 + entry] == tag) {
			_cache.Tags[way * 64 + entry] |= CacheState::Invalid;
		}
	}
}

void Sh2Core::CachePurgeAll(int ways)
{
	memset(_cache.Lru, 0, sizeof(_cache.Lru));
	for(int i = 0; i < 64 * ways; i++) {
		_cache.Tags[i] |= CacheState::Invalid;
	}
}

uint32_t Sh2Core::ReadByte(uint32_t addr)
{
	if(_flatBus) {
		return BusRead8(addr);
	}
	switch(addr >> 29) {
		case Cached:
			if(_cache.Enable) {
				return CacheRead<1>(addr, false);
			}
			return BusRead8(addr & 0x1FFFFFFF);
		case Uncached: return BusRead8(addr & 0x1FFFFFFF);
		case DataArray: return _cache.Lines[(addr >> 4) & 255][addr & 15];
		case Io: return ReadIo(addr, 1);
	}
	return 0;
}

uint32_t Sh2Core::ReadWord(uint32_t addr)
{
	if(_flatBus) {
		return BusRead16(addr);
	}
	switch(addr >> 29) {
		case Cached:
			if(_cache.Enable) {
				return CacheRead<2>(addr, false);
			}
			return BusRead16(addr & 0x1FFFFFFE);
		case Uncached: return BusRead16(addr & 0x1FFFFFFE);
		case DataArray: {
			uint8_t* line = _cache.Lines[(addr >> 4) & 255];
			uint32_t o = addr & 14;
			return (uint32_t)(line[o] << 8 | line[o + 1]);
		}
		case Io: return ReadIo(addr & ~1, 2);
	}
	return 0;
}

uint32_t Sh2Core::ReadLong(uint32_t addr)
{
	if(_flatBus) {
		return BusRead32(addr);
	}
	switch(addr >> 29) {
		case Cached:
			if(_cache.Enable) {
				return CacheRead<4>(addr, false);
			}
			return BusRead32(addr & 0x1FFFFFFC);
		case Uncached: return BusRead32(addr & 0x1FFFFFFC);
		case AddressArray: {
			//Tag (bits 28-10), LRU (bits 9-4), valid (bit 2) of the way WaySelect
			uint32_t entry = (addr >> 4) & 63;
			uint32_t tag = _cache.Tags[_cache.WaySelect * 64 + entry];
			return (tag & 0x7FFFF) << 10 | (uint32_t)_cache.Lru[entry] << 4 | ((tag & CacheState::Invalid) ? 0 : 4);
		}
		case DataArray: {
			uint8_t* line = _cache.Lines[(addr >> 4) & 255];
			uint32_t o = addr & 12;
			return (uint32_t)line[o] << 24 | (uint32_t)line[o + 1] << 16 | (uint32_t)line[o + 2] << 8 | line[o + 3];
		}
		case Io: return ReadIo(addr & ~3, 4);
	}
	return 0;
}

uint16_t Sh2Core::FetchWord(uint32_t addr)
{
	if(_flatBus) {
		return BusFetch16(addr);
	}
	switch(addr >> 29) {
		case Cached:
			if(_cache.Enable) {
				return (uint16_t)CacheRead<2>(addr, true);
			}
			return BusFetch16(addr & 0x1FFFFFFE);
		case Uncached: return BusFetch16(addr & 0x1FFFFFFE);
		default: return (uint16_t)ReadWord(addr);
	}
}

void Sh2Core::WriteByte(uint32_t addr, uint32_t value)
{
	if(_flatBus) {
		BusWrite8(addr, (uint8_t)value);
		return;
	}
	switch(addr >> 29) {
		case Cached:
			if(_cache.Enable) {
				CacheWrite<1>(addr, value);
			}
			BusWrite8(addr & 0x1FFFFFFF, (uint8_t)value);
			break;
		case Uncached: BusWrite8(addr & 0x1FFFFFFF, (uint8_t)value); break;
		case Purge: CachePurgeLine(addr); break;
		case DataArray: _cache.Lines[(addr >> 4) & 255][addr & 15] = (uint8_t)value; break;
		case Io: WriteIo(addr, value, 1); break;
	}
}

void Sh2Core::WriteWord(uint32_t addr, uint32_t value)
{
	if(_flatBus) {
		BusWrite16(addr, (uint16_t)value);
		return;
	}
	switch(addr >> 29) {
		case Cached:
			if(_cache.Enable) {
				CacheWrite<2>(addr, value);
			}
			BusWrite16(addr & 0x1FFFFFFE, (uint16_t)value);
			break;
		case Uncached: BusWrite16(addr & 0x1FFFFFFE, (uint16_t)value); break;
		case Purge: CachePurgeLine(addr); break;
		case DataArray: {
			uint8_t* line = _cache.Lines[(addr >> 4) & 255];
			uint32_t o = addr & 14;
			line[o] = (uint8_t)(value >> 8);
			line[o + 1] = (uint8_t)value;
			break;
		}
		case Io: WriteIo(addr & ~1, value, 2); break;
	}
}

void Sh2Core::WriteLong(uint32_t addr, uint32_t value)
{
	if(_flatBus) {
		BusWrite32(addr, value);
		return;
	}
	switch(addr >> 29) {
		case Cached:
			if(_cache.Enable) {
				CacheWrite<4>(addr, value);
			}
			BusWrite32(addr & 0x1FFFFFFC, value);
			break;
		case Uncached: BusWrite32(addr & 0x1FFFFFFC, value); break;
		case Purge: CachePurgeLine(addr); break;
		case AddressArray: {
			//Tag from the address (bits 28-10), valid from address bit 2, LRU from data bits 9-4
			uint32_t entry = (addr >> 4) & 63;
			uint32_t tag = (addr >> 10) & 0x7FFFF;
			_cache.Lru[entry] = (uint8_t)((value >> 4) & 63);
			_cache.Tags[_cache.WaySelect * 64 + entry] = tag | ((addr & 4) ? 0 : CacheState::Invalid);
			break;
		}
		case DataArray: {
			uint8_t* line = _cache.Lines[(addr >> 4) & 255];
			uint32_t o = addr & 12;
			line[o] = (uint8_t)(value >> 24);
			line[o + 1] = (uint8_t)(value >> 16);
			line[o + 2] = (uint8_t)(value >> 8);
			line[o + 3] = (uint8_t)value;
			break;
		}
		case Io: WriteIo(addr & ~3, value, 4); break;
	}
}

// ---------------------------------------------------------------------------------------------
// On-chip registers ($FFFFFE00-$FFFFFFFF), byte-addressed like ares
// ---------------------------------------------------------------------------------------------

void Sh2Core::DivuWait(uint32_t addr)
{
	//Any access to the DIVU while it divides waits for the end of the operation (SH7604
	//hardware manual 10.4.1)
	if(addr >= 0xFFFFFF00 && addr < 0xFFFFFF40 && _state.CycleCount < _divu.BusyUntil) {
		AddCycles((uint32_t)(_divu.BusyUntil - _state.CycleCount));
	}
}

uint32_t Sh2Core::ReadIo(uint32_t addr, int size)
{
	DivuWait(addr);
	uint32_t v = 0;
	for(int i = 0; i < size; i++) {
		v = v << 8 | ReadIo8(addr + i);
	}
	return v;
}

void Sh2Core::WriteIo(uint32_t addr, uint32_t value, int size)
{
	DivuWait(addr);
	for(int i = 0; i < size; i++) {
		WriteIo8(addr + i, (uint8_t)(value >> ((size - 1 - i) * 8)));
	}
}

uint8_t Sh2Core::ReadIo8(uint32_t addr)
{
	auto b = [](uint32_t v, int byte) { return (uint8_t)(v >> (byte * 8)); };
	auto dmaChcr = [&](int ch, bool low) -> uint8_t {
		const DmaChannel& c = _dmac.Ch[ch];
		if(!low) {
			return (uint8_t)(c.Dm << 6 | c.Sm << 4 | c.Ts << 2 | c.Ar << 1 | c.Am);
		}
		return (uint8_t)(c.Al << 7 | c.Ds << 6 | c.Dl << 5 | c.Tb << 4 | c.Ta << 3 | c.Ie << 2 | c.Te << 1 | c.De);
	};

	switch(addr) {
		case 0xFFFFFE00: return _sci.Smr;
		case 0xFFFFFE01: return _sci.Brr;
		case 0xFFFFFE02: return _sci.Scr;
		case 0xFFFFFE03: return _sci.Tdr;
		case 0xFFFFFE04:
			return (uint8_t)(_sci.Tdre << 7 | _sci.Rdrf << 6 | _sci.Orer << 5 | _sci.Fer << 4 | _sci.Per << 3 | _sci.Tend << 2 | _sci.Mpb << 1 | _sci.Mpbt);
		case 0xFFFFFE05: return _sci.Rdr;

		case 0xFFFFFE10: return (uint8_t)(_frt.Icie << 7 | _frt.Ociae << 3 | _frt.Ocibe << 2 | _frt.Ovie << 1 | 1);
		case 0xFFFFFE11: return (uint8_t)(_frt.Icf << 7 | _frt.Ocfa << 3 | _frt.Ocfb << 2 | _frt.Ovf << 1 | _frt.Cclra);
		//FRC, OCRA/B, FICR: reading the upper byte latches the lower one in TEMP (SH7604 11.3)
		case 0xFFFFFE12:
			_frt.Temp = b(_frt.Frc, 0);
			return b(_frt.Frc, 1);
		case 0xFFFFFE14: {
			uint16_t ocr = _frt.Ocrs ? _frt.Ocrb : _frt.Ocra;
			_frt.Temp = b(ocr, 0);
			return b(ocr, 1);
		}
		case 0xFFFFFE18:
			_frt.Temp = b(_frt.Ficr, 0);
			return b(_frt.Ficr, 1);
		case 0xFFFFFE13:
		case 0xFFFFFE15:
		case 0xFFFFFE19:
			return _frt.Temp;
		case 0xFFFFFE16: return (uint8_t)(_frt.Iedg << 7 | _frt.Cks);
		case 0xFFFFFE17: return (uint8_t)(0xE0 | _frt.Ocrs << 4 | _frt.Olvla << 1 | _frt.Olvlb);

		case 0xFFFFFE60: return (uint8_t)(_intc.SciIp << 4 | _intc.FrtIp);
		case 0xFFFFFE62: return _intc.SciErrVec;
		case 0xFFFFFE63: return _intc.SciRxVec;
		case 0xFFFFFE64: return _intc.SciTxVec;
		case 0xFFFFFE65: return _intc.SciTxEndVec;
		case 0xFFFFFE66: return _intc.FrtIcVec;
		case 0xFFFFFE67: return _intc.FrtOcVec;
		case 0xFFFFFE68: return _intc.FrtOvVec;
		case 0xFFFFFE71: return _dmac.Ch[0].Drcr;
		case 0xFFFFFE72: return _dmac.Ch[1].Drcr;

		case 0xFFFFFE80: return (uint8_t)(_wdt.Ovf << 7 | _wdt.Wtit << 6 | _wdt.Tme << 5 | 0x18 | _wdt.Cks);
		case 0xFFFFFE81: return _wdt.Wtcnt;
		case 0xFFFFFE83: return (uint8_t)(_wdt.Wovf << 7 | _wdt.Rste << 6 | _wdt.Rsts << 5 | 0x1F);

		case 0xFFFFFE91: return _sbycr;
		case 0xFFFFFE92:
		case 0xFFFFFE93:
			//CP (bit 4) always reads 0
			return (uint8_t)(_cache.WaySelect << 6 | (_cache.TwoWay ? 8 : 0) | _cache.DisableData << 2 | _cache.DisableCode << 1 | _cache.Enable);

		case 0xFFFFFEE0: return (uint8_t)(_intc.Nmil << 7 | _intc.Nmie);
		case 0xFFFFFEE1: return _intc.Vecmd;
		case 0xFFFFFEE2: return (uint8_t)(_intc.DivuIp << 4 | _intc.DmacIp);
		case 0xFFFFFEE3: return (uint8_t)(_intc.WdtIp << 4);
		case 0xFFFFFEE4: return _intc.WdtItVec;
		case 0xFFFFFEE5: return _intc.BscCmVec;

		case 0xFFFFFF00:
		case 0xFFFFFF01:
		case 0xFFFFFF02:
		case 0xFFFFFF03:
			return b(_divu.Dvsr, 3 - (addr & 3));
		case 0xFFFFFF04:
		case 0xFFFFFF05:
		case 0xFFFFFF06:
		case 0xFFFFFF07:
			return b(_divu.Dvdntl, 3 - (addr & 3));
		case 0xFFFFFF0B: return (uint8_t)(_divu.Ovfie << 1 | _divu.Ovf);
		case 0xFFFFFF0F: return _divu.Vector;
		case 0xFFFFFF10:
		case 0xFFFFFF11:
		case 0xFFFFFF12:
		case 0xFFFFFF13:
		case 0xFFFFFF18:
		case 0xFFFFFF19:
		case 0xFFFFFF1A:
		case 0xFFFFFF1B:
			return b(_divu.Dvdnth, 3 - (addr & 3));
		case 0xFFFFFF14:
		case 0xFFFFFF15:
		case 0xFFFFFF16:
		case 0xFFFFFF17:
		case 0xFFFFFF1C:
		case 0xFFFFFF1D:
		case 0xFFFFFF1E:
		case 0xFFFFFF1F:
			return b(_divu.Dvdntl, 3 - (addr & 3));

		case 0xFFFFFF40:
		case 0xFFFFFF41:
		case 0xFFFFFF42:
		case 0xFFFFFF43: return b(_ubc.Bara, 3 - (addr & 3));
		case 0xFFFFFF44:
		case 0xFFFFFF45:
		case 0xFFFFFF46:
		case 0xFFFFFF47: return b(_ubc.Bamra, 3 - (addr & 3));
		case 0xFFFFFF48: return b(_ubc.Bbra, 1);
		case 0xFFFFFF49: return b(_ubc.Bbra, 0);
		case 0xFFFFFF60:
		case 0xFFFFFF61:
		case 0xFFFFFF62:
		case 0xFFFFFF63: return b(_ubc.Barb, 3 - (addr & 3));
		case 0xFFFFFF64:
		case 0xFFFFFF65:
		case 0xFFFFFF66:
		case 0xFFFFFF67: return b(_ubc.Bamrb, 3 - (addr & 3));
		case 0xFFFFFF68: return b(_ubc.Bbrb, 1);
		case 0xFFFFFF69: return b(_ubc.Bbrb, 0);
		case 0xFFFFFF70:
		case 0xFFFFFF71:
		case 0xFFFFFF72:
		case 0xFFFFFF73: return b(_ubc.Bdrb, 3 - (addr & 3));
		case 0xFFFFFF74:
		case 0xFFFFFF75:
		case 0xFFFFFF76:
		case 0xFFFFFF77: return b(_ubc.Bdmrb, 3 - (addr & 3));
		case 0xFFFFFF78: return b(_ubc.Brcr, 1);
		case 0xFFFFFF79: return b(_ubc.Brcr, 0);

		case 0xFFFFFF80:
		case 0xFFFFFF81:
		case 0xFFFFFF82:
		case 0xFFFFFF83: return b(_dmac.Ch[0].Sar, 3 - (addr & 3));
		case 0xFFFFFF84:
		case 0xFFFFFF85:
		case 0xFFFFFF86:
		case 0xFFFFFF87: return b(_dmac.Ch[0].Dar, 3 - (addr & 3));
		case 0xFFFFFF89:
		case 0xFFFFFF8A:
		case 0xFFFFFF8B: return b(_dmac.Ch[0].Tcr, 3 - (addr & 3));
		case 0xFFFFFF8E: return dmaChcr(0, false);
		case 0xFFFFFF8F: return dmaChcr(0, true);
		case 0xFFFFFF90:
		case 0xFFFFFF91:
		case 0xFFFFFF92:
		case 0xFFFFFF93: return b(_dmac.Ch[1].Sar, 3 - (addr & 3));
		case 0xFFFFFF94:
		case 0xFFFFFF95:
		case 0xFFFFFF96:
		case 0xFFFFFF97: return b(_dmac.Ch[1].Dar, 3 - (addr & 3));
		case 0xFFFFFF99:
		case 0xFFFFFF9A:
		case 0xFFFFFF9B: return b(_dmac.Ch[1].Tcr, 3 - (addr & 3));
		case 0xFFFFFF9E: return dmaChcr(1, false);
		case 0xFFFFFF9F: return dmaChcr(1, true);
		case 0xFFFFFFA3: return _dmac.Ch[0].Vector;
		case 0xFFFFFFAB: return _dmac.Ch[1].Vector;
		case 0xFFFFFFB3: return (uint8_t)(_dmac.Pr << 3 | _dmac.Ae << 2 | _dmac.Nmif << 1 | _dmac.Dme);

		case 0xFFFFFFE2: return b(_bsc.Bcr1, 1);
		case 0xFFFFFFE3: return b(_bsc.Bcr1, 0);
		case 0xFFFFFFE6: return b(_bsc.Bcr2, 1);
		case 0xFFFFFFE7: return b(_bsc.Bcr2, 0);
		case 0xFFFFFFEA: return b(_bsc.Wcr, 1);
		case 0xFFFFFFEB: return b(_bsc.Wcr, 0);
		case 0xFFFFFFEE: return b(_bsc.Mcr, 1);
		case 0xFFFFFFEF: return b(_bsc.Mcr, 0);
		case 0xFFFFFFF3: return b(_bsc.Rtcsr, 0);
		case 0xFFFFFFF7: return b(_bsc.Rtcnt, 0);
		case 0xFFFFFFFB: return b(_bsc.Rtcor, 0);
	}
	return 0;
}

void Sh2Core::WriteIo8(uint32_t addr, uint8_t v)
{
	auto setByte = [](uint32_t& reg, int byte, uint8_t value) {
		reg = (reg & ~(0xFFu << (byte * 8))) | ((uint32_t)value << (byte * 8));
	};
	auto setByte16 = [](uint16_t& reg, int byte, uint8_t value) {
		reg = (uint16_t)((reg & ~(0xFF << (byte * 8))) | (value << (byte * 8)));
	};
	auto bit = [v](int n) { return ((v >> n) & 1) != 0; };

	switch(addr) {
		case 0xFFFFFE00: _sci.Smr = v; return;
		case 0xFFFFFE01: _sci.Brr = v; return;
		case 0xFFFFFE02: {
			bool te = _sci.Scr & 0x20;
			_sci.Scr = v;
			if(te && !(v & 0x20)) {
				_sci.Tdre = true;
			}
			return;
		}
		case 0xFFFFFE03: _sci.Tdr = v; return;
		case 0xFFFFFE04: {
			//Flags are cleared by writing 0 after reading 1; MPB and TEND are read-only
			bool tdre = _sci.Tdre;
			_sci.Mpbt = bit(0);
			_sci.Per &= bit(3);
			_sci.Fer &= bit(4);
			_sci.Orer &= bit(5);
			_sci.Rdrf &= bit(6);
			_sci.Tdre &= bit(7);
			if(tdre && !_sci.Tdre) {
				SciRun();
			}
			return;
		}
		case 0xFFFFFE05: _sci.Rdr = v; return;

		case 0xFFFFFE10:
			_frt.Ovie = bit(1);
			_frt.Ocibe = bit(2);
			_frt.Ociae = bit(3);
			_frt.Icie = bit(7);
			return;
		case 0xFFFFFE11:
			_frt.Cclra = bit(0);
			_frt.Ovf &= bit(1);
			_frt.Ocfb &= bit(2);
			_frt.Ocfa &= bit(3);
			_frt.Icf &= bit(7);
			return;
		//Writing the upper byte stores it in TEMP; the lower-byte write stores both (SH7604 11.3)
		case 0xFFFFFE12:
		case 0xFFFFFE14: _frt.Temp = v; return;
		case 0xFFFFFE13: _frt.Frc = (uint16_t)(_frt.Temp << 8 | v); return;
		case 0xFFFFFE15: (_frt.Ocrs ? _frt.Ocrb : _frt.Ocra) = (uint16_t)(_frt.Temp << 8 | v); return;
		case 0xFFFFFE16:
			//IEDG is bit 7 (ares writes it from bit 2; findings S1)
			_frt.Cks = v & 3;
			_frt.Iedg = bit(7);
			return;
		case 0xFFFFFE17:
			_frt.Olvlb = bit(0);
			_frt.Olvla = bit(1);
			_frt.Ocrs = bit(4);
			return;

		case 0xFFFFFE60:
			_intc.FrtIp = v & 15;
			_intc.SciIp = v >> 4;
			return;
		case 0xFFFFFE62: _intc.SciErrVec = v & 0x7F; return;
		case 0xFFFFFE63: _intc.SciRxVec = v & 0x7F; return;
		case 0xFFFFFE64: _intc.SciTxVec = v & 0x7F; return;
		case 0xFFFFFE65: _intc.SciTxEndVec = v & 0x7F; return;
		case 0xFFFFFE66: _intc.FrtIcVec = v & 0x7F; return;
		case 0xFFFFFE67: _intc.FrtOcVec = v & 0x7F; return;
		case 0xFFFFFE68: _intc.FrtOvVec = v & 0x7F; return;
		case 0xFFFFFE71: _dmac.Ch[0].Drcr = v & 3; return;
		case 0xFFFFFE72: _dmac.Ch[1].Drcr = v & 3; return;

		//WTCSR/WTCNT: written as a word, upper byte $A5 (WTCSR) or $5A (WTCNT); ares keeps the
		//upper byte as a selector
		case 0xFFFFFE80: _wdt.Prescaler = (_wdt.Prescaler & 0x00FFFFFF) | (uint32_t)v << 24; return;
		case 0xFFFFFE81: {
			uint8_t select = (uint8_t)(_wdt.Prescaler >> 24);
			if(select == 0xA5) {
				_wdt.Cks = v & 7;
				_wdt.Tme = bit(5);
				_wdt.Wtit = bit(6);
				_wdt.Ovf &= bit(7);
				if(!_wdt.Tme) {
					_wdt.Wtcnt = 0;
				}
			} else if(select == 0x5A) {
				_wdt.Wtcnt = v;
			}
			return;
		}
		case 0xFFFFFE83:
			_wdt.Rsts = bit(5);
			_wdt.Rste = bit(6);
			_wdt.Wovf = bit(7);
			return;

		case 0xFFFFFE91: _sbycr = v & 0xDF; return;
		case 0xFFFFFE92:
		case 0xFFFFFE93:
			_cache.Enable = bit(0);
			_cache.DisableCode = bit(1);
			_cache.DisableData = bit(2);
			_cache.TwoWay = bit(3) ? 2 : 0;
			_cache.WaySelect = v >> 6;
			//TW: software must invalidate ways 0-1 itself (SH7604 8.4.5); ares purges them here
			if(bit(4)) {
				CachePurgeAll(4);
			}
			return;

		case 0xFFFFFEE0: _intc.Nmie = bit(0); return;
		case 0xFFFFFEE1: _intc.Vecmd = bit(0); return;
		case 0xFFFFFEE2:
			_intc.DmacIp = v & 15;
			_intc.DivuIp = v >> 4;
			return;
		case 0xFFFFFEE3: _intc.WdtIp = v >> 4; return;
		case 0xFFFFFEE4: _intc.WdtItVec = v & 0x7F; return;
		case 0xFFFFFEE5: _intc.BscCmVec = v & 0x7F; return;

		case 0xFFFFFF00:
		case 0xFFFFFF01:
		case 0xFFFFFF02:
		case 0xFFFFFF03:
			setByte(_divu.Dvsr, 3 - (addr & 3), v);
			return;
		case 0xFFFFFF04:
		case 0xFFFFFF05:
		case 0xFFFFFF06:
		case 0xFFFFFF07:
			setByte(_divu.Dvdntl, 3 - (addr & 3), v);
			if(addr == 0xFFFFFF07) {
				DivuStart32();
			}
			return;
		case 0xFFFFFF0B:
			_divu.Ovf = bit(0);
			_divu.Ovfie = bit(1);
			return;
		case 0xFFFFFF0F: _divu.Vector = v & 0x7F; return;
		case 0xFFFFFF10:
		case 0xFFFFFF11:
		case 0xFFFFFF12:
		case 0xFFFFFF13:
		case 0xFFFFFF18:
		case 0xFFFFFF19:
		case 0xFFFFFF1A:
		case 0xFFFFFF1B:
			setByte(_divu.Dvdnth, 3 - (addr & 3), v);
			return;
		case 0xFFFFFF14:
		case 0xFFFFFF15:
		case 0xFFFFFF16:
		case 0xFFFFFF17:
		case 0xFFFFFF1C:
		case 0xFFFFFF1D:
		case 0xFFFFFF1E:
		case 0xFFFFFF1F:
			setByte(_divu.Dvdntl, 3 - (addr & 3), v);
			if((addr & 3) == 3) {
				DivuStart64();
			}
			return;

		case 0xFFFFFF40:
		case 0xFFFFFF41:
		case 0xFFFFFF42:
		case 0xFFFFFF43: setByte(_ubc.Bara, 3 - (addr & 3), v); return;
		case 0xFFFFFF44:
		case 0xFFFFFF45:
		case 0xFFFFFF46:
		case 0xFFFFFF47: setByte(_ubc.Bamra, 3 - (addr & 3), v); return;
		case 0xFFFFFF49: setByte16(_ubc.Bbra, 0, v); return;
		case 0xFFFFFF60:
		case 0xFFFFFF61:
		case 0xFFFFFF62:
		case 0xFFFFFF63: setByte(_ubc.Barb, 3 - (addr & 3), v); return;
		case 0xFFFFFF64:
		case 0xFFFFFF65:
		case 0xFFFFFF66:
		case 0xFFFFFF67: setByte(_ubc.Bamrb, 3 - (addr & 3), v); return;
		case 0xFFFFFF69: setByte16(_ubc.Bbrb, 0, v); return;
		case 0xFFFFFF70:
		case 0xFFFFFF71:
		case 0xFFFFFF72:
		case 0xFFFFFF73: setByte(_ubc.Bdrb, 3 - (addr & 3), v); return;
		case 0xFFFFFF74:
		case 0xFFFFFF75:
		case 0xFFFFFF76:
		case 0xFFFFFF77: setByte(_ubc.Bdmrb, 3 - (addr & 3), v); return;
		case 0xFFFFFF78: setByte16(_ubc.Brcr, 1, v); return;
		case 0xFFFFFF79: setByte16(_ubc.Brcr, 0, v); return;

		case 0xFFFFFF80:
		case 0xFFFFFF81:
		case 0xFFFFFF82:
		case 0xFFFFFF83: setByte(_dmac.Ch[0].Sar, 3 - (addr & 3), v); return;
		case 0xFFFFFF84:
		case 0xFFFFFF85:
		case 0xFFFFFF86:
		case 0xFFFFFF87: setByte(_dmac.Ch[0].Dar, 3 - (addr & 3), v); return;
		case 0xFFFFFF89:
		case 0xFFFFFF8A:
		case 0xFFFFFF8B: setByte(_dmac.Ch[0].Tcr, 3 - (addr & 3), v); return;
		case 0xFFFFFF90:
		case 0xFFFFFF91:
		case 0xFFFFFF92:
		case 0xFFFFFF93: setByte(_dmac.Ch[1].Sar, 3 - (addr & 3), v); return;
		case 0xFFFFFF94:
		case 0xFFFFFF95:
		case 0xFFFFFF96:
		case 0xFFFFFF97: setByte(_dmac.Ch[1].Dar, 3 - (addr & 3), v); return;
		case 0xFFFFFF99:
		case 0xFFFFFF9A:
		case 0xFFFFFF9B: setByte(_dmac.Ch[1].Tcr, 3 - (addr & 3), v); return;
		case 0xFFFFFF8E:
		case 0xFFFFFF9E: {
			DmaChannel& c = _dmac.Ch[addr == 0xFFFFFF9E];
			c.Am = bit(0);
			c.Ar = bit(1);
			c.Ts = (v >> 2) & 3;
			c.Sm = (v >> 4) & 3;
			c.Dm = (v >> 6) & 3;
			return;
		}
		case 0xFFFFFF8F:
		case 0xFFFFFF9F: {
			DmaChannel& c = _dmac.Ch[addr == 0xFFFFFF9F];
			c.De = bit(0);
			c.Te &= bit(1);
			c.Ie = bit(2);
			c.Ta = bit(3);
			c.Tb = bit(4);
			c.Dl = bit(5);
			c.Ds = bit(6);
			c.Al = bit(7);
			return;
		}
		case 0xFFFFFFA3: _dmac.Ch[0].Vector = v; return;
		case 0xFFFFFFAB: _dmac.Ch[1].Vector = v; return;
		case 0xFFFFFFB3:
			_dmac.Dme = bit(0);
			_dmac.Nmif &= bit(1);
			_dmac.Ae &= bit(2);
			_dmac.Pr = bit(3);
			return;

		case 0xFFFFFFE2: setByte16(_bsc.Bcr1, 1, v); return;
		case 0xFFFFFFE3: setByte16(_bsc.Bcr1, 0, v); return;
		case 0xFFFFFFE6: setByte16(_bsc.Bcr2, 1, v); return;
		case 0xFFFFFFE7: setByte16(_bsc.Bcr2, 0, v); return;
		case 0xFFFFFFEA: setByte16(_bsc.Wcr, 1, v); return;
		case 0xFFFFFFEB: setByte16(_bsc.Wcr, 0, v); return;
		case 0xFFFFFFEE: setByte16(_bsc.Mcr, 1, v); return;
		case 0xFFFFFFEF: setByte16(_bsc.Mcr, 0, v); return;
		case 0xFFFFFFF3: _bsc.Rtcsr = v; return;
		case 0xFFFFFFF7: _bsc.Rtcnt = v; return;
		case 0xFFFFFFFB: _bsc.Rtcor = v; return;
	}
}

void Sh2Core::DivuStart32()
{
	//Writing DVDNT also sets DVDNTL and sign-extends into DVDNTH (SH7604 10.2.2): a 32/32
	//division is the 64/32 one on the sign-extended dividend
	_divu.Dvdnth = (int32_t)_divu.Dvdntl < 0 ? 0xFFFFFFFF : 0;
	DivuStart64();
}

void Sh2Core::DivuStart64()
{
	//64/32 signed division of DVDNTH:DVDNTL: 39 cycles, or 6 on overflow (SH7604 10.3).
	//Overflow: the quotient doesn't fit in signed 32 bits, or the divisor is 0. DVDNTL then
	//holds $7FFFFFFF (positive overflow) or $80000000 (negative); DVDNTH and, with OVFIE set,
	//DVDNTL hold the unit's intermediate result after 6 cycles, which isn't modelled (the
	//remainder / saturated value is used instead; findings S1).
	int64_t dividend = (int64_t)((uint64_t)_divu.Dvdnth << 32 | _divu.Dvdntl);
	int64_t divisor = (int32_t)_divu.Dvsr;
	bool overflow = false;
	bool negative = false;
	if(divisor == 0) {
		overflow = true;
		negative = dividend < 0;
	} else if(dividend == INT64_MIN && divisor == -1) {
		overflow = true;
	} else {
		int64_t quotient = dividend / divisor;
		_divu.Dvdnth = (uint32_t)(dividend % divisor);
		if(quotient > INT32_MAX || quotient < INT32_MIN) {
			overflow = true;
			negative = quotient < 0;
		} else {
			_divu.Dvdntl = (uint32_t)quotient;
		}
	}
	if(overflow) {
		_divu.Dvdntl = negative ? 0x80000000 : 0x7FFFFFFF;
		_divu.Ovf = true;
	}
	_divu.BusyUntil = _state.CycleCount + (overflow ? 6 : 39);
}

// ---------------------------------------------------------------------------------------------
// Peripherals: FRT, WDT, DMAC, SCI
// ---------------------------------------------------------------------------------------------

void Sh2Core::RunPeripherals(uint32_t cycles)
{
	//FRT: internal clock / 8, / 32, / 128; external clock (CKS = 3) counts every cycle (ares)
	static constexpr uint8_t frtShift[4] = { 3, 5, 7, 0 };
	uint32_t shift = frtShift[_frt.Cks];
	_frt.Prescaler += cycles;
	uint32_t incs = _frt.Prescaler >> shift;
	_frt.Prescaler &= (1u << shift) - 1;
	while(incs--) {
		_frt.Frc++;
		if(_frt.Frc == 0) {
			_frt.Ovf = true;
		}
		if(_frt.Frc == _frt.Ocrb) {
			_frt.Ocfb = true;
		}
		if(_frt.Frc == _frt.Ocra) {
			_frt.Ocfa = true;
			if(_frt.Cclra) {
				_frt.Frc = 0;
			}
		}
	}

	if(_wdt.Tme) {
		static constexpr uint8_t wdtShift[8] = { 1, 6, 7, 8, 9, 10, 12, 13 };
		uint32_t wshift = wdtShift[_wdt.Cks];
		uint32_t pre = (_wdt.Prescaler & 0xFFFFFF) + cycles;
		uint32_t winc = pre >> wshift;
		pre &= (1u << wshift) - 1;
		_wdt.Prescaler = (_wdt.Prescaler & 0xFF000000) | pre;
		if(winc) {
			uint32_t sum = _wdt.Wtcnt + winc;
			_wdt.Wtcnt = (uint8_t)sum;
			if(sum > 0xFF) {
				if(_wdt.Wtit) {
					_wdt.Wovf = true; //watchdog mode: the reset itself is not emulated (findings S1)
				} else {
					_wdt.Ovf = true;
				}
			}
		}
	}
}

void Sh2Core::RunDma()
{
	//One transfer unit per CPU step (burst mode and bus timing are not modelled). Priority only
	//decides between simultaneous requests: channel 0 first (PR = 0), or round-robin, where the
	//channel that just transferred drops to low priority (PR = 1) (SH7604 9.3.3)
	int first = _dmac.Pr ? _dmac.RoundRobinFirst : 0;
	for(int i = 0; i < 2; i++) {
		int ch = first ^ i;
		if(!IsDmaActive(ch)) {
			continue;
		}
		const DmaChannel& c = _dmac.Ch[ch];
		bool request = c.Ar || (c.Drcr == 0 && c.Dreq);
		if(request) {
			DmaTransfer(ch);
			_dmac.RoundRobinFirst = (uint8_t)(ch ^ 1);
			return;
		}
	}
}

void Sh2Core::DmaTransfer(int ch)
{
	DmaChannel& c = _dmac.Ch[ch];
	//DMA accesses bypass the cache: area 0 is treated as cache-through
	auto rd = [&](uint32_t a, int size) -> uint32_t {
		if((a >> 29) == Cached) {
			a |= 0x20000000;
		}
		return size == 1 ? ReadByte(a) : (size == 2 ? ReadWord(a) : ReadLong(a));
	};
	auto wr = [&](uint32_t a, uint32_t v, int size) {
		if((a >> 29) == Cached) {
			a |= 0x20000000;
		}
		if(size == 1) {
			WriteByte(a, v);
		} else if(size == 2) {
			WriteWord(a, v);
		} else {
			WriteLong(a, v);
		}
	};

	switch(c.Ts) {
		case 0: wr(c.Dar, rd(c.Sar, 1), 1); break;
		case 1: wr(c.Dar, rd(c.Sar, 2), 2); break;
		case 2: wr(c.Dar, rd(c.Sar, 4), 4); break;
		case 3:
			//16-byte unit: the source always increments, the count drops by 4 (ares)
			for(uint32_t i = 0; i < 16; i += 4) {
				wr(c.Dar + i, rd(c.Sar + i, 4), 4);
			}
			c.Sar += 16;
			c.Tcr = (c.Tcr - 3) & 0xFFFFFF;
			break;
	}

	static constexpr uint32_t srcStep[4] = { 1, 2, 4, 0 };
	static constexpr uint32_t dstStep[4] = { 1, 2, 4, 16 };
	if(c.Sm == 1) {
		c.Sar += srcStep[c.Ts];
	} else if(c.Sm == 2) {
		c.Sar -= srcStep[c.Ts];
	}
	if(c.Dm == 1) {
		c.Dar += dstStep[c.Ts];
	} else if(c.Dm == 2) {
		c.Dar -= dstStep[c.Ts];
	}

	c.Tcr = (c.Tcr - 1) & 0xFFFFFF;
	if(c.Tcr == 0) {
		c.Te = true;
	}
}

void Sh2Core::SciRun()
{
	//Instant transfer to the linked SH-2's receiver (ares; the 32X connects the two SCIs)
	if(!_sciLink || !(_sciLink->_sci.Scr & 0x10) || _sci.Tdre) {
		return;
	}
	_sciLink->_sci.Rdr = _sci.Tdr;
	_sciLink->_sci.Rdrf = true;
	_sci.Tdre = true;
}

// ---------------------------------------------------------------------------------------------
// Exceptions and interrupts
// ---------------------------------------------------------------------------------------------

void Sh2Core::Push(uint32_t value)
{
	_state.R[15] -= 4;
	WriteLong(_state.R[15], value);
}

void Sh2Core::EnterException(uint32_t vector, uint32_t returnPc)
{
	Push(_state.SR);
	Push(returnPc);
	_state.PC = ReadLong(_state.VBR + vector * 4);
	_state.SlotPending = false;
	_exceptionTaken = true;
}

void Sh2Core::IllegalInstruction()
{
	if(_inDelaySlot) {
		IllegalSlotInstruction();
		return;
	}
	//General illegal instruction: the saved PC is the illegal instruction's address
	EnterException(4, _instPc);
}

void Sh2Core::IllegalSlotInstruction()
{
	//Saved PC: the jump destination of the delayed branch (SH7604 hardware manual 4.5.3 and
	//table 4.11)
	EnterException(6, _curSlotTarget);
}

bool Sh2Core::CheckInterrupts()
{
	//Highest level wins; equal levels follow the SH7604 default order:
	//NMI > IRL > DIVU > DMAC0 > DMAC1 > WDT > SCI (ERI, RXI, TXI, TEI) > FRT (ICI, OCI, OVI)
	uint32_t mask = (_state.SR >> 4) & 15;
	if(_nmiPending) {
		_nmiPending = false;
		_state.Sleeping = false;
		EnterException(11, _state.PC);
		_state.SR = (_state.SR & ~0xF0) | 0xF0;
		return true;
	}

	uint32_t bestLevel = mask;
	uint32_t bestVector = 0;
	bool external = false;
	auto offer = [&](uint32_t level, uint32_t vector, bool ext = false) {
		if(level > bestLevel) {
			bestLevel = level;
			bestVector = vector;
			external = ext;
		}
	};

	offer(_irlLevel, _irlVector, true);
	if(_divu.Ovf && _divu.Ovfie) {
		offer(_intc.DivuIp, _divu.Vector);
	}
	for(int ch = 0; ch < 2; ch++) {
		if(_dmac.Ch[ch].Te && _dmac.Ch[ch].Ie) {
			offer(_intc.DmacIp, _dmac.Ch[ch].Vector & 0x7F);
		}
	}
	if(_wdt.Ovf && !_wdt.Wtit) {
		offer(_intc.WdtIp, _intc.WdtItVec);
	}
	if(_intc.SciIp > bestLevel) {
		bool rie = _sci.Scr & 0x40;
		if((_sci.Orer || _sci.Fer || _sci.Per) && rie) {
			offer(_intc.SciIp, _intc.SciErrVec);
		} else if(_sci.Rdrf && rie) {
			offer(_intc.SciIp, _intc.SciRxVec);
		} else if(_sci.Tdre && (_sci.Scr & 0x80)) {
			offer(_intc.SciIp, _intc.SciTxVec);
		} else if(_sci.Tend && (_sci.Scr & 0x04)) {
			offer(_intc.SciIp, _intc.SciTxEndVec);
		}
	}
	if(_intc.FrtIp > bestLevel) {
		if(_frt.Icf && _frt.Icie) {
			offer(_intc.FrtIp, _intc.FrtIcVec);
		} else if((_frt.Ocfa && _frt.Ociae) || (_frt.Ocfb && _frt.Ocibe)) {
			offer(_intc.FrtIp, _intc.FrtOcVec);
		} else if(_frt.Ovf && _frt.Ovie) {
			offer(_intc.FrtIp, _intc.FrtOvVec);
		}
	}

	if(bestLevel <= mask) {
		return false;
	}

	_state.Sleeping = false;
	EnterException(bestVector, _state.PC);
	_state.SR = (_state.SR & ~0xF0) | (bestLevel << 4);
	if(external) {
		OnExternalInterruptAccepted((uint8_t)bestLevel);
	}
	return true;
}

// ---------------------------------------------------------------------------------------------
// Execution
// ---------------------------------------------------------------------------------------------

void Sh2Core::Step()
{
	uint64_t start = _state.CycleCount;
	_exceptionTaken = false;

	if(_state.PendingReset) {
		uint32_t v = _state.PendingReset == 1 ? 0 : 8;
		_state.PendingReset = 0;
		_state.VBR = 0;
		_state.SR = (_state.SR & SrMask) | 0xF0;
		_state.SlotPending = false;
		_state.Sleeping = false;
		_state.PC = ReadLong(v);
		_state.R[15] = ReadLong(v + 4);
		AddCycles(1);
		RunPeripherals((uint32_t)(_state.CycleCount - start));
		return;
	}

	//Interrupts are not accepted in a delay slot or right after LDC/STC/LDS/STS
	if(!_state.SlotPending && !_state.NoInterrupt) {
		if(CheckInterrupts()) {
			AddCycles(1);
			RunPeripherals((uint32_t)(_state.CycleCount - start));
			return;
		}
	}
	_state.NoInterrupt = false;

	if(_state.Sleeping) {
		AddCycles(1);
		if(_dmac.Dme) {
			RunDma();
		}
		RunPeripherals((uint32_t)(_state.CycleCount - start));
		return;
	}

	_inDelaySlot = _state.SlotPending;
	uint32_t target = _state.SlotTarget;
	_curSlotTarget = target;
	_state.SlotPending = false;

	_instPc = _state.PC;
	uint16_t op = FetchWord(_instPc);
	OnInstructionStart(_instPc, op);
	_state.PC = _instPc + 2;
	AddCycles(1);
	Execute(op);

	if(_inDelaySlot && !_exceptionTaken) {
		_state.PC = target;
	}
	_inDelaySlot = false;

	if(_dmac.Dme) {
		RunDma();
	}
	RunPeripherals((uint32_t)(_state.CycleCount - start));
}

void Sh2Core::DelayedBranch(uint32_t target)
{
	_state.SlotPending = true;
	_state.SlotTarget = target;
}

uint32_t Sh2Core::PcRelBase()
{
	//PC-relative operands use the instruction address + 4; in a delay slot, the branch
	//destination + 2 (Hitachi programming manual, MOVA/MOV @(disp,PC); same as ares)
	return _inDelaySlot ? _curSlotTarget + 2 : _instPc + 4;
}

// Execution states beyond the 1 charged per instruction come from the Hitachi SH-1/SH-2
// programming manual's timing table (base values, no contention); see findings S1.
void Sh2Core::Execute(uint16_t op)
{
	uint32_t* R = _state.R;
	uint32_t n = (op >> 8) & 15;
	uint32_t m = (op >> 4) & 15;
	uint32_t& sr = _state.SR;
	auto setT = [&sr](bool t) { sr = (sr & ~SrT) | (t ? SrT : 0); };
	auto getT = [&sr]() -> uint32_t { return sr & SrT; };
	auto slotCheck = [&]() -> bool {
		if(_inDelaySlot) {
			IllegalSlotInstruction();
			return true;
		}
		return false;
	};

	switch(op >> 12) {
		case 0x0:
			Exec0(op);
			return;

		case 0x1: //MOV.L Rm,@(disp,Rn)
			WriteLong(R[n] + (op & 15) * 4, R[m]);
			return;

		case 0x2:
			switch(op & 15) {
				case 0x0: WriteByte(R[n], R[m]); return; //MOV.B Rm,@Rn
				case 0x1: WriteWord(R[n], R[m]); return; //MOV.W Rm,@Rn
				case 0x2: WriteLong(R[n], R[m]); return; //MOV.L Rm,@Rn
				case 0x4: { //MOV.B Rm,@-Rn
					uint32_t v = R[m];
					R[n] -= 1;
					WriteByte(R[n], v);
					return;
				}
				case 0x5: {
					uint32_t v = R[m];
					R[n] -= 2;
					WriteWord(R[n], v);
					return;
				}
				case 0x6: {
					uint32_t v = R[m];
					R[n] -= 4;
					WriteLong(R[n], v);
					return;
				}
				case 0x7: { //DIV0S Rm,Rn
					uint32_t q = R[n] >> 31;
					uint32_t mm = R[m] >> 31;
					sr = (sr & ~(SrQ | SrM | SrT)) | (q ? SrQ : 0) | (mm ? SrM : 0) | (q != mm ? SrT : 0);
					return;
				}
				case 0x8: setT((R[n] & R[m]) == 0); return; //TST
				case 0x9: R[n] &= R[m]; return;
				case 0xA: R[n] ^= R[m]; return;
				case 0xB: R[n] |= R[m]; return;
				case 0xC: { //CMP/STR
					uint32_t t = R[n] ^ R[m];
					setT(!(t & 0xFF000000) || !(t & 0x00FF0000) || !(t & 0x0000FF00) || !(t & 0x000000FF));
					return;
				}
				case 0xD: R[n] = R[n] >> 16 | R[m] << 16; return; //XTRCT
				case 0xE: _state.MACL = (uint32_t)((uint16_t)R[n] * (uint16_t)R[m]); return; //MULU.W
				case 0xF: _state.MACL = (uint32_t)((int16_t)R[n] * (int16_t)R[m]); return; //MULS.W
			}
			break;

		case 0x3:
			switch(op & 15) {
				case 0x0: setT(R[n] == R[m]); return;
				case 0x2: setT(R[n] >= R[m]); return; //CMP/HS
				case 0x3: setT((int32_t)R[n] >= (int32_t)R[m]); return;
				case 0x4: { //DIV1
					//Transcribed from the programming manual's C description: Rn is shifted
					//before Rm is read, so DIV1 Rn,Rn uses the shifted value (ares reads Rm
					//first; findings S1)
					bool oldQ = sr & SrQ;
					bool mBit = sr & SrM;
					bool q = R[n] >> 31;
					R[n] = R[n] << 1 | getT();
					uint32_t tmp0 = R[n];
					bool tmp1;
					if(oldQ == mBit) {
						R[n] -= R[m];
						tmp1 = R[n] > tmp0;
					} else {
						R[n] += R[m];
						tmp1 = R[n] < tmp0;
					}
					//The manual's four (old Q, M) switch cases all reduce to: Q = tmp1 when the
					//new Q equals M, otherwise !tmp1
					q = (q == mBit) ? tmp1 : !tmp1;
					bool t = q == mBit;
					sr = (sr & ~(SrQ | SrT)) | (q ? SrQ : 0) | (t ? SrT : 0);
					return;
				}
				case 0x5: { //DMULU.L
					uint64_t r = (uint64_t)R[n] * R[m];
					_state.MACH = (uint32_t)(r >> 32);
					_state.MACL = (uint32_t)r;
					AddCycles(1);
					return;
				}
				case 0x6: setT(R[n] > R[m]); return; //CMP/HI
				case 0x7: setT((int32_t)R[n] > (int32_t)R[m]); return;
				case 0x8: R[n] -= R[m]; return;
				case 0xA: { //SUBC
					uint32_t a = R[n];
					uint32_t b = a - R[m];
					R[n] = b - getT();
					setT(a < b || b < R[n]);
					return;
				}
				case 0xB: { //SUBV
					uint32_t d = (int32_t)R[n] < 0;
					uint32_t s = ((int32_t)R[m] < 0) + d;
					R[n] -= R[m];
					uint32_t r = ((int32_t)R[n] < 0) + d;
					setT(s == 1 ? r == 1 : false);
					return;
				}
				case 0xC: R[n] += R[m]; return;
				case 0xD: { //DMULS.L
					int64_t r = (int64_t)(int32_t)R[n] * (int32_t)R[m];
					_state.MACH = (uint32_t)((uint64_t)r >> 32);
					_state.MACL = (uint32_t)r;
					AddCycles(1);
					return;
				}
				case 0xE: { //ADDC
					uint32_t a = R[n];
					uint32_t b = a + R[m];
					R[n] = b + getT();
					setT(a > b || b > R[n]);
					return;
				}
				case 0xF: { //ADDV
					uint32_t d = (int32_t)R[n] < 0;
					uint32_t s = ((int32_t)R[m] < 0) + d;
					R[n] += R[m];
					uint32_t r = ((int32_t)R[n] < 0) + d;
					setT(s != 1 ? r == 1 : false);
					return;
				}
			}
			break;

		case 0x4:
			Exec4(op);
			return;

		case 0x5: //MOV.L @(disp,Rm),Rn
			R[n] = ReadLong(R[m] + (op & 15) * 4);
			return;

		case 0x6:
			switch(op & 15) {
				case 0x0: R[n] = (uint32_t)(int8_t)ReadByte(R[m]); return;
				case 0x1: R[n] = (uint32_t)(int16_t)ReadWord(R[m]); return;
				case 0x2: R[n] = ReadLong(R[m]); return;
				case 0x3: R[n] = R[m]; return;
				case 0x4: {
					uint32_t v = (uint32_t)(int8_t)ReadByte(R[m]);
					if(n != m) {
						R[m] += 1;
					}
					R[n] = v;
					return;
				}
				case 0x5: {
					uint32_t v = (uint32_t)(int16_t)ReadWord(R[m]);
					if(n != m) {
						R[m] += 2;
					}
					R[n] = v;
					return;
				}
				case 0x6: {
					uint32_t v = ReadLong(R[m]);
					if(n != m) {
						R[m] += 4;
					}
					R[n] = v;
					return;
				}
				case 0x7: R[n] = ~R[m]; return;
				case 0x8: R[n] = (R[m] & 0xFFFF0000) | (R[m] >> 8 & 0xFF) | (R[m] << 8 & 0xFF00); return; //SWAP.B
				case 0x9: R[n] = R[m] << 16 | R[m] >> 16; return;
				case 0xA: { //NEGC
					uint32_t t = 0 - R[m];
					R[n] = t - getT();
					setT(0 < t || t < R[n]);
					return;
				}
				case 0xB: R[n] = 0 - R[m]; return;
				case 0xC: R[n] = (uint8_t)R[m]; return;
				case 0xD: R[n] = (uint16_t)R[m]; return;
				case 0xE: R[n] = (uint32_t)(int8_t)R[m]; return;
				case 0xF: R[n] = (uint32_t)(int16_t)R[m]; return;
			}
			break;

		case 0x7: //ADD #imm,Rn
			R[n] += (uint32_t)(int8_t)op;
			return;

		case 0x8:
			Exec8(op);
			return;

		case 0x9: //MOV.W @(disp,PC),Rn
			R[n] = (uint32_t)(int16_t)ReadWord(PcRelBase() + (op & 0xFF) * 2);
			return;

		case 0xA: //BRA
			if(slotCheck()) {
				return;
			}
			DelayedBranch(_instPc + 4 + (uint32_t)((int32_t)((uint32_t)op << 20) >> 19));
			AddCycles(1);
			return;

		case 0xB: //BSR
			if(slotCheck()) {
				return;
			}
			_state.PR = _instPc + 4;
			DelayedBranch(_instPc + 4 + (uint32_t)((int32_t)((uint32_t)op << 20) >> 19));
			AddCycles(1);
			return;

		case 0xC:
			ExecC(op);
			return;

		case 0xD: //MOV.L @(disp,PC),Rn
			R[n] = ReadLong((PcRelBase() & ~3) + (op & 0xFF) * 4);
			return;

		case 0xE: //MOV #imm,Rn
			R[n] = (uint32_t)(int8_t)op;
			return;
	}

	IllegalInstruction();
}

void Sh2Core::Exec0(uint16_t op)
{
	uint32_t* R = _state.R;
	uint32_t n = (op >> 8) & 15;
	uint32_t m = (op >> 4) & 15;
	uint32_t& sr = _state.SR;

	switch(op & 15) {
		case 0x4: WriteByte(R[n] + R[0], R[m]); return;
		case 0x5: WriteWord(R[n] + R[0], R[m]); return;
		case 0x6: WriteLong(R[n] + R[0], R[m]); return;
		case 0x7: //MUL.L
			_state.MACL = R[n] * R[m];
			AddCycles(1);
			return;
		case 0xC: R[n] = (uint32_t)(int8_t)ReadByte(R[m] + R[0]); return;
		case 0xD: R[n] = (uint32_t)(int16_t)ReadWord(R[m] + R[0]); return;
		case 0xE: R[n] = ReadLong(R[m] + R[0]); return;
		case 0xF: { //MAC.L @Rm+,@Rn+
			int32_t a = (int32_t)ReadLong(R[n]);
			R[n] += 4;
			int32_t b = (int32_t)ReadLong(R[m]);
			R[m] += 4;
			int64_t mac = (int64_t)((uint64_t)_state.MACH << 32 | _state.MACL);
			mac += (int64_t)a * b;
			if(sr & SrS) {
				mac = ClampSigned(mac, 48);
			}
			_state.MACH = (uint32_t)((uint64_t)mac >> 32);
			_state.MACL = (uint32_t)mac;
			AddCycles(2);
			return;
		}
	}

	switch(op & 0xFF) {
		case 0x02:
			R[n] = sr;
			_state.NoInterrupt = true;
			return; //STC SR,Rn
		case 0x12:
			R[n] = _state.GBR;
			_state.NoInterrupt = true;
			return;
		case 0x22:
			R[n] = _state.VBR;
			_state.NoInterrupt = true;
			return;
		case 0x0A:
			R[n] = _state.MACH;
			_state.NoInterrupt = true;
			return;
		case 0x1A:
			R[n] = _state.MACL;
			_state.NoInterrupt = true;
			return;
		case 0x2A:
			R[n] = _state.PR;
			_state.NoInterrupt = true;
			return;
		case 0x29: R[n] = sr & SrT; return; //MOVT
		case 0x03: //BSRF Rm
			if(_inDelaySlot) {
				IllegalSlotInstruction();
				return;
			}
			_state.PR = _instPc + 4;
			DelayedBranch(_instPc + 4 + R[n]);
			AddCycles(1);
			return;
		case 0x23: //BRAF Rm
			if(_inDelaySlot) {
				IllegalSlotInstruction();
				return;
			}
			DelayedBranch(_instPc + 4 + R[n]);
			AddCycles(1);
			return;
	}

	switch(op) {
		case 0x0008: sr &= ~SrT; return; //CLRT
		case 0x0009: return; //NOP
		case 0x000B: //RTS
			if(_inDelaySlot) {
				IllegalSlotInstruction();
				return;
			}
			DelayedBranch(_state.PR);
			AddCycles(1);
			return;
		case 0x0018: sr |= SrT; return; //SETT
		case 0x0019: sr &= ~(SrQ | SrM | SrT); return; //DIV0U
		case 0x001B: //SLEEP
			_state.Sleeping = true;
			AddCycles(2);
			return;
		case 0x0028: _state.MACH = _state.MACL = 0; return; //CLRMAC
		case 0x002B: { //RTE
			if(_inDelaySlot) {
				IllegalSlotInstruction();
				return;
			}
			uint32_t pc = ReadLong(R[15]);
			sr = ReadLong(R[15] + 4) & SrMask;
			R[15] += 8;
			DelayedBranch(pc);
			AddCycles(3);
			return;
		}
	}

	IllegalInstruction();
}

void Sh2Core::Exec4(uint16_t op)
{
	uint32_t* R = _state.R;
	uint32_t n = (op >> 8) & 15;
	uint32_t m = (op >> 4) & 15;
	uint32_t& sr = _state.SR;
	auto setT = [&sr](bool t) { sr = (sr & ~SrT) | (t ? SrT : 0); };

	if((op & 15) == 0xF) { //MAC.W @Rm+,@Rn+
		int16_t a = (int16_t)ReadWord(R[n]);
		R[n] += 2;
		int16_t b = (int16_t)ReadWord(R[m]);
		R[m] += 2;
		int64_t prod = (int64_t)a * b;
		if(!(sr & SrS)) {
			uint64_t mac = (uint64_t)_state.MACH << 32 | _state.MACL;
			mac += (uint64_t)prod;
			_state.MACH = (uint32_t)(mac >> 32);
			_state.MACL = (uint32_t)mac;
		} else {
			//S = 1: 32-bit saturation of MACL; MACH is unchanged on the SH-2 (programming manual's
			//MACW definition; ares sets MACH bit 0 and treats MACL as unsigned; findings S1)
			_state.MACL = (uint32_t)ClampSigned((int64_t)(int32_t)_state.MACL + prod, 32);
		}
		AddCycles(2);
		return;
	}

	switch(op & 0xFF) {
		case 0x00:
			setT(R[n] >> 31);
			R[n] <<= 1;
			return; //SHLL
		case 0x01:
			setT(R[n] & 1);
			R[n] >>= 1;
			return; //SHLR
		case 0x02:
			R[n] -= 4;
			WriteLong(R[n], _state.MACH);
			_state.NoInterrupt = true;
			return; //STS.L MACH
		case 0x03:
			R[n] -= 4;
			WriteLong(R[n], sr);
			_state.NoInterrupt = true;
			AddCycles(1);
			return; //STC.L SR
		case 0x04:
			setT(R[n] >> 31);
			R[n] = R[n] << 1 | R[n] >> 31;
			return; //ROTL
		case 0x05:
			setT(R[n] & 1);
			R[n] = R[n] >> 1 | R[n] << 31;
			return; //ROTR
		case 0x06:
			_state.MACH = ReadLong(R[n]);
			R[n] += 4;
			_state.NoInterrupt = true;
			return; //LDS.L @Rm+,MACH
		case 0x07:
			sr = ReadLong(R[n]) & SrMask;
			R[n] += 4;
			_state.NoInterrupt = true;
			AddCycles(2);
			return; //LDC.L @Rm+,SR
		case 0x08: R[n] <<= 2; return;
		case 0x09: R[n] >>= 2; return;
		case 0x0A:
			_state.MACH = R[n];
			_state.NoInterrupt = true;
			return;
		case 0x0B: //JSR @Rm
			if(_inDelaySlot) {
				IllegalSlotInstruction();
				return;
			}
			_state.PR = _instPc + 4;
			DelayedBranch(R[n]);
			AddCycles(1);
			return;
		case 0x0E:
			sr = R[n] & SrMask;
			_state.NoInterrupt = true;
			return; //LDC Rm,SR
		case 0x10:
			R[n] -= 1;
			setT(R[n] == 0);
			return; //DT
		case 0x11: setT((int32_t)R[n] >= 0); return; //CMP/PZ
		case 0x12:
			R[n] -= 4;
			WriteLong(R[n], _state.MACL);
			_state.NoInterrupt = true;
			return;
		case 0x13:
			R[n] -= 4;
			WriteLong(R[n], _state.GBR);
			_state.NoInterrupt = true;
			AddCycles(1);
			return;
		case 0x15: setT((int32_t)R[n] > 0); return; //CMP/PL
		case 0x16:
			_state.MACL = ReadLong(R[n]);
			R[n] += 4;
			_state.NoInterrupt = true;
			return;
		case 0x17:
			_state.GBR = ReadLong(R[n]);
			R[n] += 4;
			_state.NoInterrupt = true;
			AddCycles(2);
			return;
		case 0x18: R[n] <<= 8; return;
		case 0x19: R[n] >>= 8; return;
		case 0x1A:
			_state.MACL = R[n];
			_state.NoInterrupt = true;
			return;
		case 0x1B: { //TAS.B @Rn (a locked read-modify-write; bypasses the cache on hardware)
			uint32_t addr = R[n];
			if(!_flatBus && (addr >> 29) == Cached) {
				addr |= 0x20000000;
			}
			uint8_t v = (uint8_t)ReadByte(addr);
			setT(v == 0);
			WriteByte(addr, v | 0x80);
			AddCycles(3);
			return;
		}
		case 0x1E:
			_state.GBR = R[n];
			_state.NoInterrupt = true;
			return;
		case 0x20:
			setT(R[n] >> 31);
			R[n] <<= 1;
			return; //SHAL
		case 0x21:
			setT(R[n] & 1);
			R[n] = (uint32_t)((int32_t)R[n] >> 1);
			return; //SHAR
		case 0x22:
			R[n] -= 4;
			WriteLong(R[n], _state.PR);
			_state.NoInterrupt = true;
			return;
		case 0x23:
			R[n] -= 4;
			WriteLong(R[n], _state.VBR);
			_state.NoInterrupt = true;
			AddCycles(1);
			return;
		case 0x24: { //ROTCL
			bool c = R[n] >> 31;
			R[n] = R[n] << 1 | (sr & SrT);
			setT(c);
			return;
		}
		case 0x25: { //ROTCR
			bool c = R[n] & 1;
			R[n] = R[n] >> 1 | (sr & SrT) << 31;
			setT(c);
			return;
		}
		case 0x26:
			_state.PR = ReadLong(R[n]);
			R[n] += 4;
			_state.NoInterrupt = true;
			return;
		case 0x27:
			_state.VBR = ReadLong(R[n]);
			R[n] += 4;
			_state.NoInterrupt = true;
			AddCycles(2);
			return;
		case 0x28: R[n] <<= 16; return;
		case 0x29: R[n] >>= 16; return;
		case 0x2A:
			_state.PR = R[n];
			_state.NoInterrupt = true;
			return;
		case 0x2B: //JMP @Rm
			if(_inDelaySlot) {
				IllegalSlotInstruction();
				return;
			}
			DelayedBranch(R[n]);
			AddCycles(1);
			return;
		case 0x2E:
			_state.VBR = R[n];
			_state.NoInterrupt = true;
			return;
	}

	(void)m;
	IllegalInstruction();
}

void Sh2Core::Exec8(uint16_t op)
{
	uint32_t* R = _state.R;
	uint32_t& sr = _state.SR;
	uint32_t d = op & 0xFF;
	uint32_t m = (op >> 4) & 15;
	auto setT = [&sr](bool t) { sr = (sr & ~SrT) | (t ? SrT : 0); };
	uint32_t target = _instPc + 4 + (uint32_t)((int32_t)(int8_t)d * 2);

	switch((op >> 8) & 15) {
		case 0x0: WriteByte(R[m] + (op & 15), R[0]); return; //MOV.B R0,@(disp,Rn)
		case 0x1: WriteWord(R[m] + (op & 15) * 2, R[0]); return;
		case 0x4: R[0] = (uint32_t)(int8_t)ReadByte(R[m] + (op & 15)); return; //MOV.B @(disp,Rm),R0
		case 0x5: R[0] = (uint32_t)(int16_t)ReadWord(R[m] + (op & 15) * 2); return;
		case 0x8: setT(R[0] == (uint32_t)(int8_t)d); return; //CMP/EQ #imm,R0
		case 0x9: //BT
		case 0xB: { //BF
			if(_inDelaySlot) {
				IllegalSlotInstruction();
				return;
			}
			bool t = sr & SrT;
			if(t == (((op >> 8) & 15) == 0x9)) {
				_state.PC = target;
				AddCycles(2);
			}
			return;
		}
		case 0xD: //BT/S
		case 0xF: { //BF/S
			if(_inDelaySlot) {
				IllegalSlotInstruction();
				return;
			}
			bool t = sr & SrT;
			if(t == (((op >> 8) & 15) == 0xD)) {
				DelayedBranch(target);
				AddCycles(1);
			}
			return;
		}
	}
	IllegalInstruction();
}

void Sh2Core::ExecC(uint16_t op)
{
	uint32_t* R = _state.R;
	uint32_t& sr = _state.SR;
	uint32_t d = op & 0xFF;
	uint32_t gbr = _state.GBR;
	auto setT = [&sr](bool t) { sr = (sr & ~SrT) | (t ? SrT : 0); };

	switch((op >> 8) & 15) {
		case 0x0: WriteByte(gbr + d, R[0]); return;
		case 0x1: WriteWord(gbr + d * 2, R[0]); return;
		case 0x2: WriteLong(gbr + d * 4, R[0]); return;
		case 0x3: //TRAPA #imm
			if(_inDelaySlot) {
				IllegalSlotInstruction();
				return;
			}
			EnterException(d, _instPc + 2);
			AddCycles(7);
			return;
		case 0x4: R[0] = (uint32_t)(int8_t)ReadByte(gbr + d); return;
		case 0x5: R[0] = (uint32_t)(int16_t)ReadWord(gbr + d * 2); return;
		case 0x6: R[0] = ReadLong(gbr + d * 4); return;
		case 0x7: R[0] = (PcRelBase() & ~3) + d * 4; return; //MOVA
		case 0x8: setT((R[0] & d) == 0); return; //TST #imm,R0
		case 0x9: R[0] &= d; return;
		case 0xA: R[0] ^= d; return;
		case 0xB: R[0] |= d; return;
		case 0xC:
			setT((ReadByte(gbr + R[0]) & d) == 0);
			AddCycles(2);
			return; //TST.B
		case 0xD:
			WriteByte(gbr + R[0], ReadByte(gbr + R[0]) & d);
			AddCycles(2);
			return; //AND.B
		case 0xE:
			WriteByte(gbr + R[0], ReadByte(gbr + R[0]) ^ d);
			AddCycles(2);
			return; //XOR.B
		case 0xF:
			WriteByte(gbr + R[0], ReadByte(gbr + R[0]) | d);
			AddCycles(2);
			return; //OR.B
	}
}
