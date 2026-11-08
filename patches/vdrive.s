; Virtual-drive hooks for the 8 serial-layer KERNAL entry points (901227-03).
; Each hook asks the TR+ mailbox whether it owns the current IEC transaction
; (the device numbers are configured in the firmware, d64/vd_cfg.h). If not ("not mine"), the displaced original instructions run
; and flow continues in the stock routine, so real drives still work.
;
; Mailbox (IO1 on hardware; RAM in the VICE mock build, see Makefile). It sits at $DE10, clear of the
; SwiftLink/Turbo232 ACIA at $DE00-$DE07 so both can be active together (firmware handler "VDrive+SwiftLink"):
;   MBX+0 CMD   W  write triggers the Teensy; sets BUSY at once
;   MBX+1 ARG   RW argument (device / secondary / byte)
;   MBX+2 DATA  R  result byte (ACPTR)
;   MBX+3 STAT  R  $80 BUSY, then $5A (answered) or $A5 (not mine)
;   MBX+4 ST    R  IEC status bits to OR into $90 (EOI=$40, no device=$80)
;   MBX+5 LEN   RW name / block length for the fast LOAD / SAVE commands
;   MBX+6 DEV0  R  device number of the SD drive now, MBX+7 DEV1 the USB drive (0 = none): PEEK(56854), PEEK(56855)
;   MBX+8 BORDER R request for the IRQ routine's border flash (disk-switch feedback, see irq_border at the end)
;   MBX+9 ID0, MBX+10 ID1  R  $D6 $64 while the VDrive handler answers (the reset hook, see rst_hook at the end)
;
; Reclaims tape bit-timing code $F8E2-$FB8D and the cassette IRQ set-up $F875-$F8CF (device 1 tape I/O stops working).

.ifndef MBX
MBX = $DE10
.endif
.ifndef WIN
WIN = $DF80                 ; 128-byte IO2 window: file names and data blocks ($DF00-$DF7F: REU registers in "VDrive+Swift+REU")
.endif
WINSZ    = 128              ; VIO_WIN in d64/vdrive_io.h
MBX_CMD  = MBX+0
MBX_ARG  = MBX+1
MBX_DATA = MBX+2
MBX_STAT = MBX+3
MBX_ST   = MBX+4
MBX_LEN  = MBX+5
MBX_BORDER = MBX+8
MBX_ID   = MBX+9
VD_ID0   = $D6              ; VIO_ID0_VAL / VIO_ID1_VAL in d64/vdrive_io.h
VD_ID1   = $64
STAT_OK      = $5A      ; answer tokens: exact values, so a misread can't pass for "done"
STAT_NOTMINE = $A5
STAT_SCAN    = $3C      ; "scan the serial bus first, then send this command again" (only ever answered to a LISTEN, TALK, fast LOAD / SAVE open)

ST       = $90

C_LISTEN = 1
C_TALK   = 2
C_SECOND = 3
C_TKSA   = 4
C_CIOUT  = 5
C_ACPTR  = 6
C_UNLSN  = 7
C_UNTLK  = 8
C_FLOAD_OPEN  = 9
C_FLOAD_BLK   = 10
C_FSAVE_OPEN  = 11
C_FSAVE_BLK   = 12
C_FSAVE_CLOSE = 13
C_SCAN   = 14
C_FREE   = 15
C_MENU   = 16
C_FLOAD_DMA = 17

; ---- fast LOAD / SAVE hooks: replace the `JSR $F3D5` (open serial file) --------
.segment "P_F4C8"
        jmp fl_hook             ; LOAD, after "SEARCHING FOR": X = original secondary address
.segment "P_F605"
        jmp fs_hook             ; SAVE, where the file would be opened

; ---- border flash: the IRQ routine ($EA31) calls UDTIM through `JSR $FFEA`; point that call at irq_border instead ----
.segment "P_EA32"
        .word irq_border

