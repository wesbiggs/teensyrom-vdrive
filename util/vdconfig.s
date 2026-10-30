; VDrive Config: edit the Alt-button disk lists (/vdrive-config.txt on each unit) and show / change the mounted images, entirely in machine code.
; Loads at $0801 with a BASIC "10 SYS 2061" stub. Talks to the drives only through the KERNAL: the "!CONFIG[,W]" and "!STATE" host text
; channels (docs/DESIGN.md 23), "$/folder/" listings for the browser and "CD:" on the command channel (logical file 15 for the SD unit, 14 for
; the USB unit, open for the whole run: closing a command channel closes every channel of its unit).
; The file is kept as text in TXT (each line ends in CR) with an index of its lines; every change rebuilds the text in NEWB (one line replaced,
; removed, swapped or added), copies it back, re-indexes and re-parses the entries, then writes the whole file. Lines that are not entries
; (comments, blank lines) stay where they are.
; Buffers and tables are BSS at $2000-$4FFF (not in the PRG). Messages are upper case ASCII, which is also the default-mode PETSCII.
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
PLOT    = $FFF0
LISTEN  = $FFB1
SECOND  = $FF93
UNLSN   = $FFAE
STATUS  = $90
QTSW    = $D4           ; quote mode of the screen editor: a '"' in a path must not turn the control codes after it into characters
INSRT   = $D8           ; insert count, likewise
BORDER  = $D020
BGCOL   = $D021
TXTCOL  = $0286
DEV0REG = $DE16         ; vdrive: current device number of the SD unit, $DE17 the USB unit (0 = none)

P1      = $FB           ; inline strings
P2      = $FD           ; the string being printed, parsed or compared
P3      = $22           ; write pointer; $22-$25 are BASIC's scratch pointers, unused while this runs
P4      = $24           ; read pointer

MAXL    = 119           ; lines kept from the file
MAXE    = 32            ; list entries (what the firmware reads)
MAXB    = 100           ; browser entries
SAVEMAX = 2000          ; bytes of the file the firmware reads
VR      = 16            ; list rows on the screen (rows 4-19)
BPMAX   = 200           ; browser path
CURSOR  = $A4           ; the line editor's cursor (an underscore)
LARROW  = 95            ; left arrow: cancel; "CD:<-" unmounts
OUTSCR  = 0             ; OUTM: where fch sends characters
OUTLINE = 1
OUTCMD  = 2

.macro PS str
        jsr pstr
        .byte str, 0
.endmacro
.macro MSG str          ; a message on row 20
        jsr msgbeg
        PS {str}
        jsr fpad
.endmacro
.macro CMDS str         ; CMD = str; fch appends to CMD until scr
        jsr cmdbeg
        PS {str}
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
        jsr scr
        PS {"READING CONFIG...", 13}
        lda #8
        jsr probe               ; the first bus access runs the vdrive bus scan; until then DEV0/DEV1 hold the defaults
        ldx #1
@dv:    lda DEV0REG,x
        cmp #8
        bcc @no
        cmp #31
        bcc @ok
@no:    lda #0
@ok:    sta DEVS,x
        dex
        bpl @dv
        lda DEVS
        ora DEVS+1
        bne @act
        lda #255
        sta FLDN
        PS {"VDRIVE IS NOT ACTIVE.", 13, "START THIS PROGRAM FROM THE TEENSYROM MENU WITH THE VDRIVE KERNAL.", 13}
        rts
@act:   lda BORDER
        and #15
        sta OC
        lda BGCOL
        and #15
        sta OB
        lda TXTCOL
        and #15
        sta OT
        lda DEVS
        beq @o1
        tax
        lda #15
        ldy #15
        jsr openc
@o1:    lda DEVS+1
        beq @o2
        tax
        lda #14
        ldy #15
        jsr openc
@o2:    lda #0
        sta BORDER
        sta BGCOL
        sta UN
        lda #1
        sta TXTCOL
        lda DEVS
        bne @u
        inc UN                  ; no SD unit: start on the USB list
@u:     jsr setunit
        jsr readstate
        jsr readcfg
        lda #0
        sta CU
        sta TP
        jsr drawall
        MSG "READY"
main:   jsr getkey
        cmp #'Q'
        beq quit
        ldx #NKEYS-1
@f:     cmp keytab,x
        beq @go
        dex
        bpl @f
        bmi main
@go:    jsr @call
        jmp main
@call:  lda keyhi,x
        pha
        lda keylo,x
        pha
        rts

keytab: .byte 145, 17, 13, 'B', 'U', 'A', 'E', 'D', '+', '-', 'T', 'R'
NKEYS = * - keytab
keylo:  .lobytes k_up-1, k_down-1, k_ret-1, k_brow-1, k_unm-1, k_add-1, k_edit-1, k_del-1, k_plus-1, k_minus-1, k_tog-1, k_ref-1
keyhi:  .hibytes k_up-1, k_down-1, k_ret-1, k_brow-1, k_unm-1, k_add-1, k_edit-1, k_del-1, k_plus-1, k_minus-1, k_tog-1, k_ref-1

quit:   lda #15
        jsr CLOSE
        lda #14
        jsr CLOSE
        jsr CLRCHN
        lda OC
        sta BORDER
        lda OB
        sta BGCOL
        lda OT
        sta TXTCOL
        lda #255
        sta FLDN
        PS {147, 154, "BYE"}
        rts

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

; DV / EL: device and command channel of the unit whose list is shown
setunit:ldx UN
        lda DEVS,x
        sta DV
        lda #15
        cpx #0
        beq @s
        lda #14
@s:     sta EL
        rts

; ---------------------------------------------------------------------------------------------------------------------------------
; Output. Everything goes through fch, which sends to the screen, to LINEB or to CMD (OUTM). On the screen, printable characters
; count down FLDN and are dropped at 0 (a field of fixed width, padded by fpad); control codes always go out.

; one character; keeps A, X, Y
fch:    sta FCH
        stx FX
        sty FY
        ldx OUTM
        bne @buf
        and #$7F
        cmp #$20
        bcc @out
        lda FLDN
        beq @r
        dec FLDN
