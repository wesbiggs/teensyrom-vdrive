; vcall (one mailbox transaction) and bus_scan (the lazy probe of the real serial bus for automatic numbers) of the KERNAL patch
; (patches/vdrive.s). Needs mailbox.inc first. Its $FFB1 / $FF93 / $FFAE calls reach the patch's hooks, which answer not-mine while
; the scan runs. (A separate file since the first plain-TR build shared it, DESIGN.md 45; the agent does its scan in C.)

; in: command in A, argument already in MBX_ARG and saved in $A7 (hooks do both).
; Preserves X, Y. Waits up to ~1 s for the Teensy; a mailbox that never answers
; (VDrive handler not active) is treated as "not mine" so the real IEC code runs.
; out: C=0 handled, A=DATA (Z set from it), $90 updated; C=1 not mine, A=original argument.
vcall:
        pha                     ; the command: sent again after a bus scan
vc_send:
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
        lda MBX_ARG
        pha
        lda $A7
        pha
        lda ST
        pha
        jsr bus_scan
        pla
        sta ST
        pla
        sta $A7
        pla
        sta MBX_ARG             ; the Teensy kept LEN and the file name window as they were
        pla
        tay
        pla
        tax
        pla                     ; the command again
        pha
        jmp vc_send

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
        sta MBX_ARG
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
        sta MBX_ARG
        sta $A7
        lda #C_FREE
        jsr vcall
        bcs @done
        bne @next               ; somebody still needs a number
@done:  pla
        rts
