; Bus stress test for the TR+ mailbox (IO1 $DE10..) and window (IO2 $DF80..). Raw 6502 accesses at full speed, no commands are sent:
; it only uses registers that read back what was written (ARG, LEN), the idle STAT token and the 128-byte window.
; Output per test: error count (hex) and the OR of all wrong bit patterns (hex). Zero errors everywhere = the bus handlers keep up.
.ifndef MBX
MBX      = $DE10
.endif
.ifndef WIN
WIN      = $DF80                 ; 128 bytes (VIO_WIN); every window loop below runs Y 0..127
.endif
MBX_ARG  = MBX+1
MBX_STAT = MBX+3
MBX_LEN  = MBX+5
CHROUT   = $FFD2
BUF      = $C000
cnt      = $FB          ; 16-bit error count
msk      = $FD          ; OR of the XOR of every wrong read
tmp      = $FE
rep      = $FF

.segment "LOADADDR"
        .word $0801
.segment "CODE"
        .word @next
        .word 10
        .byte $9E, "2061", 0
@next:  .word 0
        sei
        lda #$A5
        sta MBX_ARG
        lda #$5A
        sta MBX_LEN
        sta MBX_STAT            ; ignored by the TR (STAT is read-only there); only seeds the RAM mock in VICE
        jsr fillwin
        lda #<t1n
        ldx #>t1n
        jsr run1
        lda #<t2n
        ldx #>t2n
        jsr run2
        lda #<t3n
        ldx #>t3n
        jsr run3
        lda #<t4n
        ldx #>t4n
        jsr run4
        lda #<t5n
        ldx #>t5n
        jsr run5
        lda #<t6n
        ldx #>t6n
        jsr run6
        lda #<t7n
        ldx #>t7n
        jsr run7
        lda #<t8n
        ldx #>t8n
        jsr run8
        lda #<donen
        ldx #>donen
        jsr puts
        cli
        rts

; --- helpers -------------------------------------------------------------------------------------------------------
rec:    ora msk                 ; A = XOR of got and expected
        sta msk
        inc cnt
        bne :+
        inc cnt+1
:       rts

begin:  pha                     ; A/X = name, printed first
        txa
        pha
        lda #0
        sta cnt
        sta cnt+1
        sta msk
        lda #8                  ; each test runs 8 x 65536 accesses
        sta rep
        pla
        tax
        pla
        jmp puts

report: lda #' '
        jsr CHROUT
        lda cnt+1
        jsr hex
        lda cnt
        jsr hex
        lda #' '
        jsr CHROUT
        lda msk
        jsr hex
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
        adc #6                  ; carry is set here: +7 in total, ':' becomes 'A'
:       jmp CHROUT

puts:   sta $F7
        stx $F8
        ldy #0
:       lda ($F7),y
        beq :+
        jsr CHROUT
        iny
        bne :-
:       rts

fillwin: ldy #0
:       tya
        eor #$5C
        sta WIN,y
        iny
        bpl :-
        rts

; --- tests ---------------------------------------------------------------------------------------------------------
; T1: read ARG back to back (read, compare, loop)
run1:   jsr begin
@rp:    ldx #0
@lo:    ldy #0
@li:    lda MBX_ARG
        cmp #$A5
        bne @e
@r:     dey
        bne @li
        dex
        bne @lo
        dec rep
        bne @rp
        jmp report
@e:     eor #$A5
        jsr rec
        jmp @r

; T2: ARG, LEN, STAT alternately (three different registers back to back)
run2:   jsr begin
@rp:    ldx #0
@lo:    ldy #0
@li:    lda MBX_ARG
        cmp #$A5
        bne @e1
@a:     lda MBX_LEN
        cmp #$5A
        bne @e2
@b:     lda MBX_STAT
        cmp #$5A
        bne @e3
@r:     dey
        bne @li
        dex
        bne @lo
        dec rep
        bne @rp
        jmp report
@e1:    eor #$A5
        jsr rec
        jmp @a
@e2:    eor #$5A
        jsr rec
        jmp @b
@e3:    eor #$5A
        jsr rec
        jmp @r

; T3: window read, each byte compared right after the read (about 16 cycles apart)
run3:   jsr begin
@rp:    ldx #0
@lo:    ldy #0
@li:    lda WIN,y
        sta tmp
        tya
        eor #$5C
        eor tmp
        beq @ok
        jsr rec
@ok:    iny
        bpl @li
        dex
        bne @lo
        dec rep
        bne @rp
        jmp report

; T4: window read as fast as the CPU can (LDA abs / STA abs, 8 cycles per byte), checked afterwards
run4:   jsr begin
@rp:    ldx #0                  ; 256 passes of 128 reads
@lo:
.repeat 128, i
        lda WIN+i
        sta BUF+i
.endrepeat
        ldy #0
@ck:    tya
        eor #$5C
        eor BUF,y
        beq @ok
        jsr rec
@ok:    iny
        bpl @ck
        dex
        beq :+
        jmp @lo                 ; too far for a branch after the unrolled loop
:       dec rep
        beq :+
        jmp @rp
:       jmp report

; T5: write the window then read the same byte back immediately (write cycle followed by a read cycle)
run5:   jsr begin
@rp:    ldx #0
@lo:    ldy #0
@li:    tya
        eor #$5C
        sta WIN,y
        lda WIN,y
        sta tmp
        tya
        eor #$5C
        eor tmp
        beq @ok
        jsr rec
@ok:    iny
        bpl @li
        dex
        bne @lo
        dec rep
        bne @rp
        jmp report

; T6: write ARG, read it back at once (varying value)
run6:   jsr begin
@rp:    ldx #0
@lo:    ldy #0
@li:    tya
        sta MBX_ARG
        lda MBX_ARG
        sta tmp
        tya
        eor tmp
        beq @ok
        jsr rec
@ok:    dey
        bne @li
        dex
        bne @lo
        dec rep
        bne @rp
        lda #$A5
        sta MBX_ARG
        jmp report

; T7: window written with the fast unrolled stores, then verified: write errors only
run7:   jsr begin
@rp:    ldx #0
@lo:
.repeat 128, i
        lda BUF+i                       ; BUF holds whatever T4 left: the pattern, so the stores below are idempotent
        sta WIN+i
.endrepeat
        ldy #0
@ck:    tya
        eor #$5C
        eor WIN,y
        beq @ok
        jsr rec
@ok:    iny
        bpl @ck
        dex
        beq :+
        jmp @lo
:       dec rep
        beq :+
        jmp @rp
:       jmp report

; T8: read the KERNAL ROM the TR serves (stock ROM-serving path, no VDrive register involved): each byte twice in a row
run8:   jsr begin
@rp:    ldx #0
@lo:    ldy #0
@li:    lda $F000,y
        sta tmp
        lda $F000,y
        eor tmp
        beq @ok
        jsr rec
@ok:    iny
        bne @li
        dex
        bne @lo
        dec rep
        bne @rp
        jmp report

.segment "RODATA"
t1n:    .byte "T1 ARG READ        ", 0
t2n:    .byte "T2 ARG/LEN/STAT    ", 0
t3n:    .byte "T3 WIN READ        ", 0
t4n:    .byte "T4 WIN READ FAST   ", 0
t5n:    .byte "T5 WIN WR+RD       ", 0
t6n:    .byte "T6 ARG WR+RD       ", 0
t7n:    .byte "T7 WIN WRITE FAST  ", 0
t8n:    .byte "T8 KERNAL READ x2  ", 0
donen:  .byte "DONE (COUNT, BITS)", 13, 0