@out:   lda FCH
        jsr CHROUT
        lda #0
        sta QTSW
        sta INSRT
        beq @r
@buf:   dex
        bne @cmd
        ldx LBLEN
        cpx #255
        bcs @r
        lda FCH
        sta LINEB,x
        inc LBLEN
        bne @r
@cmd:   ldx CMDLEN
        cpx #255
        bcs @r
        lda FCH
        sta CMD,x
        inc CMDLEN
@r:     ldx FX
        ldy FY
        lda FCH
        rts

scr:    lda #OUTSCR
        sta OUTM
        rts

cmdbeg: lda #OUTCMD
        sta OUTM
        lda #0
        sta CMDLEN
        rts

; spaces up to the end of the field
fpad:   lda FLDN
        beq @d
        lda #' '
        jsr fch
        jmp fpad
@d:     rts

; print the string that follows the JSR (zero terminated)
pstr:   pla
        sta P1
        pla
        sta P1+1
        ldy #1
@l:     lda (P1),y
        beq @d
        jsr fch
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

; A in decimal, right justified in X columns (0 = no padding)
fnum:   stx NW
        ldx #0
@l:     ldy #0
@s:     cmp #10
        bcc @r
        sbc #10
        iny
        bne @s
@r:     ora #'0'
        sta DIGS,x
        inx
        tya
        bne @l
        stx ND
        lda NW
        sec
        sbc ND
        bcc @o
        beq @o
        tay
@p:     lda #' '
        jsr fch
        dey
        bne @p
@o:     ldx ND
@q:     lda DIGS-1,x
        jsr fch
        dex
        bne @q
        rts

; the SLEN characters at (P2), from index 0 / from index Y
fmem:   ldy #0
fmemy:  cpy SLEN
        bcs @d
        lda (P2),y
        jsr fch
        iny
        bne fmemy
@d:     rts

; (P2)/SLEN in a field of A columns
ffield: sta FLDN
        jsr fmem
        jmp fpad

; (P2)/SLEN in a field of A columns; when it is longer, ".." and its last A-2 characters
fpath:  sta FLDN
        cmp SLEN
        bcs @all
        lda #'.'
        jsr fch
        jsr fch
        lda SLEN
        sec
        sbc FLDN
        tay
        jsr fmemy
        jmp fpad
@all:   jsr fmem
        jmp fpad

; cursor to row X, column 0
atrow:  ldy #0
        clc
        jmp PLOT

; cursor to row X, a field of 39 columns
rowbeg: jsr atrow
        lda #39
        sta FLDN
        rts

msgbeg: jsr scr
        ldx #20
        jmp rowbeg

; "ERROR nn,message" on row 20
perr:   jsr msgbeg
        PS "ERROR "
        lda ENUM
        ldx #0
        jsr fnum
        PS ","
        jsr pemsg
        jmp fpad

; ---------------------------------------------------------------------------------------------------------------------------------
; Read the error channel of logical file A: ERRBUF = "NN,MESSAGE,TT,SS", ENUM = NN (255 = not understood).
rdel:   lda EL
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

; the message part of the error channel text (after the first comma, without the leading spaces, up to the next comma)
pemsg:  ldy #3
        cpy ERRLEN
        bcs @d
@s:     lda ERRBUF,y
        cmp #' '
        bne @l
        iny
        bne @s
@l:     lda ERRBUF,y
        beq @d
        cmp #','
        beq @d
        jsr fch
        iny
        bne @l
@d:     rts

; send CMD and a CR to the command channel of the shown unit, then read its error channel
sendcmd:jsr scr
        ldx EL
        jsr CHKOUT
        lda #0
        sta IX
@l:     ldy IX
        cpy CMDLEN
        bcs @e
        lda CMD,y
        jsr CHROUT
        inc IX
        bne @l
@e:     lda #13
        jsr CHROUT
        jsr CLRCHN
        jmp rdel

; ---------------------------------------------------------------------------------------------------------------------------------
; MPL / MPB: the image mounted on each unit, from the first line of "!STATE" (empty when none or no unit).
readstate:
        ldx #1
@u:     stx U
        lda #0
        sta MPL,x
        cpx #0
        beq @o
        lda #128
@o:     sta UOFS
        lda DEVS,x
        beq @nx
        CMDS "!STATE"
        jsr scr
        ldx U
        lda DEVS,x
        tax
        lda #2
        ldy #2
        jsr openn
        lda #15
        ldx U
        beq @e
        lda #14
@e:     jsr rderr
        lda ENUM
        bne @cl
        ldx #2
        jsr CHKIN
        lda #0
        sta STATUS
        sta LLEN
@rd:    jsr CHRIN
        sta CH
        lda STATUS
        sta RS
        and #$BF
        bne @end
        lda CH
        cmp #13
        beq @end
        ldy LLEN
        cpy #127
        bcs @sk
        tya
        ora UOFS
        tax
        lda CH
        sta MPB,x
        inc LLEN
@sk:    lda RS
        and #$40
        beq @rd
@end:   jsr CLRCHN
        ldx U
        lda LLEN
        sta MPL,x
@cl:    lda #2
        jsr CLOSE
@nx:    ldx U
        dex
        bmi @x
        jmp @u
@x:     rts

; P2 / SLEN = the image mounted on unit X
mpptr:  lda MPL,x
        sta SLEN
        lda #<MPB
        cpx #0
        beq @z
        lda #<(MPB+128)
@z:     sta P2
        lda #>MPB
        sta P2+1
        rts

; ---------------------------------------------------------------------------------------------------------------------------------
; Read the shown unit's "!CONFIG" into TXT: lines end in CR, 0 bytes are dropped, at most MAXL lines of 255 characters
; (a missing file is an empty list). Then index and parse.
readcfg:lda #<TXT
        sta P3
        lda #>TXT
        sta P3+1
        lda #>TXT+15
        sta LIMHI
        lda #0
        sta NLC
        sta LLEN
        CMDS "!CONFIG"
        jsr scr
        lda #2
        ldx DV
        ldy #2
        jsr openn
        jsr rdel
        lda ENUM
        bne @cl
        ldx #2
        jsr CHKIN
        lda #0
        sta STATUS
