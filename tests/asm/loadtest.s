; LOADTEST: DMA LOAD (docs/DESIGN.md 49) into RAM under BASIC and across $D000. Loads two files from the test disk with the
; KERNAL's LOAD (secondary address 1, the last-used device, 8 if none) and checks every byte.
;   LDA000: $A000-$BFEF, all under the BASIC ROM. Byte at addr = lo EOR hi EOR $3C. With DMA LOAD: parts of 2 KB by DMA.
;   LDD000: $C7F0-$DBFF. $C7F0-$CFFF RAM (lo EOR hi EOR $C3): a 2 KB DMA part to $CFF0, then one cut short at $D000;
;           from $D000 on the window path, which writes I/O as a stock LOAD does: $D000-$D3FF VIC registers (a harmless
;           64-byte set, 16 times; the screen registers are put back right after the LOAD), $D400-$D7FF SID (zeros),
;           $D800-$DBFF colour RAM ((lo EOR hi EOR $C3) AND $0F, checked in the low nibble). It stops short of the CIAs.
; Also checked: guard bytes after/before each file ($55 left alone), the RAM under $D000-$DBFF untouched ($AA: nothing may
; DMA there), and LOAD's end address (X/Y). The jiffy count shows which path ran: a window LOAD of 8 KB takes ~14, DMA ~2-3.
; Results also stay in memory: $0334 = number of failed checks.
CHROUT   = $FFD2
SETLFS   = $FFBA
SETNAM   = $FFBD
LOAD     = $FFD5
SETMSG   = $FF90
LINPRT   = $BDCD            ; BASIC: print A (hi) / X (lo) as an unsigned number

KA       = $3C
KD       = $C3

FAILS    = $0334
DEV      = $0335
MSGS     = $0336
T0       = $0337
JIF      = $0338
ENDL     = $0339
ENDH     = $033A
LERR     = $033B
MODE     = $033C            ; bit 7: constant (WANTT), else the pattern
WANTT    = $033D
WANTE    = $033E            ; expected end address lo/hi
RES      = $0340            ; ERR lo/hi, FIRST lo/hi, GOT, WANT
ERR      = RES
FIRST    = RES+2
GOT      = RES+4
WANT     = RES+5
RESC     = $0346            ; the colour check's results, kept while the others print
VSAVE    = $0350            ; $D011 $D016 $D018 $D020 $D021 $D015 $D01A

P        = $FB              ; $FB/$FC
E        = $FD              ; $FD/$FE
K        = $02
MASK     = $A3              ; serial scratch, free while we do not bit-bang
SP       = $A4              ; $A4/$A5: puts' string pointer (serial scratch too)

.macro  ptrs ps, pe
        lda #<(ps)
        sta P
        lda #>(ps)
        sta P+1
        lda #<(pe)
        sta E
        lda #>(pe)
        sta E+1
.endmacro
.macro  print s
        ldx #<(s)
        ldy #>(s)
        jsr puts
.endmacro

.segment "CODE"
start:                          ; VDSUITE calls it by SYS through the jump table in tests/asm/suite.s
        lda #0
        sta FAILS
        lda #1
        sta $CC                 ; no cursor blink writing colour RAM
        lda $9D
        sta MSGS
        lda #0
        jsr SETMSG              ; no SEARCHING/LOADING text while colour RAM is being loaded
        lda $BA
        cmp #8
        bcs :+
        lda #8
:       sta DEV
        print hdr
        jsr test_a
        jsr test_d
        lda MSGS
        jsr SETMSG
        lda FAILS
        bne @f
        print allok
        rts
@f:     print somebad
        lda #0
        ldx FAILS
        jsr LINPRT
        lda #13
        jmp CHROUT

; ---------------------------------------------------------------- LDA000
test_a: ptrs $A000, $C000       ; the file and its guard to $55 (writes reach the RAM under the BASIC ROM)
        lda #$55
        jsr fillc
        print la_t
        lda #<$BFF0
        sta WANTE
        lda #>$BFF0
        sta WANTE+1
        lda #6
        ldx #<na
        ldy #>na
        jsr doload
        bcc :+
        jmp loaderr
