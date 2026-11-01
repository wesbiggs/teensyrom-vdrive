; VDCOPY: copy files or whole disks between any two drive units (real IEC drives, SD, USB), entirely in machine code.
; Loads at $0801 with a BASIC "10 SYS 2061" stub. A menu offers:
; F  files: asks for a source and a destination unit, lists the source disk (PRG, SEQ, USR) and copies the selected files 2 KB at a
;    time through the KERNAL (CHKIN/CHRIN from the source, CHKOUT/CHROUT to the destination), checking the destination's error channel
;    after every chunk.
; D  a whole disk: the 683 sectors of a 35-track 1541 disk, one at a time through a buffer channel on each side ("#", U1 on the source,
;    B-P + U2 on the destination), so a .D64 image goes onto a real floppy or a floppy into a new .D64 image (any two units work). A vdrive
;    source may first mount an image (CD:path); a vdrive destination gets a new image (N:NAME.D64) or the mounted one is written over; a real
;    destination may be formatted first with the source's disk name and ID. Read errors are copied as they come and counted; the screen
;    shows a map of the sectors (columns are tracks).
; Tables live at $2800-$3FFF (BSS, not in the PRG), the copy buffer at $C000-$C7FF (in disk mode: the sector at $C000, track 18 sector 0
; of the source at $C100). Messages are upper case ASCII, which is also the default-mode PETSCII (the upper-case character set is kept).
; Colours follow the TR menu, as in VDrive Config: its seven colour references come as the third line of "!STATE" from a vdrive unit (the
; menu's defaults otherwise). Bytes C_BAN..C_NAME in a printed string select one; a reverse banner, keys in reverse in the option colour.
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
PLOT    = $FFF0
STATUS  = $90
STOPKEY = $91           ; $7F while RUN/STOP is down
TXTCOL  = $0286
BORDER  = $D020
BGCOL   = $D021
.ifndef DEV0REG
DEV0REG = $DE16         ; vdrive: current device number of the SD unit (0 = none), $DE17 the USB unit's (the VICE mock: -D DEV0REG=$C806)
.endif
SCREEN  = $0400
COLRAM  = $D800

P1      = $FB           ; $FB-$FE are free for the user
PTR     = $FB           ; chunk copy pointer
P2      = $FD
END     = $FD           ; chunk end pointer (only while a chunk runs)
BUF     = $C000         ; 2 KB buffer $C000-$C7FF
BUFEND  = $C8
HBLK    = $C100         ; disk mode: track 18 sector 0 of the source (disk name at +$90, ID at +$A2)
MAXF    = 254           ; files kept from a directory
LINEMAX = 62
INMAX   = 60            ; line input
CMDMAX  = 80
C_BAN   = 3             ; string bytes 3-7: colour reference 2-6 of the TR menu (THEME: background, border, banner, time, option, sources, name)
C_TIME  = 4
C_OPT   = 5
C_SRC   = 6
C_NAME  = 7
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
.macro CMDS str         ; CMD = str
        jsr cmdbeg
        jsr cmdps
        .byte str, 0
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
        lda BORDER              ; the colours to give back on Q
        and #15
        sta OC
        lda BGCOL
        and #15
        sta OB
        lda TXTCOL
        and #15
        sta OT
        PS {142, 8}             ; the upper-case character set, and Shift+C= may not switch it
        lda #8
        jsr probe               ; the first bus access also runs the vdrive scan that numbers the units
        ldx #1
@dv:    lda DEV0REG,x           ; the vdrive units (0 = none or no vdrive)
        cmp #8
        bcc @no
        cmp #31
        bcc @ok
@no:    lda #0
@ok:    sta VD0,x
        dex
        bpl @dv
        jsr readtheme
        lda THEME+1
        sta BORDER
        lda THEME
        sta BGCOL
        lda VD0
        bne @dok
        lda #9
@dok:   sta DU                  ; destination default: the SD unit
        lda #8
        cmp DU
        bne @s8
        lda #9
@s8:    sta SU                  ; source default: 8 (9 when that is the destination)

menu:   jsr banner
        PS {C_SRC, "COPY BETWEEN ANY TWO UNITS: REAL DRIVES,", 13, "THE SD CARD AND THE USB STICK.", 13, 13}
        jsr punits
        PS {13, 18, C_OPT, " F ", 146, C_SRC, " COPY FILES", 13, 13, 18, C_OPT, " D ", 146, C_SRC, " COPY A WHOLE DISK, SECTOR BY SECTOR", 13}
        PS {"    (.D64 IMAGE TO FLOPPY AND BACK)", 13, 13, 18, C_OPT, " Q ", 146, C_SRC, " QUIT", 13}
@k:     jsr getkey
        cmp #'F'
        bne @1
        jmp files
@1:     cmp #'D'
        bne @2
        jmp disk
@2:     cmp #'Q'
        bne @k
        lda OC
        sta BORDER
        lda OB
        sta BGCOL
        lda OT
        sta TXTCOL
        PS {$93}
        rts

; clear the screen and draw the banner across row 0 (its 40th column straight into the screen: printing it would link rows 0 and 1)
banner: PS {C_NAME, $93, C_BAN, 18, "                VDCOPY                 ", 146, 13}
        lda #$A0
        sta SCREEN+39
        lda THEME+2
        sta COLRAM+39
        rts

; "VDRIVE: SD UNIT 8  USB UNIT 9" / "VDRIVE: NOT ACTIVE"
punits: PS {C_SRC, "VDRIVE: "}
        lda VD0
        ora VD1
        bne @a
        PS {C_TIME, "NOT ACTIVE", 13}
        rts
@a:     lda VD0
        beq @u
        PS {"SD UNIT ", C_NAME}
        lda VD0
        jsr pbyte0
        PS {C_SRC, "  "}
@u:     lda VD1
        beq @e
        PS {"USB UNIT ", C_NAME}
        lda VD1
        jsr pbyte0
@e:     CR
        rts

; Z set when unit A is a vdrive unit
isvd:   cmp VD0
        beq @y
        cmp VD1
@y:     rts

; ---------------------------------------------------------------------------------------------------------------------------------
; Ask for the source and destination units (SU, DU) until both are valid, different and answer on the bus.
askunits:
        PS {C_SRC, "COPY FROM UNIT ("}
        lda SU
        jsr pbyte0
        PS {"): ", C_NAME}
        jsr readline
        CR
        lda INLEN
        beq @1
        jsr parse_unit
        sta SU
@1:     PS {C_SRC, "COPY TO UNIT ("}
        lda DU
        jsr pbyte0
        PS {"): ", C_NAME}
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
        rts
@bad:   PS {C_TIME, "BAD UNIT NUMBERS", 13}
        jmp askunits
@nosu:  PS {C_TIME, "NOTHING ON UNIT "}
        lda SU
        jmp @nomsg
@nodu:  PS {C_TIME, "NOTHING ON UNIT "}
        lda DU
@nomsg: jsr pbyte0
        CR
        jmp askunits

; the command channels: 15 on the source, 14 on the destination
openchans:
        lda #15
        ldx SU
        ldy #15
        jsr openc
        lda #14
        ldx DU
        ldy #15
        jmp openc

; close everything and go back to the menu
waitback:
        PS {C_SRC, "PRESS A KEY", 13}
        jsr getkey
back:   lda #3
        jsr CLOSE
        lda #2
        jsr CLOSE
        lda #14
        jsr CLOSE
        lda #15
        jsr CLOSE
        jsr CLRCHN
        jmp menu

; ---------------------------------------------------------------------------------------------------------------------------------
files:  jsr banner
        PS {C_SRC, "COPY FILES (PRG, SEQ, USR)", 13, 13}
        jsr askunits
        jsr openchans
        PS {C_SRC, "READING DIRECTORY OF UNIT "}
        lda SU
        jsr pbyte0
        CR
        jsr readdir
        lda NF
        bne @list
        PS {C_TIME, "NO FILES TO COPY", 13}
        jmp waitback
@list:  jsr listing

prompt: PS {13, C_SRC, "COPY: A=ALL, OR 1,3,5-8 (RETURN=MENU)", 13, C_NAME}
        jsr readline
        CR
        lda INLEN
        bne @sel0
        jmp back
@sel0:  jsr parse_sel
        lda NM
        bne @sel
        PS {C_TIME, "NOTHING SELECTED", 13}
        jmp prompt
@sel:   PS {C_SRC}
        lda NM
        jsr pbyte0
        PS {" SELECTED. REPLACE EXISTING (Y/N)?", C_NAME}
        jsr yesno
        lda #0
        rol a
        sta OW
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
@done:  PS {13, C_TIME}
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

; Y or N (either case), echoed; carry set = yes
yesno:  jsr getkey
        cmp #$59
        beq @y
        cmp #$D9
        beq @y
        cmp #$4E
        beq @n
        cmp #$CE
        bne yesno
@n:     jsr CHROUT
        CR
        clc
        rts
@y:     jsr CHROUT
        CR
        sec
        rts

; ---------------------------------------------------------------------------------------------------------------------------------
; Is anything listening on unit A? LISTEN + SECOND, as the KERNAL bus scan does it. Returns A = ST (Z set = present).
probe:  ldx #0
        stx STATUS              ; ST collects bits: an EOF left from an earlier read would look like "nothing there"
        jsr LISTEN
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

; print the string that follows the JSR (zero terminated); bytes C_BAN..C_NAME set the text colour from THEME
pstr:   pla
        sta P1
        pla
        sta P1+1
        ldy #1
@l:     lda (P1),y
        beq @d
        cmp #C_NAME+1
        bcs @ch
        cmp #C_BAN
        bcc @ch
        tax
        lda THEME-1,x
        sta TXTCOL
        jmp @n
@ch:    jsr CHROUT
@n:     iny
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
; Line input into INBUF (zero terminated, INLEN characters): digits , - and A (readline) or any printable character but the
; quote (readtext, for paths); DEL edits; RETURN ends.
readline:
        lda #0
        beq rl
readtext:
        lda #1
rl:     sta RLM
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
@chr:   ldx RLM
        beq @dig
        cmp #34
        beq @cur
        cmp #$20
        bcc @cur
        cmp #$60
        bcc @acc
        cmp #$C1
        bcc @cur
        cmp #$DB
        bcc @acc
        bcs @cur
@dig:   cmp #','
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
        cpy #INMAX
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
        jsr banner
        PS {C_NAME}
        ldy #0
@hl:    lda HDR,y
        beq @hd
        jsr CHROUT
        iny
        bne @hl
@hd:    PS {13, C_SRC}
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
        lda #4
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
        PS {" ", C_NAME, 34}
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
@pe:    PS {C_SRC, " "}
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
        PS {C_TIME, "-- PRESS A KEY --", C_SRC}
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
        PS {C_SRC}
        lda IDX
        clc
        adc #1
        sta NUM
        lda #0
        sta NUM+1
        sta NUM+2
        lda #3
        jsr pdec
        PS {" ", C_NAME, 34}
        ldy #0
@pn:    cpy NAMLEN
        bcs @pe
        lda NAMBUF,y
        jsr CHROUT
        iny
        bne @pn
@pe:    PS {34, " ", C_TIME}
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
; D: a whole 35-track disk, sector by sector, from unit SU to unit DU.
disk:   jsr banner
        PS {C_SRC, "COPY A WHOLE 1541 DISK, ALL 683 SECTORS:", 13, "A .D64 IMAGE TO A FLOPPY OR A FLOPPY TO", 13, "A .D64 IMAGE (OR ANY TWO UNITS).", 13, 13}
        jsr askunits
        jsr openchans
        lda SU
        jsr isvd
        bne @sopen
        PS {C_SRC, "IMAGE ON UNIT "}
        lda SU
        jsr pbyte0
        PS {", E.G. /GAMES/GAME.D64", 13, "(RETURN = THE MOUNTED ONE):", 13, C_NAME}
        jsr readtext
        CR
        lda INLEN
        beq @sopen
        CMDS "CD:"
        jsr cmdin
        ldx #15
        jsr sendcmd
        lda #15
        jsr rderr
        lda ENUM
        beq @sopen
        jmp dfail
@sopen: lda #2
        ldx SU
        jsr openbuf
        lda #15
        jsr rderr
        lda ENUM
        beq @r18
        jmp dfail
@r18:   lda #18
        sta TRK
        lda #0
        sta SCT
        jsr readsec
        lda RFAIL
        bne @rbad
        lda ENUM
        beq @hdr
@rbad:  PS {C_TIME, "CANNOT READ TRACK 18 SECTOR 0", 13}
        jmp dfail
@hdr:   ldx #0
@cp:    lda BUF,x
        sta HBLK,x
        inx
        bne @cp
        PS {C_SRC, "DISK ", C_NAME, 34}
        lda #0
        sta T1
@dn:    ldx T1
        lda HBLK+$90,x
        cmp #$A0
        beq @dne
        jsr psafe
        inc T1
        lda T1
        cmp #16
        bcc @dn
@dne:   PS {34, C_SRC, " ID ", C_NAME}
        lda HBLK+$A2
        jsr psafe
        lda HBLK+$A3
        jsr psafe
        CR
        lda HBLK+2              ; "A" and single-sided: a 1541 disk (a D71 says $80, a D81 has its header on track 40)
        cmp #$41
        bne @odd
        lda HBLK+3
        beq @dst
@odd:   PS {C_TIME, "TRACK 18 DOES NOT LOOK LIKE A 1541 DISK.", 13, C_SRC, "COPY 35 TRACKS ANYWAY (Y/N)? ", C_NAME}
        jsr yesno
        bcs @dst
        jmp back
@dst:   lda DU
        jsr isvd
        beq @j1486
        jmp @dreal
@j1486:
        PS {C_SRC, "NEW IMAGE ON UNIT "}
        lda DU
        jsr pbyte0
        PS {", E.G. GAME.D64", 13, "(RETURN = WRITE OVER THE MOUNTED ONE):", 13, C_NAME}
        jsr readtext
        CR
        lda INLEN
        beq @over
        CMDS {"CD:", 95}        ; unmount first (N: makes a new image only with nothing mounted); an error = nothing was
        ldx #14
        jsr sendcmd
        lda #14
        jsr rderr
        CMDS "N:"
        jsr cmdin
        jsr cmdd64
        ldx #14
        jsr sendcmd
        lda #14
        jsr rderr
        lda ENUM
        bne @j1508
        jmp @dopen
@j1508:
        jmp dfail
@over:  PS {C_SRC, "WRITE OVER THE IMAGE ON UNIT "}
        lda DU
        jsr pbyte0
        PS {" (Y/N)? ", C_NAME}
        jsr yesno
        bcc @j1515
        jmp @dopen
@j1515:
        jmp back
@dreal: PS {C_SRC, "THE DISK IN UNIT "}
        lda DU
        jsr pbyte0
        PS {" WILL BE OVERWRITTEN.", 13, 18, C_OPT, "F", 146, C_SRC, " FORMAT FIRST  ", 18, C_OPT, "W", 146, C_SRC, " JUST WRITE  "}
        PS {18, C_OPT, "Q", 146, C_SRC, " CANCEL", 13}
@fk:    jsr getkey
        cmp #'Q'
        bne @f1
        jmp back
@f1:    cmp #'W'
        beq @dopen
        cmp #'F'
        bne @fk
        PS {C_TIME, "FORMATTING...", 13}
        jsr mkfmt
        ldx #14
        jsr sendcmd
        lda #14
        jsr rderr               ; the drive answers when the format is done
        lda ENUM
        beq @dopen
        jmp dfail
@dopen: lda #3
        ldx DU
        jsr openbuf
        lda #14
        jsr rderr
        lda ENUM
        beq @go
        jmp dfail
@go:    jsr copydisk
        jsr getkey
        jmp back

; the error channel text (ENUM, ERRBUF) when there is one, then back to the menu after a key
dfail:  lda ENUM
        beq @k
        PS {C_TIME, "ERROR "}
        jsr penum
        lda #','
        jsr CHROUT
        jsr pemsg
        CR
@k:     jmp waitback

; print A, a control character or a quote as "?"
psafe:  pha
        and #$7F
        cmp #$20
        pla
        bcc @q
        cmp #34
        bne @o
@q:     lda #'?'
@o:     jmp CHROUT

; CMD = "N:NAME,ID" for a real drive: the source's disk name and ID (characters the command cannot carry as "-" / "0")
mkfmt:  CMDS "N:"
        lda #0
        sta T1
@n:     ldx T1
        lda HBLK+$90,x
        cmp #$A0
        beq @e
        jsr fsafe
        bcc @c
        lda #'-'
@c:     jsr cmdadd
        inc T1
        lda T1
        cmp #16
        bcc @n
@e:     lda T1
        bne @id
        lda #'-'
        jsr cmdadd
@id:    lda #','
        jsr cmdadd
        ldx #0
@i:     lda HBLK+$A2,x
        jsr fsafe
        bcc @ic
        lda #'0'
@ic:    jsr cmdadd
        inx
        cpx #2
        bcc @i
        rts

; carry set when A cannot go into a disk name or ID of an N: command
fsafe:  cmp #$A0
        beq @b
        pha
        and #$7F
        cmp #$20
        pla
        bcc @b
        ldy #fbadn-1
@l:     cmp fbad,y
        beq @b
        dey
        bpl @l
        clc
        rts
@b:     sec
        rts
fbad:   .byte ",:=*?", 34, 13
fbadn = * - fbad

; Copy the 683 sectors, track 1 to 35. RUN/STOP stops; a write error or a failed transfer ends the copy; read errors are counted and the
; sector is written as the drive gave it.
copydisk:
        jsr drawmap
        lda #0
        sta NE
        sta NE+1
        lda tfirst
        sta TRK
@t:     lda #0
        sta SCT
@s:     jsr pstat
        lda THEME+4             ; this sector: "+" in the option colour
        sta CC
        jsr cellpos
        lda #$2B
        jsr cell
        jsr readsec
        lda RFAIL
        bne @rf
        lda RERR
        beq @w
        inc NE
        bne @w
        inc NE+1
@w:     jsr writesec
        lda WFAIL
        beq @wok
        jmp @wf
@wok:
        lda ENUM
        beq @j1655
        jmp @we
@j1655:
        jsr markcell
        lda STOPKEY
        cmp #$7F
        bne @j1659
        jmp @stp
@j1659:
        inc SCT
        ldx TRK
        lda SCT
        cmp spt-1,x
        bcc @s
        lda TRK
        inc TRK
        cmp tlast
        bcc @t
        CMDS "I"                ; the destination reads its new BAM
        ldx #14
        jsr sendcmd
        lda #14
        jsr rderr
        jsr pstat
        jsr row1
        PS {C_TIME, "DONE, "}
        jsr pne
        PS " READ ERRORS"
        jmp @end
@rf:    jsr row1
        PS {C_TIME, "READ FAILED AT "}
        jmp @at
@wf:    jsr row1
        PS {C_TIME, "WRITE FAILED AT "}
        jmp @at
@stp:   jsr row1
        PS {C_TIME, "STOPPED AT "}
        jmp @at
@we:    jsr row1
        PS {C_TIME}
        jsr penum
        lda #','
        jsr CHROUT
        jsr pemsg
        PS " AT "
@at:    lda TRK
        jsr pbyte0
        lda #'/'
        jsr CHROUT
        lda SCT
        jsr pbyte0
@end:   jsr padrow
        clc
        ldx #0
        ldy #27
        jsr PLOT
        PS {C_BAN, 18, "PRESS A KEY", 146}
        rts

; row 1: "8>9  TRACK 18 SECTOR  3  ERRORS 0"
pstat:  jsr row1
        PS {C_NAME}
        lda SU
        jsr pbyte0
        lda #'>'
        jsr CHROUT
        lda DU
        jsr pbyte0
        PS {C_TIME, "  TRACK "}
        lda TRK
        jsr pnum2
        PS " SECTOR "
        lda SCT
        jsr pnum2
        PS "  ERRORS "
        jsr pne
        jmp padrow

pne:    lda NE
        sta NUM
        lda NE+1
        sta NUM+1
        lda #0
        sta NUM+2
        jmp pdec

pnum2:  sta NUM
        lda #0
        sta NUM+1
        sta NUM+2
        lda #2
        jmp pdec

row1:   clc
        ldx #1
        ldy #0
        jmp PLOT

; spaces up to column 38 (the 40th column would link the row with the next)
padrow: sec
        jsr PLOT
@l:     cpy #39
        bcs @d
        lda #' '
        jsr CHROUT
        iny
        bne @l
@d:     rts

; The sector map: rows 2-3 the track numbers (columns 4-38), rows 4-24 the sectors 0-20 ("." for each sector of the disk).
drawmap:jsr banner
        lda THEME+5
        sta CC
        lda #1
        sta TRK
@t:     lda TRK
        jsr dv10
        sta T1
        txa
        beq @sp
        ora #$30
        bne @w
@sp:    lda #$20
@w:     pha
        lda TRK
        clc
        adc #3
        tay
        pla
        ldx #2
        jsr cell
        lda T1
        ora #$30
        ldx #3
        jsr cell
        inc TRK
        lda TRK
        cmp #36
        bcc @t
        lda #0
        sta SCT
@s:     lda SCT
        clc
        adc #4
        sta T2
        lda SCT
        jsr dv10
        sta T1
        txa
        beq @s1
        ora #$30
        bne @s2
@s1:    lda #$20
@s2:    ldx T2
        ldy #1
        jsr cell
        lda T1
        ora #$30
        ldy #2
        jsr cell
        lda #1
        sta TRK
@c:     ldx TRK
        lda SCT
        cmp spt-1,x
        bcs @n                  ; no such sector on this track
        jsr cellpos
        lda #$2E
        jsr cell
@n:     inc TRK
        lda TRK
        cmp #36
        bcc @c
        inc SCT
        lda SCT
        cmp #21
        bcc @s
        rts

; the cell of TRK/SCT: done (a ball in the name colour) or a read error (the last digit of its number, reverse, in the time colour)
markcell:
        lda RERR
        bne @e
        lda THEME+6
        sta CC
        lda #$51
        bne @w
@e:     lda THEME+3
        sta CC
        lda RERR
        jsr dv10
        ora #$B0
@w:     pha
        jsr cellpos
        pla
        jmp cell

; X = row, Y = column of TRK/SCT
cellpos:lda SCT
        clc
        adc #4
        tax
        lda TRK
        clc
        adc #3
        tay
        rts

; screen code A at row X, column Y, in colour CC (X and Y are kept)
cell:   pha
        lda rowlo,x
        sta P2
        lda rowhi,x
        sta P2+1
        pla
        sta (P2),y
        lda P2+1
        clc
        adc #>(COLRAM-SCREEN)
        sta P2+1
        lda CC
        sta (P2),y
        rts

; A (< 100) -> X = tens, A = units
dv10:   ldx #0
@l:     cmp #10
        bcc @d
        sbc #10
        inx
        bne @l
@d:     rts

rowlo:
.repeat 25, I
        .byte <(SCREEN + I * 40)
.endrep
rowhi:
.repeat 25, I
        .byte >(SCREEN + I * 40)
.endrep
spt:    .byte 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 19, 19, 19, 19, 19, 19, 19, 18, 18, 18, 18, 18, 18, 17, 17, 17, 17, 17

; TRK/SCT of the source into BUF through buffer channel 2 ("U1:2,0,T,S"). RERR = the source's error number for it (the bytes come anyway),
; RFAIL = 1 when the 256 bytes did not arrive (a bus error, or the end of data before the last byte).
readsec:lda #'1'
        ldx #'2'
        jsr mku
        ldx #15
        jsr sendcmd
        lda #15
        jsr rderr
        lda ENUM
        sta RERR
        lda #1
        sta RFAIL
        ldx #2
        jsr CHKIN
        bcs @x
        lda #0
        sta STATUS
        sta BI
@l:     jsr CHRIN
        ldx BI
        sta BUF,x
        lda STATUS
        beq @n
        cmp #$40
        bne @x
        cpx #255
        bne @x
@n:     inc BI
        bne @l
        lda #0
        sta RFAIL
@x:     jmp CLRCHN

; BUF to TRK/SCT of the destination through buffer channel 3 ("B-P:3,0", the bytes, "U2:3,0,T,S"). WFAIL = 1 when the bytes did not
; go out; else ENUM = the destination's answer to U2.
writesec:
        CMDS "B-P:3,0"
        ldx #14
        jsr sendcmd
        lda #1
        sta WFAIL
        ldx #3
        jsr CHKOUT
        bcs @x
        lda #0
        sta STATUS
        sta BI
@l:     ldx BI
        lda BUF,x
        jsr CHROUT
        inc BI
        bne @l
        jsr CLRCHN              ; UNLISTEN sends the last byte
        lda STATUS
        bne @r
        lda #'2'
        ldx #'3'
        jsr mku
        ldx #14
        jsr sendcmd
        lda #14
        jsr rderr
        lda #0
        sta WFAIL
@r:     rts
@x:     jmp CLRCHN

; CMD = "U" A ":" X ",0," TRK "," SCT
mku:    sta T1
        stx T2
        jsr cmdbeg
        lda #'U'
        jsr cmdadd
        lda T1
        jsr cmdadd
        lda #':'
        jsr cmdadd
        lda T2
        jsr cmdadd
        jsr cmdps
        .byte ",0,", 0
        lda TRK
        jsr cmddec
        lda #','
        jsr cmdadd
        lda SCT
cmddec: jsr dv10
        pha
        txa
        beq @u
        ora #$30
        jsr cmdadd
@u:     pla
        ora #$30
        jmp cmdadd

; open buffer channel A (logical file and secondary address) on unit X: OPEN A,X,A,"#"
openbuf:sta T3
        stx T2
        CMDS "#"
        lda T3
        ldx T2
        ldy T3
        jmp openn

; ---------------------------------------------------------------------------------------------------------------------------------
; CMD building: cmdbeg empties it, cmdadd appends A (X kept), cmdps the string after the JSR, cmdin the input line, cmdd64 ".D64"
; unless CMD ends in it.
cmdbeg: lda #0
        sta CMDLEN
        rts

cmdadd: ldy CMDLEN
        cpy #CMDMAX
        bcs @f
        sta CMD,y
        inc CMDLEN
@f:     rts

cmdps:  pla
        sta P1
        pla
        sta P1+1
        lda #1
        sta CI
@l:     ldy CI
        lda (P1),y
        beq @d
        jsr cmdadd
        inc CI
        bne @l
@d:     lda CI
        clc
        adc P1
        tax
        lda P1+1
        adc #0
        pha
        txa
        pha
        rts

cmdin:  lda #0
        sta CI
@l:     ldy CI
        cpy INLEN
        bcs @d
        lda INBUF,y
        jsr cmdadd
        inc CI
        bne @l
@d:     rts

cmdd64: ldy CMDLEN
        cpy #6
        bcc @add
        ldx #3
@c:     dey
        lda CMD,y
        cmp d64s,x
        bne @add
        dex
        bpl @c
        rts
@add:   ldx #0
@a:     lda d64s,x
        jsr cmdadd
        inx
        cpx #4
        bcc @a
        rts
d64s:   .byte ".D64"

; send CMD and a CR to the command channel open as logical file X
sendcmd:jsr CHKOUT
        bcs @e
        lda #0
        sta CI
@l:     ldy CI
        cpy CMDLEN
        bcs @cr
        lda CMD,y
        jsr CHROUT
        inc CI
        bne @l
@cr:    lda #13
        jsr CHROUT
@e:     jmp CLRCHN

; ---------------------------------------------------------------------------------------------------------------------------------
; THEME = the TR menu's colour references from the third line of "!STATE" on a vdrive unit (all seven hex digits or none)
readtheme:
        lda VD0
        bne @d
        lda VD1
        bne @d
        rts
@d:     sta T2
        CMDS "!STATE"
        lda #4
        ldx T2
        ldy #4
        jsr openn
        ldx #4
        jsr CHKIN
        bcs @c
        lda #0
        sta STATUS
        lda #2
        sta T1
@sk:    jsr CHRIN               ; skip the image and folder lines
        sta CH
        lda STATUS
        bne @c
        lda CH
        cmp #13
        bne @sk
        dec T1
        bne @sk
@h:     jsr CHRIN
        sta CH
        lda STATUS
        and #$BF
        bne @c
        lda CH
        jsr hexv
        bcs @c
        ldx T1
        sta THN,x
        inx
        stx T1
        cpx #7
        bcs @ok
        lda STATUS
        beq @h
        bne @c
@ok:    dex
@cp:    lda THN,x
        sta THEME,x
        dex
        bpl @cp
@c:     jsr CLRCHN
        lda #4
        jmp CLOSE

; hex digit A ('0'-'9', 'A'-'F') -> A, carry clear; carry set when it is none
hexv:   sec
        sbc #'0'
        cmp #10
        bcc @ok
        sbc #'A'-'0'
        cmp #6
        bcs @b
        adc #10
@ok:    clc
        rts
@b:     sec
        rts

tfirst: .byte 1                 ; the tracks copied (the VICE test patches them to copy fewer: each mailbox byte stops VICE there)
tlast:  .byte 35
THEME:  .byte 0, 4, 4, 10, 7, 14, 13 ; the TR menu's colour references (its defaults until "!STATE" gives them): see C_BAN

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
INBUF:  .res INMAX+2
ERRBUF: .res 48
CMD:    .res CMDMAX
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
VD0:    .res 1          ; the vdrive units (VD1 right after VD0)
VD1:    .res 1
OC:     .res 1
OB:     .res 1
OT:     .res 1
RLM:    .res 1
CI:     .res 1
CC:     .res 1
TRK:    .res 1
SCT:    .res 1
RERR:   .res 1
RFAIL:  .res 1
WFAIL:  .res 1
BI:     .res 1
NE:     .res 2
THN:    .res 7