@rd:    jsr CHRIN
        sta CH
        lda STATUS
        sta RS
        and #$BF
        bne @eof
        lda CH
        beq @nx
        cmp #13
        bne @c
        jsr endl
        jmp @nx
@c:     lda LLEN
        cmp #255
        bcs @nx
        lda NLC
        cmp #MAXL
        bcs @nx
        lda CH
        jsr putc
        bcs @nx
        inc LLEN
@nx:    lda RS
        and #$40
        beq @rd
@eof:   lda LLEN
        beq @e2
        jsr endl                ; a last line without a CR
@e2:    jsr CLRCHN
@cl:    lda #2
        jsr CLOSE
        sec
        lda P3
        sbc #<TXT
        sta TLEN
        lda P3+1
        sbc #>TXT
        sta TLEN+1
        jmp reindex

; end a line of the file: a CR, unless MAXL lines are there already
endl:   lda #0
        sta LLEN
        lda NLC
        cmp #MAXL
        bcs @d
        inc NLC
        lda #13
        ldy #0
        sta (P3),y
        jmp incp3
@d:     rts

; A -> (P3)+ while P3 is below page LIMHI (carry set = no room; the last page is left for the CRs)
putc:   ldx P3+1
        cpx LIMHI
        bcs putcr
        ldy #0
        sta (P3),y
incp3:  inc P3
        bne putok
        inc P3+1
putok:  clc
putcr:  rts

; LSL/LSH/LLN, NL: start and length of every line of TXT (TLEN bytes); then parse
reindex:lda #<TXT
        sta P4
        clc
        adc TLEN
        sta TEND
        lda #>TXT
        sta P4+1
        adc TLEN+1
        sta TEND+1
        lda #0
        sta NL
        sta LLEN
        jsr @mark
@l:     lda P4
        cmp TEND
        bne @c
        lda P4+1
        cmp TEND+1
        bne @c
        jmp parse
@c:     ldy #0
        lda (P4),y
        inc P4
        bne @i
        inc P4+1
@i:     cmp #13
        beq @e
        inc LLEN
        jmp @l
@e:     ldx NL
        cpx #MAXL
        bcs @s
        lda LS0
        sta LSL,x
        lda LS0+1
        sta LSH,x
        lda LLEN
        sta LLN,x
        inc NL
@s:     lda #0
        sta LLEN
        jsr @mark
        jmp @l
@mark:  lda P4
        sta LS0
        lda P4+1
        sta LS0+1
        rts

; The entries: lines that start with "/" after any spaces, "path[, colour[, flashes]]"; the numbers are 1-3 digits after the last
; commas (spaces around them allowed), colour taken AND 15, flashes at most 20, both 1 when missing. NE entries of at most MAXE:
; EPL/EPH/ELN the path (in TXT), EC, EF, EX the line.
parse:  lda #0
        sta NE
        sta LI
@ln:    lda LI
        cmp NL
        bcc @1
        rts
@1:     lda NE
        cmp #MAXE
        bcc @2
        rts
@nxj:   jmp @nx
@2:     ldx LI
        lda LSL,x
        sta P2
        lda LSH,x
        sta P2+1
        lda LLN,x
        sta SLEN
        ldy #0
@sp:    cpy SLEN
        bcs @nxj
        lda (P2),y
        cmp #' '
        bne @3
        iny
        bne @sp
@3:     cmp #'/'
        bne @nxj
        sty T0                  ; the path starts here
        tya
        clc
        adc P2
        sta P2
        bcc @4
        inc P2+1
@4:     lda SLEN
        sec
        sbc T0
        sta SLEN
        lda #1
        sta PC
        sta PF
        jsr trailnum
        bcs @tr
        lda V
        sta N1
        lda V+1
        sta N1+1
        jsr trailnum
        bcc @two
        lda N1                  ; one number: the colour
        and #15
        sta PC
        jmp @tr
@two:   lda V
        and #15
        sta PC
        lda N1+1
        bne @20
        lda N1
        cmp #21
        bcc @f
@20:    lda #20
@f:     sta PF
@tr:    ldy SLEN                ; trailing spaces off
        beq @st
        dey
        lda (P2),y
        cmp #' '
        bne @st
        dec SLEN
        jmp @tr
@st:    ldx NE
        lda P2
        sta EPL,x
        lda P2+1
        sta EPH,x
        lda SLEN
        sta ELN,x
        lda PC
        sta EC,x
        lda PF
        sta EF,x
        lda LI
        sta EX,x
        inc NE
@nx:    inc LI
        jmp @ln

; the number after the last comma of (P2)/SLEN: V, and SLEN cut before that comma; carry set = none
trailnum:
        ldy SLEN
        jmp @f
@no0:   sec
        rts
@f:     tya
        beq @no0
        dey
        lda (P2),y
        cmp #','
        bne @f
        sty CI
        iny
@s:     cpy SLEN
        bcs @no0
        lda (P2),y
        cmp #' '
        bne @e0
        iny
        bne @s
@e0:    sty T0                  ; first non-space
        ldy SLEN
@e:     dey
        lda (P2),y
        cmp #' '
        beq @e
        iny
        sty T1                  ; after the last non-space
        tya
        sec
        sbc T0
        cmp #4
        bcs @no
        lda #0
        sta V
        sta V+1
        ldy T0
@d:     lda (P2),y
        sec
        sbc #'0'
        cmp #10
        bcs @no
        sta T2
        lda V                   ; V = V * 10 + digit
        sta T3
        lda V+1
        sta T4
        asl V
        rol V+1
        asl V
        rol V+1
        clc
        lda V
        adc T3
        sta V
        lda V+1
        adc T4
        sta V+1
        asl V
        rol V+1
        clc
        lda V
        adc T2
        sta V
        bcc @n
        inc V+1
@n:     iny
        cpy T1
        bcc @d
        lda CI
        sta SLEN
        clc
        rts
@no:    sec
        rts

