; REUTEST: how big is the REU really, and does every byte of it keep its own data?
; Written for the VDrive+Swift+REU Special IO, whose REU size comes from the RAM left after SwiftLink (docs/DESIGN.md 32),
; but works with any 1700/1764/1750-style REU at $DF00.
;   1. The status register's chip bit (bit 4: 1 = 256 Kbit chips, a 1764/1750; 0 = a 1700).
;   2. Size: one tag byte at offset 0 of each 64 KB bank, written from bank 7 down to bank 0, then read back from bank 0 up.
;      An REU of N banks wraps the address, so bank N reads back bank 0's tag (bank N+1 bank 1's, ...). The first bank that does
;      not hold its own tag ends the REU. Every bank's line is printed ("BANK 4 HOLDS BANK 0" = wrapped there).
;   3. Data: every 8 KB chunk of the size found gets its own pattern (STASH from $6000), then each chunk is FETCHed to $8000
;      and compared byte by byte. A dot per good chunk, an X per bad one, the count of wrong bytes and the first one.
; Results also stay in memory for a monitor: $0334 status, $0335 banks found, $0336/$0337 wrong bytes, $0338 first bad chunk ($FF none).
; The REU command is an immediate one ($FF00 trigger off); nothing here uses the disk.
CHROUT   = $FFD2

REU      = $DF00
R_STAT   = REU + 0
R_CMD    = REU + 1
R_C64LO  = REU + 2
R_C64HI  = REU + 3
R_REULO  = REU + 4
R_REUMI  = REU + 5
R_REUBK  = REU + 6
R_LENLO  = REU + 7
R_LENHI  = REU + 8
R_IMASK  = REU + 9
R_ACTRL  = REU + 10
C_STASH  = $90              ; execute, no $FF00 wait, C64 -> REU
C_FETCH  = $91              ; execute, no $FF00 wait, REU -> C64

BUF      = $6000            ; 8 KB pattern (above VDSUITE, which lowers the top of BASIC to $6000)
BACK     = $8000            ; 8 KB read back
TAGBASE  = $50              ; bank b's tag is TAGBASE + b
MAXBANK  = 8                ; 512 KB, the TeensyROM's limit

RSTAT    = $0334            ; results for a monitor
RBANKS   = $0335
RERR     = $0336            ; lo/hi, stops at $FFFF
RFIRST   = $0338            ; first bad chunk, $FF = none
FOFS     = $0339            ; its first bad offset lo/hi
FGOT     = $033B
FEXP     = $033C
TAG      = $033D            ; one-byte transfer buffer
BANK     = $033E
CHUNKS   = $033F

ptr      = $F7              ; $F7/$F8
ptr2     = $F9              ; $F9/$FA
sptr     = $FB              ; $FB/$FC
chunk    = $FD
cx       = $FE
hk       = $02
bad      = $03              ; this chunk had a wrong byte

.segment "CODE"
start:                          ; VDSUITE calls it by SYS through the jump table in tests/asm/suite.s
        lda #0
        sta RBANKS
        sta RERR
        sta RERR+1
        lda #$FF
        sta RFIRST
        ldx #<hdr
        ldy #>hdr
        jsr puts

        ; an REU at all? Its C64 address register reads back what was written; open bus does not.
        lda #$55
        sta R_C64LO
        cmp R_C64LO
        bne @none
        lda #$AA
        sta R_C64LO
        cmp R_C64LO
        beq @there
@none:  ldx #<noreu
        ldy #>noreu
        jmp puts
@there: lda #0
        sta R_IMASK
        sta R_ACTRL
        lda R_STAT
        sta RSTAT
        ldx #<stat
        ldy #>stat
        jsr puts
        lda RSTAT
        jsr hex
        ldx #<chips1
        ldy #>chips1
        lda RSTAT
        and #$10
        bne :+
        ldx #<chips0
        ldy #>chips0
:       jsr puts

        ; --- size: tags from bank 7 down to 0, then read 0 up
        ldx #MAXBANK-1
