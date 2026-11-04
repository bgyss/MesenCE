--------------------------- MODULE NesDmaScheduler ---------------------------
(***************************************************************************)
(* Cycle-level model of the NES (2A03) DMA scheduler as MesenCE implements *)
(* it.  This is a model of the *code*, not of the hardware:                *)
(*                                                                         *)
(*   NesCpu::MemoryRead / MemoryWrite      Core/NES/NesCpu.cpp:245-270      *)
(*   NesCpu::ProcessPendingDma             Core/NES/NesCpu.cpp:329-430      *)
(*   NesCpu::RunDMATransfer ($4014 write)  Core/NES/NesCpu.cpp (end)        *)
(*   NesCpu::StartDmcTransfer/StopDmcTransfer                               *)
(*   DeltaModulationChannel::ProcessClock / SetDmcReadBuffer (APU side)     *)
(*                                                                         *)
(* One TLA+ step is one CPU cycle.  Bus values are abstracted away; what   *)
(* is kept is the scheduling state: the halt/dummy/abort flags, the        *)
(* get/put parity, the OAM byte counter, and what kind of cycle ran.       *)
(*                                                                         *)
(* The APU and the program are the environment.  They may raise or cancel  *)
(* a DMC DMA on any cycle (APU work runs inside StartCpuCycle), and start  *)
(* an OAM DMA on a CPU write cycle (a write to $4014).  MaxEnvEvents       *)
(* bounds how many such events one behaviour may contain, which keeps the  *)
(* state space finite so TLC can also check liveness.                      *)
(*                                                                         *)
(* Status: a first sketch.  Every action cites the C++ it mirrors; check  *)
(* those mappings before trusting a result.                               *)
(***************************************************************************)
EXTENDS Naturals

CONSTANTS
    OamBytes,             \* bytes per OAM DMA: 256 on hardware, small for TLC
    MaxEnvEvents,         \* bound on DMA start/stop events per behaviour
    MaxCpuWrites,         \* longest run of CPU write cycles (3: BRK/IRQ push)
    MaxInstrCycles,       \* longest gap between opcode fetches (PAL halt rule)
    Pal,                  \* TRUE: DMA may only halt the CPU on an opcode fetch
    DmcStartOnlyWhenIdle, \* TRUE: APU never re-requests DMC DMA while pending
    DuplicationGlitch,    \* EnableDmcSampleDuplicationGlitch (off by default):
                          \* SetDmcReadBuffer may request a new DMC DMA right
                          \* after a DMC read (DeltaModulationChannel.cpp:96-110)
    RmwOn4014,            \* FALSE: $4014 is only written by STA/STX/STY,
                          \* whose write is the instruction's last cycle, so
                          \* an opcode fetch follows.  TRUE also allows
                          \* read-modify-write instructions (INC $4014, ...):
                          \* their dummy write to $4014 is followed by a
                          \* final write to $4014, which calls RunDMATransfer
                          \* again, and then an opcode fetch.
    OamProgressPersists   \* FALSE: the code as of 4b8a196e, where the OAM DMA
                          \* counter is a local of ProcessPendingDma and a
                          \* nested call restarts it.  TRUE: the proposed
                          \* hardening, where it is CPU state reset only by
                          \* RunDMATransfer.

VARIABLE s

Modes  == {"cpu", "halt", "dma", "retry"}
       \* cpu   : CPU running normally
       \* halt  : next cycle is the halt cycle of a nested ProcessPendingDma
       \*         call (the NoInlineProcessPendingDma recursion)
       \* dma   : inside the while(_dmcDmaRunning || _spriteDmaTransfer) loop
       \* retry : DMA finished; the read it interrupted now runs
       \*         (without calling ProcessPendingDma again)

Kinds == {"init", "cpuRead", "cpuWrite", "halt", "dummy", "align",
          "dmcRead", "oamRead", "oamWrite"}

NoEpisode == [len |-> 0, dmc |-> FALSE, oam |-> FALSE, oamWrites |-> 0,
              dmcReads |-> 0]

TypeOK ==
    /\ s.mode \in Modes
    /\ s.par \in {0, 1}               \* _state.CycleCount & 1 (0 = get cycle)
    /\ s.needHalt \in BOOLEAN
    /\ s.needDummyRead \in BOOLEAN
    /\ s.dmcRunning \in BOOLEAN
    /\ s.abortDmc \in BOOLEAN
    /\ s.spriteDma \in BOOLEAN
    /\ s.oamCount \in 0..(2 * OamBytes)  \* local spriteDmaCounter
    /\ s.writeRun \in 0..MaxCpuWrites
    /\ s.after4014 \in {"none", "fetch", "rmwFinal"}  \* what must follow a
                                                    \* $4014 write (see above)
    /\ s.sinceFetch \in 0..MaxInstrCycles
    /\ s.budget \in 0..MaxEnvEvents
    /\ s.kind \in Kinds
    /\ s.inEp \in BOOLEAN
    /\ s.ep \in [len : Nat, dmc : BOOLEAN, oam : BOOLEAN, oamWrites : Nat,
                 dmcReads : Nat]
    /\ s.last \in [len : Nat, dmc : BOOLEAN, oam : BOOLEAN, oamWrites : Nat,
                   dmcReads : Nat]