; ---------------------------------------------------------------------------------------------------------------------------------
; Change the file: OPK 0 = line OPA becomes LINEB, 1 = line OPA goes, 2 = lines OPA and OPB swap, 3 = LINEB is added at the end.
; The new text is built in NEWB and copied to TXT, then indexed and parsed. Carry set (nothing changed) when it does not fit.
rebuild:lda #<NEWB
        sta P3
        lda #>NEWB
        sta P3+1
        lda #>NEWB+15
        sta LIMHI
        lda #0
        sta OVF
        sta LI
@l:     lda LI
        cmp NL
        bcs @end
        ldx LI
        lda OPK
        cmp #2
        bne @n2
        cpx OPA
        bne @a
        ldx OPB
        jmp @cp
@a:     cpx OPB
        bne @cp
        ldx OPA
        jmp @cp
@n2:    cpx OPA
        bne @cp
        cmp #1
        beq @nx
        cmp #0
        bne @cp
        jsr emitlb
        jmp @nx
@cp:    jsr emitln
@nx:    inc LI
        jmp @l
@end:   lda OPK
        cmp #3
        bne @ck
        jsr emitlb
@ck:    lda OVF
        beq @ok
        sec
        rts
@ok:    sec
        lda P3
        sbc #<NEWB
        sta TLEN
        lda P3+1
        sbc #>NEWB
        sta TLEN+1
        lda #<NEWB
        sta P4
        lda #>NEWB
        sta P4+1
        lda #<TXT
        sta P3
        lda #>TXT
        sta P3+1
        ldx #16                 ; the whole 4 KB
        ldy #0
@m:     lda (P4),y
        sta (P3),y
        iny
        bne @m
        inc P4+1
        inc P3+1
        dex
        bne @m
        jsr reindex
        clc
        rts

; line X of TXT / LINEB, and a CR, to (P3)
emitln: lda LSL,x
        sta P4
        lda LSH,x
        sta P4+1
        lda LLN,x
        sta SLEN2
        jmp emit
emitlb: lda #<LINEB
        sta P4
        lda #>LINEB
        sta P4+1
        lda LBLEN
        sta SLEN2
emit:   lda #0
        sta IX
@l:     ldy IX
        cpy SLEN2
        bcs @cr
        lda (P4),y
        jsr putc
        bcc @n
        lda #1
        sta OVF
@n:     inc IX
        jmp @l
@cr:    lda #13
        ldy #0
        sta (P3),y
        jmp incp3

; LINEB = "path, colour, flashes" from (P2)/SLEN, PC, PF
mkline: lda #OUTLINE
        sta OUTM
        lda #0
        sta LBLEN
        jsr fmem
        PS ", "
        lda PC
        ldx #0
        jsr fnum
        PS ", "
        lda PF
        ldx #0
        jsr fnum
        jmp scr

; Write TXT to the shown unit's "!CONFIG,W" (refused over SAVEMAX bytes) and say how it went.
save:   lda TLEN+1
        cmp #>(SAVEMAX+1)
        bcc @ok
        bne @long
        lda TLEN
        cmp #<(SAVEMAX+1)
        bcc @ok
@long:  MSG "LIST TOO LONG TO SAVE"
        rts
@ok:    CMDS "!CONFIG,W"
        jsr scr
        lda #2
        ldx DV
        ldy #2
        jsr openn
        jsr rdel
        lda ENUM
        beq @w
        lda #2
        jsr CLOSE
        jsr msgbeg
        PS "CANNOT WRITE:"
        jsr pemsg
        jmp fpad
@w:     ldx #2
        jsr CHKOUT
        lda #<TXT
        sta P4
        lda #>TXT
        sta P4+1
        lda #0
        sta CNT
        sta CNT+1
@l:     lda CNT
        cmp TLEN
        bne @c
        lda CNT+1
        cmp TLEN+1
        beq @e
@c:     ldy #0
        lda (P4),y
        jsr CHROUT
        inc P4
        bne @i
        inc P4+1
@i:     inc CNT
        bne @l
        inc CNT+1
        jmp @l
@e:     jsr CLRCHN
        lda #2
        jsr CLOSE
        jsr rdel
        lda ENUM
        bne @we
        MSG "SAVED"
        rts
@we:    jsr msgbeg
        PS "WRITE ERROR "
        lda ENUM
        ldx #0
        jsr fnum
        jmp fpad

; ---------------------------------------------------------------------------------------------------------------------------------
; The screen: row 0 title, 1-2 the units, 3 the list header, 4-19 the list, 20 messages, 21-23 the keys.
drawall:jsr scr
        PS {147}
        ldx #0
        jsr rowbeg
        PS {18, " VDRIVE CONFIG"}
        jsr fpad
        PS {146}
drawmid:jsr drawdrv
        jsr drawlh
        jsr drawlist
        ldx #21
        jsr rowbeg
        PS "RETURN:MOUNT B:BROWSE+MOUNT U:UNMOUNT"
        jsr fpad
        ldx #22
        jsr rowbeg
        PS "A:ADD E:EDIT D:DELETE +/-:REORDER"
        jsr fpad
        ldx #23
        jsr rowbeg
        PS "T:SD/USB LIST R:REFRESH Q:QUIT"
        jmp fpad

; "SD  8: /path" / "USB 9: (NOTHING MOUNTED)" / "USB  : NO DRIVE", the shown unit's name in reverse
drawdrv:lda #0
        sta U
@u:     ldx U
        inx
        jsr atrow
        lda #255
        sta FLDN
        lda U
        cmp UN
        bne @n
        PS {18}
@n:     lda U
        bne @usb
        PS "SD "
        jmp @num
@usb:   PS "USB"
@num:   ldx U
        lda DEVS,x
        bne @d
        PS "  : "
        jmp @m
@d:     ldx #2
        jsr fnum
        PS ": "
@m:     PS {146}
        lda #32
        sta FLDN
        ldx U
        lda DEVS,x
        bne @h
        PS "NO DRIVE"
        jmp @p
@h:     lda MPL,x
        bne @mp
        PS "(NOTHING MOUNTED)"
        jmp @p
