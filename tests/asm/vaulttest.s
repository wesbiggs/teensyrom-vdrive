; RAM under the I/O area and the KERNAL, the way Quest for the King uses it (its vault.s): switch to all-RAM ($01 = $34), write or
; read pages of $D000-$FEFF, switch back. Needs no mailbox, so it runs under any KERNAL image / Special IO (compare the stock image
; with a patched one, both through Kernal Replace). A clean run prints 0000 in every column.
;   T1  fill every page in one all-RAM session, verify in the same session
;   T2  one session per page (enter, 256 bytes, leave) for the fill and again for the verify: $01 changes all the time
;   T3  as T2 with 64 bytes per session, like the game's row reads
; Columns: wrong bytes in the $Dxxx, $Exxx and $Fxxx pages.
CHROUT   = $FFD2
BANK     = $34
errs     = $0334            ; cassette buffer: D lo/hi, E lo/hi, F lo/hi
ptr      = $FB              ; $FB/$FC: page base (low byte 0)
seed     = $FD
kept     = $FE
rnd      = $FF
ystart   = $F7
yend     = $F8              ; 0 = 256

.segment "LOADADDR"
        .word $0801
.segment "CODE"
        .word @next
        .word 10
        .byte $9E, "2061", 0
@next:  .word 0
        lda #<hdr
        ldx #>hdr
        jsr puts
        lda #<t1n
        ldx #>t1n
        jsr puts
        jsr clr
        jsr test1
        jsr report
        lda #<t2n
        ldx #>t2n
        jsr puts
        jsr clr
        jsr test2
        jsr report
        lda #<t3n
        ldx #>t3n
        jsr puts
        jsr clr
        jsr test3
        jsr report
        lda #<donen
        ldx #>donen
        jmp puts

clr:    ldx #5
        lda #0
:       sta errs,x
        dex
        bpl :-
        rts

enter:  sei
        lda $01
        sta kept
        lda #BANK
        sta $01
        rts
leave:  lda kept
        sta $01
        cli
        rts

fillr:  ldy ystart
:       tya
        eor ptr+1
        eor seed
        sta (ptr),y
        iny
        cpy yend
        bne :-
        rts

checkr: ldy ystart
:       tya
        eor ptr+1
        eor seed
        cmp (ptr),y
        beq @ok
        jsr bad
@ok:    iny
        cpy yend
        bne :-
        rts

bad:    tya
        pha
        lda ptr+1
        lsr
        lsr
        lsr
        lsr                     ; $D, $E or $F
        sec
        sbc #$0D
        asl
        tax
        inc errs,x
        bne :+
        inc errs+1,x
:       pla
        tay
        rts

firstpage:
        lda #$D0
        sta ptr+1
        lda #0
        sta ptr
        rts

; T1: 64 rounds
test1:  lda #0
        sta rnd
@r:     lda rnd
        sta seed
        lda #0
        sta ystart
        sta yend
        jsr enter
        jsr firstpage
@f:     jsr fillr
        inc ptr+1
        lda ptr+1
        cmp #$FF
        bne @f
        jsr firstpage
@c:     jsr checkr
        inc ptr+1
        lda ptr+1
        cmp #$FF
        bne @c
        jsr leave
        inc rnd
        lda rnd
        cmp #64
        bne @r
        rts

; T2: 32 rounds, one session per page
test2:  lda #0
        sta rnd
@r:     lda rnd
        sta seed
        lda #0
        sta ystart
        sta yend
        jsr firstpage
@f:     jsr enter
        jsr fillr
        jsr leave
        inc ptr+1
        lda ptr+1
        cmp #$FF
        bne @f
        jsr firstpage
@c:     jsr enter
        jsr checkr
        jsr leave
        inc ptr+1
        lda ptr+1
        cmp #$FF
        bne @c
        inc rnd
        lda rnd
        cmp #32
        bne @r
        rts

; T3: 32 rounds, one session per 64 bytes
test3:  lda #0
        sta rnd
@r:     lda rnd
        sta seed
        jsr firstpage
@f:     lda #0
        sta ystart
@f1:    clc
        adc #64
        sta yend
        jsr enter
        jsr fillr
        jsr leave
        lda yend
        sta ystart
        bne @f1
        inc ptr+1
        lda ptr+1
        cmp #$FF
        bne @f
        jsr firstpage
@c:     lda #0
        sta ystart
@c1:    clc
        adc #64
        sta yend
        jsr enter
        jsr checkr
        jsr leave
        lda yend
        sta ystart
        bne @c1
        inc ptr+1
        lda ptr+1
        cmp #$FF
        bne @c
        inc rnd
        lda rnd
        cmp #32
        bne @r
        rts

report: ldx #0
@l:     lda #' '
        jsr CHROUT
        lda errs+1,x
        jsr hex
        lda errs,x
        jsr hex
        inx
        inx
        cpx #6
        bne @l
        lda #13
        jmp CHROUT

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

puts:   sta $F9
        stx $FA
        ldy #0
:       lda ($F9),y
        beq :+
        jsr CHROUT
        iny
        bne :-
:       rts

.segment "RODATA"
hdr:    .byte "RAM UNDER ROM (COLUMNS: D E F)", 13, 0
t1n:    .byte "T1 ONE SESSION  ", 0
t2n:    .byte "T2 PAGE SESSIONS", 0
t3n:    .byte "T3 ROW SESSIONS ", 0
donen:  .byte "DONE", 13, 0
