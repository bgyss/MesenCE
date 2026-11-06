; sa1_timing.s - self-checking SA-1 CPU timing test.
;
; Measures how many SA-1 cycles single instructions take, using the SNES CPU's
; PPU V counter as the clock, and prints expected vs measured for each. Covers the
; SA-1 vector-fetch fix (docs/research/snes-verification-findings.md #12) and the
; SA-1 memory speeds.
;
; Method:
;   - The SNES CPU runs its timing loop from WRAM ($00:1000), so it never accesses
;     ROM or I-RAM while the SA-1 is measured (no SNES/SA-1 bus conflicts).
;   - It waits for the start of line 0 (V-IRQ with VTIME = 0, I flag set, WAI),
;     then sends "go" with the SA-1 message register (CCNT $2200).
;   - The SA-1 runs REPS = 1364 unrolled copies of the instruction from ROM bank
;     1, then answers through SCNT ($2209).
;   - The SNES CPU latches the V counter. A line is 1364 master clocks = 682 SA-1
;     cycles, so 1364 copies of an X-cycle instruction take exactly 2*X lines, and
;     the handshake overhead is far below a line: X = lines / 2, exactly.
;
; Expected values: WDC W65C816S cycle counts, plus 1 cycle per BW-RAM access
; (BW-RAM runs at half the SA-1 clock). BRK/COP are measured together with the RTI
; of their handler; MesenCE adds 1 cycle to returns into PRG ROM (its SA-1 model,
; upstream fe470dd8), which is included in the expected value and is itself
; something a hardware run should confirm.
;
; Build: mise run build:test-roms (ca65/ld65, sa1_timing.cfg).

.p816
.smart -

.import __MEASURE_LOAD__, __MEASURE_RUN__, __MEASURE_SIZE__

REPS = 1364

; SNES CPU registers
INIDISP = $2100
BGMODE  = $2105
BG1SC   = $2107
BG12NBA = $210B
BG1HOFS = $210D
BG1VOFS = $210E
VMAIN   = $2115
VMADDL  = $2116
VMDATAL = $2118
VMDATAH = $2119
CGADD   = $2121
CGDATA  = $2122
TM      = $212C
SLHV    = $2137
OPVCT   = $213D
STAT78  = $213F
NMITIMEN = $4200
WRIO    = $4201
VTIMEL  = $4209
VTIMEH  = $420A
TIMEUP  = $4211

; SA-1 registers
CCNT  = $2200   ; SNES -> SA-1 control (message in bits 0-3)
CRV   = $2203   ; SA-1 reset vector
SCNT  = $2209   ; SA-1 -> SNES control (message in bits 0-3)
CXB   = $2220
DXB   = $2221
EXB   = $2222
FXB   = $2223
BMAPS = $2224
SBWE  = $2226
CBWE  = $2227
SIWP  = $2229
CIWP  = $222A
SFR   = $2300   ; SNES reads the SA-1's message
CFR   = $2301   ; SA-1 reads the SNES's message

; Direct page variables (WRAM $0000)
Result = $00    ; 16-bit line count
Got    = $02
Exp    = $03
Fails  = $04
Attr   = $05    ; tilemap attribute byte (palette << 2)
Row    = $06
NamePtr = $08   ; 16-bit

.segment "CODE"

; ---------------------------------------------------------------------------
; SNES CPU

Reset:
	sei
	clc
	xce
	rep #$38
	.a16
	.i16
	ldx #$1FFF
	txs
	lda #$0000
	tcd
	sep #$20
	.a8
	lda #$00
	pha
	plb
	lda #$80
	sta INIDISP
	stz NMITIMEN
	lda #$FF
	sta WRIO

	; SA-1 setup: keep it in reset, set its reset vector and memory map
	lda #$20
	sta CCNT
	rep #$20
	.a16
	lda #.loword(Sa1Entry)
	sta CRV
	sep #$20
	.a8
	stz CXB
	lda #$01
	sta DXB
	lda #$02
	sta EXB
	lda #$03
	sta FXB
	stz BMAPS
	lda #$80
	sta SBWE
	lda #$FF
	sta SIWP

	; Copy the timing routine to WRAM
	ldx #$0000
@copy:
	lda f:__MEASURE_LOAD__,x
	sta f:__MEASURE_RUN__,x
	inx
	cpx #__MEASURE_SIZE__
	bne @copy

	jsr InitVideo

	; Release the SA-1 (reset bit clear, message 0)
	stz CCNT

	stz Fails
	lda #4
	sta Row

	.macro RUNTEST num, expected, name
	lda #num
	jsr Measure
	rep #$20
	.a16
	lda Result
	lsr a
	sep #$20
	.a8
	sta Got
	lda #expected
	sta Exp
	ldx #name
	stx NamePtr
	jsr ShowRow
	.endmacro

	RUNTEST 1, 2, N_Nop
	RUNTEST 2, 4, N_LdaRom
	RUNTEST 3, 4, N_LdaIram
	RUNTEST 4, 6, N_LdaBwram
	RUNTEST 5, 16, N_Brk
	RUNTEST 6, 16, N_Cop

	; Summary
	inc Row
	ldy #1
	jsr SetPos
	lda Fails
	bne @failed
	lda #(1 << 2)
	sta Attr
	ldx #StrAllPass
	jsr PrintStr
	bra @show
@failed:
	lda #(2 << 2)
	sta Attr
	lda Fails
	jsr PrintDec2
	ldx #StrFailed
	jsr PrintStr
@show:
	lda #$0F
	sta INIDISP
@forever:
	bra @forever

; Prints one result row: NamePtr, Exp, Got at Row
ShowRow:
	stz Attr
	ldy #1
	jsr SetPos
	ldx NamePtr
	jsr PrintStr
	ldy #18
	jsr SetPos
	lda Exp
	jsr PrintDec2
	ldy #22
	jsr SetPos
	lda Got
	jsr PrintDec2
	ldy #26
	jsr SetPos
	lda Got
	cmp Exp
	bne @ng
	lda #(1 << 2)
	sta Attr
	ldx #StrOk
	jsr PrintStr
	bra @next
@ng:
	lda #(2 << 2)
	sta Attr
	ldx #StrNg
	jsr PrintStr
	inc Fails
@next:
	inc Row
	rts

; Y = column: set the VRAM address to (Row, column) in the BG1 tilemap at $0400
SetPos:
	rep #$20
	.a16
	lda Row
	and #$00FF
	asl a
	asl a
	asl a
	asl a
	asl a
	sty Result
	clc
	adc Result
	adc #$0400
	sta VMADDL
	sep #$20
	.a8
	rts

; X = zero-terminated string (bank 0), Attr = attribute byte
PrintStr:
	lda a:$0000,x
	beq @done
	sec
	sbc #$20
	sta VMDATAL
	lda Attr
	sta VMDATAH
	inx
	bra PrintStr
@done:
	rts

; A = 0-99, printed as two digits
PrintDec2:
	ldx #$0000
@tens:
	cmp #10
	bcc @ones
	sbc #10
	inx
	bra @tens
@ones:
	pha
	txa
	clc
	adc #$10            ; '0' - $20
	sta VMDATAL
	lda Attr
	sta VMDATAH
	pla
	clc
	adc #$10
	sta VMDATAL
	lda Attr
	sta VMDATAH
	rts

InitVideo:
	; Clear VRAM
	lda #$80
	sta VMAIN
	rep #$20
	.a16
	stz VMADDL
	ldx #$8000
@clear:
	stz VMDATAL
	dex
	bne @clear
	; Font: 2bpp tiles at word $0000, plane 0 only (colour 1 of each palette)
	stz VMADDL
	ldx #$0000
@font:
	lda f:Font,x
	and #$00FF
	sta VMDATAL
	inx
	cpx #(FontEnd - Font)
	bne @font
	sep #$20
	.a8
	; Palettes: 0 = white text, 1 = green, 2 = red (colour 1 of each)
	stz CGADD
	ldx #$0000
@pal:
	lda f:Palette,x
	sta CGDATA
	inx
	cpx #(PaletteEnd - Palette)
	bne @pal
	stz BGMODE
	lda #$04            ; BG1 tilemap at word $0400
	sta BG1SC
	stz BG12NBA         ; BG1 tiles at word $0000
	stz BG1HOFS
	stz BG1HOFS
	stz BG1VOFS
	stz BG1VOFS
	lda #$01
	sta TM
	; Titles
	stz Attr
	lda #1
	sta Row
	ldy #1
	jsr SetPos
	ldx #StrTitle
	jsr PrintStr
	lda #2
	sta Row
	ldy #1
	jsr SetPos
	ldx #StrHeader
	jsr PrintStr
	rts

; ---------------------------------------------------------------------------
; SA-1 CPU (starts at CRV)

Sa1Entry:
	sei
	clc
	xce
	rep #$38
	.a16
	.i16
	lda #$37FF          ; stack at the top of I-RAM
	tcs
	lda #$0000
	tcd
	sep #$20
	.a8
	lda #$FF
	sta CIWP            ; allow SA-1 writes to all of I-RAM (the stack)
	lda #$80
	sta CBWE            ; and to BW-RAM
Sa1Loop:
	lda CFR
	and #$0F
	beq Sa1Loop
	dec a
	asl a
	rep #$30
	.a16
	and #$00FF
	tax
	sep #$20
	.a8
	jmp (Sa1Table,x)
Sa1Done:
	lda #$01
	sta SCNT
@ack:
	lda CFR
	and #$0F
	bne @ack
	stz SCNT
	bra Sa1Loop

Sa1Table:
	.addr J_Nop, J_LdaRom, J_LdaIram, J_LdaBwram, J_Brk, J_Cop
J_Nop:      jml B_Nop
J_LdaRom:   jml B_LdaRom
J_LdaIram:  jml B_LdaIram
J_LdaBwram: jml B_LdaBwram
J_Brk:      jml B_Brk
J_Cop:      jml B_Cop

BrkHandler:
	rti
CopHandler:
	rti
IgnoreInt:
	rti

; ---------------------------------------------------------------------------
; Data

N_Nop:      .byte "NOP", 0
N_LdaRom:   .byte "LDA ABS (ROM)", 0
N_LdaIram:  .byte "LDA ABS (IRAM)", 0
N_LdaBwram: .byte "LDA LONG (BWRAM)", 0
N_Brk:      .byte "BRK + RTI", 0
N_Cop:      .byte "COP + RTI", 0

StrTitle:   .byte "SA-1 TIMING (SA-1 CYCLES)", 0
StrHeader:  .byte "INSTRUCTION      EXP GOT", 0
StrOk:      .byte "OK", 0
StrNg:      .byte "NG", 0
StrAllPass: .byte "ALL PASS", 0
StrFailed:  .byte " FAILED", 0

Palette:
	.word $0000, $7FFF, $0000, $0000   ; palette 0: white
	.word $0000, $03E0, $0000, $0000   ; palette 1: green
	.word $0000, $001F, $0000, $0000   ; palette 2: red
PaletteEnd:

Font:
.include "font65.inc"
FontEnd:

; ---------------------------------------------------------------------------
; SNES timing routine, copied to WRAM $00:1000. A = test number (1-15).
; Returns the V counter (lines since the start of line 0) in Result.

.segment "MEASURE"
Measure:
	pha
	stz VTIMEL
	stz VTIMEH
	lda TIMEUP          ; clear any pending timer IRQ
	lda #$20            ; V-IRQ only
	sta NMITIMEN
	wai                 ; I is set: WAI ends at the IRQ without taking it
	lda TIMEUP          ; acknowledge
	stz NMITIMEN
	pla
	sta CCNT            ; go
@poll:
	lda SFR
	and #$0F
	beq @poll
	lda STAT78          ; reset the OPVCT read flip-flop
	lda SLHV            ; latch the H/V counters
	lda OPVCT
	xba
	lda OPVCT
	and #$01
	xba
	rep #$20
	.a16
	sta Result
	sep #$20
	.a8
	stz CCNT            ; clear the message
@wait:
	lda SFR
	and #$0F
	bne @wait
	rts

; ---------------------------------------------------------------------------
; SA-1 measured blocks (ROM bank 1)

.segment "BLOCKS"
B_Nop:
.repeat REPS
	nop
.endrepeat
	jml Sa1Done

B_LdaRom:
.repeat REPS
	lda a:$8000
.endrepeat
	jml Sa1Done

B_LdaIram:
.repeat REPS
	lda a:$3000
.endrepeat
	jml Sa1Done

B_LdaBwram:
.repeat REPS
	lda f:$400000
.endrepeat
	jml Sa1Done

B_Brk:
.repeat REPS
	.byte $00, $00      ; BRK #$00
.endrepeat
	jml Sa1Done

B_Cop:
.repeat REPS
	.byte $02, $00      ; COP #$00
.endrepeat
	jml Sa1Done

; ---------------------------------------------------------------------------

.segment "HEADER"
	.byte "SA1 TIMING TEST      "   ; 21 bytes
	.byte $23                       ; map mode: SA-1
	.byte $34                       ; ROM + SA-1 + RAM
	.byte $06                       ; ROM size: 64 KiB
	.byte $03                       ; RAM size: 8 KiB (BW-RAM)
	.byte $01                       ; region
	.byte $33                       ; developer id
	.byte $00                       ; version
	.word $FFFF, $0000              ; checksum complement, checksum (tests/tools/snes_checksum.py)

.segment "VECTORS"
	; Native mode: $FFE0-$FFEF
	.word 0, 0
	.addr CopHandler                ; $FFE4 COP
	.addr BrkHandler                ; $FFE6 BRK
	.addr IgnoreInt                 ; $FFE8 ABORT
	.addr IgnoreInt                 ; $FFEA NMI
	.word 0
	.addr IgnoreInt                 ; $FFEE IRQ
	; Emulation mode: $FFF0-$FFFF
	.word 0, 0
	.addr IgnoreInt                 ; $FFF4 COP
	.word 0
	.addr IgnoreInt                 ; $FFF8 ABORT
	.addr IgnoreInt                 ; $FFFA NMI
	.addr Reset                     ; $FFFC RESET
	.addr IgnoreInt                 ; $FFFE IRQ/BRK
