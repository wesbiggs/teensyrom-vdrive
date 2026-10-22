; VDCOPY: copy files between any two drive units (real IEC drives, SD, USB), entirely in machine code.
; Loads at $0801 with a BASIC "10 SYS 2061" stub. Asks for a source and a destination unit, lists the source disk (PRG, SEQ, USR) and copies
; the selected files 2 KB at a time through the KERNAL (CHKIN/CHRIN from the source, CHKOUT/CHROUT to the destination), checking the
; destination's error channel after every chunk. Tables live at $2000-$37FF (BSS, not in the PRG), the copy buffer at $C000-$C7FF.
; Messages are upper case ASCII, which is also the default-mode PETSCII.
.setcpu "6502"

SETLFS  = $FFBA
SETNAM  = $FFBD
OPEN    = $FFC0
CLOSE   = $FFC3
CHKIN   = $FFC6
CHKOUT  = $FFC9
CLRCHN  = $FFCC
CHRIN   = $FFCF
CHROUT  = $FFD2
GETIN   = $FFE4
LISTEN  = $FFB1
SECOND  = $FF93
UNLSN   = $FFAE
STATUS  = $90
STOPKEY = $91           ; $7F while RUN/STOP is down
DEV0REG = $DE16         ; vdrive: current device number of the SD unit (0 = none)

P1      = $FB           ; $FB-$FE are free for the user
PTR     = $FB           ; chunk copy pointer
P2      = $FD
END     = $FD           ; chunk end pointer (only while a chunk runs)
BUF     = $C000         ; 2 KB buffer $C000-$C7FF
BUFEND  = $C8
MAXF    = 254           ; files kept from a directory
LINEMAX = 62
MDOS    = 4             ; message kinds for a failed copy
MREAD   = 3
MWRITE  = 2
MCHAN   = 1
MSTOP   = 5

.macro PS str
        jsr pstr
        .byte str, 0
.endmacro
.macro CR
        lda #13
        jsr CHROUT
.endmacro

.segment "LOADADDR"
        .word $0801
.segment "BASIC"
        .word @next
        .word 10
        .byte $9E, "2061", 0
@next:  .word 0

.segment "CODE"
start:
.assert start = 2061, error, "the SYS address in the BASIC stub must match"
        PS {$93, "VDCOPY - COPY FILES BETWEEN TWO DRIVES", 13}
        lda #8
        jsr probe               ; the first bus access also runs the vdrive scan that numbers the units
        lda DEV0REG
        cmp #8
        bcc @d9
        cmp #31
        bcc @dok
@d9:    lda #9
@dok:   sta DU                  ; destination default: the SD unit
        lda #8
        cmp DU
        bne @s8
        lda #9
@s8:    sta SU                  ; source default: 8 (9 when that is the destination)

ask:    PS "COPY FROM UNIT ("
        lda SU
        jsr pbyte0
        PS "): "
        jsr readline
        CR
        lda INLEN
        beq @1
        jsr parse_unit
        sta SU
@1:     PS "COPY TO UNIT ("
        lda DU
        jsr pbyte0
        PS "): "
        jsr readline
        CR
        lda INLEN
        beq @2
        jsr parse_unit
        sta DU
@2:     lda SU
        cmp #8
        bcc @bad
        cmp #31
        bcs @bad
        lda DU
        cmp #8
        bcc @bad
        cmp #31
        bcs @bad
        lda SU
        cmp DU
        beq @bad
        jsr probe
        bne @nosu
        lda DU
        jsr probe
        bne @nodu
        jmp @go
@bad:   PS {"BAD UNIT NUMBERS", 13}
        jmp ask
@nosu:  PS "NOTHING ON UNIT "
        lda SU
        jmp @nomsg
@nodu:  PS "NOTHING ON UNIT "
        lda DU
@nomsg: jsr pbyte0
        CR
        jmp ask

@go:    lda #15
        ldx SU
        ldy #15
        jsr openc
        lda #14
        ldx DU
        ldy #15
        jsr openc
        PS "READING DIRECTORY OF UNIT "
        lda SU
        jsr pbyte0
        CR
        jsr readdir
        lda NF
        bne @list
        PS {"NO FILES TO COPY", 13}
        jmp quit
