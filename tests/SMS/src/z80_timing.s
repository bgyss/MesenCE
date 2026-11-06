; z80_timing.s - self-checking Z80 instruction timing test for the Sega Master System.
;
; Measures the T-states of single Z80 instructions using the VDP's V counter
; (port $7E) as the clock, and prints expected (Zilog UM0080) vs measured for each.
; Covers the instructions whose timing the fork fixed in SmsCpu (see
; docs/research/sms-verification-findings.md #2-3), plus unchanged controls.
;
; Method: wait for the start of line 0, run REPS = 456 unrolled copies of the
; instruction, and read the V counter. A scanline is 228 T-states (NTSC and PAL),
; so 456 copies of a T-state instruction take exactly 2*T lines. Sync and readout
; overhead is far below one line, so the line count is exactly 2*T and T is
; exact. Interrupts are disabled and the display is off while measuring.
;
; Build: wla-z80 -o z80_timing.o z80_timing.s && wlalink link.txt z80_timing.sms
; (mise run build:test-roms). Result: "ALL PASS" in green, or the number of
; failing rows in red; each row shows OK (green) or NG (red).

.DEFINE VDPDATA $BE
.DEFINE VDPCTRL $BF
.DEFINE VCOUNT  $7E
.DEFINE REPS    456
.DEFINE BUF     $C100       ; scratch byte the instructions under test work on

.MEMORYMAP
DEFAULTSLOT 0
SLOTSIZE $8000
SLOT 0 $0000
.ENDME

.ROMBANKMAP
BANKSTOTAL 1
BANKSIZE $8000
BANKS 1
.ENDRO

.EMPTYFILL $00
.SMSTAG

.ENUM $C000
TestPtr  DW
NamePtr  DW
Row      DB
Fails    DB
Measured DB
Expected DB
.ENDE

.BANK 0 SLOT 0
.ORG $0000
	di
	im 1
	ld sp,$DFF0
	jp Main

.ORG $0038
	reti

.ORG $0066
	retn

.ORG $0100
Main:
	; VDP registers (display off while drawing and measuring)
	ld hl,VdpRegs
	ld b,VdpRegsEnd-VdpRegs
	ld c,VDPCTRL
	otir

	; Clear all 16 KiB of VRAM
	xor a
	out (VDPCTRL),a
	ld a,$40
	out (VDPCTRL),a
	ld bc,$4000
-	xor a
	out (VDPDATA),a
	dec bc
	ld a,b
	or c
	jr nz,-

	; Palette: 0 black, 1 white, 2 green, 3 red
	xor a
	out (VDPCTRL),a
	ld a,$C0
	out (VDPCTRL),a
	ld hl,Palette
	ld b,4
	ld c,VDPDATA
	otir

	; Three copies of the font: tiles 0 (white), 64 (green), 128 (red)
	xor a
	out (VDPCTRL),a
	ld a,$40
	out (VDPCTRL),a
	ld d,0
	call LoadFont
	ld d,1
	call LoadFont
	ld d,2
	call LoadFont

	; No sprites: Y = $D0 terminates the sprite table at $3F00
	xor a
	out (VDPCTRL),a
	ld a,$7F
	out (VDPCTRL),a
	ld a,$D0
	out (VDPDATA),a

	; Titles
	ld de,$0101
	call SetPos
	ld hl,StrTitle
	ld c,0
	call PrintStr
	ld de,$0201
	call SetPos
	ld hl,StrHeader
	ld c,0
	call PrintStr

	; Run the tests
	ld hl,Tests
	ld (TestPtr),hl
	ld a,4
	ld (Row),a
	xor a
	ld (Fails),a

NextTest:
	ld hl,(TestPtr)
	ld e,(hl)
	inc hl
	ld d,(hl)
	inc hl
	ld a,d
	or e
	jp z,Done
	ld a,(hl)
	inc hl
	ld (Expected),a
	ld c,(hl)
	inc hl
	ld b,(hl)
	inc hl
	ld (TestPtr),hl
	ld (NamePtr),bc
	ex de,hl
	call JumpHL             ; returns the elapsed line count in A
	srl a                   ; T = lines / 2
	ld (Measured),a

	ld a,(Row)
	ld d,a
	ld e,1
	call SetPos
	ld hl,(NamePtr)
	ld c,0
	call PrintStr

	ld a,(Row)
	ld d,a
	ld e,18
	call SetPos
	ld a,(Expected)
	ld c,0
	call PrintDec2

	ld a,(Row)
	ld d,a
	ld e,22
	call SetPos
	ld a,(Measured)
	ld c,0
	call PrintDec2

	ld a,(Row)
	ld d,a
	ld e,26
	call SetPos
	ld a,(Measured)
	ld b,a
	ld a,(Expected)
	cp b
	jr nz,@fail
	ld hl,StrOk
	ld c,64
	call PrintStr
	jr @next
@fail:
	ld hl,StrNg
	ld c,128
	call PrintStr
	ld hl,Fails
	inc (hl)
@next:
	ld hl,Row
	inc (hl)
	jp NextTest

Done:
	ld a,(Row)
	inc a
	ld d,a
	ld e,1
	call SetPos
	ld a,(Fails)
	or a
	jr nz,@failed
	ld hl,StrAllPass
	ld c,64
	call PrintStr
	jr @show
@failed:
	ld c,128
	call PrintDec2
	ld hl,StrFailed
	ld c,128
	call PrintStr
@show:
	; Display on, no interrupts
	ld a,$C0
	out (VDPCTRL),a
	ld a,$81
	out (VDPCTRL),a
-	jr -

JumpHL:
	jp (hl)

; Wait for the start of line 0 (V counter changes to 0)
Sync:
-	in a,(VCOUNT)
	or a
	jr z,-
-	in a,(VCOUNT)
	or a
	jr nz,-
	ret

; D = row, E = column: set the VRAM write address to that name table entry
SetPos:
	ld h,0
	ld l,d
	add hl,hl
	add hl,hl
	add hl,hl
	add hl,hl
	add hl,hl
	add hl,hl               ; row * 64
	ld a,e
	add a,a
	add a,l
	ld l,a
	ld a,h
	add a,$78               ; $3800 | $4000 (write)
	ld h,a
	ld a,l
	out (VDPCTRL),a
	ld a,h
	out (VDPCTRL),a
	ret

; HL = zero-terminated string, C = colour (tile base 0, 64 or 128)
PrintStr:
-	ld a,(hl)
	or a
	ret z
	sub $20
	add a,c
	out (VDPDATA),a
	xor a
	out (VDPDATA),a
	inc hl
	jr -

; A = value 0-99, printed as two digits; C = colour
PrintDec2:
	ld b,0
-	cp 10
	jr c,+
	sub 10
	inc b
	jr -
+	ld e,a
	ld a,b
	add a,$10               ; '0' - $20
	add a,c
	out (VDPDATA),a
	xor a
	out (VDPDATA),a
	ld a,e
	add a,$10
	add a,c
	out (VDPDATA),a
	xor a
	out (VDPDATA),a
	ret

; D = 0 white (plane 0), 1 green (plane 1), 2 red (planes 0+1)
LoadFont:
	ld hl,Font
	ld bc,FontEnd-Font
-	ld e,(hl)
	inc hl
	ld a,d
	cp 1
	jr z,@green
	ld a,e
	out (VDPDATA),a
	ld a,d
	or a
	jr z,@zero
	ld a,e
	jr @plane1
@zero:
	xor a
@plane1:
	out (VDPDATA),a
	jr @rest
@green:
	xor a
	out (VDPDATA),a
	ld a,e
	out (VDPDATA),a
@rest:
	xor a
	out (VDPDATA),a
	out (VDPDATA),a
	; 64 tiles per copy: pad 59 glyphs to 64 (5 * 32 bytes) at the end
	dec bc
	ld a,b
	or c
	jr nz,-
	ld b,5*32
-	xor a
	out (VDPDATA),a
	djnz -
	ret

; ---------------------------------------------------------------------------
; Timed routines. Each returns the V counter after REPS copies.

T_Nop:
	call Sync
.REPT REPS
	nop
.ENDR
	in a,(VCOUNT)
	ret

T_LdAHl:
	ld hl,BUF
	call Sync
.REPT REPS
	ld a,(hl)
.ENDR
	in a,(VCOUNT)
	ret

T_LdHlN:
	ld hl,BUF
	call Sync
.REPT REPS
	ld (hl),$00
.ENDR
	in a,(VCOUNT)
	ret

T_IncHl:
	ld hl,BUF
	call Sync
.REPT REPS
	inc (hl)
.ENDR
	in a,(VCOUNT)
	ret

T_DecHl:
	ld hl,BUF
	call Sync
.REPT REPS
	dec (hl)
.ENDR
	in a,(VCOUNT)
	ret

T_Rld:
	ld hl,BUF
	call Sync
.REPT REPS
	rld
.ENDR
	in a,(VCOUNT)
	ret

T_Rrd:
	ld hl,BUF
	call Sync
.REPT REPS
	rrd
.ENDR
	in a,(VCOUNT)
	ret

T_LdIxN:
	ld ix,BUF
	call Sync
.REPT REPS
	ld (ix+0),$00
.ENDR
	in a,(VCOUNT)
	ret

T_IncIx:
	ld ix,BUF
	call Sync
.REPT REPS
	inc (ix+0)
.ENDR
	in a,(VCOUNT)
	ret

T_DecIx:
	ld ix,BUF
	call Sync
.REPT REPS
	dec (ix+0)
.ENDR
	in a,(VCOUNT)
	ret

T_BitIx:
	ld ix,BUF
	call Sync
.REPT REPS
	bit 0,(ix+0)
.ENDR
	in a,(VCOUNT)
	ret

; DD CB d 40: undocumented register form of BIT 0,(IX+d)
T_BitIxAlias:
	ld ix,BUF
	call Sync
.REPT REPS
	.DB $DD, $CB, $00, $40
.ENDR
	in a,(VCOUNT)
	ret

; REPS is even, so the swaps leave the return address in place
T_ExSpHl:
	call Sync
.REPT REPS
	ex (sp),hl
.ENDR
	in a,(VCOUNT)
	ret

T_ExSpIx:
	call Sync
.REPT REPS
	ex (sp),ix
.ENDR
	in a,(VCOUNT)
	ret

; ---------------------------------------------------------------------------
; routine, expected T-states (Zilog UM0080), name

Tests:
	.DW T_Nop
	.DB 4
	.DW N_Nop
	.DW T_LdAHl
	.DB 7
	.DW N_LdAHl
	.DW T_LdHlN
	.DB 10
	.DW N_LdHlN
	.DW T_IncHl
	.DB 11
	.DW N_IncHl
	.DW T_DecHl
	.DB 11
	.DW N_DecHl
	.DW T_Rld
	.DB 18
	.DW N_Rld
	.DW T_Rrd
	.DB 18
	.DW N_Rrd
	.DW T_LdIxN
	.DB 19
	.DW N_LdIxN
	.DW T_IncIx
	.DB 23
	.DW N_IncIx
	.DW T_DecIx
	.DB 23
	.DW N_DecIx
	.DW T_BitIx
	.DB 20
	.DW N_BitIx
	.DW T_BitIxAlias
	.DB 20
	.DW N_BitIxAlias
	.DW T_ExSpHl
	.DB 19
	.DW N_ExSpHl
	.DW T_ExSpIx
	.DB 23
	.DW N_ExSpIx
	.DW 0

N_Nop:        .DB "NOP", 0
N_LdAHl:      .DB "LD A,(HL)", 0
N_LdHlN:      .DB "LD (HL),N", 0
N_IncHl:      .DB "INC (HL)", 0
N_DecHl:      .DB "DEC (HL)", 0
N_Rld:        .DB "RLD", 0
N_Rrd:        .DB "RRD", 0
N_LdIxN:      .DB "LD (IX+D),N", 0
N_IncIx:      .DB "INC (IX+D)", 0
N_DecIx:      .DB "DEC (IX+D)", 0
N_BitIx:      .DB "BIT 0,(IX+D)", 0
N_BitIxAlias: .DB "DDCB D 40", 0
N_ExSpHl:     .DB "EX (SP),HL", 0
N_ExSpIx:     .DB "EX (SP),IX", 0

StrTitle:   .DB "Z80 TIMING (T-STATES)", 0
StrHeader:  .DB "INSTRUCTION      EXP GOT", 0
StrOk:      .DB "OK", 0
StrNg:      .DB "NG", 0
StrAllPass: .DB "ALL PASS", 0
StrFailed:  .DB " FAILED", 0

VdpRegs:
	.DB $04, $80            ; R0: mode 4
	.DB $80, $81            ; R1: display off, no frame interrupt
	.DB $FF, $82            ; R2: name table at $3800
	.DB $FF, $83
	.DB $FF, $84
	.DB $FF, $85            ; R5: sprite table at $3F00
	.DB $FB, $86            ; R6: sprite tiles at $0000
	.DB $00, $87            ; R7: border colour 0
	.DB $00, $88            ; R8: X scroll
	.DB $00, $89            ; R9: Y scroll
	.DB $FF, $8A            ; R10: line counter
VdpRegsEnd:

Palette:
	.DB $00, $3F, $0C, $03  ; black, white, green, red (--BBGGRR)

Font:
.INCLUDE "font.inc"
FontEnd:
