; VDrive for the plain TeensyROM (no Kernal Replace): the drives through the KERNAL's RAM vectors instead of a patched ROM.
; docs/DESIGN.md 52. The stock KERNAL stays as it is (tape included); OPEN/CLOSE/CHKIN/CHKOUT/CLRCHN/CHRIN/CHROUT/GETIN/CLALL/
; LOAD/SAVE through $031A-$0333 come here first. A handler asks the mailbox whether the device is ours (the same byte-level
; and fast LOAD/SAVE commands the KERNAL patch sends, so the Teensy side is unchanged) and on "not mine" jumps through the
; vector value it found at install, so real drives, JiffyDOS and other wedges installed earlier keep working.
;
; Not reached: anything that calls the serial routines ($FFB1 LISTEN, $FFB4 TALK, $FF93, $FF96, $FFA5, $FFA8, $FFAB, $FFAE)
; or the stock routines behind the vectors directly. RESTOR ($FF8A, RUN/STOP+RESTORE, a reset) puts the ROM vectors back: the
; IMAIN hook (v_main) puts ours back at the next READY when only RESTOR happened; after a reset, SYS to the start again.
;
; Layout: one block, linked at the address given to ld65 (-S, vector/vdvec.cfg); the first three bytes are the install entry
; (SYS49152 for the VICE build at $C000). The block writes into itself (the saved vectors), so it must sit in RAM or in a
; Teensy-served IO page that keeps writes.

.include "mailbox.inc"

.segment "LOADADDR"
        .addr   start

NV      = 12                    ; hooked vectors (vtab / hand below)

.segment "CODE"
start:  jmp     install

; the vector values found at install, in vtab's order; handlers leave through JMP (old+n). Kept inside one page (JMP ($xxFF)).
old:    .res    NV*2

; zero page: $A3-$A7, the serial-bus scratch bytes the KERNAL patch uses too (free outside bit-banging); $93 / $B7-$BC / $C1-$C4 /
; $AE / $AF as the stock routines use them
LA      = $B8
SA      = $B9
FA      = $BA
FNLEN   = $B7
FNADR   = $BB
DFLTN   = $99
DFLTO   = $9A
VERCK   = $93
NFILES  = $98
LAT     = $0259
FAT     = $0263
SAT     = $026D

; KERNAL error exits (A = number, message when in direct mode, C=1): stock entry points, the same in every ROM on the
; firmware's list and in JiffyDOS
E_NOTFOUND = $F704
E_NODEV    = $F707

; ---- install ------------------------------------------------------------------------------------------------------------------
; Each vector that is not already ours: remember its value, point it here. Calling it again does nothing (SYS twice is safe).
install:
hook:   sei
        ldx     #0
@l:     ldy     vtab,x          ; the vector's low byte; all are in page 3
        lda     $0300,y
        cmp     hand,x
        bne     @new
        lda     $0301,y
        cmp     hand+1,x
        beq     @next
@new:   lda     $0300,y
        sta     old,x
        lda     $0301,y
        sta     old+1,x
        lda     hand,x
        sta     $0300,y
        lda     hand+1,x
        sta     $0301,y
@next:  inx
        inx
        cpx     #NV*2
        bne     @l
        cli
        rts

; vector low bytes (page 3) and our handlers, the same order as `old`
vtab:   .word   $031A, $031C, $031E, $0320, $0322, $0324, $0326, $032A, $032C, $0330, $0332, $0302
hand:   .word   v_open, v_close, v_chkin, v_ckout, v_clrch, v_basin, v_bsout, v_getin, v_clall, v_load, v_save, v_main
.assert hand - vtab = NV*2, error, "vtab and NV disagree"
O_OPEN  = old + 0
O_CLOSE = old + 2
O_CHKIN = old + 4
O_CKOUT = old + 6
O_CLRCH = old + 8
O_BASIN = old + 10
O_BSOUT = old + 12
O_GETIN = old + 14
O_CLALL = old + 16
O_LOAD  = old + 18
O_SAVE  = old + 20
O_MAIN  = old + 22

