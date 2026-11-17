//used by JMP and JSR: as PC is guaranteed to change, avoid performing any unnecessary prefetches
auto M68000::prefetched(EffectiveAddress& ea) -> n32 {
  if(ea.valid) return ea.address;
  ea.valid = true;

  switch(ea.mode) {

  case AddressRegisterIndirect: {
    return ea.address = r.a[ea.reg];
  }

  case AddressRegisterIndirectWithDisplacement: {
    idle(2);
    return ea.address = r.a[ea.reg] + (i16)prefetched();
  }

  case AddressRegisterIndirectWithIndex: {
    idle(6);
    auto extension = prefetched();
    auto index = extension & 0x8000 ? r.a[extension >> 12 & 7] : r.d[extension >> 12 & 7];
    if(!(extension & 0x800)) index = (i16)index;
    return ea.address = r.a[ea.reg] + index + (i8)extension;
  }

  case AbsoluteShortIndirect: {
    idle(2);
    return ea.address = (i16)prefetched();
  }

  case AbsoluteLongIndirect: {
    auto hi = extension<Word>();  //MesenCE: an extension fetch, it does not latch IR
    auto lo = prefetched();
    return ea.address = hi << 16 | lo << 0;
  }

  case ProgramCounterIndirectWithDisplacement: {
    idle(2);
    auto base = r.pc - 2;
    return ea.address = base + (i16)prefetched();
  }

  case ProgramCounterIndirectWithIndex: {
    idle(6);
    auto base = r.pc - 2;
    auto extension = prefetched();
    auto index = extension & 0x8000 ? r.a[extension >> 12 & 7] : r.d[extension >> 12 & 7];
    if(!(extension & 0x800)) index = (i16)index;
    return ea.address = base + index + (i8)extension;
  }

  }

  return ea.address = 0;  //should never occur
}

template<u32 Size> auto M68000::fetch(EffectiveAddress& ea) -> n32 {
  if(ea.valid) return ea.address;
  ea.valid = true;

  switch(ea.mode) {

  case DataRegisterDirect: {
    return ea.address = read(DataRegister{ea.reg});
  }

  case AddressRegisterDirect: {
    return ea.address = read(AddressRegister{ea.reg});
  }

  case AddressRegisterIndirect: {
    return ea.address = read(AddressRegister{ea.reg});
  }

  case AddressRegisterIndirectWithPostIncrement: {
    return ea.address = read(AddressRegister{ea.reg});
  }

  case AddressRegisterIndirectWithPreDecrement: {
    return ea.address = read(AddressRegister{ea.reg});
  }

  case AddressRegisterIndirectWithDisplacement: {
    return ea.address = read(AddressRegister{ea.reg}) + (i16)extension<Word>();
  }

  case AddressRegisterIndirectWithIndex: {
    idle(2);
    auto data = extension<Word>();
    auto index = data & 0x8000
    ? read(AddressRegister{data >> 12})
    : read(DataRegister{data >> 12});
    if(!(data & 0x800)) index = (i16)index;
    return ea.address = read(AddressRegister{ea.reg}) + index + (i8)data;
  }

  case AbsoluteShortIndirect: {
    return ea.address = (i16)extension<Word>();
  }

  case AbsoluteLongIndirect: {
    return ea.address = extension<Long>();
  }

  case ProgramCounterIndirectWithDisplacement: {
    auto base = r.pc - 2;
    return ea.address = base + (i16)extension<Word>();
  }

  case ProgramCounterIndirectWithIndex: {
    idle(2);
    auto base = r.pc - 2;
    auto data = extension<Word>();
    auto index = data & 0x8000
    ? read(AddressRegister{data >> 12})
    : read(DataRegister{data >> 12});
    if(!(data & 0x800)) index = (i16)index;
    return ea.address = base + index + (i8)data;
  }

  case Immediate: {
    return ea.address = extension<Size>();
  }

  }

  return ea.address = 0;  //should never occur
}

