; Keyboard and IRQ hooks for the plain TeensyROM (docs/DESIGN.md 53): the agent's partner block, served by the Teensy from IO2 at
; MBX+$10 ($DF10, after the mailbox registers; $C810 in the VICE build, in RAM). The vector engine (d64/vdrive_vec.c) points two
; more vectors here when it hooks the others, and keeps the old values in old_irq / old_keylog:
;
;   KEYLOG $028F -> f_key: CTRL+C= with a key newly pressed (the key at this scan, SFDX $CB, is not the one of the last scan, LSTX $C5)
;                  writes its matrix code to the KEY register. The Teensy takes F1 (4) as "next image on the SD card" and F3 (5) as
;                  "next image on the USB stick", the TR+'s Alt-button taps, and ignores the rest. The keyboard scan goes on as
;                  stock: the CTRL table holds $FF (no key) for F1 / F3, so nothing reaches the keyboard buffer, and LSTX becomes
;                  the key, so holding it sends nothing more. KEYLOG runs only while a key is down.
;   CINV $0314   -> f_irq: the border flash after a switch (the KERNAL build's irq_border, patches/vdrive.s), then the old IRQ routine.
;                  The colour remembered by a SAVE request is kept in `saved`, in this block (the Teensy keeps the 6502's writes).
;
; f_sei is called by the engine (a CALL through the agent's ctramp) before it writes the two vectors: both are used from the IRQ, so a
; half-written one must never be seen. The return to the caller restores its own I flag.
;
; The offsets the Teensy needs are asserted at the end and repeated in d64/vdrive_vec.h (VV_H_*).

.include "mailbox.inc"

LSTX    = $C5                   ; the key at the last scan ($40 = none)
SFDX    = $CB                   ; the key at this scan
SHFLAG  = $028D                 ; 1 SHIFT, 2 C=, 4 CTRL
HOTMODS = 6                     ; exactly CTRL+C=

.segment "LOADADDR"
        .addr   base

.segment "CODE"
base:
old_irq:    .word $EA31         ; +0: CINV as it was (INSTALL writes the real one)
old_keylog: .word $EB48         ; +2: KEYLOG as it was
f_sei:  sei                     ; +4
        rts
saved:  .byte   $0E             ; +6: the border colour a SAVE request remembered

f_irq:  lda     MBX_BORDER      ; +7: read twice: a late answer ($DE, bit-mixed) must not look like a request
        cmp     MBX_BORDER
        bne     @done
        eor     #$C0            ; $C0|c -> c, $D0 -> $10, $B0 -> $70, $80 (idle) -> $40
        cmp     #$10
        bcc     @set
        beq     @rest
        cmp     #$70
        bne     @done
        lda     $D020           ; save: reads back $Fx, never zero, so the branch is always taken
        sta     saved
        bne     @done
@rest:  lda     saved
@set:   sta     $D020
@done:  jmp     (old_irq)

f_key:  lda     SHFLAG
        cmp     #HOTMODS
        bne     @chain
        lda     SFDX
        cmp     LSTX
        beq     @chain          ; held since the last scan: sent already
        sta     MBX_KEY
@chain: jmp     (old_keylog)
end_:

; d64/vdrive_vec.h repeats these
.assert old_irq - base = 0, error, "VV_H_OLD_IRQ"
.assert old_keylog - base = 2, error, "VV_H_OLD_KEYLOG"
.assert f_sei - base = 4, error, "VV_H_SEI"
.assert f_irq - base = 7, error, "VV_H_IRQ"
.assert f_key - base = 44, error, "VV_H_KEY"
.assert end_ - base <= $70, error, "the hooks must fit MBX+$10 .. the window ($DF10-$DF7F)"