@list:  jsr listing

prompt: PS {13, "COPY: A=ALL, OR 1,3,5-8 (RETURN=QUIT)", 13}
        jsr readline
        CR
        lda INLEN
        bne @sel0
        jmp quit
@sel0:  jsr parse_sel
        lda NM
        bne @sel
        PS {"NOTHING SELECTED", 13}
        jmp prompt
@sel:   jsr pbyte0
        PS " SELECTED. REPLACE EXISTING (Y/N)?"
@yn:    jsr getkey
        cmp #$59
        beq @y
        cmp #$D9
        beq @y
        cmp #$4E
        beq @n
        cmp #$CE
        bne @yn
@n:     ldx #0
        beq @st
@y:     ldx #1
@st:    stx OW
        jsr CHROUT              ; echo the key
        CR
        CR
        lda #0
        sta NC
        sta NX
        sta NR
        sta AB
        sta IDX
@cl:    ldx IDX
        lda SEL,x
        beq @skip
        jsr copy_file
        lda AB
        bne @done
@skip:  inc IDX
        lda IDX
        cmp NF
        bcc @cl
@done:  CR
        lda NC
        jsr pbyte0
        PS " COPIED, "
        lda NX
        jsr pbyte0
        PS " SKIPPED, "
        lda NR
        jsr pbyte0
        PS " FAILED"
        CR
        jmp prompt

quit:   lda #14
        jsr CLOSE
        lda #15
        jsr CLOSE
        jmp CLRCHN

; ---------------------------------------------------------------------------------------------------------------------------------
; Is anything listening on unit A? LISTEN + SECOND, as the KERNAL bus scan does it. Returns A = ST (Z set = present).
probe:  jsr LISTEN
        lda #$6F
        jsr SECOND
        lda STATUS
        pha
        bne @gone
        jsr UNLSN
@gone:  lda #0
        sta STATUS
        pla
        rts

; open logical file A on device X, secondary Y, without a name (the command channel)
openc:  jsr SETLFS
        lda #0
        jsr SETNAM
        jmp OPEN

; open logical file A on device X, secondary Y, with the name in CMD (CMDLEN bytes)
openn:  jsr SETLFS
        lda CMDLEN
        ldx #<CMD
        ldy #>CMD
        jsr SETNAM
        jmp OPEN

; wait for a key
getkey: jsr GETIN
        beq getkey
        rts

; print the string that follows the JSR (zero terminated)
pstr:   pla
        sta P1
        pla
        sta P1+1
        ldy #1
@l:     lda (P1),y
        beq @d
        jsr CHROUT
        iny
        bne @l
@d:     tya
        clc
        adc P1
        tax
        lda P1+1
        adc #0
        pha
        txa
        pha
        rts

; NUM (24 bit) / 10 -> NUM, remainder in A
div10:  ldx #24
        lda #0
@l:     asl NUM
        rol NUM+1
        rol NUM+2
        rol a
        cmp #10
        bcc @n
        sbc #10
        inc NUM
@n:     dex
        bne @l
        rts

; print NUM in decimal, right justified in A columns
pdec:   sta WID
        ldy #0
@div:   jsr div10
        clc
        adc #'0'
        pha
        iny
        lda NUM
        ora NUM+1
        ora NUM+2
        bne @div
        sty CNT
        lda WID
        sec
        sbc CNT
        bcc @out
        beq @out
        sta PADC
@pad:   lda #' '
        jsr CHROUT
        dec PADC
        bne @pad
@out:   pla
        jsr CHROUT
        dec CNT
        bne @out
        rts

; print A in decimal
pbyte0: sta NUM
        lda #0
        sta NUM+1
        sta NUM+2
        jmp pdec

; ---------------------------------------------------------------------------------------------------------------------------------
; Line input into INBUF (zero terminated, INLEN characters): digits , - and A; DEL edits; RETURN ends.
readline:
        lda #0
        sta INLEN
        sta INBUF
@cur:   PS {$12, " ", $92, $9D}        ; a reverse-video cursor
        jsr getkey
        sta CH
        PS {" ", $9D}
        lda CH
        cmp #13
        beq @ret
        cmp #$14
        bne @chr
        lda INLEN
        beq @cur
        dec INLEN
        ldy INLEN
        lda #0
        sta INBUF,y
        lda #$14
        jsr CHROUT
        jmp @cur