@mp:    jsr mpptr
        lda #32
        jsr fpath
@p:     jsr fpad
        inc U
        lda U
        cmp #2
        bcs @x
        jmp @u
@x:     rts

drawlh: ldx #3
        jsr rowbeg
        PS {18, "LIST: "}
        lda UN
        bne @u
        PS "SD"
        jmp @n
@u:     PS "USB"
@n:     PS " ("
        lda NE
        ldx #0
        jsr fnum
        PS " ENTRIES)"
        jsr fpad
        PS {146}
        rts

drawlist:
        lda #0
        sta R
@r:     lda TP
        clc
        adc R
        sta KK
        jsr drawrow
        inc R
        lda R
        cmp #VR
        bcc @r
        rts

; list entry KK on its row: ">" on the cursor, "*" when mounted, number, path (reverse on the cursor), colour, a block in it, flashes
drawrow:lda KK
        sec
        sbc TP
        clc
        adc #4
        tax
        jsr rowbeg
        lda KK
        cmp NE
        bcc @e
        jmp fpad
@e:     lda #255
        sta FLDN
        lda #' '
        ldx KK
        cpx CU
        bne @c
        lda #'>'
@c:     jsr fch
        jsr mstar
        jsr fch
        lda KK
        clc
        adc #1
        ldx #3
        jsr fnum
        lda #' '
        jsr fch
        lda KK
        cmp CU
        bne @p
        lda #18
        jsr fch
@p:     ldx KK
        lda EPL,x
        sta P2
        lda EPH,x
        sta P2+1
        lda ELN,x
        sta SLEN
        lda #22
        jsr fpath
        lda #255
        sta FLDN
        PS {146, " "}
        ldx KK
        lda EC,x
        ldx #3
        jsr fnum
        PS {" ", 18}
        ldx KK
        ldy EC,x
        lda colcodes,y
        jsr fch
        PS {"  ", 146, 5}
        ldx KK
        lda EF,x
        ldx #3
        jsr fnum
        lda #'X'
        jmp fch

colcodes:
        .byte 144, 5, 28, 159, 156, 30, 31, 158, 129, 149, 150, 151, 152, 153, 154, 155

; A = '*' when entry KK is the image mounted on the shown unit, else ' '
mstar:  ldx UN
        jsr mpptr
        lda P2
        sta P4
        lda P2+1
        sta P4+1
        ldx KK
        lda ELN,x
        cmp SLEN
        bne @no
        lda EPL,x
        sta P2
        lda EPH,x
        sta P2+1
        ldy SLEN
@c:     dey
        cpy #255
        beq @yes
        lda (P2),y
        cmp (P4),y
        beq @c
@no:    lda #' '
        rts
@yes:   lda #'*'
        rts

; keep the cursor on the screen
fixtp:  lda CU
        cmp TP
        bcs @a
        sta TP
@a:     lda TP
        clc
        adc #VR
        sta T0
        lda CU
        cmp T0
        bcc @r
        sec
        sbc #VR-1
        sta TP
@r:     rts

; keep the cursor in the list
clampcu:lda CU
        cmp NE
        bcc @r
        ldx NE
        dex
        bpl @s
        ldx #0
@s:     stx CU
@r:     rts

; ---------------------------------------------------------------------------------------------------------------------------------
; The keys.
k_up:   lda CU
        beq @r
        dec CU
        lda CU
        cmp TP
        bcs @two
        sta TP
        jmp drawlist
@two:   sta KK
        inc KK
        jsr drawrow
        lda CU
        sta KK
        jmp drawrow
@r:     rts

k_down: lda CU
        clc
        adc #1
        cmp NE
        bcs @r
        sta CU
        lda TP
        clc
        adc #VR
        cmp CU
        beq @sc
        bcc @sc
        lda CU
        sta KK
        dec KK
        jsr drawrow
        lda CU
        sta KK
        jmp drawrow
@sc:    lda CU
        sec
        sbc #VR-1
        sta TP
        jmp drawlist
@r:     rts

; RETURN: mount the entry
k_ret:  lda NE
        bne @go
        rts
@go:    ldx CU
        lda EPL,x
        sta P2
        lda EPH,x
        sta P2+1
        lda ELN,x
        sta SLEN
; "CD:" + (P2)/SLEN on the shown unit
mountp: CMDS "CD:"
        jsr fmem
        jsr sendcmd
        lda ENUM
        bne @e
        MSG "MOUNTED"
        jmp @d
@e:     jsr perr
@d:     jsr readstate
        jmp drawmid

; B: browse the card, mount the image picked
k_brow: jsr browse
        lda BR
        bne @m
        jmp drawall
@m:     jsr pathbp
        jmp mountp

; U: unmount
k_unm:  ldx UN
        lda MPL,x
        bne @u
        MSG "NOTHING MOUNTED"
        rts
@u:     CMDS {"CD:", LARROW}
        jsr sendcmd
        lda ENUM
        bne @e
        MSG "UNMOUNTED"
        jmp @d
@e:     jsr msgbeg
        PS "ERROR "
        lda ENUM
        ldx #0
        jsr fnum
        jsr fpad
@d:     jsr readstate
        jmp drawmid

; A: add an entry (browse, colour, flashes)
k_add:  lda NE
        cmp #MAXE
        bcs @full
        lda NL
        cmp #MAXL-1
        bcc @ok
@full:  MSG "LIST IS FULL"
        rts
@ok:    jsr browse
        lda BR
        bne @c
        jmp drawall
@c:     lda #15
        sta NMAX
        lda #1
        ldx #<lbcol
        ldy #>lbcol
        jsr asknum
        bcs @cn
        sta PC
        lda #20
        sta NMAX
        lda #1
        ldx #<lbfl
        ldy #>lbfl
        jsr asknum
        bcs @cn
        sta PF
        jsr pathbp
        jsr mkline
        lda #3
        sta OPK
        lda #255
        sta OPA
        jsr rebuild
        bcs @long
        ldx NE
        dex
        stx CU
        jsr fixtp
        jsr drawall
        jmp save
