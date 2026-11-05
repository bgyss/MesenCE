-------------------------- MODULE GbIrqStateMachine --------------------------
(***************************************************************************)
(* Instruction-level model of MesenCE's Game Boy (SM83) interrupt / HALT /  *)
(* EI state machine.  This is a model of the *code*:                        *)
(*                                                                         *)
(*   GbCpu::Exec                   Core/Gameboy/GbCpu.cpp:52-132            *)
(*   GbCpu::ProcessNextCycleStart  GbCpu.cpp:139-171 (samples the IRQ line)  *)
(*   GbCpu::ProcessHaltBug         GbCpu.cpp:173-188                        *)
(*   GbCpu::HALT / EI / DI / RETI  GbCpu.cpp:903-916, 1289-1297, 1242-1247  *)
(*                                                                         *)
(* One step is one GbCpu::Exec() call.  The environment chooses the next    *)
(* instruction (only EI, DI, RETI and HALT matter; everything else is       *)
(* "NOP"), and may raise an interrupt either before the instruction samples *)
(* the IRQ line (so HALT sees it) or after it (so only the end-of-Exec      *)
(* sample sees it).  "pending" abstracts IE & IF != 0 for one source.      *)
(*                                                                         *)
(* The properties are the documented SM83 rules from Pan Docs              *)
(* (Interrupts, halt.html).                                                 *)
(***************************************************************************)
EXTENDS Naturals

CONSTANTS
    MaxRaises,        \* bound on interrupts raised by the environment
    ReturnToHaltFix   \* TRUE: current code, where HALT executed with IME=1 and an
                      \* interrupt pending does not halt and rewinds PC to the HALT
                      \* (as SameBoy does), so the dispatch returns to the HALT.
                      \* FALSE: the code before the fix (6d4e3ec4).

Ops == {"EI", "DI", "RETI", "HALT", "NOP"}
Raise == {"none", "before", "after"}

VARIABLES
    ime, eiPending,        \* GbCpuState.IME / EiPending
    halted, haltBug,       \* HaltCounter > 0 / HaltBug
    pending,               \* IE & IF != 0
    prevIrq,               \* _prevIrqVector != 0 (sampled at the end of Exec)
    pcAtHalt,              \* PC points back at the HALT just executed (fix only)
    repeatOp,              \* halt bug: the op that must be executed again, or "none"
    prevOp, prevPrevOp,    \* last two executed instructions
    haltSawPending,        \* the last HALT executed with an interrupt pending
    dispatched,            \* an interrupt was dispatched in this step
    retToHalt,             \* that dispatch pushed the HALT's address (not the next one)
    ctxEiHalt,             \* that dispatch came right after EI;HALT with IRQ pending at HALT
    raises

vars == << ime, eiPending, halted, haltBug, pending, prevIrq, pcAtHalt, repeatOp,
           prevOp, prevPrevOp, haltSawPending, dispatched, retToHalt, ctxEiHalt, raises >>

TypeOK ==
    /\ ime \in BOOLEAN /\ eiPending \in BOOLEAN
    /\ halted \in BOOLEAN /\ haltBug \in BOOLEAN
    /\ pending \in BOOLEAN /\ prevIrq \in BOOLEAN /\ pcAtHalt \in BOOLEAN
    /\ repeatOp \in Ops \cup {"none"}
    /\ prevOp \in Ops \cup {"none"} /\ prevPrevOp \in Ops \cup {"none"}
    /\ haltSawPending \in BOOLEAN /\ dispatched \in BOOLEAN
    /\ retToHalt \in BOOLEAN /\ ctxEiHalt \in BOOLEAN
    /\ raises \in 0..MaxRaises

Init ==
    /\ ime \in BOOLEAN /\ eiPending = FALSE
    /\ halted = FALSE /\ haltBug = FALSE
    /\ pending \in BOOLEAN /\ prevIrq = pending /\ pcAtHalt = FALSE
    /\ repeatOp = "none" /\ prevOp = "none" /\ prevPrevOp = "none"
    /\ haltSawPending = FALSE /\ dispatched = FALSE
    /\ retToHalt = FALSE /\ ctxEiHalt = FALSE
    /\ raises = MaxRaises

-----------------------------------------------------------------------------
(* Executing one instruction (ExecOpCode).  Returns the new values of the   *)
(* variables it touches as a record.                                        *)

Exec(op, st) ==
    CASE op = "EI"   -> [st EXCEPT !.eiPending = TRUE]
      [] op = "DI"   -> [st EXCEPT !.ime = FALSE]
      [] op = "RETI" -> [st EXCEPT !.ime = TRUE]
      [] op = "HALT" ->
            IF st.ime /\ st.pending /\ ReturnToHaltFix
            THEN [st EXCEPT !.pcAtHalt = TRUE, !.haltSaw = TRUE]          \* proposed fix
            ELSE IF st.ime \/ ~st.pending
            THEN [st EXCEPT !.halted = TRUE, !.haltSaw = st.pending]      \* HaltCounter = 1
            ELSE [st EXCEPT !.halted = TRUE, !.haltBug = TRUE, !.haltSaw = TRUE]
      [] OTHER       -> st

Rec == [ime |-> ime, eiPending |-> eiPending, halted |-> halted, haltBug |-> haltBug,
        pending |-> pending, pcAtHalt |-> pcAtHalt, haltSaw |-> haltSawPending]

Step ==
    \E op \in Ops, r \in Raise :
        /\ r # "none" => raises > 0
        /\ repeatOp # "none" => op = repeatOp            \* halt bug: same byte again
        /\ LET s0 == [Rec EXCEPT !.pending = @ \/ r = "before"]
               \* Exec(): an interrupt sampled at the end of the previous Exec,
               \* unless the halt bug is in progress.
               irqTaken == prevIrq /\ ~haltBug
               s1 == IF irqTaken THEN [s0 EXCEPT !.halted = FALSE] ELSE s0
               doDispatch == irqTaken /\ s1.ime
               \* Dispatch: push PC, clear the IF bit, IME and EiPending.
               s2 == IF doDispatch
                     THEN [s1 EXCEPT !.pending = FALSE, !.ime = FALSE, !.eiPending = FALSE,
                                     !.pcAtHalt = FALSE]
                     ELSE s1
               \* Then: halted (no instruction), halt bug, or a normal instruction.
               runsOp == ~s2.halted \/ s2.haltBug
               s3 == IF ~runsOp THEN s2
                     ELSE LET e == IF s2.eiPending THEN [s2 EXCEPT !.ime = TRUE, !.eiPending = FALSE]
                                   ELSE s2
                              e2 == IF s2.haltBug THEN [e EXCEPT !.halted = FALSE, !.haltBug = FALSE]
                                    ELSE e
                          IN Exec(op, e2)
               s4 == [s3 EXCEPT !.pending = @ \/ r = "after"]
           IN /\ ime' = s4.ime
              /\ eiPending' = s4.eiPending
              /\ halted' = s4.halted
              /\ haltBug' = s4.haltBug
              /\ pending' = s4.pending
              /\ pcAtHalt' = s4.pcAtHalt
              /\ haltSawPending' = s4.haltSaw
              \* ProcessNextCycleStart: sample the IRQ line for the next Exec.
              /\ prevIrq' = s4.pending
              /\ repeatOp' = IF runsOp /\ s2.haltBug THEN op ELSE "none"
              /\ prevOp' = IF runsOp THEN op ELSE prevOp
              /\ prevPrevOp' = IF runsOp THEN prevOp ELSE prevPrevOp
              /\ dispatched' = doDispatch
              /\ retToHalt' = IF doDispatch THEN pcAtHalt ELSE retToHalt
              /\ ctxEiHalt' = IF doDispatch THEN (prevPrevOp = "EI" /\ prevOp = "HALT" /\ haltSawPending)
                              ELSE ctxEiHalt
              /\ raises' = IF r # "none" THEN raises - 1 ELSE raises

Next == Step

Spec == Init /\ [][Next]_vars /\ WF_vars(Next)

-----------------------------------------------------------------------------
(* Documented behaviour (Pan Docs)                                          *)

\* "The effect of ei is delayed by one instruction": when EI enables
\* interrupts (IME was 0), no interrupt is taken between EI and the
\* instruction after it.
EiDelay == [][(prevOp = "EI" /\ ~dispatched /\ ~ime) => ~dispatched']_vars

\* "ei followed immediately by di does not allow any interrupts between them"
\* (EiDelay covers "between"); afterwards IME must be off.
EiDiImeOff == (prevPrevOp = "EI" /\ prevOp = "DI" /\ ~dispatched) => ~ime

\* Dispatch resets IME and the IF bit.
DispatchClearsIme == (dispatched /\ prevOp \notin {"EI", "RETI"}) => ~ime

\* halt.html: "EI immediately before HALT with an interrupt pending: the
\* interrupt is serviced and the handler called, but the interrupt returns to
\* the halt."
EiHaltReturnsToHalt == (dispatched /\ ctxEiHalt) => retToHalt

\* A halted CPU always wakes once an interrupt is pending.  (A halt-bug loop,
\* e.g. HALT;HALT with IME=0 and an interrupt pending, re-executes HALT forever
\* by design: "this can repeat if said byte executes another halt".)
\* One Exec can wake the CPU, dispatch, and run a handler that halts again, so a
\* wake shows up as a dispatch, a non-halted state, or a halt-bug entry.
HaltWakes == [](halted /\ ~haltBug /\ pending ~> (~halted \/ dispatched \/ haltBug))
=============================================================================