:       jsr showend
        sei
        lda #$36                ; BASIC out
        sta $01
        ptrs $A000, $BFF0
        lda #KA
        sta K
        lda #$FF
        sta MASK
        jsr chkp
        jsr ret37
        print la_r
        jsr report
        sei
        lda #$36
        sta $01
        ptrs $BFF0, $C000
        lda #$55
        jsr chkc
        jsr ret37
        print la_g
        jmp report

; ---------------------------------------------------------------- LDD000
test_d: lda $D011
        sta VSAVE
        lda $D016
        sta VSAVE+1
        lda $D018
        sta VSAVE+2
        lda $D020
        sta VSAVE+3
        lda $D021
        sta VSAVE+4
        lda $D015
        sta VSAVE+5
        lda $D01A
        sta VSAVE+6
        ptrs $C7E0, $D000       ; guard + RAM part
        lda #$55
        jsr fillc
        sei
        lda #$34                ; all RAM: the RAM under I/O to $AA
        sta $01
        ptrs $D000, $DC00
        lda #$AA
        jsr fillc
        jsr ret37
        ptrs $D800, $DC00       ; colour RAM to the inverse of what the file brings
        lda #KD ^ $0F
        sta K
        lda #$0F
        sta MASK
        jsr fillp
        print ld_t
        lda #<$DC00
        sta WANTE
        lda #>$DC00
        sta WANTE+1
        lda #6
        ldx #<nd
        ldy #>nd
        jsr doload
        php
        lda VSAVE               ; the screen back first
        sta $D011
        lda VSAVE+1
        sta $D016
        lda VSAVE+2
        sta $D018
        lda VSAVE+3
        sta $D020
        lda VSAVE+4
        sta $D021
        lda VSAVE+5
        sta $D015
        lda VSAVE+6
        sta $D01A
        plp
        bcc :+
        jsr colfix
        jmp loaderr
:       ptrs $D800, $DC00       ; colour RAM before anything prints
        lda #KD
        sta K
        lda #$0F
        sta MASK
        jsr chkp
        ldx #5
:       lda RES,x
        sta RESC,x
        dex
        bpl :-
        jsr colfix
        jsr showend
        ptrs $C7F0, $D000
        lda #KD
        sta K
        lda #$FF
        sta MASK
        jsr chkp
        print ld_r
        jsr report
        ptrs $C7E0, $C7F0
        lda #$55
        jsr chkc
        print ld_g
        jsr report
        ldx #5
:       lda RESC,x
        sta RES,x
        dex
        bpl :-
        print ld_c
        jsr report
        sei
        lda #$34
        sta $01
        ptrs $D000, $DC00
        lda #$FF
        sta MASK
        lda #$AA
        jsr chkc
        jsr ret37
        print ld_u
        jmp report

colfix: ptrs $D800, $DC00       ; colour RAM to the text colour again
        lda $0286
        jmp fillc

ret37:  lda #$37
        sta $01
        cli
        rts

; A = name length, X/Y = name. LOAD ,DEV,1. C set: error, A = code.
doload: jsr SETNAM
        lda #1
        ldx DEV
        ldy #1
        jsr SETLFS
        lda $A2
        sta T0
        lda #0
        jsr LOAD
        stx ENDL
        sty ENDH
        sta LERR
        php
        lda $A2
        sec
        sbc T0
        sta JIF
        plp
        rts

loaderr: print lerr
        lda #0
        ldx LERR
        jsr LINPRT
        lda #13
        jsr CHROUT
        inc FAILS
        rts

showend: print endt
        lda ENDH
        jsr hex
        lda ENDL
        jsr hex
        lda ENDL
        cmp WANTE
        bne @bad
        lda ENDH
        cmp WANTE+1
        beq @ok