Init ==
    s = [ mode |-> "cpu", par |-> 0,
          needHalt |-> FALSE, needDummyRead |-> FALSE, dmcRunning |-> FALSE,
          abortDmc |-> FALSE, spriteDma |-> FALSE, oamCount |-> 0,
          writeRun |-> 0, after4014 |-> "none", sinceFetch |-> 0, budget |-> MaxEnvEvents,
          kind |-> "init", inEp |-> FALSE, ep |-> NoEpisode, last |-> NoEpisode ]

-----------------------------------------------------------------------------
(* Environment: events raised during StartCpuCycle of a cycle.            *)

Events == {"none", "dmcStart", "dmcStop", "oamStart"}

EventOK(st, ev, isCpuWrite) ==
    CASE ev = "none"     -> TRUE
      [] ev = "dmcStart" -> st.budget > 0
                            /\ (~DmcStartOnlyWhenIdle \/ ~st.dmcRunning)
      [] ev = "dmcStop"  -> st.budget > 0 /\ st.dmcRunning
      [] ev = "oamStart" -> st.budget > 0 /\ isCpuWrite /\ ~st.spriteDma

MarkDmc(st) == IF st.inEp THEN [st.ep EXCEPT !.dmc = TRUE] ELSE st.ep

\* NesCpu::StartDmcTransfer
DmcStart(st) == [st EXCEPT !.dmcRunning = TRUE, !.needDummyRead = TRUE,
                           !.needHalt = TRUE, !.budget = @ - 1, !.ep = MarkDmc(st)]

\* NesCpu::StopDmcTransfer
DmcStop(st) ==
    LET st1 == [st EXCEPT !.budget = @ - 1, !.ep = MarkDmc(st)] IN
    IF st.needHalt
    THEN [st1 EXCEPT !.dmcRunning = FALSE, !.needDummyRead = FALSE,
                     !.needHalt = FALSE]
    ELSE [st1 EXCEPT !.abortDmc = TRUE]

\* NesCpu::RunDMATransfer (the $4014 write)
OamStart(st) == [st EXCEPT !.spriteDma = TRUE, !.needHalt = TRUE,
                           !.budget = @ - 1,
                           !.oamCount = IF OamProgressPersists THEN 0 ELSE @]

ApplyEnv(st, ev) ==
    CASE ev = "none"     -> st
      [] ev = "dmcStart" -> DmcStart(st)
      [] ev = "dmcStop"  -> DmcStop(st)
      [] ev = "oamStart" -> OamStart(st)

-----------------------------------------------------------------------------
(* Helpers                                                                *)

Tick(st, k) ==       \* StartCpuCycle/EndCpuCycle bookkeeping for one cycle
    [st EXCEPT !.par = 1 - @, !.kind = k,
               !.ep = IF st.inEp THEN [@ EXCEPT !.len = @ + 1] ELSE @]

\* The processCycle lambda in ProcessPendingDma (runs before StartCpuCycle)
ProcessCycle(st) ==
    IF st.abortDmc
    THEN [st EXCEPT !.dmcRunning = FALSE, !.abortDmc = FALSE,
                    !.needDummyRead = FALSE, !.needHalt = FALSE]
    ELSE IF st.needHalt THEN [st EXCEPT !.needHalt = FALSE]
    ELSE IF st.needDummyRead THEN [st EXCEPT !.needDummyRead = FALSE]
    ELSE st

\* Loop condition while(_dmcDmaRunning || _spriteDmaTransfer)
AfterDmaCycle(st) ==
    IF st.dmcRunning \/ st.spriteDma
    THEN [st EXCEPT !.mode = "dma"]
    ELSE [st EXCEPT !.mode = "retry", !.inEp = FALSE, !.last = st.ep]