@chr:   cmp #','
        beq @acc
        cmp #'-'
        beq @acc
        cmp #'0'
        bcc @cur
        cmp #':'
        bcc @acc
        cmp #$41
        beq @acc
        cmp #$C1
        bne @cur
@acc:   ldy INLEN
        cpy #31
        bcs @cur
        sta INBUF,y
        iny
        lda #0
        sta INBUF,y
        inc INLEN
        lda CH
        jsr CHROUT
        jmp @cur
@ret:   rts

; INBUF digits -> A (0 = not a valid number)
parse_unit:
        lda #0
        sta TMP
        tay
@l:     lda INBUF,y
        beq @d
        sec
        sbc #'0'
        cmp #10
        bcs @bad
        sta T1
        lda TMP
        cmp #25
        bcs @bad
        asl a
        sta T2
        asl a
        asl a
        clc
        adc T2
        clc
        adc T1
        sta TMP
        iny
        bne @l
@d:     lda TMP
        rts
@bad:   lda #0
        rts

; ---------------------------------------------------------------------------------------------------------------------------------
; INBUF -> SEL[], NM: "A" selects all, otherwise numbers and ranges (1,3,5-8; "5-" runs to the end).
parse_sel:
        lda #0
        tax
@clr:   sta SEL,x
        inx
        cpx #MAXF
        bcc @clr
        sta NM
        lda INBUF
        cmp #$41
        beq @all
        cmp #$C1
        bne @tok
@all:   ldx #0
        lda #1
@a1:    sta SEL,x
        inx
        cpx NF
        bcc @a1
        lda NF
        sta NM
        rts
@tok:   lda #0
        sta V
        sta V+1
        sta LO
        sta LO+1
        sta RG
        sta PI
@lp:    ldy PI
        lda INBUF,y
        sta CH
        sec
        sbc #'0'
        cmp #10
        bcs @nd
        sta T1
        lda V+1                 ; V >= 1024: stays out of range
        cmp #4
        bcs @adv
        lda V                   ; V = V * 10 + digit
        sta T2
        lda V+1
        sta T3
        asl V
        rol V+1
        asl V
        rol V+1
        clc
        lda V
        adc T2
        sta V
        lda V+1
        adc T3
        sta V+1
        asl V
        rol V+1
        clc
        lda V
        adc T1
        sta V
        bcc @adv
        inc V+1
@adv:   inc PI
        jmp @lp
@nd:    lda CH
        cmp #'-'
        bne @sep
        lda V
        sta LO
        lda V+1
        sta LO+1
        lda #0
        sta V
        sta V+1
        lda #1
        sta RG
        jmp @adv
@sep:   jsr dorange
        lda #0
        sta V
        sta V+1
        sta LO
        sta LO+1
        sta RG
        lda CH
        bne @adv
        rts

dorange:
        lda V
        sta HI
        lda V+1
        sta HI+1
        lda RG
        bne @r
        lda V
        sta LO
        lda V+1
        sta LO+1
        jmp @chk
@r:     lda V                   ; "5-": up to the last file
        ora V+1
        bne @chk
        lda NF
        sta HI
        lda #0
        sta HI+1
@chk:   lda LO+1
        bne @out
        lda LO
        beq @out
        lda NF
        cmp LO
        bcc @out                ; lo > nf
        lda HI+1
        bne @cl
        lda NF
        cmp HI
        bcs @ok
@cl:    lda NF
        sta HI
@ok:    lda HI
        cmp LO
        bcc @out
        ldx LO
        dex
@m:     lda SEL,x
        bne @s
        lda #1
        sta SEL,x
        inc NM
@s:     inx
        cpx HI
        bcc @m
@out:   rts

; ---------------------------------------------------------------------------------------------------------------------------------
; NAMES + A*16 -> P2
nameptr:
        sta P2
        lda #0
        sta P2+1
        ldx #4
@s:     asl P2
        rol P2+1
        dex
        bne @s
        lda P2+1
        clc
        adc #>NAMES
        sta P2+1
        rts