@bad:   inc FAILS
        print wrong
        lda WANTE+1
        jsr hex
        lda WANTE
        jsr hex
@ok:    print jift
        lda #0
        ldx JIF
        jsr LINPRT
        print jif2
        rts

report: lda ERR
        ora ERR+1
        bne @bad
        print okt
        rts
@bad:   inc FAILS
        lda ERR+1
        ldx ERR
        jsr LINPRT
        print badt
        lda FIRST+1
        jsr hex
        lda FIRST
        jsr hex
        print gott
        lda GOT
        jsr hex
        print wantt
        lda WANT
        jsr hex
        lda #13
        jmp CHROUT

; ---- fill / check [P, E): constant (A) or pattern (lo EOR hi EOR K) AND MASK
fillc:  sta WANTT
        lda #$80
        bne fill0
fillp:  lda #0
fill0:  sta MODE
        ldy #0
@lp:    lda P
        cmp E
        bne @go
        lda P+1
        cmp E+1
        beq @done
@go:    jsr want
        sta (P),y
        inc P
        bne @lp
        inc P+1
        bne @lp
@done:  rts

chkc:   sta WANTT
        lda #$80
        bne chk0
chkp:   lda #0
chk0:   sta MODE
        lda #0
        sta ERR
        sta ERR+1
        ldy #0
@lp:    lda P
        cmp E
        bne @go
        lda P+1
        cmp E+1
        beq @done
@go:    jsr want
        lda (P),y
        and MASK
        cmp WANTT
        beq @nx
        jsr bad
@nx:    inc P
        bne @lp
        inc P+1
        bne @lp
@done:  rts

want:   bit MODE                ; WANTT and A = the byte expected at P
        bmi @c
        lda P
        eor P+1
        eor K
        and MASK
        sta WANTT
@c:     lda WANTT
        rts

bad:    tax                     ; A = the byte read
        lda ERR
        ora ERR+1
        bne @n
        stx GOT
        lda WANTT
        sta WANT
        lda P
        sta FIRST
        lda P+1
        sta FIRST+1
@n:     inc ERR
        bne :+
        inc ERR+1
:       rts

hex:    pha
        lsr
        lsr
        lsr
        lsr
        jsr @d
        pla
        and #$0F
@d:     cmp #10
        bcc :+
        adc #6
:       adc #'0'
        jmp CHROUT

puts:   stx SP
        sty SP+1
        ldy #0
:       lda (SP),y
        beq :+
        jsr CHROUT
        iny
        bne :-
:       rts

.segment "RODATA"
na:     .byte "LDA000"
nd:     .byte "LDD000"
hdr:    .byte 147, "LOADTEST: UNDER BASIC, ACROSS $D000", 13, 13, 0
la_t:   .byte "LDA000 TO $A000-$BFEF", 13, 0
la_r:   .byte "  A000-BFEF UNDER ROM ", 0
la_g:   .byte "  BFF0-BFFF GUARD     ", 0
ld_t:   .byte 13, "LDD000 TO $C7F0-$DBFF", 13, 0
ld_r:   .byte "  C7F0-CFFF RAM       ", 0
ld_g:   .byte "  C7E0-C7EF GUARD     ", 0
ld_c:   .byte "  D800-DBFF COLOUR    ", 0
ld_u:   .byte "  D000-DBFF UNDER I/O ", 0
endt:   .byte "  END $", 0
wrong:  .byte " WRONG, WANT $", 0
jift:   .byte ", ", 0
jif2:   .byte " JIFFIES", 13, 0
okt:    .byte "OK", 13, 0
badt:   .byte " BAD AT $", 0
gott:   .byte 13, "    GOT $", 0
wantt:  .byte " WANT $", 0
lerr:   .byte "  LOAD ERROR ", 0
allok:  .byte 13, "ALL CHECKS PASSED", 13, 0
somebad: .byte 13, "FAILED CHECKS: ", 0
