; VDrive agent for the plain TeensyROM (docs/DESIGN.md 53): a small 6502 routine the Teensy serves from the IO1 page. The KERNAL's
; RAM vectors point into it; it reports each call to the Teensy and then does what the Teensy's answer says. All drive and file-table
; logic is C on the Teensy (d64/vdrive_vec.c), which also decides "not ours" and chains to the vector it replaced.
;
; Request (agent -> Teensy): the frame in the window, then CMD = C_VEC (or C_CONT after a PEEK / CONT op: the window holds PEEK's data)
;   WIN+0 Y, +1 X, +2 A, +3 P, +4 the entry's return-address low byte (which entry; `ctramp`'s own entry = a CALL's result),
;   WIN+5.. the zero-page bytes in zpl
; Answer (Teensy -> agent): a script from WIN+0, run in order, ending in GO, PEEK or CONT. Op codes are decoded by shifting:
;   OP_GO   (0) Y P A X : load the registers and JMP (WIN+126). Return = GO to `rtsop`, chain = GO to the old vector, error = GO to
;                         the KERNAL's $F6FB+3(n-1), CALL = POKE the routine into ctramp+1 and GO to ctramp (its result comes back as a frame).
;   OP_POKE (1) lo hi n d[n-1]..d[0] : n bytes to memory (the data last byte first)
;   OP_PEEK (2) lo hi n : n bytes of memory to WIN+0.., then C_CONT
;   OP_CONT (4)         : C_CONT (the Teensy has more to send)
; Nothing is kept in zero page or RAM: POKE/PEEK patch their own operands, CALL the operand of ctramp's JSR (the Teensy keeps writes
; to the page it serves, as GeoRAM does in VICE). The wait has no time-out: while this code is visible, the handler that answers
; is running; RUN/STOP+RESTORE gets out of a hang.
;
; Fast paths: CHROUT/CHRIN/GETIN with DFLTO/DFLTN < 4 (screen, keyboard, tape, RS-232) go straight to the old vector, so printing
; and keyboard input cost nothing extra.
;
; The offsets the Teensy needs are asserted at the end and repeated in d64/vdrive_vec.h (VV_OFS_*).

.include "mailbox.inc"

C_VEC   = 20
C_CONT  = 21

; entries, in this order; d64/vdrive_vec.h has the same numbers (VV_E_*)
E_BASIN   = 6                 ; also GETIN for devices 4 and up (stock runs the same serial code for both)
E_BSOUT   = 7
NE        = 12

.segment "LOADADDR"
        .addr   base

.segment "CODE"
base:
ctramp: jsr     $FFFF           ; base+0: CALL: the Teensy writes the routine's address here, then GOes here
        jsr     enter           ; base+3: the routine's result goes back as a frame from this entry
old_bsout: .word $F1CA          ; base+6: the vectors the fast paths chain to (INSTALL writes the real ones)
old_basin: .word $F157          ; base+8
old_getin: .word $F13E          ; base+10
entries:                        ; base+12: one JSR per entry; the pushed return address says which
        .repeat NE
        jsr     enter
        .endrep
e_basin = entries + 3*E_BASIN
e_bsout = entries + 3*E_BSOUT

f_bsout:
        pha
        lda     $9A
        cmp     #4
        pla
        bcs     e_bsout
        jmp     (old_bsout)
f_basin:
        pha
        lda     $99
        cmp     #4
        pla
        bcs     e_basin
        jmp     (old_basin)
f_getin:
        pha
        lda     $99
        cmp     #4
        pla
        bcs     e_basin
        jmp     (old_getin)

enter:  php
        pha
        txa
        pha
        tya
        pha
        ldy     #5              ; Y, X, A, P, return-address low -> WIN+0..4
        ldx     #0
@p:     pla
        jsr     putx
        inx
        dey
        bne     @p
        pla                     ; the return address's high byte: this page
@z:     ldy     zpl-5,x         ; X = 5..
        lda     $0000,y
        jsr     putx
        inx
        cpx     #5+NZ
        bne     @z
        lda     #C_VEC
send:   sta     MBX_CMD
@w:     lda     MBX_STAT        ; BUSY until the answer is complete (a token: a misread never equals OK)
        cmp     #STAT_OK
        bne     @w
        ldx     #0
next:   jsr     rd
        lsr     a
        bcs     poke            ; 1
        lsr     a
        bcs     peek            ; 2
        bne     cont            ; 4
        jsr     rd              ; 0: GO
        tay
        jsr     rd
        pha                     ; P
        jsr     rd
        pha                     ; A
        jsr     rd
        tax
        pla
        plp
        jmp     (WIN+126)

poke:   jsr     rd
        sta     @st+1
        jsr     rd
        sta     @st+2
        jsr     rd
        tay
@l:     dey
        jsr     rd
@st:    sta     $FFFF,y
        tya
        bne     @l
        beq     next

peek:   jsr     rd
        sta     @ld+1
        jsr     rd
        sta     @ld+2
        jsr     rd
        tay
@l:     dey
@ld:    lda     $FFFF,y
@w:     sta     WIN,y           ; written back and checked
        cmp     WIN,y
        bne     @w
        tya
        bne     @l
cont:   lda     #C_CONT
        bne     send

; window helpers: putx stores A at WIN+X until it reads back; rd reads WIN+X until two reads agree, then X+1
rd:     lda     WIN,x
        cmp     WIN,x
        bne     rd
        inx
        rts
putx:   sta     WIN,x
        cmp     WIN,x
        bne     putx
rtsop:  rts                     ; GO here returns to the caller

zpl:    .byte   $90, $98, $99, $9A ; ST, open files, DFLTN, DFLTO
NZ      = * - zpl
end_:

; d64/vdrive_vec.h repeats these
.assert ctramp - base = 0, error, "VV_OFS_CTRAMP"
.assert old_bsout - base = 6, error, "VV_OFS_OLD (old_bsout, old_basin, old_getin)"
.assert entries - base = 12, error, "VV_OFS_ENTRIES"
.assert f_bsout - base = 48, error, "VV_OFS_F_BSOUT"
.assert f_basin - base = 59, error, "VV_OFS_F_BASIN"
.assert f_getin - base = 70, error, "VV_OFS_F_GETIN"
.assert rtsop - base = 241, error, "VV_OFS_RTS"
.assert NZ = 4, error, "VV_NZ"
.assert end_ - base <= 248, error, "the agent must fit $DE08-$DEFF (IO1 minus the SwiftLink ACIA)"