\* The halt cycle at the top of ProcessPendingDma, cpp:343-360.
\* In the current code each (re)entry restarts the local spriteDmaCounter.
HaltCycle(st, ev) ==
    LET st1 == [st EXCEPT !.needHalt = FALSE]
        st2 == Tick(ApplyEnv(st1, ev), "halt")
        st3 == IF OamProgressPersists THEN st2 ELSE [st2 EXCEPT !.oamCount = 0]
    IN IF st3.abortDmc
       THEN LET st4 == [st3 EXCEPT !.dmcRunning = FALSE, !.abortDmc = FALSE]
            IN IF ~st4.spriteDma
               THEN \* early return: DMA cancelled, nothing else to do
                    [st4 EXCEPT !.needDummyRead = FALSE, !.mode = "retry",
                                !.inEp = FALSE, !.last = st4.ep]
               ELSE AfterDmaCycle(st4)
       ELSE AfterDmaCycle(st3)

DmaAllowed(isFetch) == ~Pal \/ isFetch       \* cpp:335-339

-----------------------------------------------------------------------------
(* Actions                                                                *)

\* A normal CPU read.  If DMA is pending, the halt cycle replaces it.
CpuRead ==
    /\ s.mode = "cpu"
    /\ \E isFetch \in BOOLEAN, ev \in Events :
        /\ isFetch \/ s.sinceFetch < MaxInstrCycles
        /\ s.after4014 # "rmwFinal"
        /\ s.after4014 = "fetch" => isFetch   \* opcode fetch after a $4014 write
        /\ EventOK(s, ev, FALSE)
        /\ LET st0 == [s EXCEPT !.writeRun = 0, !.after4014 = "none",
                                !.sinceFetch = IF isFetch THEN 0 ELSE @ + 1]
           IN IF s.needHalt /\ DmaAllowed(isFetch)
              THEN s' = HaltCycle([st0 EXCEPT !.inEp = TRUE,
                                     !.ep = [NoEpisode EXCEPT !.oam = s.spriteDma,
                                                              !.dmc = s.dmcRunning]],
                                  ev)
              ELSE s' = Tick(ApplyEnv(st0, ev), "cpuRead")

\* A CPU write cycle: never halted (MemoryWrite skips ProcessPendingDma).
\* If the write starts an OAM DMA it is either the last cycle of STA $4014
\* (an opcode fetch follows) or, with RmwOn4014, the dummy write of an RMW
\* instruction on $4014 (the final write to $4014 follows).
CpuWrite ==
    /\ s.mode = "cpu"
    /\ s.writeRun < MaxCpuWrites
    /\ s.sinceFetch < MaxInstrCycles
    /\ s.after4014 = "none"
    /\ \E ev \in Events, rmw \in BOOLEAN :
        /\ EventOK(s, ev, TRUE)
        /\ rmw => (RmwOn4014 /\ ev = "oamStart"
                   \* room for the final write that must follow
                   /\ s.writeRun + 2 <= MaxCpuWrites
                   /\ s.sinceFetch + 2 <= MaxInstrCycles)
        /\ s' = Tick(ApplyEnv([s EXCEPT !.writeRun = @ + 1,
                                        !.sinceFetch = @ + 1,
                                        !.after4014 =
                                            IF ev # "oamStart" THEN "none"
                                            ELSE IF rmw THEN "rmwFinal"
                                            ELSE "fetch"],
                               ev), "cpuWrite")

\* Final write of an RMW instruction on $4014.  Within the cycle the APU runs
\* first (StartCpuCycle) and may raise/cancel a DMC DMA; then the write calls
\* RunDMATransfer again, re-arming _needHalt.  An opcode fetch follows.
RmwFinalWrite ==
    /\ s.mode = "cpu"
    /\ s.after4014 = "rmwFinal"
    /\ \E ev \in {"none", "dmcStart", "dmcStop"} :
        /\ EventOK(s, ev, TRUE)
        /\ LET st1 == ApplyEnv([s EXCEPT !.writeRun = @ + 1,
                                          !.sinceFetch = @ + 1,
                                          !.after4014 = "fetch"], ev)
           IN s' = Tick([st1 EXCEPT !.spriteDma = TRUE, !.needHalt = TRUE,
                                    !.oamCount = IF OamProgressPersists
                                                 THEN 0 ELSE @],
                        "cpuWrite")

\* The read that was interrupted by DMA finally runs (MemoryRead, after
\* ProcessPendingDma returns).  It does not re-check _needHalt.
Retry ==
    /\ s.mode = "retry"
    /\ \E ev \in Events :
        /\ EventOK(s, ev, FALSE)
        /\ s' = [Tick(ApplyEnv(s, ev), "cpuRead") EXCEPT !.mode = "cpu"]

\* Halt cycle of the nested NoInlineProcessPendingDma call (cpp:398-400).
NestedHalt ==
    /\ s.mode = "halt"
    /\ \E ev \in Events :
        /\ EventOK(s, ev, FALSE)
        /\ s' = HaltCycle(s, ev)

