# Social posts: verifying the emulator, and why it matters for decomp/recomp

Drafts for LinkedIn, X and Reddit about the fork's verification work. The figures come from
[docs/research](../research/) as of the SPC700/Z80 work (PR #6).

**Before posting:**
- Fill in the `[bracketed placeholders]`: the recomp project's name, and a line on what has
  actually been decompiled or recompiled.
- "Formal verification" here means differential testing against SingleStepTests plus TLA+ model
  checking of specific state machines, not end-to-end proofs. Keep the wording that way.
- The Z80 numbers depend on PR #6. Wait until it's merged before linking them.
- r/EmuDev and r/emulation have self-promotion rules. Keep the caveats section in the Reddit
  post.

---

## LinkedIn

**Using verification to make an emulator a trustworthy oracle**

I've been running an experiment: take a mature multi-system emulator (MesenCE, a community fork
of Mesen2), treat it as an independent project, and see how far LLM-assisted development can go
when every change has to come with evidence.

The rule is simple. No fix goes in without a reproduction, a source for the intended behaviour,
a regression test wired into the build, and a written findings log. That includes the results
that turn out to be "not a bug".

So far that has meant:
• NES 6502: 2.56M per-cycle tests against the SingleStepTests suite
• Game Boy SM83: 500K tests, plus TLA+ model checking of the interrupt/HALT/EI state machine
• SNES SPC700: 251K tests
• Master System / Game Gear Z80: 1.6M tests, now passing 100%, checking bus traces, T-state
  timing and the hidden WZ/Q registers

It found real bugs:
• an OAM DMA restart bug on NES
• an EI;HALT interrupt-return bug on Game Boy
• six Z80 instructions with the wrong cycle count, some of them very common ones
• a debugger crash on the SNES sound CPU

None of these were exotic. They were the kind of thing nobody finds until something independent
checks every instruction.

**Why I care: my decompilation and recompilation work.**

When you decompile or statically recompile a game, you need an oracle: something that tells you,
instruction by instruction and cycle by cycle, what the original hardware would have done. If the
oracle is wrong, you spend days chasing a "bug" in your recompiled code that is really a bug in
your reference.

A verified emulator changes that:
→ Differential testing: run the original in the emulator and the recompiled build side by side,
  and compare state traces. A divergence now points at my code, not the reference.
→ CPU semantics as a spec: the verified instruction behaviour (flags, timing, bus order) is
  exactly what a lifter or recompiler has to reproduce.
→ Faster iteration: harnesses that already drive the CPU headlessly can also drive
  [my recomp project] comparisons.
→ Confidence in edge cases: undocumented flags and timing quirks are where recompiled games
  break in subtle ways.

The bigger point: LLMs make it cheap to write code, so verification is now the bottleneck. Cheap
code plus an expensive standard of proof is a good trade.

Repo and findings logs: https://github.com/bgyss/MesenCE

#emulation #reverseengineering #formalmethods #softwaretesting #AI

---

## X (thread)

**1/**
I've been turning an emulator into a verified oracle for my decomp/recomp work.

MesenCE (a Mesen2 fork), LLM-assisted, with one rule: no fix without a repro, a source, a
regression test and a findings log. 🧵

**2/**
CPU cores checked against the SingleStepTests suites:
• NES 6502: 2.56M tests
• GB SM83: 500K
• SNES SPC700: 251K
• SMS/GG Z80: 1.6M, now 100%

Checked per instruction: bus trace, cycle timing and every register, including the hidden ones
(WZ, Q).

**3/**
It found real bugs:
• Z80 LD (HL),n took 12 T-states instead of 10, and INC (HL) took 10 instead of 11
• NES OAM DMA restarting mid-transfer
• Game Boy EI;HALT returning past the HALT
• a SNES debugger crash on SLEEP/STOP

**4/**
Where tests aren't enough, there's TLA+. The NES DMA scheduler and the GB interrupt/HALT logic
are model-checked against the documented rules, and the pre-fix configs are kept so the bug stays
reproducible.

**5/**
Why it matters for decomp/recomp: you need an oracle you can trust. With a verified reference, a
trace divergence points at your recompiled code, not at the emulator.

**6/**
Next up is wiring the harnesses into [recomp project]: running the original and the recompiled
build in lockstep and diffing state each frame.

LLMs made code cheap. Verification is where the work is now.

github.com/bgyss/MesenCE

---

## Reddit (r/EmuDev, or r/emulation with the more technical paragraphs trimmed)

**Title:** Checking Mesen2's CPU cores against SingleStepTests: 4 CPUs, ~5M tests, and the bugs
it found (Z80 timing, NES DMA, GB EI;HALT)

**Body:**

I maintain an independent fork of Mesen2 (MesenCE). I'm using it as a testbed for LLM-assisted
development where every change has to come with evidence: a repro, a source for the correct
behaviour, a regression test, and an entry in a findings log. I record retractions and "not a
bug" results too.

**Method**
- Differential testing of each CPU core against the SingleStepTests JSON suites. I check the bus
  trace (address, value, direction and order), the cycle or T-state count, all registers, and
  RAM.
- For the Z80, the harness compiles the real `SmsCpu.cpp` against a flat test bus, so it tests
  the shipping code rather than the debugger's dummy CPU.
- TLA+ model checking for state machines that tests cover badly: NES DMC/OAM DMA scheduling, and
  Game Boy IME/EI/HALT.
- A save-state round trip into a fresh console with per-frame comparison, which catches state
  missing from serialization.

**Results**
- NES 6502: 2.56M tests. The divergences are documented; LXA's magic constant is a deliberate
  choice.
- GB SM83: 498K/500K. STOP and HALT are bookkeeping divergences, not bugs.
- SNES SPC700: 249.5K/251.5K. I fixed the BRK/DAA/DAS dummy reads and a `DummySpc` null
  dereference on SLEEP/STOP that the debugger's predictive breakpoints could trigger.
- Z80: 1.418M → 1.604M/1.604M. The fixes:
  - timing: `LD (HL),n` 12→10, `LD (IX+d),n` 24→19, `INC/DEC (HL)` 10→11, `RLD` 14→18, and the
    DD/FD CB BIT aliases 19→20;
  - MEMPTR updates on JR, JP nn and (IX+d) operands;
  - block I/O flags (the Banks repeat rules tested S instead of N);
  - Q being reset by DD/FD prefixes.

**Caveats, because this sub will ask**
- SingleStepTests is generated from emulators (ares-derived), not captured from hardware. Where
  the suite disagrees with docs I cite the source, and where it's a judgment call I say so.
- MesenTests has no SMS tests, so the Z80 timing fixes haven't been run against any game or test
  ROM yet. If you have timing-sensitive SMS test ROMs, I'd love pointers.
- "Formal" means model checking specific state machines, not proving the emulator correct.

**Why I'm doing this**

I do decompilation and static recompilation work ([briefly describe yours]). The biggest time
sink there is not knowing whether a divergence is in my output or in the reference. A reference
whose CPU behaviour has been checked instruction by instruction makes lockstep trace diffing
practical: when they disagree, I know where to look.

Findings logs, with the method, the repros and each fix's source, are in `docs/research/` in the
repo: https://github.com/bgyss/MesenCE

Credit to Sour and the Mesen2 contributors for the emulator, and to the SingleStepTests
maintainers for the suites.