; Read the directory of unit SU: NF files (P, S, U only) into NAMES/NLEN/TYP/BLO/BHI, the header line into HDR, SK others skipped.
readdir:
        lda #0
        sta NF
        sta SK
        sta HAVEHDR
        lda #1
        ldx SU
        ldy #0
        jsr SETLFS
        lda #1
        ldx #<dollar
        ldy #>dollar
        jsr SETNAM
        jsr OPEN
        lda #0
        sta STATUS
        ldx #1
        jsr CHKIN
        jsr CHRIN               ; load address
        jsr CHRIN
@next:  jsr CHRIN               ; link
        sta T0
        jsr CHRIN
        ora T0
        sta T0
        lda STATUS
        and #$40
        beq @e4x
        jmp @end
@e4x:
        lda T0
        bne @e3x
        jmp @end
@e3x:
        lda STATUS
        and #$BF
        beq @e2x
        jmp @err
@e2x:
        jsr CHRIN               ; blocks
        sta BL
        jsr CHRIN
        sta BH
        lda #0
        sta LLEN
@rl:    jsr CHRIN
        sta CH
        lda STATUS
        and #$BF
        beq @e1x
        jmp @err
@e1x:
        lda CH
        beq @eol
        ldy LLEN
        cpy #LINEMAX
        bcs @rl
        sta LINE,y
        inc LLEN
        jmp @rl
@eol:   ldy LLEN
        lda #0
        sta LINE,y
        lda HAVEHDR
        bne @entry
        lda #1
        sta HAVEHDR
        ldy #0                  ; the first line is the disk header
@h:     lda LINE,y
        sta HDR,y
        beq @hd
        iny
        cpy #39
        bcc @h
        lda #0
        sta HDR,y
@hd:    jmp @next
@entry: lda #0
        sta Q1
        sta Q2
        tay
@q:     cpy LLEN
        bcs @qd
        lda LINE,y
        cmp #34
        bne @qn
        tya
        clc
        adc #1                  ; 1-based position
        sta Q2
        ldx Q1
        bne @qn
        sta Q1
@qn:    iny
        bne @q
@qd:    lda Q1
        cmp Q2
        bcc @q1
        jmp @sk
@q1:
        ldy Q2                  ; the first letter after the closing quote is the type
@t:     cpy LLEN
        bcs @notype
        lda LINE,y
        cmp #$41
        bcc @tn
        cmp #$5B
        bcc @gott
@tn:    iny
        bne @t
@notype:lda #0
@gott:  sta TC
        cmp #'P'
        beq @v
        cmp #'S'
        beq @v
        cmp #'U'
        bne @sk
@v:     lda NF
        cmp #MAXF
        bcs @sk
        ldx NF
        lda TC
        sta TYP,x
        lda BL
        sta BLO,x
        lda BH
        sta BHI,x
        lda Q2
        sec
        sbc Q1
        sbc #1
        cmp #17
        bcc @ln
        lda #16
@ln:    sta NLEN,x
        lda NF
        jsr nameptr
        ldx NF
        lda NLEN,x
        sta NAMLEN
        ldy #0
@cp:    cpy NAMLEN
        bcs @cpd
        sty TMPI
        tya
        clc
        adc Q1
        tay
        lda LINE,y              ; the name starts at 0-based index Q1
        ldy TMPI
        sta (P2),y
        iny
        bne @cp
@cpd:   inc NF
        jmp @next
@sk:    lda SK
        cmp #255
        beq @skd
        inc SK
@skd:   jmp @next
@err:   PS {"DIRECTORY READ ERROR", 13}
        lda #0
        sta NF
@end:   lda #1
        jsr CLOSE
        jmp CLRCHN

dollar: .byte "$"

; Show the directory: header, count, one line per file, a key press after each screenful.
listing:
        PS {$93}
        ldy #0
@hl:    lda HDR,y
        beq @hd
        jsr CHROUT
        iny
        bne @hl
@hd:    CR
        lda NF
        jsr pbyte0
        PS " FILES"
        lda SK
        beq @nosk
        PS ", "
        lda SK
        jsr pbyte0
        PS " NOT LISTED (REL, DEL OR TOO MANY)"