\* One iteration of the DMA loop, cpp:381-428.
DmaCycle ==
    /\ s.mode = "dma"
    /\ \E ev \in Events :
        /\ EventOK(s, ev, FALSE)
        /\ LET get == s.par = 0 IN
           IF get /\ s.dmcRunning /\ ~s.needHalt /\ ~s.needDummyRead
           THEN \* DMC byte read, then SetDmcReadBuffer
                \E dup \in BOOLEAN :
                  /\ dup => (DuplicationGlitch
                             /\ s.budget > (IF ev = "none" THEN 0 ELSE 1))
                  /\ LET st1 == Tick(ApplyEnv(ProcessCycle(s), ev), "dmcRead")
                         st2 == [st1 EXCEPT !.dmcRunning = FALSE, !.abortDmc = FALSE,
                                            !.ep.dmcReads = @ + 1]
                         st3 == IF dup THEN DmcStart(st2) ELSE st2
                     IN s' = IF st3.needHalt
                             THEN [st3 EXCEPT !.mode = "halt"]   \* recursion
                             ELSE AfterDmaCycle(st3)
           ELSE IF get /\ s.spriteDma
           THEN s' = AfterDmaCycle(
                        [Tick(ApplyEnv(ProcessCycle(s), ev), "oamRead")
                            EXCEPT !.oamCount = @ + 1])
           ELSE IF get
           THEN s' = AfterDmaCycle(Tick(ApplyEnv(ProcessCycle(s), ev), "dummy"))
           ELSE IF s.spriteDma /\ s.oamCount % 2 = 1
           THEN LET st1 == Tick(ApplyEnv(ProcessCycle(s), ev), "oamWrite")
                    st2 == [st1 EXCEPT !.oamCount = @ + 1,
                                       !.ep.oamWrites = @ + 1]
                    st3 == IF st2.oamCount = 2 * OamBytes
                           THEN [st2 EXCEPT !.spriteDma = FALSE] ELSE st2
                IN s' = AfterDmaCycle(st3)
           ELSE s' = AfterDmaCycle(Tick(ApplyEnv(ProcessCycle(s), ev), "align"))

Next == CpuRead \/ CpuWrite \/ RmwFinalWrite \/ Retry \/ NestedHalt \/ DmaCycle

Spec == Init /\ [][Next]_s /\ WF_s(Next)

-----------------------------------------------------------------------------
(* Properties                                                             *)

\* The C++ assert in the dummy-read branch (cpp:410): if the loop falls
\* through to a plain dummy read on a get cycle, a halt or dummy read must
\* actually be owed.
DummyReadJustified ==
    (s.mode = "dma" /\ s.par = 0 /\ ~s.spriteDma
        /\ ~(s.dmcRunning /\ ~s.needHalt /\ ~s.needDummyRead))
    => (s.needHalt \/ s.needDummyRead)

\* Each OAM DMA writes exactly OamBytes bytes to $2004.
OamWritesExact ==
    (s.mode = "retry" /\ s.last.oam) => s.last.oamWrites = OamBytes

\* OAM DMA with no DMC activity takes 2*OamBytes + 1 or + 2 cycles
\* (513 or 514 for 256 bytes, per the NESdev wiki DMA page).
OamOnlyLength ==
    (s.mode = "retry" /\ s.last.oam /\ ~s.last.dmc)
    => s.last.len \in {2 * OamBytes + 1, 2 * OamBytes + 2}

\* A DMC-only DMA that runs to completion takes 3 or 4 cycles
\* (halt, dummy, optional alignment, get), per the NESdev wiki.
\* Cancelled episodes and back-to-back DMC fetches (the duplication glitch)
\* are excluded.
DmcOnlyLength ==
    (s.mode = "retry" /\ s.last.dmc /\ ~s.last.oam /\ s.kind = "dmcRead"
        /\ s.last.dmcReads = 1)
    => s.last.len \in {3, 4}

\* Candidate invariant: no DMC bookkeeping is left behind once DMC DMA
\* is no longer running.  A violation is a question, not necessarily a bug.
NoStaleDmcFlags ==
    (s.mode = "cpu" /\ ~s.dmcRunning) => (~s.needDummyRead /\ ~s.abortDmc)

\* Liveness: pending DMA always finishes and the CPU always gets the bus back.
DmcFinishes == [](s.dmcRunning ~> ~s.dmcRunning)
OamFinishes == [](s.spriteDma ~> ~s.spriteDma)
CpuResumes  == [](s.mode = "dma" ~> s.mode = "cpu")
=============================================================================
