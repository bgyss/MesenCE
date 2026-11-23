#include "MD/M68k/ares/m68000.h"

namespace ares {

enum : u32  { Byte, Word, Long };
enum : bool { Reverse = 1 };

#include "registers.inc"
#include "memory.inc"
#include "effective-address.inc"
#include "traits.inc"
#include "conditions.inc"
#include "algorithms.inc"
#include "instructions.inc"
#include "disassembler.inc"
#include "instruction.inc"

auto M68000::power() -> void {
  for(auto& dr : r.d) dr = 0;
  for(auto& ar : r.a) ar = 0;
  r.sp = 0;
  r.pc = 0;

  r.c = 0;
  r.v = 0;
  r.z = 0;
  r.n = 0;
  r.x = 0;
  r.i = 7;
  r.s = 1;
  r.t = 0;

  r.irc = 0x4e71;  //nop
  r.ir  = 0x4e71;  //nop
  r.ird = 0x4e71;  //nop

  r.stop  = false;
  r.reset = false;
}

auto M68000::supervisor() -> bool {
  if(r.s) return true;

  exception(Exception::Unprivileged, Vector::Unprivileged);
  return false;
}

auto M68000::exception(u32 exception, u32 vector, u32 priority, u32 leadIdle) -> void {
  r.stop  = false;

  //MesenCE: bus order and idle placement follow SingleStepTests/m68000 (MAME microcode):
  //PC low, then SR, then PC high; two dead cycles between the two prefetches.
  bool irq = exception == Exception::Interrupt;
  idle(irq ? 6 : leadIdle);
  n32 pc = r.pc - 4;
  n32 sr = readSR();
  if(!r.s) swap(r.a[7], r.sp);
  r.s = 1;
  r.t = 0;

  write<Word>(r.a[7] - 2, pc & 0x0000ffff);

  // external interrupt handling
  if(irq) {
    // IACK vector number acquisition (+4 cyc normal or +10 to +18 cyc autovectored)
    // & justify vector number (+4 cyc)
    idle(14+4); // assuming autovector cycles (approximated for Megadrive)
    r.i = priority;
  }

  write<Word>(r.a[7] - 6, sr);
  write<Word>(r.a[7] - 4, pc >> 16);
  r.a[7] -= 6;

  r.pc = read<Long>(vector << 2);

  prefetch();
  idle(2);
  prefetch();
}

auto M68000::interrupt(u32 vector, u32 priority) -> void {
  return exception(Exception::Interrupt, vector, priority);
}

}