@nosk:  CR
        lda #3
        sta LN
        lda #0
        sta LI
@row:   lda LI
        clc
        adc #1
        sta NUM
        lda #0
        sta NUM+1
        sta NUM+2
        lda #3
        jsr pdec
        PS {" ", 34}
        ldx LI
        lda NLEN,x
        sta NAMLEN
        lda LI
        jsr nameptr
        ldy #0
@nm:    cpy NAMLEN
        bcs @np
        lda (P2),y
        jsr CHROUT
        iny
        bne @nm
@np:    PS {34}
        lda #16
        sec
        sbc NAMLEN
        sta PADC
@pd:    lda PADC
        beq @pe
        lda #' '
        jsr CHROUT
        dec PADC
        jmp @pd
@pe:    PS " "
        ldx LI
        lda TYP,x
        jsr CHROUT
        ldx LI
        lda BLO,x
        sta NUM
        lda BHI,x
        sta NUM+1
        lda #0
        sta NUM+2
        lda #4
        jsr pdec
        CR
        inc LN
        lda LN
        cmp #23
        bcc @nx
        lda LI
        clc
        adc #1
        cmp NF
        bcs @nx                 ; no pause after the last line
        PS "-- PRESS A KEY --"
        jsr getkey
        CR
        lda #0
        sta LN
@nx:    inc LI
        lda LI
        cmp NF
        bcs @ret
        jmp @row
@ret:   rts

; ---------------------------------------------------------------------------------------------------------------------------------
; Read the error channel of logical file A (15 = source, 14 = destination): ERRBUF = "NN,MESSAGE,TT,SS", ENUM = NN (255 = not understood).
rderr:  tax
        jsr CHKIN
        lda #0
        sta STATUS
        sta EI
@l:     jsr CHRIN
        ldy EI
        cmp #13
        beq @e
        sta ERRBUF,y
        inc EI
        lda STATUS
        bne @e
        lda EI
        cmp #46
        bcc @l
@e:     ldy EI
        lda #0
        sta ERRBUF,y
        sty ERRLEN
        jsr CLRCHN
        lda ERRBUF
        sec
        sbc #'0'
        cmp #10
        bcs @bad
        sta TMP
        asl a
        asl a
        adc TMP
        asl a
        sta TMP
        lda ERRBUF+1
        sec
        sbc #'0'
        cmp #10
        bcs @bad
        clc
        adc TMP
        sta ENUM
        rts
@bad:   lda #255
        sta ENUM
        rts

penum:  lda ENUM
        jmp pbyte0

; the message part of the error channel text (between the first and the second comma)
pemsg:  lda ERRLEN
        cmp #4
        bcc @d
        lda #3
        sta TMPI
@l:     ldy TMPI
        lda ERRBUF,y
        beq @d
        cmp #','
        beq @d
        jsr CHROUT
        inc TMPI
        bne @l
@d:     rts

; ---------------------------------------------------------------------------------------------------------------------------------
; file IDX: NAMBUF/NAMLEN/TYPC from the tables
setname:
        lda IDX
        jsr nameptr
        ldx IDX
        lda NLEN,x
        sta NAMLEN
        lda TYP,x
        sta TYPC
        ldy #0
@c:     cpy NAMLEN
        bcs @d
        lda (P2),y
        sta NAMBUF,y
        iny
        bne @c
@d:     rts

; CMD = "NAME,T,A" (A = R or W)
mkcmd:  sta MODE
        ldy #0
@c:     cpy NAMLEN
        bcs @t
        lda NAMBUF,y
        sta CMD,y
        iny
        bne @c
@t:     lda #','
        sta CMD,y
        iny
        lda TYPC
        sta CMD,y
        iny
        lda #','
        sta CMD,y
        iny
        lda MODE
        sta CMD,y
        iny
        sty CMDLEN
        rts

; send "S:NAME" to the destination's command channel
scratch:
        ldx #14
        jsr CHKOUT
        lda #'S'
        jsr CHROUT
        lda #':'
        jsr CHROUT
        lda #0
        sta TMPI
@c:     lda TMPI
        cmp NAMLEN
        bcs @e
        tay
        lda NAMBUF,y
        jsr CHROUT
        inc TMPI
        bne @c
