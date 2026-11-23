#include "pch.h"
#include "MD/Sh2/Sh2Core.h"
#include "Utilities/Serializer.h"

void Sh2Core::Serialize(Serializer& s)
{
	SVArray(_state.R, 16);
	SV(_state.PC);
	SV(_state.PR);
	SV(_state.GBR);
	SV(_state.VBR);
	SV(_state.MACH);
	SV(_state.MACL);
	SV(_state.SR);
	SV(_state.CycleCount);
	SV(_state.SlotPending);
	SV(_state.SlotTarget);
	SV(_state.NoInterrupt);
	SV(_state.Sleeping);
	SV(_state.PendingReset);

	SVArray(_cache.Lru, 64);
	SVArray(_cache.Tags, 4 * 64);
	SVArray(&_cache.Lines[0][0], 4 * 64 * 16);
	SV(_cache.Enable);
	SV(_cache.DisableCode);
	SV(_cache.DisableData);
	SV(_cache.TwoWay);
	SV(_cache.WaySelect);

	SV(_intc.Nmie);
	SV(_intc.Nmil);
	SV(_intc.Vecmd);
	SV(_intc.WdtIp);
	SV(_intc.DmacIp);
	SV(_intc.DivuIp);
	SV(_intc.FrtIp);
	SV(_intc.SciIp);
	SV(_intc.SciErrVec);
	SV(_intc.SciRxVec);
	SV(_intc.SciTxEndVec);
	SV(_intc.SciTxVec);
	SV(_intc.FrtIcVec);
	SV(_intc.FrtOcVec);
	SV(_intc.FrtOvVec);
	SV(_intc.BscCmVec);
	SV(_intc.WdtItVec);

	for(int i = 0; i < 2; i++) {
		DmaChannel& c = _dmac.Ch[i];
		SVI(c.Sar);
		SVI(c.Dar);
		SVI(c.Tcr);
		SVI(c.De);
		SVI(c.Te);
		SVI(c.Ie);
		SVI(c.Ta);
		SVI(c.Tb);
		SVI(c.Dl);
		SVI(c.Ds);
		SVI(c.Al);
		SVI(c.Am);
		SVI(c.Ar);
		SVI(c.Ts);
		SVI(c.Sm);
		SVI(c.Dm);
		SVI(c.Vector);
		SVI(c.Drcr);
		SVI(c.Dreq);
	}
	SV(_dmac.Dme);
	SV(_dmac.Nmif);
	SV(_dmac.Ae);
	SV(_dmac.Pr);
	SV(_dmac.RoundRobinFirst);

	SV(_divu.Dvsr);
	SV(_divu.Dvdnth);
	SV(_divu.Dvdntl);
	SV(_divu.Ovf);
	SV(_divu.Ovfie);
	SV(_divu.Vector);
	SV(_divu.BusyUntil);

	SV(_frt.Ovie);
	SV(_frt.Ociae);
	SV(_frt.Ocibe);
	SV(_frt.Icie);
	SV(_frt.Cclra);
	SV(_frt.Ovf);
	SV(_frt.Ocfa);
	SV(_frt.Ocfb);
	SV(_frt.Icf);
	SV(_frt.Frc);
	SV(_frt.Ocra);
	SV(_frt.Ocrb);
	SV(_frt.Ficr);
	SV(_frt.Cks);
	SV(_frt.Iedg);
	SV(_frt.Olvla);
	SV(_frt.Olvlb);
	SV(_frt.Ocrs);
	SV(_frt.Temp);
	SV(_frt.Prescaler);

	SV(_wdt.Cks);
	SV(_wdt.Tme);
	SV(_wdt.Wtit);
	SV(_wdt.Ovf);
	SV(_wdt.Rsts);
	SV(_wdt.Rste);
	SV(_wdt.Wovf);
	SV(_wdt.Wtcnt);
	SV(_wdt.Prescaler);

	SV(_sci.Smr);
	SV(_sci.Brr);
	SV(_sci.Scr);
	SV(_sci.Tdr);
	SV(_sci.Rdr);
	SV(_sci.Tend);
	SV(_sci.Rdrf);
	SV(_sci.Tdre);
	SV(_sci.Orer);
	SV(_sci.Fer);
	SV(_sci.Per);
	SV(_sci.Mpb);
	SV(_sci.Mpbt);

	SV(_bsc.Bcr1);
	SV(_bsc.Bcr2);
	SV(_bsc.Wcr);
	SV(_bsc.Mcr);
	SV(_bsc.Rtcsr);
	SV(_bsc.Rtcnt);
	SV(_bsc.Rtcor);

	SV(_ubc.Bara);
	SV(_ubc.Bamra);
	SV(_ubc.Barb);
	SV(_ubc.Bamrb);
	SV(_ubc.Bdrb);
	SV(_ubc.Bdmrb);
	SV(_ubc.Bbra);
	SV(_ubc.Bbrb);
	SV(_ubc.Brcr);
	SV(_sbycr);

	SV(_irlLevel);
	SV(_irlVector);
	SV(_nmiPending);
}