@cn:    jmp drawall
@long:  jmp toolong

; E: edit the entry (path, "?" browses for it; colour; flashes)
k_edit: lda NE
        bne @go
        rts
@go:    ldx CU
        lda EPL,x
        sta P2
        lda EPH,x
        sta P2+1
        lda ELN,x
        cmp #255
        bcc @l
        lda #254
@l:     sta EDLEN
        ldy #0
@cp:    cpy EDLEN
        bcs @cd
        lda (P2),y
        sta EDBUF,y
        iny
        bne @cp
@cd:    lda #78
        ldx #<lbpath
        ldy #>lbpath
        jsr edline
        bcc @got
        jmp drawmid
@got:   lda EDLEN
        cmp #1
        bne @typed
        lda EDBUF
        cmp #'?'
        bne @typed
        jsr browse
        lda BR
        bne @chk
        jmp drawall
@typed: ldy #0
@t:     cpy EDLEN
        bcs @td
        lda EDBUF,y
        sta PATHB,y
        iny
        bne @t
@td:    sty PATHBL
@chk:   lda PATHBL
        beq @bad
        lda PATHB
        cmp #'/'
        beq @num
@bad:   jsr drawmid
        MSG "PATH MUST START WITH /"
        rts
@num:   lda #15
        sta NMAX
        ldx CU
        lda EC,x
        ldx #<lbcol
        ldy #>lbcol
        jsr asknum
        bcs @cn
        sta PC
        lda #20
        sta NMAX
        ldx CU
        lda EF,x
        ldx #<lbfl
        ldy #>lbfl
        jsr asknum
        bcs @cn
        sta PF
        jsr pathbp
        jsr mkline
        lda #0
        sta OPK
        ldx CU
        lda EX,x
        sta OPA
        jsr rebuild
        bcs toolong
        jsr drawall
        jmp save
@cn:    jmp drawall

toolong:jsr drawall
        MSG "LIST TOO LONG"
        rts

; D: delete the entry (asks)
k_del:  lda NE
        bne @go
        rts
@go:    jsr msgbeg
        PS "DELETE ENTRY "
        lda CU
        clc
        adc #1
        ldx #0
        jsr fnum
        PS "? (Y/N)"
        jsr fpad
        jsr getkey
        cmp #'Y'
        beq @y
        cmp #'Y'+128
        beq @y
        jsr msgbeg
        jmp fpad
@y:     lda #1
        sta OPK
        ldx CU
        lda EX,x
        sta OPA
        jsr rebuild
        jsr clampcu
        jmp moved

; +: move the entry down, -: up
k_plus: lda CU
        clc
        adc #1
        cmp NE
        bcc @go
        rts
@go:    tax
        lda EX,x
        sta OPB
        ldx CU
        lda EX,x
        sta OPA
        lda #2
        sta OPK
        jsr rebuild
        inc CU
moved:  jsr fixtp
        jsr drawmid
        jmp save

k_minus:lda CU
        bne @go
        rts
@go:    tax
        dex
        lda EX,x
        sta OPB
        ldx CU
        lda EX,x
        sta OPA
        lda #2
        sta OPK
        jsr rebuild
        dec CU
        jmp moved

; T: the other unit's list
k_tog:  lda UN
        eor #1
        tax
        lda DEVS,x
        bne @ok
        MSG "NO DRIVE ON THAT UNIT"
        rts
@ok:    stx UN
        jsr setunit
        jsr readcfg
        lda #0
        sta CU
        sta TP
        jmp drawmid

; R: read everything again (the Alt button may have switched the disk)
k_ref:  jsr readstate
        jsr setunit
        jsr readcfg
        jsr clampcu
        jsr fixtp
        jsr drawmid
        MSG "REFRESHED"
        rts

pathbp: lda #<PATHB
        sta P2
        lda #>PATHB
        sta P2+1
        lda PATHBL
        sta SLEN
        rts

lbcol:  .byte "COLOUR 0-15: ", 0
lbfl:   .byte "FLASHES 0-20: ", 0
lbpath: .byte "PATH(?=BROWSE): ", 0

; ---------------------------------------------------------------------------------------------------------------------------------
; Line editor on row 23: label at X/Y (zero terminated), EDBUF/EDLEN the text, at most A characters typed (a longer text can still be
; shortened). RETURN: carry clear; left arrow: carry set. Typed: codes 32-94.
edline: sta ML
        stx LBP
        sty LBP+1
@show:  ldx #23
        jsr atrow
        lda #255
        sta FLDN
        lda LBP
        sta P2
        lda LBP+1
        sta P2+1
        ldy #0
@lb:    lda (P2),y
        beq @le
        jsr fch
        iny
        bne @lb
@le:    sty T0
        lda #38
        sec
        sbc T0
        sta T1
        ldx EDLEN
        lda #CURSOR
        sta EDBUF,x
        inx
        stx SLEN
        lda #<EDBUF
        sta P2
        lda #>EDBUF
        sta P2+1
        lda T1
        jsr fpath
@k:     jsr getkey
        cmp #13
        beq @ok
        cmp #LARROW
        beq @cn
        cmp #20
        bne @ch
        lda EDLEN
        beq @k
        dec EDLEN
        jmp @show
@ch:    cmp #32
        bcc @k
        cmp #LARROW
        bcs @k
        ldx EDLEN
        cpx ML
        bcs @k
        sta EDBUF,x
        inc EDLEN
        jmp @show
@ok:    clc
        rts
@cn:    sec
        rts

; Ask for a number with label X/Y, default A, at most NMAX: A = the number (the default when it is not one or too big), carry set = cancelled.
asknum: sta DEF
        stx LBP2
        sty LBP2+1
        lda #OUTLINE
        sta OUTM
        lda #0
        sta LBLEN
        lda DEF
        ldx #0
        jsr fnum
        jsr scr
        ldx #0
@c:     cpx LBLEN
        bcs @d
        lda LINEB,x
        sta EDBUF,x
        inx
        bne @c