@e:     lda #13
        jsr CHROUT
        jmp CLRCHN

; why a copy failed (MK)
pmsg:   lda MK
        cmp #MCHAN
        bne @a
        PS "CHANNEL ERROR "
        lda RESEP
        jmp pbyte0
@a:     cmp #MWRITE
        bne @b
        PS "WRITE FAILED, ST "
        lda RESWS
        jmp pbyte0
@b:     cmp #MREAD
        bne @c
        PS "READ FAILED, ST "
        lda RESRS
        jmp pbyte0
@c:     cmp #MDOS
        bne @d
        jsr penum
        PS " "
        jmp pemsg
@d:     cmp #MSTOP
        bne @e
        PS "STOPPED"
        rts
@e:     PS "ERROR"
        rts

; Copy file IDX from unit SU to unit DU.
copy_file:
        jsr setname
        lda IDX
        clc
        adc #1
        sta NUM
        lda #0
        sta NUM+1
        sta NUM+2
        lda #3
        jsr pdec
        PS {" ", 34}
        ldy #0
@pn:    cpy NAMLEN
        bcs @pe
        lda NAMBUF,y
        jsr CHROUT
        iny
        bne @pn
@pe:    PS {34, " "}
        ldy #0
@bd:    cpy NAMLEN              ; names the command syntax cannot carry
        bcs @okname
        lda NAMBUF,y
        cmp #'*'
        beq @odd
        cmp #','
        beq @odd
        cmp #':'
        beq @odd
        cmp #'='
        beq @odd
        cmp #'?'
        beq @odd
        iny
        bne @bd
@odd:   PS {"SKIPPED (ODD NAME)", 13}
        inc NX
        rts
@okname:lda #'R'
        jsr mkcmd
        lda #2
        ldx SU
        ldy #2
        jsr openn
        lda #15
        jsr rderr
        lda ENUM
        beq @ropen
        lda #2
        jsr CLOSE
        PS "READ ERROR "
        jsr penum
        CR
        inc NR
        rts
@ropen: lda #'W'
        jsr mkcmd
        lda #3
        ldx DU
        ldy #3
        jsr openn
        lda #14
        jsr rderr
        lda ENUM
        cmp #63                 ; file exists
        bne @chk
        lda OW
        beq @chk
        lda #3
        jsr CLOSE
        jsr scratch
        lda #14
        jsr rderr
        lda #3
        ldx DU
        ldy #3
        jsr openn
        lda #14
        jsr rderr
@chk:   lda ENUM
        beq @copy
        lda #3
        jsr CLOSE
        lda #2
        jsr CLOSE
        lda ENUM
        cmp #63
        bne @werr
        PS {"EXISTS", 13}
        inc NX
        rts
@werr:  PS "WRITE ERROR "
        jsr penum
        PS " "
        jsr pemsg
        CR
        inc NR
        rts

@copy:  lda #0
        sta BT
        sta BT+1
        sta BT+2
@chunk: jsr chunk
        clc
        lda BT
        adc RESN
        sta BT
        lda BT+1
        adc RESN+1
        sta BT+1
        lda BT+2
        adc #0
        sta BT+2
        lda #'.'
        jsr CHROUT
        lda #14
        jsr rderr
        lda RESEP
        ora RESWS
        bne @err
        lda RESRS
        and #$BF
        bne @err
        lda ENUM
        bne @err
        lda RESRS
        and #$40
        bne @fin                ; EOF: everything is copied
        lda STOPKEY
        cmp #$7F
        bne @chunk
        lda #1
        sta AB
        lda #MSTOP
        sta MK
        jmp @fail
@err:   lda #0
        sta MK
        lda RESEP
        beq @e1
        lda #MCHAN
        sta MK
@e1:    lda RESWS
        beq @e2
        lda #MWRITE
        sta MK
@e2:    lda RESRS
        and #$BF
        beq @e3
        lda #MREAD
        sta MK
@e3:    lda ENUM
        beq @fail
        lda #MDOS
        sta MK
        jmp @fail
@fin:   lda #3
        jsr CLOSE
        lda #2
        jsr CLOSE
        lda #14
        jsr rderr
        lda ENUM
        beq @ok
        lda #MDOS
        sta MK
        jmp @fail
