; dmc_oam_overlap.s - self-checking regression ROM for the OAM DMA restart bug
; (docs/research/nes-verification-findings.md, finding 1).
;
; Repeatedly:  disable DMC, wait a pseudo-random number of cycles,
;              STA $4015 (enable DMC)  ->  DMC DMA requested 2-3 cycles later
;              STX $4014 (OAM DMA)     ->  write lands on the 4th cycle
; When the DMC request is raised during the 3rd cycle of STX $4014 (an operand
; read), it is only serviced at the next read, which follows the $4014 write,
; so DMC DMA and OAM DMA start at the same halt.  Because a $4015-started
; request is not aligned to the DMC output unit's reload, some timer phase
; makes the DMC fetch end exactly on a reload: with the sample-duplication
; glitch enabled that requests another DMC DMA in the middle of the OAM DMA.
;
; Page $02 holds 0,1,2,...,255.  After every OAM DMA the ROM reads OAM[0] back
; through $2003/$2004 (rendering is off, so this is reliable).  A correct DMA
; leaves OAM[0] = 0; the bug (257 writes, OAM shifted by one) leaves 255.
;
; Results in CPU RAM (read by tools/nes-dma-regression):
;   $10-$11  iterations (16-bit, little endian)
;   $12-$13  failures   (16-bit, little endian)
;
; Build:  ca65 dmc_oam_overlap.s -o dmc_oam_overlap.o
;         ld65 -C nrom.cfg dmc_oam_overlap.o -o dmc_oam_overlap.nes

ITER = $10
FAIL = $12

.segment "HEADER"
    .byte "NES", $1A
    .byte 1             ; 16 KB PRG
    .byte 1             ; 8 KB CHR
    .byte 0, 0          ; mapper 0, horizontal mirroring
    .res 8, 0

.segment "ZEROPAGE"
.res $10                ; $00-$0F unused
iter:  .res 2           ; $10
fail:  .res 2           ; $12
seed:  .res 1           ; $14
seed2: .res 1           ; $15

.segment "CODE"
reset:
    sei
    cld
    ldx #$FF
    txs
    lda #$00
    sta $2000           ; NMI off
    sta $2001           ; rendering off
    sta $4015           ; all channels off
    lda #$40
    sta $4017           ; frame IRQ off

    ldx #$00            ; page $02 = 0,1,2,...,255
:   txa
    sta $0200,x
    inx
    bne :-

    lda #$00
    sta iter
    sta iter+1
    sta fail
    sta fail+1

    lda #$0F            ; DMC: rate 15 (fastest), no IRQ, no loop
    sta $4010
    lda #$00            ; sample address $C000
    sta $4012
    lda #$01            ; sample length 17 bytes
    sta $4013

    lda #$01
    sta seed

loop:
    lda #$00
    sta $4015           ; disable DMC (bytesRemaining -> 0 after a short delay)
    sta $2003           ; OAMADDR = 0 for the next DMA

    ; 8-bit Galois LFSR, period 255
    lda seed
    asl a
    bcc :+
    eor #$1D
:   sta seed

    ; parity tweak: 2 or 3 extra cycles depending on bit 0
    and #$01
    beq :+
    bit $00
:
    ; coarse delay: ~5 cycles per count, 5..1275 cycles
    ldy seed
dly:
    dey
    bne dly

    ; second delay: a plain counter (period 256, coprime with the LFSR's 255),
    ; so the two together sweep ~65k timing combinations, not 255
    inc seed2
    ldy seed2
    beq :++
:   dey
    bne :-
:

    ldx #$02            ; OAM DMA source page $0200
    lda #$10
    sta $4015           ; enable DMC -> DMA request after 2-3 cycles
    stx $4014           ; start OAM DMA

    ; check: OAM[0] must be 0
    lda #$00
    sta $2003
    lda $2004
    beq ok
    inc fail
    bne ok
    inc fail+1
ok:
    inc iter
    bne :+
    inc iter+1
:   jmp loop

nmi:
irq:
    rti

.segment "VECTORS"
    .word nmi, reset, irq