@d:     stx EDLEN
        lda #2
        ldx LBP2
        ldy LBP2+1
        jsr edline
        bcs @r
        jsr edval
        bcs @def
        cmp NMAX
        bcc @ok
        beq @ok
@def:   lda DEF
@ok:    clc
@r:     rts

; EDBUF as a number (leading spaces, then digits up to the first other character; empty = 0): A, carry set if it starts with '-'
edval:  ldy #0
        lda #0
        sta V
@s:     cpy EDLEN
        bcs @d
        lda EDBUF,y
        cmp #' '
        bne @g
        iny
        bne @s
@g:     cmp #'-'
        beq @bad
@l:     cpy EDLEN
        bcs @d
        lda EDBUF,y
        sec
        sbc #'0'
        cmp #10
        bcs @d
        sta T2
        lda V
        asl a
        asl a
        adc V
        asl a
        adc T2
        sta V
        iny
        bne @l
@d:     lda V
        clc
        rts
@bad:   sec
        rts

; ---------------------------------------------------------------------------------------------------------------------------------
; The browser: folders and .D64/.D71/.D81 of the shown unit, from "$/folder/" listings (a listing by path leaves the mount alone).
; RETURN opens a folder (".." goes up) or picks an image: BR = 1 and PATHB / PATHBL its path. Left arrow: BR = 0.
browse: lda #'/'
        sta BP
        lda #1
        sta BPLN
        lda #0
        sta BR
@new:   lda #0
        sta BQ
        sta BW
        jsr blist
@draw:  jsr bdraw
@key:   jsr getkey
        cmp #LARROW
        bne @1
        rts
@1:     cmp #145
        bne @2
        lda BQ
        beq @key
        dec BQ
        jmp @mv
@2:     cmp #17
        bne @3
        lda BQ
        clc
        adc #1
        cmp BK
        bcs @key
        sta BQ
        jmp @mv
@3:     cmp #13
        bne @key
        lda BK
        beq @key
        lda BQ
        bne @in
        lda BPLN
        cmp #2
        bcc @in                 ; the root has no ".."
        ldy BPLN                 ; "..": cut at the last "/"
@sl:    dey
        lda BP,y
        cmp #'/'
        bne @sl
        sty BPLN
        tya
        bne @new
        lda #1                  ; up to "/" itself
        sta BPLN
        jmp @new
@in:    jsr mkjn
        bcs @key
        ldx BQ
        lda BTY,x
        bne @img
        ldy #0                  ; a folder: BP = PATHB
@c:     lda PATHB,y
        sta BP,y
        iny
        cpy PATHBL
        bcc @c
        sty BPLN
        jmp @new
@img:   lda #1
        sta BR
        rts
@mv:    lda BQ
        cmp BW
        bcs @m1
        sta BW
@m1:    lda BW
        clc
        adc #VR
        sta T0
        lda BQ
        cmp T0
        bcc @m2
        sec
        sbc #VR-1
        sta BW
@m2:    jmp @draw

; PATHB = BP + "/" + name BQ (no second "/" at the root); carry set (and a message) when longer than BPMAX
mkjn:   lda BQ
        jsr bname
        lda BPLN
        clc
        adc SLEN
        cmp #BPMAX
        bcs @long
        ldy #0
@c:     lda BP,y
        sta PATHB,y
        iny
        cpy BPLN
        bcc @c
        cpy #2
        bcc @r
        lda #'/'
        sta PATHB,y
        iny
@r:     tya
        tax
        ldy #0
@n:     cpy SLEN
        bcs @d
        lda (P2),y
        sta PATHB,x
        inx
        iny
        bne @n
@d:     stx PATHBL
        clc
        rts
@long:  MSG "PATH TOO LONG"
        sec
        rts

; P2 = the name of browser entry A (16-byte slots), SLEN its length
bname:  tax
        lda BNL,x
        sta SLEN
        stx P2
        lda #0
        sta P2+1
        ldy #4
@s:     asl P2
        rol P2+1
        dey
        bne @s
        clc
        lda P2
        adc #<BNAMES
        sta P2
        lda P2+1
        adc #>BNAMES
        sta P2+1
        rts

; read the listing of BP into BNAMES/BNL/BTY, BK entries (first ".." below the root)
blist:  lda #0
        sta BK
        lda BPLN
        cmp #2
        bcc @o
        lda #0
        jsr bname
        lda #'.'
        ldy #0
        sta (P2),y
        iny
        sta (P2),y
        lda #2
        sta BNL
        lda #0
        sta BTY
        inc BK
@o:     CMDS "$"
        lda #<BP
        sta P2
        lda #>BP
        sta P2+1
        lda BPLN
        sta SLEN
        jsr fmem
        ldx BPLN
        lda BP-1,x
        cmp #'/'
        beq @s
        lda #'/'
        jsr fch
@s:     jsr scr
        lda #1
        ldx DV
        ldy #0
        jsr openn
        jsr rdel
        lda ENUM
        beq @rd
        lda #1
        jsr CLOSE
        jsr msgbeg
        PS "CANNOT LIST: "
        jsr pemsg
        jmp fpad
@rd:    ldx #1
        jsr CHKIN
        lda #0
        sta STATUS
        jsr CHRIN               ; load address
        jsr CHRIN
@line:  jsr CHRIN               ; link
        sta T0
        jsr CHRIN
        ora T0
        sta T0
        lda STATUS
        and #$BF
        bne @end
        lda T0
        beq @end
        jsr CHRIN               ; line number (blocks)
        jsr CHRIN
        lda #0
        sta LLEN
@ch:    jsr CHRIN
        sta CH
        lda STATUS
        sta RS
        and #$BF
        bne @end
        lda CH
        beq @eol
        ldy LLEN
        cpy #60
        bcs @nx
        sta LINE,y
        inc LLEN
@nx:    lda RS
        and #$40
        beq @ch
@eol:   jsr bentry
        lda RS
        and #$40
        beq @line
@end:   jsr CLRCHN
        lda #1
        jmp CLOSE

; one listing line in LINE/LLEN: the name between the first two quotes (trailing spaces off), kept when the type after it is DIR/D64/D71/D81
bentry: ldy LLEN
        lda #0
        sta LINE,y
        sta Q1
        ldy #0