@ok:    PS " OK "
        lda BT
        sta NUM
        lda BT+1
        sta NUM+1
        lda BT+2
        sta NUM+2
        lda #0
        jsr pdec
        PS {" BYTES", 13}
        inc NC
        rts
@fail:  lda #3
        jsr CLOSE
        lda #2
        jsr CLOSE
        PS " "
        jsr pmsg
        CR
        jsr scratch             ; remove the partial copy
        lda #14
        jsr rderr
        inc NR
        rts

; Copy one chunk (up to 2 KB) from logical file 2 to logical file 3 through the KERNAL.
; Results: RESN bytes copied, RESRS ST of the read, RESWS ST of the write, RESEP KERNAL error from CHKIN/CHKOUT (0 = none).
; A read ST of $40 (EOF) alone is normal: the last byte is valid and counted.
chunk:  lda #0
        sta RESN
        sta RESN+1
        sta RESRS
        sta RESWS
        sta RESEP
        ldx #2
        jsr CHKIN
        bcc @a
        sta RESEP
        jmp CLRCHN
@a:     lda #0
        sta STATUS              ; CHRIN returns CR without reading while ST <> 0
        sta PTR
        lda #>BUF
        sta PTR+1
@rd:    jsr CHRIN
        ldy #0
        sta (PTR),y
        lda STATUS
        sta RESRS
        and #$BF
        bne @rdend              ; error: this byte is not valid, PTR stays
        inc PTR
        bne @b
        inc PTR+1
@b:     lda RESRS
        bne @rdend              ; EOF: the byte is valid and counted
        lda PTR+1
        cmp #BUFEND
        bcc @rd
@rdend: jsr CLRCHN
        lda PTR
        sta END
        sta RESN
        lda PTR+1
        sta END+1
        sec
        sbc #>BUF
        sta RESN+1
        ora RESN
        bne @w
        rts                     ; nothing to write
@w:     ldx #3
        jsr CHKOUT
        bcc @b2
        sta RESEP
        jmp CLRCHN
@b2:    lda #0
        sta STATUS
        sta PTR
        lda #>BUF
        sta PTR+1
@wr:    ldy #0
        lda (PTR),y
        jsr CHROUT
        inc PTR
        bne @c
        inc PTR+1
@c:     lda PTR
        cmp END
        bne @wr
        lda PTR+1
        cmp END+1
        bne @wr
        jsr CLRCHN              ; UNLISTEN flushes the last byte
        lda STATUS
        sta RESWS
        rts

; ---------------------------------------------------------------------------------------------------------------------------------
.segment "BSS"
NAMES:  .res MAXF*16
NLEN:   .res MAXF
TYP:    .res MAXF
BLO:    .res MAXF
BHI:    .res MAXF
SEL:    .res MAXF
HDR:    .res 40
LINE:   .res 64
INBUF:  .res 34
ERRBUF: .res 48
CMD:    .res 40
NAMBUF: .res 17
SU:     .res 1
DU:     .res 1
NF:     .res 1
SK:     .res 1
NM:     .res 1
NC:     .res 1
NX:     .res 1
NR:     .res 1
AB:     .res 1
OW:     .res 1
IDX:    .res 1
INLEN:  .res 1
T0:     .res 1
T1:     .res 1
T2:     .res 1
T3:     .res 1
BL:     .res 1
BH:     .res 1
LLEN:   .res 1
Q1:     .res 1
Q2:     .res 1
TC:     .res 1
HAVEHDR:.res 1
CH:     .res 1
TMP:    .res 1
TMPI:   .res 1
EI:     .res 1
PI:     .res 1
LI:     .res 1
LN:     .res 1
ENUM:   .res 1
ERRLEN: .res 1
MK:     .res 1
NAMLEN: .res 1
TYPC:   .res 1
MODE:   .res 1
CMDLEN: .res 1
WID:    .res 1
CNT:    .res 1
PADC:   .res 1
RG:     .res 1
NUM:    .res 3
BT:     .res 3
V:      .res 2
LO:     .res 2
HI:     .res 2
RESN:   .res 2
RESRS:  .res 1
RESWS:  .res 1
RESEP:  .res 1
