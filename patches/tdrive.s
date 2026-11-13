; Virtual-drive hooks for the 8 serial-layer KERNAL entry points (901227-03).
; Each hook asks the TR+ mailbox whether it owns the current IEC transaction
; (the device numbers are configured in the firmware, d64/vd_cfg.h). If not ("not mine"), the displaced original instructions run
; and flow continues in the stock routine, so real drives still work.
;
; Mailbox (IO1 on hardware; RAM in the VICE mock build, see Makefile). It sits at $DE10, clear of the
; SwiftLink/Turbo232 ACIA at $DE00-$DE07 so both can be active together (firmware handler "TDrive+SwiftLink"):
;   MBX+0 CMD   W  write triggers the Teensy; sets BUSY at once
;   MBX+1 ARG   RW argument (device / secondary / byte)
;   MBX+2 DATA  R  result byte (ACPTR)
;   MBX+3 STAT  R  $80 BUSY, then $5A (answered) or $A5 (not mine)
;   MBX+4 ST    R  IEC status bits to OR into $90 (EOI=$40, no device=$80)
;   MBX+5 LEN   RW name / block length for the fast LOAD / SAVE commands
;   MBX+6 DEV0  R  device number of the SD drive now, MBX+7 DEV1 the USB drive (0 = none): PEEK(56854), PEEK(56855)
;   MBX+8          unused (the BORDER register until 0.9.0.63; the Alt button's border flash is done by DMA now)
;   MBX+9 ID0, MBX+10 ID1  R  $D6 $64 while the TDrive handler answers (the reset hook, see rst_hook at the end)
;
; Reclaims tape bit-timing code $F8E2-$FB8D and the cassette IRQ set-up $F875-$F8CF (device 1 tape I/O stops working).

.include "mailbox.inc"

; ---- fast LOAD / SAVE hooks: replace the `JSR $F3D5` (open serial file) --------
.segment "P_F4C8"
        jmp fl_hook             ; LOAD, after "SEARCHING FOR": X = original secondary address
.segment "P_F605"
        jmp fs_hook             ; SAVE, where the file would be opened

; ---- cold start ($FCE2: the RESET vector, also SYS64738): back to the TR menu while the TDrive handler runs ----
; Replaces `LDX #$FF / SEI`; rst_hook does them and goes on at the stock `TXS`.
.segment "P_FCE2"
        jmp rst_hook

; ---- default device for LOAD/SAVE/VERIFY without a number: 8, as on the SX-64 (stock: 1, the tape, whose code we reclaimed) ----
; BASIC's parameter routine at $E1D4 does `LDX #$01` at $E1D9 before SETLFS; this is its operand.
.segment "P_E1DA"
        .byte 8

; ---- SHIFT+RUN/STOP: LOAD"*",8,1 and RUN (stock: LOAD and RUN, i.e. from the tape) ----
; The screen editor's `LDX #$09 / SEI / STX $C6` at $E5EE fills the keyboard buffer from a table; shift_run (after rst_hook) does it instead.
.segment "P_E5EE"
        jmp shift_run

; ---- hooks over the stock entry points (3 bytes each) ----------------------
.segment "P_ED09"
        jmp hk_talk
.segment "P_ED0C"
        jmp hk_listen
.segment "P_EDB9"
        jmp hk_second
.segment "P_EDC7"
        jmp hk_tksa
.segment "P_EDDD"
        jmp hk_ciout
.segment "P_EDEF"
        jmp hk_untlk
.segment "P_EDFE"
        jmp hk_unlsn
.segment "P_EE13"
        jmp hk_acptr

; ---- stubs in reclaimed space ----------------------------------------------
.segment "P_F8E2"
.export iec_listen, iec_second, iec_unlsn   ; the real bus only: patches/vdbanner.s probes it before the mailbox is live

.include "vcall.inc"

hk_talk:
        sta $A7
        lda #C_TALK
        jsr vcall
        bcs @orig
        rts
@orig:  ora #$40
        bne tl_common

hk_listen:
        sta $A7
        lda #C_LISTEN
        jsr vcall
        bcs iec_listen
        rts
iec_listen:                     ; the stock LISTEN without the hook (also called by the power-on banner, patches/vdbanner.s)
        ora #$20
tl_common:
        jsr $F0A4
        jmp $ED11

hk_second:
        sta $A7
        lda #C_SECOND
        jsr vcall
        bcs iec_second
        rts
iec_second:                     ; stock SECOND without the hook
        sta $95
        jsr $ED36
        jmp $EDBE

hk_tksa:
        sta $A7
        lda #C_TKSA
        jsr vcall
        bcs @orig
        rts
@orig:  sta $95
        jsr $ED36
        jmp $EDCC

hk_ciout:
        sta $A7
        lda #C_CIOUT
        jsr vcall
        bcs @orig
        lda $A7                 ; CHROUT tail-jumps here: A must come back as the byte sent, as in the stock
        clc                     ; routine (BASIC's PRINT depends on it), not the mailbox DATA byte
        rts
@orig:  bit $94
        bmi @buf
        jmp $EDE1
@buf:   jmp $EDE6

hk_untlk:
        lda #C_UNTLK
        jsr vcall
        bcs @orig
        rts
@orig:  sei                     ; stock UNTLK falls through `BIT $3FA9` into UNLSN's code at $EDFE, which the UNLSN hook overwrote
        jsr $EE8E               ; (the real drive never got its UNTALK and kept talking), so the whole routine is spelled out here
        lda $DD00
        ora #$08
        sta $DD00
        lda #$5F
        jsr $ED11
        jmp $EE03

hk_unlsn:
        lda #C_UNLSN
        jsr vcall
        bcs iec_unlsn
        rts
iec_unlsn:                      ; stock UNLSN without the hook
        lda #$3F
        jsr $ED11
        jmp $EE03

hk_acptr:
        lda #C_ACPTR
        jsr vcall
        bcs @orig
        rts
@orig:  sei
        lda #$00
        jmp $EE16               ; the STA $A5 (not $EE17, its operand: that ran LDA $20 / STA $EE, in the screen line-link table)

; ---- fast LOAD ---------------------------------------------------------------
; Entered instead of `JSR $F3D5` in the serial LOAD path. Handles LOAD entirely through the mailbox for whatever devices the Teensy
; claims: the Teensy streams the file in blocks of <=128 bytes through the IO2 window, or writes parts of it into RAM by DMA
; (docs/DESIGN.md 49). VERIFY (with VD_DMA_LOAD) opens with FVERIFY_OPEN: only a Teensy that can compare the whole file with RAM by
; DMA takes it, and then every part is a DMA one (DATA = 0, never a window block to store); the Teensy ORs $10 into ST on a
; difference, as the stock loop does (docs/DESIGN.md 51). Anything else (also a name too long for the window), or a
; firmware without the fast commands, replays the displaced JSR and continues
; in the stock code ($F4CB).
; in: A = FLOAD_OPEN / FSAVE_OPEN. Copies the file name to the window and sends the command (out: as vcall).
; A name of WINSZ bytes or more does not fit: C=1 ("not mine"), and the byte-level path opens it instead.
open_named:
        pha
        jsr vd_here             ; no handler: write neither the window nor LEN (see vcall)
        bne @long
        ldy $B7                 ; never 0 here: LOAD / SAVE stop at MISSING FILE NAME first
        bmi @long               ; >= 128 = WINSZ
        ldy #0
@nm:    lda ($BB),y             ; file name -> window, each byte read back
        sta WIN,y
        cmp WIN,y
        bne @nm
        iny
        cpy $B7
        bne @nm
        sty MBX_LEN
        lda $BA
        sta $A7
        pla
        jmp vcall
@long:  pla
        sec
        rts

fl_fail:
        jmp $F707               ; DEVICE NOT PRESENT; also when an answer failed mid-transfer: an error instead of stale data
fl_slowx:
        ldx $A4                 ; restore X (original SA) for the stock path
fl_slow:
        jsr $F3D5
        jmp $F4CB
.if VD_DMA_LOAD
fl_hook:
        stx $A4                 ; original SA: 0 = load to caller's address (the Teensy reads it by DMA for a VERIFY)
        lda #C_FLOAD_OPEN
        ldy $93
        beq @open
        lda #C_FVERIFY_OPEN     ; VERIFY: answered not mine (the stock path) unless the Teensy compares it all by DMA
@open:  jsr open_named
.else
fl_hook:
        lda $93
        bne fl_slow               ; VERIFY
        stx $A4                 ; original SA: 0 = load to caller's address
        lda #C_FLOAD_OPEN
        jsr open_named
.endif
        bcs fl_slowx
        tay                     ; keep the result while X is reloaded
        ldx $A4
        tya
        beq @ok
        cmp #5
.if VD_DMA_LOAD                 ; (bytes saved for the DMA requests; VD_DMA_LOAD=0 stays byte-identical to the 0.9.0.51 image)
        beq fl_fail             ; DEVICE NOT PRESENT
        jmp $F704               ; FILE NOT FOUND
.else
        beq @nodev
        jmp $F704               ; FILE NOT FOUND
@nodev: jmp $F707               ; DEVICE NOT PRESENT
.endif
@ok:    lda WIN                 ; load address: double reads
        cmp WIN
        bne @ok
        sta $AE
@h1:    lda WIN+1
        cmp WIN+1
        bne @h1
        sta $AF
        txa                     ; SA = 0: use the caller's address instead
        bne @go
        lda $C3
        sta $AE
        lda $C4
        sta $AF
@go:    jsr $F5D2               ; "LOADING"
.if VD_DMA_LOAD
@blk:   lda $AE                 ; where the next byte goes: ARG/LEN. The Teensy answers with a window block as FLOAD_BLK, or
        sta $A7                 ; (DATA = 0) has written the next part straight into RAM by DMA and moved $AE/$AF on itself
        lda $AF                 ; (VIO_FLOAD_DMA, d64/tdrive_io.h; never below $0200 or in $D000-$DFFF)
        sta MBX_LEN
        lda #C_FLOAD_DMA
.else
@blk:   lda #C_FLOAD_BLK
.endif
        jsr vcall
        bcs fl_fail             ; time-out / not-mine in the middle of a load
        sta $A3                 ; byte count (0..WINSZ)
        beq @chk
        ldy #0
@cp:    lda WIN,y               ; double read: the byte is only taken when two reads in a row agree
        cmp WIN,y
        bne @cp
        sta ($AE),y
        iny
        cpy $A3
        bne @cp
        lda $AE
        clc
        adc $A3
        sta $AE
        bcc @chk
        inc $AF
@chk:   lda $90                 ; vcall folded the Teensy's EOI (bit 6) into ST
        and #$40
        beq @blk
        jmp $F5A9               ; CLC, X/Y = end address, RTS

; ---- fast SAVE ---------------------------------------------------------------
fs_slow:
        jsr $F3D5
        jmp $F608
fs_hook:
        lda #C_FSAVE_OPEN
        jsr open_named
        bcs fs_slow
        sta $A4                 ; 0 = open, else DOS error / 5 = no device
        jsr $F68F               ; "SAVING name"
        lda $A4
        cmp #5
.if VD_DMA_LOAD
        beq @ff                 ; DEVICE NOT PRESENT
        tax
.else
        bne @nodevok
        jmp $F707               ; DEVICE NOT PRESENT
@nodevok:
        lda $A4
.endif
        bne @done               ; drive refused (exists / full): stock KERNAL stays silent too
        lda $C1                 ; load address header
        sta WIN
        sta $AC
        lda $C2
        sta WIN+1
        sta $AD
        lda #2
        sta MBX_LEN
        lda #C_FSAVE_BLK
        jsr vcall
@loop:
.if VD_DMA_LOAD
        lda #C_FSAVE_DMA        ; DATA = 0: the Teensy read the next part from RAM by DMA ($AC-$AF), saved it and moved $AC/$AD on:
        jsr vcall               ; ask again. Anything else (DATA 1: not by DMA here; not mine from an older firmware, A = $A7 = the
        beq @loop               ; device) sends the next window block (docs/DESIGN.md 51)
.endif
        lda $AE                 ; remaining = end - current
        sec
        sbc $AC
        sta $A3
        lda $AF
        sbc $AD
        bcc @fin
        bne @full
        lda $A3
        beq @fin
        cmp #WINSZ+1
        bcc @send               ; the rest fits in one block
@full:  lda #WINSZ
        sta $A3
@send:  ldy #0
@c:     lda ($AC),y
        sta WIN,y
        cmp WIN,y               ; read back: a bad write is sent again
        bne @c
        iny
        cpy $A3
        bne @c
        lda $A3
        sta MBX_LEN
        lda #C_FSAVE_BLK
        jsr vcall
        bcs @ff
        lda $AC
        clc
        adc $A3
        sta $AC
        bcc @loop
        inc $AD
        bcs @loop
@fin:   lda #C_FSAVE_CLOSE
        jsr vcall
@done:  clc
        rts
@ff:    jmp $F707               ; mailbox stopped answering mid-save

tdrive_end:
.assert tdrive_end <= $FB8E, lderror, "reclaimed tape space overflows into the stock KERNAL"

; ---- reset hook: SYS64738 (or any jump to the RESET vector) returns to the TR menu ----
; The cold start also runs when the TR menu launches a program, while the menu's own IO1 handler still answers (the mailbox is not
; live yet), and on a machine with no TDrive at all. So nothing is written to the mailbox until both ID registers read right: the
; menu's $DE19/$DE1A are a SID's init address, never $D6xx; an unanswered read floats to $DE. Then VIO_MENU: the Teensy answers,
; writes back what is unsaved and resets the C64 into its menu, so after the answer the CPU only waits. No answer (~1 s, vcall's
; time-out): the stock cold start. Lives in the cassette IRQ set-up ($F875-$F8CF), reached only by a tape transfer with PLAY pressed,
; which already ran into the reclaimed tape code above; now it restarts the machine instead.
.segment "P_F875"
rst_hook:
        ldx #$FF
        sei
        txs                     ; vcall needs a stack
        lda MBX_ID              ; both bytes, the first read twice: a bit-mixed late answer must not pass
        cmp #VD_ID0
        bne @stock
        cmp MBX_ID
        bne @stock
        lda MBX_ID+1
        cmp #VD_ID1
        bne @stock
        lda #C_MENU
        jsr vcall
        bcc *                   ; answered: the Teensy resets the C64 into its menu
@stock: jmp $FCE5               ; the stock cold start from its `TXS`

; SHIFT+RUN/STOP, from the screen editor's key loop at $E5EE: `LOAD"*",8,1` CR `RUN` CR is 16 keys and the keyboard buffer holds 10
; (the SX-64 puts 15 in and runs over $0281-$0285; one more would reach the text colour at $0286). So the first 6 are echoed here through
; the editor's own output ($E716, as $E5CA does for every typed key; X, Y kept) and the other 9 go in the buffer as stock does.
; Same as typing them: the line is read from the screen at the first CR, RUN waits in the buffer until the load ends.
SR_ECHO = 6                     ; keys echoed; the rest (SR_KEYS) go in the buffer
shift_run:
        ldx #0
@echo:  lda sr_keys,x
        jsr $E716
        inx
        cpx #SR_ECHO
        bne @echo
        ldx #SR_KEYS
        sei
        stx $C6                 ; NDX: keys in the buffer
@buf:   lda sr_keys+SR_ECHO-1,x
        sta $0276,x
        dex
        bne @buf
        jmp $E5CD               ; the editor's wait for a key, as stock
sr_keys: .byte "LOAD", $22, "*", $22, ",8,1", $0D, "RUN", $0D
SR_KEYS = * - sr_keys - SR_ECHO
.assert SR_KEYS <= 10, error, "SHIFT+RUN puts more keys in the buffer than it holds ($0277-$0280)"
rst_hook_end:
.assert rst_hook_end <= $F8D0, lderror, "the reset hook and SHIFT+RUN overflow into the tape STOP check ($F8D0, used by PRESS PLAY)"

; ---- is the TDrive handler there? (vcall asks before every command) ----
; Z=1: ID0 ($DE19) read $D6 in one of 4 tries. Up to 4, so that a late (bit-mixed) answer of a handler that is there does not
; hand a transaction to the real bus mid-way, where it would go wrong; with no handler 4 misses cost about 50 cycles. Under the
; TR menu's handler $DE19 is a SID's init address, never $D6xx (I/O); a floating bus that reads $D6 by chance only gets
; vcall's time-out, as before this check. ID1 is not asked (one byte is enough for that, and the tape space is nearly full).
; Uses $A5 (vcall clears it next); X, Y kept. Also asked by open_named before it fills the window and LEN.
.segment "P_F8E2"
vd_here:
        lda #$7C                ; counts up: $7C..$7F, then $80 (N=1, Z=0) ends it
        sta $A5
@try:   lda MBX_ID
        cmp #VD_ID0
        beq @out                ; Z=1
        inc $A5
        bpl @try
@out:   rts
vd_here_end:
.assert vd_here_end <= $FB8E, lderror, "reclaimed tape space overflows into the stock KERNAL"