; ---- IMAIN ($0302, BASIC's direct-mode loop): RESTOR resets $0314-$0333 but not $0302 ----------------------------------------
; When ILOAD holds again exactly the value we replaced, RESTOR ran (RUN/STOP+RESTORE, a program that called $FF8A): hook again.
; Any other value (a wedge installed after us) is left alone.
v_main: lda     $0330
        cmp     O_LOAD
        bne     @go
        lda     $0331
        cmp     O_LOAD+1
        bne     @go
        jsr     hook
@go:    jmp     (O_MAIN)

; ---- mailbox primitives: A = argument; out as vcall (C=0 ours, C=1 not mine). X, Y kept. ---------------------------------------
.macro  prim    cmd
        sta     MBX_ARG
        sta     $A7
        lda     #cmd
        jmp     vcall
.endmacro
listen_: prim   C_LISTEN
talk_:  prim    C_TALK
second_: prim   C_SECOND
tksa_:  prim    C_TKSA
ciout_: prim    C_CIOUT
unlsn_: lda     #C_UNLSN
        .byte   $2C
untlk_: lda     #C_UNTLK
        .byte   $2C
acptr_: lda     #C_ACPTR
        jmp     vcall

.include "vcall.inc"

vd_here:                        ; as in patches/vdrive.s: Z=1 when the ID register reads $D6 (4 tries)
        lda     #$7C
        sta     $A5
@try:   lda     MBX_ID
        cmp     #VD_ID0
        beq     @out
        inc     $A5
        bpl     @try
@out:   rts

; ---- the file table (as the stock LOOKUP at $F314 / $F31F) ---------------------------------------------------------------------
; in: A = logical file number. out: Z=1 found, X = its index
find:   ldx     NFILES
@l:     dex
        bmi     @no
        cmp     LAT,x
        bne     @l
        rts
@no:    ldx     #$FF            ; Z=0
        rts

; ---- OPEN ($031A) ---------------------------------------------------------------------------------------------------------------
; Stock OPEN only touches the bus for a serial device with a name and a secondary address. Ours: the Teensy gets LISTEN first
; (the ownership question, and a bus scan when it is due), then the stock OPEN fills the file table with the name length set to 0
; (so it sends nothing), then SECOND, the name and UNLISTEN go to the Teensy.
v_open: lda     FA
        cmp     #4
        bcc     @old
        lda     SA
        bmi     @old
        lda     FNLEN
        beq     @old
        lda     #0
        sta     ST
        lda     FA
        jsr     listen_
        bcs     @old
        lda     ST              ; DEVICE NOT PRESENT from the LISTEN (a unit with no card): the stock OPEN clears ST
        pha
        lda     FNLEN
        pha
        lda     #0
        sta     FNLEN
        jsr     @jold
        tax
        pla
        sta     FNLEN
        pla
        bcs     @err            ; FILE OPEN, TOO MANY FILES, ...: the Teensy's LISTEN must still end
        ora     ST
        sta     ST
        lda     SA              ; the stock OPEN made it SA | $60
        ora     #$F0
        jsr     second_
        bit     ST
        bmi     @nodev          ; the table entry stays, as stock
        ldy     #0
@n:     lda     (FNADR),y
        jsr     ciout_
        iny
        cpy     FNLEN
        bne     @n
        jsr     unlsn_
        clc
        rts
@nodev: jsr     unlsn_
        jmp     E_NODEV
@err:   txa
        pha
        jsr     unlsn_
        pla
        sec
        rts
@jold:  jmp     (O_OPEN)
@old:   jmp     (O_OPEN)

; ---- CLOSE ($031C), A = logical file ---------------------------------------------------------------------------------------------
; Ours: CLOSE (SECOND $E0|channel, UNLISTEN) goes to the Teensy, then the entry's secondary address gets bit 7 so the stock CLOSE
; only removes it from the table.
v_close:
        pha
        jsr     find
        bne     @old
        lda     FAT,x
        cmp     #4
        bcc     @old
        lda     SAT,x
        bmi     @old
        lda     FAT,x
        jsr     listen_
        bcs     @old
        lda     SAT,x
        and     #$EF
        ora     #$E0
        jsr     second_
        jsr     unlsn_
        lda     SAT,x
        ora     #$80
        sta     SAT,x
@old:   pla
        jmp     (O_CLOSE)

; ---- CHKIN ($031E) / CHKOUT ($0320), X = logical file --------------------------------------------------------------------------
v_chkin:
        ldy     #0
        .byte   $2C
v_ckout:
        ldy     #1
        txa
        pha
        lda     #0
        sta     ST
        pla
        pha
        jsr     find
        bne     @old
        lda     FAT,x
        cmp     #4
        bcc     @old
        lda     LAT,x           ; as stock $F31F: the file's numbers into zero page
        sta     LA
        lda     SAT,x
        sta     SA
        lda     FAT,x
        sta     FA
        cpy     #1
        beq     @out
        jsr     talk_
        bcs     @old
        lda     SA
        bmi     @in             ; no secondary address: TALK alone, as stock
        jsr     tksa_
@in:    pla
        bit     ST
        bmi     @nodev
        lda     FA
        sta     DFLTN
        clc
        rts
@out:   jsr     listen_
        bcs     @old
        lda     SA
        bmi     @o2
        jsr     second_
@o2:    pla
        bit     ST
        bmi     @nodev
        lda     FA
        sta     DFLTO
        clc
        rts
@nodev: jmp     E_NODEV
@old:   pla
        tax
        cpy     #1
        beq     @jo
        jmp     (O_CHKIN)
@jo:    jmp     (O_CKOUT)

; ---- CLRCHN ($0322) / CLALL ($032C) --------------------------------------------------------------------------------------------
; Ours: UNLISTEN / UNTALK to the Teensy and the defaults back to screen / keyboard, so the stock routine that follows sends
; nothing on the real bus for them. Not ours: the stock routine handles the real device.
clr_ours:
        lda     DFLTO
        cmp     #4
        bcc     @in
        jsr     unlsn_
        bcs     @in
        lda     #3
        sta     DFLTO
@in:    lda     DFLTN
        cmp     #4
        bcc     @done
        jsr     untlk_
        bcs     @done
        lda     #0
        sta     DFLTN
@done:  rts
v_clrch:
        jsr     clr_ours
        jmp     (O_CLRCH)
v_clall:
        jsr     clr_ours
        jmp     (O_CLALL)

; ---- CHRIN ($0324) / GETIN ($032A) --------------------------------------------------------------------------------------------
; A serial device with ST set gets a CR from the stock code without bus traffic, so only ST = 0 asks the Teensy.
v_basin:
        lda     DFLTN
        cmp     #4
        bcc     @old
        lda     ST
        bne     @old
        jsr     acptr_
        bcs     @old
        clc
        rts
@old:   jmp     (O_BASIN)
v_getin:
        lda     DFLTN
        cmp     #4
        bcc     @old
        lda     ST
        bne     @old
        jsr     acptr_
        bcs     @old
        clc
        rts
@old:   jmp     (O_GETIN)

; ---- CHROUT ($0326), A = byte: A comes back as the byte (BASIC's PRINT depends on it) ------------------------------------------
v_bsout:
        pha
        lda     DFLTO
        cmp     #4
        bcc     @old
        pla
        pha
        jsr     ciout_
        bcs     @old
        pla
        clc
        rts
@old:   pla
        jmp     (O_BSOUT)

; ---- names into the window ----------------------------------------------------------------------------------------------------
; in: A = FLOAD_OPEN / FSAVE_OPEN. Copies the file name to the window and sends the command (out: as vcall).
; A name of WINSZ bytes or more does not fit: C=1 without asking the Teensy (the caller tells it apart by FNLEN >= 128).
open_named:
        pha
        ldy     FNLEN           ; never 0 here
        bmi     @long           ; >= 128 = WINSZ
        ldy     #0
@nm:    lda     (FNADR),y       ; each byte read back
        sta     WIN,y
        cmp     WIN,y
        bne     @nm
        iny
        cpy     FNLEN
        bne     @nm
        sty     MBX_LEN
        lda     FA
        sta     MBX_ARG
        sta     $A7
        pla
        jmp     vcall
@long:  pla
        sec
        rts

; A name too long for the window, on a device that may be ours: ask with LISTEN / UNLISTEN. Ours: FILE NOT FOUND (no byte-level
; LOAD / SAVE in this build; DESIGN.md 52). C=1 on return: not ours.
long_name:
        lda     FA
        jsr     listen_
        bcs     @no
        jsr     unlsn_
        jmp     E_NOTFOUND
@no:    rts

; ---- LOAD ($0330): A = 0 load / 1 verify, $C3/$C4 = the caller's address, X/Y as given --------------------------------------
; The fast path for LOAD and VERIFY (the KERNAL patch leaves VERIFY to the byte-level hooks; here nothing would catch it).
v_load: sta     VERCK
        lda     FA
        cmp     #4
        bcc     @old
        lda     FNLEN
        beq     @old            ; MISSING FILE NAME from the stock code
        lda     #0
        sta     ST
        lda     SA
        sta     $A4             ; original SA: 0 = load to the caller's address
        lda     #C_FLOAD_OPEN
        jsr     open_named
        bcc     @mine
        ldy     FNLEN
        bpl     @old
        jsr     long_name
@old:   lda     VERCK
        jmp     (O_LOAD)
@mine:  pha
        jsr     $F5AF           ; "SEARCHING FOR name" (stock prints it before opening; ours is known only now)
        lda     #$60
        sta     SA
        pla
        beq     @ok
        cmp     #5
        beq     @nodev
        jmp     E_NOTFOUND
@nodev: jmp     E_NODEV
@ok:    lda     WIN             ; load address: double reads
        cmp     WIN
        bne     @ok
        sta     $AE
@h1:    lda     WIN+1
        cmp     WIN+1
        bne     @h1
        sta     $AF
        lda     $A4
        bne     @go
        lda     $C3
        sta     $AE
        lda     $C4
        sta     $AF
@go:    jsr     $F5D2           ; "LOADING" / "VERIFYING"
@blk:   lda     #C_FLOAD_BLK
        jsr     vcall
        bcs     @fail           ; time-out / not-mine in the middle of a load
        sta     $A3             ; byte count (0..WINSZ)
        beq     @chk
        ldy     #0
@cp:    lda     WIN,y           ; double read: the byte is only taken when two reads in a row agree
        cmp     WIN,y
        bne     @cp
        ldx     VERCK
        bne     @ver
        sta     ($AE),y
@nx:    iny
        cpy     $A3
        bne     @cp
        lda     $AE
        clc
        adc     $A3
        sta     $AE
        bcc     @chk
        inc     $AF
@chk:   lda     ST              ; vcall folded the Teensy's EOI (bit 6) into ST
        and     #$40
        beq     @blk
        jmp     $F5A9           ; CLC, X/Y = end address, RTS
@ver:   cmp     ($AE),y
        beq     @nx
        lda     #$10            ; VERIFY ERROR, as stock ($F4FE): ST bit 4, the load goes on
        ora     ST
        sta     ST
        bne     @nx
@fail:  jmp     E_NODEV         ; an answer failed mid-transfer: an error instead of stale data

; ---- SAVE ($0332): $C1/$C2 start, $AE/$AF end ---------------------------------------------------------------------------------
v_save: lda     FA
        cmp     #4
        bcc     @old
        lda     FNLEN
        beq     @old
        lda     #C_FSAVE_OPEN
        jsr     open_named
        bcc     @mine
        ldy     FNLEN
        bpl     @old
        jsr     long_name
@old:   jmp     (O_SAVE)
@mine:  sta     $A4             ; 0 = open, else DOS error / 5 = no device
        lda     #$61
        sta     SA
        jsr     $F68F           ; "SAVING name"
        lda     $A4
        cmp     #5
        bne     @nodevok
        jmp     E_NODEV
@nodevok:
        lda     $A4
        bne     @done           ; drive refused (exists / full): stock stays silent too (the error channel says why)
        lda     $C1             ; load address header
        sta     WIN
        sta     $AC
        lda     $C2
        sta     WIN+1
        sta     $AD
        lda     #2
        sta     MBX_LEN
        lda     #C_FSAVE_BLK
        jsr     vcall
@loop:  lda     $AE             ; remaining = end - current
        sec
        sbc     $AC
        sta     $A3
        lda     $AF
        sbc     $AD
        bcc     @fin
        bne     @full
        lda     $A3
        beq     @fin
        cmp     #WINSZ+1
        bcc     @send
@full:  lda     #WINSZ
        sta     $A3
@send:  ldy     #0
@c:     lda     ($AC),y
        sta     WIN,y
        cmp     WIN,y           ; read back: a bad write is sent again
        bne     @c
        iny
        cpy     $A3
        bne     @c
        lda     $A3
        sta     MBX_LEN
        lda     #C_FSAVE_BLK
        jsr     vcall
        bcs     @ff
        lda     $AC
        clc
        adc     $A3
        sta     $AC
        bcc     @loop
        inc     $AD
        bcs     @loop
@fin:   lda     #C_FSAVE_CLOSE
        jsr     vcall
@done:  clc
        rts
@ff:    jmp     E_NODEV         ; mailbox stopped answering mid-save

.assert >(old + NV*2 - 1) = >old, lderror, "the saved vectors cross a page: JMP (old+n) would read the wrong high byte"