; ---- cold start ($FCE2: the RESET vector, also SYS64738): back to the TR menu while the VDrive handler runs ----
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

; in: command in A, argument in $A7 (written to MBX_ARG here, after the check).
; Preserves X, Y. Nothing is written to the mailbox unless the ID register reads right (vd_here): without a VDrive handler
; (no TR, another Special IO, the TR menu's own handler; a KERNAL in a ROM socket sees all of them) every call goes to the
; real bus at once. That matters beyond speed: under the TR menu's handler $DE11 (ARG) and $DE15 (LEN) are EEPROM-backed
; settings (MIDI, timezone) that a write would change. Then it waits up to ~1 s for the Teensy; a mailbox that never answers
; is treated as "not mine" too.
; out: C=0 handled, A=DATA (Z set from it), $90 updated; C=1 not mine, A=original argument.
vcall:
        pha                     ; the command: sent again after a bus scan
        jsr vd_here
        bne @no
@send:  lda $A7
        sta MBX_ARG
        pla
        pha
        sta MBX_CMD
        lda #0
        sta $A5
        sta $A6
@wait:  lda MBX_STAT            ; BUSY ($80) until the Teensy publishes an answer token
        cmp #STAT_OK
        beq @ok
        cmp #STAT_NOTMINE
        beq @no
        cmp #STAT_SCAN
        beq @scan
        inc $A5                 ; anything else (busy, or a floating-bus misread) keeps waiting
        bne @wait
        inc $A6
        bne @wait
        beq @no                 ; timed out
@ok:    lda MBX_ST              ; every read from the TR is done twice in a row and must agree: now and then the TR answers a
        cmp MBX_ST              ; few ns late and the 6510 gets a bit-mixed byte (seen on hardware, tests/asm/bustest.s). Two late
        bne @ok                 ; answers in a row, both wrong the same way, are far less likely (the second read returns the first's byte)
        ora ST
        sta ST
        pla
@dr:    lda MBX_DATA
        cmp MBX_DATA
        bne @dr
        cmp #0                  ; Z from DATA, as callers expect
        clc
        rts
@no:    pla
        lda $A7
        sec
        rts
@scan:  txa                     ; the first transaction of the run: probe the real bus before anything else
        pha
        tya
        pha
        lda $A7
        pha
        lda ST
        pha
        jsr bus_scan
        pla
        sta ST
        pla
        sta $A7                 ; the argument again (@send writes it); the Teensy kept LEN and the file name window as they were
        pla
        tay
        pla
        tax
        jmp @send               ; the command is still on the stack

; ---- bus scan ------------------------------------------------------------------------------------------------
; Which device numbers are free on the real serial bus? Probe 8, 9, ... with LISTEN + SECOND, the way OPEN starts. LISTEN
; alone is not enough: every drive on the bus pulls DATA low when ATN falls, whatever address follows, so only on an
; empty bus does LISTEN itself fail. A drive that is not addressed lets go of DATA after the first byte, so with
; SECOND the KERNAL sees nobody there and sets ST bit 7 (DEVICE NOT PRESENT) at once, with no time-out to wait for.
; Every number where nobody answered is reported to the Teensy, which gives it to the SD drive, then the USB drive;
; the scan ends when both have one (or at 30). Runs inside a user program, so no zero page is free: the number being
; probed lives on the stack. The Teensy has released its automatic numbers meanwhile, so the probes reach the real bus.
bus_scan:
        lda #0
        sta ST
        sta $A7
        lda #C_SCAN
        jsr vcall               ; A = how many drives need a number
        bcs @out                ; (cannot happen: SCAN is always ours)
        beq @out                ; none
        lda #8
@probe: pha
        jsr $FFB1               ; LISTEN
        lda ST
        bmi @empty              ; nothing on the bus at all
        lda #$6F
        jsr $FF93               ; SECOND (command channel, nothing is sent on it)
        lda ST
        bmi @empty
        jsr $FFAE               ; UNLSN: a real device is there
@next:  pla
        clc
        adc #1
        cmp #31
        bcc @probe
@out:   rts
@empty: lda #0
        sta ST                  ; forget the DEVICE NOT PRESENT of the probe
        tsx
        lda $0101,x             ; the number just probed
        sta $A7
        lda #C_FREE
        jsr vcall
        bcs @done
        bne @next               ; somebody still needs a number
@done:  pla
        rts

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
; Entered instead of `JSR $F3D5` in the serial LOAD path. Handles LOAD (not
; VERIFY) entirely through the mailbox for whatever devices the Teensy claims: the Teensy streams the
; file in blocks of <=128 bytes through the IO2 window, or writes parts of it into RAM by DMA (docs/DESIGN.md 49). Anything else (also a name too long for the window), or a
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
        jmp $F707               ; an answer failed mid-transfer: report an error instead of using stale data
fl_slowx:
        ldx $A4                 ; restore X (original SA) for the stock path
fl_slow:
        jsr $F3D5
        jmp $F4CB
fl_hook:
        lda $93
        bne fl_slow               ; VERIFY
        stx $A4                 ; original SA: 0 = load to caller's address
        lda #C_FLOAD_OPEN
        jsr open_named
        bcs fl_slowx
        tay                     ; keep the result while X is reloaded
        ldx $A4
        tya
        beq @ok
        cmp #5
        beq @nodev
        jmp $F704               ; FILE NOT FOUND
@nodev: jmp $F707               ; DEVICE NOT PRESENT
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
@blk:   lda $AE                 ; where the next byte goes: ARG/LEN. The Teensy answers with a window block as FLOAD_BLK, or
        sta $A7                 ; (DATA = 0) has written the next part straight into RAM by DMA and moved $AE/$AF on itself
        lda $AF                 ; (VIO_FLOAD_DMA, d64/vdrive_io.h; never below $0200 or in $D000-$DFFF)
        sta MBX_LEN
        lda #C_FLOAD_DMA
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
        bne @nodevok
        jmp $F707               ; DEVICE NOT PRESENT
@nodevok:
        lda $A4
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
@loop:  lda $AE                 ; remaining = end - current
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

; ---- border flash (alt-button disk switching): called from the 60 Hz IRQ routine instead of UDTIM, ends in UDTIM ($F69B) ----
; The Teensy publishes a request in BORDER: $C0|c = border colour c, $B0 = remember the border colour in $9C (a tape-only zero
; page byte), $D0 = put it back (exact values). Anything else (idle = $80, or whatever the TR menu's registers read before the mailbox is live)
; does nothing. Read twice, as everywhere: a bit-mixed byte must not flash anything.
; A, X, Y are already on the stack at this point of the IRQ routine.
irq_border:
        lda MBX_BORDER
        cmp MBX_BORDER
        bne @done
        eor #$C0                ; $C0|c -> c, $D0 -> $10, $B0 -> $70
        cmp #$10
        bcc @set
        beq @rest
        cmp #$70
        bne @done               ; exact values only: $DE, what a late answer returns, must not look like a request
        lda $D020               ; save: reads back $Fx, never zero, so the branch is always taken
        sta $9C
        bne @done
@rest:  lda $9C                 ; restore: the colour saved earlier
@set:   sta $D020
@done:  jmp $F69B
irq_border_end:
.assert irq_border_end <= $FB8E, lderror, "reclaimed tape space overflows into the stock KERNAL"

; ---- reset hook: SYS64738 (or any jump to the RESET vector) returns to the TR menu ----
; The cold start also runs when the TR menu launches a program, while the menu's own IO1 handler still answers (the mailbox is not
; live yet), and on a machine with no VDrive at all. So nothing is written to the mailbox until both ID registers read right: the
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

; ---- is the VDrive handler there? (vcall asks before every command) ----
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