@wtag:  stx BANK
        txa
        clc
        adc #TAGBASE
        sta TAG
        lda #C_STASH
        jsr xfer1
        ldx BANK
        dex
        bpl @wtag

        lda #MAXBANK
        sta RBANKS              ; lowered at the first bank that does not hold its tag
        ldx #0
@rtag:  stx BANK
        lda #0
        sta TAG
        lda #C_FETCH
        jsr xfer1
        ldx #<bnk
        ldy #>bnk
        jsr puts
        lda BANK
        ora #'0'
        jsr CHROUT
        lda TAG
        sec
        sbc #TAGBASE
        cmp BANK
        bne @wrong
        ldx #<ok
        ldy #>ok
        jsr puts
        jmp @nextb
@wrong: pha
        lda BANK
        cmp RBANKS
        bcs :+
        sta RBANKS
:       pla
        cmp #MAXBANK
        bcs @odd
        pha
        ldx #<holds
        ldy #>holds
        jsr puts
        pla
        ora #'0'
        jsr CHROUT
        lda #13
        jsr CHROUT
        jmp @nextb
@odd:   ldx #<tagis
        ldy #>tagis
        jsr puts
        lda TAG
        jsr hex
        lda #')'
        jsr CHROUT
        lda #13
        jsr CHROUT
@nextb: ldx BANK
        inx
        cpx #MAXBANK
        bne @rtag

        ldx #<size
        ldy #>size
        jsr puts
        lda RBANKS
        bne :+
        ldx #<zero
        ldy #>zero
        jmp puts
:       asl
        tax
        lda sizes-2,x
        ldy sizes-1,x
        tax
        jsr puts
        lda RBANKS
        cmp #MAXBANK
        bne :+
        ldx #<ormore
        ldy #>ormore
        jsr puts
:       lda #13
        jsr CHROUT

        ; --- data: every 8 KB chunk of the size found
        lda RBANKS
        asl
        asl
        asl
        sta CHUNKS
        ldx #<wr
        ldy #>wr
        jsr puts
        lda #0
        sta chunk
@stash: jsr gen
        lda #C_STASH
        jsr xferk
        lda #'.'
        jsr CHROUT
        inc chunk
        lda chunk
        cmp CHUNKS
        bne @stash

        ldx #<rd
        ldy #>rd
        jsr puts
        lda #0
        sta chunk
@fetch: jsr gen
        lda #C_FETCH
        jsr xferk
        jsr cmpk
        lda #'.'
        ldx bad
        beq :+
        lda #'X'
:       jsr CHROUT
        inc chunk
        lda chunk
        cmp CHUNKS
        bne @fetch

        ldx #<wrong
        ldy #>wrong
        jsr puts
        lda RERR+1
        jsr hex
        lda RERR
        jsr hex
        lda #13
        jsr CHROUT
        lda RFIRST
        cmp #$FF
        beq @pass
        ldx #<first
        ldy #>first
        jsr puts
        lda RFIRST
        jsr hex
        ldx #<at
        ldy #>at
        jsr puts
        lda FOFS+1
        jsr hex
        lda FOFS
        jsr hex
        ldx #<got
        ldy #>got
        jsr puts
        lda FGOT
        jsr hex
        ldx #<exp
        ldy #>exp
        jsr puts
        lda FEXP
        jsr hex
        ldx #<fail
        ldy #>fail
        jmp puts
@pass:  ldx #<pass
        ldy #>pass
        jmp puts

; one byte between TAG and offset 0 of bank BANK; A = command
xfer1:  pha
        lda #<TAG
        sta R_C64LO
        lda #>TAG
        sta R_C64HI
        lda #0
        sta R_REULO
        sta R_REUMI
        sta R_LENHI
        lda BANK
        sta R_REUBK
        lda #1
        sta R_LENLO
        pla
        sta R_CMD
        rts

; 8 KB between BUF (stash) or BACK (fetch) and chunk `chunk`; A = command
xferk:  pha
        lda #0
        sta R_C64LO
        sta R_REULO
        sta R_LENLO
        pla
        pha
        ldx #>BUF
        cmp #C_FETCH
        bne :+
        ldx #>BACK
