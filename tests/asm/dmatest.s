; DMATEST: do the TR+'s DMA writes reach C64 RAM while a TDrive handler serves the KERNAL (Kernal Replace's /GAME snoop running)?
; The question behind a DMA LOAD (docs/DESIGN.md 48). The program waits; the host (tools/dmatest.py) writes a pattern with the
; remote WriteC64Mem command into four areas, then writes SEED and ROUND at $03F0. The program checks every byte, under ROM with
; $01 = $34, and acknowledges the round at $03F2. It also checks three guard pages the host never writes (a DMA write landing outside
; its range, as on some NTSC C128s) and that the served KERNAL still reads the same (a sum of $E000-$FFFF with the ROM in).
; While it waits it calls GETIN, so DMA starts while the CPU runs the served KERNAL and its IRQ code. Any key quits.
; Areas: $2000-$3FFF (what Ultimax mode would lose), $A000-$BFFF (under BASIC), $C000-$CFFF, $E000-$FEFF (under the KERNAL;
; not $FF00-$FFFF, where the program points the RAM's NMI vector at an RTI). Guards: $1F00, $4000, $9F00, filled at start.
; Pattern: byte at address hi:lo = ((lo EOR seed) + hi * $3B) AND $FF, the guards with seed $C3.
; A round's counts are added only when ROUND did not change while it was checked; else OVERRUN counts it (host wrote too soon).
CHROUT   = $FFD2
GETIN    = $FFE4
BANK     = $34
SEED     = $03F0            ; written by the host: seed, then round (1-255) in one write
ROUND    = $03F1
ACK      = $03F2            ; written here: the last round checked
GSEED    = $C3
NAREA    = 5                ; four areas + the guards

TOT      = $0340            ; 5 x 16-bit wrong bytes, all rounds
CUR      = $034A            ; 5 x 16-bit, this round
FA       = $0354            ; first wrong address per area (0 = none), lo/hi
FG       = $035E            ; its byte read
FE       = $0363            ; its byte expected
CFA      = $0368            ; the same for this round
CFG      = $0372
CFE      = $0377
ROUNDS   = $037C            ; rounds counted
OVR      = $037E            ; rounds dropped: ROUND changed while checking
ROMERR   = $0380            ; rounds whose KERNAL sum differed
ROMSUM   = $0382            ; the sum at start
SUM      = $0384
LASTR    = $0386
IDOK     = $0387            ; 1: $DE19/$DE1A read $D6 $64 (a TDrive handler answers)
ROW      = $0388
VEND     = $0389            ; end of the cleared block

kept     = $02              ; $01 while banked out
tgot     = $F7
texp     = $F8
sptr     = $F9              ; $F9/$FA: string pointer
ptr      = $FB              ; $FB/$FC: page base (low byte 0)
seed     = $FD
hk       = $FE              ; hi * $3B of the page
reg      = $FF              ; area index

.segment "LOADADDR"
        .word $0801
.segment "CODE"
        .word @next
        .word 10
        .byte $9E, "2061", 0
@next:  .word 0
        ldx #VEND-TOT-1
        lda #0
:       sta TOT,x
        dex
        bpl :-
        sta SEED
        sta ROUND
        sta ACK
        sta ptr
        lda #<nmirti            ; RAM NMI vector, used while $01 = $34 (RESTORE during a check)
        sta $FFFA
        lda #>nmirti
        sta $FFFB
        lda $DE19               ; a TDrive handler answers ID $D6 $64
        cmp #$D6
        bne :+
        lda $DE1A
        cmp #$64
        bne :+
        inc IDOK
:       jsr gfill
        jsr romsum
        lda SUM
        sta ROMSUM
        lda SUM+1
        sta ROMSUM+1
        lda #$93
        jsr CHROUT
        lda #<waitn
        ldx #>waitn
        jsr show

wait:   jsr GETIN
        beq :+
        lda #<byen
        ldx #>byen
        jmp puts
:       lda ROUND
        cmp LASTR
        beq wait
        sta LASTR
        lda #<chkn
        ldx #>chkn
        jsr show
        jsr round
        lda #<waitn
        ldx #>waitn
        jsr show
        jmp wait

nmirti: rti

; ---- one round ----------------------------------------------------------------------------------------------------------
round:  ldx #CFE+NAREA-CUR-1   ; clear CUR, CFA, CFG, CFE
        lda #0
:       sta CUR,x
        dex
        bpl :-
        lda SEED
        sta seed
        ldx #0
@a:     stx reg
        lda astart,x
        sta ptr+1
@p:     jsr vpageb
        inc ptr+1
        lda ptr+1
        ldx reg
        cmp aend,x
        bne @p
        inx
        cpx #4
        bne @a
        lda #4                  ; guards
        sta reg
        lda #GSEED
        sta seed
        ldx #2
@g:     lda gpage,x
        sta ptr+1
        txa
        pha
        jsr vpage
        pla
        tax
        dex
        bpl @g
        jsr romsum
        lda SUM
        cmp ROMSUM
        bne @rom
        lda SUM+1
        cmp ROMSUM+1
        beq @rok
@rom:   inc ROMERR
        bne @rok
        inc ROMERR+1
@rok:   lda ROUND
        cmp LASTR
        beq @keep
        inc OVR                 ; the host wrote again while this round was checked: drop its counts
        bne :+
        inc OVR+1
:       rts
@keep:  ldx #0                  ; TOT += CUR
@t:     lda TOT,x
        clc
        adc CUR,x
        sta TOT,x
        lda TOT+1,x
        adc CUR+1,x
        sta TOT+1,x
        inx
        inx
        cpx #NAREA*2
        bne @t
        ldx #0                  ; the first wrong byte of the run, per area
@f:     txa
        asl
        tay
        lda FA,y
        ora FA+1,y
        bne @fn
        lda CFA,y
        sta FA,y
        lda CFA+1,y
        sta FA+1,y
        lda CFG,x
        sta FG,x
        lda CFE,x
        sta FE,x
@fn:    inx
        cpx #NAREA
        bne @f
        inc ROUNDS
        bne :+
        inc ROUNDS+1
:       lda LASTR
        sta ACK
        rts

; page ptr+1 of area reg, banked out when the area needs it
vpageb: ldx reg
        lda abank,x
        beq vpage
        sei
        lda $01
        sta kept
        lda #BANK
        sta $01
        jsr vpage
        lda kept
        sta $01
        cli
        rts

vpage:  jsr mulkey
        ldy #0
@l:     tya
        eor seed
        clc
        adc hk
        cmp (ptr),y
        beq @ok
        jsr bad
@ok:    iny
        bne @l
        rts

; wrong byte at (ptr),y; A = expected. Keeps Y.
bad:    sta texp
        lda (ptr),y
        sta tgot
        lda reg
        asl
        tax
        inc CUR,x
        bne :+
        inc CUR+1,x
:       lda CFA,x
        ora CFA+1,x
        bne @x
        tya
        sta CFA,x
        lda ptr+1
        sta CFA+1,x
        ldx reg
        lda tgot
        sta CFG,x
        lda texp
        sta CFE,x
@x:     rts

; hk = (ptr+1) * $3B
mulkey: ldx ptr+1
        lda #0
:       clc
        adc #$3B
        dex
        bne :-
        sta hk
        rts

gfill:  lda #GSEED
        sta seed
        ldx #2
@g:     lda gpage,x
        sta ptr+1
        txa
        pha
        jsr mulkey
        ldy #0
:       tya
        eor seed
        clc
        adc hk
        sta (ptr),y
        iny
        bne :-
        pla
        tax
        dex
        bpl @g
        rts

; SUM = 16-bit sum of $E000-$FFFF as the CPU reads it now (the served KERNAL)
romsum: lda #0
        sta SUM
        sta SUM+1
        lda #$E0
        sta ptr+1
        ldy #0
@l:     lda (ptr),y
        clc
        adc SUM
        sta SUM
        bcc :+
        inc SUM+1
:       iny
        bne @l
        inc ptr+1
        bne @l
        rts

; ---- screen -------------------------------------------------------------------------------------------------------------
; the whole status from the top; A/X = the state line
show:   pha
        txa
        pha
        lda #$13
        jsr CHROUT
        lda #<hdr
        ldx #>hdr
        jsr puts
        lda IDOK
        bne :+
        lda #<noidn
        ldx #>noidn
        bne :++
:       lda #<idn
        ldx #>idn
:       jsr puts
        lda #<rndn
        ldx #>rndn
        jsr puts
        ldx #ROUNDS-TOT
        jsr p16
        lda #<ovrn
        ldx #>ovrn
        jsr puts
        ldx #OVR-TOT
        jsr p16
        lda #<romn
        ldx #>romn
        jsr puts
        ldx #ROMERR-TOT
        jsr p16
        lda #<coln
        ldx #>coln
        jsr puts
        lda #0
        sta ROW
@r:     lda ROW                 ; name: 11 characters at ROW * 11
        asl
        sta sptr
        asl
        asl
        adc sptr
        adc ROW
        tax
        ldy #11
:       lda names,x
        jsr CHROUT
        inx
        dey
        bne :-
        lda ROW
        asl
        tax
        jsr p16                 ; TOT
        jsr space
        lda ROW
        asl
        clc
        adc #FA-TOT
        tax
        lda TOT,x
        ora TOT+1,x
        bne :+
        lda #<nonen
        ldx #>nonen
        jsr puts
        jmp @n
:       jsr p16
        jsr space
        jsr space
        ldx ROW
        lda FG,x
        jsr hex
        jsr space
        jsr space
        ldx ROW
        lda FE,x
        jsr hex
@n:     lda #13
        jsr CHROUT
        inc ROW
        lda ROW
        cmp #NAREA
        bne @r
        lda #13
        jsr CHROUT
        pla
        tax
        pla
        jmp puts

; the 16-bit counter at TOT+X, as 4 hex digits
p16:    lda TOT+1,x
        jsr hex
        lda TOT,x
hex:    pha
        lsr
        lsr
        lsr
        lsr
        jsr nib
        pla
        and #$0F
nib:    ora #'0'
        cmp #'0'+10
        bcc :+
        adc #6
:       jmp CHROUT

space:  lda #' '
        jmp CHROUT

puts:   sta sptr
        stx sptr+1
        ldy #0
:       lda (sptr),y
        beq :+
        jsr CHROUT
        iny
        bne :-
:       rts

.segment "RODATA"
astart: .byte $20, $A0, $C0, $E0
aend:   .byte $40, $C0, $D0, $FF
abank:  .byte 0, 1, 0, 1, 0
gpage:  .byte $1F, $40, $9F
names:  .byte "2000-3FFF  ", "A000-BFFF  ", "C000-CFFF  ", "E000-FEFF  ", "GUARDS     "
hdr:    .byte "DMATEST: TR+ DMA WRITES INTO RAM", 13, 13, 0
idn:    .byte "TDRIVE HANDLER: YES", 13, 0
noidn:  .byte "TDRIVE HANDLER: NO (ID NOT $D6 $64)", 13, 0
rndn:   .byte "ROUNDS   ", 0
ovrn:   .byte 13, "OVERRUN  ", 0
romn:   .byte 13, "ROM SUM  ", 0
coln:   .byte 13, 13, "AREA       ERRS FIRST GOT EXP", 13, 0
nonen:  .byte "----", 0
waitn:  .byte "WAITING FOR TOOLS/DMATEST.PY  ", 13, "ANY KEY QUITS", 0
chkn:   .byte "CHECKING                      ", 13, "             ", 0
byen:   .byte 13, "BYE", 13, 0