template<u32 Size, bool hold, bool fast> auto M68000::read(EffectiveAddress& ea) -> n32 {
  fetch<Size>(ea);
  faultMode = ea.mode;
  faultLong = Size == Long;
  faultWrite = false;
  if(hold) ea.held = true;
  //MesenCE: PC-relative operands are read from program space
  if(ea.mode == ProgramCounterIndirectWithDisplacement || ea.mode == ProgramCounterIndirectWithIndex) {
    access = Access::Program;
    auto data = read<Size>(ea.address);
    access = Access::Data;
    return data;
  }

  switch(ea.mode) {

  case DataRegisterDirect: {
    return clip<Size>(ea.address);
  }

  case AddressRegisterDirect: {
    return sign<Size>(ea.address);
  }

  case AddressRegisterIndirect: {
    return read<Size>(ea.address);
  }

  case AddressRegisterIndirectWithPostIncrement: {
    auto address = ea.address + (ea.reg == 7 && Size == Byte ? bytes<Word>() : bytes<Size>());
    faultUpdate = {Size != Long, ea.reg, address};  //a faulting long access leaves An unchanged
    if(postIncFault >= 0) faultUpdate = {postIncFault > 0, ea.reg, ea.address + postIncFault};
    auto data = read<Size>(ea.address);
    faultUpdate.pending = false;
    if(!hold) write(AddressRegister{ea.reg}, ea.address = address);
    return data;
  }

  case AddressRegisterIndirectWithPreDecrement: {
    if(!fast) idle(2);
    auto address = ea.address - (ea.reg == 7 && Size == Byte ? bytes<Word>() : bytes<Size>());
    if(Size == Long && (fast || lowWordFirst)) {
      //MesenCE: ADDX/SUBX -(An) long reads fetch the low word first, and a fault reports
      //the low word's address and leaves An unchanged (SingleStepTests/m68000)
      wait(4);
      if(address & 1) addressError(false, address + 2, 1, 1);
      n32 data = read(1, 1, address + 2 & ~1);
      wait(4);
      data |= read(1, 1, address & ~1) << 16;
      if(!hold) write(AddressRegister{ea.reg}, ea.address = address);
      return data;
    }
    //MesenCE: An is decremented before the access (visible when it raises an address error)
    if(!hold) write(AddressRegister{ea.reg}, ea.address = address);
    faultUpdate = {true, ea.reg, address};
    auto data = read<Size>(address);
    faultUpdate.pending = false;
    return data;
  }

  case AddressRegisterIndirectWithDisplacement: {
    return read<Size>(ea.address);
  }

  case AddressRegisterIndirectWithIndex: {
    return read<Size>(ea.address);
  }

  case AbsoluteShortIndirect: {
    return read<Size>(ea.address);
  }

  case AbsoluteLongIndirect: {
    return read<Size>(ea.address);
  }

  case ProgramCounterIndirectWithDisplacement: {
    return read<Size>(ea.address);
  }

  case ProgramCounterIndirectWithIndex: {
    return read<Size>(ea.address);
  }

  case Immediate: {
    return clip<Size>(ea.address);
  }

  }

  return 0;
}

template<u32 Size, bool hold> auto M68000::write(EffectiveAddress& ea, n32 data) -> void {
  fetch<Size>(ea);
  faultMode = ea.mode;
  faultLong = Size == Long;
  faultWrite = true;
  //MesenCE: the write half of a long read-modify-write stores the low word first
  if constexpr(Size == Long) {
    if(ea.held && ea.mode >= AddressRegisterIndirect && ea.mode != AddressRegisterIndirectWithPreDecrement && ea.mode != Immediate) {
      n32 address = ea.address;
      if(ea.mode == AddressRegisterIndirectWithPostIncrement) {
        faultUpdate = {false, ea.reg, address + 4};
        write<Long, Reverse>(address, data);
        if(!hold) write(AddressRegister{ea.reg}, ea.address = address + 4);
      } else {
        write<Long, Reverse>(address, data);
      }
      return;
    }
  }

  switch(ea.mode) {

  case DataRegisterDirect: {
    return write<Size>(DataRegister{ea.reg}, data);
  }

  case AddressRegisterDirect: {
    return write<Size>(AddressRegister{ea.reg}, data);
  }

  case AddressRegisterIndirect: {
    return write<Size>(ea.address, data);
  }

  case AddressRegisterIndirectWithPostIncrement: {
    auto address = ea.address + (ea.reg == 7 && Size == Byte ? bytes<Word>() : bytes<Size>());
    faultUpdate = {false, ea.reg, address};  //a faulting write leaves An unchanged
    write<Size>(ea.address, data);
    faultUpdate.pending = false;
    if(!hold) write(AddressRegister{ea.reg}, ea.address = address);
    return;
  }

  case AddressRegisterIndirectWithPreDecrement: {
    auto address = ea.address - (ea.reg == 7 && Size == Byte ? bytes<Word>() : bytes<Size>());
    if(!hold) write(AddressRegister{ea.reg}, ea.address = address);
    //a faulting long write (MOVE.l) leaves An at its original value
    faultUpdate = {true, ea.reg, Size == Long ? address + 4 : address};
    write<Size, Reverse>(address, data);
    faultUpdate.pending = false;
    return;
  }

  case AddressRegisterIndirectWithDisplacement: {
    return write<Size>(ea.address, data);
  }

  case AddressRegisterIndirectWithIndex: {
    return write<Size>(ea.address, data);
  }

  case AbsoluteShortIndirect: {
    return write<Size>(ea.address, data);
  }

  case AbsoluteLongIndirect: {
    return write<Size>(ea.address, data);
  }

  case ProgramCounterIndirectWithDisplacement: {
    return write<Size>(ea.address, data);
  }

  case ProgramCounterIndirectWithIndex: {
    return write<Size>(ea.address, data);
  }

  case Immediate: {
    return;
  }

  }
}