:       stx R_C64HI
        lda chunk
        and #7
        asl
        asl
        asl
        asl
        asl
        sta R_REUMI             ; (chunk & 7) * $20 pages
        lda chunk
        lsr
        lsr
        lsr
        sta R_REUBK
        lda #$20
        sta R_LENHI
        pla
        sta R_CMD
        rts

; BUF = chunk's pattern: byte at page h, offset l = (l EOR cx) + hk, cx = chunk * $12 + $5A, hk = h * $3B + chunk
gen:    lda chunk
        asl
        sta cx
        asl
        asl
        asl
        clc
        adc cx
        adc #$5A
        sta cx
        lda chunk
        sta hk
        lda #0
        sta ptr
        lda #>BUF
        sta ptr+1
        ldy #0
@g:     tya
        eor cx
        clc
        adc hk
        sta (ptr),y
        iny
        bne @g
        lda hk
        clc
        adc #$3B
        sta hk
        inc ptr+1
        lda ptr+1
        cmp #>(BUF+$2000)
        bne @g
        rts

; compare BACK with BUF; count wrong bytes, keep the first
cmpk:   lda #0
        sta bad
        sta ptr
        sta ptr2
        lda #>BUF
        sta ptr+1
        lda #>BACK
        sta ptr2+1
        ldy #0
@c:     lda (ptr2),y
        cmp (ptr),y
        bne @diff
@cn:    iny
        bne @c
        inc ptr+1
        inc ptr2+1
        lda ptr+1
        cmp #>(BUF+$2000)
        bne @c
        rts
@diff:  lda #1
        sta bad
        inc RERR
        bne :+
        inc RERR+1
        bne :+
        lda #$FF                ; stop at $FFFF
        sta RERR
        sta RERR+1
:       lda RFIRST
        cmp #$FF
        bne @cn
        lda chunk
        sta RFIRST
        lda (ptr2),y
        sta FGOT
        lda (ptr),y
        sta FEXP
        sty FOFS
        lda ptr+1
        sec
        sbc #>BUF
        sta FOFS+1
        jmp @cn

puts:   stx sptr
        sty sptr+1
        ldy #0
:       lda (sptr),y
        beq :+
        jsr CHROUT
        iny
        bne :-
:       rts

hex:    pha
        lsr
        lsr
        lsr
        lsr
        jsr nyb
        pla
        and #$0F
nyb:    cmp #10
        bcc :+
        adc #6
:       adc #'0'
        jmp CHROUT

.segment "RODATA"
hdr:    .byte 147, "REUTEST: REU SIZE AND DATA", 13, 13, 0
noreu:  .byte "NO REU AT $DF00 (REGISTERS DO NOT", 13, "READ BACK)", 13, 0
stat:   .byte "STATUS $", 0
chips1: .byte " (CHIP BIT SET: 1764/1750)", 13, 0
chips0: .byte " (CHIP BIT CLEAR: 1700)", 13, 0
bnk:    .byte "64K BANK ", 0
ok:     .byte ": OK", 13, 0
holds:  .byte ": HOLDS BANK ", 0
tagis:  .byte ": NOT THERE (READS $", 0
size:   .byte 13, "SIZE: ", 0
zero:   .byte "NONE (BANK 0 FAILED)", 13, 0
ormore: .byte " OR MORE", 0
s64:    .byte "64K", 0
s128:   .byte "128K", 0
s192:   .byte "192K", 0
s256:   .byte "256K", 0
s320:   .byte "320K", 0
s384:   .byte "384K", 0
s448:   .byte "448K", 0
s512:   .byte "512K", 0
sizes:  .word s64, s128, s192, s256, s320, s384, s448, s512
wr:     .byte 13, "WRITE 8K CHUNKS:", 13, 0
rd:     .byte 13, "READ BACK:", 13, 0
wrong:  .byte 13, "WRONG BYTES: $", 0
first:  .byte "FIRST IN CHUNK $", 0
at:     .byte " AT $", 0
got:    .byte 13, "READ $", 0
exp:    .byte " EXPECTED $", 0
fail:   .byte 13, "FAIL", 13, 0
pass:   .byte "PASS", 13, 0
