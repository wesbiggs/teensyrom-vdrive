; Virtual-drive hooks for the 8 serial-layer KERNAL entry points (901227-03).
; Each hook asks the TR+ mailbox whether it owns the current IEC transaction
; (the device numbers are configured in the firmware, d64/vd_cfg.h). If not ("not mine"), the displaced original instructions run
; and flow continues in the stock routine, so real drives still work.
;
; Mailbox (IO1 on hardware; RAM in the VICE mock build, see Makefile):
;   MBX+0 CMD   W  write triggers the Teensy; sets BUSY at once
;   MBX+1 ARG   RW argument (device / secondary / byte)
;   MBX+2 DATA  R  result byte (ACPTR)
;   MBX+3 STAT  R  $80 BUSY, then $5A (answered) or $A5 (not mine)
;   MBX+4 ST    R  IEC status bits to OR into $90 (EOI=$40, no device=$80)
;
; Reclaims tape bit-timing code $F8E2-$FB8D (device 1 tape I/O stops working).

.ifndef MBX
MBX = $DE00
.endif
.ifndef WIN
WIN = $DF00                 ; 256-byte IO2 window: file names and data blocks
.endif
MBX_CMD  = MBX+0
MBX_ARG  = MBX+1
MBX_DATA = MBX+2
MBX_STAT = MBX+3
MBX_ST   = MBX+4
MBX_LEN  = MBX+5
STAT_OK      = $5A      ; answer tokens: exact values, so a misread can't pass for "done"
STAT_NOTMINE = $A5

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

; ---- fast LOAD / SAVE hooks: replace the `JSR $F3D5` (open serial file) --------
.segment "P_F4C8"
        jmp fl_hook             ; LOAD, after "SEARCHING FOR": X = original secondary address
.segment "P_F605"
        jmp fs_hook             ; SAVE, where the file would be opened

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

; in: command in A, argument already in MBX_ARG and saved in $A7 (hooks do both).
; Preserves X, Y. Waits up to ~1 s for the Teensy; a mailbox that never answers
; (VDrive handler not active) is treated as "not mine" so the real IEC code runs.
; out: C=0 handled, A=DATA, $90 updated; C=1 not mine, A=original argument.
vcall:
        sta MBX_CMD
        lda #0
        sta $A5
        sta $A6
@wait:  lda MBX_STAT            ; BUSY ($80) until the Teensy publishes an answer token
        cmp #STAT_OK
        beq @ok
        cmp #STAT_NOTMINE
        beq @no
        inc $A5                 ; anything else (busy, or a floating-bus misread) keeps waiting
        bne @wait
        inc $A6
        bne @wait
        beq @no                 ; timed out
@ok:    lda MBX_ST
        ora ST
        sta ST
        lda MBX_DATA
        clc
        rts
@no:    lda $A7
        sec
        rts

hk_talk:
        sta MBX_ARG
        sta $A7
        lda #C_TALK
        jsr vcall
        bcs @orig
        rts
@orig:  ora #$40
        bne tl_common

hk_listen:
        sta MBX_ARG
        sta $A7
        lda #C_LISTEN
        jsr vcall
        bcs @orig
        rts
@orig:  ora #$20
tl_common:
        jsr $F0A4
        jmp $ED11

hk_second:
        sta MBX_ARG
        sta $A7
        lda #C_SECOND
        jsr vcall
        bcs @orig
        rts
@orig:  sta $95
        jsr $ED36
        jmp $EDBE

hk_tksa:
        sta MBX_ARG
        sta $A7
        lda #C_TKSA
        jsr vcall
        bcs @orig
        rts
@orig:  sta $95
        jsr $ED36
        jmp $EDCC

hk_ciout:
        sta MBX_ARG
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
@orig:  sei
        jsr $EE8E
        jmp $EDF3

hk_unlsn:
        lda #C_UNLSN
        jsr vcall
        bcs @orig
        rts
@orig:  lda #$3F
        jsr $ED11
        jmp $EE03

hk_acptr:
        lda #C_ACPTR
        jsr vcall
        bcs @orig
        rts
@orig:  sei
        lda #$00
        jmp $EE17

; ---- fast LOAD ---------------------------------------------------------------
; Entered instead of `JSR $F3D5` in the serial LOAD path. Handles LOAD (not
; VERIFY) entirely through the mailbox for whatever devices the Teensy claims: the Teensy streams the
; file in blocks of <=255 bytes through the IO2 window. Anything else, or a
; firmware without the fast commands, replays the displaced JSR and continues
; in the stock code ($F4CB).
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
        ldy #0
@nm:    lda ($BB),y             ; file name -> window
        sta WIN,y
        iny
        cpy $B7
        bne @nm
        sty MBX_LEN
        lda $BA
        sta MBX_ARG
        lda #C_FLOAD_OPEN
        jsr vcall
        bcs fl_slowx
        tay                     ; keep the result while X is reloaded
        ldx $A4
        tya
        beq @ok
        cmp #5
        beq @nodev
        jmp $F704               ; FILE NOT FOUND
@nodev: jmp $F707               ; DEVICE NOT PRESENT
@ok:    lda WIN
        sta $AE
        lda WIN+1
        sta $AF
        txa                     ; SA = 0: use the caller's address instead
        bne @go
        lda $C3
        sta $AE
        lda $C4
        sta $AF
@go:    jsr $F5D2               ; "LOADING"
@blk:   lda #C_FLOAD_BLK
        jsr vcall
        bcs fl_fail             ; time-out / not-mine in the middle of a load
        sta $A3                 ; byte count (0..255)
        beq @chk
        ldy #0
@cp:    lda WIN,y
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
fs_fail:
        jmp $F707               ; mailbox stopped answering mid-save
fs_slow:
        jsr $F3D5
        jmp $F608
fs_hook:
        ldy #0
@nm:    lda ($BB),y
        sta WIN,y
        iny
        cpy $B7
        bne @nm
        sty MBX_LEN
        lda $BA
        sta MBX_ARG
        lda #C_FSAVE_OPEN
        jsr vcall
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
        bne @send
@full:  lda #255
        sta $A3
@send:  ldy #0
@c:     lda ($AC),y
        sta WIN,y
        iny
        cpy $A3
        bne @c
        lda $A3
        sta MBX_LEN
        lda #C_FSAVE_BLK
        jsr vcall
        bcs fs_fail
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
