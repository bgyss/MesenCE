#pragma once
#include "pch.h"

enum class CpuType : uint8_t
{
	Snes,
	Spc,
	NecDsp,
	Sa1,
	Gsu,
	Cx4,
	St018,
	Gameboy,
	Nes,
	Pce,
	Sms,
	Gba,
	Ws,
	MdM68k,
	MdZ80,
	McdM68k, //Sega CD sub CPU
	M32xMaster, //32X master SH-2
	M32xSlave //32X slave SH-2
};

class CpuTypeUtilities
{
public:
	static constexpr int GetCpuTypeCount()
	{
		return (int)CpuType::M32xSlave + 1;
	}
};