@q:     cpy LLEN
        bcs @no
        lda LINE,y
        iny
        cmp #34
        bne @q
        ldx Q1
        bne @two
        sty Q1                  ; the name starts here
        jmp @q
@two:   dey
        sty Q2                  ; the closing quote
        tya
        sec
        sbc Q1
        sta NLN
@tr:    lda NLN
        beq @ty
        clc
        adc Q1
        tay
        lda LINE-1,y
        cmp #' '
        bne @ty
        dec NLN
        jmp @tr
@ty:    ldy Q2
        iny
@sp:    lda LINE,y
        cmp #' '
        bne @t0
        iny
        bne @sp
@t0:    sty T0
        ldx #0
        stx T1
@tc:    ldy T0
        lda LINE,y
        cmp types,x
        bne @tn
        lda LINE+1,y
        cmp types+1,x
        bne @tn
        lda LINE+2,y
        cmp types+2,x
        beq @hit
@tn:    inc T1
        inx
        inx
        inx
        cpx #12
        bcc @tc
@no:    rts
@hit:   lda BK
        cmp #MAXB
        bcs @no
        jsr bname
        ldx BK
        lda T1
        sta BTY,x
        lda NLN
        cmp #17
        bcc @l
        lda #16
@l:     sta BNL,x
        sta SLEN
        ldy #0
@cp:    cpy SLEN
        bcs @d
        tya
        clc
        adc Q1
        tax
        lda LINE,x
        sta (P2),y
        iny
        bne @cp
@d:     inc BK
        rts

types:  .byte "DIRD64D71D81"

; the browser screen: row 3 the folder, 4-19 the entries, 21 the keys
bdraw:  ldx #3
        jsr rowbeg
        PS {18, "BROWSE: "}
        lda #<BP
        sta P2
        lda #>BP
        sta P2+1
        lda BPLN
        sta SLEN
        jsr fmem
        jsr fpad
        PS {146}
        lda #0
        sta R
@r:     lda BW
        clc
        adc R
        sta KK
        lda R
        clc
        adc #4
        tax
        jsr rowbeg
        lda KK
        cmp BK
        bcc @e
        jsr fpad
        jmp @nx
@e:     lda #255
        sta FLDN
        lda #' '
        ldx KK
        cpx BQ
        bne @c
        lda #'>'
@c:     jsr fch
        lda KK
        cmp BQ
        bne @n
        lda #18
        jsr fch
@n:     lda KK
        jsr bname
        lda #17
        jsr ffield
        lda #255
        sta FLDN
        PS {146, " "}
        ldx KK
        lda BTY,x
        sta T0
        asl a
        adc T0
        tax
        lda types,x
        jsr fch
        lda types+1,x
        jsr fch
        lda types+2,x
        jsr fch
        lda #17
        sta FLDN
        jsr fpad
@nx:    inc R
        lda R
        cmp #VR
        bcs @h
        jmp @r
@h:     ldx #21
        jsr rowbeg
        PS {"RETURN:OPEN/SELECT  ", LARROW, ":CANCEL"}
        jsr fpad
        ldx #22
        jsr rowbeg
        jsr fpad
        ldx #23
        jsr rowbeg
        jmp fpad

; ---------------------------------------------------------------------------------------------------------------------------------
.segment "BSS"
TXT:    .res 4096               ; the file ($2000, page aligned: putc checks pages)
NEWB:   .res 4096               ; the file being rebuilt
MPB:    .res 256                ; the mounted images, 128 bytes per unit (page aligned: mpptr adds 128 to the low byte)
.assert <TXT = 0 && <NEWB = 0 && <MPB = 0, error, "TXT, NEWB and MPB must be page aligned"
BNAMES: .res MAXB*16
BNL:    .res MAXB
BTY:    .res MAXB
LSL:    .res MAXL
LSH:    .res MAXL
LLN:    .res MAXL
EPL:    .res MAXE
EPH:    .res MAXE
ELN:    .res MAXE
EC:     .res MAXE
EF:     .res MAXE
EX:     .res MAXE
BP:     .res BPMAX+1
PATHB:  .res 256
EDBUF:  .res 257
LINEB:  .res 256
CMD:    .res 256
LINE:   .res 64
ERRBUF: .res 48
DIGS:   .res 3
DEVS:   .res 2
MPL:    .res 2
TLEN:   .res 2
TEND:   .res 2
LS0:    .res 2
CNT:    .res 2
V:      .res 2
N1:     .res 2
LBP:    .res 2
LBP2:   .res 2
OUTM:   .res 1
FLDN:   .res 1
FCH:    .res 1
FX:     .res 1
FY:     .res 1
NW:     .res 1
ND:     .res 1
SLEN:   .res 1
SLEN2:  .res 1
LBLEN:  .res 1
CMDLEN: .res 1
OC:     .res 1
OB:     .res 1
OT:     .res 1
UN:     .res 1
U:      .res 1
UOFS:   .res 1
DV:     .res 1
EL:     .res 1
NL:     .res 1
NLC:    .res 1
NE:     .res 1
LI:     .res 1
LLEN:   .res 1
LIMHI:  .res 1
CH:     .res 1
RS:     .res 1
EI:     .res 1
ERRLEN: .res 1
ENUM:   .res 1
TMP:    .res 1
IX:     .res 1
CU:     .res 1
TP:     .res 1
KK:     .res 1
R:      .res 1
PC:     .res 1
PF:     .res 1
CI:     .res 1
T0:     .res 1
T1:     .res 1
T2:     .res 1
T3:     .res 1
T4:     .res 1
OPK:    .res 1
OPA:    .res 1
OPB:    .res 1
OVF:    .res 1
ML:     .res 1
DEF:    .res 1
NMAX:   .res 1
EDLEN:  .res 1
PATHBL: .res 1
BPLN:    .res 1
BQ:     .res 1
BW:     .res 1
BK:     .res 1
BR:     .res 1
Q1:     .res 1
Q2:     .res 1
NLN:    .res 1
