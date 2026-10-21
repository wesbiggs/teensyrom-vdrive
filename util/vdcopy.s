; vdcopy helper: copy one chunk (up to 2 KB) from logical file 2 to logical file 3 through the KERNAL.
; Loaded by vdcopy.bas into the cassette buffer ($033C, SYS 828 per chunk). Results at $03F0:
;   +0/+1 bytes copied (lo/hi)   +2 ST of the read   +3 ST of the write   +4 KERNAL error from CHKIN/CHKOUT (0 = none)
; A read ST of $40 (EOF) alone is normal: the last byte is valid and counted.
CHKIN  = $FFC6
CHKOUT = $FFC9
CLRCHN = $FFCC
CHRIN  = $FFCF
CHROUT = $FFD2
STATUS = $90
PTR    = $FB            ; $FB-$FE are free for the user
END    = $FD
BUF    = $C000          ; 2 KB buffer $C000-$C7FF
BUFEND = $C8
RES    = $03F0
SRCLF  = 2
DSTLF  = 3

        .segment "CODE"
        lda #0                  ; SYS 828
        sta RES+2
        sta RES+3
        sta RES+4
        ldx #SRCLF
        jsr CHKIN
        bcc :+
        sta RES+4
        jmp stop
:       lda #0
        sta STATUS              ; CHRIN returns CR without reading while ST <> 0
        sta PTR
        lda #>BUF
        sta PTR+1
rd:     jsr CHRIN
        ldy #0
        sta (PTR),y
        lda STATUS
        sta RES+2
        and #$BF
        bne rdend               ; error: this byte is not valid, PTR stays
        inc PTR
        bne :+
        inc PTR+1
:       lda RES+2
        bne rdend               ; EOF: the byte is valid and counted
        lda PTR+1
        cmp #BUFEND
        bcc rd
rdend:  jsr CLRCHN
        lda PTR
        sta END
        sta RES
        lda PTR+1
        sta END+1
        sec
        sbc #>BUF
        sta RES+1
        ora RES
        beq stop                ; nothing to write
        ldx #DSTLF
        jsr CHKOUT
        bcc :+
        sta RES+4
        jmp stop
:       lda #0
        sta STATUS
        sta PTR
        lda #>BUF
        sta PTR+1
wr:     ldy #0
        lda (PTR),y
        jsr CHROUT
        inc PTR
        bne :+
        inc PTR+1
:       lda PTR
        cmp END
        bne wr
        lda PTR+1
        cmp END+1
        bne wr
        jsr CLRCHN              ; UNLISTEN flushes the last byte
        lda STATUS
        sta RES+3
        rts
stop:   jmp CLRCHN

; While copying, the STOP vector ($0328/$0329, normally $F6ED) is pointed here with a single POKE 809,3 (a two-byte change
; could be hit half-done by BASIC's own STOP check): never "pressed" (Z clear); vdcopy.bas polls $91 itself and aborts cleanly.
        .segment "STUB"
        lda #1
        rts
